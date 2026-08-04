#
# Flags shared by the userland tools.
#
# The tools are OpenBSD sources built against the same on-disk definitions the
# kext uses (../../include/fs/ext2fs), so there is exactly one description of
# the ext2 layout in the tree.
#

include ../../Makefile.inc

SDKROOT?=       $(shell xcrun --sdk macosx --show-sdk-path)

# Match the kext's arch handling: build for the native arch unless told
# otherwise, and accept arm64 as a spelling of arm64e.
NATIVE_ARCH:= $(shell uname -m)
ifeq ($(NATIVE_ARCH),arm64)
ARCHFLAGS?=          -arch arm64
TARGET_TRIPLE?=      arm64-apple-macos12.0
else
ARCHFLAGS?=          -arch x86_64
TARGET_TRIPLE?=      x86_64-apple-macos10.15
endif

PREFIX?=        /usr/local/sbin
MANPREFIX?=     /usr/local/share/man

CC=       $(shell xcrun -find -sdk $(SDKROOT) cc)

TOPDIR:= ../..

CFLAGS+= -target $(TARGET_TRIPLE) \
         -isysroot $(SDKROOT) \
         $(ARCHFLAGS) \
         -std=gnu99 \
         -D_DARWIN_C_SOURCE \
         -I$(TOPDIR)/include \
         -I$(TOPDIR)/vendor/libutil

CFLAGS+= -Wall -Wextra \
         -Wno-unused-parameter \
         -Wno-deprecated-declarations

debug: CFLAGS += -O0 -g
release: CFLAGS += -O2
