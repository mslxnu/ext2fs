/*
 * Copyright (c) 2022-2026 Sunneva N. Mariu
 *
 * bsdcompat.c
 *
 * Standard library routines macOS libc does not provide.
 */

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>

#include "bsdcompat.h"

#ifndef HAVE_REALLOCARRAY
/*
 * realloc() for an array, refusing to proceed if nmemb * size would wrap.
 * The threshold is the usual one: if either operand is below sqrt(SIZE_MAX)
 * the product cannot overflow, so the division is only needed above it.
 */
void *
reallocarray(void *optr, size_t nmemb, size_t size)
{
#define	MUL_NO_OVERFLOW	((size_t)1 << (sizeof(size_t) * 4))

	if ((nmemb >= MUL_NO_OVERFLOW || size >= MUL_NO_OVERFLOW) &&
	    nmemb > 0 && SIZE_MAX / nmemb < size) {
		errno = ENOMEM;
		return (NULL);
	}

	return (realloc(optr, nmemb * size));

#undef MUL_NO_OVERFLOW
}
#endif /* !HAVE_REALLOCARRAY */
