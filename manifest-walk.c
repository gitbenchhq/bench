#include "git-compat-util.h"
#include "manifest-walk.h"
#include "hex.h"

void init_manifest_desc(struct manifest_desc *desc, const void *buffer, unsigned long size, const struct git_hash_algo *algo)
{
	desc->buffer = buffer;
	desc->size = size;
	desc->algo = algo;
	oidclr(&desc->entry_oid, algo);
	desc->entry_size = 0;
}

int manifest_parse_u64(const char *p, const char *end, uint64_t *value, const char **next)
{
	uint64_t v = 0;
	const char *start = p;

	while (p < end && isdigit(*p)) {
		unsigned digit = *p - '0';
		if (v > (UINT64_MAX - digit) / 10)
			return -1; /* overflow */
		v = v * 10 + digit;
		p++;
	}
	if (p == start)
		return -1; /* no digits */
	if (*start == '0' && p - start > 1)
		return -1; /* leading zero */

	*value = v;
	*next = p;
	return 0;
}

int manifest_parse_oid(const char *p, const char *end,
		       const struct git_hash_algo *algo,
		       struct object_id *oid, const char **next)
{
	size_t hexsz = algo->hexsz;
	size_t i;

	if ((size_t)(end - p) < hexsz)
		return -1;
	for (i = 0; i < hexsz; i++) {
		char c = p[i];
		if (!(('0' <= c && c <= '9') || ('a' <= c && c <= 'f')))
			return -1; /* not lowercase hex */
	}
	if (get_oid_hex_algop(p, oid, algo) < 0)
		return -1;
	*next = p + hexsz;
	return 0;
}

/*
 * Parse the next "<oid> <size>\n" line. The OID must be exactly the
 * repository hash's hex length in lowercase; the line must end in '\n'.
 */
int manifest_entry(struct manifest_desc *desc)
{
	const char *buf = desc->buffer;
	const char *end = buf + desc->size;
	const char *p;

	if (!desc->size)
		return 0;

	if (manifest_parse_oid(buf, end, desc->algo, &desc->entry_oid, &p) < 0)
		return -1;
	if (p >= end || *p++ != ' ')
		return -1;
	if (manifest_parse_u64(p, end, &desc->entry_size, &p) < 0)
		return -1;
	if (p >= end || *p != '\n')
		return -1;
	p++;

	desc->size -= p - buf;
	desc->buffer = p;
	return 1;
}
