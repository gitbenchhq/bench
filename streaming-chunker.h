#ifndef STREAMING_CHUNKER_H
#define STREAMING_CHUNKER_H

#include "git-compat-util.h"
#include "object.h"
#include "strbuf.h"
#include "chunker.h"

/*
 * Streaming chunker for large files
 *
 * This module provides memory-efficient chunking of arbitrarily large files
 * using a streaming approach. Files are read in small pages (64KB) and
 * chunks are buffered one at a time (max 64MB per chunk).
 *
 * Key features:
 * - Bounded memory usage (max ~64MB for largest chunk + overhead)
 * - Content-defined chunking using Gear hash for deduplication
 * - Supports both file descriptor and in-memory buffer inputs
 */

#define STREAMING_BUFFER_SIZE 65536  /* 64KB read buffer */

/*
 * Streaming chunker state
 */
struct streaming_chunker {
	/* Input streaming - file mode */
	int fd;                          /* File descriptor to read from (-1 if using buffer) */
	unsigned char read_buffer[STREAMING_BUFFER_SIZE];

	/* Input streaming - buffer mode */
	const char *buffer;              /* Source buffer (if fd == -1) */
	size_t buffer_len;               /* Total buffer size */
	size_t buffer_pos;               /* Current read position in buffer */

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

	/* Content hash (for entire file as if it were one blob) */
	struct git_hash_ctx content_hash_ctx; /* Hash of "blob <total_size>\0" + entire_file_data */
	int content_hash_initialized;    /* Set to 1 after header is hashed */

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
 * Initialize a streaming chunker from a memory buffer
 *
 * This is used when we've already loaded and filtered a file into memory
 * (e.g., for medium-sized files with text filters applied).
 *
 * @param sc Streaming chunker state to initialize
 * @param data Pointer to buffer containing file data
 * @param len Length of buffer in bytes
 *
 * Returns 0 on success, -1 on error
 */
int streaming_chunker_init_from_buffer(struct streaming_chunker *sc,
                                        const char *data, size_t len);

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
