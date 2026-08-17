/*	$OpenBSD: mount_ext2fs.c,v 1.20 2022/12/04 23:50:46 cheloha Exp $	*/
/*	$NetBSD: mount_ffs.c,v 1.3 1996/04/13 01:31:19 jtc Exp $	*/

/*-
 * Copyright (c) 1993, 1994
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
 * 3. Neither the name of the University nor the names of its contributors
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
 */

/*
 * Darwin port.  Two things differ from the OpenBSD original:
 *
 *   - There is no struct ufs_args and no export_args on macOS; exports are
 *     configured out of band by nfsd, not through the mount arguments.  The
 *     file system's own struct ext2_args is used instead.
 *
 *   - Apple's getmntopts() is the NetBSD four-argument form returning an
 *     opaque parse handle, not OpenBSD's three-argument void form.
 */

#include <sys/types.h>
#include <sys/mount.h>

#include <err.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>

#include "mntopts.h"

#include <fs/ext2fs/ext2_mount.h>

static void	ext2fs_usage(void) __dead2;

static const struct mntopt mopts[] = {
	MOPT_STDOPTS,
	MOPT_UPDATE,
	{ NULL, 0, 0, 0 }
};

int
main(int argc, char *argv[])
{
	struct ext2_args args;
	mntoptparse_t mp;
	int ch, mntflags, altflags;
	char fs_name[PATH_MAX], *errcause;

	mntflags = 0;
	altflags = 0;
	optind = optreset = 1;		/* Reset for parse of new argv. */
	while ((ch = getopt(argc, argv, "o:")) != -1) {
		switch (ch) {
		case 'o':
			mp = getmntopts(optarg, mopts, &mntflags, &altflags);
			if (mp == NULL)
				err(1, "getmntopts");
			freemntopts(mp);
			break;
		default:
			ext2fs_usage();
		}
	}
	argc -= optind;
	argv += optind;

	if (argc != 2)
		ext2fs_usage();

	memset(&args, 0, sizeof(args));
	args.fspec = argv[0];		/* The name of the device file. */
	args.e2_version = EXT2_ARGSVERSION;

	if (realpath(argv[1], fs_name) == NULL)	/* The mount point. */
		err(1, "realpath %s", argv[1]);

	if (mount(MOUNT_EXT2FS, fs_name, mntflags, &args) == -1) {
		int hint = 0;

		switch (errno) {
		case EMFILE:
			errcause = "mount table full";
			break;
		case EINVAL:
			/*
			 * Upstream said "specified device does not match
			 * mounted device" here. That describes one cause of
			 * EINVAL - a mismatched update mount - and every
			 * other cause arrived wearing it, which is a good way
			 * to send someone looking at the wrong thing. The
			 * driver returns EINVAL when the superblock is not
			 * one it can make sense of.
			 */
			errcause = "not an ext2/ext3/ext4 file system, or the "
			    "superblock is damaged";
			hint = 1;
			break;
		case ENOTSUP:
			errcause = "the file system uses features this driver "
			    "does not support";
			hint = 1;
			break;
		case EROFS:
			errcause = "the file system can only be mounted "
			    "read-only; retry with -o rdonly";
			hint = 1;
			break;
		case EPERM:
			errcause = "the file system is not clean; run "
			    "fsck_ext2fs, or mount -o rdonly";
			break;
		case EOPNOTSUPP:
			errcause = "file system not supported by the kernel "
			    "(is the ext2fs kext loaded?)";
			break;
		default:
			errcause = strerror(errno);
			break;
		}
		warnx("%s on %s: %s", args.fspec, fs_name, errcause);
		/*
		 * The driver names the offending feature, or whatever else it
		 * objected to, with printf(9). That only reaches the kernel
		 * log, so point at it rather than leaving the accurate
		 * diagnosis somewhere the reader does not know to look.
		 */
		if (hint)
			warnx("for the specific reason, see: log show --last 1m"
			    " --predicate 'process == \"kernel\"' | grep ext2fs");
		exit(1);
	}
	exit(0);
}

static void
ext2fs_usage(void)
{
	(void)fprintf(stderr,
		"usage: mount_ext2fs [-o options] special node\n");
	exit(1);
}
