#ifndef STREAMING_CHUNKER_THREADS_H
#define STREAMING_CHUNKER_THREADS_H

/*
 * BENCH_THREADS: Parallel multi-threaded version of streaming-chunker
 *
 * This is a DUPLICATE of streaming-chunker.c/h with parallel compression added.
 * Most logic is identical to streaming-chunker.c, but the processing loop
 * uses a producer-consumer model with worker threads.
 *
 * IMPORTANT: Keep this separate from streaming-chunker during validation phase.
 * If parallel compression proves successful, we may merge these files in the future.
 * For now, separation allows easy rollback and maintains stable serial code path.
 *
 * Architecture:
 * - Producer (main thread): Reads file in 64KB buffers, performs dual hashing
 *   (SHA256 content + Gear boundary), accumulates into chunk buffers
 * - Worker pool: Compresses chunks in parallel, computes per-chunk SHA256
 * - Consumer (main thread): Writes compressed chunks to pack file in sequential order
 * - Backpressure: Producer blocks when all workers busy
 * - Ordered output: Consumer waits for next sequential chunk
 *
 * Memory constraint: Never exceed 1/2 total RAM (auto-detected, see repo-settings.c)
 *
 * This file marked with BENCH_THREADS for easy identification and potential removal.
 */

#ifdef BENCH_THREADS

#include "git-compat-util.h"
#include "object.h"
#include "strbuf.h"
#include "chunker.h"
#include <pthread.h>

#define STREAMING_BUFFER_SIZE 65536  /* 64KB read buffer */

/* Worker thread status */
typedef enum {
	WORKER_IDLE,     /* Ready to accept work */
	WORKER_WORKING,  /* Currently processing */
	WORKER_DONE      /* Finished, ready for consumer */
} worker_status_t;

/*
 * Worker thread state
 * Each worker has its own mutex and condition variable for thread-safe communication
 */
struct worker_state {
	pthread_t thread;
	pthread_mutex_t mutex;
	pthread_cond_t cond;

	worker_status_t status;
	int chunk_number;              /* Which chunk (0, 1, 2, ...) */

	/* Input: uncompressed chunk data */
	unsigned char *input_buffer;   /* Allocated to max_chunk_size */
	size_t input_size;             /* Actual chunk size */

	/* Output: compressed chunk data + chunk OID */
	unsigned char *output_buffer;  /* Allocated to compressBound(max_chunk_size) */
	size_t output_size;            /* Size after compression */
	struct object_id chunk_oid;    /* SHA256 of this chunk (for manifest) */

	int error;                     /* Error flag */
	int should_exit;               /* Signal to thread to exit */
	int worker_id;                 /* For debugging/logging */
};

/*
 * Streaming chunker state with parallel compression support
 */
struct streaming_chunker_threads {
	/* Input streaming - file mode */
	int fd;                          /* File descriptor to read from (-1 if using buffer) */
	unsigned char read_buffer[STREAMING_BUFFER_SIZE];

	/* Input streaming - buffer mode */
	const char *buffer;              /* Source buffer (if fd == -1) */
	size_t buffer_len;               /* Total buffer size */
	size_t buffer_pos;               /* Current read position in buffer */

	/* Chunker state */
	struct chunk_config chunk_config; /* Chunking configuration */
	uint64_t fingerprint;            /* Rolling hash state for boundary detection */

	/* Current chunk being built (by producer) */
	struct strbuf chunk_buffer;      /* Buffer for current chunk (max max_chunk_size) */
	size_t current_chunk_size;       /* Size of current chunk */

	/* Content hash (for entire file as if it were one blob) */
	struct git_hash_ctx content_hash_ctx; /* Hash of "blob <total_size>\0" + entire_file_data */
	int content_hash_initialized;    /* Set to 1 after header is hashed */

	/* Total file stats */
	uint64_t total_size;             /* Total bytes processed */

	/* Worker pool */
	struct worker_state *workers;    /* Array of worker threads */
	int num_workers;                 /* Number of workers (from repo settings) */

	/* Chunk tracking */
	int next_chunk_to_produce;       /* Next chunk number to create */
	int next_chunk_to_consume;       /* Next chunk number to write */
	int total_chunks;                /* Total chunks created (set at EOF) */
	int reading_done;                /* Flag: finished reading input */

	/* Completed chunks (for manifest creation) */
	struct object_id *chunk_oids;    /* Array of completed chunk OIDs */
	size_t chunk_count;              /* Number of chunks created */
	size_t chunk_capacity;           /* Allocated capacity for chunk_oids */

	/* Error handling */
	int error_occurred;              /* Set to 1 on error */
	struct strbuf error_message;     /* Detailed error message */

	/* BENCH_THREADS_DEBUG: Memory usage tracking (temporary instrumentation) */
#ifdef BENCH_THREADS_DEBUG
	unsigned long peak_memory_mb;    /* Peak RSS in MB */
#endif
};

/*
 * Initialize a parallel streaming chunker for the given file descriptor
 *
 * @param sc Streaming chunker state to initialize
 * @param fd File descriptor to read from
 * @param file_size Total size of the file (needed for content hash header)
 *
 * Returns 0 on success, -1 on error
 */
int streaming_chunker_threads_init(struct streaming_chunker_threads *sc, int fd, off_t file_size);

/*
 * Initialize a parallel streaming chunker from a memory buffer
 *
 * @param sc Streaming chunker state to initialize
 * @param data Pointer to buffer containing file data
 * @param len Length of buffer in bytes
 *
 * Returns 0 on success, -1 on error
 */
int streaming_chunker_threads_init_from_buffer(struct streaming_chunker_threads *sc,
                                                 const char *data, size_t len);

/*
 * Stream the entire file, creating chunks and storing them in ODB (parallel version)
 *
 * This is the main workhorse function. It:
 * 1. Reads the file in 64KB pages (producer in main thread)
 * 2. Performs dual hashing: SHA256 content + Gear boundary detection
 * 3. Detects chunk boundaries using content-defined chunking
 * 4. Dispatches chunks to worker threads for parallel compression
 * 5. Consumes compressed chunks in sequential order and writes to ODB
 *
 * Returns 0 on success, -1 on error
 */
int streaming_chunker_threads_process_file(struct streaming_chunker_threads *sc);

/*
 * Finalize by creating manifest object
 *
 * This:
 * 1. Computes content OID from content hash
 * 2. Creates manifest object in ODB with all chunk OIDs
 *
 * Returns 0 on success, -1 on error
 * On success, manifest_oid and content_oid are populated
 */
int streaming_chunker_threads_finalize(struct streaming_chunker_threads *sc,
                                         struct object_id *manifest_oid,
                                         struct object_id *content_oid);

/*
 * Clean up all resources (including worker threads)
 */
void streaming_chunker_threads_cleanup(struct streaming_chunker_threads *sc);

#endif /* BENCH_THREADS */

#endif /* STREAMING_CHUNKER_THREADS_H */
