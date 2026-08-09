// Declaration-only stand-in for scripts/native-port-probe.py. See README.md.
#pragma once

#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <stdio.h>

#define _O_RDONLY O_RDONLY
#define _O_WRONLY O_WRONLY
#define _O_RDWR   O_RDWR
#define _O_APPEND O_APPEND
#define _O_CREAT  O_CREAT
#define _O_TRUNC  O_TRUNC
#define _O_EXCL   O_EXCL
#define _O_BINARY 0
#define _O_TEXT   0
#define _O_RANDOM 0
#define _O_SEQUENTIAL 0
#define O_BINARY  0
#define _S_IREAD  S_IRUSR
#define _S_IWRITE S_IWUSR
#define _A_NORMAL 0x00
#define _A_RDONLY 0x01
#define _A_HIDDEN 0x02
#define _A_SYSTEM 0x04
#define _A_SUBDIR 0x10
#define _A_ARCH   0x20

// MSVC spells the POSIX calls with a leading underscore.
#define _open   open
#define _close  close
#define _read   read
#define _write  write
#define _lseek  lseek
#define _tell(fd) lseek((fd), 0, SEEK_CUR)
#define _dup    dup
#define _dup2   dup2
#define _unlink unlink
#define _access access
#define _isatty isatty
#define _fileno fileno
#define _chsize ftruncate
#define _commit fsync

extern "C" {
long _filelength(int fd);
int  _findclose(intptr_t handle);
}

struct _finddata_t {
	unsigned attrib;
	long     time_create;
	long     time_access;
	long     time_write;
	long     size;
	char     name[260];
};
extern "C" {
intptr_t _findfirst(const char* spec, struct _finddata_t* data);
int      _findnext(intptr_t handle, struct _finddata_t* data);
}
