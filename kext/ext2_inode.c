/*-
 *  modified for Lites 1.1
 *
 *  Aug 1995, Godmar Back (gback@cs.utah.edu)
 *  University of Utah, Department of Computer Science
 */
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
 *	@(#)ffs_inode.c	8.5 (Berkeley) 12/30/93
 * $FreeBSD$
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/mount.h>
#include <sys/buf.h>
#include <sys/vnode.h>
#include <sys/malloc.h>

#include <sys/ubc.h>

#include <fs/ext2fs/ext2_dinode.h>
#include <fs/ext2fs/inode.h>
#include <fs/ext2fs/ext2_mount.h>
#include <fs/ext2fs/ext2fs.h>
#include <fs/ext2fs/fs.h>
#include <fs/ext2fs/ext2_extern.h>
#include <fs/ext2fs/ext2_apple.h>

static int ext2_indirtrunc(struct inode *, daddr64_t, daddr64_t,
	    daddr64_t, int, e4fs_daddr_t *);

/*
 * Update the access, modified, and inode change times as specified by the
 * IN_ACCESS, IN_UPDATE, and IN_CHANGE flags respectively.  Write the inode
 * to disk if the IN_MODIFIED flag is set (it may be set initially, or by
 * the timestamp update).  The IN_LAZYMOD flag is set to force a write
 * later if not now.  If we write now, then clear both IN_MODIFIED and
 * IN_LAZYMOD to reflect the presumably successful write, and if waitfor is
 * set, then wait for the write to complete.
 */
int
ext2_update(struct vnode *vp, int waitfor)
{
	struct m_ext2fs *fs;
	struct buf *bp;
	struct inode *ip;
	int error;

	/*
	 * FreeBSD asserted the vnode was exclusively locked here. XNU does not
	 * expose vnode lock state, and its vnode layer serialises the paths
	 * that reach this on its own.
	 */
	ext2_itimes(vp);
	ip = VTOI(vp);
	if ((ip->i_flag & IN_MODIFIED) == 0 && waitfor == 0)
		return (0);
	fs = ip->i_e2fs;
	/*
	 * Test this before clearing the flags, not after. Clearing them first
	 * and then returning without writing would tell the rest of the file
	 * system the inode had been written back when it had not - harmless
	 * while the mount stays read-only, and a silently lost update the
	 * moment it is upgraded to read-write.
	 */
	if (fs->e2fs_ronly)
		return (0);
	ip->i_flag &= ~(IN_LAZYACCESS | IN_LAZYMOD | IN_MODIFIED);
	error = buf_meta_bread(ip->i_devvp,
	    (daddr64_t)fsbtodb(fs, ino_to_fsba(fs, ip->i_number)),
	    (int)fs->e2fs_bsize, NOCRED, &bp);
	if (error != 0) {
		if (bp != NULL)
			buf_brelse(bp);
		return (error);
	}
	ext2_i2ei(ip, (struct ext2fs_dinode *)((char *)buf_dataptr(bp) +
	    EXT2_INODE_SIZE(fs) * ino_to_fsbo(fs, ip->i_number)));
	if (waitfor && !DOINGASYNC(vp))
		return (buf_bwrite(bp));
	else {
		buf_bdwrite(bp);
		return (0);
	}
}

#define	SINGLE	0	/* index of single indirect block */
#define	DOUBLE	1	/* index of double indirect block */
#define	TRIPLE	2	/* index of triple indirect block */
/*
 * Truncate the inode oip to at most length size, freeing the
 * disk blocks.
 */
int
ext2_truncate(struct vnode *vp, off_t length, int flags, struct ucred *cred,
    struct thread *td)
{
	struct vnode *ovp = vp;
	int32_t lastblock;
	struct inode *oip;
	int32_t bn, lbn, lastiblock[NIADDR], indir_lbn[NIADDR];
	uint32_t oldblks[NDADDR + NIADDR], newblks[NDADDR + NIADDR];
	struct m_ext2fs *fs;
	struct buf *bp;
	int offset, size, level;
	e4fs_daddr_t count, nblocks, blocksreleased = 0;
	int error, i, allerror;
	off_t osize;
#ifdef INVARIANTS
	struct bufobj *bo;
#endif

	oip = VTOI(ovp);
#ifdef INVARIANTS
	bo = &ovp->v_bufobj;
#endif

	/*
	 * FreeBSD asserted the vnode was locked here; XNU does not expose
	 * vnode lock state.
	 */

	if (length < 0)
	    return (EINVAL);

	if (vnode_vtype(ovp) == VLNK && oip->i_size < EXT2_MAXSYMLINKLEN) {
#ifdef INVARIANTS
		if (length != 0)
			panic("ext2_truncate: partial truncate of symlink");
#endif
		bzero((char *)&oip->i_shortlink, (u_int)oip->i_size);
		oip->i_size = 0;
		oip->i_flag |= IN_CHANGE | IN_UPDATE;
		return (ext2_update(ovp, 1));
	}
	if (oip->i_size == (uint64_t)length) {
		oip->i_flag |= IN_CHANGE | IN_UPDATE;
		return (ext2_update(ovp, 0));
	}
	fs = oip->i_e2fs;
	osize = oip->i_size;
	/*
	 * Lengthen the size of the file. We must ensure that the
	 * last byte of the file is allocated. Since the smallest
	 * value of osize is 0, length will be at least 1.
	 */
	if (osize < length) {
		if (length > oip->i_e2fs->e2fs_maxfilesize)
			return (EFBIG);
		ubc_setsize(ovp, length);
		offset = blkoff(fs, length - 1);
		lbn = lblkno(fs, length - 1);
		flags |= BA_CLRBUF;
		error = ext2_balloc(oip, lbn, offset + 1, cred, &bp, flags);
		if (error) {
			ubc_setsize(vp, osize);
			return (error);
		}
		oip->i_size = length;
		/*
		 * FreeBSD marked a full-sized buffer B_CLUSTEROK so its
		 * cluster writer could coalesce it. XNU's cluster layer has
		 * no such flag - it works from the blockmap - so only the
		 * sync policy carries over.
		 */
		if (flags & IO_SYNC)
			buf_bwrite(bp);
		else if (DOINGASYNC(ovp))
			buf_bdwrite(bp);
		else
			buf_bawrite(bp);
		oip->i_flag |= IN_CHANGE | IN_UPDATE;
		return (ext2_update(ovp, !DOINGASYNC(ovp)));
	}
	/*
	 * Shorten the size of the file. If the file is not being
	 * truncated to a block boundry, the contents of the
	 * partial block following the end of the file must be
	 * zero'ed in case it ever become accessible again because
	 * of subsequent file growth.
	 */
	/* I don't understand the comment above */
	offset = blkoff(fs, length);
	if (offset == 0) {
		oip->i_size = length;
	} else {
		lbn = lblkno(fs, length);
		flags |= BA_CLRBUF;
		error = ext2_balloc(oip, lbn, offset, cred, &bp, flags);
		if (error)
			return (error);
		oip->i_size = length;
		size = blksize(fs, oip, lbn);
		bzero((char *)buf_dataptr(bp) + offset, (u_int)(size - offset));
		/*
		 * FreeBSD's allocbuf() resized the buffer's storage; XNU's
		 * buf_setsize() records the new valid length of a buffer that
		 * is already large enough, which is all that is wanted here -
		 * size is never more than the block the buffer was got for.
		 */
		buf_setsize(bp, size);
		/*
		 * FreeBSD marked a full-sized buffer B_CLUSTEROK so its
		 * cluster writer could coalesce it. XNU's cluster layer has
		 * no such flag - it works from the blockmap - so only the
		 * sync policy carries over.
		 */
		if (flags & IO_SYNC)
			buf_bwrite(bp);
		else if (DOINGASYNC(ovp))
			buf_bdwrite(bp);
		else
			buf_bawrite(bp);
	}
	/*
	 * Calculate index into inode's block list of
	 * last direct and indirect blocks (if any)
	 * which we want to keep.  Lastblock is -1 when
	 * the file is truncated to 0.
	 */
	lastblock = lblkno(fs, length + fs->e2fs_bsize - 1) - 1;
	lastiblock[SINGLE] = lastblock - NDADDR;
	lastiblock[DOUBLE] = lastiblock[SINGLE] - NINDIR(fs);
	lastiblock[TRIPLE] = lastiblock[DOUBLE] - NINDIR(fs) * NINDIR(fs);
	nblocks = btodb(fs->e2fs_bsize);
	/*
	 * Update file and block pointers on disk before we start freeing
	 * blocks.  If we crash before free'ing blocks below, the blocks
	 * will be returned to the free list.  lastiblock values are also
	 * normalized to -1 for calls to ext2_indirtrunc below.
	 */
	for (level = TRIPLE; level >= SINGLE; level--) {
		oldblks[NDADDR + level] = oip->i_ib[level];
		if (lastiblock[level] < 0) {
			oip->i_ib[level] = 0;
			lastiblock[level] = -1;
		}
	}
	for (i = 0; i < NDADDR; i++) {
		oldblks[i] = oip->i_db[i];
		if (i > lastblock)
			oip->i_db[i] = 0;
	}
	oip->i_flag |= IN_CHANGE | IN_UPDATE;
	allerror = ext2_update(ovp, !DOINGASYNC(ovp));

	/*
	 * Having written the new inode to disk, save its new configuration
	 * and put back the old block pointers long enough to process them.
	 * Note that we save the new block configuration so we can check it
	 * when we are done.
	 */
	for (i = 0; i < NDADDR; i++) {
		newblks[i] = oip->i_db[i];
		oip->i_db[i] = oldblks[i];
	}
	for (i = 0; i < NIADDR; i++) {
		newblks[NDADDR + i] = oip->i_ib[i];
		oip->i_ib[i] = oldblks[NDADDR + i];
	}
	oip->i_size = osize;
	/*
	 * Discard whatever is cached beyond the new end of file. FreeBSD's
	 * vtruncbuf() took a length and trimmed from there; XNU splits the job:
	 * ubc_setsize() drops the file's pages past the new size, and
	 * buf_invalidateblks() clears the metadata buffers held on this vnode,
	 * flushing dirty ones first so nothing is lost that belongs below the
	 * truncation point.
	 */
	error = buf_invalidateblks(ovp, BUF_WRITE_DATA, 0, 0);
	if (error && (allerror == 0))
		allerror = error;
	ubc_setsize(ovp, length);

	/*
	 * Indirect blocks first.
	 */
	indir_lbn[SINGLE] = -NDADDR;
	indir_lbn[DOUBLE] = indir_lbn[SINGLE] - NINDIR(fs) - 1;
	indir_lbn[TRIPLE] = indir_lbn[DOUBLE] - NINDIR(fs) * NINDIR(fs) - 1;
	for (level = TRIPLE; level >= SINGLE; level--) {
		bn = oip->i_ib[level];
		if (bn != 0) {
			error = ext2_indirtrunc(oip, indir_lbn[level],
			    fsbtodb(fs, bn), lastiblock[level], level, &count);
			if (error)
				allerror = error;
			blocksreleased += count;
			if (lastiblock[level] < 0) {
				oip->i_ib[level] = 0;
				ext2_blkfree(oip, bn, fs->e2fs_fsize);
				blocksreleased += nblocks;
			}
		}
		if (lastiblock[level] >= 0)
			goto done;
	}

	/*
	 * All whole direct blocks or frags.
	 */
	for (i = NDADDR - 1; i > lastblock; i--) {
		long bsize;

		bn = oip->i_db[i];
		if (bn == 0)
			continue;
		oip->i_db[i] = 0;
		bsize = blksize(fs, oip, i);
		ext2_blkfree(oip, bn, bsize);
		blocksreleased += btodb(bsize);
	}
	if (lastblock < 0)
		goto done;

	/*
	 * Finally, look for a change in size of the
	 * last direct block; release any frags.
	 */
	bn = oip->i_db[lastblock];
	if (bn != 0) {
		long oldspace, newspace;

		/*
		 * Calculate amount of space we're giving
		 * back as old block size minus new block size.
		 */
		oldspace = blksize(fs, oip, lastblock);
		oip->i_size = length;
		newspace = blksize(fs, oip, lastblock);
		if (newspace == 0)
			panic("ext2_truncate: newspace");
		if (oldspace - newspace > 0) {
			/*
			 * Block number of space to be free'd is
			 * the old block # plus the number of frags
			 * required for the storage we're keeping.
			 */
			bn += numfrags(fs, newspace);
			ext2_blkfree(oip, bn, oldspace - newspace);
			blocksreleased += btodb(oldspace - newspace);
		}
	}
done:
#ifdef INVARIANTS
	for (level = SINGLE; level <= TRIPLE; level++)
		if (newblks[NDADDR + level] != oip->i_ib[level])
			panic("itrunc1");
	for (i = 0; i < NDADDR; i++)
		if (newblks[i] != oip->i_db[i])
			panic("itrunc2");
	BO_LOCK(bo);
	if (length == 0 && (bo->bo_dirty.bv_cnt != 0 ||
	    bo->bo_clean.bv_cnt != 0))
		panic("itrunc3");
	BO_UNLOCK(bo);
#endif /* INVARIANTS */
	/*
	 * Put back the real size.
	 */
	oip->i_size = length;
	if (oip->i_blocks >= (uint64_t)blocksreleased)
		oip->i_blocks -= blocksreleased;
	else				/* sanity */
		oip->i_blocks = 0;
	oip->i_flag |= IN_CHANGE;
	ubc_setsize(ovp, length);
	return (allerror);
}

/*
 * Release blocks associated with the inode ip and stored in the indirect
 * block bn.  Blocks are free'd in LIFO order up to (but not including)
 * lastbn.  If level is greater than SINGLE, the block is an indirect block
 * and recursive calls to indirtrunc must be used to cleanse other indirect
 * blocks.
 *
 * NB: triple indirect blocks are untested.
 */

static int
ext2_indirtrunc(struct inode *ip, daddr64_t lbn, daddr64_t dbn,
    daddr64_t lastbn, int level, e4fs_daddr_t *countp)
{
	struct buf *bp;
	struct m_ext2fs *fs = ip->i_e2fs;
	struct vnode *vp;
	e2fs_daddr_t *bap, *copy;
	int i, nblocks, error = 0, allerror = 0;
	e2fs_lbn_t nb, nlbn, last;
	e4fs_daddr_t blkcount, factor, blocksreleased = 0;

	/*
	 * Calculate index in current block of last
	 * block to be kept.  -1 indicates the entire
	 * block so we need not calculate the index.
	 */
	factor = 1;
	for (i = SINGLE; i < level; i++)
		factor *= NINDIR(fs);
	last = lastbn;
	if (lastbn > 0)
		last /= factor;
	nblocks = btodb(fs->e2fs_bsize);
	/*
	 * Get buffer of block pointers, zero those entries corresponding
	 * to blocks to be free'd, and update on disk copy first.  Since
	 * double(triple) indirect before single(double) indirect, calls
	 * to bmap on these blocks will fail.  However, we already have
	 * the on disk address, so we have to set the b_blkno field
	 * explicitly instead of letting bread do everything for us.
	 */
	/*
	 * The comment above explains why FreeBSD drove the read by hand: it
	 * cached indirect blocks on the file's vnode by logical block, and
	 * bmap could not translate them once the levels above had been freed,
	 * so it filled in b_blkno itself. Here indirect blocks are already
	 * addressed physically on the device vnode - dbn is that address - so
	 * an ordinary metadata read does the same job.
	 */
	vp = ITOV(ip);
	error = buf_meta_bread(ip->i_devvp, dbn, (int)fs->e2fs_bsize, NOCRED,
	    &bp);
	if (error) {
		if (bp != NULL)
			buf_brelse(bp);
		*countp = 0;
		return (error);
	}

	bap = (e2fs_daddr_t *)buf_dataptr(bp);
	copy = malloc(fs->e2fs_bsize, M_TEMP, M_WAITOK);
	bcopy((caddr_t)bap, (caddr_t)copy, (u_int)fs->e2fs_bsize);
	bzero((caddr_t)&bap[last + 1],
	  (NINDIR(fs) - (last + 1)) * sizeof(e2fs_daddr_t));
	/*
	 * With no entries left the block is about to be freed, so there is no
	 * point writing it back; mark it invalid and let the release drop it.
	 */
	if (last == -1)
		buf_markinvalid(bp);
	if (DOINGASYNC(vp)) {
		buf_bdwrite(bp);
	} else {
		error = buf_bwrite(bp);
		if (error)
			allerror = error;
	}
	bap = copy;

	/*
	 * Recursively free totally unused blocks.
	 */
	for (i = NINDIR(fs) - 1, nlbn = lbn + 1 - i * factor; i > last;
	    i--, nlbn += factor) {
		nb = bap[i];
		if (nb == 0)
			continue;
		if (level > SINGLE) {
			if ((error = ext2_indirtrunc(ip, nlbn,
			    fsbtodb(fs, nb), (int32_t)-1, level - 1, &blkcount)) != 0)
				allerror = error;
			blocksreleased += blkcount;
		}
		ext2_blkfree(ip, nb, fs->e2fs_bsize);
		blocksreleased += nblocks;
	}

	/*
	 * Recursively free last partial block.
	 */
	if (level > SINGLE && lastbn >= 0) {
		last = lastbn % factor;
		nb = bap[i];
		if (nb != 0) {
			if ((error = ext2_indirtrunc(ip, nlbn, fsbtodb(fs, nb),
			    last, level - 1, &blkcount)) != 0)
				allerror = error;
			blocksreleased += blkcount;
		}
	}
	free(copy, M_TEMP);
	*countp = blocksreleased;
	return (allerror);
}

/*
 *	discard preallocated blocks
 */
int
ext2_inactive(struct vnop_inactive_args *ap)
{
	struct vnode *vp = ap->a_vp;
	struct inode *ip = VTOI(vp);
	int mode, error = 0;

	/*
	 * Ignore inodes related to stale file handles.
	 */
	if (ip->i_mode == 0)
		goto out;
	if (ip->i_nlink <= 0) {
		error = ext2_truncate(vp, (off_t)0, 0, NOCRED, NULL);
		ip->i_rdev = 0;
		mode = ip->i_mode;
		ip->i_mode = 0;
		ip->i_flag |= IN_CHANGE | IN_UPDATE;
		ext2_vfree(vp, ip->i_number, mode);
	}
	if (ip->i_flag & (IN_ACCESS | IN_CHANGE | IN_MODIFIED | IN_UPDATE))
		ext2_update(vp, 0);
out:
	/*
	 * If we are done with the inode, reclaim it
	 * so that it can be reused immediately.
	 */
	/*
	 * FreeBSD called vrecycle() to have the vnode torn down at once. XNU
	 * spells the same request vnode_recycle().
	 */
	if (ip->i_mode == 0)
		vnode_recycle(vp);
	return (error);
}

/*
 * Reclaim an inode so that it can be used for other purposes.
 */
int
ext2_reclaim(struct vnop_reclaim_args *ap)
{
	struct inode *ip;
	struct vnode *vp = ap->a_vp;

	ip = VTOI(vp);
	if (ip == NULL)
		return (0);

	if (ip->i_flag & IN_LAZYMOD) {
		ip->i_flag |= IN_MODIFIED;
		ext2_update(vp, 0);
	}

	/*
	 * Take the inode out of the hash before the vnode loses its identity,
	 * so no concurrent ext2_ihashget() can find it and try to reference a
	 * vnode that is being torn down. FreeBSD used the generic vfs_hash;
	 * this file system carries its own (ext2_ihash.c).
	 */
	ext2_ihashrem(ip);

	if (ip->i_lock != NULL) {
		lck_mtx_free(ip->i_lock, ext2_lck_grp);
		ip->i_lock = NULL;
	}

	/*
	 * Give back the named reference taken by VNFS_ADDFSREF in ext2_vget().
	 *
	 * This is not optional. <sys/vnode.h> states it plainly: "VFS assumes
	 * that an unused vnode will not be marked as referenced by a file
	 * system." The mark is VNAMED_FSHASH, and a reclaimed vnode that still
	 * carries it goes onto the free list looking like it is still in our
	 * hash. When the vnode layer later takes it back off the free list to
	 * reuse, process_vp() finds the flag set and panics with "free vnode
	 * still referenced" - at an arbitrary later moment, in whichever
	 * subsystem happened to ask for a vnode, with nothing pointing back
	 * here.
	 */
	vnode_removefsref(vp);

	/*
	 * Detach before freeing: after this the vnode has no file system data,
	 * which is what XNU expects of a reclaimed vnode. vnode_destroy_vobject
	 * has no counterpart - the UBC is torn down by the vnode layer itself.
	 */
	vnode_clearfsnode(vp);
	_FREE(ip, M_TEMP);

	return (0);
}
