/*
 * BENCH_THREADS: Parallel multi-threaded version of streaming-chunker
 *
 * This is a DUPLICATE of streaming-chunker.c with parallel compression added.
 * Most logic is identical, but the processing loop uses a producer-consumer
 * model with worker threads for parallel compression.
 *
 * IMPORTANT: Keep this separate during validation. May merge with
 * streaming-chunker.c in the future if successful.
 *
 * Marked with BENCH_THREADS for easy identification and potential removal.
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

/* Memory usage tracking - always available for timing reports */
static double get_memory_usage_mb(void)
{
	FILE *f = fopen("/proc/self/status", "r");
	if (!f) return -1;

	char line[256];
	long rss_kb = 0;

	while (fgets(line, sizeof(line), f)) {
		if (sscanf(line, "VmRSS: %ld kB", &rss_kb) == 1) {
			break;
		}
	}
	fclose(f);

	return rss_kb / 1024.0;
}

/* BENCH_THREADS_DEBUG: Additional debug logging */
#ifdef BENCH_THREADS_DEBUG
static void log_memory_usage(const char *phase)
{
	double mem = get_memory_usage_mb();
	if (mem > 0)
		fprintf(stderr, "[BENCH THREADS] %s: %.2f MB\n", phase, mem);
}
#else
#define log_memory_usage(phase) do { } while (0)
#endif

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
		/* DEBUG: fprintf(stderr, "[W%d] Woke up, got chunk %d to compress (size=%zu)\n",
			w->worker_id, w->chunk_number, w->input_size); */
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

		/* DEBUG: fprintf(stderr, "[W%d] Chunk %d: input_size=%zu, hdrlen=%d\n",
		        w->worker_id, w->chunk_number, w->input_size, hdrlen); */

		/* DEBUG: fprintf(stderr, "[W%d] About to call git_deflate_init...\n", w->worker_id); */
		git_deflate_init(&stream, pack_compression_level);
		/* DEBUG: fprintf(stderr, "[W%d] git_deflate_init done\n", w->worker_id); */

		bound = git_deflate_bound(&stream, w->input_size);
		/* DEBUG: fprintf(stderr, "[W%d] git_deflate_bound returned %lu\n", w->worker_id, bound); */

		stream.next_in = w->input_buffer;
		stream.avail_in = w->input_size;
		stream.next_out = out_ptr;
		stream.avail_out = bound;

		/* DEBUG: fprintf(stderr, "[W%d] About to enter deflate loop (avail_in=%u, avail_out=%u)...\n",
		        w->worker_id, stream.avail_in, stream.avail_out); */

		int loop_count = 0;
		while ((status = git_deflate(&stream, Z_FINISH)) == Z_OK) {
			loop_count++;
			/* DEBUG: if (loop_count % 100 == 0) {
				fprintf(stderr, "[W%d] Deflate loop iteration %d (status=%d, avail_in=%u, avail_out=%u)\n",
				        w->worker_id, loop_count, status, stream.avail_in, stream.avail_out);
			} */
		}
		/* DEBUG: fprintf(stderr, "[W%d] Deflate loop exited after %d iterations, status=%d\n",
		        w->worker_id, loop_count, status); */

		/* DEBUG: fprintf(stderr, "[W%d] About to call git_deflate_end...\n", w->worker_id); */
		git_deflate_end(&stream);
		/* DEBUG: fprintf(stderr, "[W%d] git_deflate_end done\n", w->worker_id); */

		/* Compute results before acquiring mutex */
		int error = 0;
		size_t output_size_result = 0;
		struct object_id chunk_oid_result;

		if (status != Z_STREAM_END) {
			/* fprintf(stderr, "[W%d] Compression FAILED for chunk %d\n", w->worker_id, w->chunk_number); */
			error = 1;
			output_size_result = 0;
		} else {
			out_size = stream.total_out;
			/* Copy pack header to beginning of output buffer */
			memcpy(w->output_buffer, pack_header, hdrlen);
			output_size_result = hdrlen + out_size;
			/* fprintf(stderr, "[W%d] Chunk %d compressed: %zu -> %zu (hdr=%d, data=%zu)\n",
			        w->worker_id, w->chunk_number, w->input_size, output_size_result, hdrlen, out_size); */

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
		/* fprintf(stderr, "[W%d] Finished chunk %d, marked DONE, oid=%s\n",
			w->worker_id, w->chunk_number, oid_to_hex(&w->chunk_oid)); */

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

	log_memory_usage("Before worker allocation");

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

	log_memory_usage("After worker allocation");

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

	log_memory_usage("After worker cleanup");
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

#ifdef BENCH_THREADS_DEBUG
	sc->peak_memory_mb = 0;
#endif

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

	/* DEBUG: fprintf(stderr, "[DISPATCH] chunk_buffer.len=%zu\n", sc->chunk_buffer.len); */
	if (sc->chunk_buffer.len == 0)
		return 0;  /* Empty chunk, nothing to do */

	/* Wait for an idle worker (backpressure) */
	/* fprintf(stderr, "[DISPATCH] Looking for idle worker (chunk %d, size=%zu)...\n",
		sc->next_chunk_to_produce, sc->chunk_buffer.len); */
	while ((worker_idx = find_idle_worker(sc)) < 0) {
		/* fprintf(stderr, "[PRODUCER WAITING] All workers busy, producer sleeping...\n"); */
		usleep(1000);  /* Sleep 1ms and retry */
	}

	w = &sc->workers[worker_idx];
	/* fprintf(stderr, "[DISPATCH] Found worker %d, dispatching chunk %d (size=%zu)\n",
		worker_idx, sc->next_chunk_to_produce, sc->chunk_buffer.len); */

	/* Copy chunk data to worker's input buffer */
	pthread_mutex_lock(&w->mutex);

	memcpy(w->input_buffer, sc->chunk_buffer.buf, sc->chunk_buffer.len);
	w->input_size = sc->chunk_buffer.len;
	w->chunk_number = sc->next_chunk_to_produce;
	w->status = WORKER_WORKING;

	pthread_cond_signal(&w->cond);  /* Wake up worker */
	pthread_mutex_unlock(&w->mutex);

	/* fprintf(stderr, "[DISPATCH] Chunk %d dispatched to worker %d, signaled\n",
		sc->next_chunk_to_produce, worker_idx); */
	sc->next_chunk_to_produce++;

	/* Signal consumer that a new chunk has been dispatched */
	pthread_mutex_lock(&sc->consumer_mutex);
	pthread_cond_signal(&sc->consumer_cond);
	pthread_mutex_unlock(&sc->consumer_mutex);

	/* Clear buffer for next chunk */
	strbuf_reset(&sc->chunk_buffer);
	sc->current_chunk_size = 0;
	sc->fingerprint = 0;  /* Reset fingerprint for new chunk */

#ifdef BENCH_THREADS_DEBUG
	double mem = get_memory_usage_mb();
	if (mem > sc->peak_memory_mb)
		sc->peak_memory_mb = mem;
#endif

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

	/* DEBUG: fprintf(stderr, "[CONSUME] Looking for chunk %d\n", sc->next_chunk_to_consume); */

	/* Find worker with next sequential chunk */
	worker_idx = find_worker_with_chunk(sc, sc->next_chunk_to_consume);
	if (worker_idx < 0) {
		/* DEBUG: fprintf(stderr, "[CONSUME] Chunk %d not assigned yet\n", sc->next_chunk_to_consume); */
		return 1;  /* Chunk not assigned to any worker yet */
	}

	w = &sc->workers[worker_idx];
	/* DEBUG: fprintf(stderr, "[CONSUME] Found chunk %d in worker %d\n", sc->next_chunk_to_consume, worker_idx); */

	pthread_mutex_lock(&w->mutex);

	/* Check if worker is done */
	if (w->status != WORKER_DONE) {
		/* DEBUG: fprintf(stderr, "[CONSUME] Worker %d status=%d (not DONE), skipping\n", worker_idx, w->status); */
		pthread_mutex_unlock(&w->mutex);
		return 1;  /* Worker still processing */
	}

	/* DEBUG: fprintf(stderr, "[CONSUME] Worker %d is DONE, consuming chunk %d\n", worker_idx, w->chunk_number); */

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
	/* DEBUG: fprintf(stderr, "[CONSUMER] Writing chunk %d: oid=%s, size=%zu\n",
	        w->chunk_number, oid_to_hex(&w->chunk_oid), w->output_size); */

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
	/* DEBUG: fprintf(stderr, "[CONSUMER] Stored chunk %d OID: %s\n",
	        w->chunk_number, oid_to_hex(&w->chunk_oid)); */
	sc->chunk_count++;

	/* Mark worker as idle */
	w->status = WORKER_IDLE;
	w->chunk_number = -1;
	pthread_cond_signal(&w->cond);  /* Wake up worker if waiting */

	pthread_mutex_unlock(&w->mutex);

	sc->next_chunk_to_consume++;
	/* DEBUG: fprintf(stderr, "[CONSUME] Chunk consumed successfully, next_chunk_to_consume now=%d\n", sc->next_chunk_to_consume); */

#ifdef BENCH_THREADS_DEBUG
	double mem = get_memory_usage_mb();
	if (mem > sc->peak_memory_mb)
		sc->peak_memory_mb = mem;
#endif

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
	int boundaries_found = 0;

	/* fprintf(stderr, "[PRODUCER] Thread started\n"); */

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
				/* fprintf(stderr, "[EOF] Dispatching final chunk (size: %zu bytes, no boundary)\n",
				        sc->chunk_buffer.len); */
				if (dispatch_chunk_to_worker(sc) < 0) {
					sc->producer_error = 1;
					return NULL;
				}
			}
			sc->reading_done = 1;
			sc->total_chunks = sc->next_chunk_to_produce;
			/* fprintf(stderr, "[EOF] Reading complete. Total chunks to produce: %d\n", sc->total_chunks); */
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
					boundaries_found++;
					/* fprintf(stderr, "[BOUNDARY] Found boundary #%d at position (chunk size: %zu bytes)\n",
					        boundaries_found, bytes_before_scan + boundary); */

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
			/* fprintf(stderr, "[MAX_SIZE] Forcing chunk cut at max size (%zu bytes)\n",
			        sc->current_chunk_size); */
			if (dispatch_chunk_to_worker(sc) < 0) {
				sc->producer_error = 1;
				return NULL;
			}
		}
	}

	/* fprintf(stderr, "[PRODUCER] Thread finished (boundaries found: %d)\n", boundaries_found); */
	return NULL;
}

int streaming_chunker_threads_process_file(struct streaming_chunker_threads *sc)
{
	log_memory_usage("Start processing");

	/* Timing instrumentation */
	struct timeval loop_start, loop_end;
	gettimeofday(&loop_start, NULL);
	long total_consume_us = 0;
	int consume_count = 0;

	/* Initialize producer state */
	sc->producer_error = 0;

	/* Start producer thread - runs independently, reading and chunking file */
	/* fprintf(stderr, "[MAIN] Starting producer thread...\n"); */
	if (pthread_create(&sc->producer_thread, NULL, producer_thread_func, sc) != 0) {
		strbuf_addf(&sc->error_message, "failed to create producer thread: %s",
		            strerror(errno));
		sc->error_occurred = 1;
		return -1;
	}
	/* fprintf(stderr, "[MAIN] Producer thread started, now running consumer loop...\n"); */

	/* CONSUMER LOOP: Main thread only consumes chunks written by workers */
	while (1) {
		struct timeval t1, t2;

		/* Try to consume next chunk in order */
		gettimeofday(&t1, NULL);
		int consume_result = consume_next_chunk(sc);
		gettimeofday(&t2, NULL);
		if (consume_result == 0) {  /* Successfully consumed a chunk */
			total_consume_us += (t2.tv_sec - t1.tv_sec) * 1000000 + (t2.tv_usec - t1.tv_usec);
			consume_count++;
		}
		if (consume_result < 0)
			return -1;  /* Error */

		/* Exit condition: reading done AND all chunks consumed */
		if (sc->reading_done && sc->next_chunk_to_consume >= sc->total_chunks) {
			/* fprintf(stderr, "[MAIN] All chunks consumed, exiting consumer loop\n"); */
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
			int wait_result = pthread_cond_timedwait(&sc->consumer_cond, &sc->consumer_mutex, &timeout);
			pthread_mutex_unlock(&sc->consumer_mutex);

			/* DEBUG: Track if we're timing out frequently */
			// if (wait_result == ETIMEDOUT) {
			// 	fprintf(stderr, "[TIMEOUT] chunk %d timed out after 10ms\n", sc->next_chunk_to_consume);
			// } else if (wait_result == 0) {
			// 	fprintf(stderr, "[SIGNAL] chunk %d woke from signal\n", sc->next_chunk_to_consume);
			// }
			(void)wait_result;  /* Suppress unused variable warning */
		}
	}

	/* Wait for producer thread to finish */
	/* fprintf(stderr, "[MAIN] Waiting for producer thread to join...\n"); */
	pthread_join(sc->producer_thread, NULL);
	/* fprintf(stderr, "[MAIN] Producer thread joined\n"); */

	/* Check if producer encountered an error */
	if (sc->producer_error) {
		/* fprintf(stderr, "[MAIN] Producer thread reported error\n"); */
		return -1;
	}

	gettimeofday(&loop_end, NULL);
	long total_us = (loop_end.tv_sec - loop_start.tv_sec) * 1000000 + (loop_end.tv_usec - loop_start.tv_usec);

	fprintf(stderr, "\n[TIMING] Producer/Consumer with separate threads:\n");
	fprintf(stderr, "[TIMING]   Total time: %.3f seconds\n", total_us / 1000000.0);
	fprintf(stderr, "[TIMING]   Chunks produced: %d, consumed: %zu\n", sc->total_chunks, sc->chunk_count);
	if (sc->total_chunks != sc->chunk_count) {
		fprintf(stderr, "[TIMING]   WARNING: Produced != Consumed! (%d != %zu)\n",
		        sc->total_chunks, sc->chunk_count);
	}
	fprintf(stderr, "[TIMING]   Consumer writing (%d chunks): %.3f seconds (%.1f%%)\n",
	        consume_count, total_consume_us / 1000000.0, (total_consume_us * 100.0) / total_us);
	fprintf(stderr, "[TIMING]   Other/waiting: %.3f seconds (%.1f%%)\n",
	        (total_us - total_consume_us) / 1000000.0,
	        ((total_us - total_consume_us) * 100.0) / total_us);

	/* Memory usage reporting */
	double current_mem = get_memory_usage_mb();
#ifdef BENCH_THREADS_DEBUG
	double peak_mem = sc->peak_memory_mb;
#else
	double peak_mem = current_mem;  /* Without debug, only show current */
#endif
	fprintf(stderr, "[TIMING]   Memory usage: Current=%.2f MB, Peak=%.2f MB\n",
	        current_mem, peak_mem);
	fprintf(stderr, "[TIMING]   Workers used: %d\n", sc->num_workers);

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
