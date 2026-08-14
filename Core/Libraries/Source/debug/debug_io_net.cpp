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

/////////////////////////////////////////////////////////////////////////EA-V1
// $File: //depot/GeneralsMD/Staging/code/Libraries/Source/debug/debug_io_net.cpp $
// $Author: mhoffe $
// $Revision: #1 $
// $DateTime: 2003/07/03 11:55:26 $
//
// (c) 2003 Electronic Arts
//
// Debug I/O class net (Network destination via named pipe)
//////////////////////////////////////////////////////////////////////////////

#include "debug.h"
#include "internal.h"
#include "internal_io.h"
#ifdef _WIN32
#include <windows.h>
#endif
#include <new>      // needed for placement new prototype

/*
  This backend talks to the Win32-only 'netserv' debug server over a named pipe. Off Windows it is
  a deliberate stub: named pipes have no portable equivalent, the peer does not exist on macOS or
  Linux, and building a socket protocol for it is not on the path to running the game. Every entry
  point behaves as if no connection was ever made, and 'net add' says why. See
  docs/porting/debug-and-profile-libs.md.
*/

DebugIONet::DebugIONet()
{
#ifndef _WIN32
  m_pipe=DebugPlatform::INVALID_FILE_HANDLE;
#endif
}

DebugIONet::~DebugIONet()
{
#ifdef _WIN32
  if (m_pipe!=INVALID_HANDLE_VALUE)
    CloseHandle(m_pipe);
#endif
}

int DebugIONet::Read(char *buf, int maxchar)
{
#ifndef _WIN32
  (void)buf;
  (void)maxchar;
  return 0;
#else
  if (m_pipe==INVALID_HANDLE_VALUE)
    return 0;

  DWORD mode=PIPE_READMODE_MESSAGE|PIPE_NOWAIT;
  SetNamedPipeHandleState(m_pipe,&mode,nullptr,nullptr);

  DWORD read;
  if (!ReadFile(m_pipe,buf,maxchar-1,&read,nullptr))
    read=0;
  mode=PIPE_READMODE_MESSAGE|PIPE_WAIT;
  SetNamedPipeHandleState(m_pipe,&mode,nullptr,nullptr);

  return read;
#endif
}

void DebugIONet::Write(StringType type, const char *src, const char *str)
{
#ifndef _WIN32
  (void)type;
  (void)src;
  (void)str;
  return;
#else
  if (m_pipe==INVALID_HANDLE_VALUE)
    return;

  DWORD dummy;
  WriteFile(m_pipe,&type,1,&dummy,nullptr);

  unsigned len;
  len=src?strlen(src):0;
  WriteFile(m_pipe,&len,4,&dummy,nullptr);
  if (len)
    WriteFile(m_pipe,src,len,&dummy,nullptr);

  len=strlen(str);
  WriteFile(m_pipe,&len,4,&dummy,nullptr);
  if (len)
    WriteFile(m_pipe,str,len,&dummy,nullptr);
#endif
}

void DebugIONet::EmergencyFlush()
{
}

void DebugIONet::Execute(class Debug& dbg, const char *cmd, bool structuredCmd,
                         unsigned argn, const char * const * argv)
{
  if (!cmd||strcmp(cmd,"help") == 0)
  {
    dbg << "net I/O help:\n"
           "  add [ <machine> ]\n"
           "    create net I/O (optionally specifying the machine to connect to)\n";
  }
  else if (strcmp(cmd,"add") == 0)
  {
#ifndef _WIN32
    (void)argn;
    (void)argv;
    dbg << "net I/O is not available on this platform: it needs the Win32 'netserv' server and a "
           "named pipe, neither of which exists here. Use the 'flat' or 'con' I/O instead.\n";
#else
    const char *machine=argn?argv[0]:".";

    char buf[256];
    wsprintf(buf,"\\\\%s\\pipe\\ea_debug_v1",machine);
    m_pipe=CreateFile(buf,GENERIC_READ|GENERIC_WRITE,
                      0,nullptr,OPEN_EXISTING,0,nullptr);
    if (m_pipe==INVALID_HANDLE_VALUE)
    {
      dbg << "Could not connect to given machine.\n";
      return;
    }

    // we're reading messages
    DWORD mode=PIPE_READMODE_MESSAGE;
    SetNamedPipeHandleState(m_pipe,&mode,nullptr,nullptr);

    // write welcome message
    char comp[128];
    mode=sizeof(comp);
    GetComputerName(comp,&mode);
    wsprintf(buf,"Client at %s\n",comp);
    Write(Other,nullptr,buf);
#endif
  }
}

DebugIOInterface *DebugIONet::Create()
{
  return new (DebugAllocMemory(sizeof(DebugIONet))) DebugIONet();
}

void DebugIONet::Delete()
{
  this->~DebugIONet();
  DebugFreeMemory(this);
}
