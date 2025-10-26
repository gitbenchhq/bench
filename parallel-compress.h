#ifndef PARALLEL_COMPRESS_H
#define PARALLEL_COMPRESS_H

#include "object.h"
#include "thread-utils.h"

/*
 * Parallel Compression API
 *
 * This module provides parallel zlib compression for Git/Bench objects.
 * It's used by both:
 *   - bench add: Compress chunks in parallel during file ingestion
 *   - bench gc/repack: Compress objects in parallel when creating pack files
 *
 * Architecture:
 *   - Thread pool with configurable number of worker threads
 *   - Bounded job queue with backpressure (prevents memory explosion)
 *   - Each job: uncompressed data → SHA256 + zlib → compressed data + OID
 *   - Results returned in submission order
 *
 * Memory safety:
 *   - Queue size limited by both thread count AND chunk size
 *   - Producer blocks when queue is full (backpressure)
 *   - Constant memory usage (~512MB) regardless of file size
 */

/* Error codes for compression jobs */
#define COMPRESS_OK             0
#define COMPRESS_ERR_HASH      -1  /* SHA256 computation failed */
#define COMPRESS_ERR_DEFLATE   -2  /* zlib compression failed */
#define COMPRESS_ERR_NOMEM     -3  /* Out of memory */

/* Compression job: input data → compressed output */
struct compress_job {
	/* Input (set by caller) */
	void *data;                      /* Uncompressed data */
	unsigned long size;              /* Size of uncompressed data */
	enum object_type type;           /* OBJ_BLOB, OBJ_TREE, etc. */
	int job_id;                      /* For ordering results */

	/* Output (filled by worker thread) */
	struct object_id oid;            /* SHA256 of object */
	void *compressed;                /* Compressed data (caller must free) */
	unsigned long compressed_size;   /* Size after compression */
	int error_code;                  /* COMPRESS_OK or error code */
	char error_message[256];         /* Human-readable error if failed */
};

/* Forward declaration */
struct repository;

/* Helper functions */
unsigned long get_total_memory_mb(void);

/* Parallel compressor state */
struct parallel_compressor {
	/* Configuration */
	struct repository *repo;
	unsigned int num_threads;
	size_t input_queue_size;         /* Fixed size for input (backpressure) */
	size_t output_queue_size;        /* Grows as needed */

	/* Thread pool */
	pthread_t *threads;
	int shutdown;                    /* Signal threads to exit */

	/* Job queues (circular buffers) */
	struct compress_job **input_queue;
	struct compress_job **output_queue;
	size_t input_head, input_tail, input_count;
	size_t output_head, output_tail, output_count;

	/* Synchronization primitives */
	pthread_mutex_t input_lock;
	pthread_mutex_t output_lock;
	pthread_cond_t input_ready;      /* Signal: jobs available for workers */
	pthread_cond_t input_space;      /* Signal: space available in input queue */
	pthread_cond_t output_ready;     /* Signal: results available for caller */

	/* Statistics */
	size_t jobs_submitted;
	size_t jobs_completed;
};

/*
 * Initialize parallel compressor
 *
 * Creates worker threads and allocates queues based on:
 *   - core.compressionThreads (default: num_cpus / 2)
 *   - core.compressionQueueMemory (default: 512 MB)
 *   - chunk.maxSize (to calculate safe queue depth)
 *
 * Returns 0 on success, -1 on error
 */
int parallel_compressor_init(struct parallel_compressor *pc,
                             struct repository *repo);

/*
 * Submit job for compression (may block if queue is full)
 *
 * The compressor takes ownership of 'data' and will free it after compression.
 * Caller must not access 'data' after submission.
 *
 * Returns 0 on success, -1 on error
 */
int parallel_compressor_submit(struct parallel_compressor *pc,
                               void *data,
                               unsigned long size,
                               enum object_type type,
                               int job_id);

/*
 * Get next completed job (blocks until available)
 *
 * Results are returned in submission order (by job_id).
 * Caller must check job->error_code before using results.
 * Caller is responsible for freeing job->compressed and job itself.
 *
 * Returns compress_job on success, NULL if no more results
 */
struct compress_job *parallel_compressor_get_result(struct parallel_compressor *pc);

/*
 * Wait for all pending jobs to complete
 *
 * Call this before get_result() loop to ensure all jobs finish.
 */
void parallel_compressor_finish(struct parallel_compressor *pc);

/*
 * Clean up and free all resources
 *
 * Signals worker threads to exit and joins them.
 * Frees all queues and synchronization primitives.
 * Safe to call even if init failed partway through.
 */
void parallel_compressor_cleanup(struct parallel_compressor *pc);

#endif /* PARALLEL_COMPRESS_H */
