// Declaration-only stand-in for scripts/native-port-probe.py. See README.md.
#pragma once

#include <windows.h>

// MMRESULT, TIMERR_NOERROR, timeGetTime, timeBeginPeriod, timeEndPeriod all come from
// Utility/time_compat.h via windows.h.

// Four-character code of a RIFF chunk or, via <vfw.h>, of an AVI stream type.
#define mmioFOURCC(ch0, ch1, ch2, ch3) \
	((DWORD)(BYTE)(ch0) | ((DWORD)(BYTE)(ch1) << 8) | \
	 ((DWORD)(BYTE)(ch2) << 16) | ((DWORD)(BYTE)(ch3) << 24))

#define MMSYSERR_NOERROR   0
#define MMSYSERR_ERROR     1
#define WAVE_FORMAT_PCM    1

typedef struct {
	WORD wFormatTag, nChannels;
	DWORD nSamplesPerSec, nAvgBytesPerSec;
	WORD nBlockAlign, wBitsPerSample, cbSize;
} WAVEFORMATEX, *LPWAVEFORMATEX;

typedef struct { UINT wPeriodMin, wPeriodMax; } TIMECAPS, *LPTIMECAPS;
extern "C" MMRESULT timeGetDevCaps(LPTIMECAPS ptc, UINT cbtc);
