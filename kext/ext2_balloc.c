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
 *	@(#)ffs_balloc.c	8.4 (Berkeley) 9/23/93
 * $FreeBSD$
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/buf.h>
#include <sys/lock.h>
#include <sys/mount.h>
#include <sys/vnode.h>

#include <fs/ext2fs/fs.h>
#include <fs/ext2fs/inode.h>
#include <fs/ext2fs/ext2fs.h>
#include <fs/ext2fs/ext2_dinode.h>
#include <fs/ext2fs/ext2_extern.h>
#include <fs/ext2fs/ext2_mount.h>
#include <fs/ext2fs/ext2_apple.h>

/*
 * Balloc defines the structure of filesystem storage
 * by allocating the physical blocks on a device given
 * the inode and the logical block number in a file.
 */
int
ext2_balloc(struct inode *ip, e2fs_lbn_t lbn, int size, struct ucred *cred,
    buf_t *bpp, int flags)
{
	struct m_ext2fs *fs;
	struct ext2mount *ump;
	buf_t bp, nbp;
	struct vnode *vp = ITOV(ip);
	struct indir indirs[NIADDR + 2];
	e4fs_daddr_t nb, newb, indirb;
	e2fs_daddr_t *bap, pref;
	int osize, nsize, num, i, error;

	*bpp = NULL;
	if (lbn < 0)
		return (EFBIG);
	fs = ip->i_e2fs;
	ump = ip->i_ump;

	/*
	 * check if this is a sequential block allocation. 
	 * If so, increment next_alloc fields to allow ext2_blkpref 
	 * to make a good guess
	 */
	if (lbn == ip->i_next_alloc_block + 1) {
		ip->i_next_alloc_block++;
		ip->i_next_alloc_goal++;
	}

	/*
	 * The first NDADDR blocks are direct blocks
	 */
	if (lbn < NDADDR) {
		nb = ip->i_db[lbn];
		/* no new block is to be allocated, and no need to expand
		   the file */
		if (nb != 0 &&
		    ip->i_size >= (uint64_t)((lbn + 1) * fs->e2fs_bsize)) {
			error = buf_meta_bread(vp, (daddr64_t)lbn,
			    (int)fs->e2fs_bsize, NOCRED, &bp);
			if (error) {
				if (bp != NULL)
					buf_brelse(bp);
				return (error);
			}
			/*
			 * FreeBSD fixed up b_blkno by hand here. On XNU the
			 * buffer's physical address is resolved through
			 * VNOP_BLOCKMAP, which reads the very i_db entry this
			 * would have been derived from, so there is nothing
			 * left to correct.
			 */
			*bpp = bp;
			return (0);
		}
		if (nb != 0) {
			/*
			 * Consider need to reallocate a fragment.
			 */
			osize = fragroundup(fs, blkoff(fs, ip->i_size));
			nsize = fragroundup(fs, size);
			if (nsize <= osize) {
				error = buf_meta_bread(vp, (daddr64_t)lbn, osize,
				    NOCRED, &bp);
				if (error) {
					if (bp != NULL)
						buf_brelse(bp);
					return (error);
				}
			} else {
			/* Godmar thinks: this shouldn't happen w/o fragments */
				printf("nsize %d(%d) > osize %d(%d) nb %d\n", 
					(int)nsize, (int)size, (int)osize, 
					(int)ip->i_size, (int)nb);
				panic(
				    "ext2_balloc: Something is terribly wrong");
/*
 * please note there haven't been any changes from here on -
 * FFS seems to work.
 */
			}
		} else {
			if (ip->i_size < (uint64_t)((lbn + 1) * fs->e2fs_bsize))
				nsize = fragroundup(fs, size);
			else
				nsize = fs->e2fs_bsize;
			EXT2_LOCK(ump);
			error = ext2_alloc(ip, lbn,
			    ext2_blkpref(ip, lbn, (int)lbn, &ip->i_db[0], 0),
			    nsize, cred, &newb);
			if (error)
				return (error);
			/*
			 * Record the block in the inode before asking for a
			 * buffer. buf_getblk() resolves the buffer's physical
			 * address through VNOP_BLOCKMAP, which reads i_db, so
			 * the pointer has to be in place first. FreeBSD could
			 * defer this because it assigned b_blkno by hand.
			 */
			ip->i_db[lbn] = newb;
			bp = buf_getblk(vp, (daddr64_t)lbn, nsize, 0, 0,
			    BLK_META);
			if (bp == NULL) {
				ip->i_db[lbn] = 0;
				ext2_blkfree(ip, newb, nsize);
				return (EIO);
			}
			if (flags & BA_CLRBUF)
				buf_clear(bp);
		}
		ip->i_flag |= IN_CHANGE | IN_UPDATE;
		*bpp = bp;
		return (0);
	}
	/*
	 * Determine the number of levels of indirection.
	 */
	pref = 0;
	if ((error = ext2_getlbns(vp, lbn, indirs, &num)) != 0)
		return (error);
#ifdef INVARIANTS
	if (num < 1)
		panic ("ext2_balloc: ext2_getlbns returned indirect block");
#endif
	/*
	 * Fetch the first indirect block allocating if necessary.
	 */
	--num;
	nb = ip->i_ib[indirs[0].in_off];
	if (nb == 0) {
		EXT2_LOCK(ump);
		pref = ext2_blkpref(ip, lbn, indirs[0].in_off + 
					     EXT2_NDIR_BLOCKS, &ip->i_db[0], 0);
		if ((error = ext2_alloc(ip, lbn, pref, fs->e2fs_bsize, cred,
			&newb)))
			return (error);
		nb = newb;
		/*
		 * Indirect blocks live on the device vnode, addressed
		 * physically. ext2_bmaparray() reads them the same way; if the
		 * two disagreed they would hold separate buffers for one disk
		 * block and this write would be invisible to the reader.
		 */
		bp = buf_getblk(ip->i_devvp, (daddr64_t)fsbtodb(fs, newb),
		    (int)fs->e2fs_bsize, 0, 0, BLK_META);
		if (bp == NULL) {
			ext2_blkfree(ip, nb, fs->e2fs_bsize);
			return (EIO);
		}
		buf_clear(bp);
		/*
		 * Write synchronously so that indirect blocks
		 * never point at garbage.
		 */
		if ((error = buf_bwrite(bp)) != 0) {
			ext2_blkfree(ip, nb, fs->e2fs_bsize);
			return (error);
		}
		ip->i_ib[indirs[0].in_off] = newb;
		ip->i_flag |= IN_CHANGE | IN_UPDATE;
	}
	/*
	 * Fetch through the indirect blocks, allocating as necessary.
	 */
	indirb = nb;
	for (i = 1;;) {
		/*
		 * indirb holds the physical address of the indirect block for
		 * this level: initially the one out of i_ib, thereafter the
		 * entry read out of the level above.
		 */
		error = buf_meta_bread(ip->i_devvp,
		    (daddr64_t)fsbtodb(fs, indirb), (int)fs->e2fs_bsize,
		    NOCRED, &bp);
		if (error) {
			if (bp != NULL)
				buf_brelse(bp);
			return (error);
		}
		bap = (e2fs_daddr_t *)buf_dataptr(bp);
		nb = bap[indirs[i].in_off];
		if (i == num)
			break;
		i += 1;
		if (nb != 0) {
			indirb = nb;
			buf_brelse(bp);
			continue;
		}
		EXT2_LOCK(ump);
		if (pref == 0)
			pref = ext2_blkpref(ip, lbn, indirs[i].in_off, bap,
						indirs[i].in_lbn);
		error =  ext2_alloc(ip, lbn, pref, (int)fs->e2fs_bsize, cred, &newb);
		if (error) {
			buf_brelse(bp);
			return (error);
		}
		nb = newb;
		nbp = buf_getblk(ip->i_devvp, (daddr64_t)fsbtodb(fs, nb),
		    (int)fs->e2fs_bsize, 0, 0, BLK_META);
		if (nbp == NULL) {
			ext2_blkfree(ip, nb, fs->e2fs_bsize);
			EXT2_UNLOCK(ump);
			buf_brelse(bp);
			return (EIO);
		}
		buf_clear(nbp);
		/*
		 * Write synchronously so that indirect blocks
		 * never point at garbage.
		 */
		if ((error = buf_bwrite(nbp)) != 0) {
			ext2_blkfree(ip, nb, fs->e2fs_bsize);
			EXT2_UNLOCK(ump);
			buf_brelse(bp);
			return (error);
		}
		bap[indirs[i - 1].in_off] = nb;
		indirb = nb;
		/*
		 * If required, write synchronously, otherwise use
		 * delayed write. FreeBSD also set B_CLUSTEROK on a
		 * full-block buffer; XNU's cluster layer has no such flag.
		 */
		if (flags & IO_SYNC) {
			buf_bwrite(bp);
		} else {
			buf_bdwrite(bp);
		}
	}
	/*
	 * Get the data block, allocating if necessary.
	 */
	if (nb == 0) {
		EXT2_LOCK(ump);
		pref = ext2_blkpref(ip, lbn, indirs[i].in_off, &bap[0],
				indirs[i].in_lbn);
		if ((error = ext2_alloc(ip,
		    lbn, pref, (int)fs->e2fs_bsize, cred, &newb)) != 0) {
			buf_brelse(bp);
			return (error);
		}
		nb = newb;
		/*
		 * Publish the new block in the indirect block before asking
		 * for its buffer: buf_getblk() on the file vnode goes through
		 * VNOP_BLOCKMAP, which walks this same indirect block to find
		 * the physical address.
		 */
		bap[indirs[i].in_off] = nb;
		nbp = buf_getblk(vp, (daddr64_t)lbn, (int)fs->e2fs_bsize, 0, 0,
		    BLK_META);
		if (nbp == NULL) {
			bap[indirs[i].in_off] = 0;
			ext2_blkfree(ip, nb, fs->e2fs_bsize);
			buf_brelse(bp);
			return (EIO);
		}
		if (flags & BA_CLRBUF)
			buf_clear(nbp);
		/*
		 * If required, write synchronously, otherwise use
		 * delayed write.
		 */
		if (flags & IO_SYNC) {
			buf_bwrite(bp);
		} else {
			buf_bdwrite(bp);
		}
		*bpp = nbp;
		return (0);
	}
	buf_brelse(bp);
	if (flags & BA_CLRBUF) {
		/*
		 * FreeBSD started a clustered read-ahead here when the caller
		 * passed a sequential-access hint. XNU's cluster_read() is a
		 * uio-based path for file data rather than something that
		 * hands back a buffer, so the hint is dropped and the block is
		 * read on its own; read-ahead for file data belongs to the
		 * cluster layer, which the read path drives directly.
		 */
		error = buf_meta_bread(vp, (daddr64_t)lbn,
		    (int)fs->e2fs_bsize, NOCRED, &nbp);
		if (error) {
			if (nbp != NULL)
				buf_brelse(nbp);
			return (error);
		}
	} else {
		nbp = buf_getblk(vp, (daddr64_t)lbn, (int)fs->e2fs_bsize, 0, 0,
		    BLK_META);
		if (nbp == NULL)
			return (EIO);
	}
	*bpp = nbp;
	return (0);
}

