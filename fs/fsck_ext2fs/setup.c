/*	$OpenBSD: setup.c,v 1.36 2025/09/17 16:07:57 deraadt Exp $	*/
/*	$NetBSD: setup.c,v 1.1 1997/06/11 11:22:01 bouyer Exp $	*/

/*
 * Copyright (c) 1997 Manuel Bouyer.
 * Copyright (c) 1980, 1986, 1993
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

#define DKTYPENAMES
#include <sys/param.h>	/* DEV_BSIZE roundup */
#include <sys/time.h>
#include <fs/ext2fs/ext2_dinode.h>
#include <fs/ext2fs/ext2fs.h>
#include <fs/ext2fs/ext2_compat.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/disk.h>

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <ctype.h>
#include <err.h>
#include <limits.h>

#include "fsck.h"
#include "extern.h"
#include "fsutil.h"

#define POWEROF2(num)	(((num) & ((num) - 1)) == 0)

void badsb(int, char *);
int calcsb(char *, int, struct m_ext2fs *);
static int getsecsize(int, uint32_t *);
static int opendisk(const char *, int, char **);
static int readsb(int);
static char rdevname[PATH_MAX];

/* Backing store for the on-disk superblock the fallback in calcsb() builds. */
static struct ext2fs protosb;

int
setup(char *dev)
{
	long cg, asked, i;
	long bmapsize;
	off_t sizepb;
	struct stat statb;
	struct m_ext2fs proto;
	char *realdev;
	int doskipclean;
	u_int64_t maxfilesize;

	havesb = 0;
	fswritefd = -1;
	doskipclean = skipclean;
	if ((fsreadfd = opendisk(dev, O_RDONLY, &realdev)) == -1) {
		printf("Can't open %s: %s\n", dev, strerror(errno));
		return (0);
	}
	if (strncmp(dev, realdev, PATH_MAX) != 0) {
		blockcheck(unrawname(realdev));
		strlcpy(rdevname, realdev, sizeof(rdevname));
		setcdevname(rdevname, dev, preen);
	}
	if (fstat(fsreadfd, &statb) == -1) {
		printf("Can't stat %s: %s\n", realdev, strerror(errno));
		return (0);
	}
	if (!S_ISCHR(statb.st_mode)) {
		pfatal("%s is not a character device", realdev);
		if (reply("CONTINUE") == 0) {
			close(fsreadfd);
			return (0);
		}
	}
	if (preen == 0)
		printf("** %s", realdev);
	if (nflag || (fswritefd = opendisk(dev, O_WRONLY, NULL)) == -1) {
		fswritefd = -1;
		if (preen)
			pfatal("NO WRITE ACCESS");
		printf(" (NO WRITE)");
	}
	if (preen == 0)
		printf("\n");
	fsmodified = 0;
	lfdir = 0;
	initbarea(&sblk);
	initbarea(&asblk);
	sblk.b_un.b_buf = malloc(SBSIZE);
	asblk.b_un.b_buf = malloc(SBSIZE);
	if (sblk.b_un.b_buf == NULL || asblk.b_un.b_buf == NULL)
		errexit("cannot allocate space for superblock\n");
	{
		uint32_t devsecsize;

		if (getsecsize(fsreadfd, &devsecsize))
			secsize = (int)devsecsize;
		else
			secsize = DEV_BSIZE;
	}

	/* pledge(2) is OpenBSD-only; macOS has no equivalent to drop into. */

	/*
	 * Read in the superblock, looking for alternates if necessary
	 */
	if (readsb(1) == 0) {
		if (bflag || preen || calcsb(realdev, fsreadfd, &proto) == 0)
			return(0);
		if (reply("LOOK FOR ALTERNATE SUPERBLOCKS") == 0)
			return (0);
		for (cg = 1; cg < proto.e2fs_gcount; cg++) {
			bflag = fsbtodb(&proto,
				cg * proto.e2fs->e2fs_bpg + proto.e2fs->e2fs_first_dblock);
			if (readsb(0) != 0)
				break;
		}
		if (cg >= proto.e2fs_gcount) {
			printf("%s %s\n%s %s\n%s %s\n",
			    "SEARCH FOR ALTERNATE SUPER-BLOCK",
			    "FAILED. YOU MUST USE THE",
			    "-b OPTION TO FSCK_FFS TO SPECIFY THE",
			    "LOCATION OF AN ALTERNATE",
			    "SUPER-BLOCK TO SUPPLY NEEDED",
			    "INFORMATION; SEE fsck_ext2fs(8).");
			return(0);
		}
		doskipclean = 0;
		pwarn("USING ALTERNATE SUPERBLOCK AT %d\n", bflag);
	}
	if (debug)
		printf("state = %d\n", sblock.e2fs->e2fs_state);
	if (sblock.e2fs->e2fs_state == E2FS_ISCLEAN) {
		if (doskipclean) {
			pwarn("%sile system is clean; not checking\n",
			    preen ? "f" : "** F");
			return (-1);
		}
		if (!preen)
			pwarn("** File system is already clean\n");
	}
	maxfsblock = sblock.e2fs->e2fs_bcount;
	maxino = sblock.e2fs_gcount * sblock.e2fs->e2fs_ipg;
	sizepb = sblock.e2fs_bsize;
	maxfilesize = sblock.e2fs_bsize * NDADDR - 1;
	for (i = 0; i < NIADDR; i++) {
		sizepb *= NINDIR(&sblock);
		maxfilesize += sizepb;
	}
	/*
	 * Check and potentially fix certain fields in the super block.
	 */
	if (/* (sblock.e2fs->e2fs_rbcount < 0) || */
			(sblock.e2fs->e2fs_rbcount > sblock.e2fs->e2fs_bcount)) {
		pfatal("IMPOSSIBLE RESERVED BLOCK COUNT=%d IN SUPERBLOCK",
			sblock.e2fs->e2fs_rbcount);
		if (reply("SET TO DEFAULT") == 1) {
			sblock.e2fs->e2fs_rbcount = sblock.e2fs->e2fs_bcount * 0.1;
			sbdirty();
			dirty(&asblk);
		}
	}
	if (sblock.e2fs->e2fs_bpg != sblock.e2fs->e2fs_fpg) {
		pfatal("WRONG FPG=%d (BPG=%d) IN SUPERBLOCK",
			sblock.e2fs->e2fs_fpg, sblock.e2fs->e2fs_bpg);
		return 0;
	}
	if (asblk.b_dirty && !bflag) {
		copyback_sb(&asblk);
		flush(fswritefd, &asblk);
	}
	/*
	 * read in the summary info.
	 */

	sblock.e2fs_gd = calloc(sblock.e2fs_gdbcount, sblock.e2fs_bsize);
	if (sblock.e2fs_gd == NULL)
		errexit("out of memory\n");
	asked = 0;
	for (i=0; i < sblock.e2fs_gdbcount; i++) {
		if (bread(fsreadfd,
			(char *)sblock.e2fs_gd + (size_t)i * sblock.e2fs_bsize,
			fsbtodb(&sblock, ((sblock.e2fs_bsize>1024)?0:1)+i+1),
			sblock.e2fs_bsize) != 0 && !asked) {
			pfatal("BAD SUMMARY INFORMATION");
			if (reply("CONTINUE") == 0)
				errexit("%s\n", "");
			asked++;
		}
	}
	/*
	 * allocate and initialize the necessary maps
	 */
	bmapsize = roundup(howmany(maxfsblock, NBBY), sizeof(int16_t));
	blockmap = calloc((unsigned)bmapsize, sizeof (char));
	if (blockmap == NULL) {
		printf("cannot alloc %u bytes for blockmap\n",
			(unsigned)bmapsize);
		goto badsblabel;
	}
	statemap = calloc((unsigned)(maxino + 2), sizeof(char));
	if (statemap == NULL) {
		printf("cannot alloc %u bytes for statemap\n",
			(unsigned)(maxino + 1));
		goto badsblabel;
	}
	typemap = calloc((unsigned)(maxino + 1), sizeof(u_char));
	if (typemap == NULL) {
		printf("cannot alloc %u bytes for typemap\n",
		    (unsigned)(maxino + 1));
		goto badsblabel;
	}
	lncntp = calloc((unsigned)(maxino + 1), sizeof(int16_t));
	if (lncntp == NULL) {
		printf("cannot alloc %u bytes for lncntp\n",
			(unsigned)((maxino + 1) * sizeof(int16_t)));
		goto badsblabel;
	}
	for (numdirs = 0, cg = 0; cg < sblock.e2fs_gcount; cg++) {
		numdirs += letoh16(EXT2_GD(&sblock, cg)->ext2bgd_ndirs);
	}
	inplast = 0;
	listmax = numdirs + 10;
	inpsort = calloc((unsigned)listmax, sizeof(struct inoinfo *));
	inphead = calloc((unsigned)numdirs, sizeof(struct inoinfo *));
	if (inpsort == NULL || inphead == NULL) {
		printf("cannot alloc %u bytes for inphead\n",
			(unsigned)(numdirs * sizeof(struct inoinfo *)));
		goto badsblabel;
	}
	bufinit();
	return (1);

badsblabel:
	ckfini(0);
	return (0);
}

/*
 * Read in the super block and its summary info.
 */
static int
readsb(int listerr)
{
	daddr32_t super = bflag ? bflag : SBOFF / DEV_BSIZE;

	if (bread(fsreadfd, (char *)sblk.b_un.b_fs, super, (long)SBSIZE) != 0)
		return (0);
	sblk.b_bno = super;
	sblk.b_size = SBSIZE;

	/* Copy the superblock in memory */
	e2fs_sbload(sblk.b_un.b_fs, sblock.e2fs);

	/*
	 * run a few consistency checks of the super block
	 */
	if (sblock.e2fs->e2fs_magic != E2FS_MAGIC) {
		badsb(listerr, "MAGIC NUMBER WRONG"); return (0);
	}
	if (sblock.e2fs->e2fs_log_bsize > 2) {
		badsb(listerr, "BAD LOG_BSIZE"); return (0);
	}
	if (sblock.e2fs->e2fs_bpg == 0) {
		badsb(listerr, "BAD BLOCKS PER GROUP"); return (0);
	}

	/* compute the dynamic fields of the in-memory sb */
	/* compute dynamic sb infos */
	sblock.e2fs_gcount =
		howmany(sblock.e2fs->e2fs_bcount - sblock.e2fs->e2fs_first_dblock,
		sblock.e2fs->e2fs_bpg);
	/* XXX assume hw bsize = 512 */
	sblock.e2fs_fsbtodb = sblock.e2fs->e2fs_log_bsize + 1;
	sblock.e2fs_bsize = 1024 << sblock.e2fs->e2fs_log_bsize;
	sblock.e2fs_bshift = LOG_MINBSIZE + sblock.e2fs->e2fs_log_bsize;
	sblock.e2fs_qbmask = sblock.e2fs_bsize - 1;
	/*
	 * OpenBSD also cached the complement of qbmask in e2fs_bmask. The
	 * shared m_ext2fs has no such member and nothing reads it -
	 * <fs/ext2fs/fs.h> builds blkoff() out of e2fs_qbmask alone.
	 */
	/*
	 * Group descriptors are EXT2_MIN_GD_SIZE bytes unless the volume has
	 * the 64bit feature, where the superblock gives their real size - 64,
	 * for anything mke2fs writes with metadata_csum. Deriving the stride
	 * from sizeof(struct ext2_gd) instead walked the table at half its
	 * true pitch, so every group after the first was read from the middle
	 * of its predecessor. This mirrors compute_sb_data() in the kext.
	 */
	sblock.e2fs_gdsize = EXT2_MIN_GD_SIZE;
	if (sblock.e2fs->e2fs_rev > E2FS_REV0 &&
	    (sblock.e2fs->e2fs_features_incompat & EXT2F_INCOMPAT_64BIT) &&
	    sblock.e2fs->e3fs_desc_size != 0)
		sblock.e2fs_gdsize = sblock.e2fs->e3fs_desc_size;
	if (sblock.e2fs_gdsize < EXT2_MIN_GD_SIZE ||
	    (sblock.e2fs_gdsize & (sblock.e2fs_gdsize - 1)) != 0 ||
	    sblock.e2fs_gdsize > sblock.e2fs_bsize) {
		pfatal("NONSENSICAL GROUP DESCRIPTOR SIZE=%u IN SUPERBLOCK",
		    sblock.e2fs_gdsize);
		return (0);
	}

	sblock.e2fs_gdbcount = howmany(sblock.e2fs_gcount,
		sblock.e2fs_bsize / sblock.e2fs_gdsize);
	sblock.e2fs_ipb = sblock.e2fs_bsize / EXT2_DINODE_SIZE(&sblock);
	sblock.e2fs_itpg = sblock.e2fs->e2fs_ipg/sblock.e2fs_ipb;
	/*
	 * Mirror the per-group counts and the inode size into the in-core
	 * superblock. OpenBSD's m_ext2fs had no such members and its macros
	 * read the embedded on-disk copy directly, but the shared
	 * <fs/ext2fs/fs.h> resolves ino_to_cg() through e2fs_ipg and dtog()
	 * through e2fs_bpg. Leaving them zero is not diagnosed on arm64, where
	 * integer division by zero yields zero rather than trapping: every
	 * inode would appear to live in group 0 and fsck would report the
	 * first group's inodes over and over as duplicates.
	 */
	sblock.e2fs_ipg = sblock.e2fs->e2fs_ipg;
	sblock.e2fs_bpg = sblock.e2fs->e2fs_bpg;
	sblock.e2fs_isize = EXT2_DINODE_SIZE(&sblock);

	/*
	 * Compute block size that the filesystem is based on,
	 * according to fsbtodb, and adjust superblock block number
	 * so we can tell if this is an alternate later.
	 */
	sblk.b_bno = super / DEV_BSIZE;

	if (sblock.e2fs_gcount == 1) {
		/* no alternate superblock; assume it's okey */
		havesb = 1;
		return 1;
	}

	getblk(&asblk, 1 * sblock.e2fs->e2fs_bpg + sblock.e2fs->e2fs_first_dblock,
		(long)SBSIZE);
	if (asblk.b_errs)
		return (0);
	if (bflag) {
		havesb = 1;
		return (1);
	}

	/*
	 * Set all possible fields that could differ, then do check
	 * of whole super block against an alternate super block.
	 * When an alternate super-block is specified this check is skipped.
	 */
	asblk.b_un.b_fs->e2fs_rbcount = sblk.b_un.b_fs->e2fs_rbcount;
	asblk.b_un.b_fs->e2fs_fbcount = sblk.b_un.b_fs->e2fs_fbcount;
	asblk.b_un.b_fs->e2fs_ficount = sblk.b_un.b_fs->e2fs_ficount;
	asblk.b_un.b_fs->e2fs_mtime = sblk.b_un.b_fs->e2fs_mtime;
	asblk.b_un.b_fs->e2fs_wtime = sblk.b_un.b_fs->e2fs_wtime;
	asblk.b_un.b_fs->e2fs_mnt_count = sblk.b_un.b_fs->e2fs_mnt_count;
	asblk.b_un.b_fs->e2fs_max_mnt_count = sblk.b_un.b_fs->e2fs_max_mnt_count;
	asblk.b_un.b_fs->e2fs_state = sblk.b_un.b_fs->e2fs_state;
	asblk.b_un.b_fs->e2fs_beh = sblk.b_un.b_fs->e2fs_beh;
	asblk.b_un.b_fs->e2fs_lastfsck = sblk.b_un.b_fs->e2fs_lastfsck;
	asblk.b_un.b_fs->e2fs_fsckintv = sblk.b_un.b_fs->e2fs_fsckintv;
	asblk.b_un.b_fs->e2fs_ruid = sblk.b_un.b_fs->e2fs_ruid;
	asblk.b_un.b_fs->e2fs_rgid = sblk.b_un.b_fs->e2fs_rgid;
	asblk.b_un.b_fs->e2fs_block_group_nr =
	    sblk.b_un.b_fs->e2fs_block_group_nr;
	asblk.b_un.b_fs->e2fs_features_rocompat &= ~EXT2F_ROCOMPAT_LARGE_FILE;
	asblk.b_un.b_fs->e2fs_features_rocompat |=
	    sblk.b_un.b_fs->e2fs_features_rocompat & EXT2F_ROCOMPAT_LARGE_FILE;
	memcpy(asblk.b_un.b_fs->e2fs_fsmnt, sblk.b_un.b_fs->e2fs_fsmnt,
	    sizeof(sblk.b_un.b_fs->e2fs_fsmnt));
	asblk.b_un.b_fs->e4fs_kbytes_written = sblk.b_un.b_fs->e4fs_kbytes_written;

	if (sblock.e2fs->e2fs_rev > E2FS_REV0 &&
	    ((sblock.e2fs->e2fs_features_incompat & ~EXT2F_INCOMPAT_SUPP) ||
	    (sblock.e2fs->e2fs_features_rocompat & ~EXT2F_ROCOMPAT_SUPP))) {
		if (debug) {
			printf("compat 0x%08x, incompat 0x%08x, compat_ro "
			    "0x%08x\n",
			    sblock.e2fs->e2fs_features_compat,
			    sblock.e2fs->e2fs_features_incompat,
			    sblock.e2fs->e2fs_features_rocompat);
		}
		badsb(listerr,"INCOMPATIBLE FEATURE BITS IN SUPER BLOCK");
		return 0;
	}
	if (memcmp(sblk.b_un.b_fs, asblk.b_un.b_fs, SBSIZE)) {
		if (debug) {
			u_int32_t *nlp, *olp, *endlp;

			printf("superblock mismatches\n");
			nlp = (u_int32_t *)asblk.b_un.b_fs;
			olp = (u_int32_t *)sblk.b_un.b_fs;
			endlp = olp + (SBSIZE / sizeof *olp);
			for ( ; olp < endlp; olp++, nlp++) {
				if (*olp == *nlp)
					continue;
				printf("offset %ld, original %ld, alternate %ld\n",
					(long)(olp - (u_int32_t *)sblk.b_un.b_fs),
					(long)letoh32(*olp),
					(long)letoh32(*nlp));
			}
		}
		badsb(listerr,
		"VALUES IN SUPER BLOCK DISAGREE WITH THOSE IN FIRST ALTERNATE");
		return (0);
	}
	havesb = 1;
	return (1);
}

void
copyback_sb(struct bufarea *bp)
{
	/* Copy the in-memory superblock back to buffer */
	bp->b_un.b_fs->e2fs_icount = letoh32(sblock.e2fs->e2fs_icount);
	bp->b_un.b_fs->e2fs_bcount = letoh32(sblock.e2fs->e2fs_bcount);
	bp->b_un.b_fs->e2fs_rbcount = letoh32(sblock.e2fs->e2fs_rbcount);
	bp->b_un.b_fs->e2fs_fbcount = letoh32(sblock.e2fs->e2fs_fbcount);
	bp->b_un.b_fs->e2fs_ficount = letoh32(sblock.e2fs->e2fs_ficount);
	bp->b_un.b_fs->e2fs_first_dblock =
					letoh32(sblock.e2fs->e2fs_first_dblock);
	bp->b_un.b_fs->e2fs_log_bsize = letoh32(sblock.e2fs->e2fs_log_bsize);
	bp->b_un.b_fs->e2fs_log_fsize = letoh32(sblock.e2fs->e2fs_log_fsize);
	bp->b_un.b_fs->e2fs_bpg = letoh32(sblock.e2fs->e2fs_bpg);
	bp->b_un.b_fs->e2fs_fpg = letoh32(sblock.e2fs->e2fs_fpg);
	bp->b_un.b_fs->e2fs_ipg = letoh32(sblock.e2fs->e2fs_ipg);
	bp->b_un.b_fs->e2fs_mtime = letoh32(sblock.e2fs->e2fs_mtime);
	bp->b_un.b_fs->e2fs_wtime = letoh32(sblock.e2fs->e2fs_wtime);
	bp->b_un.b_fs->e2fs_lastfsck = letoh32(sblock.e2fs->e2fs_lastfsck);
	bp->b_un.b_fs->e2fs_fsckintv = letoh32(sblock.e2fs->e2fs_fsckintv);
	bp->b_un.b_fs->e2fs_creator = letoh32(sblock.e2fs->e2fs_creator);
	bp->b_un.b_fs->e2fs_rev = letoh32(sblock.e2fs->e2fs_rev);
	bp->b_un.b_fs->e2fs_mnt_count = letoh16(sblock.e2fs->e2fs_mnt_count);
	bp->b_un.b_fs->e2fs_max_mnt_count =
					letoh16(sblock.e2fs->e2fs_max_mnt_count);
	bp->b_un.b_fs->e2fs_magic = letoh16(sblock.e2fs->e2fs_magic);
	bp->b_un.b_fs->e2fs_state = letoh16(sblock.e2fs->e2fs_state);
	bp->b_un.b_fs->e2fs_beh = letoh16(sblock.e2fs->e2fs_beh);
	bp->b_un.b_fs->e2fs_ruid = letoh16(sblock.e2fs->e2fs_ruid);
	bp->b_un.b_fs->e2fs_rgid = letoh16(sblock.e2fs->e2fs_rgid);
}

void
badsb(int listerr, char *s)
{

	if (!listerr)
		return;
	if (preen)
		printf("%s: ", cdevname());
	pfatal("BAD SUPER BLOCK: %s\n", s);
}

/*
 * Calculate a prototype superblock, used only to locate the backup
 * superblocks after the primary one has been found unreadable. When done the
 * cgsblock macro can be calculated and the e2fs_gcount field can be used. Do
 * NOT attempt to use other macros without verifying that their needed
 * information is available!
 *
 * OpenBSD took the block size and partition extent from the disk label. macOS
 * has no disk label: the partition size comes from the device itself, and the
 * block size cannot be recovered at all, so the ext2 default of 1024 is
 * assumed. That is the same guess newfs_ext2fs makes for small volumes, and if
 * it is wrong the backup superblock search below simply fails to find a valid
 * magic number and the user is told to supply -b.
 */
int
calcsb(char *dev, int devfd, struct m_ext2fs *fs)
{
	uint32_t secsz;
	uint64_t seccount;

	if (ioctl(devfd, DKIOCGETBLOCKSIZE, &secsz) == -1 ||
	    ioctl(devfd, DKIOCGETBLOCKCOUNT, &seccount) == -1) {
		pfatal("%s: CANNOT DETERMINE DEVICE GEOMETRY\n", dev);
		return (0);
	}
	if (secsz == 0 || seccount == 0) {
		pfatal("%s: DEVICE REPORTS A ZERO-SIZED PARTITION\n", dev);
		return (0);
	}

	memset(fs, 0, sizeof(struct m_ext2fs));
	memset(&protosb, 0, sizeof(protosb));
	fs->e2fs = &protosb;

	fs->e2fs_bsize = MINBSIZE;
	fs->e2fs->e2fs_log_bsize = 0;		/* 1024 << 0 */
	fs->e2fs->e2fs_bcount =
	    (uint32_t)((seccount * secsz) / fs->e2fs_bsize);
	fs->e2fs->e2fs_first_dblock = (fs->e2fs->e2fs_log_bsize == 0) ? 1 : 0;
	fs->e2fs->e2fs_bpg = fs->e2fs_bsize * NBBY;
	fs->e2fs_bshift = LOG_MINBSIZE + fs->e2fs->e2fs_log_bsize;
	fs->e2fs_qbmask = fs->e2fs_bsize - 1;
	fs->e2fs_gcount =
		howmany(fs->e2fs->e2fs_bcount - fs->e2fs->e2fs_first_dblock,
		fs->e2fs->e2fs_bpg);
	fs->e2fs_fsbtodb = fs->e2fs->e2fs_log_bsize + 1;
	/* Fabricated from device geometry: no superblock to read a size from. */
	fs->e2fs_gdsize = EXT2_MIN_GD_SIZE;
	fs->e2fs_gdbcount = howmany(fs->e2fs_gcount,
		fs->e2fs_bsize / fs->e2fs_gdsize);

	return (1);
}

/*
 * Sector size of an open device, replacing the disklabel's d_secsize.
 * Returns zero on failure, leaving the caller to fall back to DEV_BSIZE.
 */
static int
getsecsize(int fd, uint32_t *secsize)
{

	if (ioctl(fd, DKIOCGETBLOCKSIZE, secsize) == -1)
		return (0);
	if (*secsize == 0)
		return (0);
	return (1);
}

/*
 * Open a disk device by name, preferring the raw (character) node, and report
 * back the path actually opened. Replaces OpenBSD's opendev(3).
 */
static int
opendisk(const char *name, int flags, char **namep)
{
	static char path[PATH_MAX];
	const char *base;
	int fd;

	if (name[0] == '/') {
		base = strrchr(name, '/') + 1;
		if (strncmp(base, "disk", 4) == 0)
			(void)snprintf(path, sizeof(path), "%.*sr%s",
			    (int)(base - name), name, base);
		else
			(void)strlcpy(path, name, sizeof(path));
	} else if (strncmp(name, "rdisk", 5) == 0) {
		(void)snprintf(path, sizeof(path), "/dev/%s", name);
	} else {
		(void)snprintf(path, sizeof(path), "/dev/r%s", name);
	}

	fd = open(path, flags);
	if (fd == -1 && errno == ENOENT) {
		(void)strlcpy(path, name, sizeof(path));
		fd = open(path, flags);
	}

	if (fd != -1 && namep != NULL)
		*namep = path;

	return (fd);
}

daddr32_t
cgoverhead(int c)
{
	int overh;
	overh =	1 /* block bitmap */ +
		1 /* inode bitmap */ +
		sblock.e2fs_itpg;
	if (sblock.e2fs->e2fs_rev > E2FS_REV0 &&
	    sblock.e2fs->e2fs_features_rocompat & EXT2F_ROCOMPAT_SPARSE_SUPER) {
		if (cg_has_sb(c) == 0)
			return overh;
	}
	overh += 1 + sblock.e2fs_gdbcount;
	return overh;
}
