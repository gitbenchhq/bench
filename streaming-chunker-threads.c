/*
 * Parallel streaming chunker with multi-threaded compression
 *
 * This implementation extends streaming-chunker.c with parallel compression
 * support via a producer-consumer model. The core chunking logic remains
 * identical, but chunks are compressed in parallel by worker threads.
 *
 * Architecture:
 * - Producer thread: Reads file in 64KB buffers, performs content-defined
 *   chunking (Gear hash) and SHA256 content hashing
 * - Worker pool: Compresses chunks in parallel using zlib, computes per-chunk
 *   SHA256 hashes for manifest
 * - Consumer thread: Writes compressed chunks to pack file in sequential order
 * - Backpressure: Producer blocks when all workers are busy
 * - Ordered output: Consumer waits for next sequential chunk before writing
 *
 * This file is kept separate from streaming-chunker.c for maintainability.
 * Code is guarded with BENCH_THREADS for conditional compilation.
 */

#ifdef BENCH_THREADS

#define USE_THE_REPOSITORY_VARIABLE

#include "git-compat-util.h"
#include "streaming-chunker-threads.h"
#include "bulk-checkin.h"
#include "object-file.h"
#include "odb.h"
#include "strbuf.h"
#include "manifest.h"
#include "config.h"
#include "repository.h"
#include "hex.h"
#include "repo-settings.h"
#include "environment.h"
#include "pack.h"
#include "git-zlib.h"
#include <pthread.h>
#include <zlib.h>
#include <stdio.h>

/*
 * Worker thread function
 * Waits for work, compresses chunk, computes chunk OID, signals completion
 */
static void *worker_thread(void *arg)
{
	struct worker_state *w = (struct worker_state *)arg;

	while (1) {
		pthread_mutex_lock(&w->mutex);

		/* Wait for work (status changes from IDLE to WORKING)
		 * Also wait if status is DONE (waiting for consumer to mark us IDLE) */
		while ((w->status == WORKER_IDLE || w->status == WORKER_DONE) && !w->should_exit) {
			pthread_cond_wait(&w->cond, &w->mutex);
		}

		if (w->should_exit) {
			pthread_mutex_unlock(&w->mutex);
			break;
		}

		/* We have work (status == WORKING) */
		pthread_mutex_unlock(&w->mutex);

		/*
		 * Create pack header + compressed data for bulk_checkin.
		 * Format: [pack_header][compressed_data]
		 * This allows bulk_checkin to accept pre-compressed chunks.
		 * Use git_deflate() to match pack format expectations.
		 */
		unsigned char pack_header[MAX_PACK_OBJECT_HEADER];
		int hdrlen = encode_in_pack_object_header(pack_header, sizeof(pack_header),
		                                          OBJ_BLOB, w->input_size);

		/* Compress the chunk using git_deflate (match Git's approach) */
		git_zstream stream;
		unsigned char *out_ptr = w->output_buffer + hdrlen;
		size_t out_size = 0;
		int status;
		unsigned long bound;

		git_deflate_init(&stream, pack_compression_level);
		bound = git_deflate_bound(&stream, w->input_size);

		stream.next_in = w->input_buffer;
		stream.avail_in = w->input_size;
		stream.next_out = out_ptr;
		stream.avail_out = bound;

		while ((status = git_deflate(&stream, Z_FINISH)) == Z_OK)
			; /* nothing */

		git_deflate_end(&stream);

		/* Compute results before acquiring mutex */
		int error = 0;
		size_t output_size_result = 0;
		struct object_id chunk_oid_result;

		if (status != Z_STREAM_END) {
			error = 1;
			output_size_result = 0;
		} else {
			out_size = stream.total_out;
			/* Copy pack header to beginning of output buffer */
			memcpy(w->output_buffer, pack_header, hdrlen);
			output_size_result = hdrlen + out_size;

			/* Compute SHA256 of this chunk for manifest */
			struct strbuf chunk_header = STRBUF_INIT;
			strbuf_addf(&chunk_header, "blob %zu", w->input_size);
			strbuf_addch(&chunk_header, '\0');

			struct git_hash_ctx ctx;
			the_hash_algo->init_fn(&ctx);
			the_hash_algo->update_fn(&ctx, chunk_header.buf, chunk_header.len);
			the_hash_algo->update_fn(&ctx, w->input_buffer, w->input_size);
			the_hash_algo->final_oid_fn(&chunk_oid_result, &ctx);

			strbuf_release(&chunk_header);
		}

		/* Mark as done - CRITICAL: Write all results inside mutex to ensure visibility */
		pthread_mutex_lock(&w->mutex);
		w->error = error;
		w->output_size = output_size_result;
		if (!error) {
			oidcpy(&w->chunk_oid, &chunk_oid_result);
		}
		w->status = WORKER_DONE;
		pthread_mutex_unlock(&w->mutex);

		/* Signal consumer that work is ready */
		pthread_mutex_lock(&w->parent->consumer_mutex);
		pthread_cond_signal(&w->parent->consumer_cond);
		pthread_mutex_unlock(&w->parent->consumer_mutex);
	}

	return NULL;
}

/*
 * Initialize worker pool based on repository settings
 */
static int init_workers(struct streaming_chunker_threads *sc)
{
	int i;
	unsigned long max_chunk_size;
	unsigned long input_buffer_size;
	unsigned long output_buffer_size;

	/* Get number of workers from repository settings */
	sc->num_workers = repo_settings_get_bench_threads(the_repository);
	if (sc->num_workers < 1)
		sc->num_workers = 1;

	sc->workers = xcalloc(sc->num_workers, sizeof(struct worker_state));
	max_chunk_size = repo_settings_get_chunk_max_size(the_repository);

	/*
	 * Input buffer must accommodate max_chunk_size + one read buffer
	 * because producer reads in STREAMING_BUFFER_SIZE chunks and may exceed
	 * max_chunk_size before checking the limit.
	 */
	input_buffer_size = max_chunk_size + STREAMING_BUFFER_SIZE;

	/*
	 * Output buffer must be sized for the actual max input size,
	 * not just max_chunk_size. For incompressible data, compressed size
	 * can be larger than input (zlib overhead).
	 */
	output_buffer_size = compressBound(input_buffer_size);

	for (i = 0; i < sc->num_workers; i++) {
		struct worker_state *w = &sc->workers[i];

		w->worker_id = i;
		w->status = WORKER_IDLE;
		w->chunk_number = -1;
		w->error = 0;
		w->should_exit = 0;
		w->parent = sc;

		/* Allocate buffers */
		w->input_buffer = xmalloc(input_buffer_size);
		w->output_buffer = xmalloc(output_buffer_size);

		if (!w->input_buffer || !w->output_buffer) {
			strbuf_addf(&sc->error_message,
			            "Failed to allocate buffers for worker %d", i);
			sc->error_occurred = 1;
			return -1;
		}

		pthread_mutex_init(&w->mutex, NULL);
		pthread_cond_init(&w->cond, NULL);

		if (pthread_create(&w->thread, NULL, worker_thread, w) != 0) {
			strbuf_addf(&sc->error_message,
			            "Failed to create worker thread %d", i);
			sc->error_occurred = 1;
			return -1;
		}
	}

	/* Initialize consumer synchronization */
	pthread_mutex_init(&sc->consumer_mutex, NULL);
	pthread_cond_init(&sc->consumer_cond, NULL);

	return 0;
}

/*
 * Cleanup worker pool
 */
static void cleanup_workers(struct streaming_chunker_threads *sc)
{
	int i;

	if (!sc->workers)
		return;

	/* Signal all threads to exit */
	for (i = 0; i < sc->num_workers; i++) {
		pthread_mutex_lock(&sc->workers[i].mutex);
		sc->workers[i].should_exit = 1;
		pthread_cond_signal(&sc->workers[i].cond);
		pthread_mutex_unlock(&sc->workers[i].mutex);
	}

	/* Join all threads */
	for (i = 0; i < sc->num_workers; i++) {
		pthread_join(sc->workers[i].thread, NULL);
		free(sc->workers[i].input_buffer);
		free(sc->workers[i].output_buffer);
		pthread_mutex_destroy(&sc->workers[i].mutex);
		pthread_cond_destroy(&sc->workers[i].cond);
	}

	free(sc->workers);
	sc->workers = NULL;

	/* Destroy consumer synchronization */
	pthread_mutex_destroy(&sc->consumer_mutex);
	pthread_cond_destroy(&sc->consumer_cond);
}

/*
 * Find an idle worker (returns -1 if none available)
 */
static int find_idle_worker(struct streaming_chunker_threads *sc)
{
	int i;

	for (i = 0; i < sc->num_workers; i++) {
		pthread_mutex_lock(&sc->workers[i].mutex);
		if (sc->workers[i].status == WORKER_IDLE) {
			pthread_mutex_unlock(&sc->workers[i].mutex);
			return i;
		}
		pthread_mutex_unlock(&sc->workers[i].mutex);
	}
	return -1;
}

/*
 * Find worker handling specific chunk number
 */
static int find_worker_with_chunk(struct streaming_chunker_threads *sc, int chunk_number)
{
	int i;

	for (i = 0; i < sc->num_workers; i++) {
		pthread_mutex_lock(&sc->workers[i].mutex);
		if (sc->workers[i].chunk_number == chunk_number) {
			pthread_mutex_unlock(&sc->workers[i].mutex);
			return i;
		}
		pthread_mutex_unlock(&sc->workers[i].mutex);
	}
	return -1;
}

/*
 * Common initialization logic for both file and buffer modes
 */
static int streaming_chunker_threads_init_common(struct streaming_chunker_threads *sc, size_t data_size)
{
	/* Initialize chunk configuration from repository settings */
	sc->chunk_config.min_size = repo_settings_get_chunk_min_size(the_repository);
	sc->chunk_config.max_size = repo_settings_get_chunk_max_size(the_repository);

	/* Compute mask from target size (for Gear hash boundary detection) */
	unsigned long target_size = repo_settings_get_chunk_target_size(the_repository);
	int bits = 0;
	size_t test = target_size;
	while (test > 1) {
		test >>= 1;
		bits++;
	}
	sc->chunk_config.mask = bits > 0 ? (1ULL << bits) - 1 : CHUNK_MASK_DEFAULT;

	/* Initialize arrays for chunk OIDs */
	sc->chunk_capacity = 16;  /* Start small, grow as needed */
	sc->chunk_oids = xcalloc(sc->chunk_capacity, sizeof(struct object_id));
	sc->chunk_count = 0;

	/* Initialize chunk buffer - holds current chunk being built by producer */
	strbuf_init(&sc->chunk_buffer, 0);
	strbuf_init(&sc->error_message, 0);

	/*
	 * Initialize content hash for the entire data.
	 * This computes: hash("blob <data_size>\0" + data)
	 * This represents what Git would have computed if it hashed the data as one blob.
	 */
	struct strbuf header = STRBUF_INIT;
	strbuf_addf(&header, "blob %zu", data_size);
	strbuf_addch(&header, '\0');

	the_hash_algo->init_fn(&sc->content_hash_ctx);
	the_hash_algo->update_fn(&sc->content_hash_ctx, header.buf, header.len);
	sc->content_hash_initialized = 1;

	strbuf_release(&header);

	sc->total_size = 0;
	sc->current_chunk_size = 0;
	sc->fingerprint = 0;
	sc->error_occurred = 0;

	/* Initialize parallel compression state */
	sc->next_chunk_to_produce = 0;
	sc->next_chunk_to_consume = 0;
	sc->total_chunks = 0;
	sc->reading_done = 0;

	/* Initialize worker pool */
	if (init_workers(sc) < 0)
		return -1;

	return 0;
}

int streaming_chunker_threads_init(struct streaming_chunker_threads *sc, int fd, off_t file_size)
{
	memset(sc, 0, sizeof(*sc));

	/* File mode: set file descriptor */
	sc->fd = fd;
	sc->buffer = NULL;
	sc->buffer_len = 0;
	sc->buffer_pos = 0;

	return streaming_chunker_threads_init_common(sc, (size_t)file_size);
}

int streaming_chunker_threads_init_from_buffer(struct streaming_chunker_threads *sc,
                                                 const char *data, size_t len)
{
	memset(sc, 0, sizeof(*sc));

	/* Buffer mode: set buffer parameters, fd = -1 signals buffer mode */
	sc->fd = -1;
	sc->buffer = data;
	sc->buffer_len = len;
	sc->buffer_pos = 0;

	return streaming_chunker_threads_init_common(sc, len);
}

/*
 * Dispatch current chunk_buffer to an idle worker
 * Returns 0 on success, -1 on error
 */
static int dispatch_chunk_to_worker(struct streaming_chunker_threads *sc)
{
	int worker_idx;
	struct worker_state *w;

	if (sc->chunk_buffer.len == 0)
		return 0;  /* Empty chunk, nothing to do */

	/* Wait for an idle worker (backpressure) */
	while ((worker_idx = find_idle_worker(sc)) < 0) {
		usleep(1000);  /* Sleep 1ms and retry */
	}

	w = &sc->workers[worker_idx];

	/* Copy chunk data to worker's input buffer */
	pthread_mutex_lock(&w->mutex);

	memcpy(w->input_buffer, sc->chunk_buffer.buf, sc->chunk_buffer.len);
	w->input_size = sc->chunk_buffer.len;
	w->chunk_number = sc->next_chunk_to_produce;
	w->status = WORKER_WORKING;

	pthread_cond_signal(&w->cond);  /* Wake up worker */
	pthread_mutex_unlock(&w->mutex);

	sc->next_chunk_to_produce++;

	/* Signal consumer that a new chunk has been dispatched */
	pthread_mutex_lock(&sc->consumer_mutex);
	pthread_cond_signal(&sc->consumer_cond);
	pthread_mutex_unlock(&sc->consumer_mutex);

	/* Clear buffer for next chunk */
	strbuf_reset(&sc->chunk_buffer);
	sc->current_chunk_size = 0;
	sc->fingerprint = 0;  /* Reset fingerprint for new chunk */

	return 0;
}

/*
 * Consumer: Write next chunk in sequence to ODB
 * Returns 0 if chunk written, 1 if chunk not ready yet, -1 on error
 */
static int consume_next_chunk(struct streaming_chunker_threads *sc)
{
	int worker_idx;
	struct worker_state *w;

	/* Find worker with next sequential chunk */
	worker_idx = find_worker_with_chunk(sc, sc->next_chunk_to_consume);
	if (worker_idx < 0) {
		return 1;  /* Chunk not assigned to any worker yet */
	}

	w = &sc->workers[worker_idx];

	pthread_mutex_lock(&w->mutex);

	/* Check if worker is done */
	if (w->status != WORKER_DONE) {
		pthread_mutex_unlock(&w->mutex);
		return 1;  /* Worker still processing */
	}

	/* Worker is done - check for errors */
	if (w->error) {
		strbuf_addf(&sc->error_message,
		            "Worker %d failed to compress chunk %d",
		            worker_idx, w->chunk_number);
		sc->error_occurred = 1;
		pthread_mutex_unlock(&w->mutex);
		return -1;
	}

	/*
	 * Write pre-compressed chunk to ODB via bulk checkin.
	 * Worker has already created [pack_header][compressed_data] and computed OID.
	 */
	if (index_precompressed_buffer_bulk_checkin(&w->chunk_oid,
	                                             (const char *)w->output_buffer,
	                                             w->output_size,
	                                             INDEX_WRITE_OBJECT) < 0) {
		strbuf_addf(&sc->error_message,
		            "Failed to write chunk %d to ODB", w->chunk_number);
		sc->error_occurred = 1;
		pthread_mutex_unlock(&w->mutex);
		return -1;
	}

	/* Store chunk OID from worker (for manifest) */
	if (sc->chunk_count >= sc->chunk_capacity) {
		sc->chunk_capacity *= 2;
		REALLOC_ARRAY(sc->chunk_oids, sc->chunk_capacity);
	}

	oidcpy(&sc->chunk_oids[sc->chunk_count], &w->chunk_oid);
	sc->chunk_count++;

	/* Mark worker as idle */
	w->status = WORKER_IDLE;
	w->chunk_number = -1;
	pthread_cond_signal(&w->cond);  /* Wake up worker if waiting */

	pthread_mutex_unlock(&w->mutex);

	sc->next_chunk_to_consume++;

	return 0;
}

/*
 * Producer thread: Reads file and dispatches chunks to workers
 * Runs in separate thread to avoid blocking consumer
 */
static void *producer_thread_func(void *arg)
{
	struct streaming_chunker_threads *sc = arg;
	size_t old_chunk_size;

	/* Producer loop: Read file and create chunks */
	while (!sc->reading_done) {
		ssize_t bytes_read = 0;

		/* Read next page */
		if (sc->fd >= 0) {
			/* File mode */
			bytes_read = xread(sc->fd, sc->read_buffer, STREAMING_BUFFER_SIZE);
			if (bytes_read < 0) {
				strbuf_addf(&sc->error_message, "read error: %s", strerror(errno));
				sc->error_occurred = 1;
				sc->producer_error = 1;
				return NULL;
			}
		} else {
			/* Buffer mode */
			size_t remaining = sc->buffer_len - sc->buffer_pos;
			if (remaining > 0) {
				bytes_read = remaining < STREAMING_BUFFER_SIZE
				           ? remaining
				           : STREAMING_BUFFER_SIZE;
				memcpy(sc->read_buffer, sc->buffer + sc->buffer_pos, bytes_read);
				sc->buffer_pos += bytes_read;
			}
		}

		if (bytes_read == 0) {
			/* EOF - dispatch final chunk if any */
			if (sc->chunk_buffer.len > 0) {
				if (dispatch_chunk_to_worker(sc) < 0) {
					sc->producer_error = 1;
					return NULL;
				}
			}
			sc->reading_done = 1;
			sc->total_chunks = sc->next_chunk_to_produce;
			break;
		}

		/* Update content hash with this page */
		if (sc->content_hash_initialized) {
			the_hash_algo->update_fn(&sc->content_hash_ctx,
			                         sc->read_buffer, bytes_read);
		}

		/* Add entire page to current chunk buffer */
		old_chunk_size = sc->current_chunk_size;

		strbuf_add(&sc->chunk_buffer, sc->read_buffer, bytes_read);
		sc->current_chunk_size += bytes_read;
		sc->total_size += bytes_read;

		/* Skip boundary detection until we reach minimum chunk size */
		if (sc->current_chunk_size >= sc->chunk_config.min_size) {
			const unsigned char *scan_start;
			size_t scan_length;
			size_t bytes_before_scan;

			if (old_chunk_size < sc->chunk_config.min_size) {
				/* Just crossed minimum threshold with this page */
				size_t bytes_to_skip = sc->chunk_config.min_size - old_chunk_size;
				scan_start = sc->read_buffer + bytes_to_skip;
				scan_length = bytes_read - bytes_to_skip;
				bytes_before_scan = sc->chunk_config.min_size;
			} else {
				/* Already past minimum - scan entire page */
				scan_start = sc->read_buffer;
				scan_length = bytes_read;
				bytes_before_scan = old_chunk_size;
			}

			/* Check for content-defined boundaries using Gear hash */
			if (scan_length > 0) {
				chunk_boundary_t boundary = find_chunk_boundary(
					&sc->chunk_config,
					scan_start,
					scan_length,
					&sc->fingerprint);

				if (boundary > 0) {
					/* Boundary found - dispatch current chunk */
					size_t boundary_offset_in_page = (scan_start - sc->read_buffer) + boundary;
					size_t chunk_final_size = bytes_before_scan + boundary;
					size_t leftover_bytes = bytes_read - boundary_offset_in_page;

					/* Trim chunk_buffer to boundary point */
					strbuf_setlen(&sc->chunk_buffer, chunk_final_size);
					sc->current_chunk_size = chunk_final_size;

					/* Dispatch chunk to worker */
					if (dispatch_chunk_to_worker(sc) < 0) {
						sc->producer_error = 1;
						return NULL;
					}

					/* Start new chunk with leftover bytes */
					if (leftover_bytes > 0) {
						strbuf_add(&sc->chunk_buffer,
						          sc->read_buffer + boundary_offset_in_page,
						          leftover_bytes);
						sc->current_chunk_size = leftover_bytes;
					}

					continue;
				}
			}
		}

		/* No boundary found - check if we've hit max size */
		if (sc->current_chunk_size >= sc->chunk_config.max_size) {
			/* Force chunk cut at max_size (safety limit) */
			if (dispatch_chunk_to_worker(sc) < 0) {
				sc->producer_error = 1;
				return NULL;
			}
		}
	}

	return NULL;
}

int streaming_chunker_threads_process_file(struct streaming_chunker_threads *sc)
{
	/* Initialize producer state */
	sc->producer_error = 0;

	/* Start producer thread - runs independently, reading and chunking file */
	if (pthread_create(&sc->producer_thread, NULL, producer_thread_func, sc) != 0) {
		strbuf_addf(&sc->error_message, "failed to create producer thread: %s",
		            strerror(errno));
		sc->error_occurred = 1;
		return -1;
	}

	/* CONSUMER LOOP: Main thread only consumes chunks written by workers */
	while (1) {
		/* Try to consume next chunk in order */
		int consume_result = consume_next_chunk(sc);
		if (consume_result < 0)
			return -1;  /* Error */

		/* Exit condition: reading done AND all chunks consumed */
		if (sc->reading_done && sc->next_chunk_to_consume >= sc->total_chunks) {
			break;
		}

		/* Wait for work to be available (if chunk not ready) */
		if (consume_result == 1) {
			/* Chunk not ready yet - wait for signal from worker or timeout */
			struct timespec timeout;
			clock_gettime(CLOCK_REALTIME, &timeout);
			/* Use 10ms timeout - balances responsiveness with reduced polling */
			timeout.tv_nsec += 10000000;  /* 10ms */
			if (timeout.tv_nsec >= 1000000000) {
				timeout.tv_sec++;
				timeout.tv_nsec -= 1000000000;
			}

			pthread_mutex_lock(&sc->consumer_mutex);
			pthread_cond_timedwait(&sc->consumer_cond, &sc->consumer_mutex, &timeout);
			pthread_mutex_unlock(&sc->consumer_mutex);
		}
	}

	/* Wait for producer thread to finish */
	pthread_join(sc->producer_thread, NULL);

	/* Check if producer encountered an error */
	if (sc->producer_error) {
		return -1;
	}

	return 0;
}

int streaming_chunker_threads_finalize(struct streaming_chunker_threads *sc,
                                         struct object_id *manifest_oid,
                                         struct object_id *content_oid)
{
	/* Finalize content hash */
	the_hash_algo->final_oid_fn(content_oid, &sc->content_hash_ctx);

	/* Create manifest object */
	if (write_manifest_object(the_repository, manifest_oid, sc->total_size,
	                          content_oid, sc->chunk_count,
	                          sc->chunk_oids) < 0) {
		strbuf_addstr(&sc->error_message, "failed to create manifest object");
		sc->error_occurred = 1;
		return -1;
	}

	return 0;
}

void streaming_chunker_threads_cleanup(struct streaming_chunker_threads *sc)
{
	cleanup_workers(sc);

	strbuf_release(&sc->chunk_buffer);
	strbuf_release(&sc->error_message);
	free(sc->chunk_oids);

	memset(sc, 0, sizeof(*sc));
}

#endif /* BENCH_THREADS */
