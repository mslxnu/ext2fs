/*
 * Copyright (c) 2022-2026 Sunneva N. Mariu
 *
 * bsdcompat.h
 *
 * Standard library routines the OpenBSD tools rely on that macOS libc does
 * not provide. Nothing ext2-specific belongs here - see
 * <fs/ext2fs/ext2_compat.h> for that.
 */

#ifndef _EXT2FS_TOOLS_BSDCOMPAT_H_
#define	_EXT2FS_TOOLS_BSDCOMPAT_H_

#include <sys/cdefs.h>
#include <stddef.h>

__BEGIN_DECLS

#ifndef HAVE_REALLOCARRAY
void	*reallocarray(void *, size_t, size_t);
#endif

__END_DECLS

#endif	/* !_EXT2FS_TOOLS_BSDCOMPAT_H_ */
