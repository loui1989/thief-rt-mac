#pragma once

// Most engine code is compiled with -fpack-struct=1. System structures the
// C library fills in (struct stat, timespec, tm, dirent) must keep their
// native layout, or calls such as stat() write past the caller's copy: on
// macOS struct stat has alignment padding that packing removes. This header
// is force-included first, so declaring them here, at natural packing, keeps
// every later include of these headers (guarded) consistent.
//
// Some macOS SDK headers (sys/fcntl.h, sys/dirent.h) end with "#pragma
// pack()", which resets to the command line's 1-byte default rather than
// popping, so natural packing is re-established after each one. Including
// them here, once, also keeps them from resetting the packing of a later
// "#pragma pack(push, 8)" block around C++ standard headers.
#pragma pack(push, 8)
#include <fcntl.h>
#pragma pack(8)
#include <limits.h>
#pragma pack(8)
#include <stdio.h>
#pragma pack(8)
#include <sys/stat.h>
#pragma pack(8)
#include <sys/time.h>
#pragma pack(8)
#include <time.h>
#pragma pack(8)
#include <dirent.h>
#pragma pack(8)
#include <unistd.h>
#pragma pack(8)
#include <stdlib.h>
#pragma pack(8)
#include <string.h>
#pragma pack(8)
#include <signal.h>
#pragma pack(8)
#include <pthread.h>
#pragma pack(8)
#include <dlfcn.h>
#if defined(__cplusplus) && !defined(__APPLE__)
// (libstdc++, unlike macOS's libc++, keeps parts of its containers compiled
// into the shared library -- the red-black tree's rebalancing, the hash
// tables' rehash policy -- at natural packing: a std::map declared under
// -fpack-struct=1 has its nodes laid out differently from the library's,
// and the first rebalance corrupts it. Declared here, at natural packing.)
#pragma pack(8)
// (only these: <string> must come after scrpttyp.h's "string" macro where
// the script code expects it)
#include <map>
#include <set>
#endif
#pragma pack(pop)
#include <types.h>
#if defined(__APPLE__) && defined(__aarch64__)
#ifdef __cplusplus
static_assert(sizeof(struct stat) == 144 && alignof(struct timespec) == 8,
              "system structs must not be packed");
#else
_Static_assert(sizeof(struct stat) == 144 && _Alignof(struct timespec) == 8,
               "system structs must not be packed");
#endif
#endif

#ifndef _MAX_PATH
#ifndef PATH_MAX
#define PATH_MAX 4096
#endif
#define _MAX_PATH PATH_MAX
#endif
#ifndef O_BINARY
#define O_BINARY 0
#endif
#ifndef _O_BINARY
#define _O_BINARY O_BINARY
#endif

EXTERN FILE *PlatformFopen(const char *path, const char *mode);
EXTERN int PlatformOpen(const char *path, int flags, ...);
EXTERN int PlatformAccess(const char *path, int mode);
EXTERN int PlatformChangeDirectory(const char *path);
EXTERN int PlatformRemoveDirectory(const char *path);
EXTERN int PlatformUnlink(const char *path);

// The engine historically assumes Win32's case-preserving, case-insensitive
// lookup semantics. Route its C file opens through the Unix compatibility
// resolver instead of requiring a case-insensitive host filesystem.
#define fopen PlatformFopen
#define _access PlatformAccess
#define _chdir PlatformChangeDirectory
#define _close close
#define _filelength(fd) lseek((fd), 0, SEEK_END)
#define _getcwd getcwd
#define _lseek lseek
#define _open PlatformOpen
#define _read read
#define _rmdir PlatformRemoveDirectory
#define _unlink PlatformUnlink
#define _write write
#define _mkdir(path) mkdir((path), 0755)
