#!/bin/bash
#
# uninstall.sh
#
# Removes every trace of ext2fs from the system: unmounts any ext2 volumes,
# unloads the kext and deletes the installed files. Must already be running as
# root, and takes no input - the confirmation and the privilege escalation
# belong to whoever calls it.
#
# This is the single source of truth for what an uninstall removes. The
# Terminal front-end (installer/uninstall.command) uses it; keep it in step
# with the `uninstall` target in the top-level Makefile.
#
set -u

BUNDLE_ID=com.beako.filesystems.ext2fs
EXT_DIR=/Library/Extensions
FS_DIR=/Library/Filesystems
SBIN_DIR=/usr/local/sbin

if [ "$(id -u)" -ne 0 ]; then
    echo "uninstall.sh must be run as root." >&2
    exit 1
fi

echo "==> Uninstalling ext2fs"

# Unmount anything still mounted through this file system. Mounts are matched
# on the type reported by mount(8), which is the vfe_fsname the kext
# registered.
echo "  - unmounting ext2fs volumes"
mount | awk '/\(ext2fs[ ,]/ { print $3 }' | while read -r mp; do
    echo "      $mp"
    umount "$mp" 2>/dev/null || diskutil unmount force "$mp" 2>/dev/null || true
done

echo "  - unloading the kext and clearing the staging cache"
kmutil unload -b "$BUNDLE_ID" 2>/dev/null || true
kmutil clear-staging 2>/dev/null || true

# Remove the symlinks before the bundle they point into, so nothing is left
# dangling if the removal is interrupted. Only remove them if they actually
# point at our bundle - someone may have put their own binary there.
echo "  - removing $SBIN_DIR symlinks"
for t in newfs_ext2fs fsck_ext2fs; do
    link="$SBIN_DIR/$t"
    if [ -L "$link" ]; then
        case "$(readlink "$link")" in
            "$FS_DIR/ext2fs.fs/"*) rm -f "$link" ;;
            *) echo "      leaving $link (points elsewhere)" ;;
        esac
    fi
done

echo "  - removing the installed bundles"
rm -rf "$EXT_DIR/ext2fs.kext"
rm -rf "$FS_DIR/ext2fs.fs"

# Forget the installer receipt so a later install is treated as fresh rather
# than as an upgrade over files that are no longer there.
echo "  - forgetting the installer receipt"
pkgutil --forget "$BUNDLE_ID.pkg" >/dev/null 2>&1 || true

echo "==> ext2fs removed."
echo "    The kext stays approved in System Settings until you revoke it there;"
echo "    that is harmless with the files gone."

exit 0
