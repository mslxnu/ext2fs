/*
 * Copyright (c) 2022-2026 Sunneva N. Mariu
 *
 * ext2fs.c
 *
 * Module entry point: allocates and releases the resources that outlive any
 * single mount (the allocation tag and lock group), and drives the kext
 * start/stop handshake.
 *
 * Registration of the file system itself (vfs_fsadd) is deliberately absent
 * until the imported BSD vfsops have been ported; a kext that advertises a
 * file system whose operation vectors are not yet in place would be loadable
 * but would panic on first mount.
 */

#include <kern/locks.h>
#include <libkern/libkern.h>
#include <libkern/OSMalloc.h>
#include <libkern/version.h>
#include <libkext.h>
#include <mach/kmod.h>
#include <mach/mach_types.h>
#include <os/log.h>
#include <sys/mount.h>

#include <fs/ext2fs/ext2_apple.h>

#pragma mark -
#pragma mark External references

extern struct vfs_fsentry ext2fs_vfsentry;
extern vfstable_t ext2fs_vfs_table_ref;

#pragma mark -
#pragma mark Module-wide state

OSMallocTag	ext2_osmalloc_tag;
lck_grp_t	*ext2_lck_grp;

#pragma mark -
#pragma mark Initialization and cleanup routines

/*
 * Create the allocation tag and lock group.  Interlocked against repeat
 * calls: vfs_init() may run again for a second mount, and re-allocating
 * would leak the first pair.
 */
int
ext2_init(void)
{
	static int initialized;

	if (initialized) {
		return 0;
	}

	ext2_osmalloc_tag = OSMalloc_Tagalloc(BUNDLEID_S, OSMT_DEFAULT);
	if (ext2_osmalloc_tag == NULL) {
		return ENOMEM;
	}

	ext2_lck_grp = lck_grp_alloc_init(EXT2_LCKGRP_NAME, LCK_GRP_ATTR_NULL);
	if (ext2_lck_grp == NULL) {
		OSMalloc_Tagfree(ext2_osmalloc_tag);
		ext2_osmalloc_tag = NULL;
		return ENOMEM;
	}

	/*
	 * The in-core inode hash needs the lock group, so it is created after
	 * it and torn down before it in ext2_fini().
	 */
	if (ext2_ihashinit() != 0) {
		lck_grp_free(ext2_lck_grp);
		ext2_lck_grp = NULL;
		OSMalloc_Tagfree(ext2_osmalloc_tag);
		ext2_osmalloc_tag = NULL;
		return ENOMEM;
	}

	initialized = 1;

	return 0;
}

/*
 * Release what ext2_init() created.  Only ever called from ext2_stop(), by
 * which point the file system is unregistered and no mount can be holding a
 * reference to either object.
 */
int
ext2_fini(void)
{
	ext2_ihashdestroy();

	if (ext2_lck_grp != NULL) {
		lck_grp_free(ext2_lck_grp);
		ext2_lck_grp = NULL;
	}

	if (ext2_osmalloc_tag != NULL) {
		OSMalloc_Tagfree(ext2_osmalloc_tag);
		ext2_osmalloc_tag = NULL;
	}

	return 0;
}

#pragma mark -
#pragma mark Start/Stop routines

kern_return_t
ext2_start(kmod_info_t *ki, __unused void *d)
{
	uuid_string_t uuid;
	int ret;

	os_log(OS_LOG_DEFAULT, "%s \n", version);	/* darwin kernel version */

	ret = libkext_vma_uuid(ki->address, uuid);
	kassert(ret == 0);

	os_log(OS_LOG_DEFAULT, "kext executable uuid %s \n", uuid);

	ret = ext2_init();
	if (ret != 0) {
		os_log(OS_LOG_DEFAULT, "ext2_init() failed  errno: %d \n", ret);
		return KERN_FAILURE;
	}

	os_log(OS_LOG_DEFAULT, "lock group(%s) allocated \n", EXT2_LCKGRP_NAME);

	ret = vfs_fsadd(&ext2fs_vfsentry, &ext2fs_vfs_table_ref);
	if (ret != 0) {
		os_log(OS_LOG_DEFAULT, "vfs_fsadd() failed  errno: %d \n", ret);
		ext2fs_vfs_table_ref = NULL;
		ext2_fini();
		return KERN_FAILURE;
	}

	os_log(OS_LOG_DEFAULT, "%s file system registered \n",
	    ext2fs_vfsentry.vfe_fsname);

	os_log(OS_LOG_DEFAULT, "loaded %s version %s build %s (%s) \n",
	    BUNDLEID_S, KEXTVERSION_S, KEXTBUILD_S, __TS__);

	return KERN_SUCCESS;
}

kern_return_t
ext2_stop(kmod_info_t *ki, __unused void *d)
{
	uuid_string_t uuid;
	int ret;

	ret = libkext_vma_uuid(ki->address, uuid);
	if (ret != 0) {
		os_log(OS_LOG_DEFAULT, "libkext_vma_uuid() failed  errno: %d \n", ret);
		return KERN_FAILURE;
	}

	if (ext2fs_vfs_table_ref != NULL) {
		ret = vfs_fsremove(ext2fs_vfs_table_ref);
		if (ret != 0) {
			os_log(OS_LOG_DEFAULT,
			    "vfs_fsremove() failed  errno: %d \n", ret);
			return KERN_FAILURE;
		}
		ext2fs_vfs_table_ref = NULL;
	}

	ext2_fini();
	libkext_massert();

	os_log(OS_LOG_DEFAULT, "unloaded %s version %s build %s (%s) \n",
	    BUNDLEID_S, KEXTVERSION_S, KEXTBUILD_S, __TS__);

	return KERN_SUCCESS;
}

KMOD_EXPLICIT_DECL (BUNDLEID_S, KEXTBUILD_S, ext2_start, ext2_stop)
  __attribute__ ((visibility ("default")))

__private_extern__ kmod_start_func_t *_realmain = ext2_start;
__private_extern__ kmod_start_func_t *_antimain = ext2_stop;
__private_extern__ int _kext_apple_cc = __APPLE_CC__;
