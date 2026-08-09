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
 * @brief 3D sample voices and the listener.
 *
 * Miles uses H3DPOBJECT for both a positional voice and the listener, and AIL_set_3D_position /
 * AIL_set_3D_orientation are called with either, so both are represented by Object3D and told
 * apart by a flag.
 */

#include "OpenALAudioInternal.h"

#include <algorithm>
#include <cstring>

namespace OpenALAudio
{
namespace
{

Object3D* objectOf(H3DPOBJECT handle)
{
	return reinterpret_cast<Object3D*>(handle);
}

/// Occlusion and effects level have no core-OpenAL equivalent. Both are approximated as gain
/// attenuation so that occluded sounds at least get quieter; a real port routes them through EFX.
void applyGain(Object3D& object)
{
	if (object.voice.source == 0) {
		return;
	}
	const float attenuation = 1.0f - (object.occlusion * 0.75f);
	float gain = object.voice.volume * (attenuation < 0.0f ? 0.0f : attenuation);
	if (gain < 0.0f) gain = 0.0f;
	if (gain > 1.0f) gain = 1.0f;
	alSourcef(object.voice.source, AL_GAIN, gain);
}

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

} // namespace
} // namespace OpenALAudio

using namespace OpenALAudio;

// ------------------------------------------------------------------------------- allocation

H3DSAMPLE AIL_allocate_3D_sample_handle(HPROVIDER lib_handle)
{
	(void)lib_handle;

	Library& l = lib();
	std::lock_guard<std::recursive_mutex> guard(l.lock);
	if (!l.started) {
		return nullptr;
	}

	ALuint source = 0;
	alGenSources(1, &source);
	if (source == 0) {
		setLastError("alGenSources failed: voice limit reached");
		return nullptr;
	}

	Object3D* object = new Object3D();
	object->voice.source = source;
	alSourcei(source, AL_SOURCE_RELATIVE, AL_FALSE);
	alSourcef(source, AL_REFERENCE_DISTANCE, object->minDistance);
	alSourcef(source, AL_MAX_DISTANCE, object->maxDistance);

	l.objects.push_back(object);
	return (H3DSAMPLE)object;
}

void AIL_release_3D_sample_handle(H3DSAMPLE sample)
{
	Object3D* object = objectOf(sample);
	if (object == nullptr || object->isListener) {
		return;
	}

	Library& l = lib();
	std::lock_guard<std::recursive_mutex> guard(l.lock);

	if (object->voice.source != 0) {
		alSourceStop(object->voice.source);
		alSourcei(object->voice.source, AL_BUFFER, 0);
		alDeleteSources(1, &object->voice.source);
	}
	if (object->voice.audio.buffer != 0) {
		alDeleteBuffers(1, &object->voice.audio.buffer);
	}
	l.objects.erase(std::remove(l.objects.begin(), l.objects.end(), object), l.objects.end());
	delete object;
}

int AIL_set_3D_sample_file(H3DSAMPLE sample, const void* file_image)
{
	Object3D* object = objectOf(sample);
	if (object == nullptr) {
		return 0;
	}

	std::lock_guard<std::recursive_mutex> guard(lib().lock);

	if (object->voice.audio.buffer != 0) {
		alSourceStop(object->voice.source);
		alSourcei(object->voice.source, AL_BUFFER, 0);
		alDeleteBuffers(1, &object->voice.audio.buffer);
		object->voice.audio = DecodedAudio{};
	}

	if (!decodeWaveImage(file_image, riffSize(file_image), object->voice.audio)) {
		setLastError("unsupported 3D sample format (expected PCM or IMA ADPCM WAV)");
		return 0;
	}

	// A stereo buffer cannot be positioned by OpenAL. The engine's 3D sounds are mono; if a stereo
	// file arrives it still plays, just without spatialisation.
	alSourcei(object->voice.source, AL_BUFFER, (ALint)object->voice.audio.buffer);
	applyPlaybackRate(object->voice.source, object->voice.playbackRate, object->voice.audio.rate);
	return 1;
}

// ----------------------------------------------------------------------------------- transport

void AIL_start_3D_sample(H3DSAMPLE sample)
{
	Object3D* object = objectOf(sample);
	if (object == nullptr || object->voice.source == 0) {
		return;
	}
	std::lock_guard<std::recursive_mutex> guard(lib().lock);
	alSourcei(object->voice.source, AL_LOOPING, object->voice.loopCount == 0 ? AL_TRUE : AL_FALSE);
	alSourceRewind(object->voice.source);
	alSourcePlay(object->voice.source);
	object->voice.started = true;
	object->voice.paused = false;
}

void AIL_stop_3D_sample(H3DSAMPLE sample)
{
	Object3D* object = objectOf(sample);
	if (object == nullptr || object->voice.source == 0) {
		return;
	}
	std::lock_guard<std::recursive_mutex> guard(lib().lock);
	alSourcePause(object->voice.source);
	object->voice.paused = true;
}

void AIL_resume_3D_sample(H3DSAMPLE sample)
{
	Object3D* object = objectOf(sample);
	if (object == nullptr || object->voice.source == 0) {
		return;
	}
	std::lock_guard<std::recursive_mutex> guard(lib().lock);
	alSourcePlay(object->voice.source);
	object->voice.paused = false;
	object->voice.started = true;
}

void AIL_end_3D_sample(H3DSAMPLE sample)
{
	Object3D* object = objectOf(sample);
	if (object == nullptr || object->voice.source == 0) {
		return;
	}
	std::lock_guard<std::recursive_mutex> guard(lib().lock);
	alSourceStop(object->voice.source);
	object->voice.started = false;
	object->voice.paused = false;
}

// ------------------------------------------------------------------------------------- volume

float AIL_3D_sample_volume(H3DSAMPLE sample)
{
	Object3D* object = objectOf(sample);
	return (object != nullptr) ? object->voice.volume : 0.0f;
}

void AIL_set_3D_sample_volume(H3DSAMPLE sample, float volume)
{
	Object3D* object = objectOf(sample);
	if (object == nullptr) {
		return;
	}
	std::lock_guard<std::recursive_mutex> guard(lib().lock);
	object->voice.volume = volume;
	applyGain(*object);
}

void AIL_set_3D_sample_distances(H3DSAMPLE sample, float max_dist, float min_dist)
{
	// Note the Miles argument order: max first.
	Object3D* object = objectOf(sample);
	if (object == nullptr || object->voice.source == 0) {
		return;
	}
	std::lock_guard<std::recursive_mutex> guard(lib().lock);
	object->maxDistance = max_dist;
	object->minDistance = min_dist;
	alSourcef(object->voice.source, AL_REFERENCE_DISTANCE, min_dist);
	alSourcef(object->voice.source, AL_MAX_DISTANCE, max_dist);
}

void AIL_set_3D_sample_occlusion(H3DSAMPLE sample, float occlusion)
{
	Object3D* object = objectOf(sample);
	if (object == nullptr) {
		return;
	}
	std::lock_guard<std::recursive_mutex> guard(lib().lock);
	object->occlusion = occlusion;
	applyGain(*object);
}

void AIL_set_3D_sample_effects_level(H3DSAMPLE sample, float effect_level)
{
	// Reverb send level. Recorded only; there is no EFX effect slot yet.
	Object3D* object = objectOf(sample);
	if (object != nullptr) {
		object->effectsLevel = effect_level;
	}
}

// ------------------------------------------------------------------- looping and position

unsigned int AIL_3D_sample_loop_count(H3DSAMPLE sample)
{
	Object3D* object = objectOf(sample);
	return (object != nullptr) ? (unsigned int)object->voice.loopCount : 0;
}

void AIL_set_3D_sample_loop_count(H3DSAMPLE sample, unsigned int count)
{
	Object3D* object = objectOf(sample);
	if (object == nullptr) {
		return;
	}
	std::lock_guard<std::recursive_mutex> guard(lib().lock);
	object->voice.loopCount = (int)count;
	alSourcei(object->voice.source, AL_LOOPING, count == 0 ? AL_TRUE : AL_FALSE);
}

unsigned int AIL_3D_sample_offset(H3DSAMPLE sample)
{
	Object3D* object = objectOf(sample);
	if (object == nullptr || object->voice.source == 0) {
		return 0;
	}
	// Miles reports the offset in bytes into the sample data.
	ALint bytes = 0;
	alGetSourcei(object->voice.source, AL_BYTE_OFFSET, &bytes);
	return (unsigned int)bytes;
}

void AIL_set_3D_sample_offset(H3DSAMPLE sample, unsigned int offset)
{
	Object3D* object = objectOf(sample);
	if (object == nullptr || object->voice.source == 0) {
		return;
	}
	std::lock_guard<std::recursive_mutex> guard(lib().lock);
	alSourcei(object->voice.source, AL_BYTE_OFFSET, (ALint)offset);
}

int AIL_3D_sample_length(H3DSAMPLE sample)
{
	Object3D* object = objectOf(sample);
	return (object != nullptr) ? (int)object->voice.audio.lengthMs() : 0;
}

int AIL_3D_sample_playback_rate(H3DSAMPLE sample)
{
	Object3D* object = objectOf(sample);
	if (object == nullptr) {
		return 0;
	}
	return (object->voice.playbackRate != 0) ? object->voice.playbackRate
										     : (int)object->voice.audio.rate;
}

void AIL_set_3D_sample_playback_rate(H3DSAMPLE sample, int playback_rate)
{
	Object3D* object = objectOf(sample);
	if (object == nullptr) {
		return;
	}
	std::lock_guard<std::recursive_mutex> guard(lib().lock);
	object->voice.playbackRate = playback_rate;
	applyPlaybackRate(object->voice.source, playback_rate, object->voice.audio.rate);
}

// ---------------------------------------------------------------- positional (voice or listener)

void AIL_set_3D_position(H3DPOBJECT obj, float X, float Y, float Z)
{
	Object3D* object = objectOf(obj);
	if (object == nullptr) {
		return;
	}
	std::lock_guard<std::recursive_mutex> guard(lib().lock);
	if (object->isListener) {
		alListener3f(AL_POSITION, X, Y, Z);
	} else if (object->voice.source != 0) {
		alSource3f(object->voice.source, AL_POSITION, X, Y, Z);
	}
}

void AIL_set_3D_orientation(
	H3DPOBJECT obj, float X_face, float Y_face, float Z_face, float X_up, float Y_up, float Z_up)
{
	Object3D* object = objectOf(obj);
	if (object == nullptr) {
		return;
	}
	std::lock_guard<std::recursive_mutex> guard(lib().lock);
	if (object->isListener) {
		const ALfloat orientation[6] = { X_face, Y_face, Z_face, X_up, Y_up, Z_up };
		alListenerfv(AL_ORIENTATION, orientation);
	} else if (object->voice.source != 0) {
		// Sources have a cone direction rather than a full orientation; the up vector is unused.
		alSource3f(object->voice.source, AL_DIRECTION, X_face, Y_face, Z_face);
	}
}

void AIL_set_3D_velocity_vector(H3DSAMPLE sample, float x, float y, float z)
{
	Object3D* object = objectOf(sample);
	if (object == nullptr) {
		return;
	}
	std::lock_guard<std::recursive_mutex> guard(lib().lock);
	if (object->isListener) {
		alListener3f(AL_VELOCITY, x, y, z);
	} else if (object->voice.source != 0) {
		alSource3f(object->voice.source, AL_VELOCITY, x, y, z);
	}
}

void* AIL_3D_user_data(H3DSAMPLE sample, unsigned int index)
{
	Object3D* object = objectOf(sample);
	if (object == nullptr || index >= USER_DATA_SLOTS) {
		return nullptr;
	}
	return object->userData[index];
}

void AIL_set_3D_user_data(H3DPOBJECT obj, unsigned int index, void* value)
{
	Object3D* object = objectOf(obj);
	if (object == nullptr || index >= USER_DATA_SLOTS) {
		return;
	}
	object->userData[index] = value;
}

AIL_3dsample_callback AIL_register_3D_EOS_callback(H3DSAMPLE sample, AIL_3dsample_callback EOS)
{
	Object3D* object = objectOf(sample);
	if (object == nullptr) {
		return nullptr;
	}
	std::lock_guard<std::recursive_mutex> guard(lib().lock);
	AIL_3dsample_callback previous = object->endOfSample;
	object->endOfSample = EOS;
	return previous;
}

// -------------------------------------------------------------------------------- listener

H3DPOBJECT AIL_open_3D_listener(HPROVIDER lib_handle)
{
	(void)lib_handle;

	Library& l = lib();
	std::lock_guard<std::recursive_mutex> guard(l.lock);
	if (!l.started) {
		return nullptr;
	}
	if (l.listener != nullptr) {
		return (H3DPOBJECT)l.listener;
	}

	Object3D* listener = new Object3D();
	listener->isListener = true;
	l.objects.push_back(listener);
	l.listener = listener;
	return (H3DPOBJECT)listener;
}

void AIL_close_3D_listener(H3DPOBJECT listener)
{
	Object3D* object = objectOf(listener);
	if (object == nullptr || !object->isListener) {
		return;
	}

	Library& l = lib();
	std::lock_guard<std::recursive_mutex> guard(l.lock);
	l.objects.erase(std::remove(l.objects.begin(), l.objects.end(), object), l.objects.end());
	if (l.listener == object) {
		l.listener = nullptr;
	}
	delete object;
}
