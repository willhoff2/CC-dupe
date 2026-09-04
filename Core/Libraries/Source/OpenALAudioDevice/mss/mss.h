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

/**
 * @file
 *
 * @brief Miles Sound System API as implemented on top of OpenAL.
 *
 * This declares exactly the slice of the Miles API that the engine references — see
 * docs/porting/audio-surface.md for the measured enumeration (101 functions, 230 call sites,
 * 10 files). It is source-compatible with the miles-sdk-stub header used by the 32-bit Windows
 * build so that WWAudio and MilesAudioManager compile unmodified against either.
 *
 * Windows 32-bit builds do not use this header; cmake/miles.cmake fetches the real stub there.
 */

#pragma once

#ifdef _WIN32
// The Win32 build uses the fetched miles-sdk-stub. Reaching this header on Win32 means the
// include path is wrong, and silently substituting the OpenAL layer would change behaviour.
#error "OpenALAudioDevice/mss/mss.h must not be used on Windows; use the fetched miles-sdk-stub."
#endif

#include <stdint.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

// ---------------------------------------------------------------------------------------------
// Win32 multimedia types the Miles API signatures expose. WWAudio fills in a PCMWAVEFORMAT and
// hands it to AIL_waveOutOpen, so the layout has to match the Win32 one.
// ---------------------------------------------------------------------------------------------
typedef struct WAVEOUT* HWAVEOUT;
typedef struct WAVEHDR* LPWAVEHDR;
typedef struct WAVEFORMAT* LPWAVEFORMAT;
typedef HWAVEOUT* LPHWAVEOUT;

typedef struct WAVEFORMAT
{
	uint16_t wFormatTag;
	uint16_t nChannels;
	uint32_t nSamplesPerSec;
	uint32_t nAvgBytesPerSec;
	uint16_t nBlockAlign;
	uint16_t wBitsPerSample;
	uint16_t cbSize;
} WAVEFORMAT;

typedef struct PCMWAVEFORMAT
{
	WAVEFORMAT wf;
	uint16_t wBitsPerSample;
} PCMWAVEFORMAT;

#ifndef WAVE_FORMAT_PCM
#define WAVE_FORMAT_PCM 0x0001
#endif
#ifndef WAVE_FORMAT_IMA_ADPCM
#define WAVE_FORMAT_IMA_ADPCM 0x0011
#endif

#if !defined(__stdcall)
#define __stdcall
#endif

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------------------------
// Handles. Opaque to the engine except for HDIGDRIVER::emulated_ds, which WWAudio.cpp reads to
// decide whether the 2D driver is a DirectSound emulation. All of these are compared against
// INVALID_MILES_HANDLE (a cast integer sentinel), so they must stay pointer- or int-sized.
// ---------------------------------------------------------------------------------------------
typedef struct h3DPOBJECT
{
	unsigned int junk;
} h3DPOBJECT;
typedef h3DPOBJECT* H3DPOBJECT;
typedef H3DPOBJECT H3DSAMPLE;
typedef struct _SAMPLE* HSAMPLE;
typedef struct _STREAM* HSTREAM;

typedef struct _DIG_DRIVER
{
	char pad[168];
	int emulated_ds;
} DIG_DRIVER;
typedef struct _DIG_DRIVER* HDIGDRIVER;

typedef struct _AUDIO* HAUDIO;
typedef struct _HMDIDRIVER* HMDIDRIVER;
typedef struct _HDLSDEVICE* HDLSDEVICE;
typedef void* HPROVIDER;
typedef int HTIMER;
typedef unsigned int HPROENUM;
typedef int M3DRESULT;

typedef void* AILLPDIRECTSOUND;
typedef void* AILLPDIRECTSOUNDBUFFER;

typedef struct _AILSOUNDINFO
{
	int format;
	const void* data_ptr;
	unsigned int data_len;
	unsigned int rate;
	int bits;
	int channels;
	unsigned int samples;
	unsigned int block_size;
	const void* initial_ptr;
} AILSOUNDINFO;

typedef enum
{
	DP_ASI_DECODER = 0,
	DP_FILTER,
	DP_MERGE,
	N_SAMPLE_STAGES,
	SAMPLE_ALL_STAGES
} SAMPLESTAGE;

#define AILCALLBACK

typedef unsigned long U32;
typedef long S32;
typedef float F32;

// Miles spells these two calls both ways depending on the header vintage; the engine uses both.
#define AIL_set_3D_object_user_data AIL_set_3D_user_data
#define AIL_3D_object_user_data AIL_3D_user_data
#define AIL_3D_open_listener AIL_open_3D_listener

typedef unsigned long(__stdcall* AIL_file_open_callback)(const char*, void**);
typedef void(__stdcall* AIL_file_close_callback)(void*);
typedef long(__stdcall* AIL_file_seek_callback)(void*, long, unsigned long);
typedef unsigned long(__stdcall* AIL_file_read_callback)(void*, void*, unsigned long);
typedef void(__stdcall* AIL_stream_callback)(HSTREAM);
typedef void(__stdcall* AIL_3dsample_callback)(H3DPOBJECT);
typedef void(__stdcall* AIL_sample_callback)(HSAMPLE);

#define IMPORTS

#define DIG_USE_WAVEOUT 15
#define AIL_LOCK_PROTECTION 18
#define ENVIRONMENT_GENERIC 0
#define HPROENUM_FIRST 0
#define AIL_NO_ERROR 0
#define AIL_FILE_SEEK_BEGIN 0
#define AIL_FILE_SEEK_CURRENT 1
#define AIL_FILE_SEEK_END 2
#define AIL_3D_2_SPEAKER 0
#define AIL_3D_HEADPHONE 1
#define AIL_3D_SURROUND 2
#define AIL_3D_4_SPEAKER 3
#define AIL_3D_51_SPEAKER 4
#define AIL_3D_71_SPEAKER 5
#define M3D_NOERR 0

// Raw PCM formats for AIL_set_sample_type, as Miles numbered them (bit 0 = 16-bit, bit 1 = stereo).
#define DIG_F_MONO_8 0
#define DIG_F_MONO_16 1
#define DIG_F_STEREO_8 2
#define DIG_F_STEREO_16 3
#define DIG_PCM_SIGN 0x0001

/// Miles reported its own version into a caller-supplied buffer; the debug audio overlay prints it.
void AIL_MSS_version(char* buffer, unsigned int size);

#ifndef YES
#define YES 1
#endif

#ifndef NO
#define NO 0
#endif

// ------------------------------------------------------------------- 3D sample control
float AIL_3D_sample_volume(H3DSAMPLE sample);
void AIL_set_3D_sample_volume(H3DSAMPLE sample, float volume);
void AIL_end_3D_sample(H3DSAMPLE sample);
void AIL_resume_3D_sample(H3DSAMPLE sample);
void AIL_stop_3D_sample(H3DSAMPLE sample);
void AIL_start_3D_sample(H3DSAMPLE sample);
unsigned int AIL_3D_sample_loop_count(H3DSAMPLE sample);
void AIL_set_3D_sample_offset(H3DSAMPLE sample, unsigned int offset);
int AIL_3D_sample_length(H3DSAMPLE sample);
unsigned int AIL_3D_sample_offset(H3DSAMPLE sample);
int AIL_3D_sample_playback_rate(H3DSAMPLE sample);
void AIL_set_3D_sample_playback_rate(H3DSAMPLE sample, int playback_rate);
int AIL_set_3D_sample_file(H3DSAMPLE sample, const void* file_image);
void AIL_set_3D_sample_loop_count(H3DSAMPLE sample, unsigned int count);
void AIL_set_3D_sample_effects_level(H3DSAMPLE sample, float effect_level);
void AIL_set_3D_sample_distances(H3DSAMPLE sample, float max_dist, float min_dist);
void AIL_set_3D_sample_occlusion(H3DSAMPLE sample, float occlusion);
void AIL_set_3D_velocity_vector(H3DSAMPLE sample, float x, float y, float z);
void AIL_release_3D_sample_handle(H3DSAMPLE sample);
H3DSAMPLE AIL_allocate_3D_sample_handle(HPROVIDER lib);
void* AIL_3D_user_data(H3DSAMPLE sample, unsigned int index);

// -------------------------------------------------------------- positional objects / listener
void AIL_set_3D_position(H3DPOBJECT obj, float X, float Y, float Z);
void AIL_set_3D_orientation(
	H3DPOBJECT obj, float X_face, float Y_face, float Z_face, float X_up, float Y_up, float Z_up);
void AIL_set_3D_user_data(H3DPOBJECT obj, unsigned int index, void* value);
H3DPOBJECT AIL_open_3D_listener(HPROVIDER lib);
void AIL_close_3D_listener(H3DPOBJECT listener);

// ------------------------------------------------------------------------ 2D sample control
HSAMPLE AIL_allocate_sample_handle(HDIGDRIVER dig);
void AIL_release_sample_handle(HSAMPLE sample);
void AIL_init_sample(HSAMPLE sample);
int AIL_set_named_sample_file(
	HSAMPLE sample, const char* file_name, const void* file_image, int file_size, int block);
int AIL_set_sample_file(HSAMPLE sample, const void* file_image, int block);
void AIL_start_sample(HSAMPLE sample);
void AIL_stop_sample(HSAMPLE sample);
void AIL_resume_sample(HSAMPLE sample);
void AIL_end_sample(HSAMPLE sample);
int AIL_sample_volume(HSAMPLE sample);
void AIL_set_sample_volume(HSAMPLE sample, int volume);
int AIL_sample_pan(HSAMPLE sample);
void AIL_set_sample_pan(HSAMPLE sample, int pan);
void AIL_sample_volume_pan(HSAMPLE sample, float* volume, float* pan);
void AIL_set_sample_volume_pan(HSAMPLE sample, float volume, float pan);
int AIL_sample_loop_count(HSAMPLE sample);
void AIL_set_sample_loop_count(HSAMPLE sample, int count);
void AIL_sample_ms_position(HSAMPLE sample, long* total_ms, long* current_ms);
void AIL_set_sample_ms_position(HSAMPLE sample, int pos);
int AIL_sample_playback_rate(HSAMPLE sample);
void AIL_set_sample_playback_rate(HSAMPLE sample, int playback_rate);
void* AIL_sample_user_data(HSAMPLE sample, unsigned int index);
void AIL_set_sample_user_data(HSAMPLE sample, unsigned int index, void* value);

// Miles' raw PCM feed: a sample that plays caller-supplied buffers instead of a file image. This is
// the path Bink used when it played its sound through Miles, and the one the FFmpeg video player
// uses here. Miles double-buffered; this backend queues, so AIL_sample_buffer_ready returns the next
// free slot far more often than Miles' two would, and -1 only when the queue is full.
#define MSS_SAMPLE_BUFFER_API 1
void AIL_set_sample_type(HSAMPLE sample, int format, unsigned int flags);
int AIL_sample_buffer_ready(HSAMPLE sample);
void AIL_load_sample_buffer(HSAMPLE sample, unsigned int buff_num, const void* buffer, unsigned int len);

// -------------------------------------------------------------------------------- streaming
HSTREAM AIL_open_stream(HDIGDRIVER dig, const char* filename, int stream_mem);
HSTREAM AIL_open_stream_by_sample(HDIGDRIVER driver, HSAMPLE sample, const char* file_name, int mem);
void AIL_close_stream(HSTREAM stream);
void AIL_start_stream(HSTREAM stream);
void AIL_pause_stream(HSTREAM stream, int onoff);
int AIL_stream_volume(HSTREAM stream);
void AIL_set_stream_volume(HSTREAM stream, int volume);
int AIL_stream_pan(HSTREAM stream);
void AIL_set_stream_pan(HSTREAM stream, int pan);
void AIL_stream_volume_pan(HSTREAM stream, float* volume, float* pan);
void AIL_set_stream_volume_pan(HSTREAM stream, float volume, float pan);
int AIL_stream_loop_count(HSTREAM stream);
void AIL_set_stream_loop_count(HSTREAM stream, int count);
void AIL_set_stream_loop_block(HSTREAM stream, int loop_start, int loop_end);
void AIL_stream_ms_position(HSTREAM sample, S32* total_milliseconds, S32* current_milliseconds);
void AIL_set_stream_ms_position(HSTREAM stream, int pos);
int AIL_stream_playback_rate(HSTREAM stream);
void AIL_set_stream_playback_rate(HSTREAM stream, int rate);

// ------------------------------------------------------------------------------- callbacks
AIL_stream_callback AIL_register_stream_callback(HSTREAM stream, AIL_stream_callback callback);
AIL_3dsample_callback AIL_register_3D_EOS_callback(H3DSAMPLE sample, AIL_3dsample_callback EOS);
AIL_sample_callback AIL_register_EOS_callback(HSAMPLE sample, AIL_sample_callback EOS);
void AIL_set_file_callbacks(AIL_file_open_callback opencb, AIL_file_close_callback closecb,
	AIL_file_seek_callback seekcb, AIL_file_read_callback readcb);

// ---------------------------------------------------------------- providers, filters, device
int AIL_enumerate_3D_providers(HPROENUM* next, HPROVIDER* dest, char** name);
M3DRESULT AIL_open_3D_provider(HPROVIDER lib);
void AIL_close_3D_provider(HPROVIDER lib);
void AIL_set_3D_speaker_type(HPROVIDER lib, int speaker_type);
int AIL_enumerate_filters(HPROENUM* next, HPROVIDER* dest, char** name);
void AIL_set_filter_sample_preference(HSAMPLE sample, const char* name, const void* val);
HPROVIDER AIL_set_sample_processor(HSAMPLE sample, SAMPLESTAGE pipeline_stage, HPROVIDER provider);
int AIL_waveOutOpen(HDIGDRIVER* driver, LPHWAVEOUT* waveout, int id, LPWAVEFORMAT format);
void AIL_waveOutClose(HDIGDRIVER driver);
void AIL_get_DirectSound_info(HSAMPLE sample, AILLPDIRECTSOUND* lplpDS, AILLPDIRECTSOUNDBUFFER* lplpDSB);

// ----------------------------------------------------------------------- library lifecycle
int AIL_startup(void);
void AIL_shutdown(void);
int AIL_set_preference(unsigned int number, int value);
char* AIL_set_redist_directory(const char* dir);
char* AIL_last_error(void);
void AIL_lock(void);
void AIL_unlock(void);
void AIL_stop_timer(HTIMER timer);
void AIL_release_timer_handle(HTIMER timer);
unsigned long AIL_get_timer_highest_delay(void);

// -------------------------------------------------------------------------- decode helpers
int AIL_WAV_info(const void* data, AILSOUNDINFO* info);
int AIL_decompress_ADPCM(const AILSOUNDINFO* info, void** outdata, unsigned long* outsize);
void AIL_mem_free_lock(void* ptr);

// ------------------------------------------------------------------ "quick" one-shot playback
int AIL_quick_startup(
	int use_digital, int use_MIDI, unsigned int output_rate, int output_bits, int output_channels);
void AIL_quick_shutdown(void);
void AIL_quick_handles(HDIGDRIVER* pdig, HMDIDRIVER* pmdi, HDLSDEVICE* pdls);
HAUDIO AIL_quick_load_and_play(const char* filename, unsigned int loop_count, int wait_request);
void AIL_quick_set_volume(HAUDIO audio, float volume, float extravol);
void AIL_quick_unload(HAUDIO audio);

#ifdef __cplusplus
} // extern "C"
#endif
