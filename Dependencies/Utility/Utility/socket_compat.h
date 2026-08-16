/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2025 TheSuperHackers
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

// TheSuperHackers @port BSD sockets behind the Winsock spellings the engine already uses.
// See docs/porting/sockets-and-text-encoding.md.
//
// On Windows this is <winsock.h> and nothing else, so the Windows build is unchanged. Off
// Windows it supplies the BSD headers plus the handful of Winsock names that have no BSD
// spelling. The engine's socket code keeps saying SOCKET_ERROR, closesocket and
// WSAGetLastError; only the include line changes.
//
// Deliberately *not* included from Utility/compat.h: compat.h is on the include path of every
// translation unit through BaseTypeCore.h, and <sys/socket.h> and friends have no business
// being there. Include this header directly, in the few files that talk to sockets.
#pragma once

#ifdef _WIN32

#include <winsock.h>

// Winsock 1 spells the address-length parameter of accept/getsockname/recvfrom/getsockopt as
// int*; BSD spells it socklen_t*, and on LP64 the two are not the same type. Declaring the
// length variables socklen_t at the call sites makes them portable, and this typedef keeps
// that spelling meaning exactly int on Windows. (ws2tcpip.h defines the same typedef, and a
// repeated identical typedef is legal.)
typedef int socklen_t;

#else

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

// Winsock's socket handle is an opaque unsigned type; a BSD socket is a file descriptor, and
// -1 is the failure value for both socket() and every operation on one.
typedef int SOCKET;

#ifndef INVALID_SOCKET
#define INVALID_SOCKET (-1)
#endif
#ifndef SOCKET_ERROR
#define SOCKET_ERROR (-1)
#endif
#ifndef NO_ERROR
#define NO_ERROR 0
#endif

typedef struct hostent HOSTENT;
typedef struct hostent *LPHOSTENT;
typedef struct sockaddr SOCKADDR;
typedef struct sockaddr *LPSOCKADDR;
typedef struct sockaddr_in SOCKADDR_IN;
typedef struct sockaddr_in *LPSOCKADDR_IN;

// Winsock reports errors through its own thread-local slot; BSD sockets use errno. The engine
// only ever reads the value straight after a failed call, so the two are interchangeable here.
#ifndef WSAEWOULDBLOCK
#define WSAEWOULDBLOCK EWOULDBLOCK
#endif

// The errors a non-blocking connect() reports. Winsock and BSD sockets agree on what each of
// these means for connect(); only the spelling differs.
#ifndef WSAEINVAL
#define WSAEINVAL EINVAL
#endif
#ifndef WSAEALREADY
#define WSAEALREADY EALREADY
#endif
#ifndef WSAEISCONN
#define WSAEISCONN EISCONN
#endif

// The errors a peer-closed connection reports to recv()/send(), which the FTP transfer loop
// treats as end-of-transfer rather than as failure. Winsock and BSD sockets agree on both.
#ifndef WSAECONNRESET
#define WSAECONNRESET ECONNRESET
#endif
#ifndef WSAENOTCONN
#define WSAENOTCONN ENOTCONN
#endif

// The rest of the Winsock error names, needed by the debug-only getWSAErrorString() switches
// that turn an error number back into its name for the log. They are all in one `switch`, so
// every name has to be a distinct constant or the translation unit will not compile -- which is
// why the ones that do have a BSD counterpart are that counterpart (distinct by definition) and
// the ones that do not keep their real Win32 numbers rather than being folded onto an errno
// value that already appears above. The Win32-numbered ones can therefore never match a value
// this platform actually reports; the name is still printed if a Windows-produced number is
// logged, and nothing else in the engine compares against them.
#ifndef WSABASEERR
#define WSABASEERR 10000
#endif
#ifndef WSAEINTR
#define WSAEINTR EINTR
#endif
#ifndef WSAEBADF
#define WSAEBADF EBADF
#endif
#ifndef WSAEACCES
#define WSAEACCES EACCES
#endif
#ifndef WSAEFAULT
#define WSAEFAULT EFAULT
#endif
#ifndef WSAEMFILE
#define WSAEMFILE EMFILE
#endif
#ifndef WSAEINPROGRESS
#define WSAEINPROGRESS EINPROGRESS
#endif
#ifndef WSAENOTSOCK
#define WSAENOTSOCK ENOTSOCK
#endif
#ifndef WSAEDESTADDRREQ
#define WSAEDESTADDRREQ EDESTADDRREQ
#endif
#ifndef WSAEMSGSIZE
#define WSAEMSGSIZE EMSGSIZE
#endif
#ifndef WSAEPROTOTYPE
#define WSAEPROTOTYPE EPROTOTYPE
#endif
#ifndef WSAENOPROTOOPT
#define WSAENOPROTOOPT ENOPROTOOPT
#endif
#ifndef WSAEPROTONOSUPPORT
#define WSAEPROTONOSUPPORT EPROTONOSUPPORT
#endif
#ifndef WSAESOCKTNOSUPPORT
#define WSAESOCKTNOSUPPORT ESOCKTNOSUPPORT
#endif
#ifndef WSAEOPNOTSUPP
#define WSAEOPNOTSUPP EOPNOTSUPP
#endif
#ifndef WSAEPFNOSUPPORT
#define WSAEPFNOSUPPORT EPFNOSUPPORT
#endif
#ifndef WSAEAFNOSUPPORT
#define WSAEAFNOSUPPORT EAFNOSUPPORT
#endif
#ifndef WSAEADDRINUSE
#define WSAEADDRINUSE EADDRINUSE
#endif
#ifndef WSAEADDRNOTAVAIL
#define WSAEADDRNOTAVAIL EADDRNOTAVAIL
#endif
#ifndef WSAENETDOWN
#define WSAENETDOWN ENETDOWN
#endif
#ifndef WSAENETUNREACH
#define WSAENETUNREACH ENETUNREACH
#endif
#ifndef WSAENETRESET
#define WSAENETRESET ENETRESET
#endif
#ifndef WSAECONNABORTED
#define WSAECONNABORTED ECONNABORTED
#endif
#ifndef WSAENOBUFS
#define WSAENOBUFS ENOBUFS
#endif
#ifndef WSAESHUTDOWN
#define WSAESHUTDOWN ESHUTDOWN
#endif
#ifndef WSAETOOMANYREFS
#define WSAETOOMANYREFS ETOOMANYREFS
#endif
#ifndef WSAETIMEDOUT
#define WSAETIMEDOUT ETIMEDOUT
#endif
#ifndef WSAECONNREFUSED
#define WSAECONNREFUSED ECONNREFUSED
#endif
#ifndef WSAELOOP
#define WSAELOOP ELOOP
#endif
#ifndef WSAENAMETOOLONG
#define WSAENAMETOOLONG ENAMETOOLONG
#endif
#ifndef WSAEHOSTDOWN
#define WSAEHOSTDOWN EHOSTDOWN
#endif
#ifndef WSAEHOSTUNREACH
#define WSAEHOSTUNREACH EHOSTUNREACH
#endif
#ifndef WSAENOTEMPTY
#define WSAENOTEMPTY ENOTEMPTY
#endif
#ifndef WSAEUSERS
#define WSAEUSERS EUSERS
#endif
#ifndef WSAEDQUOT
#define WSAEDQUOT EDQUOT
#endif
#ifndef WSAESTALE
#define WSAESTALE ESTALE
#endif
#ifndef WSAEREMOTE
#define WSAEREMOTE EREMOTE
#endif
// No BSD counterpart: these describe the state of the Winsock library itself, or come out of
// Winsock's own name resolver rather than out of errno.
#ifndef WSAEPROCLIM
#define WSAEPROCLIM 10067
#endif
#ifndef WSAEDISCON
#define WSAEDISCON 10101
#endif
#ifndef WSASYSNOTREADY
#define WSASYSNOTREADY 10091
#endif
#ifndef WSAVERNOTSUPPORTED
#define WSAVERNOTSUPPORTED 10092
#endif
#ifndef WSANOTINITIALISED
#define WSANOTINITIALISED 10093
#endif
#ifndef WSAHOST_NOT_FOUND
#define WSAHOST_NOT_FOUND 11001
#endif
#ifndef WSATRY_AGAIN
#define WSATRY_AGAIN 11002
#endif
#ifndef WSANO_RECOVERY
#define WSANO_RECOVERY 11003
#endif
#ifndef WSANO_DATA
#define WSANO_DATA 11004
#endif

// Word building, normally from <windef.h>. Only used to spell the Winsock version number.
#ifndef MAKEWORD
#define MAKEWORD(low, high) \
	((unsigned short)((((unsigned short)(high)) << 8) | ((unsigned char)(low))))
#endif
#ifndef LOBYTE
#define LOBYTE(w) ((unsigned char)((w) & 0xff))
#endif
#ifndef HIBYTE
#define HIBYTE(w) ((unsigned char)((((unsigned short)(w)) >> 8) & 0xff))
#endif

struct WSADATA
{
	unsigned short wVersion;
	unsigned short wHighVersion;
	char szDescription[257];
	char szSystemStatus[129];
	unsigned short iMaxSockets;
	unsigned short iMaxUdpDg;
	char *lpVendorInfo;
};

typedef WSADATA *LPWSADATA;

// There is no library to start up. WSAStartup reports back the version that was asked for so
// that the callers' (LOBYTE(wVersion) != 2) checks behave as they do on Windows.
inline int WSAStartup(unsigned short versionRequested, WSADATA *data)
{
	if (data != nullptr)
	{
		data->wVersion = versionRequested;
		data->wHighVersion = versionRequested;
		data->szDescription[0] = '\0';
		data->szSystemStatus[0] = '\0';
		data->iMaxSockets = 0;
		data->iMaxUdpDg = 0;
		data->lpVendorInfo = nullptr;
	}
	return 0;
}

inline int WSACleanup()
{
	return 0;
}

inline int WSAGetLastError()
{
	return errno;
}

inline void WSASetLastError(int error)
{
	errno = error;
}

inline int closesocket(SOCKET socket)
{
	return ::close(socket);
}

// FIONBIO takes an unsigned long on Winsock and an int on BSD, so the argument is narrowed and
// widened rather than reinterpreted; on a big-endian LP64 host the reinterpretation would read
// the wrong half of the word. Only the ioctls the engine uses (FIONBIO, FIONREAD) are covered,
// which is all of them: both are int-valued.
inline int ioctlsocket(SOCKET socket, long command, unsigned long *argument)
{
	int value = (argument != nullptr) ? (int)*argument : 0;
	const int result = ::ioctl(socket, (unsigned long)command, &value);
	if (argument != nullptr)
	{
		*argument = (unsigned long)value;
	}
	return result;
}

#endif // _WIN32
