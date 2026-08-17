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
 *	@(#)ffs_alloc.c	8.8 (Berkeley) 2/21/94
 * $FreeBSD$
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/conf.h>
#include <sys/vnode.h>
#include <sys/stat.h>
#include <sys/mount.h>
#include <sys/sysctl.h>
#include <sys/syslog.h>
#include <sys/buf.h>
#include <sys/kauth.h>
#include <libkern/libkern.h>

#include <fs/ext2fs/fs.h>
#include <fs/ext2fs/inode.h>
#include <fs/ext2fs/ext2_mount.h>
#include <fs/ext2fs/ext2fs.h>
#include <fs/ext2fs/ext2_extern.h>
#include <fs/ext2fs/ext2_apple.h>

static daddr64_t	ext2_alloccg(struct inode *, int, daddr64_t, int);
static daddr64_t	ext2_clusteralloc(struct inode *, int, daddr64_t, int);
static u_long	ext2_dirpref(struct inode *);
static void	ext2_fserr(struct m_ext2fs *, uid_t, char *);
static u_long	ext2_hashalloc(struct inode *, int, long, int,
				daddr64_t (*)(struct inode *, int, daddr64_t, 
						int));
static daddr64_t	ext2_nodealloccg(struct inode *, int, daddr64_t, int);
static daddr64_t  ext2_mapsearch(struct m_ext2fs *, char *, daddr64_t);

/*
 * Allocate a block in the filesystem.
 *
 * A preference may be optionally specified. If a preference is given
 * the following hierarchy is used to allocate a block:
 *   1) allocate the requested block.
 *   2) allocate a rotationally optimal block in the same cylinder.
 *   3) allocate a block in the same cylinder group.
 *   4) quadradically rehash into other cylinder groups, until an
 *        available block is located.
 * If no block preference is given the following hierarchy is used
 * to allocate a block:
 *   1) allocate a block in the cylinder group that contains the
 *        inode for the file.
 *   2) quadradically rehash into other cylinder groups, until an
 *        available block is located.
 */
int
ext2_alloc(struct inode *ip, daddr64_t lbn, e4fs_daddr_t bpref, int size,
    struct ucred *cred, e4fs_daddr_t *bnp)
{
	struct m_ext2fs *fs;
	struct ext2mount *ump;
	int32_t bno;
	int cg;	
	*bnp = 0;
	fs = ip->i_e2fs;
	ump = ip->i_ump;
	LCK_MTX_ASSERT(EXT2_MTX(ump), LCK_MTX_ASSERT_OWNED);
#ifdef INVARIANTS
	if ((u_int)size > fs->e2fs_bsize || blkoff(fs, size) != 0) {
		vn_printf(ip->i_devvp, "bsize = %lu, size = %d, fs = %s\n",
		    (long unsigned int)fs->e2fs_bsize, size, fs->e2fs_fsmnt);
		panic("ext2_alloc: bad size");
	}
	if (cred == NOCRED)
		panic("ext2_alloc: missing credential");
#endif /* INVARIANTS */
	if (size == (int)fs->e2fs_bsize && fs->e2fs->e2fs_fbcount == 0)
		goto nospace;
	if (kauth_cred_getuid(cred) != 0 && 
		fs->e2fs->e2fs_fbcount < fs->e2fs->e2fs_rbcount)
		goto nospace;
	if (bpref >= fs->e2fs->e2fs_bcount)
		bpref = 0;
	if (bpref == 0)
		cg = ino_to_cg(fs, ip->i_number);
	else
		cg = dtog(fs, bpref);
	bno = (daddr64_t)ext2_hashalloc(ip, cg, bpref, fs->e2fs_bsize,
				      ext2_alloccg);
	if (bno > 0) {
		/* set next_alloc fields as done in block_getblk */
		ip->i_next_alloc_block = lbn;
		ip->i_next_alloc_goal = bno;

		ip->i_blocks += btodb(fs->e2fs_bsize);
		ip->i_flag |= IN_CHANGE | IN_UPDATE;
		*bnp = bno;
		return (0);
	}
nospace:
	EXT2_UNLOCK(ump);
	ext2_fserr(fs, kauth_cred_getuid(cred), "filesystem full");
	printf("\n%s: write failed, filesystem is full\n", fs->e2fs_fsmnt);
	return (ENOSPC);
}

/*
 * Reallocate a sequence of blocks into a contiguous sequence of blocks.
 *
 * The vnode and an array of buffer pointers for a range of sequential
 * logical blocks to be made contiguous is given. The allocator attempts
 * to find a range of sequential blocks starting as close as possible to
 * an fs_rotdelay offset from the end of the allocation for the logical
 * block immediately preceding the current range. If successful, the
 * physical block numbers in the buffer pointers and in the inode are
 * changed to reflect the new allocation. If unsuccessful, the allocation
 * is left unchanged. The success in doing the reallocation is returned.
 * Note that the error return is not reflected back to the user. Rather
 * the previous block allocation will be used.
 */

/*
 * FreeBSD implements VOP_REALLOCBLKS here: on a clustered write it relocates a
 * file's blocks so they end up contiguous. XNU has no VNOP_REALLOCBLKS - there
 * is no such entry in the vnode operation vector - so nothing could ever call
 * it, and the implementation depended on struct cluster_save and the rest of
 * FreeBSD's cluster layer besides. It is dropped rather than left to rot.
 *
 * The two sysctls that tuned it (vfs.ext2fs.doasyncfree and .doreallocblks)
 * went with it; the remaining users of doasyncfree are gone along with the
 * function itself.
 */

/*
 * Allocate an inode in the filesystem.
 * 
 */
int
ext2_valloc(struct vnode *pvp, int mode, struct ucred *cred, struct vnode **vpp)
{
	struct timespec ts;
	struct inode *pip;
	struct m_ext2fs *fs;
	struct inode *ip;
	struct ext2mount *ump;
	ino_t ino, ipref;
	int i, error, cg;
	
	*vpp = NULL;
	pip = VTOI(pvp);
	fs = pip->i_e2fs;
	ump = pip->i_ump;

	EXT2_LOCK(ump);
	if (fs->e2fs->e2fs_ficount == 0)
		goto noinodes;
	/*
	 * If it is a directory then obtain a cylinder group based on
	 * ext2_dirpref else obtain it using ino_to_cg. The preferred inode is
	 * always the next inode.
	 */
	if ((mode & IFMT) == IFDIR) {
		cg = ext2_dirpref(pip);
		if (fs->e2fs_contigdirs[cg] < 255)
			fs->e2fs_contigdirs[cg]++;
	} else {
		cg = ino_to_cg(fs, pip->i_number);
		if (fs->e2fs_contigdirs[cg] > 0)
			fs->e2fs_contigdirs[cg]--;
	}
	ipref = cg * fs->e2fs->e2fs_ipg + 1;
	ino = (ino_t)ext2_hashalloc(pip, cg, (long)ipref, mode, ext2_nodealloccg);

	if (ino == 0) 
		goto noinodes;
	/*
	 * XNU does not export VFS_VGET to kexts - the VFS dispatch macros are
	 * kernel-internal - so the file system calls its own vget directly.
	 * There is no lock flag to pass either: XNU vnodes come back from
	 * vnode_create() with an iocount and no lock mode to choose.
	 */
	error = ext2_vget(vnode_mount(pvp), ino, vpp, vfs_context_current());
	if (error) {
		ext2_vfree(pvp, ino, mode);
		return (error);
	}
	ip = VTOI(*vpp);

	/*
	 * The question is whether using VGET was such good idea at all:
	 * Linux doesn't read the old inode in when it is allocating a
	 * new one. I will set at least i_size and i_blocks to zero.
	 */
	ip->i_size = 0;
	ip->i_blocks = 0;
	ip->i_mode = 0;
	ip->i_flags = 0;
	/* now we want to make sure that the block pointers are zeroed out */
	for (i = 0; i < NDADDR; i++)
		ip->i_db[i] = 0;
	for (i = 0; i < NIADDR; i++)
		ip->i_ib[i] = 0;

	/*
	 * Set up a new generation number for this inode.
	 * XXX check if this makes sense in ext2
	 */
	/*
	 * random() is not part of the KPI; read_random() is the published way
	 * to get entropy. The value is only a generation counter, so any
	 * non-zero number will do.
	 */
	if (ip->i_gen == 0 || ++ip->i_gen == 0) {
		uint32_t gen;

		read_random(&gen, sizeof(gen));
		ip->i_gen = (gen >> 1) + 1;
	}

	nanotime(&ts);
	ip->i_birthtime = ts.tv_sec;
	ip->i_birthnsec = ts.tv_nsec;

/*
printf("ext2_valloc: allocated inode %d\n", ino);
*/
	return (0);
noinodes:
	EXT2_UNLOCK(ump);
	ext2_fserr(fs, kauth_cred_getuid(cred), "out of inodes");
	printf("\n%s: create/symlink failed, no inodes free\n", fs->e2fs_fsmnt);
	return (ENOSPC);
}

/*
 * Find a cylinder to place a directory.
 *
 * The policy implemented by this algorithm is to allocate a
 * directory inode in the same cylinder group as its parent
 * directory, but also to reserve space for its files inodes
 * and data. Restrict the number of directories which may be
 * allocated one after another in the same cylinder group
 * without intervening allocation of files.
 *
 * If we allocate a first level directory then force allocation
 * in another cylinder group.
 *
 */
static u_long
ext2_dirpref(struct inode *pip)
{
	struct m_ext2fs *fs;
	int cg, prefcg, cgsize;
	u_int avgifree, avgbfree, avgndir, curdirsize;
	u_int minifree, minbfree, maxndir;
	u_int mincg, minndir;
	u_int dirsize, maxcontigdirs;

	LCK_MTX_ASSERT(EXT2_MTX(pip->i_ump), LCK_MTX_ASSERT_OWNED);
	fs = pip->i_e2fs;

	avgifree = fs->e2fs->e2fs_ficount / fs->e2fs_gcount;
	avgbfree = fs->e2fs->e2fs_fbcount / fs->e2fs_gcount;
	avgndir  = fs->e2fs_total_dir / fs->e2fs_gcount;

	/*
	 * Force allocation in another cg if creating a first level dir.
	 */
	/*
	 * FreeBSD asserted here that the parent's vnode was locked. XNU has no
	 * equivalent - it does not expose a vnode's lock state - so the
	 * assertion is dropped; vnode_isvroot() replaces the VV_ROOT flag test,
	 * and read_random() stands in for arc4random(), which is not KPI.
	 */
	if (vnode_isvroot(ITOV(pip))) {
		uint32_t r;

		read_random(&r, sizeof(r));
		prefcg = r % fs->e2fs_gcount;
		mincg = prefcg;
		minndir = fs->e2fs_ipg;
		for (cg = prefcg; cg < (int)fs->e2fs_gcount; cg++)
			if (EXT2_GD(fs, cg)->ext2bgd_ndirs < minndir &&
			    EXT2_GD(fs, cg)->ext2bgd_nifree >= avgifree &&
			    EXT2_GD(fs, cg)->ext2bgd_nbfree >= avgbfree) {
				mincg = cg;
				minndir = EXT2_GD(fs, cg)->ext2bgd_ndirs;
			}
		for (cg = 0; cg < prefcg; cg++)
			if (EXT2_GD(fs, cg)->ext2bgd_ndirs < minndir &&
			    EXT2_GD(fs, cg)->ext2bgd_nifree >= avgifree &&
			    EXT2_GD(fs, cg)->ext2bgd_nbfree >= avgbfree) {
				mincg = cg;
				minndir = EXT2_GD(fs, cg)->ext2bgd_ndirs;
			}

		return (mincg);
	}

	/*
	 * Count various limits which used for
	 * optimal allocation of a directory inode.
	 */
	maxndir = min(avgndir + fs->e2fs_ipg / 16, fs->e2fs_ipg);
	minifree = avgifree - avgifree / 4;
	if (minifree < 1)
		minifree = 1;
	minbfree = avgbfree - avgbfree / 4;
	if (minbfree < 1)
		minbfree = 1;
	cgsize = fs->e2fs_fsize * fs->e2fs_fpg;
	dirsize = AVGDIRSIZE;
	curdirsize = avgndir ? (cgsize - avgbfree * fs->e2fs_bsize) / avgndir : 0;
	if (dirsize < curdirsize)
		dirsize = curdirsize;
	maxcontigdirs = min((avgbfree * fs->e2fs_bsize) / dirsize, 255);
	maxcontigdirs = min(maxcontigdirs, fs->e2fs_ipg / AFPDIR);
	if (maxcontigdirs == 0)
		maxcontigdirs = 1;

	/*
	 * Limit number of dirs in one cg and reserve space for 
	 * regular files, but only if we have no deficit in
	 * inodes or space.
	 */
	prefcg = ino_to_cg(fs, pip->i_number);
	for (cg = prefcg; cg < (int)fs->e2fs_gcount; cg++)
		if (EXT2_GD(fs, cg)->ext2bgd_ndirs < maxndir &&
		    EXT2_GD(fs, cg)->ext2bgd_nifree >= minifree &&
		    EXT2_GD(fs, cg)->ext2bgd_nbfree >= minbfree) {
			if (fs->e2fs_contigdirs[cg] < maxcontigdirs)
				return (cg);
		}
	for (cg = 0; cg < prefcg; cg++)
		if (EXT2_GD(fs, cg)->ext2bgd_ndirs < maxndir &&
		    EXT2_GD(fs, cg)->ext2bgd_nifree >= minifree &&
		    EXT2_GD(fs, cg)->ext2bgd_nbfree >= minbfree) {
			if (fs->e2fs_contigdirs[cg] < maxcontigdirs)
				return (cg);
		}
	/*
	 * This is a backstop when we have deficit in space.
	 */
	for (cg = prefcg; cg < (int)fs->e2fs_gcount; cg++)
		if (EXT2_GD(fs, cg)->ext2bgd_nifree >= avgifree)
			return (cg);
	for (cg = 0; cg < prefcg; cg++)
		if (EXT2_GD(fs, cg)->ext2bgd_nifree >= avgifree)
			break;
	return (cg);
}

/*
 * Select the desired position for the next block in a file.  
 *
 * we try to mimic what Remy does in inode_getblk/block_getblk
 *
 * we note: blocknr == 0 means that we're about to allocate either
 * a direct block or a pointer block at the first level of indirection
 * (In other words, stuff that will go in i_db[] or i_ib[])
 *
 * blocknr != 0 means that we're allocating a block that is none
 * of the above. Then, blocknr tells us the number of the block
 * that will hold the pointer
 */
e4fs_daddr_t
ext2_blkpref(struct inode *ip, e2fs_lbn_t lbn, int indx, e2fs_daddr_t *bap,
    e2fs_daddr_t blocknr)
{
	int	tmp;
	LCK_MTX_ASSERT(EXT2_MTX(ip->i_ump), LCK_MTX_ASSERT_OWNED);

	/* if the next block is actually what we thought it is,
	   then set the goal to what we thought it should be
	*/
	if (ip->i_next_alloc_block == lbn && ip->i_next_alloc_goal != 0)
		return ip->i_next_alloc_goal;

	/* now check whether we were provided with an array that basically
	   tells us previous blocks to which we want to stay closeby
	*/
	if (bap)
		for (tmp = indx - 1; tmp >= 0; tmp--) 
			if (bap[tmp]) 
				return bap[tmp];

	/* else let's fall back to the blocknr, or, if there is none,
	   follow the rule that a block should be allocated near its inode
	*/
	return blocknr ? blocknr :
			(e2fs_daddr_t)(ip->i_block_group * 
			EXT2_BLOCKS_PER_GROUP(ip->i_e2fs)) + 
			ip->i_e2fs->e2fs->e2fs_first_dblock;
}

/*
 * Implement the cylinder overflow algorithm.
 *
 * The policy implemented by this algorithm is:
 *   1) allocate the block in its requested cylinder group.
 *   2) quadradically rehash on the cylinder group number.
 *   3) brute force search for a free block.
 */
static u_long
ext2_hashalloc(struct inode *ip, int cg, long pref, int size,
                daddr64_t (*allocator)(struct inode *, int, daddr64_t, int))
{
	struct m_ext2fs *fs;
	ino_t result;
	int i, icg = cg;

	LCK_MTX_ASSERT(EXT2_MTX(ip->i_ump), LCK_MTX_ASSERT_OWNED);
	fs = ip->i_e2fs;
	/*
	 * 1: preferred cylinder group
	 */
	result = (*allocator)(ip, cg, pref, size);
	if (result)
		return (result);
	/*
	 * 2: quadratic rehash
	 */
	for (i = 1; i < (int)fs->e2fs_gcount; i *= 2) {
		cg += i;
		if (cg >= (int)fs->e2fs_gcount)
			cg -= fs->e2fs_gcount;
		result = (*allocator)(ip, cg, 0, size);
		if (result)
			return (result);
	}
	/*
	 * 3: brute force search
	 * Note that we start at i == 2, since 0 was checked initially,
	 * and 1 is always checked in the quadratic rehash.
	 */
	cg = (icg + 2) % fs->e2fs_gcount;
	for (i = 2; i < (int)fs->e2fs_gcount; i++) {
		result = (*allocator)(ip, cg, 0, size);
		if (result)
			return (result);
		cg++;
		if (cg == (int)fs->e2fs_gcount)
			cg = 0;
	}
	return (0);
}

/*
 * Determine whether a block can be allocated.
 *
 * Check to see if a block of the appropriate size is available,
 * and if it is, allocate it.
 */
static daddr64_t
ext2_alloccg(struct inode *ip, int cg, daddr64_t bpref, int size)
{
	struct m_ext2fs *fs;
	buf_t bp;
	struct ext2mount *ump;
	daddr64_t bno, runstart, runlen;
	int bit, loc, end, error, start;
	char *bbp;
	/* XXX ondisk32 */
	fs = ip->i_e2fs;
	ump = ip->i_ump;
	if (EXT2_GD(fs, cg)->ext2bgd_nbfree == 0)
		return (0);
	EXT2_UNLOCK(ump);
	error = buf_meta_bread(ip->i_devvp, fsbtodb(fs,
		EXT2_GD(fs, cg)->ext2bgd_b_bitmap),
		(int)fs->e2fs_bsize, NOCRED, &bp);
	if (error) {
		buf_brelse(bp);
		EXT2_LOCK(ump);
		return (0);
	}
	if (EXT2_GD(fs, cg)->ext2bgd_nbfree == 0) {
		/*
		 * Another thread allocated the last block in this
		 * group while we were waiting for the buffer.
		 */
		buf_brelse(bp);
		EXT2_LOCK(ump);
		return (0);
	}
	bbp = (char *)buf_dataptr(bp);

	if (dtog(fs, bpref) != cg)
		bpref = 0;
	if (bpref != 0) {
		bpref = dtogd(fs, bpref);
		/*
		 * if the requested block is available, use it
		 */
		if (isclr(bbp, bpref)) {
			bno = bpref;
			goto gotit;
		}
	}
	/*
	 * no blocks in the requested cylinder, so take next
	 * available one in this cylinder group.
	 * first try to get 8 contigous blocks, then fall back to a single
	 * block.
	 */
	if (bpref)
		start = dtogd(fs, bpref) / NBBY;
	else
		start = 0;
	end = howmany(fs->e2fs->e2fs_fpg, NBBY) - start;
retry:
	runlen = 0;
	runstart = 0;
	for (loc = start; loc < end; loc++) {
		if (bbp[loc] == (char)0xff) {
			runlen = 0;
			continue;
		}

		/*
		 * Start of a run, find the number of high clear bits.
		 *
		 * The (u_char) casts matter. bbp is a char *, char is signed
		 * here, and a bitmap byte from 0x80 upwards would otherwise be
		 * sign-extended on the way in: ext2_fls() would answer 32 and
		 * runlen would go negative, so a run could be reported where
		 * the bits are actually in use.
		 */
		if (runlen == 0) {
			bit = ext2_fls((u_char)bbp[loc]);
			runlen = NBBY - bit;
			runstart = loc * NBBY + bit;
		} else if (bbp[loc] == 0) {
			/* Continue a run. */
			runlen += NBBY;
		} else {
			/*
			 * Finish the current run.  If it isn't long
			 * enough, start a new one.
			 */
			bit = ffs((u_char)bbp[loc]) - 1;
			runlen += bit;
			if (runlen >= 8) {
				bno = runstart;
				goto gotit;
			}

			/* Run was too short, start a new one. */
			bit = ext2_fls((u_char)bbp[loc]);
			runlen = NBBY - bit;
			runstart = loc * NBBY + bit;
		}

		/* If the current run is long enough, use it. */
		if (runlen >= 8) {
			bno = runstart;
			goto gotit;
		}
	}
	if (start != 0) {
		end = start;
		start = 0;
		goto retry;
	}

	bno = ext2_mapsearch(fs, bbp, bpref);
	if (bno < 0){
		buf_brelse(bp);
		EXT2_LOCK(ump);
		return (0);
	}
gotit:
#ifdef INVARIANTS
	if (isset(bbp, bno)) {
		printf("ext2fs_alloccgblk: cg=%d bno=%jd fs=%s\n",
			cg, (intmax_t)bno, fs->e2fs_fsmnt);
		panic("ext2fs_alloccg: dup alloc");
	}
#endif
	setbit(bbp, bno);
	EXT2_LOCK(ump);
	ext2_clusteracct(fs, bbp, cg, bno, -1);
	fs->e2fs->e2fs_fbcount--;
	EXT2_GD(fs, cg)->ext2bgd_nbfree--;
	fs->e2fs_fmod = 1;
	EXT2_UNLOCK(ump);
	buf_bdwrite(bp);
	return (cg * fs->e2fs->e2fs_fpg + fs->e2fs->e2fs_first_dblock + bno);
}

/*
 * Determine whether a cluster can be allocated.
 */
static daddr64_t
ext2_clusteralloc(struct inode *ip, int cg, daddr64_t bpref, int len)
{
	struct m_ext2fs *fs;
	struct ext2mount *ump;
	buf_t bp;
	char *bbp;
	int bit, error, got, i, loc, run;
	int32_t *lp;
	daddr64_t bno;

	fs = ip->i_e2fs;
	ump = ip->i_ump;

	if (fs->e2fs_maxcluster[cg] < len)
		return (0);

	EXT2_UNLOCK(ump);
	error = buf_meta_bread(ip->i_devvp,
	    fsbtodb(fs, EXT2_GD(fs, cg)->ext2bgd_b_bitmap),
	    (int)fs->e2fs_bsize, NOCRED, &bp);
	if (error)
		goto fail_lock;

	bbp = (char *)buf_dataptr(bp);
	EXT2_LOCK(ump);
	/*
	 * Check to see if a cluster of the needed size (or bigger) is
	 * available in this cylinder group.
	 */
	lp = &fs->e2fs_clustersum[cg].cs_sum[len];
	for (i = len; i <= fs->e2fs_contigsumsize; i++)
		if (*lp++ > 0)
			break;
	if (i > fs->e2fs_contigsumsize) {
		/*
		 * Update the cluster summary information to reflect
		 * the true maximum-sized cluster so that future cluster
		 * allocation requests can avoid reading the bitmap only
		 * to find no cluster.
		 */
		lp = &fs->e2fs_clustersum[cg].cs_sum[len - 1];
			for (i = len - 1; i > 0; i--)
				if (*lp-- > 0)
					break;
		fs->e2fs_maxcluster[cg] = i;
		goto fail;
	}
	EXT2_UNLOCK(ump);

	/* Search the bitmap to find a big enough cluster like in FFS. */
	if (dtog(fs, bpref) != cg)
		bpref = 0;
	if (bpref != 0)
		bpref = dtogd(fs, bpref);
	loc = bpref / NBBY;
	bit = 1 << (bpref % NBBY);
	for (run = 0, got = bpref; got < (int)fs->e2fs->e2fs_fpg; got++) {
		if ((bbp[loc] & bit) != 0)
			run = 0;
		else {
			run++;
			if (run == len)
				break;
		}
		if ((got & (NBBY - 1)) != (NBBY - 1))
			bit <<= 1;
		else {
			loc++;
			bit = 1;
		}
	}

	if (got >= (int)fs->e2fs->e2fs_fpg)
		goto fail_lock;

	/* Allocate the cluster that we found. */
	for (i = 1; i < len; i++)
		if (!isclr(bbp, got - run + i))
			panic("ext2_clusteralloc: map mismatch");

	bno = got - run + 1;
	if (bno >= fs->e2fs->e2fs_fpg)
		panic("ext2_clusteralloc: allocated out of group");

	EXT2_LOCK(ump);
	for (i = 0; i < len; i += fs->e2fs_fpb) {
		setbit(bbp, bno + i);
		ext2_clusteracct(fs, bbp, cg, bno + i, -1);
		fs->e2fs->e2fs_fbcount--;
		EXT2_GD(fs, cg)->ext2bgd_nbfree--;
	}
	fs->e2fs_fmod = 1;
	EXT2_UNLOCK(ump);

	buf_bdwrite(bp);
	return (cg * fs->e2fs->e2fs_fpg + fs->e2fs->e2fs_first_dblock + bno);

fail_lock:
	EXT2_LOCK(ump);
fail:
	buf_brelse(bp);
	return (0);
}

/*
 * Determine whether an inode can be allocated.
 *
 * Check to see if an inode is available, and if it is,
 * allocate it using tode in the specified cylinder group.
 */
static daddr64_t
ext2_nodealloccg(struct inode *ip, int cg, daddr64_t ipref, int mode)
{
	struct m_ext2fs *fs;
	buf_t bp;
	struct ext2mount *ump;
	int error, start, len;
	char *ibp, *loc;
	ipref--; /* to avoid a lot of (ipref -1) */
	if (ipref == -1)
		ipref = 0;
	fs = ip->i_e2fs;
	ump = ip->i_ump;
	if (EXT2_GD(fs, cg)->ext2bgd_nifree == 0)
		return (0);
	EXT2_UNLOCK(ump);	
	error = buf_meta_bread(ip->i_devvp, fsbtodb(fs,
		EXT2_GD(fs, cg)->ext2bgd_i_bitmap),
		(int)fs->e2fs_bsize, NOCRED, &bp);
	if (error) {
		buf_brelse(bp);
		EXT2_LOCK(ump);
		return (0);
	}
	if (EXT2_GD(fs, cg)->ext2bgd_nifree == 0) {
		/*
		 * Another thread allocated the last i-node in this
		 * group while we were waiting for the buffer.
		 */
		buf_brelse(bp);
		EXT2_LOCK(ump);
		return (0);
	}
	ibp = (char *)buf_dataptr(bp);
	if (ipref) {
		ipref %= fs->e2fs->e2fs_ipg;
		if (isclr(ibp, ipref))
			goto gotit;
	}
	start = ipref / NBBY;
	len = howmany(fs->e2fs->e2fs_ipg - ipref, NBBY);
	loc = memcchr(&ibp[start], 0xff, len);
	if (loc == NULL) {
		len = start + 1;
		start = 0;
		loc = memcchr(&ibp[start], 0xff, len);
		if (loc == NULL) {
			printf("cg = %d, ipref = %lld, fs = %s\n",
				cg, (long long)ipref, fs->e2fs_fsmnt);
			panic("ext2fs_nodealloccg: map corrupted");
			/* NOTREACHED */
		}
	}
	ipref = (loc - ibp) * NBBY + ffs(~*loc) - 1;
gotit:
	setbit(ibp, ipref);
	EXT2_LOCK(ump);
	EXT2_GD(fs, cg)->ext2bgd_nifree--;
	fs->e2fs->e2fs_ficount--;
	fs->e2fs_fmod = 1;
	if ((mode & IFMT) == IFDIR) {
		EXT2_GD(fs, cg)->ext2bgd_ndirs++;
		fs->e2fs_total_dir++;
	}
	EXT2_UNLOCK(ump);
	buf_bdwrite(bp);
	return (cg * fs->e2fs->e2fs_ipg + ipref +1);
}

/*
 * Free a block or fragment.
 *
 */
void
ext2_blkfree(struct inode *ip, e4fs_daddr_t bno, long size)
{
	struct m_ext2fs *fs;
	buf_t bp;
	struct ext2mount *ump;
	int cg, error;
	char *bbp;

	fs = ip->i_e2fs;
	ump = ip->i_ump;
	cg = dtog(fs, bno);
	if ((u_int)bno >= fs->e2fs->e2fs_bcount) {
		printf("bad block %lld, ino %ju\n", (long long)bno,
		    (uintmax_t)ip->i_number);
		ext2_fserr(fs, ip->i_uid, "bad block");
		return;
	}
	error = buf_meta_bread(ip->i_devvp,
		fsbtodb(fs, EXT2_GD(fs, cg)->ext2bgd_b_bitmap),
		(int)fs->e2fs_bsize, NOCRED, &bp);
	if (error) {
		buf_brelse(bp);
		return;
	}
	bbp = (char *)buf_dataptr(bp);
	bno = dtogd(fs, bno);
	if (isclr(bbp, bno)) {
		printf("block = %lld, fs = %s\n",
		     (long long)bno, fs->e2fs_fsmnt);
		panic("ext2_blkfree: freeing free block");
	}
	clrbit(bbp, bno);
	EXT2_LOCK(ump);
	ext2_clusteracct(fs, bbp, cg, bno, 1);
	fs->e2fs->e2fs_fbcount++;
	EXT2_GD(fs, cg)->ext2bgd_nbfree++;
	fs->e2fs_fmod = 1;
	EXT2_UNLOCK(ump);
	buf_bdwrite(bp);
}

/*
 * Free an inode.
 *
 */
int
ext2_vfree(struct vnode *pvp, ino_t ino, int mode)
{
	struct m_ext2fs *fs;
	struct inode *pip;
	buf_t bp;
	struct ext2mount *ump;
	int error, cg;
	char * ibp;

	pip = VTOI(pvp);
	fs = pip->i_e2fs;
	ump = pip->i_ump;
	if ((u_int)ino > fs->e2fs_ipg * fs->e2fs_gcount)
		panic("ext2_vfree: range: devvp = %p, ino = %ju, fs = %s",
		    pip->i_devvp, (uintmax_t)ino, fs->e2fs_fsmnt);

	cg = ino_to_cg(fs, ino);
	error = buf_meta_bread(pip->i_devvp,
		fsbtodb(fs, EXT2_GD(fs, cg)->ext2bgd_i_bitmap),
		(int)fs->e2fs_bsize, NOCRED, &bp);
	if (error) {
		buf_brelse(bp);
		return (0);
	}
	ibp = (char *)buf_dataptr(bp);
	ino = (ino - 1) % fs->e2fs->e2fs_ipg;
	if (isclr(ibp, ino)) {
		printf("ino = %llu, fs = %s\n",
			 (unsigned long long)ino, fs->e2fs_fsmnt);
		if (fs->e2fs_ronly == 0)
			panic("ext2_vfree: freeing free inode");
	}
	clrbit(ibp, ino);
	EXT2_LOCK(ump);
	fs->e2fs->e2fs_ficount++;
	EXT2_GD(fs, cg)->ext2bgd_nifree++;
	if ((mode & IFMT) == IFDIR) {
		EXT2_GD(fs, cg)->ext2bgd_ndirs--;
		fs->e2fs_total_dir--;
	}
	fs->e2fs_fmod = 1;
	EXT2_UNLOCK(ump);
	buf_bdwrite(bp);
	return (0);
}

/*
 * Find a block in the specified cylinder group.
 *
 * It is a panic if a request is made to find a block if none are
 * available.
 */
static daddr64_t
ext2_mapsearch(struct m_ext2fs *fs, char *bbp, daddr64_t bpref)
{
	char *loc;
	int start, len;

	/*
	 * find the fragment by searching through the free block
	 * map for an appropriate bit pattern
	 */
	if (bpref)
		start = dtogd(fs, bpref) / NBBY;
	else
		start = 0;
	len = howmany(fs->e2fs->e2fs_fpg, NBBY) - start;
	loc = memcchr(&bbp[start], 0xff, len);
	if (loc == NULL) {
		len = start + 1;
		start = 0;
		loc = memcchr(&bbp[start], 0xff, len);
		if (loc == NULL) {
			printf("start = %d, len = %d, fs = %s\n",
				start, len, fs->e2fs_fsmnt);
			panic("ext2_mapsearch: map corrupted");
			/* NOTREACHED */
		}
	}
	return ((loc - bbp) * NBBY + ffs(~*loc) - 1);
}

/*
 * Fserr prints the name of a filesystem with an error diagnostic.
 * 
 * The form of the error message is:
 *	fs: error message
 */
static void
ext2_fserr(struct m_ext2fs *fs, uid_t uid, char *cp)
{

	log(LOG_ERR, "uid %u on %s: %s\n", uid, fs->e2fs_fsmnt, cp);
}

int
cg_has_sb(int i)
{
	int a3, a5, a7;

	if (i == 0 || i == 1)
		return 1;
	for (a3 = 3, a5 = 5, a7 = 7;
	    a3 <= i || a5 <= i || a7 <= i;
	    a3 *= 3, a5 *= 5, a7 *= 7)
		if (i == a3 || i == a5 || i == a7)
			return 1;
	return 0;
}
