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
#include <sys/buf.h>
#include <sys/kernel_types.h>
#include <sys/vnode.h>

/*
 * Block numbers.
 *
 * The imported sources were written against FreeBSD, whose daddr_t is 64 bits
 * wide; XNU's daddr_t is int32_t and its 64-bit spelling is daddr64_t. The
 * ext2 code both stores physical block numbers in daddr_t and, in the extent
 * paths, shifts the high half of a 48-bit address up by 32, so the narrow type
 * would truncate silently. Every plain daddr_t in the imported code has been
 * changed to daddr64_t for that reason. e2fs_daddr_t is untouched: that one is
 * genuinely a 32-bit on-disk quantity.
 */

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
