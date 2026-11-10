#include "git-compat-util.h"
#include "manifest.h"
#include "object-file.h"
#include "repository.h"
#include "repo-settings.h"
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
#include "gettext.h"

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
	uint64_t total_size;
	uint64_t bytes_read;
	int initialized;
	int at_end;
};

__attribute__((format (printf, 3, 4)))
static enum manifest_error fail(struct strbuf *err, enum manifest_error code,
				const char *fmt, ...)
{
	if (err) {
		va_list ap;
		va_start(ap, fmt);
		strbuf_vaddf(err, fmt, ap);
		va_end(ap);
	}
	return code;
}

/* Parse one canonical decimal number that must be followed by '\n'. */
static int parse_number_line(const char **p, const char *end, uint64_t *value)
{
	const char *q;

	if (manifest_parse_u64(*p, end, value, &q) < 0 || q >= end || *q != '\n')
		return -1;
	*p = q + 1;
	return 0;
}

enum manifest_error parse_manifest(const struct git_hash_algo *algo,
				   const void *buffer, unsigned long size,
				   unsigned flags,
				   struct manifest_header *header,
				   struct strbuf *err)
{
	const char *p = buffer;
	const char *end = p + size;
	struct manifest_desc desc;
	uint64_t version, chunk_count, sum = 0;
	size_t lines = 0;
	int ret;

	if (parse_number_line(&p, end, &version) < 0)
		return fail(err, MANIFEST_ERR_BAD_VERSION,
			    "missing or non-canonical version line");
	if (version != MANIFEST_VERSION)
		return fail(err, MANIFEST_ERR_UNKNOWN_VERSION,
			    "manifest version %"PRIu64" is not supported (this bench reads version %d)",
			    version, MANIFEST_VERSION);
	header->version = MANIFEST_VERSION;

	if (parse_number_line(&p, end, &header->total_size) < 0)
		return fail(err, MANIFEST_ERR_BAD_TOTAL_SIZE,
			    "missing or non-canonical total_size");

	if (manifest_parse_oid(p, end, algo, &header->content_oid, &p) < 0 ||
	    p >= end || *p++ != '\n')
		return fail(err, MANIFEST_ERR_BAD_CONTENT_OID,
			    "content_oid is not %"PRIuMAX" lowercase hex digits and a newline",
			    (uintmax_t)algo->hexsz);

	if (parse_number_line(&p, end, &chunk_count) < 0)
		return fail(err, MANIFEST_ERR_BAD_CHUNK_COUNT,
			    "missing or non-canonical chunk_count");
	if (!chunk_count)
		return fail(err, MANIFEST_ERR_BAD_CHUNK_COUNT, "chunk_count is 0");
	/*
	 * No cap: walking the lines below is bounded by the bytes present,
	 * so a huge chunk_count only produces a count mismatch.
	 */
	if (chunk_count > SIZE_MAX)
		return fail(err, MANIFEST_ERR_CHUNK_COUNT_MISMATCH,
			    "chunk_count %"PRIu64" is more than the chunk lines can hold",
			    chunk_count);
	header->chunk_count = chunk_count;

	header->chunk_data = p;
	header->chunk_data_len = end - p;

	if (flags & MANIFEST_PARSE_HEADER_ONLY)
		return MANIFEST_OK;

	/*
	 * Walk exactly chunk_count lines. Fewer lines, more lines and
	 * trailing bytes are all a count mismatch; a line that is not
	 * "<oid> <size>\n" is a bad chunk line.
	 */
	init_manifest_desc(&desc, header->chunk_data, header->chunk_data_len, algo);
	while (lines < header->chunk_count) {
		ret = manifest_entry(&desc);
		if (ret < 0)
			return fail(err, MANIFEST_ERR_BAD_CHUNK_LINE,
				    "chunk line %"PRIuMAX" is not '<oid> <size>'",
				    (uintmax_t)lines + 1);
		if (!ret)
			break;
		if (desc.entry_size > UINT64_MAX - sum)
			return fail(err, MANIFEST_ERR_SIZE_MISMATCH,
				    "chunk sizes overflow");
		sum += desc.entry_size;
		lines++;
	}
	if (lines < header->chunk_count)
		return fail(err, MANIFEST_ERR_CHUNK_COUNT_MISMATCH,
			    "chunk_count is %"PRIuMAX" but there are %"PRIuMAX" chunk lines",
			    (uintmax_t)header->chunk_count, (uintmax_t)lines);
	if (desc.size)
		return fail(err, MANIFEST_ERR_CHUNK_COUNT_MISMATCH,
			    "data after chunk line %"PRIuMAX" (chunk_count is %"PRIuMAX")",
			    (uintmax_t)lines, (uintmax_t)header->chunk_count);
	if (sum != header->total_size)
		return fail(err, MANIFEST_ERR_SIZE_MISMATCH,
			    "chunk sizes sum to %"PRIu64" but total_size is %"PRIu64,
			    sum, header->total_size);

	return MANIFEST_OK;
}

/* Parse a manifest read from the object store, reporting any error. */
static int parse_manifest_object(struct repository *r,
				 const struct object_id *oid,
				 const void *buffer, unsigned long size,
				 unsigned flags,
				 struct manifest_header *header)
{
	struct strbuf err = STRBUF_INIT;
	int ret = 0;

	if (parse_manifest(r->hash_algo, buffer, size, flags, header, &err) != MANIFEST_OK)
		ret = error("manifest %s: %s", oid_to_hex(oid), err.buf);
	strbuf_release(&err);
	return ret;
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
	if (parse_manifest_object(r, manifest_oid, manifest_buffer, manifest_size,
				  0, &header) < 0) {
		free(manifest_buffer);
		return NULL;
	}
	
	stream = xcalloc(1, sizeof(*stream));
	stream->repo = r;
	stream->manifest_buffer = manifest_buffer;
	stream->manifest_size = manifest_size;
	stream->total_size = header.total_size;
	
	/* Initialize descriptor to point at the chunk lines */
	init_manifest_desc(&stream->desc, header.chunk_data, header.chunk_data_len, r->hash_algo);
	
	if (size) {
		if (stream->total_size > ULONG_MAX) {
			error("manifest %s: file size %"PRIu64" does not fit in unsigned long",
			      oid_to_hex(manifest_oid), stream->total_size);
			close_manifest_stream(stream);
			return NULL;
		}
		*size = (unsigned long)stream->total_size;
	}
	
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
	
	/* Get next chunk line from manifest */
	switch (manifest_entry(&stream->desc)) {
	case 0:
		stream->at_end = 1;
		return 0; /* End of manifest */
	case 1:
		break;
	default:
		return error("malformed chunk line in manifest");
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

	/* The manifest's recorded size must be the chunk's real size. */
	if ((uint64_t)chunk_size != stream->desc.entry_size) {
		close_istream(stream->current_chunk_stream);
		stream->current_chunk_stream = NULL;
		return error("chunk %s is %lu bytes, manifest says %"PRIu64,
			     oid_to_hex(&stream->current_chunk_oid), chunk_size,
			     stream->desc.entry_size);
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
		/*
		 * Apply filters while streaming.
		 *
		 * The filter may not consume all input in one call (it might buffer
		 * for look-ahead). We need to keep feeding unconsumed input along with
		 * new input until everything is consumed.
		 */
		size_t unconsumed = 0;  /* Bytes not yet consumed by filter */

		while ((bytes_read = read_manifest_stream(stream,
		                                          ibuf + unconsumed,
		                                          sizeof(ibuf) - unconsumed)) > 0) {
			size_t total_available = unconsumed + bytes_read;
			const char *input_ptr = ibuf;

			/* Keep filtering until all input is consumed or output buffer fills */
			while (total_available > 0) {
				size_t to_feed = total_available;
				size_t to_receive = sizeof(obuf);

				if (stream_filter(filter, input_ptr, &to_feed, obuf, &to_receive)) {
					result = -1;
					goto cleanup_filter;
				}

				/* Write any filtered output */
				size_t filtered_size = sizeof(obuf) - to_receive;
				if (filtered_size > 0) {
					if (write_in_full(fd, obuf, filtered_size) < 0) {
						result = -1;
						goto cleanup_filter;
					}
				}

				/* Update for consumed input */
				size_t consumed = total_available - to_feed;
				input_ptr += consumed;
				total_available = to_feed;

				/* If filter didn't consume anything and didn't produce output,
				 * we need more input - break inner loop to read more data */
				if (consumed == 0 && filtered_size == 0)
					break;
			}

			/* Move unconsumed data to start of buffer for next iteration */
			if (total_available > 0 && total_available < sizeof(ibuf)) {
				memmove(ibuf, input_ptr, total_available);
				unconsumed = total_available;
			} else if (total_available >= sizeof(ibuf)) {
				/* Buffer full with unconsumed data - should not happen with 16KB buffer */
				warning("manifest streaming: input buffer exhausted, filter needs too much buffering");
				result = -1;
				goto cleanup_filter;
			} else {
				unconsumed = 0;
			}
		}

		if (bytes_read < 0) {
			result = -1;
			goto cleanup_filter;
		}

		/* Process any remaining unconsumed data */
		if (unconsumed > 0 && result == 0) {
			const char *input_ptr = ibuf;
			size_t remaining = unconsumed;

			while (remaining > 0) {
				size_t to_feed = remaining;
				size_t to_receive = sizeof(obuf);

				if (stream_filter(filter, input_ptr, &to_feed, obuf, &to_receive)) {
					result = -1;
					goto cleanup_filter;
				}

				size_t filtered_size = sizeof(obuf) - to_receive;
				if (filtered_size > 0) {
					if (write_in_full(fd, obuf, filtered_size) < 0) {
						result = -1;
						goto cleanup_filter;
					}
				}

				size_t consumed = remaining - to_feed;
				input_ptr += consumed;
				remaining = to_feed;

				if (consumed == 0 && filtered_size == 0)
					break;  /* Can't make progress */
			}
		}

		/* Drain any remaining filtered output (finalize filter) */
		if (result == 0) {
			size_t to_receive = sizeof(obuf);
			if (stream_filter(filter, NULL, NULL, obuf, &to_receive) == 0) {
				size_t final_size = sizeof(obuf) - to_receive;
				if (final_size > 0 && write_in_full(fd, obuf, final_size) < 0)
					result = -1;
			}
		}

	cleanup_filter:
		free_stream_filter(filter);
	}
	
	close_manifest_stream(stream);
	return result;
}

int manifest_check_memory(struct repository *r, size_t chunk_count,
			  struct strbuf *err)
{
	uint64_t limit = repo_settings_get_manifest_memory_limit(r);
	uint64_t need = (uint64_t)chunk_count * MANIFEST_MEMORY_PER_CHUNK;

	if (need <= limit)
		return 0;
	strbuf_addstr(err, _("this file's manifest needs more than "));
	strbuf_humanise_bytes(err, need);
	strbuf_addstr(err, _(" of memory (bench.manifestMemoryLimit is "));
	strbuf_humanise_bytes(err, limit);
	strbuf_addstr(err, _("); raise bench.manifestMemoryLimit, or use larger "
			     "chunks (chunk.minSize, chunk.targetSize)"));
	return -1;
}

/*
 * Build the release-format (version 1) manifest bytes. The format is fixed
 * here, not chosen by configuration: a per-clone setting would let two
 * people write different bytes, and so different object ids, for the same
 * chunks. The caller is responsible for freeing the strbuf.
 * Returns 0 on success, -1 on error.
 */
static int build_manifest_content(struct strbuf *buf,
                                 uint64_t total_size,
                                 const struct object_id *content_oid,
                                 size_t chunk_count,
                                 const struct object_id *chunk_oids,
                                 const uint64_t *chunk_sizes)
{
	uint64_t sum = 0;
	size_t i;

	if (!chunk_count)
		return error("BUG: manifest with zero chunks");
	for (i = 0; i < chunk_count; i++) {
		if (chunk_sizes[i] > UINT64_MAX - sum)
			return error("BUG: manifest chunk sizes overflow");
		sum += chunk_sizes[i];
	}
	if (sum != total_size)
		return error("BUG: manifest total size %"PRIu64" != sum of chunk sizes %"PRIu64,
			     total_size, sum);

	strbuf_addf(buf, "%d\n", MANIFEST_VERSION);
	strbuf_addf(buf, "%"PRIu64"\n", total_size);
	strbuf_addf(buf, "%s\n", oid_to_hex(content_oid));
	strbuf_addf(buf, "%"PRIuMAX"\n", (uintmax_t)chunk_count);
	for (i = 0; i < chunk_count; i++)
		strbuf_addf(buf, "%s %"PRIu64"\n",
			    oid_to_hex(&chunk_oids[i]), chunk_sizes[i]);

	return 0;
}

int write_manifest_object(struct repository *r UNUSED, struct object_id *oid,
                         uint64_t total_size,
                         const struct object_id *content_oid,
                         size_t chunk_count,
                         const struct object_id *chunk_oids,
                         const uint64_t *chunk_sizes)
{
	struct strbuf buf = STRBUF_INIT;
	int ret;

	/* Build the manifest content */
	if (build_manifest_content(&buf, total_size, content_oid, chunk_count,
				   chunk_oids, chunk_sizes) < 0) {
		strbuf_release(&buf);
		return -1;
	}

	/* Write manifest object */
	ret = write_object_file(buf.buf, buf.len, OBJ_MANIFEST, oid);
	strbuf_release(&buf);
	
	return ret < 0 ? -1 : 0;
}

int hash_manifest_object(struct repository *r, struct object_id *oid,
                        uint64_t total_size,
                        const struct object_id *content_oid,
                        size_t chunk_count,
                        const struct object_id *chunk_oids,
                        const uint64_t *chunk_sizes)
{
	struct strbuf buf = STRBUF_INIT;

	/* Build the manifest content */
	if (build_manifest_content(&buf, total_size, content_oid, chunk_count,
				   chunk_oids, chunk_sizes) < 0) {
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
	if (parse_manifest_object(r, manifest_oid, manifest_buffer, manifest_size,
				  MANIFEST_PARSE_HEADER_ONLY, &header) == 0) {
		oidcpy(content_oid, &header.content_oid);
		ret = 0;
	}
	
	free(manifest_buffer);
	return ret;
}

int get_manifest_chunk_oids(struct repository *r,
                           const struct object_id *manifest_oid,
                           uint64_t *total_size,
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
	if (parse_manifest_object(r, manifest_oid, manifest_buffer, manifest_size,
				  0, &header) < 0) {
		free(manifest_buffer);
		return -1;
	}
	
	/* Collect chunk OIDs if requested */
	if (chunk_oids) {
		/* Initialize descriptor to iterate through chunk OIDs */
		init_manifest_desc(&desc, header.chunk_data, header.chunk_data_len, r->hash_algo);

		/* parse_manifest_object() already checked every line */
		oid_array_clear(chunk_oids);
		while (manifest_entry(&desc) > 0)
			oid_array_append(chunk_oids, &desc.entry_oid);
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
	
	/*
	 * NUL-terminated, like every git object buffer: callers such as
	 * symlink checkout pass it to functions that expect a C string.
	 */
	content = xmallocz(manifest_size);
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
                     uint64_t *size)
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
	if (parse_manifest_object(r, manifest_oid, manifest_buffer, manifest_size,
				  MANIFEST_PARSE_HEADER_ONLY, &header) < 0) {
		free(manifest_buffer);
		return -1;
	}
	
	*size = header.total_size;
	free(manifest_buffer);
	return 0;
}

/* Validation for fsck */

/* "blob <uint64>\0" fits easily (object-file.c uses 32 for MAX_HEADER_LEN). */
#define MAX_MANIFEST_HDR 32

/*
 * Hash one chunk by streaming it in small reads, never loading it whole:
 * its blob id into *chunk_oid, and its bytes into content_ctx.
 */
static int stream_chunk_hash(struct repository *r, const struct object_id *oid,
			     unsigned long size, struct git_hash_ctx *content_ctx,
			     struct object_id *chunk_oid, char *buf, size_t bufsz)
{
	struct git_istream *st;
	struct git_hash_ctx chunk_ctx;
	enum object_type type;
	unsigned long st_size;
	uint64_t seen = 0;
	char hdr[MAX_MANIFEST_HDR];
	int hdrlen, ret = 0;

	st = open_istream(r, oid, &type, &st_size, NULL);
	if (!st)
		return -1;

	hdrlen = format_object_header(hdr, sizeof(hdr), OBJ_BLOB, size);
	r->hash_algo->init_fn(&chunk_ctx);
	git_hash_update(&chunk_ctx, hdr, hdrlen);
	for (;;) {
		ssize_t n = read_istream(st, buf, bufsz);
		if (n < 0) {
			ret = -1;
			break;
		}
		if (!n)
			break;
		git_hash_update(&chunk_ctx, buf, n);
		git_hash_update(content_ctx, buf, n);
		seen += n;
	}
	close_istream(st);
	git_hash_final_oid(chunk_oid, &chunk_ctx);
	if (!ret && seen != size)
		ret = -1;
	return ret;
}

enum manifest_error check_manifest_chunks(struct repository *r,
					  const struct manifest_header *header,
					  int full, struct strbuf *err)
{
	struct manifest_desc desc;
	struct git_hash_ctx content_ctx;
	struct object_id computed;
	enum manifest_error ret = MANIFEST_OK;
	const size_t bufsz = 64 * 1024;
	char *buf = NULL;
	int all_local = 1;
	size_t i = 0;

	if (full) {
		/* content_oid is the git blob id of the whole file */
		char hdr[MAX_MANIFEST_HDR];
		int hdrlen = format_object_header(hdr, sizeof(hdr), OBJ_BLOB,
						  header->total_size);

		r->hash_algo->init_fn(&content_ctx);
		git_hash_update(&content_ctx, hdr, hdrlen);
		buf = xmalloc(bufsz);
	}

	init_manifest_desc(&desc, header->chunk_data, header->chunk_data_len,
			   r->hash_algo);
	while (!ret && manifest_entry(&desc) > 0) {
		struct object_info oi = OBJECT_INFO_INIT;
		struct object_id chunk_oid;
		enum object_type type;
		unsigned long size;

		i++;
		/* Type and size only: no chunk data is read here. */
		oi.typep = &type;
		oi.sizep = &size;
		if (odb_read_object_info_extended(r->objects, &desc.entry_oid, &oi,
						  OBJECT_INFO_SKIP_FETCH_OBJECT) < 0) {
			all_local = 0;
			continue;
		}
		if (type != OBJ_BLOB)
			ret = fail(err, MANIFEST_ERR_CHUNK_WRONG_TYPE,
				   "chunk %"PRIuMAX" (%s) is a %s, not a blob",
				   (uintmax_t)i, oid_to_hex(&desc.entry_oid),
				   type_name(type));
		else if ((uint64_t)size != desc.entry_size)
			ret = fail(err, MANIFEST_ERR_CHUNK_SIZE_MISMATCH,
				   "chunk %"PRIuMAX" (%s) is %lu bytes, manifest says %"PRIu64,
				   (uintmax_t)i, oid_to_hex(&desc.entry_oid),
				   size, desc.entry_size);
		else if (full) {
			if (stream_chunk_hash(r, &desc.entry_oid, size, &content_ctx,
					      &chunk_oid, buf, bufsz) < 0)
				ret = fail(err, MANIFEST_ERR_CHUNK_CORRUPT,
					   "chunk %"PRIuMAX" (%s) could not be read",
					   (uintmax_t)i, oid_to_hex(&desc.entry_oid));
			else if (!oideq(&chunk_oid, &desc.entry_oid))
				ret = fail(err, MANIFEST_ERR_CHUNK_CORRUPT,
					   "chunk %"PRIuMAX" (%s) content hashes to %s",
					   (uintmax_t)i, oid_to_hex(&desc.entry_oid),
					   oid_to_hex(&chunk_oid));
		}
	}

	if (full) {
		/* Always finalize, so the hash context is released. */
		git_hash_final_oid(&computed, &content_ctx);
		if (!ret && all_local && !oideq(&computed, &header->content_oid))
			ret = fail(err, MANIFEST_ERR_CONTENT_MISMATCH,
				   "content_oid is %s, but the file's blob id is %s",
				   oid_to_hex(&header->content_oid),
				   oid_to_hex(&computed));
		free(buf);
	}
	return ret;
}
