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
 * The file data path: read, write and the extent and indirect block readers
 * behind them. Split out of ext2_vnops.c, which keeps the metadata
 * operations, so the two can be reviewed apart; ext2fsx kept the same
 * division, as gnu/ext2fs/ext2_readwrite.c.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/buf.h>
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
static int
ext2_read(struct vop_read_args *ap)
{
	struct vnode *vp;
	struct inode *ip;
	int error;

	vp = ap->a_vp;
	ip = VTOI(vp);

	/*EXT4_EXT_LOCK(ip);*/
	if (ip->i_flag & IN_E4EXTENTS)
		error = ext4_ext_read(ap);
	else
		error = ext2_ind_read(ap);
	/*EXT4_EXT_UNLOCK(ip);*/
	return (error);
}

/*
 * Vnode op for reading.
 */
static int
ext2_ind_read(struct vop_read_args *ap)
{
	struct vnode *vp;
	struct inode *ip;
	struct uio *uio;
	struct m_ext2fs *fs;
	struct buf *bp;
	daddr64_t lbn, nextlbn;
	off_t bytesinfile;
	long size, xfersize, blkoffset;
	int error, orig_resid, seqcount;
	int ioflag;

	vp = ap->a_vp;
	uio = ap->a_uio;
	ioflag = ap->a_ioflag;

	seqcount = ap->a_ioflag >> IO_SEQSHIFT;
	ip = VTOI(vp);

#ifdef INVARIANTS
	if (uio->uio_rw != UIO_READ)
		panic("%s: mode", "ext2_read");

	if (vp->v_type == VLNK) {
		if ((int)ip->i_size < vp->v_mount->mnt_maxsymlinklen)
			panic("%s: short symlink", "ext2_read");
	} else if (vp->v_type != VREG && vp->v_type != VDIR)
		panic("%s: type %d", "ext2_read", vp->v_type);
#endif
	orig_resid = uio->uio_resid;
	KASSERT(orig_resid >= 0, ("ext2_read: uio->uio_resid < 0"));
	if (orig_resid == 0)
		return (0);
	KASSERT(uio->uio_offset >= 0, ("ext2_read: uio->uio_offset < 0"));
	fs = ip->i_e2fs;
	if (uio->uio_offset < ip->i_size &&
	    uio->uio_offset >= fs->e2fs_maxfilesize)
	    	return (EOVERFLOW);

	for (error = 0, bp = NULL; uio->uio_resid > 0; bp = NULL) {
		if ((bytesinfile = ip->i_size - uio->uio_offset) <= 0)
			break;
		lbn = lblkno(fs, uio->uio_offset);
		nextlbn = lbn + 1;
		size = blksize(fs, ip, lbn);
		blkoffset = blkoff(fs, uio->uio_offset);

		xfersize = fs->e2fs_fsize - blkoffset;
		if (uio->uio_resid < xfersize)
			xfersize = uio->uio_resid;
		if (bytesinfile < xfersize)
			xfersize = bytesinfile;

		if (lblktosize(fs, nextlbn) >= ip->i_size)
			error = bread(vp, lbn, size, NOCRED, &bp);
		else if ((vp->v_mount->mnt_flag & MNT_NOCLUSTERR) == 0) {
			error = cluster_read(vp, ip->i_size, lbn, size,
			    NOCRED, blkoffset + uio->uio_resid, seqcount,
			    0, &bp);
		} else if (seqcount > 1) {
			u_int nextsize = blksize(fs, ip, nextlbn);
			error = breadn(vp, lbn,
			    size, &nextlbn, &nextsize, 1, NOCRED, &bp);
		} else
			error = bread(vp, lbn, size, NOCRED, &bp);
		if (error) {
			brelse(bp);
			bp = NULL;
			break;
		}

		/*
		 * If IO_DIRECT then set B_DIRECT for the buffer.  This
		 * will cause us to attempt to release the buffer later on
		 * and will cause the buffer cache to attempt to free the
		 * underlying pages.
		 */
		if (ioflag & IO_DIRECT)
			bp->b_flags |= B_DIRECT;

		/*
		 * We should only get non-zero b_resid when an I/O error
		 * has occurred, which should cause us to break above.
		 * However, if the short read did not cause an error,
		 * then we want to ensure that we do not uiomove bad
		 * or uninitialized data.
		 */
		size -= bp->b_resid;
		if (size < xfersize) {
			if (size == 0)
				break;
			xfersize = size;
		}
		error = uiomove((char *)bp->b_data + blkoffset,
			(int)xfersize, uio);
		if (error)
			break;

		if (ioflag & (IO_VMIO|IO_DIRECT)) {
			/*
			 * If it's VMIO or direct I/O, then we don't
			 * need the buf, mark it available for
			 * freeing. If it's non-direct VMIO, the VM has
			 * the data.
			 */
			bp->b_flags |= B_RELBUF;
			brelse(bp);
		} else {
			/*
			 * Otherwise let whoever
			 * made the request take care of
			 * freeing it. We just queue
			 * it onto another list.
			 */
			bqrelse(bp);
		}
	}

	/* 
	 * This can only happen in the case of an error
	 * because the loop above resets bp to NULL on each iteration
	 * and on normal completion has not set a new value into it.
	 * so it must have come from a 'break' statement
	 */
	if (bp != NULL) {
		if (ioflag & (IO_VMIO|IO_DIRECT)) {
			bp->b_flags |= B_RELBUF;
			brelse(bp);
		} else {
			bqrelse(bp);
		}
	}

	if ((error == 0 || uio->uio_resid != orig_resid) &&
	    (vp->v_mount->mnt_flag & (MNT_NOATIME | MNT_RDONLY)) == 0)
		ip->i_flag |= IN_ACCESS;
	return (error);
}

/*
 * this function handles ext4 extents block mapping
 */
static int
ext4_ext_read(struct vop_read_args *ap)
{
	struct vnode *vp;
	struct inode *ip;
	struct uio *uio;
	struct m_ext2fs *fs;
	struct buf *bp;
	struct ext4_extent nex, *ep;
	struct ext4_extent_path path;
	daddr64_t lbn, newblk;
	off_t bytesinfile;
	int cache_type;
	ssize_t orig_resid;
	int error;
	long size, xfersize, blkoffset;

	vp = ap->a_vp;
	ip = VTOI(vp);
	uio = ap->a_uio;
	memset(&path, 0, sizeof(path));

	orig_resid = uio->uio_resid;
	KASSERT(orig_resid >= 0, ("%s: uio->uio_resid < 0", __func__));
	if (orig_resid == 0)
		return (0);
	KASSERT(uio->uio_offset >= 0, ("%s: uio->uio_offset < 0", __func__));
	fs = ip->i_e2fs;
	if (uio->uio_offset < ip->i_size && uio->uio_offset >= fs->e2fs_maxfilesize)
		return (EOVERFLOW);

	while (uio->uio_resid > 0) {
		if ((bytesinfile = ip->i_size - uio->uio_offset) <= 0)
			break;
		lbn = lblkno(fs, uio->uio_offset);
		size = blksize(fs, ip, lbn);
		blkoffset = blkoff(fs, uio->uio_offset);

		xfersize = fs->e2fs_fsize - blkoffset;
		xfersize = MIN(xfersize, uio->uio_resid);
		xfersize = MIN(xfersize, bytesinfile);

		/* get block from ext4 extent cache */
		cache_type = ext4_ext_in_cache(ip, lbn, &nex);
		switch (cache_type) {
		case EXT4_EXT_CACHE_NO:
			ext4_ext_find_extent(fs, ip, lbn, &path);
			ep = path.ep_ext;
			if (ep == NULL)
				return (EIO);

			ext4_ext_put_cache(ip, ep, EXT4_EXT_CACHE_IN);

			newblk = lbn - ep->e_blk + (ep->e_start_lo |
			    (daddr64_t)ep->e_start_hi << 32);

			if (path.ep_bp != NULL) {
				brelse(path.ep_bp);
				path.ep_bp = NULL;
			}
			break;

		case EXT4_EXT_CACHE_GAP:
			/* block has not been allocated yet */
			return (0);

		case EXT4_EXT_CACHE_IN:
			newblk = lbn - nex.e_blk + (nex.e_start_lo |
			    (daddr64_t)nex.e_start_hi << 32);
			break;

		default:
			panic("%s: invalid cache type", __func__);
		}

		error = bread(ip->i_devvp, fsbtodb(fs, newblk), size, NOCRED, &bp);
		if (error) {
			brelse(bp);
			return (error);
		}

		size -= bp->b_resid;
		if (size < xfersize) {
			if (size == 0) {
				bqrelse(bp);
				break;
			}
			xfersize = size;
		}
		error = uiomove(bp->b_data + blkoffset, (int)xfersize, uio);
		bqrelse(bp);
		if (error)
			return (error);
	}

	return (0);
}

/*
 * Vnode op for writing.
 */
static int
ext2_write(struct vop_write_args *ap)
{
	struct vnode *vp;
	struct uio *uio;
	struct inode *ip;
	struct m_ext2fs *fs;
	struct buf *bp;
	daddr64_t lbn;
	off_t osize;
	int blkoffset, error, flags, ioflag, resid, size, seqcount, xfersize;

	ioflag = ap->a_ioflag;
	uio = ap->a_uio;
	vp = ap->a_vp;

	seqcount = ioflag >> IO_SEQSHIFT;
	ip = VTOI(vp);

#ifdef INVARIANTS
	if (uio->uio_rw != UIO_WRITE)
		panic("%s: mode", "ext2_write");
#endif

	switch (vp->v_type) {
	case VREG:
		if (ioflag & IO_APPEND)
			uio->uio_offset = ip->i_size;
		if ((ip->i_flags & APPEND) && uio->uio_offset != ip->i_size)
			return (EPERM);
		/* FALLTHROUGH */
	case VLNK:
		break;
	case VDIR:
		/* XXX differs from ffs -- this is called from ext2_mkdir(). */
		if ((ioflag & IO_SYNC) == 0)
		panic("ext2_write: nonsync dir write");
		break;
	default:
		panic("ext2_write: type %p %d (%jd,%jd)", (void *)vp,
		    vp->v_type, (intmax_t)uio->uio_offset,
		    (intmax_t)uio->uio_resid);
	}

	KASSERT(uio->uio_resid >= 0, ("ext2_write: uio->uio_resid < 0"));
	KASSERT(uio->uio_offset >= 0, ("ext2_write: uio->uio_offset < 0"));
	fs = ip->i_e2fs;
	if ((uoff_t)uio->uio_offset + uio->uio_resid > fs->e2fs_maxfilesize)
		return (EFBIG);
	/*
	 * Maybe this should be above the vnode op call, but so long as
	 * file servers have no limits, I don't think it matters.
	 */
	if (vn_rlimit_fsize(vp, uio, uio->uio_td))
		return (EFBIG);

	resid = uio->uio_resid;
	osize = ip->i_size;
	if (seqcount > BA_SEQMAX)
		flags = BA_SEQMAX << BA_SEQSHIFT;
	else
		flags = seqcount << BA_SEQSHIFT;
	if ((ioflag & IO_SYNC) && !DOINGASYNC(vp))
		flags |= IO_SYNC;

	for (error = 0; uio->uio_resid > 0;) {
		lbn = lblkno(fs, uio->uio_offset);
		blkoffset = blkoff(fs, uio->uio_offset);
		xfersize = fs->e2fs_fsize - blkoffset;
		if (uio->uio_resid < xfersize)
			xfersize = uio->uio_resid;
		if (uio->uio_offset + xfersize > ip->i_size)
			vnode_pager_setsize(vp, uio->uio_offset + xfersize);

		/*
		 * We must perform a read-before-write if the transfer size
		 * does not cover the entire buffer.
		 */
		if (fs->e2fs_bsize > xfersize)
			flags |= BA_CLRBUF;
		else
			flags &= ~BA_CLRBUF;
		error = ext2_balloc(ip, lbn, blkoffset + xfersize,
		    ap->a_cred, &bp, flags);
		if (error != 0)
			break;

		if ((ioflag & (IO_SYNC|IO_INVAL)) == (IO_SYNC|IO_INVAL))
			bp->b_flags |= B_NOCACHE;
		if (uio->uio_offset + xfersize > ip->i_size)
			ip->i_size = uio->uio_offset + xfersize;
		size = blksize(fs, ip, lbn) - bp->b_resid;
		if (size < xfersize)
			xfersize = size;

		error =
		    uiomove((char *)bp->b_data + blkoffset, (int)xfersize, uio);
		/*
		 * If the buffer is not already filled and we encounter an
		 * error while trying to fill it, we have to clear out any
		 * garbage data from the pages instantiated for the buffer.
		 * If we do not, a failed uiomove() during a write can leave
		 * the prior contents of the pages exposed to a userland mmap.
		 *
		 * Note that we need only clear buffers with a transfer size
		 * equal to the block size because buffers with a shorter
		 * transfer size were cleared above by the call to ext2_balloc()
		 * with the BA_CLRBUF flag set.
		 *
		 * If the source region for uiomove identically mmaps the
		 * buffer, uiomove() performed the NOP copy, and the buffer
		 * content remains valid because the page fault handler
		 * validated the pages.
		 */
		if (error != 0 && (bp->b_flags & B_CACHE) == 0 &&
		    fs->e2fs_bsize == xfersize)
			vfs_bio_clrbuf(bp);
		if (ioflag & (IO_VMIO|IO_DIRECT)) {
			bp->b_flags |= B_RELBUF;
		}

		/*
		 * If IO_SYNC each buffer is written synchronously.  Otherwise
		 * if we have a severe page deficiency write the buffer
		 * asynchronously.  Otherwise try to cluster, and if that
		 * doesn't do it then either do an async write (if O_DIRECT),
		 * or a delayed write (if not).
		 */
		if (ioflag & IO_SYNC) {
			(void)bwrite(bp);
		} else if (vm_page_count_severe() ||
		    buf_dirty_count_severe() ||
		    (ioflag & IO_ASYNC)) {
			bp->b_flags |= B_CLUSTEROK;
			bawrite(bp);
		} else if (xfersize + blkoffset == fs->e2fs_fsize) {
			if ((vp->v_mount->mnt_flag & MNT_NOCLUSTERW) == 0) {
				bp->b_flags |= B_CLUSTEROK;
				cluster_write(vp, bp, ip->i_size, seqcount, 0);
			} else {
				bawrite(bp);
			}
		} else if (ioflag & IO_DIRECT) {
			bp->b_flags |= B_CLUSTEROK;
			bawrite(bp);
		} else {
			bp->b_flags |= B_CLUSTEROK;
			bdwrite(bp);
		}
		if (error || xfersize == 0)
			break;
	}
	/*
	 * If we successfully wrote any data, and we are not the superuser
	 * we clear the setuid and setgid bits as a precaution against
	 * tampering.
	 */
	if ((ip->i_mode & (ISUID | ISGID)) && resid > uio->uio_resid &&
	    ap->a_cred) {
		if (priv_check_cred(ap->a_cred, PRIV_VFS_RETAINSUGID, 0))
			ip->i_mode &= ~(ISUID | ISGID);
	}
	if (error) {
		if (ioflag & IO_UNIT) {
			(void)ext2_truncate(vp, osize,
			    ioflag & IO_SYNC, ap->a_cred, uio->uio_td);
			uio->uio_offset -= resid - uio->uio_resid;
			uio->uio_resid = resid;
		}
	}
	if (uio->uio_resid != resid) {
		ip->i_flag |= IN_CHANGE | IN_UPDATE;
		if (ioflag & IO_SYNC)
			error = ext2_update(vp, 1);
	}
	return (error);
}
