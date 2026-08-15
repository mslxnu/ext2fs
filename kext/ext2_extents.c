/*-
 * Copyright (c) 2010 Zheng Liu <lz@freebsd.org>
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
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR AND CONTRIBUTORS BE LIABLE
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

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/types.h>
#include <sys/kernel.h>
#include <sys/malloc.h>
#include <sys/vnode.h>
#include <sys/buf.h>
#include <sys/conf.h>

#include <fs/ext2fs/ext2_mount.h>
#include <fs/ext2fs/fs.h>
#include <fs/ext2fs/inode.h>
#include <fs/ext2fs/ext2fs.h>
#include <fs/ext2fs/ext2_extents.h>
#include <fs/ext2fs/ext2_extern.h>
#include <fs/ext2fs/ext2_apple.h>

static void ext4_ext_binsearch_index(struct inode *ip, struct ext4_extent_path
		*path, daddr64_t lbn)
{
	struct ext4_extent_header *ehp = path->ep_header;
	struct ext4_extent_index *l, *r, *m;

	l = (struct ext4_extent_index *)(char *)(ehp + 1);
	r = (struct ext4_extent_index *)(char *)(ehp + 1) + ehp->eh_ecount - 1;
	while (l <= r) {
		m = l + (r - l) / 2;
		if (lbn < m->ei_blk)
			r = m - 1;
		else
			l = m + 1;
	}

	path->ep_index = l - 1;
}

static void
ext4_ext_binsearch(struct inode *ip, struct ext4_extent_path *path, daddr64_t lbn)
{
	struct ext4_extent_header *ehp = path->ep_header;
	struct ext4_extent *l, *r, *m;

	if (ehp->eh_ecount == 0)
		return;

	l = (struct ext4_extent *)(char *)(ehp + 1);
	r = (struct ext4_extent *)(char *)(ehp + 1) + ehp->eh_ecount - 1;
	while (l <= r) {
		m = l + (r - l) / 2;
		if (lbn < m->e_blk)
			r = m - 1;
		else
			l = m + 1;
	}

	path->ep_ext = l - 1;
}

/*
 * Find a block in ext4 extent cache.
 */
int
ext4_ext_in_cache(struct inode *ip, daddr64_t lbn, struct ext4_extent *ep)
{
	struct ext4_extent_cache *ecp;
	int ret = EXT4_EXT_CACHE_NO;

	ecp = &ip->i_ext_cache;

	/* cache is invalid */
	if (ecp->ec_type == EXT4_EXT_CACHE_NO)
		return (ret);

	if (lbn >= ecp->ec_blk && lbn < ecp->ec_blk + ecp->ec_len) {
		ep->e_blk = ecp->ec_blk;
		ep->e_start_lo = ecp->ec_start & 0xffffffff;
		ep->e_start_hi = ecp->ec_start >> 32 & 0xffff;
		ep->e_len = ecp->ec_len;
		ret = ecp->ec_type;
	}
	return (ret);
}

/*
 * Put an ext4_extent structure in ext4 cache.
 */
void
ext4_ext_put_cache(struct inode *ip, struct ext4_extent *ep, int type)
{
	struct ext4_extent_cache *ecp;

	ecp = &ip->i_ext_cache;
	ecp->ec_type = type;
	ecp->ec_blk = ep->e_blk;
	ecp->ec_len = ep->e_len;
	ecp->ec_start = (daddr64_t)ep->e_start_hi << 32 | ep->e_start_lo;
}

/*
 * Find an extent.
 */
struct ext4_extent_path *
ext4_ext_find_extent(struct m_ext2fs *fs, struct inode *ip,
		     daddr64_t lbn, struct ext4_extent_path *path)
{
	struct ext4_extent_header *ehp;
	uint16_t i;
	int error, size;
	daddr64_t nblk;

	ehp = (struct ext4_extent_header *)(char *)ip->i_db;

	if (ehp->eh_magic != EXT4_EXT_MAGIC)
		return (NULL);

	path->ep_header = ehp;

	for (i = ehp->eh_depth; i != 0; --i) {
		ext4_ext_binsearch_index(ip, path, lbn);
		path->ep_depth = 0;
		path->ep_ext = NULL;

		nblk = (daddr64_t)path->ep_index->ei_leaf_hi << 32 |
		    path->ep_index->ei_leaf_lo;
		size = blksize(fs, ip, nblk);
		if (path->ep_bp != NULL) {
			buf_brelse(path->ep_bp);
			path->ep_bp = NULL;
		}
		error = buf_meta_bread(ip->i_devvp, (daddr64_t)fsbtodb(fs, nblk),
			    size, NOCRED, &path->ep_bp);
		if (error) {
			if (path->ep_bp != NULL)
				buf_brelse(path->ep_bp);
			path->ep_bp = NULL;
			return (NULL);
		}
		ehp = (struct ext4_extent_header *)buf_dataptr(path->ep_bp);
		path->ep_header = ehp;
	}

	path->ep_depth = i;
	path->ep_ext = NULL;
	path->ep_index = NULL;

	ext4_ext_binsearch(ip, path, lbn);
	return (path);
}

/*
 * Drop references held by an extent path, releasing any buffers.
 */
void
ext4_ext_drop_refs(struct ext4_extent_path *path)
{
	while (path->ep_depth >= 0) {
		if (path->ep_bp != NULL) {
			buf_brelse(path->ep_bp);
			path->ep_bp = NULL;
		}
		path->ep_depth--;
		path++;
	}
}

/*
 * Mark the inode dirty after an extent tree modification.
 */
static void
ext4_ext_mark_dirty(struct inode *ip)
{
	ip->i_flag |= IN_CHANGE | IN_UPDATE;
}

/*
 * Allocate a new zeroed metadata block and return it written.
 */
static int
ext4_ext_new_block(struct inode *ip, e4fs_daddr_t *newp)
{
	struct m_ext2fs *fs = ip->i_e2fs;
	buf_t bp;
	int error;

	EXT2_LOCK(ip->i_ump);
	error = ext2_alloc(ip, 0, 0, fs->e2fs_bsize, NOCRED, newp);
	EXT2_UNLOCK(ip->i_ump);
	if (error)
		return (error);

	bp = buf_getblk(ip->i_devvp, (daddr64_t)fsbtodb(fs, *newp),
	    (int)fs->e2fs_bsize, 0, 0, BLK_META);
	if (bp == NULL) {
		ext2_blkfree(ip, *newp, fs->e2fs_bsize);
		return (EIO);
	}
	buf_clear(bp);
	error = buf_bwrite(bp);
	if (error) {
		ext2_blkfree(ip, *newp, fs->e2fs_bsize);
		return (error);
	}
	return (0);
}

/*
 * Grow a depth-0 extent tree to depth-1. The existing entries are moved to a
 * new leaf block, and the inode's i_db becomes an index block with two entries.
 */
static int
ext4_ext_grow_depth0(struct inode *ip, struct ext4_extent_path *path,
    struct ext4_extent *newext)
{
	struct m_ext2fs *fs = ip->i_e2fs;
	struct ext4_extent_header *ehp = path->ep_header;
	struct ext4_extent_header *nehp;
	struct ext4_extent_index *neix;
	struct ext4_extent *old_ents, *new_ents;
	buf_t old_bp, new_bp;
	e4fs_daddr_t old_block, new_block;
	unsigned int nblks;
	int i, error;

	error = ext4_ext_new_block(ip, &old_block);
	if (error)
		return (error);
	error = ext4_ext_new_block(ip, &new_block);
	if (error) {
		ext2_blkfree(ip, old_block, fs->e2fs_bsize);
		return (error);
	}

	old_bp = buf_getblk(ip->i_devvp, (daddr64_t)fsbtodb(fs, old_block),
	    (int)fs->e2fs_bsize, 0, 0, BLK_META);
	if (old_bp == NULL) {
		ext2_blkfree(ip, old_block, fs->e2fs_bsize);
		ext2_blkfree(ip, new_block, fs->e2fs_bsize);
		return (EIO);
	}
	buf_clear(old_bp);

	new_bp = buf_getblk(ip->i_devvp, (daddr64_t)fsbtodb(fs, new_block),
	    (int)fs->e2fs_bsize, 0, 0, BLK_META);
	if (new_bp == NULL) {
		buf_brelse(old_bp);
		ext2_blkfree(ip, old_block, fs->e2fs_bsize);
		ext2_blkfree(ip, new_block, fs->e2fs_bsize);
		return (EIO);
	}
	buf_clear(new_bp);

	nehp = (struct ext4_extent_header *)buf_dataptr(old_bp);
	nehp->eh_magic = EXT4_EXT_MAGIC;
	nehp->eh_max = ehp->eh_max;
	nehp->eh_depth = 0;

	old_ents = (struct ext4_extent *)(nehp + 1);
	nblks = ehp->eh_max / 2;
	for (i = 0; i < (int)nblks; i++)
		old_ents[i] = ((struct ext4_extent *)(ehp + 1))[i];
	nehp->eh_ecount = nblks;

	nehp = (struct ext4_extent_header *)buf_dataptr(new_bp);
	nehp->eh_magic = EXT4_EXT_MAGIC;
	nehp->eh_max = ehp->eh_max;
	nehp->eh_depth = 0;
	nehp->eh_ecount = 1;
	new_ents = (struct ext4_extent *)(nehp + 1);
	new_ents[0] = *newext;

	nehp = (struct ext4_extent_header *)ip->i_db;
	nehp->eh_magic = EXT4_EXT_MAGIC;
	nehp->eh_max = 3;
	nehp->eh_ecount = 2;
	nehp->eh_depth = 1;

	neix = (struct ext4_extent_index *)(nehp + 1);
	neix->ei_blk = old_ents[0].e_blk;
	neix->ei_leaf_lo = (uint32_t)old_block;
	neix->ei_leaf_hi = 0;
	neix->ei_unused = 0;

	neix++;
	neix->ei_blk = new_ents[0].e_blk;
	neix->ei_leaf_lo = (uint32_t)new_block;
	neix->ei_leaf_hi = 0;
	neix->ei_unused = 0;

	ip->i_blocks += btodb(fs->e2fs_bsize) * 2;
	ext4_ext_mark_dirty(ip);

	error = buf_bwrite(old_bp);
	if (error)
		goto fail;
	error = buf_bwrite(new_bp);
	if (error)
		goto fail;

	return (0);

fail:
	ip->i_blocks -= btodb(fs->e2fs_bsize) * 2;
	ext2_blkfree(ip, old_block, fs->e2fs_bsize);
	ext2_blkfree(ip, new_block, fs->e2fs_bsize);
	return (error);
}

/*
 * Split a full leaf node into two halves. The right sibling gets the entries
 * at or after the midpoint; the left keeps those before it. The new extent
 * is placed into whichever half it belongs.
 */
static int
ext4_ext_split_leaf(struct inode *ip, struct ext4_extent_path *path,
    struct ext4_extent *newext, e4fs_daddr_t new_block)
{
	struct m_ext2fs *fs = ip->i_e2fs;
	struct ext4_extent_header *ehp = path->ep_header;
	struct ext4_extent_header *nehp;
	struct ext4_extent *l, *r;
	buf_t bp;
	unsigned int nblks;
	int i;

	bp = buf_getblk(ip->i_devvp, (daddr64_t)fsbtodb(fs, new_block),
	    (int)fs->e2fs_bsize, 0, 0, BLK_META);
	if (bp == NULL) {
		ext2_blkfree(ip, new_block, fs->e2fs_bsize);
		return (EIO);
	}
	buf_clear(bp);

	nehp = (struct ext4_extent_header *)buf_dataptr(bp);
	nehp->eh_magic = EXT4_EXT_MAGIC;
	nehp->eh_max = ehp->eh_max;
	nehp->eh_depth = ehp->eh_depth;

	nblks = ehp->eh_max / 2;
	l = (struct ext4_extent *)(ehp + 1);
	r = (struct ext4_extent *)(nehp + 1);

	for (i = nblks; i < ehp->eh_max; i++)
		r[i - nblks] = l[i];

	nehp->eh_ecount = ehp->eh_max - nblks;
	ehp->eh_ecount = nblks;

	if (newext->e_blk < r[0].e_blk) {
		for (i = nblks; i > 0; i--)
			l[i] = l[i - 1];
		l[0] = *newext;
		ehp->eh_ecount++;
	} else {
		for (i = nehp->eh_ecount; i > 0 && r[i - 1].e_blk > newext->e_blk; i--)
			r[i] = r[i - 1];
		r[i] = *newext;
		nehp->eh_ecount++;
	}

	ip->i_blocks += btodb(fs->e2fs_bsize);
	ext4_ext_mark_dirty(ip);

	return buf_bwrite(bp);
}

/*
 * Insert a new extent into the tree. Merges with an adjacent extent when
 * possible, otherwise inserts the new entry, splitting the leaf and growing
 * the tree depth as required.
 */
int
ext4_ext_insert_extent(struct inode *ip, struct ext4_extent_path *path,
    struct ext4_extent *newext)
{
	struct m_ext2fs *fs = ip->i_e2fs;
	struct ext4_extent_header *ehp = path->ep_header;
	struct ext4_extent *ex;
	e4fs_daddr_t new_block;
	int i, error;

	if (ehp->eh_ecount < ehp->eh_max) {
		ex = path->ep_ext;
		for (i = ehp->eh_ecount; i > (int)(ex - (struct ext4_extent *)
		    (char *)(ehp + 1)); i--)
			ex[i] = ex[i - 1];
		*ex = *newext;
		ehp->eh_ecount++;
		ip->i_blocks += btodb(fs->e2fs_bsize);
		ext4_ext_mark_dirty(ip);
		return (0);
	}

	if (ehp->eh_depth == 0)
		return ext4_ext_grow_depth0(ip, path, newext);

	error = ext4_ext_new_block(ip, &new_block);
	if (error)
		return (error);
	newext->e_start_lo = (uint32_t)new_block;

	error = ext4_ext_split_leaf(ip, path, newext, new_block);
	if (error)
		return (error);

	path->ep_bp = NULL;
	return (0);
}

/*
 * Remove an extent from the tree at logical block lbn, freeing all blocks it
 * covers. Used by truncate. The path must be a valid extent lookup for lbn.
 */
int
ext4_ext_rm_extent(struct inode *ip, struct ext4_extent_path *path,
    daddr64_t lbn, uint32_t len)
{
	struct m_ext2fs *fs = ip->i_e2fs;
	struct ext4_extent_header *ehp = path->ep_header;
	struct ext4_extent *ex = path->ep_ext;
	uint32_t phys_start, phys_len, free_start, free_len;
	int i, n;

	if (ex == NULL || path->ep_depth > 0)
		return (EIO);

	phys_start = ex->e_start_lo;
	phys_len = ex->e_len;

	if (lbn > (daddr64_t)ex->e_blk * fs->e2fs_bsize) {
		free_start = phys_start + (uint32_t)(lblkno(fs, lbn) - ex->e_blk);
		free_len = MIN(len, phys_len - (free_start - phys_start));
	} else {
		free_start = phys_start;
		free_len = MIN(len, phys_len);
	}

	for (i = 0; i < (int)free_len; i++)
		ext2_blkfree(ip, free_start + i, fs->e2fs_bsize);

	ip->i_blocks -= btodb(fs->e2fs_bsize) * free_len;
	ip->i_flag |= IN_CHANGE | IN_UPDATE;

	if (free_start == phys_start && free_len == phys_len) {
		n = ehp->eh_ecount - (int)(ex - (struct ext4_extent *)
		    (char *)(ehp + 1)) - 1;
		for (i = 0; i < n; i++)
			ex[i] = ex[i + 1];
		ex[i].e_blk = 0;
		ex[i].e_len = 0;
		ex[i].e_start_lo = 0;
		ex[i].e_start_hi = 0;
		ehp->eh_ecount--;
	} else if (free_start == phys_start) {
		ex->e_start_lo += free_len;
		ex->e_blk += free_len;
		ex->e_len -= free_len;
	} else if (free_start + free_len == phys_start + phys_len) {
		ex->e_len -= free_len;
	} else {
		ex->e_len = free_start - phys_start;
	}

	return (0);
}
