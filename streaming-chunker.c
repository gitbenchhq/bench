#define USE_THE_REPOSITORY_VARIABLE

#include "git-compat-util.h"
#include "streaming-chunker.h"
#include "bulk-checkin.h"
#include "object-file.h"
#include "odb.h"
#include "strbuf.h"
#include "manifest.h"
#include "config.h"
#include "repository.h"
#include "hex.h"
#include "repo-settings.h"

int should_chunk_file(off_t file_size)
{
	/*
	 * NOTE: This function assumes we're already in bench mode.
	 * The caller (index_path) should check repo_has_bench_extensions()
	 * before calling this function.
	 */

	/*
	 * Files up to chunk.minSize are stored as one chunk; larger files
	 * are chunked. Reading the checked config here means an invalid
	 * chunk configuration stops every add, before any file is read,
	 * not only adds of files large enough to chunk.
	 */
	struct chunk_config cfg;

	chunk_config_from_repo(the_repository, &cfg);
	return file_size > 0 && (size_t)file_size > cfg.min_size;
}

/*
 * Common initialization logic for both file and buffer modes
 */
static int streaming_chunker_init_common(struct streaming_chunker *sc, size_t data_size)
{
	/* chunk.minSize / targetSize / maxSize, checked (chunker.h) */
	chunk_config_from_repo(the_repository, &sc->chunk_config);

	/* Initialize arrays */
	sc->chunk_capacity = 16;  /* Start small, grow as needed */
	sc->chunk_oids = xcalloc(sc->chunk_capacity, sizeof(struct object_id));
	sc->chunk_sizes = xcalloc(sc->chunk_capacity, sizeof(uint64_t));
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
	sc->expected_size = data_size;
	strbuf_addf(&header, "blob %zu", data_size);
	strbuf_addch(&header, '\0');

	the_hash_algo->init_fn(&sc->content_hash_ctx);
	the_hash_algo->update_fn(&sc->content_hash_ctx, header.buf, header.len);
	sc->content_hash_initialized = 1;

	strbuf_release(&header);

	sc->total_size = 0;
	sc->cdc = (struct cdc_state)CDC_STATE_INIT;
	sc->read_size = chunker_read_size(STREAMING_BUFFER_SIZE);
	sc->error_occurred = 0;
	sc->write_objects = 1;

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
	chunker_stamp_file(fd, &sc->stamp);

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
 * Finalize the current chunk: write to object database
 */
static int finalize_current_chunk(struct streaming_chunker *sc)
{
	struct object_id chunk_oid;

	if (sc->chunk_buffer.len == 0)
		return 0;  /* Empty chunk, nothing to do */

	/* Stop early if this file's manifest would not fit in memory. */
	if (manifest_check_memory(the_repository, sc->chunk_count + 1,
				  &sc->error_message) < 0) {
		sc->error_occurred = 1;
		return -1;
	}

	/*
	 * Write the chunk as a blob, packed with the others by bulk checkin,
	 * or, when only computing ids (add --dry-run, diff), just hash it.
	 */
	if (!sc->write_objects) {
		hash_object_file(the_hash_algo, sc->chunk_buffer.buf,
				 sc->chunk_buffer.len, OBJ_BLOB, &chunk_oid);
	} else if (index_buffer_bulk_checkin(&chunk_oid, sc->chunk_buffer.buf,
					     sc->chunk_buffer.len,
					     INDEX_WRITE_OBJECT) < 0) {
		strbuf_addf(&sc->error_message, "failed to write chunk %zu to ODB",
		            sc->chunk_count);
		sc->error_occurred = 1;
		return -1;
	}

	/* Grow array if needed */
	if (sc->chunk_count >= sc->chunk_capacity) {
		sc->chunk_capacity *= 2;
		REALLOC_ARRAY(sc->chunk_oids, sc->chunk_capacity);
		REALLOC_ARRAY(sc->chunk_sizes, sc->chunk_capacity);
	}

	oidcpy(&sc->chunk_oids[sc->chunk_count], &chunk_oid);
	sc->chunk_sizes[sc->chunk_count] = sc->chunk_buffer.len;
	sc->chunk_count++;

	/* Clear buffer for the next chunk (cdc_feed resets its own state) */
	strbuf_reset(&sc->chunk_buffer);

	return 0;
}

int streaming_chunker_process_file(struct streaming_chunker *sc)
{
	/*
	 * Streaming chunker main loop.
	 *
	 * Algorithm:
	 *   1. Read the input in reads of read_size bytes (64 KB)
	 *   2. Update the content hash (for the entire file)
	 *   3. Split each read into chunks with cdc_feed() (chunker.h), which
	 *      holds the cut rule: skip the first min_size bytes, cut on the
	 *      gear hash, or at exactly max_size. Bytes after a cut start the
	 *      next chunk and are scanned like any others.
	 *   4. At EOF: finalize the remaining chunk
	 *
	 * Two hashes are computed:
	 *   - Content hash: hash("blob <total_size>\0" + entire_file_data)
	 *                   Computed incrementally for the entire file.
	 *   - Chunk hashes: hash("blob <chunk_size>\0" + chunk_data) per chunk
	 *                   Computed in finalize_current_chunk().
	 */

	ssize_t bytes_read;
	size_t pos;

	while (1) {
		/* Read next page from file or buffer */
		if (sc->fd >= 0) {
			/* File mode: read from file descriptor */
			bytes_read = xread(sc->fd, sc->read_buffer, sc->read_size);
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
				bytes_read = remaining < sc->read_size
				           ? remaining
				           : sc->read_size;
				memcpy(sc->read_buffer, sc->buffer + sc->buffer_pos, bytes_read);
				sc->buffer_pos += bytes_read;
			}
		}

		if (bytes_read == 0)
			break;  /* EOF */

		/* Update content hash with this page */
		if (sc->content_hash_initialized) {
			the_hash_algo->update_fn(&sc->content_hash_ctx,
			                         sc->read_buffer, bytes_read);
		}

		sc->total_size += bytes_read;

		/*
		 * Split this read into chunks. cdc_feed() says how many bytes
		 * belong to the current chunk and whether it ends there; the
		 * rest of the read is fed again, so a cut in the middle of a
		 * read (content-defined or at exactly max_size) starts the next
		 * chunk with the remaining bytes, which are scanned as usual.
		 */
		pos = 0;
		while (pos < (size_t)bytes_read) {
			int cut;
			size_t take = cdc_feed(&sc->chunk_config, &sc->cdc,
					       sc->read_buffer + pos,
					       bytes_read - pos, &cut);

			strbuf_add(&sc->chunk_buffer, sc->read_buffer + pos, take);
			pos += take;
			if (cut && finalize_current_chunk(sc) < 0)
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
	if (sc->error_occurred)
		return -1;

	/*
	 * Chunks were already written synchronously in finalize_current_chunk().
	 * Now we just need to finalize the content OID and create the manifest.
	 */

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

	/*
	 * content_oid's "blob <size>" header was hashed up front with the
	 * size the file had when we opened it. If the file grew or shrank
	 * while we read it, that header is wrong, so refuse (as git does).
	 */
	if (sc->fd >= 0 && chunker_file_changed(sc->fd, &sc->stamp,
						 &sc->error_message)) {
		sc->error_occurred = 1;
		return -1;
	}
	if (sc->total_size != sc->expected_size) {
		strbuf_addf(&sc->error_message,
			    "file changed while reading (expected %"PRIu64" bytes, read %"PRIu64")",
			    sc->expected_size, sc->total_size);
		sc->error_occurred = 1;
		return -1;
	}

	/* Create the manifest object, or only compute its id */
	if ((sc->write_objects ? write_manifest_object : hash_manifest_object)(
		    the_repository, manifest_oid, sc->total_size, content_oid,
		    sc->chunk_count, sc->chunk_oids, sc->chunk_sizes) < 0) {
		strbuf_addstr(&sc->error_message, "failed to create manifest object");
		sc->error_occurred = 1;
		return -1;
	}

	return 0;
}

void streaming_chunker_cleanup(struct streaming_chunker *sc)
{
	/* Free arrays */
	free(sc->chunk_oids);
	free(sc->chunk_sizes);

	/* Release strbufs */
	strbuf_release(&sc->chunk_buffer);
	strbuf_release(&sc->error_message);

	memset(sc, 0, sizeof(*sc));
}
