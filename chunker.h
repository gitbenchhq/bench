#ifndef CHUNKER_H
#define CHUNKER_H

#include "git-compat-util.h"
#include "hash.h"

/**
 * Content-Defined Chunking using Gear Hash (FastCDC variant)
 *
 * This implements a simple rolling hash algorithm for splitting large files
 * into variable-sized chunks. Chunks are cut at content-defined boundaries,
 * enabling deduplication when file content shifts.
 *
 * Algorithm:
 *   1. Skip first min_size bytes (guaranteed minimum chunk size)
 *   2. Compute rolling Gear hash: fp = (fp << 1) + GearTable[byte]
 *   3. Check if (fp & mask) == 0 → boundary found
 *   4. Force cut at max_size if no boundary found
 *
 * The mask determines average chunk size:
 *   mask with N trailing 1 bits → avg chunk size = 2^N bytes
 *   Example: mask = 0xFFFFFF (24 bits) → avg 16MB chunks
 */

/* Chunk size configuration */
struct chunk_config {
	size_t min_size;  /* Minimum chunk size (e.g., 4MB) */
	size_t max_size;  /* Maximum chunk size (e.g., 128MB) */
	uint64_t mask;    /* Bit mask for boundary detection */
};

/*
 * Default chunk size parameters
 *
 * These are fallback values used when repo settings are not available.
 * Normally, chunk sizes should be read from repository configuration via
 * repo_settings_get_chunk_*_size() functions (see repo-settings.h).
 *
 * Defaults optimized for genomics files (50-200 GB typical):
 *   - min: 2 MB (prevents excessive fragmentation)
 *   - target: 16 MB (optimal for large files, manageable manifests)
 *   - max: 64 MB (reasonable upper bound, memory-safe)
 *
 * Users can configure via:
 *   bench config chunk.minSize 4m
 *   bench config chunk.targetSize 32m
 *   bench config chunk.maxSize 128m
 */
#define CHUNK_MIN_SIZE_DEFAULT  (2 * 1024 * 1024)      /* 2MB */
#define CHUNK_TARGET_SIZE_DEFAULT (16 * 1024 * 1024)   /* 16MB */
#define CHUNK_MAX_SIZE_DEFAULT  (64 * 1024 * 1024)     /* 64MB */

/* Mask calculation: for target size 2^N, mask = (1 << N) - 1 */
#define CHUNK_MASK_BITS_DEFAULT 24  /* 2^24 = 16MB average */
#define CHUNK_MASK_DEFAULT ((1ULL << CHUNK_MASK_BITS_DEFAULT) - 1)

/*
 * Chunk boundary result
 *
 * Return value interpretation:
 *   -1: No boundary found in this buffer, keep accumulating bytes
 *    0: Empty buffer (shouldn't happen in normal operation)
 *   >0: Boundary found at this byte offset (chunk contains 'offset' bytes)
 *
 * Note: The caller is responsible for EOF handling and min/max size enforcement.
 */
typedef ssize_t chunk_boundary_t;

/**
 * Initialize chunker configuration with default values
 */
void init_chunk_config(struct chunk_config *cfg);

/**
 * Initialize chunker configuration with custom target size
 *
 * @param cfg Configuration struct to initialize
 * @param target_bits Number of bits for mask (avg chunk size = 2^target_bits)
 *                    Example: 24 → 16MB avg, 26 → 64MB avg
 */
void init_chunk_config_custom(struct chunk_config *cfg, int target_bits);

/**
 * Find the next chunk boundary in a data stream
 *
 * This function scans the input buffer using Gear hash to find content-defined
 * boundaries. It knows nothing about min/max size or EOF.
 *
 * @param cfg Chunking configuration (mask is used for boundary detection)
 * @param data Input data buffer
 * @param len Length of input data
 * @param fingerprint Pointer to rolling hash state (must be initialized to 0
 *                    for first chunk, then maintained across calls for same file)
 * @return Boundary offset:
 *           -1 = no boundary found in this buffer
 *            0 = empty buffer
 *           >0 = boundary found at this offset (chunk contains 'offset' bytes)
 *
 * This is the hash scan only. Chunkers must not call it directly: use
 * cdc_feed() below, which applies min_size, max_size and the fingerprint
 * reset around it so every chunker cuts at the same places.
 */
chunk_boundary_t find_chunk_boundary(const struct chunk_config *cfg,
				     const unsigned char *data,
				     size_t len,
				     uint64_t *fingerprint);

/**
 * Initialize the Gear hash lookup table
 *
 * This must be called once at program startup before using the chunker.
 * The table contains 256 random 64-bit values used for rolling hash computation.
 *
 * The lookup table is deterministic (uses fixed seed) to ensure chunking
 * is reproducible across different runs and machines.
 */
void init_gear_table(void);

/*
 * The cut rule, in one place, for every chunker. One chunk is being built;
 * cdc_state holds its length so far and its gear fingerprint.
 *
 * cdc_feed() takes the next n bytes of the input and returns how many of
 * them belong to the current chunk (at least 1 when n > 0). If the chunk
 * ends right after those bytes it sets *cut and resets the state for the
 * next chunk. The caller appends the returned bytes to its chunk, emits
 * the chunk when *cut is set, and calls again with the remaining bytes.
 *
 * The rule:
 *   - the first min_size bytes of a chunk are not hashed;
 *   - after that, fp = (fp << 1) + gear[byte], and the chunk ends after
 *     the first byte where (fp & mask) == 0;
 *   - otherwise it ends at exactly max_size bytes;
 *   - fp starts at 0 for every chunk.
 * Cut points depend only on the bytes, never on how the input was split
 * into calls, so any read size gives the same chunks.
 */
struct cdc_state {
	size_t len;
	uint64_t fp;
};

#define CDC_STATE_INIT { 0, 0 }

size_t cdc_feed(const struct chunk_config *cfg, struct cdc_state *st,
		const unsigned char *buf, size_t n, int *cut);

/*
 * Bytes per read for the streaming chunkers: buf_size, or for tests the
 * value of GIT_TEST_BENCH_READ_SIZE (clamped to 1..buf_size), so the
 * test suite can check that cut points do not depend on the read size.
 */
size_t chunker_read_size(size_t buf_size);

/*
 * A file's size and modification time, taken before chunking it, so the
 * chunkers can refuse a file that was written while they read it. The
 * size check alone misses an in-place rewrite that keeps the size (an
 * instrument or simulation updating its output); mtime catches those.
 * ctime is not compared: chmod/chown change it without touching content.
 */
struct chunker_file_stamp {
	int valid;
	off_t size;
	time_t mtime;
	unsigned int mtime_nsec;
};

/* Record fd's size and mtime. Leaves the stamp invalid if fstat fails. */
void chunker_stamp_file(int fd, struct chunker_file_stamp *stamp);

/*
 * Return 1, with a message appended to err, if fd's size or mtime differs
 * from the stamp; 0 if unchanged or the stamp is invalid.
 */
int chunker_file_changed(int fd, const struct chunker_file_stamp *stamp,
			 struct strbuf *err);

#endif /* CHUNKER_H */
