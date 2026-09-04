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
 * @brief 2D sample voices, plus the "quick" one-shot playback used for speech.
 */

#include "OpenALAudioInternal.h"

#include <algorithm>
#include <cstring>
#include <vector>

namespace OpenALAudio
{
namespace
{

SampleVoice* voiceOf(HSAMPLE handle)
{
	return reinterpret_cast<SampleVoice*>(handle);
}

void releaseAudio(Voice& voice)
{
	if (voice.source != 0) {
		alSourceStop(voice.source);
		alSourcei(voice.source, AL_BUFFER, 0);
	}
	if (voice.audio.buffer != 0) {
		alDeleteBuffers(1, &voice.audio.buffer);
		voice.audio = DecodedAudio{};
	}
}

/// Miles' AIL_set_*_sample_file family takes a raw file image and returns 1 on success.
int loadImage(Voice& voice, const void* image, unsigned int size)
{
	releaseAudio(voice);
	if (!decodeWaveImage(image, size, voice.audio)) {
		setLastError("unsupported sample format (expected PCM or IMA ADPCM WAV)");
		return 0;
	}
	alSourcei(voice.source, AL_BUFFER, (ALint)voice.audio.buffer);
	applyPlaybackRate(voice.source, voice.playbackRate, voice.audio.rate);
	return 1;
}

/// Miles takes no image size. Bound the read with the RIFF size field, as AIL_WAV_info does.
unsigned int riffSize(const void* image)
{
	if (image == nullptr) {
		return 0;
	}
	const unsigned char* p = (const unsigned char*)image;
	const unsigned int riff = (unsigned int)p[4] | ((unsigned int)p[5] << 8)
		| ((unsigned int)p[6] << 16) | ((unsigned int)p[7] << 24);
	return (riff < 0xFFFFFFFFu - 8u) ? riff + 8u : 0xFFFFFFFFu;
}

struct QuickAudio
{
	ALuint source = 0;
	ALuint buffer = 0;
};

std::vector<QuickAudio*>& quickAudioList()
{
	static std::vector<QuickAudio*> list;
	return list;
}

} // namespace
} // namespace OpenALAudio

using namespace OpenALAudio;

// ------------------------------------------------------------------------------- allocation

HSAMPLE AIL_allocate_sample_handle(HDIGDRIVER dig)
{
	ApiCall api;
	(void)dig;

	Library& l = lib();
	LibraryGuard guard;
	if (!l.started) {
		return nullptr;
	}

	ALuint source = 0;
	alGenSources(1, &source);
	if (source == 0) {
		setLastError("alGenSources failed: voice limit reached");
		return nullptr;
	}

	// 2D voices are listener relative and unattenuated so that pan and volume are the only things
	// affecting them.
	alSourcei(source, AL_SOURCE_RELATIVE, AL_TRUE);
	alSourcef(source, AL_ROLLOFF_FACTOR, 0.0f);
	alSource3f(source, AL_POSITION, 0.0f, 0.0f, 0.0f);

	SampleVoice* voice = new SampleVoice();
	voice->source = source;
	l.samples.push_back(voice);
	return (HSAMPLE)voice;
}

void AIL_release_sample_handle(HSAMPLE sample)
{
	ApiCall api;
	SampleVoice* voice = voiceOf(sample);
	if (voice == nullptr) {
		return;
	}

	Library& l = lib();
	LibraryGuard guard;

	releaseAudio(*voice);
	if (voice->source != 0) {
		alDeleteSources(1, &voice->source);
	}
	l.samples.erase(std::remove(l.samples.begin(), l.samples.end(), voice), l.samples.end());
	delete voice;
}

void AIL_init_sample(HSAMPLE sample)
{
	ApiCall api;
	SampleVoice* voice = voiceOf(sample);
	if (voice == nullptr) {
		return;
	}

	Library& l = lib();
	LibraryGuard guard;

	releaseAudio(*voice);
	voice->volume = 1.0f;
	voice->pan = 0.5f;
	voice->loopCount = 1;
	voice->playbackRate = 0;
	voice->started = false;
	voice->completionPending = false;
	voice->paused = false;
	voice->filterPreferences.clear();
	std::memset(voice->processor, 0, sizeof(voice->processor));
	applyVolumePan(voice->source, voice->volume, voice->pan);
	alSourcef(voice->source, AL_PITCH, 1.0f);
	alSourcei(voice->source, AL_LOOPING, AL_FALSE);
}

// -------------------------------------------------------------------------------- file input

int AIL_set_sample_file(HSAMPLE sample, const void* file_image, int block)
{
	ApiCall api;
	(void)block;
	SampleVoice* voice = voiceOf(sample);
	if (voice == nullptr) {
		return 0;
	}
	LibraryGuard guard;
	return loadImage(*voice, file_image, riffSize(file_image));
}

int AIL_set_named_sample_file(
	HSAMPLE sample, const char* file_name, const void* file_image, int file_size, int block)
{
	ApiCall api;
	// The name is only used by Miles to pick a decoder by extension; the image is authoritative.
	(void)file_name;
	(void)block;
	SampleVoice* voice = voiceOf(sample);
	if (voice == nullptr) {
		return 0;
	}
	LibraryGuard guard;
	const unsigned int size = (file_size > 0) ? (unsigned int)file_size : riffSize(file_image);
	return loadImage(*voice, file_image, size);
}

// ----------------------------------------------------------------------------------- transport

void AIL_start_sample(HSAMPLE sample)
{
	ApiCall api;
	SampleVoice* voice = voiceOf(sample);
	if (voice == nullptr || voice->source == 0) {
		return;
	}
	LibraryGuard guard;

	// Miles' loop count of 0 means "forever". OpenAL can only express infinite looping natively;
	// finite repeat counts above one are approximated by a single pass, which matches how the
	// engine uses them (it retriggers effects itself).
	if (diagnostics().enabled) {
		diagnostics().sampleStarts.fetch_add(1);
		ALint state = 0;
		alGetSourcei(voice->source, AL_SOURCE_STATE, &state);
		if (state == AL_PLAYING) {
			diagnostics().sampleRestartsWhilePlaying.fetch_add(1);
		}
	}
	alSourcei(voice->source, AL_LOOPING, voice->loopCount == 0 ? AL_TRUE : AL_FALSE);
	alSourceRewind(voice->source);
	alSourcePlay(voice->source);
	voice->started = true;
	voice->completionPending = false;
	voice->paused = false;
}

void AIL_stop_sample(HSAMPLE sample)
{
	ApiCall api;
	SampleVoice* voice = voiceOf(sample);
	if (voice == nullptr || voice->source == 0) {
		return;
	}
	LibraryGuard guard;
	// Miles' stop is a pause that keeps the play position; AIL_end_sample is the hard stop.
	alSourcePause(voice->source);
	voice->paused = true;
}

void AIL_resume_sample(HSAMPLE sample)
{
	ApiCall api;
	SampleVoice* voice = voiceOf(sample);
	if (voice == nullptr || voice->source == 0) {
		return;
	}
	LibraryGuard guard;
	alSourcePlay(voice->source);
	voice->paused = false;
	voice->started = true;
	voice->completionPending = false;
}

void AIL_end_sample(HSAMPLE sample)
{
	ApiCall api;
	SampleVoice* voice = voiceOf(sample);
	if (voice == nullptr || voice->source == 0) {
		return;
	}
	LibraryGuard guard;
	alSourceStop(voice->source);
	voice->started = false;
	voice->completionPending = false;
	voice->paused = false;
}

// -------------------------------------------------------------------------- volume and pan

int AIL_sample_volume(HSAMPLE sample)
{
	ApiCall api;
	SampleVoice* voice = voiceOf(sample);
	return (voice != nullptr) ? (int)(voice->volume * MILES_MAX_INT_VOLUME) : 0;
}

void AIL_set_sample_volume(HSAMPLE sample, int volume)
{
	ApiCall api;
	SampleVoice* voice = voiceOf(sample);
	if (voice == nullptr) {
		return;
	}
	LibraryGuard guard;
	voice->volume = (float)volume / (float)MILES_MAX_INT_VOLUME;
	applyVolumePan(voice->source, voice->volume, voice->pan);
}

int AIL_sample_pan(HSAMPLE sample)
{
	ApiCall api;
	SampleVoice* voice = voiceOf(sample);
	return (voice != nullptr) ? (int)(voice->pan * MILES_MAX_INT_VOLUME) : 0;
}

void AIL_set_sample_pan(HSAMPLE sample, int pan)
{
	ApiCall api;
	SampleVoice* voice = voiceOf(sample);
	if (voice == nullptr) {
		return;
	}
	LibraryGuard guard;
	voice->pan = (float)pan / (float)MILES_MAX_INT_VOLUME;
	applyVolumePan(voice->source, voice->volume, voice->pan);
}

void AIL_sample_volume_pan(HSAMPLE sample, float* volume, float* pan)
{
	ApiCall api;
	SampleVoice* voice = voiceOf(sample);
	if (voice == nullptr) {
		return;
	}
	// Callers pass nullptr for the field they do not want; MilesAudioManager relies on this.
	if (volume != nullptr) *volume = voice->volume;
	if (pan != nullptr) *pan = voice->pan;
}

void AIL_set_sample_volume_pan(HSAMPLE sample, float volume, float pan)
{
	ApiCall api;
	SampleVoice* voice = voiceOf(sample);
	if (voice == nullptr) {
		return;
	}
	LibraryGuard guard;
	voice->volume = volume;
	voice->pan = pan;
	applyVolumePan(voice->source, volume, pan);
}

// ------------------------------------------------------------------- looping and position

int AIL_sample_loop_count(HSAMPLE sample)
{
	ApiCall api;
	SampleVoice* voice = voiceOf(sample);
	return (voice != nullptr) ? voice->loopCount : 0;
}

void AIL_set_sample_loop_count(HSAMPLE sample, int count)
{
	ApiCall api;
	SampleVoice* voice = voiceOf(sample);
	if (voice == nullptr) {
		return;
	}
	LibraryGuard guard;
	voice->loopCount = count;
	alSourcei(voice->source, AL_LOOPING, count == 0 ? AL_TRUE : AL_FALSE);
}

void AIL_sample_ms_position(HSAMPLE sample, long* total_ms, long* current_ms)
{
	ApiCall api;
	SampleVoice* voice = voiceOf(sample);
	if (voice == nullptr) {
		if (total_ms != nullptr) *total_ms = 0;
		if (current_ms != nullptr) *current_ms = 0;
		return;
	}

	LibraryGuard guard;
	if (total_ms != nullptr) {
		*total_ms = (long)voice->audio.lengthMs();
	}
	if (current_ms != nullptr) {
		ALfloat seconds = 0.0f;
		alGetSourcef(voice->source, AL_SEC_OFFSET, &seconds);
		*current_ms = (long)(seconds * 1000.0f);
	}
}

void AIL_set_sample_ms_position(HSAMPLE sample, int pos)
{
	ApiCall api;
	SampleVoice* voice = voiceOf(sample);
	if (voice == nullptr) {
		return;
	}
	LibraryGuard guard;
	alSourcef(voice->source, AL_SEC_OFFSET, (float)pos / 1000.0f);
}

int AIL_sample_playback_rate(HSAMPLE sample)
{
	ApiCall api;
	SampleVoice* voice = voiceOf(sample);
	if (voice == nullptr) {
		return 0;
	}
	return (voice->playbackRate != 0) ? voice->playbackRate : (int)voice->audio.rate;
}

void AIL_set_sample_playback_rate(HSAMPLE sample, int playback_rate)
{
	ApiCall api;
	SampleVoice* voice = voiceOf(sample);
	if (voice == nullptr) {
		return;
	}
	LibraryGuard guard;
	voice->playbackRate = playback_rate;
	applyPlaybackRate(voice->source, playback_rate, voice->audio.rate);
}

// ------------------------------------------------------------------------------- user data

void* AIL_sample_user_data(HSAMPLE sample, unsigned int index)
{
	ApiCall api;
	SampleVoice* voice = voiceOf(sample);
	if (voice == nullptr || index >= USER_DATA_SLOTS) {
		return nullptr;
	}
	return voice->userData[index];
}

void AIL_set_sample_user_data(HSAMPLE sample, unsigned int index, void* value)
{
	ApiCall api;
	SampleVoice* voice = voiceOf(sample);
	if (voice == nullptr || index >= USER_DATA_SLOTS) {
		return;
	}
	voice->userData[index] = value;
}

AIL_sample_callback AIL_register_EOS_callback(HSAMPLE sample, AIL_sample_callback EOS)
{
	ApiCall api;
	SampleVoice* voice = voiceOf(sample);
	if (voice == nullptr) {
		return nullptr;
	}
	LibraryGuard guard;
	AIL_sample_callback previous = voice->endOfSample;
	voice->endOfSample = EOS;
	return previous;
}

// ------------------------------------------------------------------------------------ filters

HPROVIDER AIL_set_sample_processor(HSAMPLE sample, SAMPLESTAGE pipeline_stage, HPROVIDER provider)
{
	ApiCall api;
	SampleVoice* voice = voiceOf(sample);
	if (voice == nullptr || pipeline_stage < 0 || pipeline_stage >= N_SAMPLE_STAGES) {
		return nullptr;
	}
	// Recorded but not applied: EFX is the intended route for reverb and mono delay.
	LibraryGuard guard;
	HPROVIDER previous = voice->processor[pipeline_stage];
	voice->processor[pipeline_stage] = provider;
	return previous;
}

void AIL_set_filter_sample_preference(HSAMPLE sample, const char* name, const void* val)
{
	ApiCall api;
	SampleVoice* voice = voiceOf(sample);
	if (voice == nullptr || name == nullptr) {
		return;
	}
	// All engine callers pass a float by address.
	const float value = (val != nullptr) ? *(const float*)val : 0.0f;
	LibraryGuard guard;
	voice->filterPreferences.emplace_back(name, value);
}

// ------------------------------------------------------------ "quick" one-shot playback (speech)

int AIL_quick_startup(
	int use_digital, int use_MIDI, unsigned int output_rate, int output_bits, int output_channels)
{
	ApiCall api;
	// MIDI is unused by Zero Hour and unsupported here.
	(void)use_digital;
	(void)use_MIDI;

	if (AIL_startup() != AIL_NO_ERROR) {
		return 0;
	}

	Library& l = lib();
	LibraryGuard guard;
	if (output_rate != 0) l.driver.rate = output_rate;
	if (output_bits != 0) l.driver.bits = (unsigned int)output_bits;
	if (output_channels != 0) l.driver.channels = (unsigned int)output_channels;
	l.driver.base.emulated_ds = 0;
	l.driver.open = true;
	return 1;
}

void AIL_quick_shutdown(void)
{
	ApiCall api;
	AIL_shutdown();
}

void AIL_quick_handles(HDIGDRIVER* pdig, HMDIDRIVER* pmdi, HDLSDEVICE* pdls)
{
	ApiCall api;
	Library& l = lib();
	if (pdig != nullptr) *pdig = (HDIGDRIVER)&l.driver;
	if (pmdi != nullptr) *pmdi = nullptr;
	if (pdls != nullptr) *pdls = nullptr;
}

HAUDIO AIL_quick_load_and_play(const char* filename, unsigned int loop_count, int wait_request)
{
	ApiCall api;
	// Used for speech playback. The file is read through the engine's file callbacks, because
	// speech lives in the same archives as everything else.
	(void)wait_request;

	Library& l = lib();
	LibraryGuard guard;

	if (!l.started || filename == nullptr || l.fileOpen == nullptr || l.fileRead == nullptr
		|| l.fileSeek == nullptr || l.fileClose == nullptr) {
		return nullptr;
	}

	void* file = nullptr;
	if (l.fileOpen(filename, &file) == 0 || file == nullptr) {
		setLastError("quick load: file open failed");
		return nullptr;
	}

	const long size = l.fileSeek(file, 0, AIL_FILE_SEEK_END);
	l.fileSeek(file, 0, AIL_FILE_SEEK_BEGIN);
	if (size <= 0) {
		l.fileClose(file);
		return nullptr;
	}

	std::vector<unsigned char> image((size_t)size);
	const unsigned long read = l.fileRead(file, image.data(), (unsigned long)size);
	l.fileClose(file);
	if (read == 0) {
		return nullptr;
	}

	DecodedAudio audio;
	if (!decodeWaveImage(image.data(), (unsigned int)read, audio)) {
		setLastError("quick load: unsupported format");
		return nullptr;
	}

	ALuint source = 0;
	alGenSources(1, &source);
	if (source == 0) {
		alDeleteBuffers(1, &audio.buffer);
		return nullptr;
	}

	alSourcei(source, AL_SOURCE_RELATIVE, AL_TRUE);
	alSourcef(source, AL_ROLLOFF_FACTOR, 0.0f);
	alSourcei(source, AL_BUFFER, (ALint)audio.buffer);
	alSourcei(source, AL_LOOPING, loop_count == 0 ? AL_TRUE : AL_FALSE);
	alSourcePlay(source);

	QuickAudio* quick = new QuickAudio{ source, audio.buffer };
	quickAudioList().push_back(quick);
	return (HAUDIO)quick;
}

void AIL_quick_set_volume(HAUDIO audio, float volume, float extravol)
{
	ApiCall api;
	QuickAudio* quick = reinterpret_cast<QuickAudio*>(audio);
	if (quick == nullptr) {
		return;
	}
	// Miles' second argument is an additional pan/volume trim; the engine passes a pan of 0.5.
	(void)extravol;
	LibraryGuard guard;
	alSourcef(quick->source, AL_GAIN, volume);
}

void AIL_quick_unload(HAUDIO audio)
{
	ApiCall api;
	QuickAudio* quick = reinterpret_cast<QuickAudio*>(audio);
	if (quick == nullptr) {
		return;
	}
	LibraryGuard guard;
	alSourceStop(quick->source);
	alSourcei(quick->source, AL_BUFFER, 0);
	alDeleteSources(1, &quick->source);
	alDeleteBuffers(1, &quick->buffer);

	std::vector<QuickAudio*>& list = quickAudioList();
	list.erase(std::remove(list.begin(), list.end(), quick), list.end());
	delete quick;
}
