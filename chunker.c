#include "git-compat-util.h"
#include "chunker.h"
#include "hash.h"
#include "repository.h"

/*
 * Gear hash lookup table - 256 random 64-bit values
 *
 * This table is used for fast rolling hash computation:
 *   fingerprint = (fingerprint << 1) + GearTable[byte]
 *
 * The values are generated deterministically (fixed seed) to ensure
 * reproducible chunking across different machines and runs.
 */
static uint64_t gear_table[256];
static int gear_table_initialized = 0;

/*
 * Initialize the Gear hash lookup table with deterministic random values
 *
 * We use Git's SHA256 hash function to generate pseudorandom 64-bit values
 * from sequential input (0, 1, 2, ..., 255). This ensures:
 *   - Deterministic: Same table on every machine
 *   - Good distribution: SHA256 provides quality randomness
 *   - No external dependencies: Uses Git's existing hash infrastructure
 *
 * Reference: FastCDC and restic use similar table generation approaches
 */
void init_gear_table(void)
{
	struct git_hash_ctx ctx;
	unsigned char hash[GIT_MAX_RAWSZ];
	const struct git_hash_algo *algo;
	int i;

	if (gear_table_initialized)
		return;

	/*
	 * Use SHA256 for generating pseudorandom values
	 * (Bench uses SHA256 as default hash algorithm)
	 */
	algo = &hash_algos[GIT_HASH_SHA256];

	/*
	 * Generate 256 pseudorandom 64-bit values
	 * Hash input byte i to get random value for gear_table[i]
	 */
	for (i = 0; i < 256; i++) {
		unsigned char input[1];
		input[0] = (unsigned char)i;

		/* Hash single byte using SHA256 */
		algo->init_fn(&ctx);
		algo->update_fn(&ctx, input, 1);
		algo->final_fn(hash, &ctx);

		/*
		 * Extract first 8 bytes as 64-bit value
		 * Use big-endian for consistency across architectures
		 */
		gear_table[i] = ((uint64_t)hash[0] << 56) |
				((uint64_t)hash[1] << 48) |
				((uint64_t)hash[2] << 40) |
				((uint64_t)hash[3] << 32) |
				((uint64_t)hash[4] << 24) |
				((uint64_t)hash[5] << 16) |
				((uint64_t)hash[6] << 8) |
				((uint64_t)hash[7]);
	}

	gear_table_initialized = 1;
}

/*
 * Initialize chunk configuration with default values
 */
void init_chunk_config(struct chunk_config *cfg)
{
	cfg->min_size = CHUNK_MIN_SIZE_DEFAULT;
	cfg->max_size = CHUNK_MAX_SIZE_DEFAULT;
	cfg->mask = CHUNK_MASK_DEFAULT;
}

/*
 * Initialize chunk configuration with custom target size
 *
 * The mask determines average chunk size: 2^target_bits
 * Examples:
 *   target_bits = 24 → mask = 0xFFFFFF → avg 16MB chunks
 *   target_bits = 26 → mask = 0x3FFFFFF → avg 64MB chunks
 */
void init_chunk_config_custom(struct chunk_config *cfg, int target_bits)
{
	cfg->min_size = CHUNK_MIN_SIZE_DEFAULT;
	cfg->max_size = CHUNK_MAX_SIZE_DEFAULT;
	cfg->mask = (1ULL << target_bits) - 1;
}

/*
 * Find next chunk boundary using Gear hash rolling window
 *
 * Returns:
 *   -1: No boundary found in this buffer (keep accumulating)
 *    0: Empty buffer
 *   >0: Boundary found at this byte offset
 *
 * The caller is responsible for:
 *   - EOF handling (detecting end of file)
 *   - min_size enforcement (ignoring boundaries before min_size)
 *   - max_size enforcement (forcing cuts at max_size)
 *
 * The fingerprint state is maintained across calls to handle files
 * larger than the buffer size.
 */
chunk_boundary_t find_chunk_boundary(const struct chunk_config *cfg,
				     const unsigned char *data,
				     size_t len,
				     uint64_t *fingerprint)
{
	size_t i;
	uint64_t fp;

	/* Handle empty buffer */
	if (len == 0)
		return 0;

	/* Ensure gear table is initialized */
	if (!gear_table_initialized)
		init_gear_table();

	/*
	 * Copy fingerprint from caller's state.
	 * This maintains hash state across multiple buffer reads.
	 */
	fp = *fingerprint;

	/*
	 * Scan through buffer looking for chunk boundary
	 */
	for (i = 0; i < len; i++) {
		/*
		 * Update rolling hash with current byte.
		 * This is the core of the Gear hash algorithm:
		 *   - Left shift maintains history
		 *   - Lookup table adds pseudorandom contribution
		 */
		fp = (fp << 1) + gear_table[data[i]];

		/*
		 * Check if hash matches boundary pattern (mask bits all zero).
		 *
		 * Note: We don't enforce min_size here - that's the caller's job.
		 * We just report where content-defined boundaries occur.
		 */
		if ((fp & cfg->mask) == 0) {
			/*
			 * Found content-defined boundary!
			 * Return offset = i+1 (number of bytes in chunk, including current byte)
			 *
			 * Example: If i=99, we've processed bytes 0-99 (100 bytes total)
			 *          Return 100 to indicate chunk size
			 */
			*fingerprint = fp;
			return (ssize_t)(i + 1);
		}
	}

	/*
	 * No boundary found in this buffer.
	 * Return -1 to signal "keep accumulating bytes".
	 *
	 * The caller should:
	 *   - Add these bytes to current chunk
	 *   - Check if max_size exceeded (force cut if so)
	 *   - Continue reading more data
	 */
	*fingerprint = fp;
	return -1;
}
