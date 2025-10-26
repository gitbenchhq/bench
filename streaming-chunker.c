#define USE_THE_REPOSITORY_VARIABLE

#include "git-compat-util.h"
#include "streaming-chunker.h"
#include "parallel-compress.h"
#include "object-file.h"
#include "odb.h"
#include "strbuf.h"
#include "manifest.h"
#include "config.h"
#include "repository.h"
#include "hex.h"
#include "repo-settings.h"
#include "wrapper.h"
#include "write-or-die.h"
#include "path.h"
#include <sys/time.h>

/* Timing helper */
static inline double get_time_ms(void)
{
	struct timeval tv;
	gettimeofday(&tv, NULL);
	return tv.tv_sec * 1000.0 + tv.tv_usec / 1000.0;
}

/* Timing statistics */
static double time_read_ms = 0;
static double time_content_hash_ms = 0;  /* Full-file SHA256 hashing */
static double time_gear_hash_ms = 0;     /* Gear hash boundary detection */
static double time_compress_ms = 0;
static double time_write_ms = 0;

/*
 * Async content hasher worker thread.
 * Continuously processes hash jobs from the queue.
 */
static void *async_hasher_worker(void *arg)
{
	struct async_hasher *hasher = arg;

	pthread_mutex_lock(&hasher->lock);

	while (1) {
		/* Wait for work or shutdown signal */
		while (hasher->queue_count == 0 && !hasher->shutdown) {
			pthread_cond_wait(&hasher->has_work, &hasher->lock);
		}

		/* Check for shutdown */
		if (hasher->shutdown && hasher->queue_count == 0) {
			pthread_mutex_unlock(&hasher->lock);
			return NULL;
		}

		/* Dequeue job */
		struct hash_job job = hasher->queue[hasher->queue_head];
		hasher->queue_head = (hasher->queue_head + 1) % hasher->queue_size;
		hasher->queue_count--;

		/* Signal that space is available */
		pthread_cond_signal(&hasher->has_space);

		/* Unlock while processing (allow main thread to continue) */
		pthread_mutex_unlock(&hasher->lock);

		/* Process hash update */
		the_hash_algo->update_fn(hasher->hash_ctx, job.data, job.size);
		free(job.data);

		/* Reacquire lock for next iteration */
		pthread_mutex_lock(&hasher->lock);
	}
}

/*
 * Initialize async content hasher with a dedicated thread.
 * Queue size of 8 allows main thread to stay ahead while hash thread processes.
 */
static struct async_hasher *async_hasher_init(struct git_hash_ctx *hash_ctx)
{
	struct async_hasher *hasher = xmalloc(sizeof(*hasher));
	memset(hasher, 0, sizeof(*hasher));

	hasher->queue_size = 8;
	hasher->queue = xcalloc(hasher->queue_size, sizeof(struct hash_job));
	hasher->hash_ctx = hash_ctx;

	pthread_mutex_init(&hasher->lock, NULL);
	pthread_cond_init(&hasher->has_work, NULL);
	pthread_cond_init(&hasher->has_space, NULL);

	if (pthread_create(&hasher->thread, NULL, async_hasher_worker, hasher) != 0) {
		error("failed to create async hash thread");
		free(hasher->queue);
		free(hasher);
		return NULL;
	}

	return hasher;
}

/*
 * Submit data to async hasher for processing.
 * Blocks if queue is full (backpressure).
 */
static void async_hasher_submit(struct async_hasher *hasher,
                                 const unsigned char *data, size_t size)
{
	if (!hasher)
		return;

	/* Make a copy of the data for the hash thread */
	unsigned char *data_copy = xmalloc(size);
	memcpy(data_copy, data, size);

	pthread_mutex_lock(&hasher->lock);

	/* Wait for space in queue (backpressure) */
	while (hasher->queue_count >= hasher->queue_size) {
		pthread_cond_wait(&hasher->has_space, &hasher->lock);
	}

	/* Enqueue job */
	hasher->queue[hasher->queue_tail].data = data_copy;
	hasher->queue[hasher->queue_tail].size = size;
	hasher->queue_tail = (hasher->queue_tail + 1) % hasher->queue_size;
	hasher->queue_count++;

	/* Signal worker thread */
	pthread_cond_signal(&hasher->has_work);

	pthread_mutex_unlock(&hasher->lock);
}

/*
 * Wait for async hasher to finish all pending work and shut down.
 */
static void async_hasher_finish(struct async_hasher *hasher)
{
	if (!hasher)
		return;

	/* Signal shutdown and wake worker */
	pthread_mutex_lock(&hasher->lock);
	hasher->shutdown = 1;
	pthread_cond_signal(&hasher->has_work);
	pthread_mutex_unlock(&hasher->lock);

	/* Wait for thread to exit */
	pthread_join(hasher->thread, NULL);

	/* Cleanup */
	pthread_mutex_destroy(&hasher->lock);
	pthread_cond_destroy(&hasher->has_work);
	pthread_cond_destroy(&hasher->has_space);
	free(hasher->queue);
	free(hasher);
}

int should_chunk_file(off_t file_size)
{
	/*
	 * NOTE: This function assumes we're already in bench mode.
	 * The caller (index_path) should check repo_has_bench_extensions()
	 * before calling this function.
	 */

	/*
	 * Use chunk.minSize as the threshold for when to start chunking.
	 * Files smaller than or equal to this will be stored as single-chunk manifests.
	 * Files larger than this will be chunked using content-defined chunking.
	 * Default is 2MB (see repo-settings.c).
	 */
	unsigned long threshold = repo_settings_get_chunk_min_size(the_repository);

	return file_size > threshold;
}

/*
 * Common initialization logic for both file and buffer modes
 */
static int streaming_chunker_init_common(struct streaming_chunker *sc, size_t data_size)
{
	/*
	 * Initialize chunk configuration from repository settings.
	 * This reads from config values:
	 *   - chunk.minSize (default: 2MB)
	 *   - chunk.targetSize (default: 16MB)
	 *   - chunk.maxSize (default: 64MB)
	 */
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

	/* Initialize arrays */
	sc->chunk_capacity = 16;  /* Start small, grow as needed */
	sc->chunk_oids = xcalloc(sc->chunk_capacity, sizeof(struct object_id));
	sc->chunk_count = 0;

	/* Initialize chunk buffer - will hold one chunk at a time */
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

	/* Initialize parallel compressor (only if >1 thread configured) */
	unsigned int num_threads = repo_settings_get_compression_threads(the_repository);
	if (num_threads > 1) {
		sc->compressor = xcalloc(1, sizeof(struct parallel_compressor));
		if (parallel_compressor_init(sc->compressor, the_repository) < 0) {
			strbuf_addstr(&sc->error_message, "failed to initialize parallel compressor");
			sc->error_occurred = 1;
			free(sc->compressor);
			sc->compressor = NULL;
			return -1;
		}
	} else {
		sc->compressor = NULL;  /* Use synchronous compression */
	}

	/* Initialize async content hasher to overlap hashing with Gear hash work */
	sc->hasher = async_hasher_init(&sc->content_hash_ctx);
	if (!sc->hasher) {
		warning("async content hasher failed to initialize, falling back to synchronous");
	}

	return 0;
}

int streaming_chunker_init(struct streaming_chunker *sc, int fd, off_t file_size)
{
	memset(sc, 0, sizeof(*sc));

	/* File mode: set file descriptor */
	sc->fd = fd;
	sc->buffer = NULL;
	sc->buffer_len = 0;
	sc->buffer_pos = 0;

	return streaming_chunker_init_common(sc, (size_t)file_size);
}

int streaming_chunker_init_from_buffer(struct streaming_chunker *sc,
                                        const char *data, size_t len)
{
	memset(sc, 0, sizeof(*sc));

	/* Buffer mode: set buffer parameters, fd = -1 signals buffer mode */
	sc->fd = -1;
	sc->buffer = data;
	sc->buffer_len = len;
	sc->buffer_pos = 0;

	return streaming_chunker_init_common(sc, len);
}

/*
 * Write already-compressed data directly to a loose object file.
 * This is used for parallel compression where compression happens in worker threads.
 *
 * The compressed data must already include the object header ("blob <size>\0")
 * compressed together with the object content.
 */
static int write_compressed_loose_object(const struct object_id *oid,
                                         const void *compressed_data,
                                         unsigned long compressed_size)
{
	int fd, dirlen;
	struct strbuf tmp_file = STRBUF_INIT;
	struct strbuf filename = STRBUF_INIT;

	/* Get the path for this loose object */
	odb_loose_path(the_repository->objects->sources, &filename, oid);

	/* Create temporary file (inline create_tmpfile logic) */
	/* Find the last '/' to get directory length */
	{
		const char *s = strrchr(filename.buf, '/');
		dirlen = s ? (s - filename.buf + 1) : 0;
	}
	strbuf_add(&tmp_file, filename.buf, dirlen);
	strbuf_addstr(&tmp_file, "tmp_obj_XXXXXX");
	fd = git_mkstemp_mode(tmp_file.buf, 0444);
	if (fd < 0 && dirlen && errno == ENOENT) {
		/* Create directory and retry */
		strbuf_reset(&tmp_file);
		strbuf_add(&tmp_file, filename.buf, dirlen - 1);
		if (mkdir(tmp_file.buf, 0777) && errno != EEXIST) {
			strbuf_release(&tmp_file);
			strbuf_release(&filename);
			return -1;
		}
		if (adjust_shared_perm(the_repository, tmp_file.buf)) {
			strbuf_release(&tmp_file);
			strbuf_release(&filename);
			return -1;
		}
		strbuf_addstr(&tmp_file, "/tmp_obj_XXXXXX");
		fd = git_mkstemp_mode(tmp_file.buf, 0444);
	}

	if (fd < 0) {
		strbuf_release(&tmp_file);
		strbuf_release(&filename);
		return -1;
	}

	/* Write compressed data directly */
	if (write_in_full(fd, compressed_data, compressed_size) < 0) {
		close(fd);
		unlink(tmp_file.buf);
		strbuf_release(&tmp_file);
		strbuf_release(&filename);
		return -1;
	}

	/* Close and finalize */
	if (fsync_object_files > 0)
		fsync_or_die(fd, tmp_file.buf);
	if (close(fd) != 0) {
		unlink(tmp_file.buf);
		strbuf_release(&tmp_file);
		strbuf_release(&filename);
		return -1;
	}

	/* Move temp file to final location */
	if (finalize_object_file_flags(tmp_file.buf, filename.buf,
	                                FOF_SKIP_COLLISION_CHECK) < 0) {
		strbuf_release(&tmp_file);
		strbuf_release(&filename);
		return -1;
	}

	strbuf_release(&tmp_file);
	strbuf_release(&filename);
	return 0;
}

/*
 * Finalize the current chunk: submit for parallel compression
 */
static int finalize_current_chunk(struct streaming_chunker *sc)
{
	void *chunk_data;
	size_t chunk_size;

	if (sc->chunk_buffer.len == 0)
		return 0;  /* Empty chunk, nothing to do */

	/* Grow array if needed */
	if (sc->chunk_count >= sc->chunk_capacity) {
		sc->chunk_capacity *= 2;
		REALLOC_ARRAY(sc->chunk_oids, sc->chunk_capacity);
	}

	chunk_size = sc->chunk_buffer.len;

	/* Use parallel or synchronous compression based on configuration */
	if (sc->compressor) {
		/*
		 * Submit chunk for parallel compression.
		 * We transfer ownership of the data to the compressor.
		 */
		chunk_data = xmalloc(chunk_size);
		memcpy(chunk_data, sc->chunk_buffer.buf, chunk_size);

		if (parallel_compressor_submit(sc->compressor, chunk_data, chunk_size,
		                               OBJ_BLOB, sc->chunk_count) < 0) {
			free(chunk_data);
			strbuf_addf(&sc->error_message, "failed to submit chunk %zu for compression",
			            sc->chunk_count);
			sc->error_occurred = 1;
			return -1;
		}
	} else {
		/* Synchronous: write chunk immediately */
		struct object_id chunk_oid;
		if (write_object_file_flags(sc->chunk_buffer.buf, chunk_size,
		                            OBJ_BLOB, &chunk_oid, NULL,
		                            WRITE_OBJECT_FILE_PERSIST) < 0) {
			strbuf_addf(&sc->error_message, "failed to write chunk %zu to ODB",
			            sc->chunk_count);
			sc->error_occurred = 1;
			return -1;
		}
		oidcpy(&sc->chunk_oids[sc->chunk_count], &chunk_oid);
	}

	sc->chunk_count++;

	/* Clear buffer and reset for next chunk */
	strbuf_reset(&sc->chunk_buffer);
	sc->current_chunk_size = 0;
	sc->fingerprint = 0;  /* Reset fingerprint for new chunk */

	return 0;
}

int streaming_chunker_process_file(struct streaming_chunker *sc)
{
	/*
	 * Streaming chunker main loop.
	 *
	 * Algorithm:
	 *   1. Read file in 8KB pages
	 *   2. Add each page to current chunk buffer
	 *   3. Update content hash (for entire file)
	 *   4. Check for content-defined boundaries using Gear hash:
	 *      - Skip boundary detection until chunk >= min_size (FastCDC optimization)
	 *      - Once past min_size, scan for boundaries in each page
	 *      - Force cut at max_size if no boundary found
	 *   5. When boundary found: finalize chunk, start new one with leftover bytes
	 *   6. At EOF: finalize remaining chunk
	 *
	 * Two parallel hashes are computed:
	 *   - Content hash: hash("blob <total_size>\0" + entire_file_data)
	 *                   This is what Git would compute for the whole file.
	 *   - Chunk hashes: hash("blob <chunk_size>\0" + chunk_data) per chunk
	 *                   Computed in finalize_current_chunk().
	 */

	ssize_t bytes_read;
	size_t old_chunk_size;
	double t_start, t_end;

	while (1) {
		/* Read next page from file or buffer */
		t_start = get_time_ms();

		if (sc->fd >= 0) {
			/* File mode: read from file descriptor */
			bytes_read = xread(sc->fd, sc->read_buffer, STREAMING_BUFFER_SIZE);
			if (bytes_read < 0) {
				strbuf_addf(&sc->error_message, "failed to read file: %s",
				            strerror(errno));
				sc->error_occurred = 1;
				return -1;
			}
		} else {
			/* Buffer mode: read from memory buffer */
			size_t remaining = sc->buffer_len - sc->buffer_pos;
			if (remaining == 0) {
				bytes_read = 0;  /* EOF */
			} else {
				bytes_read = remaining < STREAMING_BUFFER_SIZE
				           ? remaining
				           : STREAMING_BUFFER_SIZE;
				memcpy(sc->read_buffer, sc->buffer + sc->buffer_pos, bytes_read);
				sc->buffer_pos += bytes_read;
			}
		}

		t_end = get_time_ms();
		time_read_ms += (t_end - t_start);

		if (bytes_read == 0)
			break;  /* EOF */

		/* Update content hash with this page (async or synchronous) */
		t_start = get_time_ms();
		if (sc->content_hash_initialized) {
			if (sc->hasher) {
				/* Async: submit to hash thread (non-blocking with backpressure) */
				async_hasher_submit(sc->hasher, sc->read_buffer, bytes_read);
			} else {
				/* Fallback: synchronous hashing */
				the_hash_algo->update_fn(&sc->content_hash_ctx,
				                         sc->read_buffer, bytes_read);
			}
		}
		t_end = get_time_ms();
		time_content_hash_ms += (t_end - t_start);

		/* Add entire page to current chunk buffer */
		old_chunk_size = sc->current_chunk_size;

		strbuf_add(&sc->chunk_buffer, sc->read_buffer, bytes_read);
		sc->current_chunk_size += bytes_read;
		sc->total_size += bytes_read;

		/*
		 * Skip boundary detection until we reach minimum chunk size.
		 * This is FastCDC's "skip sub-minimum cut-point" optimization.
		 */
		if (sc->current_chunk_size < sc->chunk_config.min_size)
			continue;

		/*
		 * We're now at or past minimum size. Determine which portion
		 * of the page to scan for boundaries.
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
			t_start = get_time_ms();
			chunk_boundary_t boundary = find_chunk_boundary(
				&sc->chunk_config,
				scan_start,
				scan_length,
				&sc->fingerprint);
			t_end = get_time_ms();
			time_gear_hash_ms += (t_end - t_start);

			if (boundary > 0) {
				/* Boundary found - finalize current chunk */
				size_t boundary_offset_in_page = (scan_start - sc->read_buffer) + boundary;
				size_t chunk_final_size = bytes_before_scan + boundary;
				size_t leftover_bytes = bytes_read - boundary_offset_in_page;

				/* Trim chunk_buffer to boundary point */
				strbuf_setlen(&sc->chunk_buffer, chunk_final_size);
				sc->current_chunk_size = chunk_final_size;

				/* Write chunk to ODB as blob object */
				if (finalize_current_chunk(sc) < 0)
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

		/* No boundary found - check if we've hit max size */
		if (sc->current_chunk_size >= sc->chunk_config.max_size) {
			/* Force chunk cut at max_size (safety limit) */
			if (finalize_current_chunk(sc) < 0)
				return -1;
		}
	}

	/* EOF reached - finalize remaining chunk */
	if (sc->chunk_buffer.len > 0) {
		if (finalize_current_chunk(sc) < 0)
			return -1;
	}

	/* Sanity check: at least one chunk must exist */
	if (sc->chunk_count == 0) {
		strbuf_addstr(&sc->error_message, "no chunks created");
		sc->error_occurred = 1;
		return -1;
	}

	return 0;
}

int streaming_chunker_finalize(struct streaming_chunker *sc,
                                struct object_id *manifest_oid,
                                struct object_id *content_oid)
{
	size_t i;
	int errors_encountered = 0;

	if (sc->error_occurred)
		return -1;

	/*
	 * If using parallel compression, wait for all jobs and collect results.
	 * Otherwise, chunks were already written synchronously.
	 */
	if (sc->compressor) {
		double t_start, t_end;

		/* Wait for all compression jobs to complete */
		t_start = get_time_ms();
		parallel_compressor_finish(sc->compressor);
		t_end = get_time_ms();
		time_compress_ms += (t_end - t_start);

		/*
		 * Collect results and write to disk.
		 * Results may arrive in any order, so we loop chunk_count times
		 * and use job->job_id to determine which chunk it is.
		 */
		for (i = 0; i < sc->chunk_count; i++) {
		struct compress_job *job;

		job = parallel_compressor_get_result(sc->compressor);

		if (!job) {
			error("failed to get compression result");
			errors_encountered++;
			continue;
		}


		/* Validate job_id is in range */
		if (job->job_id < 0 || (size_t)job->job_id >= sc->chunk_count) {
			error("invalid job_id %d (expected 0-%zu)",
			      job->job_id, sc->chunk_count - 1);
			errors_encountered++;
			if (job->compressed)
				free(job->compressed);
			free(job);
			continue;
		}

		/* Check for compression errors */
		if (job->error_code != COMPRESS_OK) {
			error("chunk %d compression failed: %s",
			      job->job_id, job->error_message);
			errors_encountered++;
			if (job->compressed)
				free(job->compressed);
			free(job);
			continue;
		}

		/* Write compressed chunk to disk as loose object */
		t_start = get_time_ms();
		if (write_compressed_loose_object(&job->oid, job->compressed,
		                                   job->compressed_size) < 0) {
			t_end = get_time_ms();
			time_write_ms += (t_end - t_start);
			error("failed to write chunk %d to disk", job->job_id);
			errors_encountered++;
			if (job->compressed) {
				free(job->compressed);
			}
			free(job);
			break;  /* Stop processing - don't try to get more results */
		}
		t_end = get_time_ms();
		time_write_ms += (t_end - t_start);

		/* Save OID (results come in order, so job_id == i) */
		oidcpy(&sc->chunk_oids[job->job_id], &job->oid);

		/* Free job resources */
		if (job->compressed)
			free(job->compressed);
		free(job);
		}

		/* If any errors occurred, abort */
		if (errors_encountered > 0) {
			strbuf_addf(&sc->error_message,
			           "%d chunks failed to compress or write",
			           errors_encountered);
			sc->error_occurred = 1;
			return -1;
		}
	}  /* End of parallel compression path */

	/*
	 * Wait for async content hasher to complete (if using async hashing).
	 * This ensures all hash updates are processed before we finalize.
	 */
	if (sc->hasher) {
		double t_start, t_end;
		t_start = get_time_ms();
		async_hasher_finish(sc->hasher);
		sc->hasher = NULL;  /* Hasher is now freed */
		t_end = get_time_ms();
		time_content_hash_ms += (t_end - t_start);
	}

	/*
	 * Finalize content OID.
	 * This has been computed incrementally during file reading as:
	 *   hash("blob <total_size>\0" + entire_file_data)
	 *
	 * This represents what Git would have computed if it hashed the
	 * entire file as a single blob object.
	 *
	 * For single-chunk files: content_oid == chunk_oids[0]
	 * For multi-chunk files: content_oid = hash of reconstructed file
	 */
	if (!sc->content_hash_initialized) {
		strbuf_addstr(&sc->error_message, "content hash not initialized");
		sc->error_occurred = 1;
		return -1;
	}

	the_hash_algo->final_oid_fn(content_oid, &sc->content_hash_ctx);

	/* Create manifest object */
	if (write_manifest_object(the_repository, manifest_oid, sc->total_size,
	                          content_oid, sc->chunk_count,
	                          sc->chunk_oids) < 0) {
		strbuf_addstr(&sc->error_message, "failed to create manifest object");
		sc->error_occurred = 1;
		return -1;
	}

	/* Display timing breakdown */
	if (sc->compressor) {
		double total_ms = time_read_ms + time_content_hash_ms + time_gear_hash_ms + time_compress_ms + time_write_ms;
		fprintf(stderr, "\n[BENCH] Timing breakdown for %zu chunks (%.2f MB):\n",
		        sc->chunk_count, sc->total_size / (1024.0 * 1024.0));
		fprintf(stderr, "[BENCH]   Read I/O:       %7.2f ms (%5.1f%%)\n",
		        time_read_ms, 100.0 * time_read_ms / total_ms);
		fprintf(stderr, "[BENCH]   Content hash:   %7.2f ms (%5.1f%%) [full-file SHA256]\n",
		        time_content_hash_ms, 100.0 * time_content_hash_ms / total_ms);
		fprintf(stderr, "[BENCH]   Gear hash:      %7.2f ms (%5.1f%%) [boundary detect]\n",
		        time_gear_hash_ms, 100.0 * time_gear_hash_ms / total_ms);
		fprintf(stderr, "[BENCH]   Compression:    %7.2f ms (%5.1f%%) [parallel]\n",
		        time_compress_ms, 100.0 * time_compress_ms / total_ms);
		fprintf(stderr, "[BENCH]   Write I/O:      %7.2f ms (%5.1f%%)\n",
		        time_write_ms, 100.0 * time_write_ms / total_ms);
		fprintf(stderr, "[BENCH]   TOTAL:          %7.2f ms\n", total_ms);
	}

	return 0;
}

void streaming_chunker_cleanup(struct streaming_chunker *sc)
{
	/* Clean up async hasher (if not already finished) */
	if (sc->hasher) {
		async_hasher_finish(sc->hasher);
		sc->hasher = NULL;
	}

	/* Clean up parallel compressor */
	if (sc->compressor) {
		parallel_compressor_cleanup(sc->compressor);
		free(sc->compressor);
		sc->compressor = NULL;
	}

	/* Free arrays */
	free(sc->chunk_oids);

	/* Release strbufs */
	strbuf_release(&sc->chunk_buffer);
	strbuf_release(&sc->error_message);

	memset(sc, 0, sizeof(*sc));
}
