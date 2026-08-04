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
#include <sys/dirent.h>	/* struct dirent, for EXT2_DIRENT_RECLEN */
#include <sys/random.h>	/* read_random */
#include <sys/kernel_types.h>
#include <sys/vnode.h>

#include <fs/ext2fs/ext2fs.h>	/* EXT2_MIN_BLOCK_LOG_SIZE */

/*
 * Bytes <-> device blocks.
 *
 * <arm/param.h> and <i386/param.h> define btodb/dbtob under __APPLE__ taking
 * the block size as a second argument. The BSD file system code these sources
 * come from uses the one-argument form that assumes DEV_BSIZE, so the Apple
 * spelling is replaced outright rather than worked around at each call site.
 * The same substitution is made for the userland tools in ext2_compat.h.
 */
#undef btodb
#undef dbtob
#define	btodb(bytes)	((bytes) >> DEV_BSHIFT)
#define	dbtob(db)	((db) << DEV_BSHIFT)

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
 * Per-inode lock. FreeBSD's VI_LOCK/VI_UNLOCK took the vnode interlock; XNU
 * has no such thing, so the inode carries its own mutex.
 *
 * The NULL check covers the window before ext2_vget() has allocated it - a
 * partially built inode on an error path - rather than being an ordinary
 * condition.
 */
#define	EXT2_ILOCK(ip)		do {					\
	if ((ip)->i_lock != NULL)					\
		lck_mtx_lock((ip)->i_lock);				\
} while (0)

#define	EXT2_IUNLOCK(ip)	do {					\
	if ((ip)->i_lock != NULL)					\
		lck_mtx_unlock((ip)->i_lock);				\
} while (0)

/* Smallest block an ext2 file system may use. */
#ifndef MINBSIZE
#define	MINBSIZE	(1 << EXT2_MIN_BLOCK_LOG_SIZE)
#endif

/* FreeBSD utility macros with no XNU counterpart. */
#ifndef nitems
#define	nitems(x)	(sizeof((x)) / sizeof((x)[0]))
#endif
#ifndef roundup2
#define	roundup2(x, y)	(((x) + ((y) - 1)) & (~((y) - 1)))	/* y is a power of 2 */
#endif

/*
 * Length of the struct dirent that readdir hands back for a name of the given
 * length: the fixed part, plus the name and its terminator, rounded up to four
 * bytes so the next record stays aligned.
 *
 * This is FreeBSD's GENERIC_DIRSIZ() written against XNU's struct dirent,
 * which has a different fixed part.
 */
#define	EXT2_DIRENT_RECLEN(namlen)					\
	((uint16_t)((__builtin_offsetof(struct dirent, d_name) +	\
	    (namlen) + 1 + 3) & ~3))

/*
 * Is this mount running asynchronously?
 *
 * FreeBSD spells this DOINGASYNC(vp), reaching into the mount structure. XNU
 * keeps the mount opaque and publishes the same flag through vfs_flags().
 */
#define	DOINGASYNC(vp)	((vfs_flags(vnode_mount(vp)) & MNT_ASYNC) != 0)

/*
 * Find the first byte in a buffer that is not equal to c, or NULL if every
 * byte matches.
 *
 * FreeBSD's libkern supplies this; XNU has no counterpart. The allocator uses
 * it to skip over fully-used bytes of a block or inode bitmap, where c is
 * 0xff, so a byte-at-a-time scan is adequate - the bitmaps are one block and
 * the loop stops at the first byte with a free bit.
 */
static __inline void *
memcchr(const void *s, int c, size_t n)
{
	const unsigned char *p = (const unsigned char *)s;

	for (; n != 0; n--, p++) {
		if (*p != (unsigned char)c) {
			return (void *)(uintptr_t)p;
		}
	}

	return (NULL);
}

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

/*
 * In-core inode hash. Declared here as well as in ext2_extern.h so the module
 * entry point can create and destroy it without pulling in the whole of the
 * file system's internal interface.
 */
int	ext2_ihashinit(void);
void	ext2_ihashdestroy(void);

__END_DECLS

#endif /* KERNEL */

#endif	/* !_FS_EXT2FS_EXT2_APPLE_H_ */
