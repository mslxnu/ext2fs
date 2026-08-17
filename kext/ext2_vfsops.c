/*-
 * Copyright (c) 1989, 1991, 1993, 1994
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
 *	@(#)ffs_vfsops.c	8.8 (Berkeley) 4/18/94
 * $FreeBSD$
 */

/*
 * ext2_vfsops.c - the VFS operations, written against the XNU KPI.
 *
 * Unlike the rest of the port this file is not a transformation of the
 * NextBSD original. Almost every entry point differs in shape - XNU hands
 * VFS_MOUNT a resolved device vnode, asks for statistics by name through
 * VFS_GETATTR, iterates a mount's vnodes on the file system's behalf, and
 * registers operations through a vfs_fsentry rather than a link-time macro -
 * so only the parts that are about ext2 rather than about the host survive
 * unchanged: ext2_check_sb_compat(), compute_sb_data(), ext2_sbupdate(),
 * ext2_cgupdate() and ext2_vget().
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/buf.h>
#include <sys/disk.h>
#include <sys/endian.h>	/* htole32, via include/bsdcompat */
#include <sys/fcntl.h>
#include <sys/kauth.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/ubc.h>
#include <sys/vnode.h>

#include <fs/ext2fs/fs.h>
#include <fs/ext2fs/ext2_mount.h>
#include <fs/ext2fs/inode.h>
#include <fs/ext2fs/ext2fs.h>
#include <fs/ext2fs/ext2_dinode.h>
#include <fs/ext2fs/ext2_extern.h>
#include <fs/ext2fs/ext2_apple.h>

static int	ext2_mount(struct mount *, vnode_t, user_addr_t, vfs_context_t);
static int	ext2_unmount(struct mount *, int, vfs_context_t);
static int	ext2_root(struct mount *, struct vnode **, vfs_context_t);
static int	ext2_vfs_getattr(struct mount *, struct vfs_attr *,
		    vfs_context_t);
static int	ext2_sync(struct mount *, int, vfs_context_t);
static int	ext2_fhtovp(struct mount *, int, unsigned char *,
		    struct vnode **, vfs_context_t);
static int	ext2_vptofh(struct vnode *, int *, unsigned char *,
		    vfs_context_t);
static int	ext2_vfs_init(struct vfsconf *);

static int	ext2_mountfs(struct vnode *, struct mount *, vfs_context_t);
static int	ext2_statfs_internal(struct mount *);
static int	ext2_flushfiles(struct mount *, int, vfs_context_t);
static int	ext2_check_sb_compat(struct ext2fs *, int);
static int	compute_sb_data(struct vnode *, struct ext2fs *,
		    struct m_ext2fs *);
static int	ext2_sbupdate(struct ext2mount *, int);
static int	ext2_cgupdate(struct ext2mount *, int);
static void	ext2_journal_replay(struct vnode *, struct m_ext2fs *);

extern struct vnodeopv_desc ext2fs_vnodeop_opv_desc;
extern int (**ext2_vnodeop_p)(void *);


/*
 * VFS_MOUNT.
 *
 * FreeBSD's ext2_mount() received only the mount and dug the device path and
 * the options out of a mount-argument namespace. XNU has already done that
 * work: registering with VFS_TBLLOCALVOL makes the vfs layer read the device
 * path from the front of struct ext2_args, resolve it, authorise it, open it,
 * and hand back devvp with the argument pointer advanced past it. It closes
 * the device again after VFS_UNMOUNT, which is why nothing here opens or
 * closes it - and why VNOP_OPEN and VNOP_CLOSE are not exported to kexts.
 *
 * Remounts are refused. Flipping a mounted volume between read-only and
 * read-write means reconciling in-core state with the disk, and quietly
 * accepting the request without doing that would be worse than saying no.
 */
static int
ext2_mount(struct mount *mp, vnode_t devvp, user_addr_t data, vfs_context_t ctx)
{
	struct ext2_mount_tail {
		uint32_t	e2_version;
		uint32_t	e2_flags;
	} tail;
	int error;

	if (vfs_isupdate(mp))
		return (ENOTSUP);

	if (data != USER_ADDR_NULL) {
		error = copyin(data, &tail, sizeof(tail));
		if (error != 0)
			return (error);
		if (tail.e2_version != EXT2_ARGSVERSION)
			return (EINVAL);
	}

	error = ext2_mountfs(devvp, mp, ctx);
	if (error != 0)
		return (error);

	(void)ext2_statfs_internal(mp);

	return (0);
}

/*
 * Read the superblock and build the in-core mount.
 *
 * FreeBSD reached the disk through GEOM - g_vfs_open() took an exclusive
 * consumer and installed g_vfs_bufops on the device vnode's bufobj. XNU has no
 * such layer: buffers go to the device vnode through the ordinary buffer
 * cache, so the topology locking, the consumer and the bufobj all disappear
 * with nothing to replace them.
 */
static int
ext2_mountfs(struct vnode *devvp, struct mount *mp, vfs_context_t ctx)
{
	struct ext2mount *ump = NULL;
	buf_t bp = NULL;
	struct m_ext2fs *fs;
	struct ext2fs *es;
	int error, i, size;
	int32_t *lp;
	struct csum *sump;
	int ronly;
	int e2fs_maxcontig;

	ronly = (vfs_flags(mp) & MNT_RDONLY) ? 1 : 0;

	error = buf_meta_bread(devvp, (daddr64_t)SBLOCK, SBSIZE, NOCRED, &bp);
	if (error != 0)
		goto out;
	es = (struct ext2fs *)buf_dataptr(bp);
	error = ext2_check_sb_compat(es, ronly);
	if (error == 2) {
		/* Mountable, but not the way it was asked for. */
		error = EROFS;
		goto out;
	} else if (error == 3) {
		/*
		 * The volume is well formed, it just uses features this driver
		 * does not implement. That is not EINVAL: mount(8) renders a
		 * bare EINVAL from a local file system as "specified device
		 * does not match mounted device", which sends the reader off
		 * to look at devices. ENOTSUP says what is actually wrong, and
		 * the printf above names the offending feature.
		 */
		error = ENOTSUP;
		goto out;
	} else if (error != 0) {
		error = EINVAL;
		goto out;
	}
	if ((es->e2fs_state & E2FS_ISCLEAN) == 0 ||
	    (es->e2fs_state & E2FS_ERRORS)) {
		if (ronly || (vfs_flags(mp) & MNT_FORCE)) {
			printf("ext2fs: WARNING: file system was not properly "
			    "dismounted\n");
		} else {
			printf("ext2fs: R/W mount denied, file system is not "
			    "clean - run fsck\n");
			error = EPERM;
			goto out;
		}
	}

	ump = _MALLOC(sizeof(*ump), M_TEMP, M_WAITOK | M_ZERO);
	if (ump == NULL) {
		error = ENOMEM;
		goto out;
	}
	ump->um_e2fs = _MALLOC(sizeof(struct m_ext2fs), M_TEMP,
	    M_WAITOK | M_ZERO);
	if (ump->um_e2fs == NULL) {
		error = ENOMEM;
		goto out;
	}
	ump->um_e2fs->e2fs = _MALLOC(sizeof(struct ext2fs), M_TEMP,
	    M_WAITOK | M_ZERO);
	if (ump->um_e2fs->e2fs == NULL) {
		error = ENOMEM;
		goto out;
	}

	/*
	 * FreeBSD initialised a mutex embedded in the mount; XNU's are
	 * allocated out of a lock group.
	 */
	ump->um_lock = lck_mtx_alloc_init(ext2_lck_grp, LCK_ATTR_NULL);
	if (ump->um_lock == NULL) {
		error = ENOMEM;
		goto out;
	}

	bcopy(es, ump->um_e2fs->e2fs, (u_int)sizeof(struct ext2fs));
	if ((error = compute_sb_data(devvp, ump->um_e2fs->e2fs, ump->um_e2fs)))
		goto out;

	buf_brelse(bp);
	bp = NULL;
	fs = ump->um_e2fs;
	fs->e2fs_ronly = (char)ronly;

	if (!ronly && (fs->e2fs->e2fs_features_incompat & EXT2F_INCOMPAT_RECOVER))
		ext2_journal_replay(devvp, fs);

	e2fs_maxcontig = MAX(1, MAXPHYS / fs->e2fs_bsize);
	fs->e2fs_contigsumsize = MIN(e2fs_maxcontig, EXT2_MAXCONTIG);
	if (fs->e2fs_contigsumsize > 0) {
		size = (int)(fs->e2fs_gcount * sizeof(int32_t));
		fs->e2fs_maxcluster = _MALLOC(size, M_TEMP, M_WAITOK | M_ZERO);
		size = (int)(fs->e2fs_gcount * sizeof(struct csum));
		/*
		 * M_ZERO matters here. The loop below fills in every cs_sum,
		 * but it can leave partway through on ENOMEM, and both the
		 * error path and ext2_unmount() then walk all e2fs_gcount
		 * entries handing each cs_sum to _FREE(). Uninitialised, the
		 * untouched tail would be heap garbage passed to the
		 * allocator as if it were a pointer.
		 */
		fs->e2fs_clustersum = _MALLOC(size, M_TEMP, M_WAITOK | M_ZERO);
		if (fs->e2fs_maxcluster == NULL ||
		    fs->e2fs_clustersum == NULL) {
			error = ENOMEM;
			goto out;
		}
		lp = fs->e2fs_maxcluster;
		sump = fs->e2fs_clustersum;
		for (i = 0; i < (int)fs->e2fs_gcount; i++, sump++) {
			*lp++ = fs->e2fs_contigsumsize;
			sump->cs_init = 0;
			sump->cs_sum = _MALLOC((fs->e2fs_contigsumsize + 1) *
			    sizeof(int32_t), M_TEMP, M_WAITOK | M_ZERO);
			if (sump->cs_sum == NULL) {
				error = ENOMEM;
				goto out;
			}
		}
	}

	fs->e2fs_wasvalid = fs->e2fs->e2fs_state & E2FS_ISCLEAN ? 1 : 0;
	if (ronly == 0) {
		fs->e2fs_fmod = 1;			/* mark it modified */
		fs->e2fs->e2fs_state &= ~E2FS_ISCLEAN;	/* set fs invalid */
	}

	/*
	 * Hand the in-core mount to the vfs layer. struct mount is opaque on
	 * XNU: the private pointer goes through vfs_setfsprivate() and the
	 * volume's identity through vfs_setflags() and vfs_getnewfsid().
	 */
	vfs_setfsprivate(mp, ump);
	vfs_setflags(mp, MNT_LOCAL);
	vfs_getnewfsid(mp);

	ump->um_mountp = mp;
	ump->um_devvp = devvp;

	/*
	 * Setting those two parameters allowed us to use
	 * ufs_bmap w/o changes!
	 */
	ump->um_nindir = EXT2_ADDR_PER_BLOCK(fs);
	ump->um_bptrtodb = fs->e2fs->e2fs_log_bsize + 1;
	ump->um_seqinc = EXT2_FRAGS_PER_BLOCK(fs);

	if (ronly == 0)
		ext2_sbupdate(ump, MNT_WAIT);

	(void)ctx;
	return (0);

out:
	if (bp != NULL)
		buf_brelse(bp);
	if (ump != NULL) {
		if (ump->um_lock != NULL)
			lck_mtx_free(ump->um_lock, ext2_lck_grp);
		if (ump->um_e2fs != NULL) {
			struct m_ext2fs *ofs = ump->um_e2fs;

			/*
			 * The cluster summary can be half-built if the loop
			 * above stopped on ENOMEM. Every cs_sum is either a
			 * live allocation or NULL, because the array is
			 * M_ZERO, so freeing the whole span is safe.
			 */
			if (ofs->e2fs_clustersum != NULL) {
				struct csum *osump = ofs->e2fs_clustersum;
				int j;

				for (j = 0; j < (int)ofs->e2fs_gcount;
				    j++, osump++)
					_FREE(osump->cs_sum, M_TEMP);
				_FREE(ofs->e2fs_clustersum, M_TEMP);
			}
			_FREE(ofs->e2fs_maxcluster, M_TEMP);
			_FREE(ofs->e2fs_gd, M_TEMP);
			_FREE(ofs->e2fs_contigdirs, M_TEMP);
			_FREE(ofs->e2fs, M_TEMP);
			_FREE(ofs, M_TEMP);
		}
		_FREE(ump, M_TEMP);
		vfs_setfsprivate(mp, NULL);
	}
	return (error);
}

static int
ext2_unmount(struct mount *mp, int mntflags, vfs_context_t ctx)
{
	struct ext2mount *ump;
	struct m_ext2fs *fs;
	struct csum *sump;
	int error, flags, i, ronly;

	flags = 0;
	if (mntflags & MNT_FORCE) {
		/*
		 * FreeBSD refused a forced unmount of the root file system.
		 * ext2fs is never the root volume here and XNU does not
		 * publish MNT_ROOTFS, so there is nothing to refuse.
		 */
		flags |= FORCECLOSE;
	}
	if ((error = ext2_flushfiles(mp, flags, ctx)) != 0)
		return (error);

	/*
	 * EXT2_UNMOUNT_STAGE is a diagnostic build switch - see kext/Makefile.
	 * Unset, it is 4 and this function behaves normally. Lower values stop
	 * partway through the teardown, leaking whatever is left, to bisect a
	 * machine freeze on umount that produces no panic. Stage 1 has already
	 * been shown to survive, which is what puts the fault below this line.
	 *
	 *   1  return now: nothing written back, nothing freed
	 *   2  + write back the superblock and invalidate the device's buffers
	 *   3  + free the superblock and the per-group arrays
	 *   4  + free the mount lock and the mount itself (normal)
	 *
	 * This is what found the vnode_put() below, by bisecting to the one
	 * stage that froze. Kept because the next fault here will want it.
	 */
#ifndef EXT2_UNMOUNT_STAGE
#define	EXT2_UNMOUNT_STAGE	4
#endif
#if EXT2_UNMOUNT_STAGE < 4
	printf("ext2fs: unmount stage %d of 4, leaking the rest\n",
	    EXT2_UNMOUNT_STAGE);
#endif

	ump = VFSTOEXT2(mp);
	fs = ump->um_e2fs;

#if EXT2_UNMOUNT_STAGE >= 2
	ronly = fs->e2fs_ronly;
	if (ronly == 0 && ext2_cgupdate(ump, MNT_WAIT) == 0) {
		if (fs->e2fs_wasvalid)
			fs->e2fs->e2fs_state |= E2FS_ISCLEAN;
		ext2_sbupdate(ump, MNT_WAIT);
	}

	/*
	 * Push anything still cached against the device, so a later mount does
	 * not see stale metadata. FreeBSD released a GEOM consumer here.
	 *
	 * Nothing else is done to the device vnode, and in particular it is not
	 * put. This file system never took a reference on it: the vfs layer
	 * resolves the path, references and opens the device before VFS_MOUNT,
	 * and unwinds all of that itself - vnode_rele() and VNOP_CLOSE() from
	 * dounmount() after this returns, and the I/O count from namei() dropped
	 * at the exit: label of mount_common(), where the comment reads "drop
	 * I/O count on the device vp if there was one".
	 *
	 * A vnode_put() here was therefore releasing a count we never held.
	 * vnode_put() panics on an I/O count below one, and it does so holding
	 * the vnode spin lock with preemption disabled, which is why it came out
	 * as an instant whole-machine freeze with no panic report rather than as
	 * a panic. It froze on every single unmount.
	 */
	(void)buf_invalidateblks(ump->um_devvp, BUF_WRITE_DATA, 0, 0);
#else
	(void)ronly;
	(void)fs;
#endif

#if EXT2_UNMOUNT_STAGE >= 3
	if (fs->e2fs_clustersum != NULL) {
		sump = fs->e2fs_clustersum;
		for (i = 0; i < (int)fs->e2fs_gcount; i++, sump++)
			_FREE(sump->cs_sum, M_TEMP);
		_FREE(fs->e2fs_clustersum, M_TEMP);
	}
	_FREE(fs->e2fs_maxcluster, M_TEMP);
	_FREE(fs->e2fs_gd, M_TEMP);
	_FREE(fs->e2fs_contigdirs, M_TEMP);
	_FREE(fs->e2fs, M_TEMP);
	_FREE(fs, M_TEMP);
#else
	(void)sump;
	(void)i;
#endif

#if EXT2_UNMOUNT_STAGE >= 4
	if (ump->um_lock != NULL)
		lck_mtx_free(ump->um_lock, ext2_lck_grp);
	_FREE(ump, M_TEMP);
#endif

	vfs_setfsprivate(mp, NULL);
	vfs_clearflags(mp, MNT_LOCAL);

	return (0);
}

static int
ext2_root(struct mount *mp, struct vnode **vpp, vfs_context_t ctx)
{
	struct vnode *nvp;
	int error;

	error = ext2_vget(mp, (ino64_t)EXT2_ROOTINO, &nvp, ctx);
	if (error != 0)
		return (error);
	*vpp = nvp;
	return (0);
}

/*
 * Work out the volume's block accounting and fill in the mount's cached
 * statfs.
 *
 * FreeBSD's VFS_STATFS filled a caller-supplied struct statfs. XNU keeps one
 * per mount, reachable through vfs_statfs(), and asks for individual
 * attributes through VFS_GETATTR - so the arithmetic lives here once and both
 * paths use the result.
 */
static int
ext2_statfs_internal(struct mount *mp)
{
	struct ext2mount *ump;
	struct m_ext2fs *fs;
	struct vfsstatfs *sbp;
	uint32_t overhead, overhead_per_group, ngdb;
	int i, ngroups;

	ump = VFSTOEXT2(mp);
	fs = ump->um_e2fs;
	sbp = vfs_statfs(mp);

	if (fs->e2fs->e2fs_magic != E2FS_MAGIC)
		return (EINVAL);

	/*
	 * Compute the overhead (FS structures).
	 */
	overhead_per_group =
	    1 /* block bitmap */ +
	    1 /* inode bitmap */ +
	    fs->e2fs_itpg;
	overhead = fs->e2fs->e2fs_first_dblock +
	    fs->e2fs_gcount * overhead_per_group;
	if (fs->e2fs->e2fs_rev > E2FS_REV0 &&
	    fs->e2fs->e2fs_features_rocompat & EXT2F_ROCOMPAT_SPARSESUPER) {
		for (i = 0, ngroups = 0; i < (int)fs->e2fs_gcount; i++) {
			if (cg_has_sb(i))
				ngroups++;
		}
	} else {
		ngroups = (int)fs->e2fs_gcount;
	}
	ngdb = fs->e2fs_gdbcount;
	if (fs->e2fs->e2fs_rev > E2FS_REV0 &&
	    fs->e2fs->e2fs_features_compat & EXT2F_COMPAT_RESIZE)
		ngdb += fs->e2fs->e2fs_reserved_ngdb;
	overhead += (uint32_t)ngroups * (1 /* superblock */ + ngdb);

	sbp->f_bsize = EXT2_FRAG_SIZE(fs);
	sbp->f_iosize = EXT2_BLOCK_SIZE(fs);
	sbp->f_blocks = fs->e2fs->e2fs_bcount - overhead;
	sbp->f_bfree = fs->e2fs->e2fs_fbcount;
	sbp->f_bavail = (sbp->f_bfree > fs->e2fs->e2fs_rbcount) ?
	    (sbp->f_bfree - fs->e2fs->e2fs_rbcount) : 0;
	sbp->f_files = fs->e2fs->e2fs_icount;
	sbp->f_ffree = fs->e2fs->e2fs_ficount;

	return (0);
}

/*
 * VFS_GETATTR. XNU names the attributes it wants; VFSATTR_RETURN records both
 * the value and the fact that it was supplied.
 */
static int
ext2_vfs_getattr(struct mount *mp, struct vfs_attr *fsap, vfs_context_t ctx)
{
	struct vfsstatfs *sbp;
	int error;

	if ((error = ext2_statfs_internal(mp)) != 0)
		return (error);
	sbp = vfs_statfs(mp);

	VFSATTR_RETURN(fsap, f_bsize, sbp->f_bsize);
	VFSATTR_RETURN(fsap, f_iosize, sbp->f_iosize);
	VFSATTR_RETURN(fsap, f_blocks, sbp->f_blocks);
	VFSATTR_RETURN(fsap, f_bfree, sbp->f_bfree);
	VFSATTR_RETURN(fsap, f_bavail, sbp->f_bavail);
	VFSATTR_RETURN(fsap, f_bused, sbp->f_blocks - sbp->f_bfree);
	VFSATTR_RETURN(fsap, f_files, sbp->f_files);
	VFSATTR_RETURN(fsap, f_ffree, sbp->f_ffree);

	if (VFSATTR_IS_ACTIVE(fsap, f_capabilities)) {
		vol_capabilities_attr_t *cap = &fsap->f_capabilities;

		/*
		 * capabilities says what the volume can do; valid says which
		 * of those answers this file system actually knows. Only the
		 * format bits are claimed - none of the interface capabilities
		 * (search-fs, exchange-data, extended attributes) are
		 * implemented.
		 */
		cap->capabilities[VOL_CAPABILITIES_FORMAT] =
		    VOL_CAP_FMT_SYMBOLICLINKS |
		    VOL_CAP_FMT_HARDLINKS |
		    VOL_CAP_FMT_SPARSE_FILES |
		    VOL_CAP_FMT_CASE_SENSITIVE |
		    VOL_CAP_FMT_CASE_PRESERVING |
		    VOL_CAP_FMT_FAST_STATFS;
		cap->capabilities[VOL_CAPABILITIES_INTERFACES] = 0;
		cap->capabilities[VOL_CAPABILITIES_RESERVED1] = 0;
		cap->capabilities[VOL_CAPABILITIES_RESERVED2] = 0;

		cap->valid[VOL_CAPABILITIES_FORMAT] =
		    VOL_CAP_FMT_PERSISTENTOBJECTIDS |
		    VOL_CAP_FMT_SYMBOLICLINKS |
		    VOL_CAP_FMT_HARDLINKS |
		    VOL_CAP_FMT_JOURNAL |
		    VOL_CAP_FMT_JOURNAL_ACTIVE |
		    VOL_CAP_FMT_NO_ROOT_TIMES |
		    VOL_CAP_FMT_SPARSE_FILES |
		    VOL_CAP_FMT_ZERO_RUNS |
		    VOL_CAP_FMT_CASE_SENSITIVE |
		    VOL_CAP_FMT_CASE_PRESERVING |
		    VOL_CAP_FMT_FAST_STATFS;
		cap->valid[VOL_CAPABILITIES_INTERFACES] = 0;
		cap->valid[VOL_CAPABILITIES_RESERVED1] = 0;
		cap->valid[VOL_CAPABILITIES_RESERVED2] = 0;

		VFSATTR_SET_SUPPORTED(fsap, f_capabilities);
	}

	(void)ctx;
	return (0);
}

/*
 * Flush the file system.
 *
 * FreeBSD walked the mount's vnode list itself with MNT_VNODE_FOREACH_ALL,
 * taking the mount interlock and each vnode's interlock as it went. XNU keeps
 * that list private and iterates it on the file system's behalf:
 * vnode_iterate() calls the callback with an iocount already held, so the
 * callback only decides whether the vnode needs writing and writes it.
 */
struct ext2_sync_arg {
	int	sa_waitfor;
	int	sa_error;
};

static int
ext2_sync_callback(struct vnode *vp, void *arg)
{
	struct ext2_sync_arg *sa = arg;
	struct inode *ip;
	int error;

	ip = VTOI(vp);
	if (ip == NULL)
		return (VNODE_RETURNED);

	if ((ip->i_flag &
	    (IN_ACCESS | IN_CHANGE | IN_MODIFIED | IN_UPDATE)) == 0)
		return (VNODE_RETURNED);

	error = ext2_update(vp, sa->sa_waitfor == MNT_WAIT);
	if (error != 0)
		sa->sa_error = error;

	return (VNODE_RETURNED);
}

static int
ext2_sync(struct mount *mp, int waitfor, vfs_context_t ctx)
{
	struct ext2mount *ump = VFSTOEXT2(mp);
	struct m_ext2fs *fs;
	struct ext2_sync_arg sa;
	struct timespec ts;
	int error;

	fs = ump->um_e2fs;
	if (fs->e2fs_fmod != 0 && fs->e2fs_ronly != 0) {
		printf("ext2fs: %s: modified on a read-only mount\n",
		    fs->e2fs_fsmnt);
		return (EINVAL);
	}

	sa.sa_waitfor = waitfor;
	sa.sa_error = 0;
	(void)vnode_iterate(mp, 0, ext2_sync_callback, &sa);

	/*
	 * Write back the modified superblock.
	 */
	if (fs->e2fs_fmod != 0) {
		fs->e2fs_fmod = 0;
		nanotime(&ts);
		fs->e2fs->e2fs_wtime = (uint32_t)ts.tv_sec;
		if ((error = ext2_cgupdate(ump, waitfor)) != 0)
			sa.sa_error = error;
	}

	(void)ctx;
	return (sa.sa_error);
}

/*
 * File handles.
 *
 * XNU passes an opaque buffer and its length rather than FreeBSD's struct fid,
 * so the layout is the file system's to choose. The generation number is what
 * makes a handle to a since-reused inode detectable as stale.
 */
struct ext2fid {
	uint32_t	ext2fid_ino;
	uint32_t	ext2fid_gen;
};

static int
ext2_fhtovp(struct mount *mp, int fhlen, unsigned char *fhp,
    struct vnode **vpp, vfs_context_t ctx)
{
	struct ext2fid *ufhp = (struct ext2fid *)fhp;
	struct m_ext2fs *fs;
	struct inode *ip;
	struct vnode *nvp;
	int error;

	*vpp = NULL;
	if (fhlen < (int)sizeof(struct ext2fid))
		return (EINVAL);

	fs = VFSTOEXT2(mp)->um_e2fs;
	if (ufhp->ext2fid_ino < EXT2_ROOTINO ||
	    ufhp->ext2fid_ino > fs->e2fs_gcount * fs->e2fs->e2fs_ipg)
		return (ESTALE);

	error = ext2_vget(mp, (ino64_t)ufhp->ext2fid_ino, &nvp, ctx);
	if (error != 0)
		return (error);

	ip = VTOI(nvp);
	if (ip->i_mode == 0 || ip->i_gen != ufhp->ext2fid_gen ||
	    ip->i_nlink <= 0) {
		vnode_put(nvp);
		return (ESTALE);
	}
	*vpp = nvp;
	return (0);
}

static int
ext2_vptofh(struct vnode *vp, int *fhlenp, unsigned char *fhp,
    vfs_context_t ctx)
{
	struct inode *ip;
	struct ext2fid *ufhp;

	if (*fhlenp < (int)sizeof(struct ext2fid)) {
		*fhlenp = (int)sizeof(struct ext2fid);
		return (EOVERFLOW);
	}

	ip = VTOI(vp);
	ufhp = (struct ext2fid *)fhp;
	ufhp->ext2fid_ino = (uint32_t)ip->i_number;
	ufhp->ext2fid_gen = ip->i_gen;
	*fhlenp = (int)sizeof(struct ext2fid);

	(void)ctx;
	return (0);
}

/*
 * VFS_INIT. The allocation tag, lock group and inode hash are created by the
 * kext start routine, which runs before any mount, so there is nothing left to
 * do per file system type.
 */
static int
ext2_vfs_init(struct vfsconf *vfsp)
{

	(void)vfsp;
	return (0);
}

/*
 * Flush all the vnodes on a mount.
 *
 * FreeBSD's vflush() took a thread; XNU's takes the vfs_context, and the
 * "skip this vnode" argument is a vnode rather than a count of references to
 * tolerate.
 */
static int
ext2_flushfiles(struct mount *mp, int flags, vfs_context_t ctx)
{

	return (vflush(mp, NULLVP, flags));
	(void)ctx;
}

/*
 * Decide whether this superblock describes a file system we can mount, and
 * whether we can write to it.
 *
 * FreeBSD's version checked only the magic number and the feature masks. The
 * geometry checks below come from OpenBSD's e2fs_sbcheck(), and they matter
 * here for a specific reason: every field is attacker-controlled if the volume
 * is, and two of them are used in arithmetic that goes badly wrong on bad
 * input. e2fs_log_bsize feeds shift counts - e2fs_bshift and e2fs_fsbtodb -
 * where a large value is undefined behaviour rather than merely a wrong
 * answer. e2fs_bpg is a divisor in dtog() and ino_to_cg(), and on arm64 an
 * integer divide by zero yields zero instead of trapping, so a zero there
 * would not crash: it would quietly place every block in group 0.
 */
static int
ext2_check_sb_compat(struct ext2fs *es, int ronly)
{
	uint32_t mask, tmp;
	size_t i;

	if (es->e2fs_magic != E2FS_MAGIC) {
		printf("ext2fs: wrong magic number %#x (expected %#x)\n",
		    es->e2fs_magic, E2FS_MAGIC);
		return (1);
	}

	/* Skewed log(block size): 1024 -> 0, 2048 -> 1, 4096 -> 2. */
	if (es->e2fs_log_bsize > 2) {
		printf("ext2fs: unsupported block size 2^%u\n",
		    es->e2fs_log_bsize + 10);
		return (1);
	}

	if (es->e2fs_bpg == 0) {
		printf("ext2fs: zero blocks per group\n");
		return (1);
	}

	if (es->e2fs_rev > E2FS_REV1) {
		printf("ext2fs: unsupported revision number %#x\n",
		    es->e2fs_rev);
		return (1);
	}
	if (es->e2fs_rev == E2FS_REV0)
		return (0);

	if (es->e2fs_first_ino != EXT2_FIRSTINO) {
		printf("ext2fs: unsupported first inode %u\n",
		    es->e2fs_first_ino);
		return (1);
	}

	/*
	 * Incompatible features split three ways: those we implement, those we
	 * can only read, and the rest.
	 */
	tmp = es->e2fs_features_incompat;
	mask = tmp & ~(EXT2F_INCOMPAT_SUPP | EXT4F_RO_INCOMPAT_SUPP);
	if (mask != 0) {
		printf("ext2fs: unsupported incompat features:");
		for (i = 0; i < nitems(ext2_incompat_names); i++)
			if (mask & ext2_incompat_names[i].mask)
				printf(" %s", ext2_incompat_names[i].name);
		printf("\n");
		return (3);
	}

	/*
	 * What is left after removing everything we implement is the set that
	 * forces read-only. Clearing EXT2F_INCOMPAT_SUPP rather than a
	 * hand-written subset of it matters: the list here used to name
	 * extents, flex_bg and meta_bg but not filetype, so every volume with
	 * filetype set - which is every volume mke2fs has produced by default
	 * for twenty years - was refused a read-write mount with EROFS.
	 */
	tmp &= ~EXT2F_INCOMPAT_SUPP;
	if (!ronly && tmp != 0) {
		printf("ext2fs: read-only mount required for:");
		for (i = 0; i < nitems(ext2_incompat_names); i++)
			if (tmp & ext2_incompat_names[i].mask)
				printf(" %s", ext2_incompat_names[i].name);
		printf("\n");
		return (2);
	}

	tmp = es->e2fs_features_rocompat & ~EXT2F_ROCOMPAT_SUPP;
	if (!ronly && tmp != 0) {
		printf("ext2fs: read-only mount required for:");
		for (i = 0; i < nitems(ext2_rocompat_names); i++)
			if (tmp & ext2_rocompat_names[i].mask)
				printf(" %s", ext2_rocompat_names[i].name);
		printf("\n");
		return (2);
	}

	return (0);
}

static int
compute_sb_data(struct vnode *devvp, struct ext2fs *es,
    struct m_ext2fs *fs)
{
	int db_count, error;
	int i;
	int logic_sb_block = 1;	/* XXX for now */
	buf_t bp;
	uint32_t e2fs_descpb;

	fs->e2fs_bshift = EXT2_MIN_BLOCK_LOG_SIZE + es->e2fs_log_bsize;
	fs->e2fs_bsize = 1U << fs->e2fs_bshift;
	fs->e2fs_fsbtodb = es->e2fs_log_bsize + 1;
	fs->e2fs_qbmask = fs->e2fs_bsize - 1;
	fs->e2fs_fsize = EXT2_MIN_FRAG_SIZE << es->e2fs_log_fsize;
	if (fs->e2fs_fsize)
		fs->e2fs_fpb = fs->e2fs_bsize / fs->e2fs_fsize;
	fs->e2fs_bpg = es->e2fs_bpg;
	fs->e2fs_fpg = es->e2fs_fpg;
	fs->e2fs_ipg = es->e2fs_ipg;
	if (es->e2fs_rev == E2FS_REV0) {
		fs->e2fs_isize = E2FS_REV0_INODE_SIZE ;
	} else {
		fs->e2fs_isize = es->e2fs_inode_size;

		/*
		 * Simple sanity check for superblock inode size value.
		 */
		if (EXT2_INODE_SIZE(fs) < E2FS_REV0_INODE_SIZE ||
		    EXT2_INODE_SIZE(fs) > fs->e2fs_bsize ||
		    (fs->e2fs_isize & (fs->e2fs_isize - 1)) != 0) {
			printf("ext2fs: invalid inode size %d\n",
			    fs->e2fs_isize);
			return (EIO);
		}
	}
	/* Check for extra isize in big inodes. */
	if (EXT2_HAS_RO_COMPAT_FEATURE(fs, EXT2F_ROCOMPAT_EXTRA_ISIZE) &&
	    EXT2_INODE_SIZE(fs) < sizeof(struct ext2fs_dinode)) {
		printf("ext2fs: no space for extra inode timestamps\n");
		return (EINVAL);
	}

	fs->e2fs_ipb = fs->e2fs_bsize / EXT2_INODE_SIZE(fs);
	fs->e2fs_itpg = fs->e2fs_ipg / fs->e2fs_ipb;
	/* s_resuid / s_resgid ? */
	fs->e2fs_gcount = (es->e2fs_bcount - es->e2fs_first_dblock +
	    EXT2_BLOCKS_PER_GROUP(fs) - 1) / EXT2_BLOCKS_PER_GROUP(fs);
	/*
	 * Group descriptors are EXT2_MIN_GD_SIZE bytes unless the volume
	 * carries the 64bit feature, in which case the superblock says how
	 * big they really are - 64, for anything mke2fs writes with
	 * metadata_csum. Everything that walks the table has to use this
	 * stride, which is what EXT2_GD() is for; taking it from
	 * sizeof(struct ext2_gd) instead read every group after the first
	 * from the wrong offset.
	 */
	fs->e2fs_gdsize = EXT2_MIN_GD_SIZE;
	if (es->e2fs_rev > E2FS_REV0 &&
	    (es->e2fs_features_incompat & EXT2F_INCOMPAT_64BIT) &&
	    es->e3fs_desc_size != 0)
		fs->e2fs_gdsize = es->e3fs_desc_size;
	if (fs->e2fs_gdsize < EXT2_MIN_GD_SIZE ||
	    (fs->e2fs_gdsize & (fs->e2fs_gdsize - 1)) != 0 ||
	    fs->e2fs_gdsize > fs->e2fs_bsize) {
		printf("ext2fs: nonsensical group descriptor size %u\n",
		    fs->e2fs_gdsize);
		return (EINVAL);
	}

	e2fs_descpb = fs->e2fs_bsize / fs->e2fs_gdsize;
	db_count = (fs->e2fs_gcount + e2fs_descpb - 1) / e2fs_descpb;
	fs->e2fs_gdbcount = db_count;
	fs->e2fs_gd = _MALLOC(db_count * fs->e2fs_bsize,
	    M_TEMP, M_WAITOK);
	fs->e2fs_contigdirs = _MALLOC(fs->e2fs_gcount *
	    sizeof(*fs->e2fs_contigdirs), M_TEMP, M_WAITOK | M_ZERO);

	/*
	 * Adjust logic_sb_block.
	 * Godmar thinks: if the blocksize is greater than 1024, then
	 * the superblock is logically part of block zero.
	 */
	if(fs->e2fs_bsize > SBSIZE)
		logic_sb_block = 0;
	for (i = 0; i < db_count; i++) {
		error = buf_meta_bread(devvp ,
			 fsbtodb(fs, logic_sb_block + i + 1 ),
			fs->e2fs_bsize, NOCRED, &bp);
		if (error) {
			_FREE(fs->e2fs_contigdirs, M_TEMP);
			_FREE(fs->e2fs_gd, M_TEMP);
			buf_brelse(bp);
			return (error);
		}
		e2fs_cgload((struct ext2_gd *)buf_dataptr(bp),
		    (struct ext2_gd *)((char *)fs->e2fs_gd +
			(size_t)i * fs->e2fs_bsize),
		    fs->e2fs_bsize);
		buf_brelse(bp);
		bp = NULL;
	}
	/* Initialization for the ext2 Orlov allocator variant. */
	fs->e2fs_total_dir = 0;
	for (i = 0; i < (int)fs->e2fs_gcount; i++)
		fs->e2fs_total_dir += EXT2_GD(fs, i)->ext2bgd_ndirs;

	/*
	 * Largest file this volume can hold.
	 *
	 * Two limits apply and the smaller wins. Logically, a file cannot
	 * outgrow the block pointer tree: twelve direct blocks plus one, two
	 * and three levels of indirection, b = bsize/4 pointers per block.
	 * Physically, i_blocks counts 512-byte units in a 32-bit field, so
	 * without the huge_file feature the file is capped at UINT_MAX of
	 * them; huge_file widens that to 48 bits.
	 *
	 * The previous value here was INT64_MAX whenever large_file was set,
	 * which is off by orders of magnitude - it let ext2_write() and
	 * ext2_truncate() accept offsets the block tree cannot address, so
	 * the failure surfaced later, from ext2_getlbns(), after the file had
	 * already been extended. Taken from OpenBSD's ext2fs_maxfilesize().
	 */
	if (es->e2fs_rev == E2FS_REV0 ||
	    !EXT2_HAS_RO_COMPAT_FEATURE(fs, EXT2F_ROCOMPAT_LARGEFILE)) {
		fs->e2fs_maxfilesize = 0x7fffffff;
	} else {
		int huge = EXT2_HAS_RO_COMPAT_FEATURE(fs,
		    EXT2F_ROCOMPAT_HUGE_FILE) ? 1 : 0;
		off_t b = fs->e2fs_bsize / 4;
		off_t physically, logically;

		physically = dbtob(huge ? ((off_t)1 << 48) - 1 :
		    (off_t)UINT_MAX);
		logically = (12 + b + b * b + b * b * b) *
		    (off_t)fs->e2fs_bsize;

		fs->e2fs_maxfilesize = MIN(logically, physically);
	}
	return (0);
}

int
ext2_vget(struct mount *mp, ino64_t ino, struct vnode **vpp, vfs_context_t ctx)
{

	return (ext2_vget_typed(mp, ino, 0, vpp, ctx));
}

/*
 * The body of ext2_vget(), plus a way to say what the inode is about to
 * become.
 *
 * XNU fixes a vnode's type at vnode_create() time and offers no way to change
 * it afterwards, so IFTOVT(ip->i_mode) has to be right before the vnode
 * exists. That is fine for an inode being read off disk, but ext2_valloc()
 * calls this for one it has just allocated, whose on-disk mode is zero or
 * stale from whatever last used the slot. IFTOVT(0) is VNON, so the new
 * directory or file came back as a vnode of no type at all, and the first
 * write to it fell through ext2_write()'s "not a directory, not a regular
 * file" arm and returned EPERM. Passing the intended mode in vmode fixes the
 * type at the only moment it can be set.
 */
int
ext2_vget_typed(struct mount *mp, ino64_t ino, mode_t vmode,
    struct vnode **vpp, vfs_context_t ctx)
{
	struct m_ext2fs *fs;
	struct inode *ip;
	struct ext2mount *ump;
	struct vnode_fsparam vfsp;
	buf_t bp;
	struct vnode *vp;
	int i, error;
	int used_blocks;

	*vpp = NULL;
	ump = VFSTOEXT2(mp);
	fs = ump->um_e2fs;

	/*
	 * Already in core? FreeBSD asked the generic vfs_hash; this file
	 * system keeps its own table, and a hit comes back with an iocount
	 * held.
	 */
	error = ext2_ihashget(ump, ino, vpp);
	if (error != 0)
		return (error);
	if (*vpp != NULL)
		return (0);

	ip = _MALLOC(sizeof(struct inode), M_TEMP, M_WAITOK | M_ZERO);
	if (ip == NULL)
		return (ENOMEM);

	ip->i_e2fs = fs;
	ip->i_ump = ump;
	ip->i_number = ino;

	ip->i_lock = lck_mtx_alloc_init(ext2_lck_grp, LCK_ATTR_NULL);
	if (ip->i_lock == NULL) {
		_FREE(ip, M_TEMP);
		return (ENOMEM);
	}

	/*
	 * Read the inode in before the vnode is created. XNU wants the vnode's
	 * type and, for a device, its rdev at vnode_create() time - it uses
	 * them to pick the operation vector and to find any existing alias -
	 * so unlike FreeBSD, which made the vnode first and filled it in
	 * afterwards, the on-disk inode has to be in hand first.
	 */
	error = buf_meta_bread(ump->um_devvp,
	    (daddr64_t)fsbtodb(fs, ino_to_fsba(fs, ino)),
	    (int)fs->e2fs_bsize, NOCRED, &bp);
	if (error != 0) {
		if (bp != NULL)
			buf_brelse(bp);
		lck_mtx_free(ip->i_lock, ext2_lck_grp);
		_FREE(ip, M_TEMP);
		return (error);
	}

	/* convert ext2 inode to dinode */
	ext2_ei2i((struct ext2fs_dinode *)((char *)buf_dataptr(bp) +
	    EXT2_INODE_SIZE(fs) * ino_to_fsbo(fs, ino)), ip);
	ip->i_block_group = ino_to_cg(fs, ino);
	ip->i_next_alloc_block = 0;
	ip->i_next_alloc_goal = 0;

	/*
	 * Now we want to make sure that block pointers for unused
	 * blocks are zeroed out - ext2_balloc depends on this
	 * although for regular files and directories only
	 *
	 * If IN_E4EXTENTS is enabled, unused blocks are not zeroed
	 * out because we could corrupt the extent tree.
	 */
	if (!(ip->i_flag & IN_E4EXTENTS) &&
	    (S_ISDIR(ip->i_mode) || S_ISREG(ip->i_mode))) {
		used_blocks = (int)((ip->i_size + fs->e2fs_bsize - 1) /
		    fs->e2fs_bsize);
		for (i = used_blocks; i < EXT2_NDIR_BLOCKS; i++)
			ip->i_db[i] = 0;
	}
	buf_brelse(bp);

	/*
	 * Set up a generation number for this inode if it does not
	 * already have one. This should only happen on old filesystems.
	 */
	if (ip->i_gen == 0) {
		uint32_t gen;

		read_random(&gen, sizeof(gen));
		ip->i_gen = gen + 1;
		if ((vfs_flags(mp) & MNT_RDONLY) == 0)
			ip->i_flag |= IN_MODIFIED;
	}

	/*
	 * Build the vnode.
	 *
	 * vnfs_vtype and vnfs_rdev are what let XNU route a device or fifo to
	 * its own operation vectors instead of ours - that is why this file
	 * system has no fifo vector of its own, and why ext2_mknod has to
	 * recycle a vnode when it learns the inode is a device.
	 *
	 * vnfs_marksystem stays 0 and vnfs_cnp is NULL: the name cache entry
	 * is added by the lookup path, which has the componentname; vget can
	 * be reached without one, as it is from ext2_valloc.
	 */
	bzero(&vfsp, sizeof(vfsp));
	vfsp.vnfs_mp = mp;
	vfsp.vnfs_vtype = IFTOVT(vmode != 0 ? vmode : ip->i_mode);
	vfsp.vnfs_str = "ext2fs";
	vfsp.vnfs_dvp = NULL;
	vfsp.vnfs_fsnode = ip;
	vfsp.vnfs_vops = ext2_vnodeop_p;
	vfsp.vnfs_markroot = (ino == EXT2_ROOTINO);
	vfsp.vnfs_marksystem = 0;
	vfsp.vnfs_rdev = (vmode == 0 && (vfsp.vnfs_vtype == VBLK ||
	    vfsp.vnfs_vtype == VCHR)) ? (dev_t)ip->i_rdev : 0;
	vfsp.vnfs_filesize = (vmode != 0) ? 0 : ip->i_size;
	vfsp.vnfs_cnp = NULL;
	vfsp.vnfs_flags = VNFS_ADDFSREF | VNFS_NOCACHE;

	error = vnode_create(VNCREATE_FLAVOR, VCREATESIZE, &vfsp, &vp);
	if (error != 0) {
		lck_mtx_free(ip->i_lock, ext2_lck_grp);
		_FREE(ip, M_TEMP);
		return (error);
	}

	ip->i_vnode = vp;
	ip->i_vid = vnode_vid(vp);

	/*
	 * Publish it only now that it is fully built: a concurrent
	 * ext2_ihashget() that found it earlier would have taken a reference
	 * to a vnode with no inode behind it.
	 */
	ext2_ihashins(ip);

	*vpp = vp;
	return (0);
}

static int
ext2_sbupdate(struct ext2mount *mp, int waitfor)
{
	struct m_ext2fs *fs = mp->um_e2fs;
	struct ext2fs *es = fs->e2fs;
	buf_t bp;
	int error = 0;

	bp = buf_getblk(mp->um_devvp, (daddr64_t)SBLOCK, SBSIZE, 0, 0,
	    BLK_META);
	if (bp == NULL)
		return (EIO);
	bcopy((caddr_t)es, (caddr_t)buf_dataptr(bp),
	    (u_int)sizeof(struct ext2fs));
	if (waitfor == MNT_WAIT)
		error = buf_bwrite(bp);
	else
		buf_bawrite(bp);

	/*
	 * The buffers for group descriptors, inode bitmaps and block bitmaps
	 * are not busy at this point and are (hopefully) written by the
	 * usual sync mechanism. No need to write them here.
	 */
	return (error);
}

int
ext2_cgupdate(struct ext2mount *mp, int waitfor)
{
	struct m_ext2fs *fs = mp->um_e2fs;
	buf_t bp;
	int i, error = 0, allerror = 0;

	allerror = ext2_sbupdate(mp, waitfor);
	for (i = 0; i < (int)fs->e2fs_gdbcount; i++) {
		bp = buf_getblk(mp->um_devvp, (daddr64_t)fsbtodb(fs,
		    fs->e2fs->e2fs_first_dblock +
		    1 /* superblock */ + i), fs->e2fs_bsize, 0, 0, BLK_META);
		if (bp == NULL) {
			allerror = EIO;
			break;
		}
		e2fs_cgsave((struct ext2_gd *)((char *)fs->e2fs_gd +
		    (size_t)i * fs->e2fs_bsize),
		    (struct ext2_gd *)buf_dataptr(bp), fs->e2fs_bsize);
		if (waitfor == MNT_WAIT)
			error = buf_bwrite(bp);
		else
			buf_bawrite(bp);
	}

	if (!allerror && error)
		allerror = error;
	return (allerror);
}

/*
 * VFS operation vector and registration record.
 *
 * FreeBSD's VFS_SET() macro wired the file system in at link time. XNU takes a
 * struct vfsops of named entry points plus a vfs_fsentry describing the file
 * system, and vfs_fsadd() - called from the kext start routine - installs
 * both.
 *
 * VFS_TBLLOCALVOL is what makes the vfs layer read the device path from the
 * front of struct ext2_args, resolve and open it, and pass the resulting
 * devvp to VFS_MOUNT. That is why ext2_mount() takes a vnode rather than
 * parsing a path, and why fspec has to remain the first member of that
 * structure. VFS_TBLNOTYPENUM asks for a dynamically assigned file system
 * type number instead of one of the historical fixed values.
 *
 * VFS_TBLTHREADSAFE and VFS_TBLFSNODELOCK are not optional. vfs_fsadd()
 * rejects an entry that sets neither, outright, before it looks at anything
 * else - "Non-threadsafe filesystems are not supported":
 *
 *	if ((vfe->vfe_flags & (VFS_TBLTHREADSAFE | VFS_TBLFSNODELOCK)) == 0)
 *		return EINVAL;
 *
 * They are the last trace of the funnel: neither is turned into a vfc_vfsflag
 * or consulted again anywhere, so the check is purely an assertion that the
 * file system does its own locking. Both statements are true here - ext2fs
 * takes its own inode and mount locks - so both are set. Omitting them is why
 * the kext loaded but its start routine returned failure.
 *
 * VFS_TBL64BITREADY only feeds vfs_64bitready() and vnode_vfs64bitready(),
 * which a file system queries about its own mount; every file system in XNU's
 * own vfs_conf.c sets it.
 *
 * Deliberately not set:
 *
 *   VFS_TBLVNOP_PAGEINV2 / _PAGEOUTV2  ext2_pagein() and ext2_pageout() use
 *      the v1 convention, taking the UPL the caller already built in a_pl.
 *      The v2 convention hands the file system a null a_pl and expects it to
 *      create the UPL itself, so claiming it would fault on the first page.
 *   VFS_TBLREADDIR_EXTENDED  ext2_readdir() rejects VNODE_READDIR_EXTENDED.
 *   VFS_TBLNATIVEXATTR  there are no xattr vnops.
 */

/*
 * Journal block device header - every journal block starts with this.
 */
struct jbd_header {
	uint32_t h_magic;
	uint32_t h_blocktype;
	uint32_t h_sequence;
};

#define	JBD_MAGIC		0xc03b3998
#define	JBD_DESCRIPTOR_BLOCK	1
#define	JBD_COMMIT_BLOCK	2
#define	JBD_SUPERBLOCK_V1	3
#define	JBD_REVOKE_BLOCK	5

/*
 * Replay committed transactions from the ext3 journal. The journal inode
 * number is in the superblock's e3fs_journal_inum field (defaults to 8).
 * This is a minimal replay: it walks the journal from s_start, finds
 * committed transactions, and copies their data blocks back to the main
 * filesystem.
 */
void
ext2_journal_replay(struct vnode *devvp, struct m_ext2fs *fs)
{
	struct ext2fs *es = fs->e2fs;
	buf_t jsb_bp = NULL, *jbp = NULL;
	struct jbd_header *hdr;
	ino_t jino;
	uint32_t start, sequence, next_sequence;
	uint32_t blocktype, tag_blocknr, nblocks = 0;
	uint32_t *tags, *tagp;
	int error, i;
	char *jdata;

	printf("ext2fs: replaying ext3 journal\n");

	jino = es->e3fs_journal_inum;
	if (jino == 0)
		jino = EXT2_JOURNALINO;

	start = es->e3fs_jnl_blks[0];
	if (start == 0) {
		printf("ext2fs: journal: already clean\n");
		goto out;
	}
	sequence = es->e3fs_jnl_blks[1];
	if (sequence == 0)
		sequence = 1;

	nblocks = es->e3fs_jnl_blks[2];
	if (nblocks == 0) {
		printf("ext2fs: journal: zero length\n");
		goto out;
	}

	next_sequence = sequence;
	jbp = _MALLOC(sizeof(*jbp) * (size_t)nblocks, M_TEMP, M_WAITOK | M_ZERO);
	for (i = 0; i < (int)nblocks; i++) {
		uint32_t dbn = start + (uint32_t)i;
		if (dbn >= nblocks)
			dbn -= nblocks;

		error = buf_meta_bread(devvp, (daddr64_t)dbn,
		    (int)fs->e2fs_bsize, NOCRED, &jbp[i]);
		if (error)
			break;

		hdr = (struct jbd_header *)buf_dataptr(jbp[i]);
		if (hdr->h_magic != htole32(JBD_MAGIC)) {
			buf_brelse(jbp[i]);
			jbp[i] = NULL;
			break;
		}

		blocktype = htole32(hdr->h_blocktype);
		if (hdr->h_sequence != htole32(next_sequence)) {
			buf_brelse(jbp[i]);
			jbp[i] = NULL;
			break;
		}

		if (blocktype == JBD_COMMIT_BLOCK) {
			next_sequence++;
		} else if (blocktype == JBD_DESCRIPTOR_BLOCK) {
			tags = (uint32_t *)((char *)hdr + sizeof(*hdr));
			tagp = tags;
			while ((char *)tagp + 8 <= (char *)buf_dataptr(jbp[i]) +
			    fs->e2fs_bsize) {
				tag_blocknr = tagp[0];
				if (tag_blocknr == 0)
					break;
				tag_blocknr = htole32(tag_blocknr);

				i++;
				if (start + i >= nblocks)
					i -= nblocks;
				if (jbp[i] != NULL) {
					jdata = (char *)buf_dataptr(jbp[i]);
					if (*(uint32_t *)jdata == JBD_MAGIC)
						jdata += sizeof(struct jbd_header);
					jsb_bp = buf_getblk(devvp,
					    (daddr64_t)tag_blocknr,
					    (int)fs->e2fs_bsize, 0, 0,
					    BLK_META);
					if (jsb_bp != NULL) {
						bcopy((void *)jdata,
						    (void *)buf_dataptr(jsb_bp),
						    fs->e2fs_bsize);
						buf_bwrite(jsb_bp);
					}
				}

				tagp += 2;
				if (tagp[1] & htole32(0x08))
					break;
			}
		}
	}

	es->e3fs_jnl_blks[0] = 0;
	es->e3fs_jnl_blks[1] = next_sequence;
	fs->e2fs_fmod = 1;

	printf("ext2fs: journal replayed, %u transactions\n",
	    next_sequence - sequence);

out:
	for (i = 0; i < (int)nblocks && jbp != NULL; i++) {
		if (jbp[i] != NULL)
			buf_brelse(jbp[i]);
	}
	if (jbp != NULL)
		_FREE(jbp, M_TEMP);
	if (jsb_bp)
		buf_brelse(jsb_bp);
}
static struct vfsops ext2fs_vfsops = {
	.vfs_mount	= ext2_mount,
	.vfs_unmount	= ext2_unmount,
	.vfs_root	= ext2_root,
	.vfs_getattr	= ext2_vfs_getattr,
	.vfs_sync	= ext2_sync,
	.vfs_vget	= ext2_vget,
	.vfs_fhtovp	= ext2_fhtovp,
	.vfs_vptofh	= ext2_vptofh,
	.vfs_init	= ext2_vfs_init,
	.vfs_sysctl	= NULL,
	.vfs_setattr	= NULL,
};

static struct vnodeopv_desc *ext2fs_vnodeop_opv_desc_list[1] = {
	&ext2fs_vnodeop_opv_desc,
};

vfstable_t ext2fs_vfs_table_ref;

struct vfs_fsentry ext2fs_vfsentry = {
	.vfe_vfsops	= &ext2fs_vfsops,
	.vfe_vopcnt	= 1,
	.vfe_opvdescs	= ext2fs_vnodeop_opv_desc_list,
	.vfe_fstypenum	= 0,
	.vfe_fsname	= MOUNT_EXT2FS,
	.vfe_flags	= VFS_TBLTHREADSAFE | VFS_TBLFSNODELOCK |
			  VFS_TBL64BITREADY | VFS_TBLLOCALVOL |
			  VFS_TBLNOTYPENUM,
	.vfe_reserv	= { 0, 0 },
};
