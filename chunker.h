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
 * boundaries. The caller is responsible for min/max size enforcement and EOF handling.
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
 * Usage pattern:
 *   uint64_t fp = 0;
 *   size_t current_chunk_size = 0;
 *
 *   while (reading_file) {
 *     ssize_t nread = read(fd, buffer, sizeof(buffer));
 *     if (nread <= 0) break;  // EOF or error
 *
 *     chunk_boundary_t boundary = find_chunk_boundary(&cfg, buffer, nread, &fp);
 *
 *     if (boundary < 0) {
 *       // No boundary found, accumulate
 *       current_chunk_size += nread;
 *       if (current_chunk_size >= cfg.max_size) {
 *         // Force cut at max_size
 *       }
 *     } else if (boundary > 0) {
 *       // Boundary found
 *       current_chunk_size += boundary;
 *       if (current_chunk_size >= cfg.min_size) {
 *         // Accept boundary, create chunk
 *       }
 *     }
 *   }
 *
 *   // Handle final chunk at EOF
 *   if (current_chunk_size > 0) {
 *     // Create final chunk
 *   }
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

#endif /* CHUNKER_H */
