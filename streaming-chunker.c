#define USE_THE_REPOSITORY_VARIABLE

#include "git-compat-util.h"
#include "streaming-chunker.h"
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
	 * Use chunk.minSize as the threshold for when to start chunking.
	 * Files smaller than or equal to this will be stored as single-chunk manifests.
	 * Files larger than this will be chunked using content-defined chunking.
	 * Default is 2MB (see repo-settings.c).
	 */
	unsigned long threshold = repo_settings_get_chunk_min_size(the_repository);

	return file_size > threshold;
}

int streaming_chunker_init(struct streaming_chunker *sc, int fd, off_t file_size)
{
	memset(sc, 0, sizeof(*sc));

	sc->fd = fd;

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
	 * Initialize content hash for the entire file.
	 * This computes: hash("blob <file_size>\0" + entire_file_data)
	 * This represents what Git would have computed if it hashed the file as one blob.
	 */
	struct strbuf header = STRBUF_INIT;
	strbuf_addf(&header, "blob %"PRIuMAX, (uintmax_t)file_size);
	strbuf_addch(&header, '\0');

	the_hash_algo->init_fn(&sc->content_hash_ctx);
	the_hash_algo->update_fn(&sc->content_hash_ctx, header.buf, header.len);
	sc->content_hash_initialized = 1;

	strbuf_release(&header);

	sc->total_size = 0;
	sc->current_chunk_size = 0;
	sc->fingerprint = 0;
	sc->error_occurred = 0;

	return 0;
}

/*
 * Finalize the current chunk: write to ODB and save OID
 */
static int finalize_current_chunk(struct streaming_chunker *sc)
{
	struct object_id chunk_oid;

	if (sc->chunk_buffer.len == 0)
		return 0;  /* Empty chunk, nothing to do */

	/* Write chunk as a blob object */
	if (write_object_file_flags(sc->chunk_buffer.buf, sc->chunk_buffer.len,
	                            OBJ_BLOB, &chunk_oid, NULL,
	                            WRITE_OBJECT_FILE_PERSIST) < 0) {
		strbuf_addf(&sc->error_message, "failed to write chunk %zu to ODB",
		            sc->chunk_count);
		sc->error_occurred = 1;
		return -1;
	}

	/* Grow array if needed */
	if (sc->chunk_count >= sc->chunk_capacity) {
		sc->chunk_capacity *= 2;
		REALLOC_ARRAY(sc->chunk_oids, sc->chunk_capacity);
	}

	/* Save chunk OID */
	oidcpy(&sc->chunk_oids[sc->chunk_count], &chunk_oid);
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

	while (1) {
		/* Read next page from file */
		bytes_read = xread(sc->fd, sc->read_buffer, STREAMING_BUFFER_SIZE);

		if (bytes_read < 0) {
			strbuf_addf(&sc->error_message, "failed to read file: %s",
			            strerror(errno));
			sc->error_occurred = 1;
			return -1;
		}

		if (bytes_read == 0)
			break;  /* EOF */

		/* Update content hash with this page */
		if (sc->content_hash_initialized) {
			the_hash_algo->update_fn(&sc->content_hash_ctx,
			                         sc->read_buffer, bytes_read);
		}

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
			chunk_boundary_t boundary = find_chunk_boundary(
				&sc->chunk_config,
				scan_start,
				scan_length,
				&sc->fingerprint);

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
	if (sc->error_occurred)
		return -1;

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

	return 0;
}

void streaming_chunker_cleanup(struct streaming_chunker *sc)
{
	/* Free arrays */
	free(sc->chunk_oids);

	/* Release strbufs */
	strbuf_release(&sc->chunk_buffer);
	strbuf_release(&sc->error_message);

	memset(sc, 0, sizeof(*sc));
}
