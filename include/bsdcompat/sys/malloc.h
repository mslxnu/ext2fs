/*
 * Copyright (c) 2022-2026 Sunneva N. Mariu
 *
 * sys/malloc.h
 *
 * FreeBSD's malloc(size, type, flags) / free(addr, type) spelling, which the
 * imported ext2fs sources use and XNU does not provide. XNU publishes the same
 * allocator as _MALLOC() and _FREE(), which are exported BSD KPI and declared
 * by its own <sys/malloc.h>; this header pulls that in with #include_next and
 * adds the two names on top.
 *
 * Unlike the other headers here this one shadows a header XNU really has, so
 * it must not be included on its own - #include_next only reaches the real one
 * because -I../include/bsdcompat precedes the Kernel.framework search path.
 *
 * This replaces the libbsdmalloc submodule, which did the same thing by
 * shadowing <sys/malloc.h> and providing malloc()/free() as real functions in
 * a static library. Inline wrappers need no library at all.
 *
 * M_ZERO needs no special handling: _MALLOC() zeroes the allocation itself
 * when it is passed. libbsdmalloc carried a macro that hoisted the bzero to
 * the caller for constant sizes; nothing here allocates in a path where that
 * would matter, so the complication is dropped rather than reproduced.
 */

#ifndef _BSDCOMPAT_SYS_MALLOC_H_
#define	_BSDCOMPAT_SYS_MALLOC_H_

#include_next <sys/malloc.h>

/*
 * Wrappers rather than macros, so that a stray one-argument free() elsewhere
 * fails to compile instead of expanding into something that silently loses an
 * argument.
 */
static __inline void *
malloc(size_t size, int type, int flags)
{
	return (_MALLOC(size, type, flags));
}

static __inline void
free(void *addr, int type)
{
	_FREE(addr, type);
}

#endif	/* !_BSDCOMPAT_SYS_MALLOC_H_ */
