#include "git-compat-util.h"
#include "manifest.h"
#include "object-file.h"
#include "repository.h"
#include "alloc.h"
#include "hex.h"
#include "strbuf.h"
#include "hash.h"
#include "config.h"
#include "streaming.h"
#include "convert.h"
#include "trace.h"
#include "manifest-walk.h"
#include "write-or-die.h"
#include "oid-array.h"
#include "environment.h"
#include "object-name.h"
#include "object.h"

const char *manifest_type = "manifest";

struct manifest *lookup_manifest(struct repository *r, const struct object_id *oid)
{
	struct object *obj = lookup_object(r, oid);
	if (!obj)
		return create_object(r, oid, alloc_manifest_node(r));
	return object_as_type(obj, OBJ_MANIFEST, 0);
}

int parse_manifest_buffer(struct repository *r, struct manifest *item, void *buffer, unsigned long size)
{
	if (item->object.parsed)
		return 0;

	/*
	 * Just store the buffer like Git does for trees.
	 * We'll parse OIDs on-demand using manifest-walk.
	 */
	item->buffer = buffer;
	item->size = size;
	item->object.parsed = 1;
	return 0;
}

void free_manifest(struct manifest *m)
{
	if (!m)
		return;
	FREE_AND_NULL(m->buffer);
	m->size = 0;
}

struct manifest_stream {
	struct repository *repo;
	struct manifest_desc desc;
	void *manifest_buffer;  /* Manifest content buffer */
	unsigned long manifest_size;  /* Size of manifest */
	struct git_istream *current_chunk_stream;
	struct object_id current_chunk_oid;
	unsigned long total_size;
	unsigned long bytes_read;
	int initialized;
	int at_end;
};

/* manifest_header struct is now defined in manifest.h */

/*
 * Parse manifest header from buffer.
 * This centralizes all manifest version parsing logic.
 * Returns 0 on success, -1 on error.
 */
static int parse_manifest_header(const void *buffer, unsigned long size,
                                 struct manifest_header *header)
{
	const char *p = buffer;
	const char *end = (const char *)buffer + size;
	
	/* Parse version */
	header->version = strtol(p, (char **)&p, 10);
	if (p >= end || *p++ != '\n') {
		error("manifest missing version number");
		return -1;
	}
	
	/* Version check: We currently only support version 1 */
	if (header->version < 1) {
		error("manifest has invalid version %d", header->version);
		return -1;
	}
	if (header->version > 1) {
		error("manifest version %d is newer than supported version 1", header->version);
		return -1;
	}
	
	/* Version 1 parsing - in future, use switch(version) for other versions */
	if (header->version == 1) {
		/* Parse total size */
		header->total_size = strtoul(p, (char **)&p, 10);
		if (p >= end || *p++ != '\n') {
			error("manifest missing total size");
			return -1;
		}
		
		/* Parse content OID (complete file hash after filters) */
		int algo_idx = get_oid_hex_any(p, &header->content_oid);
		if (algo_idx == GIT_HASH_UNKNOWN) {
			error("manifest missing or invalid content OID");
			return -1;
		}
		p += hash_algos[algo_idx].hexsz;
		if (p >= end || *p++ != '\n') {
			error("manifest content OID not followed by newline");
			return -1;
		}
		
		/* Parse chunk count */
		header->chunk_count = strtoul(p, (char **)&p, 10);
		if (p >= end || *p++ != '\n') {
			error("manifest missing chunk count");
			return -1;
		}
	}
	/* Future versions would have their parsing logic here */
	
	/* Set pointer to chunk OID data */
	header->chunk_data = p;
	header->chunk_data_len = end - p;
	
	return 0;
}

struct manifest_stream *open_manifest_stream(struct repository *r,
                                             const struct object_id *manifest_oid,
                                             unsigned long *size)
{
	struct manifest_stream *stream;
	enum object_type type;
	unsigned long manifest_size;
	void *manifest_buffer;
	struct manifest_header header;
	
	/* First verify this is actually a manifest */
	type = oid_object_info(r, manifest_oid, &manifest_size);
	if (type != OBJ_MANIFEST)
		return NULL;
	
	/* Read the manifest to get its content */
	manifest_buffer = repo_read_object_file(r, manifest_oid, &type, &manifest_size);
	if (!manifest_buffer)
		return NULL;
	
	/* Parse the manifest header using shared function */
	if (parse_manifest_header(manifest_buffer, manifest_size, &header) < 0) {
		free(manifest_buffer);
		return NULL;
	}
	
	stream = xcalloc(1, sizeof(*stream));
	stream->repo = r;
	stream->manifest_buffer = manifest_buffer;
	stream->manifest_size = manifest_size;
	stream->total_size = header.total_size;
	
	/* Initialize descriptor to point at chunk OID data */
	init_manifest_desc(&stream->desc, header.chunk_data, header.chunk_data_len, r->hash_algo);
	
	if (size)
		*size = stream->total_size;
	
	stream->initialized = 1;
	return stream;
}

static int open_next_chunk(struct manifest_stream *stream)
{
	enum object_type type;
	unsigned long chunk_size;
	
	/* Close current chunk stream if open */
	if (stream->current_chunk_stream) {
		close_istream(stream->current_chunk_stream);
		stream->current_chunk_stream = NULL;
	}
	
	/* Get next chunk OID from manifest */
	if (!manifest_entry(&stream->desc)) {
		stream->at_end = 1;
		return 0; /* End of manifest */
	}
	
	oidcpy(&stream->current_chunk_oid, &stream->desc.entry_oid);
	
	/* Open stream for this chunk
	 * TODO: For checkout operations (Phase 4), we'll need to pass
	 * a filter here instead of NULL to handle CRLF conversion,
	 * clean/smudge filters, and ident expansion.
	 */
	stream->current_chunk_stream = open_istream(stream->repo,
	                                           &stream->current_chunk_oid,
	                                           &type, &chunk_size, NULL);
	if (!stream->current_chunk_stream)
		return -1;
	
	if (type != OBJ_BLOB) {
		close_istream(stream->current_chunk_stream);
		stream->current_chunk_stream = NULL;
		return -1;
	}
	
	return 0;
}

ssize_t read_manifest_stream(struct manifest_stream *stream, void *buf, size_t count)
{
	ssize_t total_read = 0;
	char *dest = buf;
	
	if (!stream || !stream->initialized)
		return -1;
	
	if (stream->at_end)
		return 0;
	
	while (count > 0) {
		ssize_t bytes_read;
		
		/* Open first/next chunk if needed */
		if (!stream->current_chunk_stream) {
			if (open_next_chunk(stream) < 0)
				return total_read > 0 ? total_read : -1;
			if (stream->at_end)
				return total_read;
		}
		
		/* Read from current chunk */
		bytes_read = read_istream(stream->current_chunk_stream, dest, count);
		
		if (bytes_read < 0)
			return total_read > 0 ? total_read : -1;
		
		if (bytes_read == 0) {
			/* Current chunk exhausted, try next one */
			if (open_next_chunk(stream) < 0)
				return total_read > 0 ? total_read : -1;
			if (stream->at_end)
				return total_read;
			continue;
		}
		
		dest += bytes_read;
		count -= bytes_read;
		total_read += bytes_read;
		stream->bytes_read += bytes_read;
	}
	
	return total_read;
}

int close_manifest_stream(struct manifest_stream *stream)
{
	if (!stream)
		return 0;
	
	if (stream->current_chunk_stream) {
		close_istream(stream->current_chunk_stream);
		stream->current_chunk_stream = NULL;
	}
	
	free(stream->manifest_buffer);
	free(stream);
	return 0;
}

int stream_manifest_to_fd(struct repository *r, int fd, const struct object_id *manifest_oid)
{
	struct manifest_stream *stream;
	unsigned long size;
	char buf[1024 * 16]; /* 16KB buffer like Git's streaming */
	ssize_t bytes_read;
	
	stream = open_manifest_stream(r, manifest_oid, &size);
	if (!stream)
		return -1;
	
	while ((bytes_read = read_manifest_stream(stream, buf, sizeof(buf))) > 0) {
		if (write_in_full(fd, buf, bytes_read) < 0) {
			close_manifest_stream(stream);
			return -1;
		}
	}
	
	close_manifest_stream(stream);
	return bytes_read < 0 ? -1 : 0;
}

int stream_manifest_to_fd_filtered(struct repository *r, int fd, 
                                   const struct object_id *manifest_oid,
                                   struct stream_filter *filter)
{
	struct manifest_stream *stream;
	unsigned long size;
	char ibuf[1024 * 16]; /* Input buffer */
	char obuf[1024 * 16]; /* Output buffer for filtered content */
	ssize_t bytes_read;
	int result = 0;
	
	stream = open_manifest_stream(r, manifest_oid, &size);
	if (!stream) {
		if (filter)
			free_stream_filter(filter);
		return -1;
	}
	
	if (!filter || is_null_stream_filter(filter)) {
		/* No filtering needed, use simple streaming */
		while ((bytes_read = read_manifest_stream(stream, ibuf, sizeof(ibuf))) > 0) {
			if (write_in_full(fd, ibuf, bytes_read) < 0) {
				result = -1;
				break;
			}
		}
		if (bytes_read < 0)
			result = -1;
	} else {
		/* Apply filters while streaming */
		while ((bytes_read = read_manifest_stream(stream, ibuf, sizeof(ibuf))) > 0) {
			size_t to_feed = bytes_read;
			size_t to_receive = sizeof(obuf);
			
			if (stream_filter(filter, ibuf, &to_feed, obuf, &to_receive)) {
				result = -1;
				break;
			}
			
			size_t filtered_size = sizeof(obuf) - to_receive;
			if (filtered_size > 0 && write_in_full(fd, obuf, filtered_size) < 0) {
				result = -1;
				break;
			}
			
			/* Handle any unconsumed input */
			if (to_feed > 0) {
				/* This shouldn't happen with our simple streaming,
				 * but if it does, we'd need more complex buffering */
				warning("manifest streaming: filter did not consume all input");
			}
		}
		
		if (bytes_read < 0)
			result = -1;
		
		/* Drain any remaining filtered output */
		if (result == 0) {
			size_t to_receive = sizeof(obuf);
			if (stream_filter(filter, NULL, NULL, obuf, &to_receive) == 0) {
				size_t final_size = sizeof(obuf) - to_receive;
				if (final_size > 0 && write_in_full(fd, obuf, final_size) < 0)
					result = -1;
			}
		}
		
		free_stream_filter(filter);
	}
	
	close_manifest_stream(stream);
	return result;
}

/*
 * Internal function to build manifest content based on the configured version.
 * The caller is responsible for freeing the strbuf.
 * Returns 0 on success, -1 on error.
 */
static int build_manifest_content(struct repository *r, struct strbuf *buf,
                                 unsigned long total_size,
                                 const struct object_id *content_oid,
                                 size_t chunk_count,
                                 const struct object_id *chunk_oids)
{
	int manifest_version;
	size_t i;

	/*
	 * Read manifest version from config. If not set, default to 1.
	 * This allows us to control which manifest format version to use
	 * when creating new manifests.
	 */
	if (repo_config_get_int(r, "bench.manifestversion", &manifest_version))
		manifest_version = 1; /* Default to version 1 if not configured */

	/*
	 * Currently we only support creating version 1 manifests.
	 * When we add support for new versions, add cases here.
	 */
	if (manifest_version != 1) {
		error("unsupported manifest version %d for creation (only version 1 is supported)",
		      manifest_version);
		return -1;
	}

	/* Build manifest based on version */
	switch (manifest_version) {
	case 1:
		/*
		 * Manifest format v1:
		 * Line 1: Version number ("1")
		 * Line 2: Total size of all chunks combined
		 * Line 3: Content OID (complete file hash after filters)
		 * Line 4: Number of chunks
		 * Line 5+: Chunk OIDs (one per line)
		 */
		strbuf_addf(buf, "%d\n", manifest_version);
		strbuf_addf(buf, "%lu\n", total_size);
		strbuf_addf(buf, "%s\n", oid_to_hex(content_oid));
		strbuf_addf(buf, "%zu\n", chunk_count);
		
		/* Add all chunk OIDs */
		for (i = 0; i < chunk_count; i++) {
			strbuf_addstr(buf, oid_to_hex(&chunk_oids[i]));
			strbuf_addch(buf, '\n');
		}
		break;

	/* Future versions would have their own cases here */

	default:
		/* This should not happen due to check above, but be defensive */
		error("BUG: unhandled manifest version %d", manifest_version);
		return -1;
	}

	return 0;
}

int write_manifest_object(struct repository *r, struct object_id *oid,
                         unsigned long total_size,
                         const struct object_id *content_oid,
                         size_t chunk_count,
                         const struct object_id *chunk_oids)
{
	struct strbuf buf = STRBUF_INIT;
	int ret;

	/* Build the manifest content */
	if (build_manifest_content(r, &buf, total_size, content_oid, chunk_count, chunk_oids) < 0) {
		strbuf_release(&buf);
		return -1;
	}

	/* Write manifest object */
	ret = write_object_file(buf.buf, buf.len, OBJ_MANIFEST, oid);
	strbuf_release(&buf);
	
	return ret < 0 ? -1 : 0;
}

int hash_manifest_object(struct repository *r, struct object_id *oid,
                        unsigned long total_size,
                        const struct object_id *content_oid,
                        size_t chunk_count,
                        const struct object_id *chunk_oids)
{
	struct strbuf buf = STRBUF_INIT;

	/* Build the manifest content */
	if (build_manifest_content(r, &buf, total_size, content_oid, chunk_count, chunk_oids) < 0) {
		strbuf_release(&buf);
		return -1;
	}

	/* Hash the manifest object without writing it */
	hash_object_file(r->hash_algo, buf.buf, buf.len, OBJ_MANIFEST, oid);
	strbuf_release(&buf);
	
	return 0;
}

int get_manifest_content_oid(struct repository *r,
                             const struct object_id *manifest_oid,
                             struct object_id *content_oid)
{
	struct manifest_header header;
	void *manifest_buffer;
	unsigned long manifest_size;
	enum object_type type;
	int ret = -1;
	
	/* Read manifest object */
	manifest_buffer = odb_read_object(r->objects, manifest_oid, &type, &manifest_size);
	if (!manifest_buffer || type != OBJ_MANIFEST)
		return -1;
	
	/* Parse header to get content OID */
	if (parse_manifest_header(manifest_buffer, manifest_size, &header) == 0) {
		oidcpy(content_oid, &header.content_oid);
		ret = 0;
	}
	
	free(manifest_buffer);
	return ret;
}

int get_manifest_chunk_oids(struct repository *r,
                           const struct object_id *manifest_oid,
                           unsigned long *total_size,
                           struct oid_array *chunk_oids)
{
	enum object_type type;
	unsigned long manifest_size;
	void *manifest_buffer;
	struct manifest_header header;
	struct manifest_desc desc;
	
	/* Verify this is actually a manifest */
	type = oid_object_info(r, manifest_oid, &manifest_size);
	if (type != OBJ_MANIFEST)
		return -1;
	
	/* Read the manifest content */
	manifest_buffer = repo_read_object_file(r, manifest_oid, &type, &manifest_size);
	if (!manifest_buffer)
		return -1;
	
	/* Parse the manifest header using shared function */
	if (parse_manifest_header(manifest_buffer, manifest_size, &header) < 0) {
		free(manifest_buffer);
		return -1;
	}
	
	/* Collect chunk OIDs if requested */
	if (chunk_oids) {
		/* Initialize descriptor to iterate through chunk OIDs */
		init_manifest_desc(&desc, header.chunk_data, header.chunk_data_len, r->hash_algo);
		
		oid_array_clear(chunk_oids);
		while (manifest_entry(&desc)) {
			oid_array_append(chunk_oids, &desc.entry_oid);
		}
	}
	
	if (total_size)
		*total_size = header.total_size;
	
	free(manifest_buffer);
	return 0;
}

void *read_manifest_content(struct repository *r,
                           const struct object_id *manifest_oid,
                           unsigned long *size)
{
	struct manifest_stream *stream;
	unsigned long manifest_size;
	void *content;
	ssize_t bytes_read, total_read = 0;
	
	stream = open_manifest_stream(r, manifest_oid, &manifest_size);
	if (!stream)
		return NULL;
	
	content = xmalloc(manifest_size);
	while (total_read < manifest_size) {
		bytes_read = read_manifest_stream(stream, 
		                                  (char *)content + total_read,
		                                  manifest_size - total_read);
		if (bytes_read <= 0)
			break;
		total_read += bytes_read;
	}
	
	close_manifest_stream(stream);
	
	if (total_read == manifest_size) {
		*size = manifest_size;
		return content;
	}
	
	free(content);
	return NULL;
}

int get_manifest_size(struct repository *r,
                     const struct object_id *manifest_oid,
                     unsigned long *size)
{
	enum object_type type;
	unsigned long manifest_size;
	void *manifest_buffer;
	struct manifest_header header;
	
	/* Read the manifest object */
	manifest_buffer = odb_read_object(r->objects, manifest_oid, &type, &manifest_size);
	if (!manifest_buffer || type != OBJ_MANIFEST)
		return -1;
	
	/* Parse the header to get the total size */
	if (parse_manifest_header(manifest_buffer, manifest_size, &header) < 0) {
		free(manifest_buffer);
		return -1;
	}
	
	*size = header.total_size;
	free(manifest_buffer);
	return 0;
}

/* Validation functions for fsck */

int validate_manifest_header(struct repository *r, const void *buffer, unsigned long size,
                           struct manifest_header *header,
                           struct strbuf *err)
{
	const char *p = buffer;
	const char *end = (const char *)buffer + size;
	
	/* Parse version */
	header->version = strtol(p, (char **)&p, 10);
	if (p >= end || *p++ != '\n') {
		strbuf_addstr(err, "manifest missing version number");
		return -1;
	}
	
	/* Version check: We currently only support version 1 */
	if (header->version < 1) {
		strbuf_addf(err, "manifest has invalid version %d", header->version);
		return -1;
	}
	if (header->version > 1) {
		strbuf_addf(err, "manifest version %d is newer than supported version 1", header->version);
		return -1;
	}
	
	/* Version 1 parsing */
	if (header->version == 1) {
		/* Parse total size - check for negative values first */
		if (*p == '-') {
			strbuf_addstr(err, "manifest has invalid negative size");
			return -1;
		}
		header->total_size = strtoul(p, (char **)&p, 10);
		if (p >= end || *p++ != '\n') {
			strbuf_addstr(err, "manifest missing total size");
			return -1;
		}
		
		/* Parse content OID (complete file hash after filters) */
		int algo_idx = get_oid_hex_any(p, &header->content_oid);
		if (algo_idx == GIT_HASH_UNKNOWN) {
			strbuf_addstr(err, "manifest missing or invalid content OID");
			return -1;
		}
		p += hash_algos[algo_idx].hexsz;
		if (p >= end || *p++ != '\n') {
			strbuf_addstr(err, "manifest content OID not followed by newline");
			return -1;
		}
		
		/* Parse chunk count */
		header->chunk_count = strtoul(p, (char **)&p, 10);
		if (p >= end || *p++ != '\n') {
			strbuf_addstr(err, "manifest missing chunk count");
			return -1;
		}
		
		/* Validate chunk count is reasonable */
		if (header->chunk_count == 0) {
			strbuf_addstr(err, "manifest has zero chunks");
			return -1;
		}
	}
	
	/* Set pointer to chunk OID data */
	header->chunk_data = p;
	header->chunk_data_len = end - p;
	
	/* Validate chunk data length matches expected chunk count */
	size_t expected_chunk_data_len = header->chunk_count * (r->hash_algo->hexsz + 1);  /* +1 for newlines */
	if (header->chunk_data_len < expected_chunk_data_len - 1) {  /* -1 for last newline */
		strbuf_addf(err, "manifest chunk data too short (expected %zu, got %zu)", 
		           expected_chunk_data_len - 1, header->chunk_data_len);
		return -1;
	}
	
	return 0;
}

int validate_manifest_chunks(struct repository *r,
                           const struct object_id *chunk_oids,
                           size_t chunk_count,
                           struct strbuf *err)
{
	size_t i;
	
	for (i = 0; i < chunk_count; i++) {
		enum object_type type;
		
		/* Check if chunk object exists */
		type = oid_object_info(r, &chunk_oids[i], NULL);
		if (type == OBJ_BAD) {
			strbuf_addf(err, "chunk %zu object %s not found", 
			           i, oid_to_hex(&chunk_oids[i]));
			return -1;
		}
		
		/* Verify chunk is a blob */
		if (type != OBJ_BLOB) {
			strbuf_addf(err, "chunk %zu object %s is %s, expected blob",
			           i, oid_to_hex(&chunk_oids[i]), type_name(type));
			return -1;
		}
	}
	
	return 0;
}

int verify_manifest_integrity(struct repository *r,
                            const struct manifest_header *header,
                            const struct object_id *chunk_oids,
                            struct strbuf *err)
{
	size_t i;
	unsigned long total_chunk_size = 0;
	struct git_hash_ctx ctx;
	struct object_id computed_oid;
	
	/* Initialize computed_oid structure */
	oidclr(&computed_oid, r->hash_algo);
	
	/* Initialize hash context for content OID computation */
	r->hash_algo->init_fn(&ctx);
	
	/* Read each chunk and add complete blob object (with header) to hash */
	for (i = 0; i < header->chunk_count; i++) {
		enum object_type type;
		unsigned long chunk_size;
		void *chunk_data;
		struct strbuf blob_object = STRBUF_INIT;
		
		/* Read chunk data */
		chunk_data = odb_read_object(r->objects, &chunk_oids[i], &type, &chunk_size);
		if (!chunk_data) {
			strbuf_addf(err, "failed to read chunk %zu object %s", 
			           i, oid_to_hex(&chunk_oids[i]));
			return -1;
		}
		
		if (type != OBJ_BLOB) {
			strbuf_addf(err, "chunk %zu object %s is not a blob", 
			           i, oid_to_hex(&chunk_oids[i]));
			free(chunk_data);
			return -1;
		}
		
		/* 
		 * For content OID computation, we need to hash the complete blob objects.
		 * Rather than manually reconstructing "blob N\0<content>", we can 
		 * leverage Git's existing hash_object_file() function.
		 */
		struct object_id chunk_as_blob_oid;
		hash_object_file(r->hash_algo, chunk_data, chunk_size, OBJ_BLOB, &chunk_as_blob_oid);
		
		/* Verify this matches the expected chunk OID */
		if (!oideq(&chunk_oids[i], &chunk_as_blob_oid)) {
			strbuf_addf(err, "chunk %zu OID mismatch: expected %s, computed %s", 
			           i, oid_to_hex(&chunk_oids[i]), oid_to_hex(&chunk_as_blob_oid));
			free(chunk_data);
			return -1;
		}
		
		/* Add the blob object's serialized form to content hash
		 * Format: "blob <size>\0<content>" */
		strbuf_addf(&blob_object, "blob %lu", chunk_size);
		strbuf_addch(&blob_object, '\0');
		strbuf_add(&blob_object, chunk_data, chunk_size);
		
		git_hash_update(&ctx, blob_object.buf, blob_object.len);
		total_chunk_size += chunk_size;
		
		strbuf_release(&blob_object);
		free(chunk_data);
	}
	
	/* Finalize hash computation */
	git_hash_final(computed_oid.hash, &ctx);
	
	/* Verify total size matches header */
	if (total_chunk_size != header->total_size) {
		strbuf_addf(err, "manifest total size mismatch: header says %lu, chunks sum to %lu",
		           header->total_size, total_chunk_size);
		return -1;
	}
	
	/* Verify content OID matches computed hash */
	if (!oideq(&computed_oid, &header->content_oid)) {
		strbuf_addf(err, "manifest content OID mismatch: header says %s, computed %s",
		           oid_to_hex(&header->content_oid), oid_to_hex(&computed_oid));
		return -1;
	}
	
	return 0;
}