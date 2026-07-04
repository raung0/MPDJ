// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright The Music Player Daemon Project

#include "Source.hxx"
#include "Domain.hxx"
#include "MusicChunk.hxx"
#include "pcm/Buffer.hxx"
#include "pcm/PcmFormat.hxx"
#include "filter/Filter.hxx"
#include "filter/Prepared.hxx"
#include "filter/plugins/ReplayGainFilterPlugin.hxx"
#include "pcm/Mix.hxx"
#include "lib/fmt/AudioFormatFormatter.hxx"
#include "lib/fmt/RuntimeError.hxx"
#include "thread/Mutex.hxx"
#include "thread/ScopeUnlock.hxx"
#include "Log.hxx"

#include <string.h>

#include <algorithm>
#include <cmath>
#include <vector>

static size_t
OutputChunkFrameCount(const MusicChunk &chunk, const AudioFormat &format) noexcept
{
	const auto frame_size = format.GetFrameSize();
	return frame_size > 0 ? chunk.length / frame_size : 0;
}

static void
LogOutputChunk(const char *label, const MusicChunk *chunk,
	       const AudioFormat &format) noexcept
{
	if (chunk == nullptr) {
		FmtDebug(output_domain, "automix output {} chunk=null input_format={}",
			 label, format);
		return;
	}

	FmtDebug(output_domain,
		 "automix output {} length={} frames={} time={} bitrate={} mix_ratio={} automix={} ratio={} has_other={} input_format={} check={}",
		 label, chunk->length, OutputChunkFrameCount(*chunk, format),
		 chunk->time.ToDoubleS(), chunk->bit_rate, chunk->mix_ratio,
		 chunk->automix, chunk->automix_time_ratio, chunk->other != nullptr,
		 format,
#ifndef NDEBUG
		 chunk->CheckFormat(format)
#else
		 true
#endif
	);
}

AudioOutputSource::AudioOutputSource() noexcept = default;
AudioOutputSource::~AudioOutputSource() noexcept = default;

AudioFormat
AudioOutputSource::Open(const AudioFormat audio_format, const MusicPipe &_pipe,
			PreparedFilter *prepared_replay_gain_filter,
			PreparedFilter *prepared_other_replay_gain_filter,
			PreparedFilter &prepared_filter)
{
	assert(audio_format.IsValid());
	FmtDebug(output_domain, "automix output open requested input_format={} source_changed={}",
		 audio_format, !IsOpen() || &_pipe != &pipe.GetPipe());

	if (!IsOpen() || &_pipe != &pipe.GetPipe()) {
		current_chunk = nullptr;
		pipe.Init(_pipe);
	}

	/* (re)open the filter */

	if (filter && (filter_flushed || audio_format != in_audio_format)) {
		/* the filter must be reopened on all input format changes */
		Cancel();
		CloseFilter();
	}

	if (filter == nullptr)
		/* open the filter */
		OpenFilter(audio_format,
			   prepared_replay_gain_filter,
			   prepared_other_replay_gain_filter,
			   prepared_filter);

	in_audio_format = audio_format;
	return filter->GetOutAudioFormat();
}

void
AudioOutputSource::Close() noexcept
{
	assert(in_audio_format.IsValid());
	in_audio_format.Clear();

	CloseFilter();

	Cancel();
}

void
AudioOutputSource::Cancel() noexcept
{
	current_chunk = nullptr;
	cross_fade_previous_mix_ratio = -1.0f;
	pipe.Cancel();

	if (replay_gain_filter)
		replay_gain_filter->Reset();

	if (other_replay_gain_filter)
		other_replay_gain_filter->Reset();

	if (filter && !filter_flushed)
		filter->Reset();
}

inline void
AudioOutputSource::OpenFilter(AudioFormat audio_format,
			      PreparedFilter *prepared_replay_gain_filter,
			      PreparedFilter *prepared_other_replay_gain_filter,
			      PreparedFilter &prepared_filter)
try {
	assert(audio_format.IsValid());

	/* the replay_gain filter cannot fail here */
	if (prepared_other_replay_gain_filter) {
		other_replay_gain_serial = 0;
		other_replay_gain_filter =
			prepared_other_replay_gain_filter->Open(audio_format);
	}

	if (prepared_replay_gain_filter) {
		replay_gain_serial = 0;
		replay_gain_filter =
			prepared_replay_gain_filter->Open(audio_format);

		audio_format = replay_gain_filter->GetOutAudioFormat();

		assert(replay_gain_filter->GetOutAudioFormat() ==
		       other_replay_gain_filter->GetOutAudioFormat());
	}

	filter = prepared_filter.Open(audio_format);
	filter_flushed = false;
} catch (...) {
	CloseFilter();
	throw;
}

void
AudioOutputSource::CloseFilter() noexcept
{
	replay_gain_filter.reset();
	other_replay_gain_filter.reset();
	filter.reset();
}

std::span<const std::byte>
AudioOutputSource::GetChunkData(const MusicChunk &chunk,
				Filter *current_replay_gain_filter,
				unsigned *replay_gain_serial_p)
{
	assert(!chunk.IsEmpty());
	LogOutputChunk("get-data-enter", &chunk, in_audio_format);
	assert(chunk.CheckFormat(in_audio_format));

	auto data = chunk.ReadData();
	FmtDebug(output_domain, "automix output get-data raw_bytes={} replay_gain_filter={}",
		 data.size(), current_replay_gain_filter != nullptr);

	assert(data.size() % in_audio_format.GetFrameSize() == 0);

	if (!data.empty() && current_replay_gain_filter != nullptr) {
		replay_gain_filter_set_mode(*current_replay_gain_filter,
					    replay_gain_mode);

		if (chunk.replay_gain_serial != *replay_gain_serial_p) {
			replay_gain_filter_set_info(*current_replay_gain_filter,
						    chunk.replay_gain_serial != 0
						    ? &chunk.replay_gain_info
						    : nullptr);
			*replay_gain_serial_p = chunk.replay_gain_serial;
		}

		/* note: the ReplayGainFilter doesn't have a
		   ReadMore() method */
		data = current_replay_gain_filter->FilterPCM(data);
		FmtDebug(output_domain, "automix output replaygain result_bytes={}", data.size());
	}

	return data;
}

inline std::span<const std::byte>
AudioOutputSource::FilterChunk(const MusicChunk &chunk)
{
	assert(filter);
	assert(!filter_flushed);
	LogOutputChunk("filter-enter", &chunk, in_audio_format);
	LogOutputChunk("filter-other-enter", chunk.other.get(), in_audio_format);

	auto data = GetChunkData(chunk, replay_gain_filter.get(),
				 &replay_gain_serial);
	if (data.empty()) {
		FmtDebug(output_domain, "automix output filter primary empty");
		return data;
	}

	/* cross-fade */

	if (chunk.other != nullptr) {
		FmtDebug(output_domain, "automix output crossfade enter primary_bytes={} mix_ratio={}",
			 data.size(), chunk.mix_ratio);
		/*
		 * The secondary chunk is optional cross-fade/automix input.
		 * It may originate from the next decoder, whose native format can
		 * differ from the current output-source input format, especially
		 * after seek/pause/reopen paths.  Never pass a mismatched secondary
		 * chunk to GetChunkData(): that function asserts because primary
		 * chunks are required to match in_audio_format.  Dropping the optional
		 * secondary chunk is safer than aborting the player.
		 */
		if (!chunk.other->CheckFormat(in_audio_format)) {
			FmtDebug(output_domain, "automix output drop other reason=format-mismatch input_format={}", in_audio_format);
			return data;
		}

		auto other_data = GetChunkData(*chunk.other,
					       other_replay_gain_filter.get(),
					       &other_replay_gain_serial);
		if (other_data.empty()) {
			FmtDebug(output_domain, "automix output other empty after get-data");
			return data;
		}
		FmtDebug(output_domain, "automix output crossfade data primary_bytes={} other_bytes={}",
			 data.size(), other_data.size());

		/* if the "other" chunk is longer, then that trailer
		   is used as-is, without mixing; it is part of the
		   "next" song being faded in, and if there's a rest,
		   it means cross-fading ends here */

		if (data.size() > other_data.size()) {
			FmtDebug(output_domain, "automix output trim primary {} -> {}", data.size(), other_data.size());
			data = data.first(other_data.size());
		}

		float mix_ratio = chunk.mix_ratio;
		if (mix_ratio >= 0)
			/* reverse the mix ratio (because the
			   arguments to pcm_mix() are reversed), but
			   only if the mix ratio is non-negative; a
			   negative mix ratio is a MixRamp special
			   case */
			mix_ratio = 1.0f - mix_ratio;

		/*
		 * For normal cross-fade, only emit the overlapping part.
		 * Decoder/output conversion can make the incoming chunk slightly longer
		 * than the outgoing chunk; appending that tail here would play the next
		 * song at full volume in the middle of the fade, which sounds like a
		 * transition glitch.  MixRamp's negative mix_ratio keeps the historical
		 * behaviour below.
		 */
		const size_t dest_size = mix_ratio >= 0
			? std::min(data.size(), other_data.size())
			: other_data.size();
		FmtDebug(output_domain,
			 "automix output mix setup ratio={} dest_size={} primary_tail={} other_tail={}",
			 mix_ratio, dest_size, data.size() - std::min(data.size(), dest_size),
			 other_data.size() - std::min(other_data.size(), dest_size));
		void *dest = cross_fade_buffer.Get(dest_size);
		memcpy(dest, other_data.data(), dest_size);
		if (mix_ratio >= 0) {
			PcmBuffer old_buffer;
			PcmBuffer new_buffer;
			auto old_float = pcm_convert_to_float(old_buffer, in_audio_format.format,
							    data.first(dest_size));
			auto new_float = pcm_convert_to_float(new_buffer, in_audio_format.format,
							    other_data.first(dest_size));
			if (old_float.size() != new_float.size())
				throw FmtRuntimeError("Cannot cross-fade format {}",
						      in_audio_format.format);

			const unsigned channels = in_audio_format.channels;
			if (channels == 0 || old_float.size() % channels != 0)
				throw FmtRuntimeError("Cannot cross-fade format {}",
						      in_audio_format.format);

			/*
			 * Rewrite the positive-ratio crossfade as a sample-accurate
			 * equal-power ramp.  The player gives us one ratio per chunk;
			 * using that as a constant gain creates zipper noise and can sound
			 * like a hard transition when automix also does EQ/format conversion.
			 */
			const float old_start = std::clamp(1.0f - mix_ratio, 0.0f, 1.0f);
			float old_end = old_start;
			if (cross_fade_previous_mix_ratio >= 0.0f) {
				const float previous_old = std::clamp(cross_fade_previous_mix_ratio,
								       0.0f, 1.0f);
				const float step = previous_old - old_start;
				if (step > 0.0f)
					old_end = std::clamp(old_start - step, 0.0f, 1.0f);
			}
			cross_fade_previous_mix_ratio = old_start;

			constexpr float HALF_PI = 1.5707963267948966f;
			const size_t frames = old_float.size() / channels;
			std::vector<float> mixed(old_float.size());
			for (size_t frame = 0; frame < frames; ++frame) {
				const float pos = frames > 1
					? static_cast<float>(frame) / static_cast<float>(frames - 1)
					: 0.0f;
				const float old_ratio = old_start + (old_end - old_start) * pos;
				const float angle = (1.0f - old_ratio) * HALF_PI;
				const float gain_new = std::sin(angle);
				const float gain_old = std::cos(angle);

				for (unsigned ch = 0; ch < channels; ++ch) {
					const size_t i = frame * channels + ch;
					mixed[i] = new_float[i] * gain_new + old_float[i] * gain_old;
				}
			}

			PcmBuffer out_buffer;
			auto mixed_raw = pcm_convert_from_float(out_buffer,
							    in_audio_format.format,
							    mixed);
			if (mixed_raw.empty())
				throw FmtRuntimeError("Cannot cross-fade format {}",
						      in_audio_format.format);

			if (mixed_raw.size() != dest_size)
				throw FmtRuntimeError("Cannot cross-fade format {}",
						      in_audio_format.format);

			memcpy(dest, mixed_raw.data(), dest_size);
		} else if (!pcm_mix(cross_fade_dither, dest, data.data(), data.size(),
				     in_audio_format.format,
				     mix_ratio))
			throw FmtRuntimeError("Cannot cross-fade format {}",
					      in_audio_format.format);

		data = {(const std::byte *)dest, dest_size};
	} else
		cross_fade_previous_mix_ratio = -1.0f;

	/* apply filter chain */

	auto filtered = filter->FilterPCM(data);
	FmtDebug(output_domain, "automix output filter-exit in_bytes={} out_bytes={}",
		 data.size(), filtered.size());
	return filtered;
}

bool
AudioOutputSource::Fill(Mutex &mutex)
{
	assert(filter);
	assert(!filter_flushed);

	if (current_chunk != nullptr && pending_tag == nullptr &&
	    pending_data.empty())
		DropCurrentChunk();

	if (current_chunk != nullptr)
		return true;

	current_chunk = pipe.Get();
	if (current_chunk == nullptr)
		return false;

	pending_tag = current_chunk->tag.get();

	try {
		/* release the mutex while the filter runs, because
		   that may take a while */
		const ScopeUnlock unlock(mutex);

		pending_data = FilterChunk(*current_chunk);
	} catch (...) {
		current_chunk = nullptr;
		throw;
	}

	return true;
}

void
AudioOutputSource::ConsumeData(size_t nbytes) noexcept
{
	assert(filter);
	assert(!filter_flushed);

	pending_data = pending_data.subspan(nbytes);

	if (pending_data.empty()) {
		/* give the filter a chance to return more data in
		   another buffer */
		pending_data = filter->ReadMore();

		if (pending_data.empty())
			DropCurrentChunk();
	}
}

std::span<const std::byte>
AudioOutputSource::Flush()
{
	assert(filter);

	filter_flushed = true;
	return filter->Flush();
}
