/*-
 *  modified for EXT2FS support in Lites 1.1
 *
 *  Aug 1995, Godmar Back (gback@cs.utah.edu)
 *  University of Utah, Department of Computer Science
 */
/*-
 * Copyright (c) 1982, 1986, 1989, 1993
 *	The Regents of the University of California.  All rights reserved.
 * (c) UNIX System Laboratories, Inc.
 * All or some portions of this file are derived from material licensed
 * to the University of California by American Telephone and Telegraph
 * Co. or Unix System Laboratories, Inc. and are reproduced herein with
 * the permission of UNIX System Laboratories, Inc.
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
 *	@(#)ufs_vnops.c	8.7 (Berkeley) 2/3/94
 *	@(#)ufs_vnops.c 8.27 (Berkeley) 5/27/95
 * $FreeBSD$
 */

/*
 * ext2_readwrite.c
 *
 * The file data path, written against XNU's cluster layer.
 *
 * FreeBSD did this work itself: ext2_read() walked the file a buffer at a
 * time with bread(), and it had two of them - ext2_ind_read() for indirect
 * blocks and ext4_ext_read() for extent-mapped files - because the caller had
 * to know how a file was laid out in order to find its blocks.
 *
 * On XNU that distinction does not belong here. The cluster layer does the
 * paging and the I/O, and asks the file system where a byte range lives
 * through VNOP_BLOCKMAP - which ext2_blockmap() already answers for both
 * layouts. So the three read paths collapse into one call, and what is left
 * in this file is the part the cluster layer cannot know: how far the file
 * extends, and which blocks a write has to allocate before its data can be
 * written back.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/buf.h>
#include <sys/kauth.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/ubc.h>
#include <sys/uio.h>
#include <sys/vnode.h>

#include <fs/ext2fs/inode.h>
#include <fs/ext2fs/ext2fs.h>
#include <fs/ext2fs/ext2_mount.h>
#include <fs/ext2fs/ext2_dinode.h>
#include <fs/ext2fs/ext2_extents.h>
#include <fs/ext2fs/ext2_extern.h>
#include <fs/ext2fs/fs.h>
#include <fs/ext2fs/ext2_apple.h>

/*
 * Vnode op for reading.
 */
int
ext2_read(struct vnop_read_args *ap)
{
	struct vnode *vp = ap->a_vp;
	struct inode *ip = VTOI(vp);
	struct uio *uio = ap->a_uio;
	int error;

	if (vnode_isdir(vp))
		return (EISDIR);
	if (!vnode_isreg(vp))
		return (EPERM);
	if (uio_offset(uio) < 0)
		return (EINVAL);
	if (uio_resid(uio) == 0)
		return (0);

	/*
	 * cluster_read() takes the file's current size so it knows where to
	 * stop, and reaches the disk through VNOP_BLOCKMAP. A hole comes back
	 * from blockmap as a -1 block number and the cluster layer zero-fills
	 * it, which is why nothing here has to recognise sparse files.
	 */
	error = cluster_read(vp, uio, (off_t)ip->i_size, ap->a_ioflag);
	if (error == 0) {
		EXT2_ILOCK(ip);
		ip->i_flag |= IN_ACCESS;
		EXT2_IUNLOCK(ip);
	}

	return (error);
}

/*
 * Make sure every block the range [offset, offset + len) falls in has been
 * allocated.
 *
 * The cluster layer writes through pages, and by the time it asks
 * VNOP_BLOCKMAP where a page belongs it is too late to allocate - blockmap
 * reports a mapping, it does not create one. So the blocks are allocated up
 * front, here, and the buffers ext2_balloc() hands back are released
 * immediately: their contents will be supplied by the cluster layer, and the
 * only thing wanted from balloc is the allocation and the block pointers it
 * writes into the inode.
 */
static int
ext2_extend_alloc(struct inode *ip, off_t offset, off_t len, struct ucred *cred)
{
	struct m_ext2fs *fs = ip->i_e2fs;
	buf_t bp;
	e2fs_lbn_t lbn, endlbn;
	int error, size;

	if (len <= 0)
		return (0);

	lbn = lblkno(fs, offset);
	endlbn = lblkno(fs, offset + len - 1);

	for (; lbn <= endlbn; lbn++) {
		/*
		 * BA_CLRBUF is deliberately not passed: the cluster layer
		 * fills these pages itself, so zeroing them here would be
		 * work thrown away. A partial first or last block is handled
		 * by cluster_write(), which reads it in before modifying it.
		 */
		size = (int)blksize(fs, ip, lbn);
		error = ext2_balloc(ip, lbn, size, cred, &bp, 0);
		if (error != 0)
			return (error);
		if (bp != NULL)
			buf_brelse(bp);
	}

	return (0);
}

/*
 * Vnode op for writing.
 */
int
ext2_write(struct vnop_write_args *ap)
{
	struct vnode *vp = ap->a_vp;
	struct inode *ip = VTOI(vp);
	struct uio *uio = ap->a_uio;
	struct m_ext2fs *fs = ip->i_e2fs;
	struct ucred *cred = vfs_context_ucred(ap->a_context);
	off_t offset, resid, oldEOF, newEOF;
	off_t headOff, tailOff;
	int ioflag = ap->a_ioflag;
	int error;

	if (vnode_isdir(vp))
		return (EISDIR);
	if (!vnode_isreg(vp))
		return (EPERM);

	if (ioflag & IO_APPEND)
		uio_setoffset(uio, (off_t)ip->i_size);
	offset = uio_offset(uio);
	resid = uio_resid(uio);

	if (offset < 0)
		return (EINVAL);
	if (resid == 0)
		return (0);
	if ((off_t)(offset + resid) > (off_t)fs->e2fs_maxfilesize)
		return (EFBIG);

	oldEOF = (off_t)ip->i_size;
	newEOF = MAX(oldEOF, offset + resid);

	/*
	 * Allocate first, then let the cluster layer move the data. If the
	 * allocation fails partway the file keeps whatever blocks were added -
	 * they are accounted for in the inode - but i_size is not advanced, so
	 * nothing beyond the old end of file becomes visible.
	 */
	error = ext2_extend_alloc(ip, offset, resid, cred);
	if (error != 0)
		return (error);

	if (newEOF > oldEOF) {
		ip->i_size = (uint64_t)newEOF;
		ubc_setsize(vp, newEOF);
	}

	/*
	 * headOff and tailOff mark the block-aligned edges of the write, so
	 * the cluster layer can read in the partial blocks at either end
	 * before modifying them rather than writing back uninitialised bytes.
	 */
	headOff = (off_t)(offset & ~((off_t)fs->e2fs_qbmask));
	tailOff = (off_t)roundup2(offset + resid, (off_t)fs->e2fs_bsize);

	error = cluster_write(vp, uio, oldEOF, newEOF, headOff, tailOff,
	    ioflag);
	if (error != 0) {
		/*
		 * The write failed after the file was grown. Put i_size back
		 * so the caller does not see a file that claims to hold data
		 * that was never written; the blocks stay allocated and are
		 * reclaimed by a later truncate.
		 */
		if (newEOF > oldEOF) {
			ip->i_size = (uint64_t)oldEOF;
			ubc_setsize(vp, oldEOF);
		}
		return (error);
	}

	EXT2_ILOCK(ip);
	ip->i_flag |= IN_CHANGE | IN_UPDATE;
	EXT2_IUNLOCK(ip);

	if (ioflag & IO_SYNC)
		error = ext2_update(vp, 1);

	return (error);
}

/*
 * Read pages in on behalf of the pager.
 *
 * FreeBSD pointed vop_getpages at vnode_pager_local_getpages, which did this
 * generically. XNU expects the file system to call the cluster layer, which
 * again finds the blocks through VNOP_BLOCKMAP.
 */
int
ext2_pagein(struct vnop_pagein_args *ap)
{
	struct vnode *vp = ap->a_vp;
	struct inode *ip = VTOI(vp);

	return (cluster_pagein(vp, ap->a_pl, ap->a_pl_offset, ap->a_f_offset,
	    (int)ap->a_size, (off_t)ip->i_size, ap->a_flags));
}

/*
 * Write pages out on behalf of the pager.
 *
 * The pager can reach this for a page whose block is already allocated - the
 * write path allocates before the data is ever paged out - so there is no
 * allocation to do here, only the I/O.
 */
int
ext2_pageout(struct vnop_pageout_args *ap)
{
	struct vnode *vp = ap->a_vp;
	struct inode *ip = VTOI(vp);

	if (vfs_flags(vnode_mount(vp)) & MNT_RDONLY)
		return (EROFS);

	return (cluster_pageout(vp, ap->a_pl, ap->a_pl_offset, ap->a_f_offset,
	    (int)ap->a_size, (off_t)ip->i_size, ap->a_flags));
}
