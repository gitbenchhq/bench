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
 * Parse a manifest buffer and extract the chunk OID references.
 * Format: One hex OID per line (40 chars for SHA-1, 64 for SHA-256).
 * Returns 0 on success, -1 on error.
 **/
int parse_manifest_buffer(struct repository *r, struct manifest *item, void *buffer, unsigned long size);


/**
 * Write a manifest object directly to the object database.
 * This is a simplified interface for object-file.c that handles the manifest
 * format internally based on the configured version.
 * Returns 0 on success, -1 on error.
 **/
int write_manifest_object(struct repository *r, struct object_id *oid,
                         unsigned long total_size,
                         const struct object_id *content_oid,
                         size_t chunk_count,
                         const struct object_id *chunk_oids);

/**
 * Hash a manifest object without writing it to the database.
 * This creates the same manifest format as write_manifest_object but only
 * computes the hash. Used when INDEX_WRITE_OBJECT flag is not set.
 * Returns 0 on success, -1 on error.
 **/
int hash_manifest_object(struct repository *r, struct object_id *oid,
                        unsigned long total_size,
                        const struct object_id *content_oid,
                        size_t chunk_count,
                        const struct object_id *chunk_oids);

/**
 * Get the content OID from a manifest object.
 * This returns the OID representing the complete file content after filters.
 * Returns 0 on success, -1 on error.
 **/
int get_manifest_content_oid(struct repository *r,
                             const struct object_id *manifest_oid,
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
                           unsigned long *total_size,
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
                     unsigned long *size);

/* Validation functions for fsck */
struct strbuf;

/* Manifest header structure for validation */
struct manifest_header {
	int version;
	unsigned long total_size;
	struct object_id content_oid;  /* OID of complete file content (after filters) */
	size_t chunk_count;
	const char *chunk_data;  /* Pointer to start of chunk OID data */
	size_t chunk_data_len;   /* Length of chunk OID data in bytes */
};

/**
 * Validate manifest header structure and report errors to buffer.
 * This is similar to parse_manifest_header but reports validation errors
 * to err buffer instead of calling error() directly.
 * Returns 0 on success, -1 on error.
 **/
int validate_manifest_header(struct repository *r, const void *buffer, unsigned long size,
                           struct manifest_header *header,
                           struct strbuf *err);

/**
 * Verify that all chunk OIDs in a manifest exist and are blobs.
 * Returns 0 if all chunks are valid, -1 if any are missing or wrong type.
 * Errors are reported to err buffer.
 **/
int validate_manifest_chunks(struct repository *r,
                           const struct object_id *chunk_oids,
                           size_t chunk_count,
                           struct strbuf *err);

/**
 * Verify manifest data integrity by checking that chunk hashes match content.
 * This validates that the content_oid matches the hash of concatenated chunks.
 * Returns 0 if integrity check passes, -1 on error.
 * Errors are reported to err buffer.
 **/
int verify_manifest_integrity(struct repository *r,
                            const struct manifest_header *header,
                            const struct object_id *chunk_oids,
                            struct strbuf *err);

#endif /* MANIFEST_H */