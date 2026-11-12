#ifndef MANIFEST_H
#define MANIFEST_H

#include "object.h"

struct oid_array;

extern const char *manifest_type;

struct manifest {
	struct object object;
	/*
	 * Manifest contains an ordered list of blob/chunk OIDs.
	 * We don't store the OIDs in memory - use manifest-walk.h
	 * to iterate through them when needed.
	 */
	void *buffer;
	unsigned long size;
};

struct manifest *lookup_manifest(struct repository *r, const struct object_id *oid);

/**
 * Attach a manifest's raw buffer to its object (parsed lazily by readers).
 *
 * Manifest format (version 1):
 *
 *	1 LF                          version
 *	<total_size> LF               sum of the chunk sizes
 *	<content_oid> LF              git blob id of the whole file:
 *	                              hash("blob <total_size>\0" + content)
 *	<chunk_count> LF
 *	<chunk_oid> SP <size> LF      once per chunk, in file order
 *
 * Numbers are canonical decimal, OIDs lowercase hex, nothing follows the
 * last line. Returns 0 on success, -1 on error.
 **/
int parse_manifest_buffer(struct repository *r, struct manifest *item, void *buffer, unsigned long size);


/**
 * Write a manifest object directly to the object database.
 * chunk_sizes[i] is the uncompressed size of chunk_oids[i]; total_size
 * must equal their sum. Always writes the release format (version 1):
 * the format is fixed by the bench release, never by configuration, so
 * the same chunks always produce the same manifest bytes.
 * Returns 0 on success, -1 on error.
 **/
int write_manifest_object(struct repository *r, struct object_id *oid,
                         uint64_t total_size,
                         const struct object_id *content_oid,
                         size_t chunk_count,
                         const struct object_id *chunk_oids,
                         const uint64_t *chunk_sizes);

/**
 * Hash a manifest object without writing it to the database.
 * This creates the same manifest format as write_manifest_object but only
 * computes the hash. Used when INDEX_WRITE_OBJECT flag is not set.
 * Returns 0 on success, -1 on error.
 **/
int hash_manifest_object(struct repository *r, struct object_id *oid,
                        uint64_t total_size,
                        const struct object_id *content_oid,
                        size_t chunk_count,
                        const struct object_id *chunk_oids,
                        const uint64_t *chunk_sizes);

/**
 * Get a manifest's content size and content OID (the id of the complete
 * file content after filters) from its header.
 * Returns 0 on success, -1 on error.
 **/
int get_manifest_content_info(struct repository *r,
			      const struct object_id *manifest_oid,
			      uint64_t *total_size,
			      struct object_id *content_oid);

/**
 * Free the memory associated with a manifest's chunk list.
 **/
void free_manifest(struct manifest *m);

/* Streaming interface for manifest content */
struct manifest_stream;

/**
 * Open a stream to read content from a manifest.
 * This transparently handles reading across multiple chunks.
 * Returns NULL on error.
 **/
struct manifest_stream *open_manifest_stream(struct repository *r,
                                             const struct object_id *manifest_oid,
                                             unsigned long *size);

/**
 * Read data from a manifest stream.
 * Transparently handles chunk boundaries - consumers don't need to
 * know about the underlying chunking.
 * Returns the number of bytes read, 0 at end of stream, or -1 on error.
 **/
ssize_t read_manifest_stream(struct manifest_stream *stream, void *buf, size_t count);

/**
 * Close a manifest stream and free associated resources.
 * Returns 0 on success, -1 on error.
 **/
int close_manifest_stream(struct manifest_stream *stream);

/**
 * Stream manifest content to a file descriptor.
 * This is a convenience function that handles the entire streaming process.
 * Returns 0 on success, -1 on error.
 **/
int stream_manifest_to_fd(struct repository *r, int fd, const struct object_id *manifest_oid);

/**
 * Stream manifest content to a file descriptor with filtering.
 * This applies the given stream filter (CRLF conversion, clean/smudge, ident)
 * while streaming manifest content. The filter is freed by this function.
 * Returns 0 on success, -1 on error.
 **/
struct stream_filter;
int stream_manifest_to_fd_filtered(struct repository *r, int fd,
                                   const struct object_id *manifest_oid,
                                   struct stream_filter *filter);

/**
 * Get the total size and chunk OIDs from a manifest.
 * This is used for reachability analysis and filtering.
 * The chunk_oids array will be populated with the OIDs.
 * Returns 0 on success, -1 on error.
 * On success, *total_size contains the sum of all chunk sizes.
 **/
int get_manifest_chunk_oids(struct repository *r,
                           const struct object_id *manifest_oid,
                           uint64_t *total_size,
                           struct oid_array *chunk_oids);

/**
 * Read the entire manifest content into memory.
 * This is used when streaming is not appropriate (small files, or when
 * filters need the entire content at once).
 * The caller is responsible for freeing the returned buffer.
 * Returns the buffer on success, NULL on error.
 * On success, *size contains the total content size.
 **/
void *read_manifest_content(struct repository *r,
                           const struct object_id *manifest_oid,
                           unsigned long *size);

/**
 * Get the logical size of a manifest without reading all content.
 * This reads just the manifest header to get the total size.
 * Returns 0 on success with size in *size, -1 on error.
 **/
int get_manifest_size(struct repository *r,
                     const struct object_id *manifest_oid,
                     uint64_t *size);

/* The only manifest version this bench reads and writes. */
#define MANIFEST_VERSION 1

/*
 * The format has no chunk-count cap. What limits a manifest is memory:
 * bench builds one in memory while writing it, using about this many bytes
 * per chunk at its peak (the OID and size arrays, the text, and its
 * compressed copy; ~200 typical, more while buffers grow). A remote may
 * set its own, lower limit.
 */
#define MANIFEST_MEMORY_PER_CHUNK 256

/*
 * Check that a manifest of chunk_count chunks fits the memory bench may use
 * for one (bench.manifestMemoryLimit, default a quarter of RAM). Returns 0
 * if it fits, otherwise appends a message to err and returns -1. The
 * chunkers call it for every chunk, so a file that is too large stops
 * early instead of after reading all of it.
 */
int manifest_check_memory(struct repository *r, size_t chunk_count,
			  struct strbuf *err);

struct strbuf;

/* A manifest's header fields, filled in by parse_manifest(). */
struct manifest_header {
	int version;
	uint64_t total_size;
	struct object_id content_oid;  /* OID of complete file content (after filters) */
	size_t chunk_count;
	const char *chunk_data;  /* Pointer to the first "<oid> <size>" line */
	size_t chunk_data_len;   /* Length of the chunk lines in bytes */
};

/*
 * What is wrong with a manifest. parse_manifest() returns the format
 * errors and check_manifest_chunks() the chunk errors; fsck maps each one
 * to its own message ID.
 */
enum manifest_error {
	MANIFEST_OK = 0,
	/* format: the manifest bytes themselves */
	MANIFEST_ERR_BAD_VERSION,          /* version line missing or not canonical */
	MANIFEST_ERR_UNKNOWN_VERSION,      /* canonical, but not MANIFEST_VERSION */
	MANIFEST_ERR_BAD_TOTAL_SIZE,
	MANIFEST_ERR_BAD_CONTENT_OID,
	MANIFEST_ERR_BAD_CHUNK_COUNT,      /* not canonical, or 0 */
	MANIFEST_ERR_BAD_CHUNK_LINE,
	MANIFEST_ERR_CHUNK_COUNT_MISMATCH, /* lines != chunk_count, or trailing bytes */
	MANIFEST_ERR_SIZE_MISMATCH,        /* sizes do not sum to total_size */
	/* chunks: needs the chunk objects */
	MANIFEST_ERR_CHUNK_WRONG_TYPE,
	MANIFEST_ERR_CHUNK_SIZE_MISMATCH,  /* chunk object size != its line's size */
	MANIFEST_ERR_CHUNK_CORRUPT,        /* chunk bytes do not hash to its OID */
	MANIFEST_ERR_CONTENT_MISMATCH,     /* content_oid is not the file's blob id */
};

/**
 * Parse a manifest strictly: one valid encoding per manifest, so the same
 * chunks always give the same bytes and the same manifest id.
 * The version is exactly MANIFEST_VERSION; numbers are canonical decimal;
 * OIDs are lowercase hex of `algo`; chunk_count is at least 1 and equals
 * the number of chunk lines; nothing follows the last line;
 * the sizes sum to total_size.
 *
 * With MANIFEST_PARSE_HEADER_ONLY in `flags`, only the four header lines
 * are checked and the chunk lines are not walked: constant cost, for
 * callers that need only total_size or content_oid. Anything that reads
 * the chunk lines must parse without it, so a damaged manifest is
 * caught before its chunks are used.
 *
 * On success fills `header` and returns MANIFEST_OK. Otherwise returns
 * the error and, if `err` is not NULL, appends a message to it.
 **/
#define MANIFEST_PARSE_HEADER_ONLY (1u << 0)
enum manifest_error parse_manifest(const struct git_hash_algo *algo,
				   const void *buffer, unsigned long size,
				   unsigned flags,
				   struct manifest_header *header,
				   struct strbuf *err);

/**
 * Check a parsed manifest against the chunk objects in the local object
 * store: each one is a blob with the size its line says (no chunk data is
 * read). With `full`, also read every chunk, check it hashes to its OID,
 * and check content_oid is hash("blob <total_size>\0" + content).
 *
 * Chunks that are not stored locally are skipped, never fetched: fsck's
 * connectivity check reports missing ones, and a partial clone may lack
 * them on purpose. content_oid is only checked when every chunk is local.
 *
 * Returns MANIFEST_OK or the first error, with a message appended to err.
 **/
enum manifest_error check_manifest_chunks(struct repository *r,
					  const struct manifest_header *header,
					  int full, struct strbuf *err);

#endif /* MANIFEST_H */