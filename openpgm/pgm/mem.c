/* vim:ts=8:sts=8:sw=4:noai:noexpandtab
 *
 * portable fail fast memory allocation.
 *
 * Copyright (c) 2010 Miru Limited.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include <ctype.h>
#include <stdio.h>
#include <impl/framework.h>

//#define MEM_DEBUG


/* globals */

bool pgm_mem_gc_friendly = FALSE;


/* locals */

struct pgm_debug_key_t {
	const char*	key;
	unsigned	value;
};
typedef struct pgm_debug_key_t pgm_debug_key_t;

static volatile uint32_t mem_ref_count = 0;


static
bool
debug_key_matches (
	const char* restrict key,
	const char* restrict token,
	unsigned	     length
	)
{
	for (; length; length--, key++, token++)
	{
		const char k = (*key   == '_') ? '-' : tolower (*key  );
		const char t = (*token == '_') ? '-' : tolower (*token);
		if (k != t)
			return FALSE;
	}
	return *key == '\0';
}

static
unsigned
pgm_parse_debug_string (
	const char*	       restrict	string,
	const pgm_debug_key_t* restrict	keys,
	const unsigned			nkeys
	)
{
	unsigned result = 0;

	if (NULL == string)
		return result;

	if (!strcasecmp (string, "all"))
	{
		for (unsigned i = 0; i < nkeys; i++)
			result |= keys[i].value;
	}
	else if (!strcasecmp (string, "help"))
	{
		fprintf (stderr, "Supported debug values:");
		for (unsigned i = 0; i < nkeys; i++)
			fprintf (stderr, " %s", keys[i].key);
		fprintf (stderr, "\n");
	}
	else
	{
		while (string) {
			const char* q = strpbrk (string, ":;, \t");
			if (!q)
				q = string + strlen (string);
			for (unsigned i = 0; i < nkeys; i++)
				if (debug_key_matches (keys[i].key, string, q - string))
					result |= keys[i].value;
			string = q;
			if (*string)
				string++;
		}
	}
	return result;
}

void
pgm_mem_init (void)
{
	static const pgm_debug_key_t keys[] = {
		{ "gc-friendly", 1 },
	};

	if (pgm_atomic_exchange_and_add32 (&mem_ref_count, 1) > 0)
		return;

	const char *val = getenv ("PGM_DEBUG");
	const unsigned flags = !val ? 0 : pgm_parse_debug_string (val, keys, PGM_N_ELEMENTS (keys));
	if (flags & 1)
		pgm_mem_gc_friendly = TRUE;
}

void
pgm_mem_shutdown (void)
{
	pgm_return_if_fail (pgm_atomic_read32 (&mem_ref_count) > 0);

	if (pgm_atomic_exchange_and_add32 (&mem_ref_count, (uint32_t)-1) != 1)
		return;

	/* nop */
}

/* malloc wrappers to hard fail */
void*
pgm_malloc (
	const size_t	n_bytes
	)
{
	if (PGM_LIKELY (n_bytes))
	{
		void* mem = malloc (n_bytes);
		if (mem)
			return mem;

		pgm_fatal ("file %s: line %d (%s): failed to allocate %zu bytes",
			__FILE__, __LINE__, __PRETTY_FUNCTION__,
			n_bytes);
		abort ();
	}
	return NULL;
}

#define SIZE_OVERFLOWS(a,b) (PGM_UNLIKELY ((a) > SIZE_MAX / (b)))

void*
pgm_malloc_n (
	const size_t	n_blocks,
	const size_t	block_bytes
	)
{
	if (SIZE_OVERFLOWS (n_blocks, block_bytes)) {
		pgm_fatal ("file %s: line %d (%s): overflow allocating %zu*%zu bytes",
			__FILE__, __LINE__, __PRETTY_FUNCTION__,
			n_blocks, block_bytes);
	}
	return pgm_malloc (n_blocks * block_bytes);
}

void*
pgm_malloc0 (
	const size_t	n_bytes
	)
{
	if (PGM_LIKELY (n_bytes))
	{
		void* mem = calloc (1, n_bytes);
		if (mem)
			return mem;

		pgm_fatal ("file %s: line %d (%s): failed to allocate %zu bytes",
			__FILE__, __LINE__, __PRETTY_FUNCTION__,
			n_bytes);
		abort ();
	}
	return NULL;
}

void*
pgm_malloc0_n (
	const size_t	n_blocks,
	const size_t	block_bytes
	)
{
	if (PGM_LIKELY (n_blocks && block_bytes))
	{
		void* mem = calloc (n_blocks, block_bytes);
		if (mem)
			return mem;

		pgm_fatal ("file %s: line %d (%s): failed to allocate %zu*%zu bytes",
			__FILE__, __LINE__, __PRETTY_FUNCTION__,
			n_blocks, block_bytes);
		abort ();
	}
	return NULL;
}

void*
pgm_memdup (
	const void*	mem,
	const size_t	n_bytes
	)
{
	void* new_mem;

	if (PGM_LIKELY (NULL != mem))
	{
		new_mem = pgm_malloc (n_bytes);
		memcpy (new_mem, mem, n_bytes);
	}
	else
		new_mem = NULL;

	return new_mem;
}

void*
pgm_realloc (
	void*		mem,
	const size_t	n_bytes
	)
{
	return realloc (mem, n_bytes);
}

void
pgm_free (
	void*		mem
	)
{
	if (PGM_LIKELY (NULL != mem))
		free (mem);
}

/* eof */
