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

/***********************************************************************************************
 ***              C O N F I D E N T I A L  ---  W E S T W O O D  S T U D I O S               ***
 ***********************************************************************************************
 *                                                                                             *
 *                 Project Name : WWAudio                                                      *
 *                                                                                             *
 *                     $Archive:: /Commando/Code/WWAudio/Threads.h                                                                                                                                                                                                                                                                                                                               $Modtime:: 7/17/99 3:32p                                               $*
 *                                                                                             *
 *                    $Revision:: 6                                                           $*
 *                                                                                             *
 *---------------------------------------------------------------------------------------------*
 * Functions:                                                                                  *
 * - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

#pragma once

#ifdef _WIN32
#include "windows.h"
#else
#include "WWLib/platform/platform_thread.h"
#endif
#include "WWLib/Vector.h"
#include "WWLib/mutex.h"

// Forward declarations
class RefCountClass;


//////////////////////////////////////////////////////////////////////////
//
//	WWAudioThreadsClass
//
//	Simple class that provides a common namespace for tying thread
// information together.
//
//////////////////////////////////////////////////////////////////////////
class WWAudioThreadsClass
{
	public:

		//////////////////////////////////////////////////////////////////////
		//	Public constructors/destructors
		//////////////////////////////////////////////////////////////////////
		WWAudioThreadsClass ();
		~WWAudioThreadsClass ();

		//////////////////////////////////////////////////////////////////////
		//	Public data types
		//////////////////////////////////////////////////////////////////////

		//
		//	How a running release thread is identified. On Windows this is the HANDLE
		//	_beginthread() returns; elsewhere it is the token WWPlatform::Thread_Create()
		//	returns. `TimeType` is DWORD spelled portably - the same type on Windows.
		//
#ifdef _WIN32
		typedef HANDLE			ThreadTokenType;
#else
		typedef unsigned long	ThreadTokenType;
#endif
		typedef unsigned long	TimeType;

		//////////////////////////////////////////////////////////////////////
		//	Public methods
		//////////////////////////////////////////////////////////////////////

		//
		//	Delayed release mechanism
		//
		static ThreadTokenType	Create_Delayed_Release_Thread (void *param = nullptr);
		static void			End_Delayed_Release_Thread (TimeType timeout = 20000);
		static void			Add_Delayed_Release_Object (RefCountClass *object, TimeType delay = 2000);
		static void			Flush_Delayed_Release_Objects ();

	private:

		//////////////////////////////////////////////////////////////////////
		//	Private methods
		//////////////////////////////////////////////////////////////////////
		static void	__cdecl Delayed_Release_Thread_Proc (void *param);

		//////////////////////////////////////////////////////////////////////
		//	Private data types
		//////////////////////////////////////////////////////////////////////
		typedef struct _DELAYED_RELEASE_INFO
		{
			RefCountClass *	object;
			TimeType				time;

			_DELAYED_RELEASE_INFO *next;

		} DELAYED_RELEASE_INFO;

		//typedef DynamicVectorClass<DELAYED_RELEASE_INFO *>	RELEASE_LIST;

		//////////////////////////////////////////////////////////////////////
		//	Private member data
		//////////////////////////////////////////////////////////////////////
		static ThreadTokenType			m_hDelayedReleaseThread;
#ifdef _WIN32
		static HANDLE						m_hDelayedReleaseEvent;
#else
		//
		//	There is no portable equivalent of waiting on a thread handle, so the thread
		//	signals m_DelayedReleaseExitEvent just before it returns and the shutdown path
		//	waits on that instead - the same bounded wait, one indirection further out.
		//
		static WWPlatform::EventClass *	m_hDelayedReleaseEvent;
		static WWPlatform::EventClass *	m_DelayedReleaseExitEvent;
#endif
		//static RELEASE_LIST		m_ReleaseList;
		static CriticalSectionClass	m_CriticalSection;
		static DELAYED_RELEASE_INFO *	m_ReleaseListHead;
		static CriticalSectionClass	m_ListMutex;
		static bool							m_IsShuttingDown;
};
