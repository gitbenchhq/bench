#ifndef MANIFEST_WALK_H
#define MANIFEST_WALK_H

#include "hash.h"

/*
 * Walks the chunk lines of a manifest. Each line is
 *
 *	<chunk oid> SP <uncompressed size> LF
 *
 * with the OID in lowercase hex of the repository's hash algorithm and the
 * size in canonical decimal (no sign, no leading zeros; zero is "0").
 */
struct manifest_desc {
	const void *buffer;
	unsigned long size;
	const struct git_hash_algo *algo;
	struct object_id entry_oid;
	uint64_t entry_size;
};

/*
 * Initialize a manifest descriptor for walking through chunk lines.
 * Points the descriptor at the first chunk line (after the header).
 */
void init_manifest_desc(struct manifest_desc *desc, const void *buffer, unsigned long size, const struct git_hash_algo *algo);

/*
 * Read the next chunk line from the manifest.
 * Returns 1 if a line was read (OID in desc->entry_oid, size in
 * desc->entry_size), 0 at the end of the chunk list, and -1 if the next
 * line is malformed. A malformed line is never treated as the end, so a
 * damaged manifest cannot silently hide chunks from its readers.
 */
int manifest_entry(struct manifest_desc *desc);

/*
 * Parse a canonical decimal uint64 from [p, end): one or more digits, no
 * sign, no leading zero unless the value is exactly 0, no overflow.
 * On success stores the value, sets *next to the first byte after the
 * digits and returns 0; returns -1 otherwise.
 */
int manifest_parse_u64(const char *p, const char *end, uint64_t *value, const char **next);

/*
 * Parse an OID from [p, end): exactly algo->hexsz lowercase hex digits.
 * On success stores the OID, sets *next to the byte after it and returns
 * 0; returns -1 otherwise.
 */
int manifest_parse_oid(const char *p, const char *end,
		       const struct git_hash_algo *algo,
		       struct object_id *oid, const char **next);

#endif /* MANIFEST_WALK_H */
