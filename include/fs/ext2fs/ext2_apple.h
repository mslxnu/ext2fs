/*
 * Copyright (c) 2022-2026 Sunneva N. Mariu
 *
 * ext2_apple.h
 *
 * Darwin/XNU glue for the BSD ext2fs sources.  The files under kext/ are
 * imported from NextBSD (FreeBSD's sys/fs/ext2fs) and expect the FreeBSD
 * kernel environment; everything XNU needs in order to host them - the
 * kext-wide allocation tag, lock group, and the module-level init/fini
 * entry points - is declared here rather than scattered through the
 * imported sources.
 */

#ifndef _FS_EXT2FS_EXT2_APPLE_H_
#define	_FS_EXT2FS_EXT2_APPLE_H_

#ifdef KERNEL

#include <kern/locks.h>
#include <libkern/OSMalloc.h>
#include <sys/kernel_types.h>

/*
 * Name reported by lck_grp/OSMalloc introspection (lockstat, zprint).  The
 * bundle id is not usable directly: it is a macro expanding to an unquoted
 * token, BUNDLEID_S is its stringified form.
 */
#define	EXT2_LCKGRP_NAME	"com.beako.filesystems.ext2fs"

/*
 * Kext-wide allocation tag and lock group.  Both are created by ext2_init()
 * on load and destroyed by ext2_fini() on unload, so every consumer must
 * treat them as valid only between those two calls.
 */
extern OSMallocTag	ext2_osmalloc_tag;
extern lck_grp_t	*ext2_lck_grp;

__BEGIN_DECLS

int	ext2_init(void);
int	ext2_fini(void);

__END_DECLS

#endif /* KERNEL */

#endif	/* !_FS_EXT2FS_EXT2_APPLE_H_ */
