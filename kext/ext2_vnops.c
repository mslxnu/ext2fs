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

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/fcntl.h>
#include <sys/filio.h>
#include <sys/stat.h>
#include <sys/buf.h>
#include <sys/endian.h>
#include <sys/mount.h>
#include <sys/unistd.h>
#include <sys/time.h>
#include <sys/vnode.h>
#include <sys/kauth.h>
#include <sys/namei.h>
#include <sys/event.h>
#include <sys/file.h>




#include <fs/ext2fs/fs.h>
#include <fs/ext2fs/inode.h>
#include <fs/ext2fs/ext2_extern.h>
#include <fs/ext2fs/ext2fs.h>
#include <fs/ext2fs/ext2_dinode.h>
#include <fs/ext2fs/ext2_dir.h>
#include <fs/ext2fs/ext2_mount.h>
#include <fs/ext2fs/ext2_apple.h>

static int ext2_makeinode(int mode, struct vnode *, struct vnode **,
		    struct componentname *, vfs_context_t);
static void ext2_itimes_locked(struct vnode *);
static int ext4_ext_read(struct vnop_read_args *);
static int ext2_ind_read(struct vnop_read_args *);

/*
 * The vnode operation vector.
 *
 * XNU builds this from an array of descriptor/implementation pairs and
 * resolves it at registration time, rather than from a designated-initialiser
 * struct. Operations left out fall through to vnop_default, which returns
 * ENOTSUP - that is how the file data path stays absent until
 * ext2_readwrite.c is ported, and how the operations FreeBSD had but XNU has
 * no counterpart for simply disappear.
 *
 * Three FreeBSD entries have no equivalent at all:
 *
 *   vop_access      - XNU authorises in the vfs layer; see the note where
 *                     ext2_access used to be.
 *   vop_reallocblks - there is no VNOP_REALLOCBLKS; it went with the
 *                     implementation in ext2_alloc.c.
 *   vop_print       - there is no VNOP_PRINT.
 *
 * vop_vptofh becomes a VFS operation on XNU and moves to ext2_vfsops.c, and
 * vop_cachedlookup and vop_lookup collapse into the single VNOP_LOOKUP that
 * ext2_lookup implements.
 */
#define	VOPFUNC	int (*)(void *)

int (**ext2_vnodeop_p)(void *);

static struct vnodeopv_entry_desc ext2_vnodeop_entries[] = {
	{ .opve_op = &vnop_default_desc,   .opve_impl = (VOPFUNC)vn_default_error },
	{ .opve_op = &vnop_lookup_desc,    .opve_impl = (VOPFUNC)ext2_lookup },
	{ .opve_op = &vnop_create_desc,    .opve_impl = (VOPFUNC)ext2_create },
	{ .opve_op = &vnop_mknod_desc,     .opve_impl = (VOPFUNC)ext2_mknod },
	{ .opve_op = &vnop_open_desc,      .opve_impl = (VOPFUNC)ext2_open },
	{ .opve_op = &vnop_close_desc,     .opve_impl = (VOPFUNC)ext2_close },
	{ .opve_op = &vnop_getattr_desc,   .opve_impl = (VOPFUNC)ext2_getattr },
	{ .opve_op = &vnop_setattr_desc,   .opve_impl = (VOPFUNC)ext2_setattr },
	{ .opve_op = &vnop_ioctl_desc,     .opve_impl = (VOPFUNC)ext2_ioctl },
	{ .opve_op = &vnop_fsync_desc,     .opve_impl = (VOPFUNC)ext2_fsync },
	{ .opve_op = &vnop_remove_desc,    .opve_impl = (VOPFUNC)ext2_remove },
	{ .opve_op = &vnop_link_desc,      .opve_impl = (VOPFUNC)ext2_link },
	{ .opve_op = &vnop_rename_desc,    .opve_impl = (VOPFUNC)ext2_rename },
	{ .opve_op = &vnop_mkdir_desc,     .opve_impl = (VOPFUNC)ext2_mkdir },
	{ .opve_op = &vnop_rmdir_desc,     .opve_impl = (VOPFUNC)ext2_rmdir },
	{ .opve_op = &vnop_symlink_desc,   .opve_impl = (VOPFUNC)ext2_symlink },
	{ .opve_op = &vnop_readdir_desc,   .opve_impl = (VOPFUNC)ext2_readdir },
	{ .opve_op = &vnop_readlink_desc,  .opve_impl = (VOPFUNC)ext2_readlink },
	{ .opve_op = &vnop_inactive_desc,  .opve_impl = (VOPFUNC)ext2_inactive },
	{ .opve_op = &vnop_reclaim_desc,   .opve_impl = (VOPFUNC)ext2_reclaim },
	{ .opve_op = &vnop_pathconf_desc,  .opve_impl = (VOPFUNC)ext2_pathconf },
	{ .opve_op = &vnop_blockmap_desc,  .opve_impl = (VOPFUNC)ext2_blockmap },
	{ .opve_op = &vnop_strategy_desc,  .opve_impl = (VOPFUNC)ext2_strategy },
	{ .opve_op = (struct vnodeop_desc *)NULL, .opve_impl = (VOPFUNC)NULL }
};

struct vnodeopv_desc ext2fs_vnodeop_opv_desc = {
	.opv_desc_vector_p = &ext2_vnodeop_p,
	.opv_desc_ops = ext2_vnodeop_entries,
};

/*
 * FIFOs and device nodes take the system's own operation vectors, not the
 * file system's. FreeBSD layered ext2_fifoops over fifo_specops so it could
 * keep an inode's timestamps current on close; XNU has no per-file-system
 * fifo vector to layer onto, and routes those vnodes to its own fifo and spec
 * vnops when ext2_vget() creates them with the matching vtype.
 */

/*
 * A virgin directory (no blushing please).
 * Note that the type and namlen fields are reversed relative to ext2.
 * Also, we don't use `struct odirtemplate', since it would just cause
 * endianness problems.
 */
static struct ext2fs_dirtemplate mastertemplate = {
	0, 12, 1, EXT2_FT_DIR, ".",
	0, DIRBLKSIZ - 12, 2, EXT2_FT_DIR, ".."
};
static struct ext2fs_dirtemplate omastertemplate = {
	0, 12, 1, EXT2_FT_UNKNOWN, ".",
	0, DIRBLKSIZ - 12, 2, EXT2_FT_UNKNOWN, ".."
};

static void
ext2_itimes_locked(struct vnode *vp)
{
	struct inode *ip;
	struct timespec ts;


	ip = VTOI(vp);
	if ((ip->i_flag & (IN_ACCESS | IN_CHANGE | IN_UPDATE)) == 0)
		return;
	if ((vnode_vtype(vp) == VBLK || vnode_vtype(vp) == VCHR))
		ip->i_flag |= IN_LAZYMOD;
	else
		ip->i_flag |= IN_MODIFIED;
	if ((vfs_flags(vnode_mount(vp)) & MNT_RDONLY) == 0) {
		nanotime(&ts);
		if (ip->i_flag & IN_ACCESS) {
			ip->i_atime = ts.tv_sec;
			ip->i_atimensec = ts.tv_nsec;
		}
		if (ip->i_flag & IN_UPDATE) {
			ip->i_mtime = ts.tv_sec;
			ip->i_mtimensec = ts.tv_nsec;
			ip->i_modrev++;
		}
		if (ip->i_flag & IN_CHANGE) {
			ip->i_ctime = ts.tv_sec;
			ip->i_ctimensec = ts.tv_nsec;
		}
	}
	ip->i_flag &= ~(IN_ACCESS | IN_CHANGE | IN_UPDATE);
}

void
ext2_itimes(struct vnode *vp)
{

	EXT2_ILOCK(VTOI(vp));
	ext2_itimes_locked(vp);
	EXT2_IUNLOCK(VTOI(vp));
}

/*
 * Create a regular file
 */
static int
ext2_create(struct vnop_create_args *ap)
{
	int error;

	error =
	    ext2_makeinode(MAKEIMODE(ap->a_vap->va_type, ap->a_vap->va_mode),
	    ap->a_dvp, ap->a_vpp, ap->a_cnp, ap->a_context);
	if (error != 0)
		return (error);
	if ((ap->a_cnp->cn_flags & MAKEENTRY) != 0)
		cache_enter(ap->a_dvp, *ap->a_vpp, ap->a_cnp);
	return (0);
}

static int
ext2_open(struct vnop_open_args *ap)
{

	if (vnode_vtype(ap->a_vp) == VBLK || vnode_vtype(ap->a_vp) == VCHR)
		return (EOPNOTSUPP);

	/*
	 * Files marked append-only must be opened for appending.
	 */
	if ((VTOI(ap->a_vp)->i_flags & APPEND) &&
	    (ap->a_mode & (FWRITE | O_APPEND)) == FWRITE)
		return (EPERM);

	vnode_create_vobject(ap->a_vp, VTOI(ap->a_vp)->i_size, ap->a_td);

	return (0);
}

/*
 * Close called.
 *
 * Update the times on the inode.
 */
static int
ext2_close(struct vnop_close_args *ap)
{
	struct vnode *vp = ap->a_vp;

	/*
	 * FreeBSD only refreshed the timestamps when another reference to the
	 * vnode remained, reading v_usecount under the vnode interlock. XNU
	 * publishes neither, and the update is cheap, so it is done
	 * unconditionally under the inode's own lock.
	 */
	EXT2_ILOCK(VTOI(vp));
	ext2_itimes_locked(vp);
	EXT2_IUNLOCK(VTOI(vp));
	return (0);
}

/*
 * There is deliberately no ext2_access().
 *
 * FreeBSD asks the file system to evaluate permissions: its VOP_ACCESS ran
 * vaccess() over the inode's mode, owner and group. XNU centralises that.
 * vnode_authorize() does the whole evaluation - POSIX bits, ownership, ACLs
 * and the immutable flags - from the attributes VNOP_GETATTR reports, and
 * VNOP_ACCESS is only ever called on file systems that declare their
 * authentication remote with vfs_setauthopaque(), which a local disk file
 * system is not.
 *
 * So the operation is left out of the vnode operation vector entirely, and
 * ext2_getattr()'s job is to report va_mode, va_uid, va_gid, va_flags and
 * va_type faithfully - that is what the authoriser decides on.
 */


static int
ext2_getattr(struct vnop_getattr_args *ap)
{
	struct vnode *vp = ap->a_vp;
	struct inode *ip = VTOI(vp);
	struct vnode_attr *vap = ap->a_vap;

	ext2_itimes(vp);

	/*
	 * XNU asks for a named subset of the attributes rather than filling a
	 * struct vattr wholesale: VATTR_RETURN records both the value and the
	 * fact that it was supplied, and anything the caller did not ask for is
	 * simply left alone. Supplying an attribute that was not requested is
	 * harmless, so the cheap ones are returned unconditionally.
	 */
	VATTR_RETURN(vap, va_fsid, (uint32_t)vfs_statfs(vnode_mount(vp))->f_fsid.val[0]);
	VATTR_RETURN(vap, va_fileid, ip->i_number);
	VATTR_RETURN(vap, va_mode, ip->i_mode & ~IFMT);
	VATTR_RETURN(vap, va_nlink, (uint32_t)ip->i_nlink);
	VATTR_RETURN(vap, va_uid, ip->i_uid);
	VATTR_RETURN(vap, va_gid, ip->i_gid);
	VATTR_RETURN(vap, va_rdev, ip->i_rdev);
	VATTR_RETURN(vap, va_data_size, ip->i_size);
	VATTR_RETURN(vap, va_total_size, ip->i_size);
	VATTR_RETURN(vap, va_total_alloc, dbtob((uint64_t)ip->i_blocks));
	VATTR_RETURN(vap, va_flags, ip->i_flags);
	VATTR_RETURN(vap, va_gen, ip->i_gen);
	VATTR_RETURN(vap, va_type, IFTOVT(ip->i_mode));
	VATTR_RETURN(vap, va_filerev, ip->i_modrev);
	VATTR_RETURN(vap, va_iosize, (uint32_t)vfs_statfs(vnode_mount(vp))->f_iosize);

	vap->va_access_time.tv_sec = ip->i_atime;
	vap->va_access_time.tv_nsec = E2DI_HAS_XTIME(ip) ? ip->i_atimensec : 0;
	VATTR_SET_SUPPORTED(vap, va_access_time);
	vap->va_modify_time.tv_sec = ip->i_mtime;
	vap->va_modify_time.tv_nsec = E2DI_HAS_XTIME(ip) ? ip->i_mtimensec : 0;
	VATTR_SET_SUPPORTED(vap, va_modify_time);
	vap->va_change_time.tv_sec = ip->i_ctime;
	vap->va_change_time.tv_nsec = E2DI_HAS_XTIME(ip) ? ip->i_ctimensec : 0;
	VATTR_SET_SUPPORTED(vap, va_change_time);

	/*
	 * Only the later on-disk inode layouts carry a creation time; on a
	 * revision 0 file system there is nothing to report and the attribute
	 * is left unsupported rather than filled with a guess.
	 */
	if (E2DI_HAS_XTIME(ip)) {
		vap->va_create_time.tv_sec = ip->i_birthtime;
		vap->va_create_time.tv_nsec = ip->i_birthnsec;
		VATTR_SET_SUPPORTED(vap, va_create_time);
	}

	return (0);
}

/*
 * Set attribute vnode op. called from several syscalls
 */
static int
ext2_setattr(struct vnop_setattr_args *ap)
{
	struct vnode_attr *vap = ap->a_vap;
	struct vnode *vp = ap->a_vp;
	struct inode *ip = VTOI(vp);
	vfs_context_t ctx = ap->a_context;
	int error = 0;

	/*
	 * XNU names the attributes the caller wants changed rather than
	 * marking the rest VNOVAL, so VATTR_IS_ACTIVE replaces the old
	 * comparisons, and every field acted on is reported back with
	 * VATTR_SET_SUPPORTED so the caller knows it took effect.
	 *
	 * All of FreeBSD's permission arithmetic is gone from this function.
	 * vnode_authorize() has already decided that the caller may change
	 * these attributes - including the privileged cases around the system
	 * flags, which it evaluates from the va_flags that ext2_getattr()
	 * reports - so repeating it here would be duplicated policy, and
	 * duplicated policy is how the two drift apart.
	 */
	if (VATTR_IS_ACTIVE(vap, va_flags)) {
		/* Disallow flags not supported by ext2fs. */
		if (vap->va_flags & ~(SF_APPEND | SF_IMMUTABLE | UF_NODUMP))
			return (EOPNOTSUPP);
		if (vfs_flags(vnode_mount(vp)) & MNT_RDONLY)
			return (EROFS);

		ip->i_flags = vap->va_flags;
		ip->i_flag |= IN_CHANGE;
		VATTR_SET_SUPPORTED(vap, va_flags);
		if (ip->i_flags & (IMMUTABLE | APPEND))
			return (0);
	}
	if (ip->i_flags & (IMMUTABLE | APPEND))
		return (EPERM);

	if (VATTR_IS_ACTIVE(vap, va_uid) || VATTR_IS_ACTIVE(vap, va_gid)) {
		uid_t uid = VATTR_IS_ACTIVE(vap, va_uid) ?
		    vap->va_uid : (uid_t)VNOVAL;
		gid_t gid = VATTR_IS_ACTIVE(vap, va_gid) ?
		    vap->va_gid : (gid_t)VNOVAL;

		if (vfs_flags(vnode_mount(vp)) & MNT_RDONLY)
			return (EROFS);
		if ((error = ext2_chown(vp, uid, gid)) != 0)
			return (error);
		VATTR_SET_SUPPORTED(vap, va_uid);
		VATTR_SET_SUPPORTED(vap, va_gid);
	}

	if (VATTR_IS_ACTIVE(vap, va_data_size)) {
		switch (vnode_vtype(vp)) {
		case VDIR:
			return (EISDIR);
		case VLNK:
		case VREG:
			if (vfs_flags(vnode_mount(vp)) & MNT_RDONLY)
				return (EROFS);
			break;
		default:
			break;
		}
		error = ext2_truncate(vp, (off_t)vap->va_data_size, 0,
		    vfs_context_ucred(ctx), NULL);
		if (error != 0)
			return (error);
		VATTR_SET_SUPPORTED(vap, va_data_size);
	}

	if (VATTR_IS_ACTIVE(vap, va_access_time) ||
	    VATTR_IS_ACTIVE(vap, va_modify_time) ||
	    VATTR_IS_ACTIVE(vap, va_create_time)) {
		if (vfs_flags(vnode_mount(vp)) & MNT_RDONLY)
			return (EROFS);
		/*
		 * FreeBSD distinguished utimes(NULL) - which only needs write
		 * permission - from an explicit timestamp, which needs
		 * ownership. XNU draws that distinction in the authoriser
		 * before getting here.
		 */
		if (VATTR_IS_ACTIVE(vap, va_access_time))
			ip->i_flag |= IN_ACCESS;
		if (VATTR_IS_ACTIVE(vap, va_modify_time))
			ip->i_flag |= IN_CHANGE | IN_UPDATE;
		ext2_itimes(vp);
		if (VATTR_IS_ACTIVE(vap, va_access_time)) {
			ip->i_atime = (int32_t)vap->va_access_time.tv_sec;
			ip->i_atimensec = (int32_t)vap->va_access_time.tv_nsec;
			VATTR_SET_SUPPORTED(vap, va_access_time);
		}
		if (VATTR_IS_ACTIVE(vap, va_modify_time)) {
			ip->i_mtime = (int32_t)vap->va_modify_time.tv_sec;
			ip->i_mtimensec = (int32_t)vap->va_modify_time.tv_nsec;
			VATTR_SET_SUPPORTED(vap, va_modify_time);
		}
		/*
		 * Only the later inode layouts have somewhere to keep a
		 * creation time; on revision 0 it is silently not supported
		 * rather than written into a field that does not exist.
		 */
		if (VATTR_IS_ACTIVE(vap, va_create_time) && E2DI_HAS_XTIME(ip)) {
			ip->i_birthtime = (int32_t)vap->va_create_time.tv_sec;
			ip->i_birthnsec = (int32_t)vap->va_create_time.tv_nsec;
			VATTR_SET_SUPPORTED(vap, va_create_time);
		}
		if ((error = ext2_update(vp, 0)) != 0)
			return (error);
	}

	if (VATTR_IS_ACTIVE(vap, va_mode)) {
		if (vfs_flags(vnode_mount(vp)) & MNT_RDONLY)
			return (EROFS);
		if ((error = ext2_chmod(vp, (int)vap->va_mode)) != 0)
			return (error);
		VATTR_SET_SUPPORTED(vap, va_mode);
	}

	return (error);
}

/*
 * Change the mode on a file.
 * Inode must be locked before calling.
 */
/*
 * Change the mode on a file.
 *
 * The permission checks FreeBSD made here - VADMIN on the file, and privilege
 * for the sticky and setgid bits - are gone. XNU's vnode_authorize() has
 * already decided the caller may write the mode by the time VNOP_SETATTR is
 * reached, and it applies its own rules to the setuid and setgid bits. What
 * is left is applying the change.
 */
static int
ext2_chmod(struct vnode *vp, int mode)
{
	struct inode *ip = VTOI(vp);

	ip->i_mode &= ~ALLPERMS;
	ip->i_mode |= (mode & ALLPERMS);
	ip->i_flag |= IN_CHANGE;
	return (0);
}

/*
 * Change the owner and group of a file.
 *
 * As with ext2_chmod(), authorisation has already happened. The one rule kept
 * is clearing the setuid and setgid bits when ownership actually moves: that
 * is not a permission check but a security property of the change itself, and
 * losing it would leave a setuid binary owned by whoever it was given to.
 */
static int
ext2_chown(struct vnode *vp, uid_t uid, gid_t gid)
{
	struct inode *ip = VTOI(vp);
	uid_t ouid;
	gid_t ogid;

	if (uid == (uid_t)VNOVAL)
		uid = ip->i_uid;
	if (gid == (gid_t)VNOVAL)
		gid = ip->i_gid;

	ogid = ip->i_gid;
	ouid = ip->i_uid;
	ip->i_gid = gid;
	ip->i_uid = uid;
	ip->i_flag |= IN_CHANGE;

	if ((ip->i_mode & (ISUID | ISGID)) && (ouid != uid || ogid != gid))
		ip->i_mode &= ~(ISUID | ISGID);

	return (0);
}

/*
 * Synch an open file.
 */
/* ARGSUSED */
static int
ext2_fsync(struct vnop_fsync_args *ap)
{
	/*
	 * Flush all dirty buffers associated with a vnode.
	 */

	vop_stdfsync(ap);

	return (ext2_update(ap->a_vp, ap->a_waitfor == MNT_WAIT));
}

/*
 * Mknod vnode call
 */
/* ARGSUSED */
static int
ext2_mknod(struct vnop_mknod_args *ap)
{
	struct vnode_attr *vap = ap->a_vap;
	struct vnode **vpp = ap->a_vpp;
	struct inode *ip;
	ino_t ino;
	int error;

	error = ext2_makeinode(MAKEIMODE(vap->va_type, vap->va_mode),
	    ap->a_dvp, vpp, ap->a_cnp, ap->a_context);
	if (error)
		return (error);
	ip = VTOI(*vpp);
	ip->i_flag |= IN_ACCESS | IN_CHANGE | IN_UPDATE;
	if (VATTR_IS_ACTIVE(vap, va_rdev)) {
		/*
		 * Want to be able to use this to make badblock
		 * inodes, so don't truncate the dev number.
		 */
		ip->i_rdev = vap->va_rdev;
		VATTR_SET_SUPPORTED(vap, va_rdev);
	}

	/*
	 * The vnode ext2_makeinode() produced was created before i_rdev was
	 * known, so it does not describe a device. Write the inode out, drop
	 * the vnode and fetch it again: ext2_vget() builds the vnode from the
	 * on-disk inode and will give it the right type and device number.
	 *
	 * FreeBSD did the same thing with vgone() plus VFS_VGET, to have the
	 * result checked for an alias in the device vnode cache. On XNU that
	 * aliasing is the vnode layer's business, handled inside
	 * vnode_create(); recycling is only needed here to rebuild the vnode
	 * from an inode that has since changed type.
	 */
	if ((error = ext2_update(*vpp, 1)) != 0) {
		vnode_put(*vpp);
		*vpp = NULL;
		return (error);
	}
	ino = ip->i_number;
	vnode_recycle(*vpp);
	vnode_put(*vpp);
	error = ext2_vget(vnode_mount(ap->a_dvp), ino, vpp, ap->a_context);
	if (error) {
		*vpp = NULL;
		return (error);
	}
	return (0);
}

static int
ext2_remove(struct vnop_remove_args *ap)
{
	struct inode *ip;
	struct vnode *vp = ap->a_vp;
	struct vnode *dvp = ap->a_dvp;
	int error;

	ip = VTOI(vp);
	if ((ip->i_flags & (NOUNLINK | IMMUTABLE | APPEND)) ||
	    (VTOI(dvp)->i_flags & APPEND)) {
		error = EPERM;
		goto out;
	}
	error = ext2_dirremove(dvp, ap->a_cnp);
	if (error == 0) {
		ip->i_nlink--;
		ip->i_flag |= IN_CHANGE;
	}
out:
	return (error);
}

/*
 * link vnode call
 */
static int
ext2_link(struct vnop_link_args *ap)
{
	struct vnode *vp = ap->a_vp;
	struct vnode *tdvp = ap->a_tdvp;
	struct componentname *cnp = ap->a_cnp;
	struct inode *ip;
	int error;

#ifdef INVARIANTS
	if ((cnp->cn_flags & HASBUF) == 0)
		panic("ext2_link: no name");
#endif
	ip = VTOI(vp);
	if ((nlink_t)ip->i_nlink >= EXT2_LINK_MAX) {
		error = EMLINK;
		goto out;
	}
	if (ip->i_flags & (IMMUTABLE | APPEND)) {
		error = EPERM;
		goto out;
	}
	ip->i_nlink++;
	ip->i_flag |= IN_CHANGE;
	error = ext2_update(vp, !DOINGASYNC(vp));
	if (!error)
		error = ext2_direnter(ip, tdvp, cnp);
	if (error) {
		ip->i_nlink--;
		ip->i_flag |= IN_CHANGE;
	}
out:
	return (error);
}

/*
 * Rename system call.
 * 	rename("foo", "bar");
 * is essentially
 *	unlink("bar");
 *	link("foo", "bar");
 *	unlink("foo");
 * but ``atomically''.  Can't do full commit without saving state in the
 * inode on disk which isn't feasible at this time.  Best we can do is
 * always guarantee the target exists.
 *
 * Basic algorithm is:
 *
 * 1) Bump link count on source while we're linking it to the
 *    target.  This also ensure the inode won't be deleted out
 *    from underneath us while we work (it may be truncated by
 *    a concurrent `trunc' or `open' for creation).
 * 2) Link source to destination.  If destination already exists,
 *    delete it first.
 * 3) Unlink source reference to inode if still around. If a
 *    directory was moved and the parent of the destination
 *    is different from the source, patch the ".." entry in the
 *    directory.
 */
static int
ext2_rename(struct vnop_rename_args *ap)
{
	struct vnode *tvp = ap->a_tvp;
	struct vnode *tdvp = ap->a_tdvp;
	struct vnode *fvp = ap->a_fvp;
	struct vnode *fdvp = ap->a_fdvp;
	struct componentname *tcnp = ap->a_tcnp;
	struct componentname *fcnp = ap->a_fcnp;
	struct inode *ip, *xp, *dp;
	struct ext2fs_dirtemplate dirbuf;
	int doingdirectory = 0, oldparent = 0, newparent = 0;
	int error = 0;
	u_char namlen;

#ifdef INVARIANTS
	if ((tcnp->cn_flags & HASBUF) == 0 ||
	    (fcnp->cn_flags & HASBUF) == 0)
		panic("ext2_rename: no name");
#endif
	/*
	 * Check for cross-device rename.
	 */
	if ((vnode_mount(fvp) != vnode_mount(tdvp)) ||
	    (tvp && (vnode_mount(fvp) != vnode_mount(tvp)))) {
		error = EXDEV;
abortit:
		if (tdvp == tvp)
			vnode_put(tdvp);
		else
			vnode_put(tdvp);
		if (tvp)
			vnode_put(tvp);
		vnode_put(fdvp);
		vnode_put(fvp);
		return (error);
	}

	if (tvp && ((VTOI(tvp)->i_flags & (NOUNLINK | IMMUTABLE | APPEND)) ||
	    (VTOI(tdvp)->i_flags & APPEND))) {
		error = EPERM;
		goto abortit;
	}

	/*
	 * Renaming a file to itself has no effect.  The upper layers should
	 * not call us in that case.  Temporarily just warn if they do.
	 */
	if (fvp == tvp) {
		printf("ext2_rename: fvp == tvp (can't happen)\n");
		error = 0;
		goto abortit;
	}

	dp = VTOI(fdvp);
	ip = VTOI(fvp);
	if (ip->i_nlink >= EXT2_LINK_MAX) {
		error = EMLINK;
		goto abortit;
	}
	if ((ip->i_flags & (NOUNLINK | IMMUTABLE | APPEND))
	    || (dp->i_flags & APPEND)) {
		error = EPERM;
		goto abortit;
	}
	if ((ip->i_mode & IFMT) == IFDIR) {
		/*
		 * Avoid ".", "..", and aliases of "." for obvious reasons.
		 */
		if ((fcnp->cn_namelen == 1 && fcnp->cn_nameptr[0] == '.') ||
		    dp == ip || (fcnp->cn_flags | tcnp->cn_flags) & ISDOTDOT ||
		    (ip->i_flag & IN_RENAME)) {
			error = EINVAL;
			goto abortit;
		}
		ip->i_flag |= IN_RENAME;
		oldparent = dp->i_number;
		doingdirectory++;
	}
	vnode_put(fdvp);

	/*
	 * When the target exists, both the directory
	 * and target vnodes are returned locked.
	 */
	dp = VTOI(tdvp);
	xp = NULL;
	if (tvp)
		xp = VTOI(tvp);

	/*
	 * 1) Bump link count while we're moving stuff
	 *    around.  If we crash somewhere before
	 *    completing our work, the link count
	 *    may be wrong, but correctable.
	 */
	ip->i_nlink++;
	ip->i_flag |= IN_CHANGE;
	if ((error = ext2_update(fvp, !DOINGASYNC(fvp))) != 0) {
		goto bad;
	}

	/*
	 * If ".." must be changed (ie the directory gets a new
	 * parent) then the source directory must not be in the
	 * directory hierarchy above the target, as this would
	 * orphan everything below the source directory. Also
	 * the user must have write permission in the source so
	 * as to be able to change "..". We must repeat the call
	 * to namei, as the parent directory is unlocked by the
	 * call to checkpath().
	 */
	/*
	 * FreeBSD checked write permission on the source here, because it was
	 * about to rewrite the ".." entry of a directory being moved. XNU
	 * authorises the rename as a whole before the vnop runs, so the check
	 * is the vfs layer's; error stays 0 for the test below.
	 */
	error = 0;
	if (oldparent != dp->i_number)
		newparent = dp->i_number;
	if (doingdirectory && newparent) {
		if (error)	/* write access check above */
			goto bad;
		/*
		 * FreeBSD released the target here and re-resolved the name
		 * afterwards, because checkpath() walked the tree with the
		 * parent unlocked and anything could have moved meanwhile.
		 *
		 * XNU holds an iocount on all four vnodes for the whole of
		 * VNOP_RENAME and never drops them, so nothing is released
		 * across the walk and there is nothing to re-resolve. The
		 * vnodes and inode pointers stay valid.
		 */
		error = ext2_checkpath(ip, dp,
		    vfs_context_ucred(ap->a_context), ap->a_context);
		if (error)
			goto out;
	}
	/*
	 * 2) If target doesn't exist, link the target
	 *    to the source and unlink the source.
	 *    Otherwise, rewrite the target directory
	 *    entry to reference the source inode and
	 *    expunge the original entry's existence.
	 */
	if (xp == NULL) {
		if (dp->i_devvp != ip->i_devvp)
			panic("ext2_rename: EXDEV");
		/*
		 * Account for ".." in new directory.
		 * When source and destination have the same
		 * parent we don't fool with the link count.
		 */
		if (doingdirectory && newparent) {
			if ((nlink_t)dp->i_nlink >= EXT2_LINK_MAX) {
				error = EMLINK;
				goto bad;
			}
			dp->i_nlink++;
			dp->i_flag |= IN_CHANGE;
			error = ext2_update(tdvp, !DOINGASYNC(tdvp));
			if (error)
				goto bad;
		}
		error = ext2_direnter(ip, tdvp, tcnp);
		if (error) {
			if (doingdirectory && newparent) {
				dp->i_nlink--;
				dp->i_flag |= IN_CHANGE;
				(void)ext2_update(tdvp, 1);
			}
			goto bad;
		}
		vnode_put(tdvp);
	} else {
		if (xp->i_devvp != dp->i_devvp || xp->i_devvp != ip->i_devvp)
		       panic("ext2_rename: EXDEV");
		/*
		 * Short circuit rename(foo, foo).
		 */
		if (xp->i_number == ip->i_number)
			panic("ext2_rename: same file");
		/*
		 * If the parent directory is "sticky", then the user must
		 * own the parent directory, or the destination of the rename,
		 * otherwise the destination may not be changed (except by
		 * root). This implements append-only directories.
		 */
		if ((dp->i_mode & S_ISTXT) && kauth_cred_getuid(vfs_context_ucred(ap->a_context)) != 0 &&
		    kauth_cred_getuid(vfs_context_ucred(ap->a_context)) != dp->i_uid &&
		    xp->i_uid != kauth_cred_getuid(vfs_context_ucred(ap->a_context))) {
			error = EPERM;
			goto bad;
		}
		/*
		 * Target must be empty if a directory and have no links
		 * to it. Also, ensure source and target are compatible
		 * (both directories, or both not directories).
		 */
		if ((xp->i_mode&IFMT) == IFDIR) {
			if (! ext2_dirempty(xp, dp->i_number, vfs_context_ucred(ap->a_context)) || 
			    xp->i_nlink > 2) {
				error = ENOTEMPTY;
				goto bad;
			}
			if (!doingdirectory) {
				error = ENOTDIR;
				goto bad;
			}
			cache_purge(tdvp);
		} else if (doingdirectory) {
			error = EISDIR;
			goto bad;
		}
		error = ext2_dirrewrite(dp, ip, tcnp);
		if (error)
			goto bad;
		/*
		 * If the target directory is in the same
		 * directory as the source directory,
		 * decrement the link count on the parent
		 * of the target directory.
		 */
		if (doingdirectory && !newparent) {
			dp->i_nlink--;
			dp->i_flag |= IN_CHANGE;
		}
		vnode_put(tdvp);
		/*
		 * Adjust the link count of the target to
		 * reflect the dirrewrite above.  If this is
		 * a directory it is empty and there are
		 * no links to it, so we can squash the inode and
		 * any space associated with it.  We disallowed
		 * renaming over top of a directory with links to
		 * it above, as the remaining link would point to
		 * a directory without "." or ".." entries.
		 */
		xp->i_nlink--;
		if (doingdirectory) {
			if (--xp->i_nlink != 0)
				panic("ext2_rename: linked directory");
			error = ext2_truncate(tvp, (off_t)0, IO_SYNC,
			    vfs_context_ucred(ap->a_context), NULL);
		}
		xp->i_flag |= IN_CHANGE;
		vnode_put(tvp);
		xp = NULL;
	}

	/*
	 * 3) Unlink the source.
	 */
	/*
	 * FreeBSD re-resolved the source name here for the same reason as
	 * above - its locks had been dropped, so the entry might no longer be
	 * the one it started with. The iocounts XNU holds mean fvp and fdvp
	 * are still the vnodes the caller named, so the lookup is dropped and
	 * the inodes are taken directly.
	 */
	xp = VTOI(fvp);
	dp = VTOI(fdvp);
	/*
	 * Ensure that the directory entry still exists and has not
	 * changed while the new name has been entered. If the source is
	 * a file then the entry may have been unlinked or renamed. In
	 * either case there is no further work to be done. If the source
	 * is a directory then it cannot have been rmdir'ed; its link
	 * count of three would cause a rmdir to fail with ENOTEMPTY.
	 * The IN_RENAME flag ensures that it cannot be moved by another
	 * rename.
	 */
	if (xp != ip) {
		if (doingdirectory)
			panic("ext2_rename: lost dir entry");
	} else {
		/*
		 * If the source is a directory with a
		 * new parent, the link count of the old
		 * parent directory must be decremented
		 * and ".." set to point to the new parent.
		 */
		if (doingdirectory && newparent) {
			dp->i_nlink--;
			dp->i_flag |= IN_CHANGE;
			error = vn_rdwr(UIO_READ, fvp, (caddr_t)&dirbuf,
				sizeof(struct ext2fs_dirtemplate), (off_t)0,
				UIO_SYSSPACE, IO_NODELOCKED | IO_NOMACCHECK,
				vfs_context_ucred(ap->a_context), NOCRED, NULL, NULL);
			if (error == 0) {
				/* Like ufs little-endian: */
				namlen = dirbuf.dotdot_type;
				if (namlen != 2 ||
				    dirbuf.dotdot_name[0] != '.' ||
				    dirbuf.dotdot_name[1] != '.') {
					ext2_dirbad(xp, (doff_t)12,
					    "rename: mangled dir");
				} else {
					dirbuf.dotdot_ino = newparent;
					(void) vn_rdwr(UIO_WRITE, fvp,
					    (caddr_t)&dirbuf,
					    sizeof(struct ext2fs_dirtemplate),
					    (off_t)0, UIO_SYSSPACE,
					    IO_NODELOCKED | IO_SYNC |
					    IO_NOMACCHECK, vfs_context_ucred(ap->a_context),
					    NOCRED, NULL, NULL);
					cache_purge(fdvp);
				}
			}
		}
		error = ext2_dirremove(fdvp, fcnp);
		if (!error) {
			xp->i_nlink--;
			xp->i_flag |= IN_CHANGE;
		}
		xp->i_flag &= ~IN_RENAME;
	}
	if (dp)
		vnode_put(fdvp);
	if (xp)
		vnode_put(fvp);
	vnode_put(ap->a_fvp);
	return (error);

bad:
	if (xp)
		vnode_put(ITOV(xp));
	vnode_put(ITOV(dp));
out:
	if (doingdirectory)
		ip->i_flag &= ~IN_RENAME;
	/*
	 * FreeBSD had to retake the source's vnode lock before touching the
	 * inode on this path. There is no such lock here; the iocount is still
	 * held, so the inode can simply be corrected.
	 */
	ip->i_nlink--;
	ip->i_flag |= IN_CHANGE;
	ip->i_flag &= ~IN_RENAME;
	vnode_put(fvp);
	return (error);
}

/*
 * Mkdir system call
 */
static int
ext2_mkdir(struct vnop_mkdir_args *ap)
{
	struct vnode *dvp = ap->a_dvp;
	struct vnode_attr *vap = ap->a_vap;
	struct componentname *cnp = ap->a_cnp;
	struct inode *ip, *dp;
	struct vnode *tvp;
	struct ext2fs_dirtemplate dirtemplate, *dtp;
	int error, dmode;

#ifdef INVARIANTS
	if ((cnp->cn_flags & HASBUF) == 0)
		panic("ext2_mkdir: no name");
#endif
	dp = VTOI(dvp);
	if ((nlink_t)dp->i_nlink >= EXT2_LINK_MAX) {
		error = EMLINK;
		goto out;
	}
	dmode = vap->va_mode & 0777;
	dmode |= IFDIR;
	/*
	 * Must simulate part of ext2_makeinode here to acquire the inode,
	 * but not have it entered in the parent directory. The entry is
	 * made later after writing "." and ".." entries.
	 */
	error = ext2_valloc(dvp, dmode, vfs_context_ucred(ap->a_context), &tvp);
	if (error)
		goto out;
	ip = VTOI(tvp);
	ip->i_gid = dp->i_gid;
#ifdef SUIDDIR
	{
		/*
		 * if we are hacking owners here, (only do this where told to)
		 * and we are not giving it TOO root, (would subvert quotas)
		 * then go ahead and give it to the other user.
		 * The new directory also inherits the SUID bit. 
		 * If user's UID and dir UID are the same,
		 * 'give it away' so that the SUID is still forced on.
		 */
		if ( (vfs_flags(vnode_mount(dvp)) & MNT_SUIDDIR) &&
		   (dp->i_mode & ISUID) && dp->i_uid) {
			dmode |= ISUID;
			ip->i_uid = dp->i_uid;
		} else {
			ip->i_uid = kauth_cred_getuid(vfs_context_ucred(ctx));
		}
	}
#else
	ip->i_uid = kauth_cred_getuid(vfs_context_ucred(ap->a_context));
#endif
	ip->i_flag |= IN_ACCESS | IN_CHANGE | IN_UPDATE;
	ip->i_mode = dmode;
	vnode_vtype(tvp) = VDIR;	/* Rest init'd in getnewvnode(). */
	ip->i_nlink = 2;
	if (cnp->cn_flags & ISWHITEOUT)
		ip->i_flags |= UF_OPAQUE;
	error = ext2_update(tvp, 1);

	/*
	 * Bump link count in parent directory
	 * to reflect work done below.  Should
	 * be done before reference is created
	 * so reparation is possible if we crash.
	 */
	dp->i_nlink++;
	dp->i_flag |= IN_CHANGE;
	error = ext2_update(dvp, !DOINGASYNC(dvp));
	if (error)
		goto bad;

	/* Initialize directory with "." and ".." from static template. */
	if (EXT2_HAS_INCOMPAT_FEATURE(ip->i_e2fs,
	    EXT2F_INCOMPAT_FTYPE))
		dtp = &mastertemplate;
	else
		dtp = &omastertemplate;
	dirtemplate = *dtp;
	dirtemplate.dot_ino = ip->i_number;
	dirtemplate.dotdot_ino = dp->i_number;
	/* note that in ext2 DIRBLKSIZ == blocksize, not DEV_BSIZE 
	 * so let's just redefine it - for this function only
	 */
#undef  DIRBLKSIZ 
#define DIRBLKSIZ  VTOI(dvp)->i_e2fs->e2fs_bsize
	dirtemplate.dotdot_reclen = DIRBLKSIZ - 12;
	error = vn_rdwr(UIO_WRITE, tvp, (caddr_t)&dirtemplate,
	    sizeof(dirtemplate), (off_t)0, UIO_SYSSPACE,
	    IO_NODELOCKED | IO_SYNC | IO_NOMACCHECK, vfs_context_ucred(ap->a_context), NOCRED,
	    NULL, NULL);
	if (error) {
		dp->i_nlink--;
		dp->i_flag |= IN_CHANGE;
		goto bad;
	}
	if (DIRBLKSIZ > VFSTOEXT2(vnode_mount(dvp))->um_mountp->mnt_stat.f_bsize)
		/* XXX should grow with balloc() */
		panic("ext2_mkdir: blksize");
	else {
		ip->i_size = DIRBLKSIZ;
		ip->i_flag |= IN_CHANGE;
	}

	/* Directory set up, now install its entry in the parent directory. */
	error = ext2_direnter(ip, dvp, cnp);
	if (error) {
		dp->i_nlink--;
		dp->i_flag |= IN_CHANGE;
	}
bad:
	/*
	 * No need to do an explicit VOP_TRUNCATE here, vrele will do this
	 * for us because we set the link count to 0.
	 */
	if (error) {
		ip->i_nlink = 0;
		ip->i_flag |= IN_CHANGE;
		vnode_put(tvp);
	} else
		*ap->a_vpp = tvp;
out:
	return (error);
#undef  DIRBLKSIZ
#define DIRBLKSIZ  DEV_BSIZE
}

/*
 * Rmdir system call.
 */
static int
ext2_rmdir(struct vnop_rmdir_args *ap)
{
	struct vnode *vp = ap->a_vp;
	struct vnode *dvp = ap->a_dvp;
	struct componentname *cnp = ap->a_cnp;
	struct inode *ip, *dp;
	int error;

	ip = VTOI(vp);
	dp = VTOI(dvp);

	/*
	 * Verify the directory is empty (and valid).
	 * (Rmdir ".." won't be valid since
	 *  ".." will contain a reference to
	 *  the current directory and thus be
	 *  non-empty.)
	 */
	if (ip->i_nlink != 2 || !ext2_dirempty(ip, dp->i_number, vfs_context_ucred(ap->a_context))) {
		error = ENOTEMPTY;
		goto out;
	}
	if ((dp->i_flags & APPEND)
	    || (ip->i_flags & (NOUNLINK | IMMUTABLE | APPEND))) {
		error = EPERM;
		goto out;
	}
	/*
	 * Delete reference to directory before purging
	 * inode.  If we crash in between, the directory
	 * will be reattached to lost+found,
	 */
	error = ext2_dirremove(dvp, cnp);
	if (error)
		goto out;
	dp->i_nlink--;
	dp->i_flag |= IN_CHANGE;
	cache_purge(dvp);
	/*
	 * Truncate inode.  The only stuff left
	 * in the directory is "." and "..".  The
	 * "." reference is inconsequential since
	 * we're quashing it.  The ".." reference
	 * has already been adjusted above.  We've
	 * removed the "." reference and the reference
	 * in the parent directory, but there may be
	 * other hard links so decrement by 2 and
	 * worry about them later.
	 */
	ip->i_nlink -= 2;
	error = ext2_truncate(vp, (off_t)0, IO_SYNC, vfs_context_ucred(ap->a_context),
	    NULL);
	cache_purge(ITOV(ip));
	/*
	 * FreeBSD reacquired the parent and child locks here in a fixed order
	 * to avoid deadlocking with another rmdir. XNU takes no such locks.
	 */
out:
	return (error);
}

/*
 * symlink -- make a symbolic link
 */
static int
ext2_symlink(struct vnop_symlink_args *ap)
{
	struct vnode *vp, **vpp = ap->a_vpp;
	struct inode *ip;
	int len, error;

	error = ext2_makeinode(IFLNK | ap->a_vap->va_mode, ap->a_dvp,
	    vpp, ap->a_cnp, ap->a_context);
	if (error)
		return (error);
	vp = *vpp;
	len = strlen(ap->a_target);
	if (len < EXT2_MAXSYMLINKLEN) {
		ip = VTOI(vp);
		bcopy(ap->a_target, (char *)ip->i_shortlink, len);
		ip->i_size = len;
		ip->i_flag |= IN_CHANGE | IN_UPDATE;
	} else
		error = vn_rdwr(UIO_WRITE, vp, ap->a_target, len, (off_t)0,
		    UIO_SYSSPACE, IO_NODELOCKED | IO_NOMACCHECK,
		    ap->a_vfs_context_ucred(ap->a_context), NOCRED, NULL, NULL);
	if (error)
		vnode_put(vp);
	return (error);
}

/*
 * Return target name of a symbolic link
 */
static int
ext2_readlink(struct vnop_readlink_args *ap)
{
	struct vnode *vp = ap->a_vp;
	struct inode *ip = VTOI(vp);
	int isize;

	isize = ip->i_size;
	if (isize < vnode_mount(vp)->mnt_maxsymlinklen) {
		uiomove((char *)ip->i_shortlink, isize, ap->a_uio);
		return (0);
	}
	return (VOP_READ(vp, ap->a_uio, 0, ap->a_cred));
}

/*
 * Calculate the logical to physical mapping if not done already,
 * then call the device strategy routine.
 *
 * In order to be able to swap to a file, the ext2_bmaparray() operation may not
 * deadlock on memory.  See ext2_bmap() for details.
 */
static int
ext2_strategy(struct vnop_strategy_args *ap)
{
	struct buf *bp = ap->a_bp;
	struct vnode *vp = ap->a_vp;
	struct bufobj *bo;
	daddr64_t blkno;
	int error;

	if (vnode_vtype(vp) == VBLK || vnode_vtype(vp) == VCHR)
		panic("ext2_strategy: spec");
	if (bp->b_blkno == bp->b_lblkno) {
		error = ext2_bmaparray(vp, bp->b_lblkno, &blkno, NULL, NULL);
		bp->b_blkno = blkno;
		if (error) {
			bp->b_error = error;
			bp->b_ioflags |= BIO_ERROR;
			bufdone(bp);
			return (0);
		}
		if ((long)bp->b_blkno == -1)
			vfs_bio_clrbuf(bp);
	}
	if ((long)bp->b_blkno == -1) {
		bufdone(bp);
		return (0);
	}
	bp->b_iooffset = dbtob(bp->b_blkno);
	bo = VFSTOEXT2(vnode_mount(vp))->um_bo;
	BO_STRATEGY(bo, bp);
	return (0);
}

/*
 * Print out the contents of an inode.
 */
static int
ext2_print(struct vnop_print_args *ap)
{
	struct vnode *vp = ap->a_vp;
	struct inode *ip = VTOI(vp);

	vn_printf(ip->i_devvp, "\tino %ju", (uintmax_t)ip->i_number);
	if (vnode_vtype(vp) == VFIFO)
		fifo_printinfo(vp);
	printf("\n");
	return (0);
}

/*
 * Close wrapper for fifos.
 *
 * Update the times on the inode then do device close.
 */
static int
ext2fifo_close(struct vnop_close_args *ap)
{
	struct vnode *vp = ap->a_vp;

	VI_LOCK(vp);
	if (vp->v_usecount > 1)
		ext2_itimes_locked(vp);
	VI_UNLOCK(vp);
	return (fifo_specops.vop_close(ap));
}

/*
 * Kqfilter wrapper for fifos.
 *
 * Fall through to ext2 kqfilter routines if needed 
 */
static int
ext2fifo_kqfilter(struct vnop_kqfilter_args *ap)
{
	int error;

	error = fifo_specops.vop_kqfilter(ap);
	if (error)
		error = vfs_kqfilter(ap);
	return (error);
}

/*
 * Return POSIX pathconf information applicable to ext2 filesystems.
 */
static int
ext2_pathconf(struct vnop_pathconf_args *ap)
{
	int error = 0;

	switch (ap->a_name) {
	case _PC_LINK_MAX:
		*ap->a_retval = EXT2_LINK_MAX;
		break;
	case _PC_NAME_MAX:
		*ap->a_retval = NAME_MAX;
		break;
	case _PC_PATH_MAX:
		*ap->a_retval = PATH_MAX;
		break;
	case _PC_PIPE_BUF:
		*ap->a_retval = PIPE_BUF;
		break;
	case _PC_CHOWN_RESTRICTED:
		*ap->a_retval = 1;
		break;
	case _PC_NO_TRUNC:
		*ap->a_retval = 1;
		break;
	case _PC_MIN_HOLE_SIZE:
		*ap->a_retval = vfs_statfs(vnode_mount(ap->a_vp))->f_iosize;
		break;
	case _PC_ASYNC_IO:
		/* _PC_ASYNC_IO should have been handled by upper layers. */
		KASSERT(0, ("_PC_ASYNC_IO should not get here"));
		error = EINVAL;
		break;
	case _PC_PRIO_IO:
		*ap->a_retval = 0;
		break;
	case _PC_SYNC_IO:
		*ap->a_retval = 0;
		break;
	case _PC_ALLOC_SIZE_MIN:
		*ap->a_retval = vfs_statfs(vnode_mount(ap->a_vp))->f_bsize;
		break;
	case _PC_FILESIZEBITS:
		*ap->a_retval = 64;
		break;
	case _PC_REC_INCR_XFER_SIZE:
		*ap->a_retval = vfs_statfs(vnode_mount(ap->a_vp))->f_iosize;
		break;
	case _PC_REC_MAX_XFER_SIZE:
		*ap->a_retval = -1; /* means ``unlimited'' */
		break;
	case _PC_REC_MIN_XFER_SIZE:
		*ap->a_retval = vfs_statfs(vnode_mount(ap->a_vp))->f_iosize;
		break;
	case _PC_REC_XFER_ALIGN:
		*ap->a_retval = PAGE_SIZE;
		break;
	case _PC_SYMLINK_MAX:
		*ap->a_retval = MAXPATHLEN;
		break;

	default:
		error = EINVAL;
		break;
	}
	return (error);
}

/*
 * Vnode pointer to File handle
 */
/* ARGSUSED */
static int
ext2_vptofh(struct vnop_vptofh_args *ap)
{
	struct inode *ip;
	struct ufid *ufhp;

	ip = VTOI(ap->a_vp);
	ufhp = (struct ufid *)ap->a_fhp;
	ufhp->ufid_len = sizeof(struct ufid);
	ufhp->ufid_ino = ip->i_number;
	ufhp->ufid_gen = ip->i_gen;
	return (0);
}

/*
 * Initialize the vnode associated with a new inode, handle aliased
 * vnodes.
 */
int
ext2_vinit(struct mount *mntp, struct vop_vector *fifoops, struct vnode **vpp)
{
	struct inode *ip;
	struct vnode *vp;

	vp = *vpp;
	ip = VTOI(vp);
	vnode_vtype(vp) = IFTOVT(ip->i_mode);
	if (vnode_vtype(vp) == VFIFO)
		vp->v_op = fifoops;

	if (ip->i_number == EXT2_ROOTINO)
		vp->v_vflag |= VV_ROOT;
	ip->i_modrev = init_va_filerev();
	*vpp = vp;
	return (0);
}

/*
 * Allocate a new inode.
 */
static int
ext2_makeinode(int mode, struct vnode *dvp, struct vnode **vpp,
    struct componentname *cnp, vfs_context_t ctx)
{
	struct inode *ip, *pdir;
	struct vnode *tvp;
	int error;

	pdir = VTOI(dvp);
	*vpp = NULL;
	if ((mode & IFMT) == 0)
		mode |= IFREG;

	error = ext2_valloc(dvp, mode, vfs_context_ucred(ctx), &tvp);
	if (error) {
		return (error);
	}
	ip = VTOI(tvp);
	ip->i_gid = pdir->i_gid;
#ifdef SUIDDIR
	{
		/*
		 * if we are
		 * not the owner of the directory,
		 * and we are hacking owners here, (only do this where told to)
		 * and we are not giving it TOO root, (would subvert quotas)
		 * then go ahead and give it to the other user.
		 * Note that this drops off the execute bits for security.
		 */
		if ( (vfs_flags(vnode_mount(dvp)) & MNT_SUIDDIR) &&
		     (pdir->i_mode & ISUID) &&
		     (pdir->i_uid != kauth_cred_getuid(vfs_context_ucred(ap->a_context))) && pdir->i_uid) {
			ip->i_uid = pdir->i_uid;
			mode &= ~07111;
		} else {
			ip->i_uid = kauth_cred_getuid(vfs_context_ucred(ap->a_context));
		}
	}
#else
	ip->i_uid = kauth_cred_getuid(vfs_context_ucred(ap->a_context));
#endif
	ip->i_flag |= IN_ACCESS | IN_CHANGE | IN_UPDATE;
	ip->i_mode = mode;
	/* The vnode's type is set when it is created, in ext2_vget(). */
	ip->i_nlink = 1;
	/*
	 * A new inode inherits its group from the parent directory, so it can
	 * come out setgid for a group the creator does not belong to. Strip the
	 * bit in that case. This is a property of creating the file rather than
	 * a permission check on an existing one, so unlike the rest of the
	 * access rules it stays with the file system; kauth_cred_ismember_gid()
	 * is XNU's group test, and a failed query is treated as "not a member".
	 */
	if (ip->i_mode & ISGID) {
		int ismember = 0;

		if (kauth_cred_ismember_gid(vfs_context_ucred(ctx),
		    (gid_t)ip->i_gid, &ismember) != 0 || !ismember)
			ip->i_mode &= ~ISGID;
	}

	if (cnp->cn_flags & ISWHITEOUT)
		ip->i_flags |= UF_OPAQUE;

	/*
	 * Make sure inode goes to disk before directory entry.
	 */
	error = ext2_update(tvp, !DOINGASYNC(tvp));
	if (error)
		goto bad;
	error = ext2_direnter(ip, dvp, cnp);
	if (error)
		goto bad;

	*vpp = tvp;
	return (0);

bad:
	/*
	 * Write error occurred trying to update the inode
	 * or the directory so must deallocate the inode.
	 */
	ip->i_nlink = 0;
	ip->i_flag |= IN_CHANGE;
	vnode_put(tvp);
	return (error);
}

static int
ext2_ioctl(struct vnop_ioctl_args *ap)
{

	switch (ap->a_command) {
	case FIOSEEKDATA:
	case FIOSEEKHOLE:
		return (vn_bmap_seekhole(ap->a_vp, ap->a_command,
		    (off_t *)ap->a_data, ap->a_cred));
	default:
		return (ENOTTY);
	}
}

