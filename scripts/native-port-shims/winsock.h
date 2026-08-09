// Declaration-only stand-in for scripts/native-port-probe.py. See README.md.
#pragma once

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <windows.h>

typedef int SOCKET;
#define INVALID_SOCKET (-1)
#define SOCKET_ERROR   (-1)
#define closesocket    close
#define WSAEWOULDBLOCK EWOULDBLOCK
typedef struct { WORD wVersion, wHighVersion; char szDescription[257], szSystemStatus[129];
                 unsigned short iMaxSockets, iMaxUdpDg; char* lpVendorInfo; } WSADATA, *LPWSADATA;
extern "C" {
int WSAStartup(WORD, LPWSADATA);
int WSACleanup();
int WSAGetLastError();
}
