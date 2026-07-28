/*
 * Copyright (c) 2022-2026 Sunneva N. Mariu
 *
 * ext2fs.util.c
 *
 * The helper diskarbitrationd runs to decide whether a piece of media holds an
 * ext2 file system, and to report its volume name and UUID. The .fs bundle's
 * FSMediaTypes entries name this binary as their FSProbeExecutable.
 *
 * Only the informational verbs are implemented. Mounting goes through the
 * bundle's FSMountExecutable (mount_ext2fs) and repair through
 * FSRepairExecutable (fsck_ext2fs), so -m, -u and -r would never be reached;
 * they are rejected rather than silently pretending to succeed.
 *
 * Exit status is one of the FSUR_* values from <sys/loadable_fs.h>. They are
 * negative and get truncated to a byte by exit(2); that is the convention every
 * system .util binary follows and what diskarbitrationd expects.
 */

#include <sys/types.h>
#include <sys/disk.h>
#include <sys/ioctl.h>
#include <sys/loadable_fs.h>
#include <sys/stat.h>

#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <fs/ext2fs/ext2fs.h>
#include <fs/ext2fs/ext2_compat.h>

static void	usage(void) __dead2;
static int	readsb(const char *, struct ext2fs *);

/*
 * Pull the primary superblock off the device.
 *
 * The raw node only accepts reads that are aligned to, and a multiple of, the
 * device's block size, so rather than reading 1024 bytes at offset 1024 we
 * read the aligned window that contains it. That matters on 4Kn media, where
 * the superblock offset is not itself a valid read offset.
 */
static int
readsb(const char *rawdev, struct ext2fs *sb)
{
	uint32_t bsize;
	off_t base;
	size_t span;
	uint8_t *buf;
	ssize_t n;
	int fd, ok = 0;

	fd = open(rawdev, O_RDONLY);
	if (fd == -1)
		return (0);

	if (ioctl(fd, DKIOCGETBLOCKSIZE, &bsize) == -1 || bsize == 0)
		bsize = DEV_BSIZE;

	base = (off_t)(SBOFF / bsize) * bsize;
	span = (size_t)(((SBOFF - base) + SBSIZE + bsize - 1) / bsize) * bsize;

	buf = malloc(span);
	if (buf == NULL) {
		close(fd);
		return (0);
	}

	n = pread(fd, buf, span, base);
	if (n == (ssize_t)span) {
		memcpy(sb, buf + (SBOFF - base), SBSIZE);
		ok = 1;
	}

	free(buf);
	close(fd);

	return (ok);
}

/*
 * A superblock is ours if the magic matches and the block size is one ext2
 * actually permits. The block size check keeps us from claiming media whose
 * bytes happen to contain 0xEF53 in the right place.
 */
static int
is_ext2(const struct ext2fs *sb)
{
	uint32_t logbs;

	if (fs2h16(sb->e2fs_magic) != E2FS_MAGIC)
		return (0);

	logbs = fs2h32(sb->e2fs_log_bsize);
	if ((MINBSIZE << logbs) > EXT2_MAXBSIZE)
		return (0);

	return (1);
}

/*
 * Volume name, as a NUL-terminated string. e2fs_vname is a fixed 16-byte field
 * that is not necessarily terminated, so it is copied rather than printed in
 * place.
 */
static void
volname(const struct ext2fs *sb, char *out, size_t outlen)
{
	size_t n;

	n = sizeof(sb->e2fs_vname);
	if (n >= outlen)
		n = outlen - 1;

	memcpy(out, sb->e2fs_vname, n);
	out[n] = '\0';
}

int
main(int argc, char *argv[])
{
	struct ext2fs sb;
	char rawdev[PATH_MAX];
	char name[sizeof(sb.e2fs_vname) + 1];
	const char *dev;
	char what;

	if (argc < 3)
		usage();

	if (argv[1][0] != '-' || argv[1][1] == '\0' || argv[1][2] != '\0')
		usage();
	what = argv[1][1];

	/*
	 * The device arrives as a bare BSD name ("disk0s2"). Probing reads a
	 * single small window, so the raw node is the right one: it avoids
	 * disturbing the buffer cache for a device we may not even claim.
	 */
	dev = argv[2];
	if (strchr(dev, '/') != NULL)
		return (FSUR_INVAL);
	(void)snprintf(rawdev, sizeof(rawdev), "/dev/r%s", dev);

	switch (what) {
	case FSUC_PROBE:
	case FSUC_PROBEFORINIT:
		if (!readsb(rawdev, &sb))
			return (FSUR_IO_FAIL);
		if (!is_ext2(&sb))
			return (FSUR_UNRECOGNIZED);

		/*
		 * diskarbitrationd reads the volume name from stdout. An
		 * unnamed volume prints nothing, which it treats as "no name"
		 * and falls back to a localized default.
		 */
		volname(&sb, name, sizeof(name));
		(void)printf("%s", name);

		return (what == FSUC_PROBEFORINIT ?
		    FSUR_INITRECOGNIZED : FSUR_RECOGNIZED);

	case 'k':	/* report the volume UUID */
		if (!readsb(rawdev, &sb))
			return (FSUR_IO_FAIL);
		if (!is_ext2(&sb))
			return (FSUR_UNRECOGNIZED);

		(void)printf("%02X%02X%02X%02X-%02X%02X-%02X%02X-"
		    "%02X%02X-%02X%02X%02X%02X%02X%02X",
		    sb.e2fs_uuid[0], sb.e2fs_uuid[1], sb.e2fs_uuid[2],
		    sb.e2fs_uuid[3], sb.e2fs_uuid[4], sb.e2fs_uuid[5],
		    sb.e2fs_uuid[6], sb.e2fs_uuid[7], sb.e2fs_uuid[8],
		    sb.e2fs_uuid[9], sb.e2fs_uuid[10], sb.e2fs_uuid[11],
		    sb.e2fs_uuid[12], sb.e2fs_uuid[13], sb.e2fs_uuid[14],
		    sb.e2fs_uuid[15]);

		return (FSUR_IO_SUCCESS);

	default:
		/* Mount, unmount and repair are the bundle's job, not ours. */
		return (FSUR_INVAL);
	}
}

static void
usage(void)
{
	(void)fprintf(stderr,
	    "usage: ext2fs.util -p device removable writable\n"
	    "       ext2fs.util -k device\n");
	exit(FSUR_INVAL);
}
