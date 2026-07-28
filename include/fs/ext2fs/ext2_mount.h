/*-
 * Copyright (c) 1982, 1986, 1989, 1993
 *	The Regents of the University of California.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 4. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 *	@(#)ufsmount.h	8.6 (Berkeley) 3/30/95
 * $FreeBSD$
 */

#ifndef _FS_EXT2FS_EXT2_MOUNT_H_
#define	_FS_EXT2FS_EXT2_MOUNT_H_

#include <stdint.h>

/*
 * Darwin: name under which the kext registers with VFS.  This is the string
 * handed to mount(2) as its type argument and the one that shows up in
 * mount(8) output, so it has to match vfe_fsname in the vfs_fsentry.
 */
#define	MOUNT_EXT2FS	"ext2fs"

/*
 * Arguments passed by mount_ext2fs(8) through mount(2).  Shared with userland,
 * hence outside the _KERNEL guard.
 *
 * fspec must stay the first member: the file system registers with
 * VFS_TBLLOCALVOL, which makes XNU read a device path pointer from the front
 * of the argument block, resolve it itself, and hand the resulting devvp to
 * VFS_MOUNT with the argument pointer advanced past it.
 *
 * The layout is only defined for 64-bit userland; macOS has shipped no 32-bit
 * user space since 10.15, well below the 12.0 deployment target.
 */
#define	EXT2_ARGSVERSION	1

struct ext2_args {
	char		*fspec;		/* block special device to mount */
	uint32_t	 e2_version;	/* EXT2_ARGSVERSION */
	uint32_t	 e2_flags;	/* EXT2MNT_* below */
};

#define	EXT2MNT_FORCE		0x00000001	/* mount despite an unclean fs */

#ifdef _KERNEL

#ifdef MALLOC_DECLARE
MALLOC_DECLARE(M_EXT2NODE);
#endif

struct vnode;

/* This structure describes the ext2fs specific mount structure data. */
struct ext2mount {
	struct	mount *um_mountp;		/* filesystem vfs structure */
	struct	cdev *um_dev;			/* device mounted */
	struct	vnode *um_devvp;		/* block device mounted vnode */

	struct	m_ext2fs *um_e2fs;		/* EXT2FS */

	u_long	um_nindir;			/* indirect ptrs per block */
	u_long	um_bptrtodb;			/* indir ptr to disk block */
	u_long	um_seqinc;			/* inc between seq blocks */

	struct mtx um_lock;			/* Protects ext2mount & fs */

	struct g_consumer *um_cp;
	struct bufobj *um_bo;
};

#define	EXT2_LOCK(aa)		mtx_lock(&(aa)->um_lock)
#define	EXT2_UNLOCK(aa)	mtx_unlock(&(aa)->um_lock)
#define	EXT2_MTX(aa)		(&(aa)->um_lock)

/* Convert mount ptr to ext2fsmount ptr. */
#define	VFSTOEXT2(mp)	((struct ext2mount *)((mp)->mnt_data))

/*
 * Macros to access file system parameters in the ufsmount structure.
 * Used by ufs_bmap.
 */
#define	MNINDIR(ump)			((ump)->um_nindir)
#define	blkptrtodb(ump, b)		((b) << (ump)->um_bptrtodb)
#define	is_sequential(ump, a, b)	((b) == (a) + ump->um_seqinc)
#endif /* _KERNEL */

#endif	/* !_FS_EXT2FS_EXT2_MOUNT_H_ */
