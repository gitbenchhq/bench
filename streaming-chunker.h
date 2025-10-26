#ifndef STREAMING_CHUNKER_H
#define STREAMING_CHUNKER_H

#include "git-compat-util.h"
#include "object.h"
#include "strbuf.h"
#include "chunker.h"

/* Forward declaration */
struct parallel_compressor;

/*
 * Streaming chunker for large files
 *
 * This module provides memory-efficient chunking of arbitrarily large files
 * using a streaming approach. Files are read in small pages (8KB) and
 * chunks are buffered one at a time (max 128MB per chunk).
 *
 * Key features:
 * - Bounded memory usage (max ~128MB for largest chunk + overhead)
 * - Parallel compression of chunks for performance
 * - Automatic fallback if chunking disabled or file too small
 */

#define STREAMING_BUFFER_SIZE 65536  /* 64KB read buffer for optimal SHA-NI performance */

/*
 * Async content hashing support.
 * A dedicated thread processes content hash updates in parallel with
 * the main thread's Gear hash and chunk boundary detection.
 */
struct hash_job {
	unsigned char *data;
	size_t size;
};

struct async_hasher {
	pthread_t thread;
	pthread_mutex_t lock;
	pthread_cond_t has_work;
	pthread_cond_t has_space;

	struct hash_job *queue;
	size_t queue_size;
	size_t queue_head;
	size_t queue_tail;
	size_t queue_count;

	struct git_hash_ctx *hash_ctx;  /* Pointer to content_hash_ctx */
	int shutdown;
	int error;
};

/*
 * Streaming chunker state
 */
struct streaming_chunker {
	/* Input streaming */
	int fd;                          /* File descriptor to read from */
	unsigned char read_buffer[STREAMING_BUFFER_SIZE];

	/* Chunker state */
	struct chunk_config chunk_config; /* Chunking configuration */
	uint64_t fingerprint;            /* Rolling hash state */
	size_t current_chunk_size;       /* Size of current chunk */

	/* Current chunk being built */
	struct strbuf chunk_buffer;      /* Buffer for current chunk (max 128MB) */

	/* Completed chunks */
	struct object_id *chunk_oids;    /* Array of completed chunk OIDs */
	size_t chunk_count;              /* Number of chunks created */
	size_t chunk_capacity;           /* Allocated capacity for chunk_oids */

	/* Parallel compression */
	struct parallel_compressor *compressor; /* Parallel compression pool */

	/* Content hash (for entire file as if it were one blob) */
	struct git_hash_ctx content_hash_ctx; /* Hash of "blob <total_size>\0" + entire_file_data */
	int content_hash_initialized;    /* Set to 1 after header is hashed */
	struct async_hasher *hasher;     /* Async content hash thread (optional) */

	/* Total file stats */
	uint64_t total_size;             /* Total bytes processed */

	/* Error handling */
	int error_occurred;              /* Set to 1 on error */
	struct strbuf error_message;     /* Detailed error message */
};

/*
 * Initialize a streaming chunker for the given file descriptor
 *
 * @param sc Streaming chunker state to initialize
 * @param fd File descriptor to read from
 * @param file_size Total size of the file (needed for content hash header)
 *
 * Returns 0 on success, -1 on error
 */
int streaming_chunker_init(struct streaming_chunker *sc, int fd, off_t file_size);

/*
 * Stream the entire file, creating chunks and storing them in ODB
 *
 * This is the main workhorse function. It:
 * 1. Reads the file in 8KB pages
 * 2. Detects chunk boundaries using content-defined chunking
 * 3. Writes each chunk to ODB using write_object_file
 * 4. Tracks all chunk OIDs for manifest creation
 *
 * Returns 0 on success, -1 on error
 */
int streaming_chunker_process_file(struct streaming_chunker *sc);

/*
 * Finalize by creating manifest object
 *
 * This:
 * 1. Computes content OID from all chunks
 * 2. Creates manifest object in ODB
 *
 * Returns 0 on success, -1 on error
 * On success, manifest_oid and content_oid are populated
 */
int streaming_chunker_finalize(struct streaming_chunker *sc,
                                struct object_id *manifest_oid,
                                struct object_id *content_oid);

/*
 * Clean up all resources
 */
void streaming_chunker_cleanup(struct streaming_chunker *sc);

/*
 * Check if a file should be chunked based on size
 *
 * IMPORTANT: Assumes we're already in bench mode. The caller should check
 * repo_has_bench_extensions() before calling this function.
 *
 * Uses repo settings:
 *   - chunk.minSize: Files > this size will be chunked (default: 2MB)
 *
 * Files <= chunk.minSize will be stored as single-chunk manifests.
 * Files > chunk.minSize will use content-defined chunking.
 *
 * Returns 1 if file should be chunked (size > threshold), 0 otherwise
 */
int should_chunk_file(off_t file_size);

#endif /* STREAMING_CHUNKER_H */
