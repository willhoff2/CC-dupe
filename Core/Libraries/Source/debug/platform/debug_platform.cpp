/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2025 Electronic Arts Inc.
**
**	This program is free software: you can redistribute it and/or modify
**	it under the terms of the GNU General Public License as published by
**	the Free Software Foundation, either version 3 of the License, or
**	(at your option) any later version.
**
**	This program is distributed in the hope that it will be useful,
**	but WITHOUT ANY WARRANTY; without even the implied warranty of
**	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
**	GNU General Public License for more details.
**
**	You should have received a copy of the GNU General Public License
**	along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

//////////////////////////////////////////////////////////////////////////////
//
// The non-Windows side of the EA/Debug library. See debug_platform.h.
//
//////////////////////////////////////////////////////////////////////////////

#include "debug_platform.h"

#ifndef _WIN32

#include <cxxabi.h>
#include <dlfcn.h>
#include <errno.h>
#include <execinfo.h>
#include <fcntl.h>
#include <pwd.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

namespace DebugPlatform
{

//////////////////////////////////////////////////////////////////////////////
// Memory
//////////////////////////////////////////////////////////////////////////////

void *Alloc(unsigned numBytes)
{
  return malloc(numBytes);
}

void *ReAlloc(void *oldPtr, unsigned newSize)
{
  return realloc(oldPtr,newSize);
}

void Free(void *ptr)
{
  free(ptr);
}

//////////////////////////////////////////////////////////////////////////////
// Process and environment
//////////////////////////////////////////////////////////////////////////////

static void CopyToBuffer(char *buf, unsigned bufSize, const char *src)
{
  if (!buf||!bufSize)
    return;
  if (!src)
    src="";
  unsigned len=strlen(src);
  if (len>=bufSize)
    len=bufSize-1;
  memcpy(buf,src,len);
  buf[len]=0;
}

void GetExecutablePath(char *buf, unsigned bufSize)
{
  if (!buf||!bufSize)
    return;
  *buf=0;

#ifdef __APPLE__
  uint32_t size=bufSize;
  if (_NSGetExecutablePath(buf,&size)!=0)
    *buf=0;
#else
  ssize_t len=readlink("/proc/self/exe",buf,bufSize-1);
  if (len<0)
    len=0;
  buf[len]=0;
#endif
}

void GetComputerName(char *buf, unsigned bufSize)
{
  if (!buf||!bufSize)
    return;
  if (gethostname(buf,bufSize)!=0)
    CopyToBuffer(buf,bufSize,"localhost");
  buf[bufSize-1]=0;

  // Win32 reports the NetBIOS name, which has no domain part; match that.
  char *dot=strchr(buf,'.');
  if (dot)
    *dot=0;
}

void GetUserName(char *buf, unsigned bufSize)
{
  if (!buf||!bufSize)
    return;

  const char *name=getenv("USER");
  if (!name||!*name)
    name=getenv("LOGNAME");
  if (!name||!*name)
  {
    struct passwd *pw=getpwuid(getuid());
    name=pw?pw->pw_name:nullptr;
  }
  CopyToBuffer(buf,bufSize,name&&*name?name:"unknown");
}

void GetLocalTime(unsigned &year, unsigned &month, unsigned &day,
                  unsigned &hour, unsigned &minute, unsigned &second, unsigned &milliseconds)
{
  struct timeval tv;
  gettimeofday(&tv,nullptr);

  struct tm local;
  time_t seconds=tv.tv_sec;
  localtime_r(&seconds,&local);

  year=local.tm_year+1900;
  month=local.tm_mon+1;
  day=local.tm_mday;
  hour=local.tm_hour;
  minute=local.tm_min;
  second=local.tm_sec;
  milliseconds=tv.tv_usec/1000;
}

unsigned GetTickCount()
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC,&ts);
  unsigned long long ms=(unsigned long long)ts.tv_sec*1000ull+ts.tv_nsec/1000000ull;
  return (unsigned)ms;
}

void TerminateProcess(unsigned exitCode)
{
  _exit((int)exitCode);
}

void ReportFatal(const char *title, const char *text)
{
  // No dialog seam at this layer, so this is the loud part: straight to stderr, unbuffered, and
  // marked so it is obvious in a log that a Win32 message box would have been shown here.
  char header[256];
  snprintf(header,sizeof(header),"\n*** EA/Debug: %s ***\n",title?title:"fatal error");
  ssize_t ignored=write(STDERR_FILENO,header,strlen(header));
  if (text&&*text)
  {
    ignored=write(STDERR_FILENO,text,strlen(text));
    ignored=write(STDERR_FILENO,"\n",1);
  }
  (void)ignored;
}

void WriteDebuggerString(const char *text)
{
  if (!text||!*text)
    return;

  // OutputDebugString's destination is the attached debugger. Off Windows the equivalent is the
  // process' own stderr: lldb/gdb show it, and it is still there when no debugger is attached.
  ssize_t ignored=write(STDERR_FILENO,text,strlen(text));
  (void)ignored;
}

//////////////////////////////////////////////////////////////////////////////
// Pointer probes
//////////////////////////////////////////////////////////////////////////////

/*
  A cached descriptor for /dev/null. Writing to it is the cheapest way to ask the kernel whether a
  range is readable without risking a signal: the copy from user space fails with EFAULT and
  nothing is written anywhere.
*/
static int GetNullDescriptor()
{
  static int nullFd=-1;
  if (nullFd<0)
    nullFd=open("/dev/null",O_WRONLY|O_CLOEXEC);
  return nullFd;
}

bool IsBadReadPtr(const void *ptr, unsigned numBytes)
{
  if (!ptr)
    return true;
  if (!numBytes)
    return false;

  int nullFd=GetNullDescriptor();
  if (nullFd<0)
    // cannot check; Win32 says "readable" for a zero length range only, so be conservative and
    // claim readable rather than hiding data the caller wanted to dump.
    return false;

  const char *cur=(const char *)ptr;
  unsigned left=numBytes;
  while (left)
  {
    ssize_t written=write(nullFd,cur,left);
    if (written<0)
    {
      if (errno==EINTR)
        continue;
      return true;
    }
    if (!written)
      return true;
    cur+=written;
    left-=(unsigned)written;
  }
  return false;
}

bool IsBadCodePtr(const void *ptr)
{
  if (!ptr)
    return true;

  // dladdr() only resolves addresses that belong to a mapped image, which is the closest thing
  // to "is this executable" that is available portably.
  Dl_info info;
  if (dladdr(ptr,&info)&&info.dli_fbase)
    return false;

  // Not in a mapped image. It may still be JIT'd or a stripped mapping, so fall back to the read
  // probe rather than reporting a bad pointer for something the caller could dump.
  return IsBadReadPtr(ptr,1);
}

//////////////////////////////////////////////////////////////////////////////
// Files
//////////////////////////////////////////////////////////////////////////////

FileHandle OpenExisting(const char *fileName)
{
  if (!fileName||!*fileName)
    return INVALID_FILE_HANDLE;
  return open(fileName,O_RDONLY|O_CLOEXEC);
}

FileHandle CreateAlways(const char *fileName)
{
  if (!fileName||!*fileName)
    return INVALID_FILE_HANDLE;
  return open(fileName,O_WRONLY|O_CREAT|O_TRUNC|O_CLOEXEC,0644);
}

FileHandle OpenAppend(const char *fileName)
{
  if (!fileName||!*fileName)
    return INVALID_FILE_HANDLE;
  return open(fileName,O_WRONLY|O_CREAT|O_APPEND|O_CLOEXEC,0644);
}

unsigned ReadFile(FileHandle handle, void *buf, unsigned numBytes)
{
  if (handle==INVALID_FILE_HANDLE||!buf)
    return 0;

  char *cur=(char *)buf;
  unsigned done=0;
  while (done<numBytes)
  {
    ssize_t got=read(handle,cur+done,numBytes-done);
    if (got<0)
    {
      if (errno==EINTR)
        continue;
      break;
    }
    if (!got)
      break;
    done+=(unsigned)got;
  }
  return done;
}

unsigned WriteFile(FileHandle handle, const void *buf, unsigned numBytes)
{
  if (handle==INVALID_FILE_HANDLE||!buf)
    return 0;

  const char *cur=(const char *)buf;
  unsigned done=0;
  while (done<numBytes)
  {
    ssize_t put=write(handle,cur+done,numBytes-done);
    if (put<0)
    {
      if (errno==EINTR)
        continue;
      break;
    }
    done+=(unsigned)put;
  }
  return done;
}

void CloseFile(FileHandle handle)
{
  if (handle!=INVALID_FILE_HANDLE)
    close(handle);
}

CopyResult CopyFileNoOverwrite(const char *source, const char *destination)
{
  if (!source||!destination)
    return COPY_FAILED;

  int in=open(source,O_RDONLY|O_CLOEXEC);
  if (in<0)
    return COPY_FAILED;

  int out=open(destination,O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC,0644);
  if (out<0)
  {
    CopyResult result=errno==EEXIST?COPY_EXISTS:COPY_FAILED;
    close(in);
    return result;
  }

  CopyResult result=COPY_OK;
  char buffer[16384];
  for (;;)
  {
    ssize_t got=read(in,buffer,sizeof(buffer));
    if (got<0)
    {
      if (errno==EINTR)
        continue;
      result=COPY_FAILED;
      break;
    }
    if (!got)
      break;
    if (WriteFile(out,buffer,(unsigned)got)!=(unsigned)got)
    {
      result=COPY_FAILED;
      break;
    }
  }

  close(in);
  close(out);
  return result;
}

//////////////////////////////////////////////////////////////////////////////
// Console
//////////////////////////////////////////////////////////////////////////////

bool HasConsole()
{
  return isatty(STDOUT_FILENO)!=0;
}

void ConsoleWrite(const char *text)
{
  if (!text||!*text)
    return;
  ssize_t ignored=write(STDOUT_FILENO,text,strlen(text));
  (void)ignored;
}

//////////////////////////////////////////////////////////////////////////////
// Stack walking
//////////////////////////////////////////////////////////////////////////////

unsigned CaptureStack(void **frames, unsigned maxFrames, unsigned skip)
{
  if (!frames||!maxFrames)
    return 0;

  // Capture into a local buffer so this function's own frame, plus whatever the caller asked to
  // skip, can be dropped without the caller having to know backtrace()'s conventions.
  enum { MAX_CAPTURE = 256 };
  void *raw[MAX_CAPTURE];

  unsigned want=skip+1+maxFrames;
  if (want>MAX_CAPTURE)
    want=MAX_CAPTURE;

  int got=backtrace(raw,(int)want);
  if (got<=0)
    return 0;

  unsigned drop=skip+1;
  if ((unsigned)got<=drop)
    return 0;

  unsigned count=(unsigned)got-drop;
  if (count>maxFrames)
    count=maxFrames;
  memcpy(frames,raw+drop,count*sizeof(*frames));
  return count;
}

bool ResolveAddress(unsigned long long addr,
                    char *bufMod, unsigned sizeMod, unsigned long long *relMod,
                    char *bufSym, unsigned sizeSym, unsigned long long *relSym)
{
  if (bufMod&&sizeMod) *bufMod=0;
  if (bufSym&&sizeSym) *bufSym=0;
  if (relMod) *relMod=0;
  if (relSym) *relSym=0;

  Dl_info info;
  memset(&info,0,sizeof(info));
  if (!dladdr((const void *)(unsigned long)addr,&info)||!info.dli_fbase)
    return false;

  if (bufMod&&sizeMod)
  {
    const char *name=info.dli_fname?info.dli_fname:"";
    const char *slash=strrchr(name,'/');
    CopyToBuffer(bufMod,sizeMod,slash?slash+1:name);
  }
  if (relMod)
    *relMod=addr-(unsigned long long)(unsigned long)info.dli_fbase;

  if (info.dli_sname&&*info.dli_sname)
  {
    if (bufSym&&sizeSym)
    {
      int status=0;
      char *demangled=abi::__cxa_demangle(info.dli_sname,nullptr,nullptr,&status);
      CopyToBuffer(bufSym,sizeSym,status==0&&demangled?demangled:info.dli_sname);
      if (demangled)
        free(demangled);
    }
    if (relSym&&info.dli_saddr)
      *relSym=addr-(unsigned long long)(unsigned long)info.dli_saddr;
  }
  else if (bufSym&&sizeSym)
    CopyToBuffer(bufSym,sizeSym,"(unknown)");

  return true;
}

//////////////////////////////////////////////////////////////////////////////
// Crash handling
//////////////////////////////////////////////////////////////////////////////

static void (*g_fatalHandler)(int signalNumber, const char *signalName);

static const char *FatalSignalName(int signalNumber)
{
  switch(signalNumber)
  {
    case SIGSEGV: return "SIGSEGV";
    case SIGBUS:  return "SIGBUS";
    case SIGFPE:  return "SIGFPE";
    case SIGILL:  return "SIGILL";
    case SIGABRT: return "SIGABRT";
    case SIGTRAP: return "SIGTRAP";
    default:      return "signal";
  }
}

static void FatalSignalTrampoline(int signalNumber)
{
  if (g_fatalHandler)
    g_fatalHandler(signalNumber,FatalSignalName(signalNumber));
  ReRaiseFatalSignal(signalNumber);
}

void InstallFatalSignalHandlers(void (*handler)(int signalNumber, const char *signalName))
{
  g_fatalHandler=handler;

  // A stack overflow faults with an unusable stack, so the handler needs its own. Without this
  // the one crash that most needs a backtrace is the one that cannot produce it.
  // 64 KiB, not SIGSTKSZ: glibc 2.34 made SIGSTKSZ a sysconf() call rather than a constant.
  static char altStack[65536];
  stack_t alt;
  memset(&alt,0,sizeof(alt));
  alt.ss_sp=altStack;
  alt.ss_size=sizeof(altStack);
  sigaltstack(&alt,nullptr);

  struct sigaction action;
  memset(&action,0,sizeof(action));
  action.sa_handler=FatalSignalTrampoline;
  action.sa_flags=SA_ONSTACK;
  sigemptyset(&action.sa_mask);

  static const int fatalSignals[]={ SIGSEGV, SIGBUS, SIGFPE, SIGILL, SIGABRT };
  for (unsigned k=0;k<sizeof(fatalSignals)/sizeof(*fatalSignals);++k)
    sigaction(fatalSignals[k],&action,nullptr);
}

void ReRaiseFatalSignal(int signalNumber)
{
  struct sigaction action;
  memset(&action,0,sizeof(action));
  action.sa_handler=SIG_DFL;
  sigemptyset(&action.sa_mask);
  sigaction(signalNumber,&action,nullptr);
  raise(signalNumber);
}

}	// namespace DebugPlatform

#endif // !_WIN32
