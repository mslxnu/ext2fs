ext2fs for macOS
================

A native ext2 file system, so that ext2 volumes mount like any other disk.


Installing
----------

Open the .pkg in this disk image and follow the installer.

macOS will not load a third-party file system extension until you approve it.
After the installer finishes:

  1. Open System Settings > General > Login Items & Extensions and allow the
     extension from "com.beako.filesystems". On some versions this appears
     under Privacy & Security instead, as a message about blocked system
     software.

  2. Restart your Mac.

After the restart, connecting an ext2 volume mounts it in the Finder.


What gets installed
-------------------

  /Library/Extensions/ext2fs.kext     the file system itself
  /Library/Filesystems/ext2fs.fs      recognises, mounts, formats and checks
                                      ext2 media; also holds newfs_ext2fs and
                                      fsck_ext2fs
  /usr/local/sbin/newfs_ext2fs        symlink into the bundle above
  /usr/local/sbin/fsck_ext2fs         symlink into the bundle above


Removing it
-----------

Run "Uninstall ext2fs.command" from this disk image. It unmounts any ext2
volumes, unloads the extension and deletes the files above.


Please read this
----------------

This file system is new and has had limited testing against real volumes. Do
not trust it with data you have not backed up somewhere else.

Volumes that use ext3 or ext4 features - a journal, or extents - are mounted
read-write with full journal replay on mount.
