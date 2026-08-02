/*
 * Copyright (c) 2022-2026 Sunneva N. Mariu
 *
 * ext2_ihash.c
 *
 * In-core inode hash.
 *
 * FreeBSD's ext2fs keeps its inodes in the kernel's generic vfs_hash, which
 * is keyed on the mount and inode number and hands back a locked vnode. XNU
 * publishes no such facility - every file system that needs one carries its
 * own, as HFS does with its cnode hash - so this is that table for ext2fs.
 *
 * Two things make the XNU version different from a straight port of the
 * FreeBSD usage:
 *
 *   - A vnode may be recycled while a lookup is walking the chain. Holding a
 *     pointer to it proves nothing. Each inode therefore records the vnode's
 *     identity (its vid) when it is hashed, and the lookup takes its
 *     reference with vnode_getwithvid(), which fails if the vnode has since
 *     been reused for something else.
 *
 *   - vnode_getwithvid() can block, and it must not be called with the hash
 *     lock held. The lookup drops the lock, takes the reference, and then
 *     restarts the walk, because the chain may have changed meanwhile.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/mount.h>
#include <sys/queue.h>
#include <sys/vnode.h>

#include <fs/ext2fs/inode.h>
#include <fs/ext2fs/ext2_mount.h>
#include <fs/ext2fs/ext2fs.h>
#include <fs/ext2fs/fs.h>
#include <fs/ext2fs/ext2_extern.h>
#include <fs/ext2fs/ext2_apple.h>

LIST_HEAD(ext2_ihashhead, inode);

static struct ext2_ihashhead	*ext2_ihashtbl;
static u_long			 ext2_ihashmask;
static lck_mtx_t		*ext2_ihash_mtx;

/*
 * Chain for an (mount, inode number) pair. The mount pointer is part of the
 * key because one kext can host several ext2 volumes at once, and inode
 * numbers repeat across them.
 */
#define	EXT2_IHASH(ump, ino) \
	(&ext2_ihashtbl[((((uintptr_t)(ump)) >> 8) ^ (ino)) & ext2_ihashmask])

/*
 * Size the table from the size of the vnode cache, the way the BSD hashes
 * have always been sized. hashinit() is not KPI, so the table is allocated
 * and rounded to a power of two here.
 */
int
ext2_ihashinit(void)
{
	u_long nelem, hashsize;

	if (ext2_ihashtbl != NULL) {
		return (0);
	}

	nelem = (u_long)desiredvnodes;
	if (nelem < 64) {
		nelem = 64;
	}
	for (hashsize = 1; hashsize <= nelem; hashsize <<= 1) {
		continue;
	}
	hashsize >>= 1;

	ext2_ihashtbl = _MALLOC(hashsize * sizeof(*ext2_ihashtbl), M_TEMP,
	    M_WAITOK | M_ZERO);
	if (ext2_ihashtbl == NULL) {
		return (ENOMEM);
	}

	ext2_ihash_mtx = lck_mtx_alloc_init(ext2_lck_grp, LCK_ATTR_NULL);
	if (ext2_ihash_mtx == NULL) {
		_FREE(ext2_ihashtbl, M_TEMP);
		ext2_ihashtbl = NULL;
		return (ENOMEM);
	}

	ext2_ihashmask = hashsize - 1;

	return (0);
}

void
ext2_ihashdestroy(void)
{

	if (ext2_ihash_mtx != NULL) {
		lck_mtx_free(ext2_ihash_mtx, ext2_lck_grp);
		ext2_ihash_mtx = NULL;
	}
	if (ext2_ihashtbl != NULL) {
		_FREE(ext2_ihashtbl, M_TEMP);
		ext2_ihashtbl = NULL;
	}
	ext2_ihashmask = 0;
}

/*
 * Look for an inode that is already in core.
 *
 * On success *vpp holds an iocount that the caller must drop with
 * vnode_put(). A return of 0 with *vpp NULL means the inode is not resident
 * and the caller should read it in.
 */
int
ext2_ihashget(struct ext2mount *ump, ino_t ino, struct vnode **vpp)
{
	struct ext2_ihashhead *hd;
	struct inode *ip;
	struct vnode *vp;
	uint32_t vid;
	int error;

	*vpp = NULL;

	if (ext2_ihashtbl == NULL) {
		return (0);
	}

restart:
	lck_mtx_lock(ext2_ihash_mtx);
	hd = EXT2_IHASH(ump, ino);
	LIST_FOREACH(ip, hd, i_hash) {
		if (ip->i_number != ino || ip->i_ump != ump) {
			continue;
		}
		vp = ip->i_vnode;
		if (vp == NULL) {
			/*
			 * Hashed but not yet attached to a vnode. Treat it as
			 * absent; ext2_vget() serialises inode creation, so
			 * this only happens transiently.
			 */
			continue;
		}
		vid = ip->i_vid;
		lck_mtx_unlock(ext2_ihash_mtx);

		/*
		 * The lock is dropped here because taking a reference can
		 * block. If the vnode was recycled in the meantime the vid no
		 * longer matches and vnode_getwithvid() fails, in which case
		 * the chain is walked again rather than trusting anything read
		 * before the window.
		 */
		error = vnode_getwithvid(vp, vid);
		if (error != 0) {
			goto restart;
		}

		*vpp = vp;
		return (0);
	}
	lck_mtx_unlock(ext2_ihash_mtx);

	return (0);
}

/*
 * Add an inode to the hash. The caller must have set i_vnode and i_vid.
 */
void
ext2_ihashins(struct inode *ip)
{
	struct ext2_ihashhead *hd;

	lck_mtx_lock(ext2_ihash_mtx);
	hd = EXT2_IHASH(ip->i_ump, ip->i_number);
	LIST_INSERT_HEAD(hd, ip, i_hash);
	lck_mtx_unlock(ext2_ihash_mtx);
}

/*
 * Remove an inode from the hash. Called from ext2_reclaim() as the vnode is
 * torn down, and on the error paths of ext2_vget().
 */
void
ext2_ihashrem(struct inode *ip)
{

	if (ext2_ihashtbl == NULL) {
		return;
	}

	lck_mtx_lock(ext2_ihash_mtx);
	if (ip->i_hash.le_prev != NULL) {
		LIST_REMOVE(ip, i_hash);
		ip->i_hash.le_prev = NULL;
		ip->i_hash.le_next = NULL;
	}
	lck_mtx_unlock(ext2_ihash_mtx);
}
