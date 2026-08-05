/*-
 * Copyright (c) 1989, 1991, 1993
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
 *	@(#)ufs_bmap.c	8.7 (Berkeley) 3/21/95
 * $FreeBSD$
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/buf.h>
#include <sys/proc.h>
#include <sys/vnode.h>
#include <sys/mount.h>
#include <sys/stat.h>

#include <fs/ext2fs/inode.h>
#include <fs/ext2fs/fs.h>
#include <fs/ext2fs/ext2fs.h>
#include <fs/ext2fs/ext2_dinode.h>
#include <fs/ext2fs/ext2_extern.h>
#include <fs/ext2fs/ext2_mount.h>
#include <fs/ext2fs/ext2_apple.h>

static int ext4_bmapext(struct vnode *, daddr64_t, daddr64_t *, int *, int *);

/*
 * Map a byte range of a file to a device block number and a count of
 * contiguous bytes.
 *
 * This replaces FreeBSD's VOP_BMAP. The two describe the same mapping but in
 * different units: VOP_BMAP takes a logical block and reports forward and
 * backward runs in blocks, while VNOP_BLOCKMAP takes a byte offset and reports
 * a single forward run in bytes. Converting here lets ext2_bmaparray() below
 * keep working in blocks, as the rest of the imported code expects.
 *
 * FreeBSD's a_bop - the caller asking for the underlying device's bufobj - has
 * no counterpart and is simply gone, and because there is no backward run to
 * report, ext2_bmaparray() is always called with runb == NULL.
 */
int
ext2_blockmap(struct vnop_blockmap_args *ap)
{
	struct vnode *vp = ap->a_vp;
	struct inode *ip;
	struct m_ext2fs *fs;
	daddr64_t lbn, bn;
	off_t blkoffset, contig;
	int error, run = 0;
	uint32_t bsize;

	if (ap->a_bpn == NULL)
		return (0);

	ip = VTOI(vp);
	fs = ip->i_e2fs;
	bsize = fs->e2fs_bsize;

	lbn = (daddr64_t)lblkno(fs, ap->a_foffset);
	blkoffset = blkoff(fs, ap->a_foffset);

	if (ip->i_flag & IN_E4EXTENTS)
		error = ext4_bmapext(vp, lbn, &bn, &run, NULL);
	else
		error = ext2_bmaparray(vp, lbn, &bn, &run, NULL);
	if (error != 0)
		return (error);

	if (ap->a_poff != NULL)
		*(int *)ap->a_poff = 0;

	if (bn < 0) {
		/* A hole: no device blocks back this offset. */
		*ap->a_bpn = (daddr64_t)-1;
		if (ap->a_run != NULL)
			*ap->a_run = 0;
		return (0);
	}

	/*
	 * bn addresses the file system block holding a_foffset; step forward
	 * to the device block that holds the byte itself.
	 */
	*ap->a_bpn = bn + (daddr64_t)(blkoffset >> DEV_BSHIFT);

	if (ap->a_run != NULL) {
		/*
		 * run counts the file system blocks that follow bn
		 * contiguously, so the mapping is good to the end of that
		 * span - less the part of the first block already behind
		 * a_foffset. Never report past what was asked for, nor past
		 * end of file.
		 */
		contig = (off_t)(run + 1) * (off_t)bsize - blkoffset;
		if (contig > (off_t)ap->a_size)
			contig = (off_t)ap->a_size;
		if (ap->a_foffset + contig > (off_t)ip->i_size)
			contig = (off_t)ip->i_size - ap->a_foffset;
		if (contig < 0)
			contig = 0;
		*ap->a_run = (size_t)contig;
	}

	return (0);
}

/*
 * Convert between a file's logical block numbers and byte offsets.
 *
 * FreeBSD has no counterpart: its VOP_BMAP works in logical blocks
 * throughout, so nothing ever needs to translate. XNU splits the two apart -
 * VNOP_BLOCKMAP is byte-based - and then needs a way back, so every file
 * system that puts metadata through the buffer cache has to supply this pair.
 *
 * They are not optional decoration. buf_strategy() calls VNOP_BLKTOOFF() to
 * turn the buffer's logical block number into the offset it then hands to
 * VNOP_BLOCKMAP(), and cluster I/O calls VNOP_OFFTOBLK() for the reverse.
 * Without them the call lands on vn_default_error(), so every read of a
 * directory block through the directory's own vnode failed with ENOTSUP and
 * the volume mounted but could not be listed. Reads issued against the device
 * vnode - the superblock, the group descriptors, the inode blocks - go through
 * spec_strategy() and never touch this vector, which is why the mount itself
 * worked.
 */
int
ext2_blktooff(struct vnop_blktooff_args *ap)
{
	struct inode *ip = VTOI(ap->a_vp);

	if (ap->a_offset == NULL)
		return (EINVAL);

	*ap->a_offset = (off_t)ap->a_lblkno * (off_t)ip->i_e2fs->e2fs_bsize;

	return (0);
}

int
ext2_offtoblk(struct vnop_offtoblk_args *ap)
{
	struct inode *ip = VTOI(ap->a_vp);

	if (ap->a_lblkno == NULL)
		return (EINVAL);

	*ap->a_lblkno = (daddr64_t)(ap->a_offset / (off_t)ip->i_e2fs->e2fs_bsize);

	return (0);
}

/*
 * This function converts the logical block number of a file to
 * its physical block number on the disk within ext4 extents.
 */
static int
ext4_bmapext(struct vnode *vp, daddr64_t bn, daddr64_t *bnp, int *runp, int *runb)
{
	struct inode *ip;
	struct m_ext2fs *fs;
	struct ext4_extent *ep;
	struct ext4_extent_path path = { .ep_bp = NULL };
	daddr64_t lbn;

	ip = VTOI(vp);
	fs = ip->i_e2fs;
	lbn = bn;

	/*
	 * TODO: need to implement read ahead to improve the performance.
	 */
	if (runp != NULL)
		*runp = 0;

	if (runb != NULL)
		*runb = 0;

	ext4_ext_find_extent(fs, ip, lbn, &path);
	ep = path.ep_ext;
	if (ep == NULL)
		return (EIO);

	*bnp = fsbtodb(fs, lbn - ep->e_blk +
	    (ep->e_start_lo | (daddr64_t)ep->e_start_hi << 32));

	if (*bnp == 0)
		*bnp = -1;

	return (0);
}

/*
 * Indirect blocks are now on the vnode for the file.  They are given negative
 * logical block numbers.  Indirect blocks are addressed by the negative
 * address of the first data block to which they point.  Double indirect blocks
 * are addressed by one less than the address of the first indirect block to
 * which they point.  Triple indirect blocks are addressed by one less than
 * the address of the first double indirect block to which they point.
 *
 * ext2_bmaparray does the bmap conversion, and if requested returns the
 * array of logical blocks which must be traversed to get to a block.
 * Each entry contains the offset into that block that gets you to the
 * next block and the disk address of the block (if it is assigned).
 */

int
ext2_bmaparray(struct vnode *vp, daddr64_t bn, daddr64_t *bnp, int *runp, int *runb)
{
	struct inode *ip;
	struct buf *bp;
	struct ext2mount *ump;
	struct mount *mp;
	struct indir a[NIADDR+1], *ap;
	e2fs_daddr_t *bap;
	daddr64_t daddr;
	e2fs_lbn_t metalbn;
	int error, num, maxrun = 0, bsize;
	int *nump;

	ap = NULL;
	ip = VTOI(vp);
	mp = vnode_mount(vp);
	ump = VFSTOEXT2(mp);

	bsize = (int)EXT2_BLOCK_SIZE(ump->um_e2fs);

	if (runp) {
		struct vfsioattr ioattr;

		/*
		 * Cap a run at what the device will accept in one read.
		 * FreeBSD keeps that limit in mp->mnt_iosize_max; XNU reports
		 * it through vfs_ioattr(), which fills in system defaults when
		 * the mount has none of its own.
		 */
		vfs_ioattr(mp, &ioattr);
		maxrun = (int)(ioattr.io_maxreadcnt / (uint32_t)bsize) - 1;
		if (maxrun < 0)
			maxrun = 0;
		*runp = 0;
	}

	if (runb) {
		*runb = 0;
	}


	ap = a;
	nump = &num;
	error = ext2_getlbns(vp, bn, ap, nump);
	if (error)
		return (error);

	num = *nump;
	if (num == 0) {
		*bnp = blkptrtodb(ump, ip->i_db[bn]);
		if (*bnp == 0) {
			*bnp = -1;
		} else if (runp) {
			daddr64_t bnb = bn;
			for (++bn; bn < NDADDR && *runp < maxrun &&
			    is_sequential(ump, ip->i_db[bn - 1], ip->i_db[bn]);
			    ++bn, ++*runp);
			bn = bnb;
			if (runb && (bn > 0)) {
				for (--bn; (bn >= 0) && (*runb < maxrun) &&
					is_sequential(ump, ip->i_db[bn],
						ip->i_db[bn + 1]);
						--bn, ++*runb);
			}
		}
		return (0);
	}


	/* Get disk address out of indirect block array */
	daddr = ip->i_ib[ap->in_off];

	for (bp = NULL, ++ap; --num; ++ap) {
		/*
		 * Stop once the path runs into a hole - no disk address means
		 * no indirect block to walk - or once we reach the meta-block
		 * the caller was asking for.
		 *
		 * FreeBSD also carried on when the block had no disk address
		 * but was present in the buffer cache. That state does not
		 * arise here: ext2_balloc() allocates an indirect block's disk
		 * address with ext2_alloc() and writes it synchronously before
		 * the buffer is ever left in the cache, so an unallocated
		 * indirect block is genuinely absent.
		 */
		metalbn = ap->in_lbn;
		if (daddr == 0 || metalbn == bn)
			break;

		if (bp != NULL)
			buf_brelse(bp);

		/*
		 * Read the indirect block through the device vnode, addressed
		 * physically.
		 *
		 * FreeBSD caches indirect blocks on the file's own vnode under
		 * negative logical block numbers, relying on VOP_BMAP to
		 * translate them on a miss. That cannot work on XNU, whose
		 * VNOP_BLOCKMAP is expressed in byte offsets into the file and
		 * has no way to describe a negative block. Metadata therefore
		 * lives on the device vnode, keyed by physical address, as it
		 * does in ext2_blkatoff() and the extent code. ext2_balloc()
		 * has to agree with this, or the two would end up holding
		 * separate buffers for the same disk block.
		 */
		error = buf_meta_bread(ip->i_devvp,
		    (daddr64_t)blkptrtodb(ump, daddr), bsize, NOCRED, &bp);
		if (error != 0) {
			if (bp != NULL)
				buf_brelse(bp);
			return (error);
		}

		bap = (e2fs_daddr_t *)buf_dataptr(bp);
		daddr = bap[ap->in_off];
		if (num == 1 && daddr && runp) {
			for (bn = ap->in_off + 1;
			    bn < (daddr64_t)MNINDIR(ump) && *runp < maxrun &&
			    is_sequential(ump, bap[bn - 1], bap[bn]);
			    ++bn, ++*runp);
			bn = ap->in_off;
			if (runb && bn) {
				for (--bn; bn >= 0 && *runb < maxrun &&
					is_sequential(ump, bap[bn], bap[bn + 1]);
					--bn, ++*runb);
			}
		}
	}
	if (bp != NULL)
		buf_brelse(bp);

	/*
	 * FreeBSD checked here for a BLK_NOCOPY/BLK_SNAP placeholder left by
	 * an FFS snapshot and reported it as a hole. ext2fs implements no
	 * snapshots - nothing ever sets SF_SNAPSHOT on an ext2 inode - and
	 * um_seqinc, the GEOM-era field the test relied on, is gone, so the
	 * check has no subject and has been dropped.
	 */
	*bnp = blkptrtodb(ump, daddr);
	if (*bnp == 0) {
		*bnp = -1;
	}
	return (0);
}

/*
 * Create an array of logical block number/offset pairs which represent the
 * path of indirect blocks required to access a data block.  The first "pair"
 * contains the logical block number of the appropriate single, double or
 * triple indirect block and the offset into the inode indirect block array.
 * Note, the logical block number of the inode single/double/triple indirect
 * block appears twice in the array, once with the offset into the i_ib and
 * once with the offset into the page itself.
 */
int
ext2_getlbns(struct vnode *vp, daddr64_t bn, struct indir *ap, int *nump)
{
	long blockcnt;
	e2fs_lbn_t metalbn, realbn;
	struct ext2mount *ump;
	int i, numlevels, off;
	int64_t qblockcnt;

	ump = VFSTOEXT2(vnode_mount(vp));
	if (nump)
		*nump = 0;
	numlevels = 0;
	realbn = bn;
	if ((long)bn < 0)
		bn = -(long)bn;

	/* The first NDADDR blocks are direct blocks. */
	if (bn < NDADDR)
		return (0);

	/*
	 * Determine the number of levels of indirection.  After this loop
	 * is done, blockcnt indicates the number of data blocks possible
	 * at the previous level of indirection, and NIADDR - i is the number
	 * of levels of indirection needed to locate the requested block.
	 */
	for (blockcnt = 1, i = NIADDR, bn -= NDADDR;; i--, bn -= blockcnt) {
		if (i == 0)
			return (EFBIG);
		/*
		 * Use int64_t's here to avoid overflow for triple indirect
		 * blocks when longs have 32 bits and the block size is more
		 * than 4K.
		 *
		 * MNINDIR is u_long, so it is cast as well: left implicit it
		 * would drag the signed operands into unsigned arithmetic,
		 * which matters because bn is a signed block number that the
		 * caller may have handed us negative (a meta-block).
		 */
		qblockcnt = (int64_t)blockcnt * (int64_t)MNINDIR(ump);
		if (bn < qblockcnt)
			break;
		blockcnt = qblockcnt;
	}

	/* Calculate the address of the first meta-block. */
	if (realbn >= 0)
		metalbn = -(realbn - bn + NIADDR - i);
	else
		metalbn = -(-realbn - bn + NIADDR - i);

	/*
	 * At each iteration, off is the offset into the bap array which is
	 * an array of disk addresses at the current level of indirection.
	 * The logical block number and the offset in that block are stored
	 * into the argument array.
	 */
	ap->in_lbn = metalbn;
	ap->in_off = off = NIADDR - i;
	ap++;
	for (++numlevels; i <= NIADDR; i++) {
		/* If searching for a meta-data block, quit when found. */
		if (metalbn == realbn)
			break;

		off = (int)((bn / blockcnt) % (int64_t)MNINDIR(ump));

		++numlevels;
		ap->in_lbn = metalbn;
		ap->in_off = off;
		++ap;

		metalbn -= -1 + off * blockcnt;
		blockcnt /= MNINDIR(ump);
	}
	if (nump)
		*nump = numlevels;
	return (0);
}
