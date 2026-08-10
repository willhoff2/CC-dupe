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

////////////////////////////////////////////////////////////////////////////////
//																																						//
//  (c) 2001-2003 Electronic Arts Inc.																				//
//																																						//
////////////////////////////////////////////////////////////////////////////////

#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine

// TheSuperHackers @port Win32 header pushed down from PreRTS.h; see docs/porting/prerts-win32-surgery.md
#ifdef _WIN32
#include <windows.h>
#endif

// TheSuperHackers @port BSD sockets behind the Winsock spellings; see
// docs/porting/sockets-and-text-encoding.md
#include <Utility/socket_compat.h>

#ifndef _WIN32
#include <ifaddrs.h>
#include <net/if.h>
#endif

#include "GameNetwork/IPEnumeration.h"
#include "GameNetwork/networkutil.h"
#include "GameClient/ClientInstance.h"

IPEnumeration::IPEnumeration()
{
	m_IPlist = nullptr;
	m_isWinsockInitialized = false;
}

IPEnumeration::~IPEnumeration()
{
	if (m_isWinsockInitialized)
	{
		WSACleanup();
		m_isWinsockInitialized = false;
	}

	EnumeratedIP *ip = m_IPlist;
	while (ip)
	{
		ip = ip->getNext();
		deleteInstance(m_IPlist);
		m_IPlist = ip;
	}
}

#ifdef _WIN32

EnumeratedIP * IPEnumeration::getAddresses()
{
	if (m_IPlist)
		return m_IPlist;

	if (!m_isWinsockInitialized)
	{
		WORD verReq = MAKEWORD(2, 2);
		WSADATA wsadata;

		int err = WSAStartup(verReq, &wsadata);
		if (err != 0) {
			return nullptr;
		}

		if ((LOBYTE(wsadata.wVersion) != 2) || (HIBYTE(wsadata.wVersion) !=2)) {
			WSACleanup();
			return nullptr;
		}
		m_isWinsockInitialized = true;
	}

	// get the local machine's host name
	char hostname[256];
	if (gethostname(hostname, sizeof(hostname)))
	{
		DEBUG_LOG(("Failed call to gethostname; WSAGetLastError returned %d", WSAGetLastError()));
		return nullptr;
	}
	DEBUG_LOG(("Hostname is '%s'", hostname));

	// get host information from the host name
	HOSTENT* hostEnt = gethostbyname(hostname);
	if (hostEnt == nullptr)
	{
		DEBUG_LOG(("Failed call to gethostbyname; WSAGetLastError returned %d", WSAGetLastError()));
		return nullptr;
	}

	// sanity-check the length of the IP adress
	if (hostEnt->h_length != 4)
	{
		DEBUG_LOG(("gethostbyname returns oddly-sized IP addresses!"));
		return nullptr;
	}

	// TheSuperHackers @feature Add one unique local host IP address for each multi client instance.
	if (rts::ClientInstance::isMultiInstance())
	{
		const UnsignedInt id = rts::ClientInstance::getInstanceId();
		addNewIP(
			127,
			(UnsignedByte)(id >> 16),
			(UnsignedByte)(id >> 8),
			(UnsignedByte)(id));
	}

	// construct a list of addresses
	int numAddresses = 0;
	char *entry;
	while ( (entry = hostEnt->h_addr_list[numAddresses++]) != nullptr )
	{
		addNewIP(
			(UnsignedByte)entry[0],
			(UnsignedByte)entry[1],
			(UnsignedByte)entry[2],
			(UnsignedByte)entry[3]);
	}

	return m_IPlist;
}

#else // !_WIN32

// TheSuperHackers @port getifaddrs() is the portable analogue of the Windows adapter
// enumeration above, but it answers a different question, so the two lists are not identical:
//
// - Windows asks the resolver for the addresses registered against the machine's host name.
//   getifaddrs() asks the kernel for the addresses configured on the interfaces. A machine
//   whose host name resolves to fewer (or more) addresses than it has configured gets a
//   different list.
// - getifaddrs() reports loopback and down interfaces too. Both are filtered out here, because
//   the Windows list contains neither, and because the 127.x address the multi-instance feature
//   below invents must stay the only loopback entry in the list.
// - getifaddrs() reports AF_INET6 and link-layer entries. The game's EnumeratedIP is a 32-bit
//   IPv4 address and the LAN protocol puts one on the wire, so only AF_INET is taken.
//
// The ordering of the resulting list is not affected: addNewIP() inserts in ascending numeric
// order regardless of the order the addresses are discovered in.
EnumeratedIP * IPEnumeration::getAddresses()
{
	if (m_IPlist)
		return m_IPlist;

	m_isWinsockInitialized = true;

	// get the local machine's host name, for the log line the Windows path also writes
	char hostname[256];
	if (gethostname(hostname, sizeof(hostname)))
	{
		DEBUG_LOG(("Failed call to gethostname; errno is %d", WSAGetLastError()));
		return nullptr;
	}
	DEBUG_LOG(("Hostname is '%s'", hostname));

	// TheSuperHackers @feature Add one unique local host IP address for each multi client instance.
	if (rts::ClientInstance::isMultiInstance())
	{
		const UnsignedInt id = rts::ClientInstance::getInstanceId();
		addNewIP(
			127,
			(UnsignedByte)(id >> 16),
			(UnsignedByte)(id >> 8),
			(UnsignedByte)(id));
	}

	struct ifaddrs *interfaces = nullptr;
	if (getifaddrs(&interfaces) != 0)
	{
		DEBUG_LOG(("Failed call to getifaddrs; errno is %d", WSAGetLastError()));
		return m_IPlist;
	}

	for (const struct ifaddrs *ifa = interfaces; ifa != nullptr; ifa = ifa->ifa_next)
	{
		if (ifa->ifa_addr == nullptr || ifa->ifa_addr->sa_family != AF_INET)
			continue;

		if ((ifa->ifa_flags & IFF_UP) == 0 || (ifa->ifa_flags & IFF_LOOPBACK) != 0)
			continue;

		// s_addr is in network byte order, which is the order the Windows path's h_addr bytes
		// arrive in, so the four bytes are taken most significant first in both.
		const UnsignedInt address = ntohl(((const struct sockaddr_in *)ifa->ifa_addr)->sin_addr.s_addr);
		addNewIP(
			(UnsignedByte)(address >> 24),
			(UnsignedByte)(address >> 16),
			(UnsignedByte)(address >> 8),
			(UnsignedByte)(address));
	}

	freeifaddrs(interfaces);

	return m_IPlist;
}

#endif // _WIN32

void IPEnumeration::addNewIP( UnsignedByte a, UnsignedByte b, UnsignedByte c, UnsignedByte d )
{
	EnumeratedIP *newIP = newInstance(EnumeratedIP);

	AsciiString str;
	str.format("%d.%d.%d.%d", (int)a, (int)b, (int)c, (int)d);

	UnsignedInt ip = AssembleIp(a, b, c, d);

	newIP->setIPstring(str);
	newIP->setIP(ip);

	DEBUG_LOG(("IP: 0x%8.8X (%s)", ip, str.str()));

	// Add the IP to the list in ascending order
	if (!m_IPlist)
	{
		m_IPlist = newIP;
		newIP->setNext(nullptr);
	}
	else
	{
		if (newIP->getIP() < m_IPlist->getIP())
		{
			newIP->setNext(m_IPlist);
			m_IPlist = newIP;
		}
		else
		{
			EnumeratedIP *p = m_IPlist;
			while (p->getNext() && p->getNext()->getIP() < newIP->getIP())
			{
				p = p->getNext();
			}
			newIP->setNext(p->getNext());
			p->setNext(newIP);
		}
	}
}

AsciiString IPEnumeration::getMachineName()
{
	if (!m_isWinsockInitialized)
	{
		WORD verReq = MAKEWORD(2, 2);
		WSADATA wsadata;

		int err = WSAStartup(verReq, &wsadata);
		if (err != 0) {
			return "";
		}

		if ((LOBYTE(wsadata.wVersion) != 2) || (HIBYTE(wsadata.wVersion) !=2)) {
			WSACleanup();
			return "";
		}
		m_isWinsockInitialized = true;
	}

	// get the local machine's host name
	char hostname[256];
	if (gethostname(hostname, sizeof(hostname)))
	{
		DEBUG_LOG(("Failed call to gethostname; WSAGetLastError returned %d", WSAGetLastError()));
		return "";
	}

	return AsciiString(hostname);
}


