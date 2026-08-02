/*
 * Copyright (c) 2022-2026 Sunneva N. Mariu
 *
 * ext2_compat.h
 *
 * Definitions the OpenBSD-derived userland tools expect but that the FreeBSD
 * headers in this directory spell differently or leave to the OpenBSD kernel
 * tree. Everything here is a thin restatement of what the shared headers
 * already define - no on-disk structure is described twice.
 */

#ifndef _FS_EXT2FS_EXT2_COMPAT_H_
#define	_FS_EXT2FS_EXT2_COMPAT_H_

#include <sys/stat.h>
#include <dirent.h>		/* DIRBLKSIZ, DT_*, IFTODT */
#include <stdint.h>
#include <string.h>		/* memcpy, for the load/save macros below */

#include <fs/ext2fs/ext2fs.h>
#include <fs/ext2fs/ext2_dir.h>	/* EXT2_FT_*, EXT2_DIR_REC_LEN */
#include <fs/ext2fs/fs.h>	/* SBSIZE, fsbtodb, dbtofsb, NINDIR */

/* OpenBSD's 32-bit disk address type. */
typedef int32_t daddr32_t;

/*
 * Disk block unit the BSD file system code counts in. macOS publishes neither
 * of these, but its block devices use the same 512-byte unit.
 */
#ifndef DEV_BSHIFT
#define	DEV_BSHIFT	9
#endif
#ifndef DEV_BSIZE
#define	DEV_BSIZE	(1 << DEV_BSHIFT)
#endif
/*
 * macOS <sys/param.h> has btodb/dbtob too, but takes the block size as a
 * second argument. The BSD file system code these tools come from uses the
 * one-argument form that assumes DEV_BSIZE, so the macOS spelling is replaced
 * outright rather than worked around at each of the call sites.
 */
#undef btodb
#undef dbtob
#define	btodb(bytes)	((bytes) >> DEV_BSHIFT)
#define	dbtob(db)	((db) << DEV_BSHIFT)

/*
 * Unprefixed file type bits. OpenBSD's tools pick these up from
 * <ufs/ufs/dinode.h>; macOS <sys/stat.h> only publishes the S_ forms.
 */
#ifndef IFMT
#define	IFMT	S_IFMT
#define	IFIFO	S_IFIFO
#define	IFCHR	S_IFCHR
#define	IFDIR	S_IFDIR
#define	IFBLK	S_IFBLK
#define	IFREG	S_IFREG
#define	IFLNK	S_IFLNK
#define	IFSOCK	S_IFSOCK
#endif

/* OpenBSD's spelling of the largest block size ext2 allows. */
#ifndef EXT2_MAXBSIZE
#define	EXT2_MAXBSIZE	EXT2_MAX_BLOCK_SIZE
#endif

/*
 * Size of an on-disk inode. Revision 0 file systems have no e2fs_inode_size
 * field and always use 128 bytes; later revisions carry the real value.
 */
#ifndef EXT2_DINODE_SIZE
#define	EXT2_DINODE_SIZE(fs)						\
	((fs)->e2fs->e2fs_rev > E2FS_REV0 ?				\
	    (fs)->e2fs->e2fs_inode_size : E2FS_REV0_INODE_SIZE)
#endif

/*
 * Sizes and thresholds OpenBSD's tools take from <ufs/ufs/fs.h> and the
 * OpenBSD ext2fs headers.
 */
#ifndef EXT2_REV0_DINODE_SIZE
#define	EXT2_REV0_DINODE_SIZE	E2FS_REV0_INODE_SIZE
#endif

/* Smallest ext2 block, and its base-2 logarithm. */
#ifndef LOG_MINBSIZE
#define	LOG_MINBSIZE	EXT2_MIN_BLOCK_LOG_SIZE
#endif
#ifndef MINBSIZE
#define	MINBSIZE	(1 << LOG_MINBSIZE)
#endif

/*
 * ext2 has no fragments - the fragment size always equals the block size - so
 * e2fs_log_fsize uses the same 1024-byte base as e2fs_log_bsize.
 */
#ifndef LOG_MINFSIZE
#define	LOG_MINFSIZE	LOG_MINBSIZE
#endif

/* Percentage of blocks newfs reserves for the super-user by default. */
#ifndef MINFREE
#define	MINFREE		5
#endif

/*
 * Space ext2 keeps clear at the very front of the volume for a boot block.
 * With a 1024-byte block this costs one whole block; larger blocks absorb it
 * into block 0.
 */
#ifndef BBSIZE
#define	BBSIZE		1024
#endif

/* Byte offset of the primary superblock. */
#ifndef SBOFF
#define	SBOFF		((off_t)(SBLOCK * DEV_BSIZE))
#endif

/*
 * Block pointers in an inode. <fs/ext2fs/inode.h> has these too, but that
 * header is kernel-only (it pulls in sys/lock.h and sys/mutex.h), so the
 * counts are restated here for userland.
 */
#ifndef NDADDR
#define	NDADDR		12	/* direct blocks */
#define	NIADDR		3	/* indirect blocks */
#endif

/* File type bits as the ext2 tools spell them. */
#ifndef EXT2_IFMT
#define	EXT2_IFMT	IFMT
#define	EXT2_IFIFO	IFIFO
#define	EXT2_IFCHR	IFCHR
#define	EXT2_IFDIR	IFDIR
#define	EXT2_IFBLK	IFBLK
#define	EXT2_IFREG	IFREG
#define	EXT2_IFLNK	IFLNK
#define	EXT2_IFSOCK	IFSOCK
#endif

/*
 * Feature flags. The shared header spells these without the underscore that
 * the OpenBSD tools use.
 */
#ifndef EXT2F_ROCOMPAT_SPARSE_SUPER
#define	EXT2F_ROCOMPAT_SPARSE_SUPER	EXT2F_ROCOMPAT_SPARSESUPER
#define	EXT2F_ROCOMPAT_LARGE_FILE	EXT2F_ROCOMPAT_LARGEFILE
#endif

/* e2fs_beh: what the driver should do after an error. */
#ifndef E2FS_BEH_CONTINUE
#define	E2FS_BEH_CONTINUE	1
#define	E2FS_BEH_READONLY	2
#define	E2FS_BEH_PANIC		3
#define	E2FS_BEH_DEFAULT	E2FS_BEH_CONTINUE
#endif

/* e2fs_creator: which OS laid the file system down. */
#ifndef E2FS_OS_LINUX
#define	E2FS_OS_LINUX		0
#define	E2FS_OS_HURD		1
#define	E2FS_OS_MASIX		2
#define	E2FS_OS_FREEBSD		3
#define	E2FS_OS_LITES		4
#endif

/*
 * Sparse superblock rule: with EXT2F_ROCOMPAT_SPARSESUPER set, only groups 0
 * and 1 and those whose index is a power of 3, 5 or 7 carry a superblock and
 * group descriptor backup.
 *
 * The kernel side has its own copy of this in kext/ext2_alloc.c, which is the
 * source of truth; the two must agree, or newfs and the driver will disagree
 * about where the backups live. Keep them in step.
 */
static __inline int
cg_has_sb(int i)
{
	int a3, a5, a7;

	if (i == 0 || i == 1)
		return (1);
	for (a3 = 3, a5 = 5, a7 = 7;
	    a3 <= i || a5 <= i || a7 <= i;
	    a3 *= 3, a5 *= 5, a7 *= 7)
		if (i == a3 || i == a5 || i == a7)
			return (1);
	return (0);
}

/*
 * Host <-> file system byte order.
 *
 * ext2 metadata is little-endian on disk and every supported target
 * (arm64e, x86_64) is little-endian, so these are identity operations. This
 * matches the assumption <fs/ext2fs/ext2fs.h> already makes where it defines
 * e2fs_cgload/e2fs_cgsave as plain memcpy.
 */
#include <libkern/OSByteOrder.h>

#define	h2fs16(x)	OSSwapHostToLittleInt16(x)
#define	h2fs32(x)	OSSwapHostToLittleInt32(x)
#define	h2fs64(x)	OSSwapHostToLittleInt64(x)
#define	fs2h16(x)	OSSwapLittleToHostInt16(x)
#define	fs2h32(x)	OSSwapLittleToHostInt32(x)
#define	fs2h64(x)	OSSwapLittleToHostInt64(x)

/* The BSD spellings of the same conversions, and the unconditional swaps. */
#ifndef htole16
#define	htole16(x)	OSSwapHostToLittleInt16(x)
#define	htole32(x)	OSSwapHostToLittleInt32(x)
#define	htole64(x)	OSSwapHostToLittleInt64(x)
#define	letoh16(x)	OSSwapLittleToHostInt16(x)
#define	letoh32(x)	OSSwapLittleToHostInt32(x)
#define	letoh64(x)	OSSwapLittleToHostInt64(x)
#endif

#ifndef swap16
#define	swap16(x)	OSSwapInt16(x)
#define	swap32(x)	OSSwapInt32(x)
#define	swap64(x)	OSSwapInt64(x)
#endif

/*
 * Superblock and inode load/save. The shared header supplies the group
 * descriptor pair but not these; they follow the same little-endian shortcut.
 */
#ifndef e2fs_sbload
#define	e2fs_sbload(old, new)	memcpy((new), (old), sizeof(struct ext2fs))
#define	e2fs_sbsave(old, new)	memcpy((new), (old), sizeof(struct ext2fs))
#endif

#ifndef e2fs_iload
#define	e2fs_iload(fs, old, new) \
	memcpy((new), (old), EXT2_DINODE_SIZE(fs))
#define	e2fs_isave(fs, old, new) \
	memcpy((new), (old), EXT2_DINODE_SIZE(fs))
#endif

/*
 * On-disk length of a directory entry holding a name of the given length.
 * The shared header calls this EXT2_DIR_REC_LEN.
 */
#ifndef EXT2FS_DIRSIZ
#define	EXT2FS_DIRSIZ(namlen)	EXT2_DIR_REC_LEN(namlen)
#endif


/* Round a byte count up to a whole file system block. */
#ifndef blkroundup
#define	blkroundup(fs, size) \
	(((size) + (fs)->e2fs_qbmask) & ~(fs)->e2fs_qbmask)
#endif

/* Inode mode bits -> dirent d_type. */
#ifndef E2IFTODT
#define	E2IFTODT(mode)	IFTODT(mode)
#endif

/*
 * dirent d_type -> the file type byte stored in an ext2 directory entry.
 * The two numbering schemes are unrelated, so this is a straight table.
 */
static __inline uint8_t
inot2ext2dt(uint16_t type)
{
	switch (type) {
	case DT_FIFO:	return (EXT2_FT_FIFO);
	case DT_CHR:	return (EXT2_FT_CHRDEV);
	case DT_DIR:	return (EXT2_FT_DIR);
	case DT_BLK:	return (EXT2_FT_BLKDEV);
	case DT_REG:	return (EXT2_FT_REG_FILE);
	case DT_LNK:	return (EXT2_FT_SYMLINK);
	case DT_SOCK:	return (EXT2_FT_SOCK);
	default:	return (EXT2_FT_UNKNOWN);
	}
}

#endif	/* !_FS_EXT2FS_EXT2_COMPAT_H_ */
