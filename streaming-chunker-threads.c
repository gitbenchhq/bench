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
#include <pthread.h>
#include <zlib.h>

/* BENCH_THREADS_DEBUG: Memory usage tracking (temporary, mark for removal) */
#ifdef BENCH_THREADS_DEBUG
#include <stdio.h>

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

		/* Wait for work (status changes from IDLE to WORKING) */
		while (w->status == WORKER_IDLE && !w->should_exit) {
			pthread_cond_wait(&w->cond, &w->mutex);
		}

		if (w->should_exit) {
			pthread_mutex_unlock(&w->mutex);
			break;
		}

		/* We have work (status == WORKING) */
		pthread_mutex_unlock(&w->mutex);

		/* Compress the chunk using zlib */
		uLongf compressed_size = compressBound(w->input_size);
		int ret = compress2(w->output_buffer, &compressed_size,
		                   w->input_buffer, w->input_size,
		                   pack_compression_level);

		if (ret != Z_OK) {
			w->error = 1;
			w->output_size = 0;
		} else {
			w->output_size = compressed_size;

			/* Compute SHA256 of this chunk for manifest */
			struct strbuf chunk_header = STRBUF_INIT;
			strbuf_addf(&chunk_header, "blob %zu", w->input_size);
			strbuf_addch(&chunk_header, '\0');

			struct git_hash_ctx ctx;
			the_hash_algo->init_fn(&ctx);
			the_hash_algo->update_fn(&ctx, chunk_header.buf, chunk_header.len);
			the_hash_algo->update_fn(&ctx, w->input_buffer, w->input_size);
			the_hash_algo->final_oid_fn(&w->chunk_oid, &ctx);

			strbuf_release(&chunk_header);
		}

		/* Mark as done */
		pthread_mutex_lock(&w->mutex);
		w->status = WORKER_DONE;
		pthread_cond_signal(&w->cond);  /* Signal consumer */
		pthread_mutex_unlock(&w->mutex);
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

	/* Get number of workers from repository settings */
	sc->num_workers = repo_settings_get_bench_threads(the_repository);
	if (sc->num_workers < 1)
		sc->num_workers = 1;

	sc->workers = xcalloc(sc->num_workers, sizeof(struct worker_state));
	max_chunk_size = repo_settings_get_chunk_max_size(the_repository);

	log_memory_usage("Before worker allocation");

	for (i = 0; i < sc->num_workers; i++) {
		struct worker_state *w = &sc->workers[i];

		w->worker_id = i;
		w->status = WORKER_IDLE;
		w->chunk_number = -1;
		w->error = 0;
		w->should_exit = 0;

		/* Allocate buffers */
		w->input_buffer = xmalloc(max_chunk_size);
		w->output_buffer = xmalloc(compressBound(max_chunk_size));

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

	/* Find worker with next sequential chunk */
	worker_idx = find_worker_with_chunk(sc, sc->next_chunk_to_consume);
	if (worker_idx < 0)
		return 1;  /* Chunk not assigned to any worker yet */

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

	/* Write compressed chunk to ODB via bulk checkin */
	/* Note: We're writing compressed data, bulk_checkin will handle packing */
	struct object_id written_oid;
	if (index_buffer_bulk_checkin(&written_oid,
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

#ifdef BENCH_THREADS_DEBUG
	double mem = get_memory_usage_mb();
	if (mem > sc->peak_memory_mb)
		sc->peak_memory_mb = mem;
#endif

	return 0;
}

int streaming_chunker_threads_process_file(struct streaming_chunker_threads *sc)
{
	size_t old_chunk_size;

	log_memory_usage("Start processing");

	/* Producer-Consumer loop */
	while (1) {
		/* PRODUCER: Read and chunk */
		if (!sc->reading_done) {
			ssize_t bytes_read = 0;

			/* Read next page */
			if (sc->fd >= 0) {
				/* File mode */
				bytes_read = xread(sc->fd, sc->read_buffer, STREAMING_BUFFER_SIZE);
				if (bytes_read < 0) {
					strbuf_addf(&sc->error_message, "read error: %s",
					            strerror(errno));
					sc->error_occurred = 1;
					return -1;
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
					if (dispatch_chunk_to_worker(sc) < 0)
						return -1;
				}
				sc->reading_done = 1;
				sc->total_chunks = sc->next_chunk_to_produce;
			} else {
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

				/*
				 * Skip boundary detection until we reach minimum chunk size.
				 * This is FastCDC's "skip sub-minimum cut-point" optimization.
				 */
				if (sc->current_chunk_size >= sc->chunk_config.min_size) {
					/*
					 * Determine which portion of the page to scan for boundaries.
					 */
					const unsigned char *scan_start;
					size_t scan_length;
					size_t bytes_before_scan;

					if (old_chunk_size < sc->chunk_config.min_size) {
						/* Just crossed minimum threshold with this page.
						 * Only scan the portion beyond min_size.
						 */
						size_t bytes_to_skip = sc->chunk_config.min_size - old_chunk_size;
						scan_start = sc->read_buffer + bytes_to_skip;
						scan_length = bytes_read - bytes_to_skip;
						bytes_before_scan = sc->chunk_config.min_size;
					} else {
						/* Already past minimum before this page.
						 * Scan the entire page.
						 */
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
							if (dispatch_chunk_to_worker(sc) < 0)
								return -1;

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
					if (dispatch_chunk_to_worker(sc) < 0)
						return -1;
				}
			}
		}

		/* CONSUMER: Write chunks in order */
		int consume_result = consume_next_chunk(sc);
		if (consume_result < 0)
			return -1;  /* Error */

		/* Exit condition: reading done AND all chunks consumed */
		if (sc->reading_done && sc->next_chunk_to_consume >= sc->total_chunks) {
			break;
		}

		/* Small sleep to avoid busy-waiting */
		if (!sc->reading_done || sc->next_chunk_to_consume < sc->total_chunks) {
			usleep(100);  /* 0.1ms */
		}
	}

	log_memory_usage("End processing");

#ifdef BENCH_THREADS_DEBUG
	fprintf(stderr, "[BENCH THREADS] Peak memory: %.2f MB\n", sc->peak_memory_mb);
	fprintf(stderr, "[BENCH THREADS] Total chunks: %d\n", sc->total_chunks);
	fprintf(stderr, "[BENCH THREADS] Workers used: %d\n", sc->num_workers);
#endif

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
