/*-
 * Copyright (c) 2009 Aditya Sarawgi
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * $FreeBSD$
 */

#ifndef _FS_EXT2FS_EXT2_DIR_H_
#define	_FS_EXT2FS_EXT2_DIR_H_

/*
 * Darwin: <sys/types.h> here does not publish the fixed-width names the way
 * FreeBSD's does, so a userland consumer that includes this header first would
 * see uint16_t and friends undeclared. The kext build compiles -nostdinc and
 * gets them from the kernel headers instead, hence the guard.
 */
#if !defined(_KERNEL) && !defined(KERNEL)
#include <stdint.h>
#include <sys/types.h>
#endif


/*
 * Structure of a directory entry
 */
#define	EXT2FS_MAXNAMLEN	255

struct	ext2fs_direct {
	uint32_t e2d_ino;		/* inode number of entry */
	uint16_t e2d_reclen;		/* length of this record */
	uint16_t e2d_namlen;		/* length of string in e2d_name */
	char e2d_name[EXT2FS_MAXNAMLEN];/* name with length<=EXT2FS_MAXNAMLEN */
};
/*
 * The new version of the directory entry.  Since EXT2 structures are
 * stored in intel byte order, and the name_len field could never be
 * bigger than 255 chars, it's safe to reclaim the extra byte for the
 * file_type field.
 */
struct	ext2fs_direct_2 {
	uint32_t e2d_ino;		/* inode number of entry */
	uint16_t e2d_reclen;		/* length of this record */
	uint8_t e2d_namlen;		/* length of string in e2d_name */
	uint8_t e2d_type;		/* file type */
	char e2d_name[EXT2FS_MAXNAMLEN];/* name with length<=EXT2FS_MAXNAMLEN */
};

/*
 * Maximal count of links to a file
 */
#define	EXT2_LINK_MAX	32000

/*
 * Ext2 directory file types.  Only the low 3 bits are used.  The
 * other bits are reserved for now.
 */
#define	EXT2_FT_UNKNOWN		0
#define	EXT2_FT_REG_FILE	1
#define	EXT2_FT_DIR		2
#define	EXT2_FT_CHRDEV		3
#define	EXT2_FT_BLKDEV 		4
#define	EXT2_FT_FIFO		5
#define	EXT2_FT_SOCK		6
#define	EXT2_FT_SYMLINK		7
#define	EXT2_FT_MAX		8

/*
 * The "." and ".." pair that opens every directory, as one writable unit.
 *
 * Each half is an on-disk directory entry whose 12-byte record length is what
 * fixes the name arrays at four bytes; the members are naturally aligned, so
 * the struct is exactly 24 bytes with no padding and can be written straight
 * to disk. OpenBSD declares this in <ufs/ext2fs/ext2fs_dir.h>.
 */
struct ext2fs_dirtemplate {
	uint32_t	dot_ino;
	uint16_t	dot_reclen;
	uint8_t		dot_namlen;
	uint8_t		dot_type;
	char		dot_name[4];		/* "." + NUL padding */
	uint32_t	dotdot_ino;
	uint16_t	dotdot_reclen;
	uint8_t		dotdot_namlen;
	uint8_t		dotdot_type;
	char		dotdot_name[4];		/* ".." + NUL padding */
};

/*
 * EXT2_DIR_PAD defines the directory entries boundaries
 *
 * NOTE: It must be a multiple of 4
 */
#define	EXT2_DIR_PAD		 	4
#define	EXT2_DIR_ROUND			(EXT2_DIR_PAD - 1)
#define	EXT2_DIR_REC_LEN(name_len)	(((name_len) + 8 + EXT2_DIR_ROUND) & \
					 ~EXT2_DIR_ROUND)
#endif /* !_FS_EXT2FS_EXT2_DIR_H_ */

