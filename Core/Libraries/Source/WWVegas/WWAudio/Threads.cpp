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
 *                     $Archive:: /Commando/Code/WWAudio/Threads.cpp                                                                                                                                                                                                                                                                                                                               $Modtime:: 7/17/99 3:32p                                               $*
 *                                                                                             *
 *                    $Revision:: 5                                                           $*
 *                                                                                             *
 *---------------------------------------------------------------------------------------------*
 * Functions:                                                                                  *
 * - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

#include "WWLib/always.h"
#include "Threads.h"
#include "Utils.h"
#ifdef _WIN32
#include <process.h>
#endif


///////////////////////////////////////////////////////////////////////////////////////////
//	Static member initialization
///////////////////////////////////////////////////////////////////////////////////////////
WWAudioThreadsClass::DELAYED_RELEASE_INFO *	WWAudioThreadsClass::m_ReleaseListHead	= nullptr;
CriticalSectionClass		WWAudioThreadsClass::m_ListMutex;
WWAudioThreadsClass::ThreadTokenType	WWAudioThreadsClass::m_hDelayedReleaseThread	= (WWAudioThreadsClass::ThreadTokenType)-1;
#ifdef _WIN32
HANDLE						WWAudioThreadsClass::m_hDelayedReleaseEvent	= (HANDLE)-1;
#else
WWPlatform::EventClass *	WWAudioThreadsClass::m_hDelayedReleaseEvent	= nullptr;
WWPlatform::EventClass *	WWAudioThreadsClass::m_DelayedReleaseExitEvent	= nullptr;
#endif
CriticalSectionClass		WWAudioThreadsClass::m_CriticalSection;
bool							WWAudioThreadsClass::m_IsShuttingDown			= false;

///////////////////////////////////////////////////////////////////////////////////////////
//
//	WWAudioThreadsClass
//
///////////////////////////////////////////////////////////////////////////////////////////
WWAudioThreadsClass::WWAudioThreadsClass ()
{
}


///////////////////////////////////////////////////////////////////////////////////////////
//
//	~WWAudioThreadsClass
//
///////////////////////////////////////////////////////////////////////////////////////////
WWAudioThreadsClass::~WWAudioThreadsClass ()
{
}

///////////////////////////////////////////////////////////////////////////////////////////
//
//	Create_Delayed_Release_Thread
//
///////////////////////////////////////////////////////////////////////////////////////////
WWAudioThreadsClass::ThreadTokenType
WWAudioThreadsClass::Create_Delayed_Release_Thread (void *param)
{
	//
	//	If the thread isn't already running, then
	//
	if (m_hDelayedReleaseThread == (ThreadTokenType)-1) {
#ifdef _WIN32
		m_hDelayedReleaseEvent	= ::CreateEvent (nullptr, FALSE, FALSE, nullptr);
		m_hDelayedReleaseThread = (HANDLE)::_beginthread (Delayed_Release_Thread_Proc, 0, param);
#else
		m_hDelayedReleaseEvent		= W3DNEW WWPlatform::EventClass;
		m_DelayedReleaseExitEvent	= W3DNEW WWPlatform::EventClass;
		m_hDelayedReleaseThread		= (ThreadTokenType)WWPlatform::Thread_Create (Delayed_Release_Thread_Proc, param);
#endif
	}

	return m_hDelayedReleaseThread;
}


///////////////////////////////////////////////////////////////////////////////////////////
//
//	End_Delayed_Release_Thread
//
///////////////////////////////////////////////////////////////////////////////////////////
void
WWAudioThreadsClass::End_Delayed_Release_Thread (TimeType timeout)
{
	m_IsShuttingDown = true;

	//
	//	If the thread is running, then wait for it to finish
	//
	if (m_hDelayedReleaseThread != (ThreadTokenType)-1) {
#ifdef _WIN32
		::SetEvent (m_hDelayedReleaseEvent);
		::WaitForSingleObject (m_hDelayedReleaseThread, timeout);

		m_hDelayedReleaseEvent	= (HANDLE)-1;
#else
		m_hDelayedReleaseEvent->Signal ();
		m_DelayedReleaseExitEvent->Wait ((int)timeout);

		//
		//	The event objects are deliberately not deleted: the Windows path above does not
		//	close its handles either, and if the wait timed out the thread may still be
		//	touching them.
		//
#endif
		m_hDelayedReleaseThread	= (ThreadTokenType)-1;
	}
}


///////////////////////////////////////////////////////////////////////////////////////////
//
//	Add_Delayed_Release_Object
//
///////////////////////////////////////////////////////////////////////////////////////////
void
WWAudioThreadsClass::Add_Delayed_Release_Object
(
	RefCountClass *	object,
	TimeType				delay
)
{
	if (m_IsShuttingDown) {
		REF_PTR_RELEASE (object);
	} else {

		//
		//	Make sure we have a thread running that will handle
		// the operation for us.
		//
		if (m_hDelayedReleaseThread == (ThreadTokenType)-1) {
			Create_Delayed_Release_Thread ();
		}

		//
		//	Wait for the release thread to finish using the
		// list pointer
		//
		{
			CriticalSectionClass::LockClass lock(m_ListMutex);

			//
			//	Create a new delay-information structure and
			//	add it to our list
			//
			DELAYED_RELEASE_INFO *info = W3DNEW DELAYED_RELEASE_INFO;
			info->object	= object;
			info->time		= ::GetTickCount () + delay;
			info->next		= m_ReleaseListHead;

			m_ReleaseListHead = info;
		}
	}
}


///////////////////////////////////////////////////////////////////////////////////////////
//
//	Flush_Delayed_Release_Objects
//
///////////////////////////////////////////////////////////////////////////////////////////
void
WWAudioThreadsClass::Flush_Delayed_Release_Objects ()
{
	CriticalSectionClass::LockClass lock(m_CriticalSection);

	//
	//	Loop through all the objects in our delay list, and
	// free them now.
	//
	DELAYED_RELEASE_INFO *info = nullptr;
	DELAYED_RELEASE_INFO *next = nullptr;
	for (info = m_ReleaseListHead; info != nullptr; info = next) {
		next = info->next;

		//
		//	Free the object
		//
		REF_PTR_RELEASE (info->object);
		SAFE_DELETE (info);
	}

	m_ReleaseListHead = nullptr;
}


///////////////////////////////////////////////////////////////////////////////////////////
//
//	Delayed_Release_Thread_Proc
//
///////////////////////////////////////////////////////////////////////////////////////////
void __cdecl
WWAudioThreadsClass::Delayed_Release_Thread_Proc (void * /*param*/)
{
	const TimeType base_timeout = 2000;
	TimeType timeout = base_timeout + rand () % 1000;

	//
	//	Keep looping forever until we are singalled to quit (or an error occurs)
	//
#ifdef _WIN32
	while (::WaitForSingleObject (m_hDelayedReleaseEvent, timeout) == WAIT_TIMEOUT) {
#else
	while (!m_hDelayedReleaseEvent->Wait ((int)timeout)) {
#endif

		{
			CriticalSectionClass::LockClass lock(m_ListMutex);

			//
			//	Loop through all the objects in our delay list, and
			// free any that have expired.
			//
			TimeType current_time		= ::GetTickCount ();
			DELAYED_RELEASE_INFO *curr = nullptr;
			DELAYED_RELEASE_INFO *prev	= nullptr;
			DELAYED_RELEASE_INFO *next	= nullptr;
			for (curr = m_ReleaseListHead; curr != nullptr; curr = next) {
				next = curr->next;

				//
				//	If the time has expired, free the object
				//
				if (current_time >= curr->time) {

					//
					//	Unlink the object
					//
					if (prev == nullptr) {
						m_ReleaseListHead = next;
					} else {
						prev->next = next;
					}

					//
					//	Free the object
					//
					REF_PTR_RELEASE (curr->object);
					SAFE_DELETE (curr);

				} else {
					prev = curr;
				}
			}
		}

		//
		//	To avoid 'periodic' releases, randomize our timeout
		//
		timeout = base_timeout + rand () % 1000;
	}

	Flush_Delayed_Release_Objects ();

#ifndef _WIN32
	//
	//	Let End_Delayed_Release_Thread() know the thread is done, since there is nothing
	//	portable to wait on the thread itself with.
	//
	m_DelayedReleaseExitEvent->Signal ();
#endif
}

/*
///////////////////////////////////////////////////////////////////////////////////////////
//
//	Begin_Modify_List
//
///////////////////////////////////////////////////////////////////////////////////////////
bool
WWAudioThreadsClass::Begin_Modify_List ()
{
	bool retval = false;

	//
	//	Wait for up to one second to modify the list object
	//
	if (m_ListMutex != nullptr) {
		retval = (::WaitForSingleObject (m_ListMutex, 1000) == WAIT_OBJECT_0);
		WWASSERT (retval);
	}

	return retval;
}


///////////////////////////////////////////////////////////////////////////////////////////
//
//	End_Modify_List
//
///////////////////////////////////////////////////////////////////////////////////////////
void
WWAudioThreadsClass::End_Modify_List ()
{
	//
	//	Release this thread's hold on the mutex object.
	//
	if (m_ListMutex != nullptr) {
		::ReleaseMutex (m_ListMutex);
	}
}
*/

