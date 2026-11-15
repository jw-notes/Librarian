// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 jw-notes <https://github.com/jw-notes>
// This file is part of Librarian. Additional terms under GPLv3 section 7
// and third-party notices: see the NOTICE file.

/* libarchive configuration for Librarian: only reading RAR / RAR5 archives
 * (CBR comics), on the host (Linux) and the PS4 (OpenOrbis). Everything
 * optional is left out: no iconv, locales, ACLs, xattrs, compression libraries
 * or encryption. Used with -DHAVE_CONFIG_H and -include of this file. */
#ifndef LIBRARIAN_LIBARCHIVE_CONFIG_H
#define LIBRARIAN_LIBARCHIVE_CONFIG_H

/* libarchive's private headers check that a configuration was included. */
#define __LIBARCHIVE_CONFIG_H_INCLUDED 1
#define HAVE_FCNTL 1
#define HAVE_PTHREAD_H 1

#define HAVE_CTYPE_H 1
#define HAVE_ERRNO_H 1
#define HAVE_FCNTL_H 1
#define HAVE_INTTYPES_H 1
#define HAVE_LIMITS_H 1
#define HAVE_STDARG_H 1
#define HAVE_STDINT_H 1
#define HAVE_STDLIB_H 1
#define HAVE_STRING_H 1
#define HAVE_STRINGS_H 1
#define HAVE_SYS_STAT_H 1
#define HAVE_SYS_TYPES_H 1
#define HAVE_SYS_TIME_H 1
#define HAVE_TIME_H 1
#define HAVE_UNISTD_H 1
#define HAVE_WCHAR_H 1
#define HAVE_EILSEQ 1
#define HAVE_FSTAT 1
#define HAVE_LSTAT 1
#define HAVE_MEMMOVE 1
#define HAVE_MEMSET 1
#define HAVE_STRCHR 1
#define HAVE_STRDUP 1
#define HAVE_STRERROR 1
#define HAVE_STRRCHR 1
#define HAVE_GMTIME_R 1
#define HAVE_LOCALTIME_R 1
#define HAVE_WCSCMP 1
#define HAVE_WCSCPY 1
#define HAVE_WCSLEN 1
#define HAVE_WCTOMB 1
#define HAVE_WMEMCMP 1
#define HAVE_WMEMCPY 1
#define HAVE_WMEMMOVE 1
#define HAVE_MBRTOWC 1
#define HAVE_WCRTOMB 1
#define HAVE_LONG_LONG_INT 1
#define HAVE_UNSIGNED_LONG_LONG 1
#define HAVE_UNSIGNED_LONG_LONG_INT 1
#define HAVE_INTMAX_T 1
#define HAVE_UINTMAX_T 1
#define HAVE_DECL_SIZE_MAX 1
#define HAVE_DECL_SSIZE_MAX 1
#define HAVE_DECL_INT32_MAX 1
#define HAVE_DECL_INT32_MIN 1
#define HAVE_DECL_INT64_MAX 1
#define HAVE_DECL_INT64_MIN 1
#define HAVE_DECL_UINT32_MAX 1
#define HAVE_DECL_UINT64_MAX 1
#define HAVE_DECL_INTMAX_MAX 1
#define HAVE_DECL_INTMAX_MIN 1
#define HAVE_DECL_UINTMAX_MAX 1
#define HAVE_WORKING_EXT2_IOC_GETFLAGS 0

#define ARCHIVE_ACL_SUPPORT 0
#define ARCHIVE_XATTR_SUPPORT 0
#define ARCHIVE_CRYPTO_NONE 1

/* ssize_t, uid/gid etc. come from <sys/types.h>. */
#define HAVE_PID_T 1
#define HAVE_SIZE_T 1
#define HAVE_SSIZE_T 1
#define HAVE_UID_T 1
#define HAVE_GID_T 1
#define HAVE_ID_T 1
#define HAVE_MODE_T 1
#define HAVE_OFF_T 1
#define HAVE_INT64_T 1
#define HAVE_UINT64_T 1
#define HAVE_INT32_T 1
#define HAVE_UINT32_T 1
#define HAVE_INT16_T 1
#define HAVE_UINT16_T 1
#define HAVE_UINT8_T 1

#endif
