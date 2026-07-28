/*
 * Copyright (c) 2022-2026 Sunneva N. Mariu
 *
 * sys/endian.h
 *
 * FreeBSD's byte order header, which XNU has no equivalent of. The imported
 * ext2fs sources include it for the fixed-endian conversions; libkern supplies
 * the same operations under different names.
 *
 * Only the little-endian and big-endian conversion families are provided -
 * that is all the ext2fs sources use. The encode/decode helpers (le32dec and
 * friends) are deliberately absent rather than guessed at; add them here if a
 * later file needs them.
 */

#ifndef _BSDCOMPAT_SYS_ENDIAN_H_
#define	_BSDCOMPAT_SYS_ENDIAN_H_

#include <libkern/OSByteOrder.h>

#define	htole16(x)	OSSwapHostToLittleInt16(x)
#define	htole32(x)	OSSwapHostToLittleInt32(x)
#define	htole64(x)	OSSwapHostToLittleInt64(x)

#define	le16toh(x)	OSSwapLittleToHostInt16(x)
#define	le32toh(x)	OSSwapLittleToHostInt32(x)
#define	le64toh(x)	OSSwapLittleToHostInt64(x)

#define	htobe16(x)	OSSwapHostToBigInt16(x)
#define	htobe32(x)	OSSwapHostToBigInt32(x)
#define	htobe64(x)	OSSwapHostToBigInt64(x)

#define	be16toh(x)	OSSwapBigToHostInt16(x)
#define	be32toh(x)	OSSwapBigToHostInt32(x)
#define	be64toh(x)	OSSwapBigToHostInt64(x)

#define	bswap16(x)	OSSwapInt16(x)
#define	bswap32(x)	OSSwapInt32(x)
#define	bswap64(x)	OSSwapInt64(x)

#endif	/* !_BSDCOMPAT_SYS_ENDIAN_H_ */
