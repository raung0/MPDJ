// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright The Music Player Daemon Project

/* \file
 *
 * The player thread controls the playback.  It acts as a bridge
 * between the decoder thread and the output thread(s): it receives
 * #MusicChunk objects from the decoder, optionally mixes them
 * (cross-fading), applies software volume, and sends them to the
 * audio outputs via PlayerOutputs::Play()
 * (i.e. MultipleOutputs::Play()).
 *
 * It is controlled by the main thread (the playlist code), see
 * Control.hxx.  The playlist enqueues new songs into the player
 * thread and sends it commands.
 *
 * The player thread itself does not do any I/O.  It synchronizes with
 * other threads via #Mutex and #Cond objects, and passes
 * #MusicChunk instances around in #MusicPipe objects.
 */

#include "Control.hxx"
#include "Outputs.hxx"
#include "Listener.hxx"
#include "decoder/Control.hxx"
#include "MusicPipe.hxx"
#include "MusicBuffer.hxx"
#include "pcm/Buffer.hxx"
#include "pcm/Convert.hxx"
#include "MusicChunk.hxx"
#include "pcm/PcmFormat.hxx"
#include "pcm/Traits.hxx"
#include "song/DetachedSong.hxx"
#include "CrossFade.hxx"
#include "pcm/MixRampGlue.hxx"
#include "tag/Tag.hxx"
#include "util/Domain.hxx"
#include "thread/Name.hxx"
#include "thread/ScopeUnlock.hxx"
#include "Log.hxx"
#include "lib/fmt/AudioFormatFormatter.hxx"

#include <exception>
#include <cmath>
#include <optional>
#include <memory>
#include <algorithm>
#include <vector>

#ifdef ENABLE_RUBBERBAND
#include <rubberband/RubberBandStretcher.h>
#endif

static constexpr Domain player_domain("player");

/**
 * Start playback as soon as enough data for this duration has been
 * pushed to the decoder pipe.
 */
static constexpr auto buffer_before_play_duration = std::chrono::seconds(1);

static size_t
AutomixFrameCount(const MusicChunk &chunk, const AudioFormat &format) noexcept
{
	const auto frame_size = format.GetFrameSize();
	return frame_size > 0 ? chunk.length / frame_size : 0;
}


static constexpr float AUTOMIX_SILENCE_LEVEL = 0.00316227766f; // -50 dBFS
static constexpr unsigned AUTOMIX_PREROLL_MS = 5;
static constexpr unsigned AUTOMIX_MAX_SILENCE_TRIM_MS = 3000;

static float
AutomixFramePeak(std::span<const float> samples, unsigned channels,
		 size_t frame) noexcept
{
	float peak = 0.0f;
	for (unsigned ch = 0; ch < channels; ++ch)
		peak = std::max(peak, std::abs(samples[frame * channels + ch]));
	return peak;
}

static float
AutomixRms(std::span<const float> samples) noexcept
{
	if (samples.empty())
		return 0.0f;

	double sum = 0.0;
	for (float sample : samples)
		sum += double(sample) * double(sample);

	return static_cast<float>(std::sqrt(sum / samples.size()));
}

static size_t
AutomixLeadingSilentFrames(std::span<const float> samples,
			   const AudioFormat &format) noexcept
{
	const unsigned channels = format.channels;
	if (channels == 0 || samples.size() < channels)
		return 0;

	const size_t frames = samples.size() / channels;
	const size_t max_trim_frames = std::min<size_t>(frames,
		format.sample_rate * AUTOMIX_MAX_SILENCE_TRIM_MS / 1000);
	const size_t preroll_frames = format.sample_rate * AUTOMIX_PREROLL_MS / 1000;

	size_t trim_frames = 0;
	while (trim_frames < max_trim_frames &&
	       AutomixFramePeak(samples, channels, trim_frames) < AUTOMIX_SILENCE_LEVEL)
		++trim_frames;

	if (trim_frames <= preroll_frames)
		return 0;

	return trim_frames - preroll_frames;
}

static void
AutomixTrimLeadingSilence(std::vector<std::byte> &bytes,
			 const AudioFormat &format,
			 const char *label) noexcept
{
	if (bytes.empty())
		return;

	PcmBuffer temp;
	auto samples = pcm_convert_to_float(temp, format.format,
		std::span<const std::byte>(bytes.data(), bytes.size()));
	const size_t trim_frames = AutomixLeadingSilentFrames(samples, format);
	if (trim_frames == 0)
		return;

	const size_t trim_bytes = std::min(bytes.size(),
		trim_frames * format.GetFrameSize());
	bytes.erase(bytes.begin(), bytes.begin() + trim_bytes);
	FmtDebug(player_domain,
		 "automix trim leading silence label={} frames={} bytes={} remaining_bytes={}",
		 label, trim_frames, trim_bytes, bytes.size());
}

static void
LogAutomixChunk(const char *label, const MusicChunk *chunk,
		const AudioFormat &format) noexcept
{
	if (chunk == nullptr) {
		FmtDebug(player_domain, "automix {} chunk=null format={}", label, format);
		return;
	}

	FmtDebug(player_domain,
		 "automix {} chunk length={} frames={} time={} bitrate={} mix_ratio={} automix={} ratio={} has_other={} empty={} format={} check={}",
		 label, chunk->length, AutomixFrameCount(*chunk, format),
		 chunk->time.ToDoubleS(), chunk->bit_rate, chunk->mix_ratio,
		 chunk->automix, chunk->automix_time_ratio,
		 chunk->other != nullptr, chunk->IsEmpty(), format,
#ifndef NDEBUG
		 chunk->CheckFormat(format)
#else
		 true
#endif
	);
}

struct AutomixPlan {
	unsigned phrase_beats;
	SongTime transition_start;
	std::optional<float> time_ratio;
	float eq_strength;
};

struct AutomixEqState {
	std::vector<float> lowpass;
	std::unique_ptr<PcmConvert> convert;
	AudioFormat convert_src_format = AudioFormat::Undefined();
	AudioFormat convert_dst_format = AudioFormat::Undefined();
	std::vector<std::byte> automix_pending;

#ifdef ENABLE_RUBBERBAND
	std::unique_ptr<RubberBand::RubberBandStretcher> stretcher;
	AudioFormat stretcher_src_format = AudioFormat::Undefined();
	AudioFormat stretcher_dst_format = AudioFormat::Undefined();
	float stretcher_time_ratio = 0.0f;
	std::vector<std::byte> stretcher_pending;
	size_t stretcher_pending_offset = 0;
	size_t stretcher_start_delay = 0;
#endif

	void Reset() noexcept {
		lowpass.clear();
		convert.reset();
		convert_src_format.Clear();
		convert_dst_format.Clear();
		automix_pending.clear();

#ifdef ENABLE_RUBBERBAND
		stretcher.reset();
		stretcher_src_format.Clear();
		stretcher_dst_format.Clear();
		stretcher_time_ratio = 0.0f;
		stretcher_pending.clear();
		stretcher_pending_offset = 0;
		stretcher_start_delay = 0;
#endif
	}

	std::span<const std::byte> Convert(const MusicChunk &src,
					  const AudioFormat &src_format,
					  const AudioFormat &dst_format) {
		if (!convert || convert_src_format != src_format ||
		    convert_dst_format != dst_format) {
			convert = std::make_unique<PcmConvert>(src_format, dst_format);
			convert_src_format = src_format;
			convert_dst_format = dst_format;
		}

		return convert->Convert(src.ReadData());
	}

#ifdef ENABLE_RUBBERBAND
	void AppendStretcherOutput(std::span<const std::byte> output) {
		if (output.empty())
			return;

		if (stretcher_pending_offset > 0 &&
		    stretcher_pending_offset == stretcher_pending.size()) {
			stretcher_pending.clear();
			stretcher_pending_offset = 0;
		}

		stretcher_pending.insert(stretcher_pending.end(),
					 output.begin(), output.end());
	}
#endif
};

static float
ComputeAutomixEqStrength(const DetachedSong &current, const DetachedSong &next,
				 unsigned phrase_beats, double best_error) noexcept
{
	const double beat_score = std::clamp(
		std::min(current.GetBeats().size(), next.GetBeats().size()) / 128.0,
		0.0, 1.0);
	const double phrase_score = phrase_beats >= 64 ? 1.0
		: phrase_beats >= 32 ? 0.7
		: 0.45;
	const double ratio_score = best_error <= 0.15
		? 1.0 - best_error / 0.15
		: 0.0;

	const double strength = 0.20 + ratio_score * 0.50 + beat_score * 0.20 +
		phrase_score * 0.10;
	return std::clamp((float)strength, 0.15f, 0.85f);
}

static void
ApplyAutomixEq(std::span<float> samples, const AudioFormat &audio_format,
	       float phase, float strength, AutomixEqState &state,
	       bool incoming) noexcept
{
	if (samples.empty() || strength <= 0.0f)
		return;

	const unsigned channels = audio_format.channels;
	if (channels == 0)
		return;

	if (state.lowpass.size() != channels)
		state.lowpass.assign(channels, 0.0f);

	phase = std::clamp(phase, 0.0f, 1.0f);

	/* Slightly lower the bass shelf cutoff for more dramatic transitions. */
	const float cutoff_hz = 150.0f + 180.0f * strength;
	const float alpha = 1.0f - std::exp(-6.28318530717958647692f * cutoff_hz /
					   (float)audio_format.sample_rate);

	const float bass_gain = incoming
		? (1.0f - strength + strength * phase)
		: (1.0f - strength * phase);
	const float treble_gain = 1.0f + 0.06f * strength * (incoming
		? (1.0f - phase)
		: phase);

	for (size_t i = 0; i < samples.size(); i += channels)
		for (unsigned ch = 0; ch < channels; ++ch) {
			float &low = state.lowpass[ch];
			const float x = samples[i + ch];

			low += alpha * (x - low);
			const float high = x - low;
			samples[i + ch] = high * treble_gain + low * bass_gain;
		}
}

static MusicChunkPtr
AutomixEqChunk(MusicBuffer &buffer, const MusicChunk &src,
	       const AudioFormat &src_format, const AudioFormat &dst_format,
	       float phase,
	       float eq_strength, AutomixEqState &eq_state, bool incoming)
{
	FmtDebug(player_domain,
		 "automix eq enter incoming={} phase={} strength={} src_format={} dst_format={}",
		 incoming, phase, eq_strength, src_format, dst_format);
	LogAutomixChunk(incoming ? "eq-src-incoming" : "eq-src-outgoing", &src, src_format);
	auto converted = eq_state.Convert(src, src_format, dst_format);
	if (converted.empty()) {
		FmtDebug(player_domain, "automix eq failed reason=convert-empty incoming={}", incoming);
		return nullptr;
	}

	PcmBuffer temp;
	auto float_data = pcm_convert_to_float(temp, dst_format.format,
					      converted);
	if (float_data.empty()) {
		FmtDebug(player_domain, "automix rubberband failed reason=float-empty converted_bytes={}", converted.size());
		return nullptr;
	}

	std::vector<float> samples(float_data.begin(), float_data.end());
	ApplyAutomixEq(samples, dst_format, phase, eq_strength, eq_state,
		       incoming);

	PcmBuffer out_temp;
	auto raw = pcm_convert_from_float(out_temp, dst_format.format,
					 samples);
	if (raw.empty()) {
		FmtDebug(player_domain, "automix eq failed reason=raw-empty incoming={} samples={}", incoming, samples.size());
		return nullptr;
	}

	auto dest = buffer.Allocate();
	if (!dest) {
		FmtDebug(player_domain, "automix eq failed reason=allocate incoming={}", incoming);
		return nullptr;
	}

	auto writable = dest->Write(dst_format, SongTime::Cast(src.time), src.bit_rate);
	const size_t nbytes = std::min(writable.size(), raw.size());
	std::copy(raw.begin(), raw.begin() + nbytes, writable.begin());
	dest->Expand(dst_format, nbytes);
	dest->tag = src.tag ? std::make_unique<Tag>(*src.tag) : nullptr;
	dest->replay_gain_info = src.replay_gain_info;
	dest->replay_gain_serial = src.replay_gain_serial;
	dest->mix_ratio = src.mix_ratio;
	dest->automix = src.automix;
	dest->automix_time_ratio = src.automix_time_ratio;
	LogAutomixChunk(incoming ? "eq-dst-incoming" : "eq-dst-outgoing", dest.get(), dst_format);
	return dest;
}

static void
LogAutomixPlan(const DetachedSong &current, const DetachedSong &next,
		 const AutomixPlan &plan, unsigned cross_fade_chunks) noexcept
{
	if (plan.time_ratio)
		FmtDebug(player_domain,
			 "automix plan current={:?} next={:?} phrase_beats={} transition_start={} ratio={} eq_strength={} chunks={}",
			 current.GetURI(), next.GetURI(), plan.phrase_beats,
			 plan.transition_start.ToDoubleS(), *plan.time_ratio,
			 plan.eq_strength,
			 cross_fade_chunks);
	else
		FmtDebug(player_domain,
			 "automix plan current={:?} next={:?} phrase_beats={} transition_start={} ratio=none eq_strength={} chunks={}",
			 current.GetURI(), next.GetURI(), plan.phrase_beats,
			 plan.transition_start.ToDoubleS(), plan.eq_strength,
			 cross_fade_chunks);
}

static std::optional<AutomixPlan>
GetAutomixPlan(const DetachedSong &current,
		const DetachedSong &next) noexcept
{
	const auto current_bpm = current.GetBpm();
	const auto next_bpm = next.GetBpm();
	if (!current_bpm) {
		FmtDebug(player_domain,
			 "automix unavailable current={:?} next={:?} reason=current bpm missing",
			 current.GetURI(), next.GetURI());
		return std::nullopt;
	}
	if (!next_bpm) {
		FmtDebug(player_domain,
			 "automix unavailable current={:?} next={:?} reason=next bpm missing",
			 current.GetURI(), next.GetURI());
		return std::nullopt;
	}
	if (*current_bpm <= 0 || *next_bpm <= 0) {
		FmtDebug(player_domain,
			 "automix unavailable current={:?} next={:?} reason=invalid bpm current={} next={}",
			 current.GetURI(), next.GetURI(), *current_bpm, *next_bpm);
		return std::nullopt;
	}

	const auto &current_beats = current.GetBeats();
	const auto &next_beats = next.GetBeats();
	FmtDebug(player_domain,
		 "automix inspect current={:?} next={:?} current_bpm={} next_bpm={} current_beats={} next_beats={}",
		 current.GetURI(), next.GetURI(), *current_bpm, *next_bpm,
		 current_beats.size(), next_beats.size());
	for (unsigned phrase_beats : { 64u, 32u, 16u }) {
		if (current_beats.size() <= phrase_beats || next_beats.size() <= phrase_beats) {
			FmtDebug(player_domain,
				 "automix reject current={:?} next={:?} phrase_beats={} reason=insufficient beat data current_beats={} next_beats={}",
				 current.GetURI(), next.GetURI(), phrase_beats,
				 current_beats.size(), next_beats.size());
			continue;
		}

		static constexpr double scales[] = { 0.5, 1.0, 2.0 };

		double best_ratio = 1.0;
		double best_error = 1e9;

		for (double current_scale : scales)
			for (double next_scale : scales) {
				const double adjusted_current = *current_bpm * current_scale;
				const double adjusted_next = *next_bpm * next_scale;
				const double ratio = adjusted_next / adjusted_current;
				const double error = std::abs(ratio - 1.0);

				if (error < best_error) {
					best_error = error;
					best_ratio = ratio;
				}
			}

		const std::size_t start_index = ((current_beats.size() - 1) / phrase_beats) * phrase_beats;
		if (start_index == 0) {
			FmtDebug(player_domain,
				 "automix reject current={:?} next={:?} phrase_beats={} reason=no aligned boundary current_beats={}",
				 current.GetURI(), next.GetURI(), phrase_beats,
				 current_beats.size());
			continue;
		}

		double transition_start = current_beats[start_index];
		const auto duration = current.GetDuration();
		if (!duration.IsNegative()) {
			/*
			 * The last phrase boundary can land inside a long outro fade.
			 * That technically creates a crossfade, but the user hears the old
			 * song finish its own fade before the next track becomes audible.
			 * Keep at least this much real overlap by pulling the start point
			 * back to the latest beat before the fade-only tail.
			 */
			static constexpr double MIN_AUDIBLE_OVERLAP = 16.0;
			const double latest_start =
				std::max(0.0, duration.ToDoubleS() - MIN_AUDIBLE_OVERLAP);

			if (transition_start > latest_start) {
				std::size_t adjusted_index = start_index;
				while (adjusted_index > 0 &&
				       current_beats[adjusted_index] > latest_start)
					--adjusted_index;

				const double adjusted_start = adjusted_index > 0
					? current_beats[adjusted_index]
					: latest_start;

				FmtDebug(player_domain,
					 "automix outro adjustment current={:?} next={:?} phrase_beats={} duration={} old_start={} new_start={} min_overlap={}",
					 current.GetURI(), next.GetURI(), phrase_beats,
					 duration.ToDoubleS(), transition_start, adjusted_start,
					 MIN_AUDIBLE_OVERLAP);
				transition_start = adjusted_start;
			}
		}

		std::optional<float> ratio;
		if (best_error <= 0.15)
			ratio = static_cast<float>(best_ratio);
		else
			FmtDebug(player_domain,
				 "automix ratio fallback current={:?} next={:?} phrase_beats={} best_ratio={} best_error={}",
				 current.GetURI(), next.GetURI(), phrase_beats,
				 best_ratio, best_error);

		return AutomixPlan{
			phrase_beats,
			SongTime::FromS(transition_start),
			ratio,
			ComputeAutomixEqStrength(current, next, phrase_beats, best_error),
		};
	}

	return std::nullopt;
}


static MusicChunkPtr
AutomixMixChunk(MusicBuffer &buffer,
	       const MusicChunk &current, const MusicChunk *incoming,
	       const AudioFormat &current_format,
	       const AudioFormat &incoming_format,
	       const AudioFormat &mix_format,
	       MusicPipe &incoming_pipe,
	       float phase_start, float phase_end,
	       float eq_strength,
	       [[maybe_unused]] AutomixEqState &outgoing_eq_state,
	       AutomixEqState &incoming_eq_state)
{
	FmtDebug(player_domain,
		 "automix mix enter phase_start={} phase_end={} strength={} current_format={} incoming_format={} mix_format={}",
		 phase_start, phase_end, eq_strength, current_format, incoming_format, mix_format);
	LogAutomixChunk("mix-current-in", &current, current_format);
	LogAutomixChunk("mix-incoming-in", incoming, incoming_format);
	auto current_data = outgoing_eq_state.Convert(current, current_format, mix_format);
	std::vector<std::byte> incoming_bytes;
	if (!incoming_eq_state.automix_pending.empty()) {
		FmtDebug(player_domain,
			 "automix mix using pending incoming_bytes={}",
			 incoming_eq_state.automix_pending.size());
		incoming_bytes.insert(incoming_bytes.end(),
			incoming_eq_state.automix_pending.begin(),
			incoming_eq_state.automix_pending.end());
		incoming_eq_state.automix_pending.clear();
	}
	if (incoming != nullptr) {
		auto first = incoming_eq_state.Convert(*incoming, incoming_format, mix_format);
		incoming_bytes.insert(incoming_bytes.end(), first.begin(), first.end());
	}

	/*
	 * Decoder chunk sizes are byte-count based, not duration based.  In the
	 * failing case A is 44.1 kHz/16-bit/stereo (1002 frames per chunk) while B
	 * is 48 kHz/24-bit/stereo (501 frames per decoder chunk).  After converting
	 * B to A's format, one B chunk covers only ~441-460 frames.  Mixing exactly
	 * one B chunk per one A chunk makes B advance at roughly half speed and
	 * sounds slow/choppy.  Pull enough incoming chunks to cover the current
	 * chunk's duration before mixing.
	 */
	unsigned pulled_extra = 0;
	auto pull_more_incoming = [&] {
		while (incoming_bytes.size() < current_data.size() && incoming_pipe.GetSize() > 0) {
			auto extra = incoming_pipe.Shift();
			if (extra == nullptr)
				break;

			LogAutomixChunk("mix-incoming-extra", extra.get(), incoming_format);
			if (extra->IsEmpty())
				continue;

			auto converted = incoming_eq_state.Convert(*extra, incoming_format, mix_format);
			incoming_bytes.insert(incoming_bytes.end(), converted.begin(), converted.end());
			++pulled_extra;
		}
	};

	pull_more_incoming();
	AutomixTrimLeadingSilence(incoming_bytes, mix_format, "incoming");
	pull_more_incoming();

	FmtDebug(player_domain,
		 "automix mix converted current_bytes={} incoming_bytes={} pulled_extra={} remaining_next_pipe={}",
		 current_data.size(), incoming_bytes.size(), pulled_extra, incoming_pipe.GetSize());
	if (current_data.empty() || incoming_bytes.empty()) {
		FmtDebug(player_domain, "automix mix failed reason=convert-empty");
		return nullptr;
	}

	PcmBuffer current_temp;
	PcmBuffer incoming_temp;
	auto current_float = pcm_convert_to_float(current_temp, mix_format.format,
						 current_data);
	auto incoming_float = pcm_convert_to_float(incoming_temp, mix_format.format,
						  std::span<const std::byte>(incoming_bytes.data(), incoming_bytes.size()));
	if (current_float.empty() || incoming_float.empty()) {
		FmtDebug(player_domain,
			 "automix mix failed reason=float-empty current_samples={} incoming_samples={}",
			 current_float.size(), incoming_float.size());
		return nullptr;
	}

	const unsigned channels = mix_format.channels;
	if (channels == 0 || current_float.size() % channels != 0 ||
	    incoming_float.size() % channels != 0)
		return nullptr;

	const size_t current_frames = current_float.size() / channels;
	const size_t incoming_frames = incoming_float.size() / channels;
	const size_t overlap_frames = std::min(current_frames, incoming_frames);
	if (overlap_frames == 0) {
		FmtDebug(player_domain, "automix mix failed reason=zero-overlap");
		return nullptr;
	}

	const size_t overlap_samples = overlap_frames * channels;
	const float current_overlap_rms = AutomixRms(
		std::span<const float>(current_float.data(), overlap_samples));
	const float incoming_overlap_rms = AutomixRms(
		std::span<const float>(incoming_float.data(), overlap_samples));
	const size_t overlap_bytes = mix_format.GetFrameSize() * overlap_frames;
	const size_t incoming_tail_bytes = incoming_bytes.size() > overlap_bytes
		? incoming_bytes.size() - overlap_bytes
		: 0;
	FmtDebug(player_domain,
		 "automix mix overlap bytes={} frames={} current_tail_bytes={} incoming_tail_bytes={}",
		 overlap_bytes, overlap_frames,
		 current_data.size() > overlap_bytes ? current_data.size() - overlap_bytes : 0,
		 incoming_tail_bytes);

	if (incoming_tail_bytes > 0) {
		/*
		 * Keep converted-but-unmixed B samples for the next automix chunk.
		 * Without this, pulling extra incoming decoder chunks to cover A's
		 * duration fixes underfeeding but skips the unused B tail every
		 * callback, making B sound choppy/fast-forwarded.
		 */
		incoming_eq_state.automix_pending.assign(incoming_bytes.begin() + overlap_bytes,
						      incoming_bytes.end());
		FmtDebug(player_domain,
			 "automix mix saved pending incoming_bytes={}",
			 incoming_eq_state.automix_pending.size());
	}

	/*
	 * Preserve the outgoing chunk duration exactly.  Earlier automix code
	 * returned only the overlapped prefix when the incoming decoder chunk was
	 * shorter after format conversion (e.g. 48 kHz/24-bit B vs 44.1 kHz/16-bit
	 * A).  That discarded the tail of A on every crossfade chunk, causing the
	 * audible speed wobble/crackle.  Start with a full copy of A and mix B only
	 * over the prefix that actually exists.
	 */
	std::vector<float> old_samples(current_float.begin(), current_float.end());
	std::vector<float> new_samples(incoming_float.begin(), incoming_float.end());
	ApplyAutomixEq(new_samples, mix_format, phase_start, eq_strength,
		       incoming_eq_state, true);

	phase_start = std::clamp(phase_start, 0.0f, 1.0f);
	phase_end = std::clamp(phase_end, phase_start, 1.0f);

	/*
	 * Avoid crossfading silence/fades as if they were full-strength song
	 * content.  If A has already faded down to near silence, move the fade
	 * clock forward so B takes over smoothly.  If B has an authored fade-in,
	 * give it a limited make-up gain so MPD does not double-fade it into
	 * inaudibility.  The gain is capped to avoid exploding actual silence.
	 */
	if (current_overlap_rms < AUTOMIX_SILENCE_LEVEL &&
	    incoming_overlap_rms >= AUTOMIX_SILENCE_LEVEL) {
		phase_start = std::max(phase_start, 0.85f);
		phase_end = std::max(phase_end, phase_start);
	}

	float incoming_makeup = 1.0f;
	if (current_overlap_rms >= AUTOMIX_SILENCE_LEVEL &&
	    incoming_overlap_rms >= AUTOMIX_SILENCE_LEVEL &&
	    incoming_overlap_rms < current_overlap_rms)
		incoming_makeup = std::clamp(
			std::sqrt(current_overlap_rms / incoming_overlap_rms),
			1.0f, 2.5f);

	FmtDebug(player_domain,
		 "automix envelope current_rms={} incoming_rms={} phase_start={} phase_end={} incoming_makeup={}",
		 current_overlap_rms, incoming_overlap_rms, phase_start, phase_end,
		 incoming_makeup);

	constexpr float HALF_PI = 1.57079632679489661923f;
	std::vector<float> mixed(old_samples);
	for (size_t frame = 0; frame < overlap_frames; ++frame) {
		const float pos = current_frames > 1
			? static_cast<float>(frame) / static_cast<float>(current_frames - 1)
			: 0.0f;
		const float phase = phase_start + (phase_end - phase_start) * pos;
		const float angle = phase * HALF_PI;
		const float gain_old = std::cos(angle);
		const float gain_new = std::min(1.0f, std::sin(angle) * incoming_makeup);

		for (unsigned ch = 0; ch < channels; ++ch) {
			const size_t i = frame * channels + ch;
			mixed[i] = old_samples[i] * gain_old + new_samples[i] * gain_new;
		}
	}

	PcmBuffer out_temp;
	auto raw = pcm_convert_from_float(out_temp, mix_format.format, mixed);
	if (raw.empty())
		return nullptr;

	auto dest = buffer.Allocate();
	if (!dest)
		return nullptr;

	auto writable = dest->Write(mix_format, SongTime::Cast(current.time), current.bit_rate);
	const size_t nbytes = std::min(writable.size(), raw.size());
	std::copy(raw.begin(), raw.begin() + nbytes, writable.begin());
	dest->Expand(mix_format, nbytes);
	dest->tag = current.tag ? std::make_unique<Tag>(*current.tag) : nullptr;
	dest->replay_gain_info = current.replay_gain_info;
	dest->replay_gain_serial = current.replay_gain_serial;
	dest->mix_ratio = current.mix_ratio;
	dest->automix = true;
	dest->automix_time_ratio = current.automix_time_ratio;
	return dest;
}

#ifdef ENABLE_RUBBERBAND
static MusicChunkPtr
AutomixChunk(MusicBuffer &buffer, const MusicChunk &src,
	     const AudioFormat &src_format, const AudioFormat &dst_format,
	     float time_ratio,
	     float eq_strength, float phase, AutomixEqState &eq_state)
{
	if (time_ratio <= 0.0f || eq_strength <= 0.0f)
		return nullptr;

	auto converted = eq_state.Convert(src, src_format, dst_format);
	if (converted.empty())
		return nullptr;

	PcmBuffer temp;
	auto float_data = pcm_convert_to_float(temp, dst_format.format,
					      converted);
	if (float_data.empty()) {
		FmtDebug(player_domain, "automix rubberband failed reason=float-empty converted_bytes={}", converted.size());
		return nullptr;
	}

	/*
	 * RubberBand's real-time stretcher has latency and may not have
	 * produced enough output for this chunk yet.  Keep an EQ-only dry copy
	 * available so the automix transition never injects zero-filled audio
	 * while the stretcher is warming up.
	 */
	std::vector<float> dry_samples(float_data.begin(), float_data.end());
	ApplyAutomixEq(dry_samples, dst_format, phase, eq_strength, eq_state, true);
	PcmBuffer dry_temp;
	auto dry_raw = pcm_convert_from_float(dry_temp, dst_format.format,
					       dry_samples);

	const unsigned channels = dst_format.channels;
	const size_t frames = float_data.size() / channels;
	if (frames == 0)
		return nullptr;

	const bool need_open = !eq_state.stretcher ||
		eq_state.stretcher_src_format != src_format ||
		eq_state.stretcher_dst_format != dst_format;

	if (need_open) {
		const RubberBand::RubberBandStretcher::Options options =
			RubberBand::RubberBandStretcher::OptionProcessRealTime |
			RubberBand::RubberBandStretcher::OptionEngineFiner |
			RubberBand::RubberBandStretcher::OptionPitchHighConsistency |
			RubberBand::RubberBandStretcher::OptionChannelsTogether;

		eq_state.stretcher = std::make_unique<RubberBand::RubberBandStretcher>(
			dst_format.sample_rate, channels, options, time_ratio, 1.0);
		eq_state.stretcher_src_format = src_format;
		eq_state.stretcher_dst_format = dst_format;
		eq_state.stretcher_time_ratio = time_ratio;
		eq_state.stretcher_pending.clear();
		eq_state.stretcher_pending_offset = 0;
		eq_state.stretcher_start_delay = 0;
		eq_state.stretcher->setMaxProcessSize(4096);

		const size_t pad = eq_state.stretcher->getPreferredStartPad();
		if (pad > 0) {
			std::vector<std::vector<float>> silence(channels,
				std::vector<float>(pad, 0.0f));
			std::vector<const float *> input_ptrs(channels);
			for (unsigned ch = 0; ch < channels; ++ch)
				input_ptrs[ch] = silence[ch].data();
			eq_state.stretcher->process(input_ptrs.data(), pad, false);
		}

		eq_state.stretcher_start_delay =
			eq_state.stretcher->getStartDelay();
	}

	if (eq_state.stretcher_time_ratio != time_ratio) {
		eq_state.stretcher->setTimeRatio(time_ratio);
		eq_state.stretcher_time_ratio = time_ratio;
	}

	std::vector<std::vector<float>> input_channels(channels,
		std::vector<float>(frames));
	for (size_t frame = 0; frame < frames; ++frame)
		for (unsigned ch = 0; ch < channels; ++ch)
			input_channels[ch][frame] = float_data[frame * channels + ch];

	std::vector<const float *> input_ptrs(channels);
	for (unsigned ch = 0; ch < channels; ++ch)
		input_ptrs[ch] = input_channels[ch].data();

	eq_state.stretcher->process(input_ptrs.data(), frames, false);

	const int available = eq_state.stretcher->available();
	if (available > 0) {
		std::vector<std::vector<float>> output_channels(channels,
			std::vector<float>(size_t(available)));
		std::vector<float *> output_ptrs(channels);
		for (unsigned ch = 0; ch < channels; ++ch)
			output_ptrs[ch] = output_channels[ch].data();

		const auto retrieved = eq_state.stretcher->retrieve(output_ptrs.data(),
								   size_t(available));
		if (retrieved > 0) {
			const size_t retrieved_frames = size_t(retrieved);
			const size_t start = std::min(eq_state.stretcher_start_delay,
						      retrieved_frames);
			eq_state.stretcher_start_delay -= start;

			if (start < retrieved_frames) {
				std::vector<float> interleaved((retrieved_frames - start) * channels);
				for (size_t frame = start; frame < retrieved_frames; ++frame)
					for (unsigned ch = 0; ch < channels; ++ch)
						interleaved[(frame - start) * channels + ch] =
							output_channels[ch][frame];

				ApplyAutomixEq(interleaved, dst_format, phase, eq_strength,
					       eq_state, true);

				PcmBuffer out_temp;
				auto raw = pcm_convert_from_float(out_temp, dst_format.format,
								 interleaved);
				if (!raw.empty())
					eq_state.AppendStretcherOutput(raw);
			}
		}
	}

	auto dest = buffer.Allocate();
	if (!dest)
		return nullptr;

	auto writable = dest->Write(dst_format, SongTime::Cast(src.time), src.bit_rate);
	const size_t nbytes = std::min(writable.size(), converted.size());
	const size_t dry_bytes = std::min(nbytes, dry_raw.size());
	if (dry_bytes > 0)
		std::copy_n(dry_raw.begin(), dry_bytes, writable.begin());
	if (dry_bytes < nbytes)
		std::fill(writable.begin() + dry_bytes, writable.begin() + nbytes,
			  std::byte{});

	const size_t pending_bytes = eq_state.stretcher_pending.size() -
				    eq_state.stretcher_pending_offset;
	const size_t copy_bytes = std::min(nbytes, pending_bytes);
	if (copy_bytes > 0) {
		std::copy_n(eq_state.stretcher_pending.begin() +
				 eq_state.stretcher_pending_offset,
				 copy_bytes, writable.begin());
		eq_state.stretcher_pending_offset += copy_bytes;
		if (eq_state.stretcher_pending_offset == eq_state.stretcher_pending.size()) {
			eq_state.stretcher_pending.clear();
			eq_state.stretcher_pending_offset = 0;
		} else if (eq_state.stretcher_pending_offset >= 65536) {
			eq_state.stretcher_pending.erase(eq_state.stretcher_pending.begin(),
						 eq_state.stretcher_pending.begin() +
						 eq_state.stretcher_pending_offset);
			eq_state.stretcher_pending_offset = 0;
		}
	}
	dest->Expand(dst_format, nbytes);
	dest->tag = src.tag ? std::make_unique<Tag>(*src.tag) : nullptr;
	dest->replay_gain_info = src.replay_gain_info;
	dest->replay_gain_serial = src.replay_gain_serial;
	dest->mix_ratio = src.mix_ratio;
	dest->automix = src.automix;
	dest->automix_time_ratio = src.automix_time_ratio;
	return dest;
}
#endif

class Player {
	PlayerControl &pc;

	DecoderControl &dc;

	MusicBuffer &buffer;

	std::shared_ptr<MusicPipe> pipe;

	/**
	 * the song currently being played
	 */
	std::unique_ptr<DetachedSong> song;

	/**
	 * The tag of the "next" song during cross-fade.  It is
	 * postponed, and sent to the output thread when the new song
	 * really begins.
	 */
	std::unique_ptr<Tag> cross_fade_tag;

	/**
	 * Start playback as soon as this number of chunks has been
	 * pushed to the decoder pipe.  This is calculated based on
	 * #buffer_before_play_duration.
	 */
	unsigned buffer_before_play;

	/**
	 * If the decoder pipe gets consumed below this threshold,
	 * it's time to wake up the decoder.
	 *
	 * It is calculated in a way which should prevent a wakeup
	 * after each single consumed chunk; it is more efficient to
	 * make the decoder decode a larger block at a time.
	 */
	const unsigned decoder_wakeup_threshold;

	/**
	 * Are we waiting for #buffer_before_play?
	 */
	bool buffering = true;

	/**
	 * true if the decoder is starting and did not provide data
	 * yet
	 */
	bool decoder_starting = false;

	/**
	 * Did we wake up the DecoderThread recently?  This avoids
	 * duplicate wakeup calls.
	 */
	bool decoder_woken = false;

	/**
	 * is the player paused?
	 */
	bool paused = false;

	/**
	 * is there a new song in pc.next_song?
	 */
	bool queued = true;

	/**
	 * Was any audio output opened successfully?  It might have
	 * failed meanwhile, but was not explicitly closed by the
	 * player thread.  When this flag is unset, some output
	 * methods must not be called.
	 */
	bool output_open = false;

	/**
	 * Is cross-fading to the next song enabled?
	 */
	enum class CrossFadeState : uint8_t {
		/**
		 * The initial state: we don't know yet if we will
		 * cross-fade; it will be determined soon.
		 */
		UNKNOWN,

		/**
		 * Cross-fading is disabled for the transition to the
		 * next song.
		 */
		DISABLED,

		/**
		 * Cross-fading is enabled (but may not yet be in
		 * progress), will start near the end of the current
		 * song.
		 */
		ENABLED,

		/**
		 * Currently cross-fading to the next song.
		 */
		ACTIVE,
	} xfade_state = CrossFadeState::UNKNOWN;

	/**
	 * The number of chunks used for crossfading.
	 */
	unsigned cross_fade_chunks = 0;

	std::optional<float> automix_time_ratio;
	std::optional<float> automix_eq_strength;
	AutomixEqState automix_eq_outgoing;
	AutomixEqState automix_eq_incoming;

	/**
	 * The current audio format for the audio outputs.
	 */
	AudioFormat play_audio_format = AudioFormat::Undefined();

	/**
	 * The time stamp of the chunk most recently sent to the
	 * output thread.  This attribute is only used if
	 * MultipleOutputs::GetElapsedTime() didn't return a usable
	 * value; the output thread can estimate the elapsed time more
	 * precisely.
	 */
	SongTime elapsed_time = SongTime::zero();

	/**
	 * If this is positive, then we need to ask the decoder to
	 * seek after it has completed startup.  This is needed if the
	 * decoder is in the middle of startup while the player
	 * receives another seek command.
	 *
	 * This is only valid while #decoder_starting is true.
	 */
	SongTime pending_seek;

public:
	Player(PlayerControl &_pc, DecoderControl &_dc,
	       MusicBuffer &_buffer) noexcept
		:pc(_pc), dc(_dc), buffer(_buffer),
		 decoder_wakeup_threshold(buffer.GetSize() * 3 / 4)
	{
	}

private:
	/**
	 * Reset cross-fading to the initial state.  A check to
	 * re-enable it at an appropriate time will be scheduled.
	 */
	void ResetAutomixTransition() noexcept {
		automix_time_ratio.reset();
		automix_eq_strength.reset();
		automix_eq_outgoing.Reset();
		automix_eq_incoming.Reset();
	}

	void ResetCrossFade() noexcept {
		xfade_state = CrossFadeState::UNKNOWN;
		ResetAutomixTransition();
	}

	template<typename P>
	void ReplacePipe(P &&_pipe) noexcept {
		ResetCrossFade();
		pipe = std::forward<P>(_pipe);
	}

	/**
	 * Start the decoder.
	 *
	 * Caller must lock the mutex.
	 */
	void StartDecoder(std::unique_lock<Mutex> &lock,
			  std::shared_ptr<MusicPipe> pipe,
			  bool initial_seek_essential) noexcept;

	/**
	 * The decoder has acknowledged the "START" command (see
	 * ActivateDecoder()).  This function checks if the decoder
	 * initialization has completed yet.  If not, it will wait
	 * some more.
	 *
	 * Caller must lock the mutex.
	 *
	 * @return false if the decoder has failed, true on success
	 * (though the decoder startup may or may not yet be finished)
	 */
	bool CheckDecoderStartup(std::unique_lock<Mutex> &lock) noexcept;

	/**
	 * Stop the decoder and clears (and frees) its music pipe.
	 *
	 * Caller must lock the mutex.
	 */
	void StopDecoder(std::unique_lock<Mutex> &lock) noexcept;

	/**
	 * Is the decoder still busy on the same song as the player?
	 *
	 * Note: this function does not check if the decoder is already
	 * finished.
	 */
	[[nodiscard]] [[gnu::pure]]
	bool IsDecoderAtCurrentSong() const noexcept {
		assert(pipe != nullptr);

		return dc.pipe == pipe;
	}

	/**
	 * Returns true if the decoder is decoding the next song (or has begun
	 * decoding it, or has finished doing it), and the player hasn't
	 * switched to that song yet.
	 */
	[[nodiscard]] [[gnu::pure]]
	bool IsDecoderAtNextSong() const noexcept {
		return dc.pipe != nullptr && !IsDecoderAtCurrentSong();
	}

	/**
	 * Invoke DecoderControl::Seek() and update our state or
	 * handle errors.
	 *
	 * Caller must lock the mutex.
	 *
	 * @return false if the decoder has failed
	 */
	bool SeekDecoder(std::unique_lock<Mutex> &lock,
			 SongTime seek_time) noexcept;

	/**
	 * This is the handler for the #PlayerCommand::SEEK command.
	 *
	 * Caller must lock the mutex.
	 *
	 * @return false if the decoder has failed
	 */
	bool SeekDecoder(std::unique_lock<Mutex> &lock) noexcept;

	void CancelPendingSeek() noexcept {
		pending_seek = SongTime::zero();
		pc.CancelPendingSeek();
	}

	/**
	 * Check if the decoder has reported an error, and forward it
	 * to PlayerControl::SetError().
	 *
	 * @return false if an error has occurred
	 */
	bool ForwardDecoderError() noexcept;

	/**
	 * After the decoder has been started asynchronously, activate
	 * it for playback.  That is, make the currently decoded song
	 * active (assign it to #song), clear PlayerControl::next_song
	 * and #queued, initialize #elapsed_time, and set
	 * #decoder_starting.
	 *
	 * When returning, the decoder may not have completed startup
	 * yet, therefore we don't know the audio format yet.  To
	 * finish decoder startup, call CheckDecoderStartup().
	 *
	 * Caller must lock the mutex.
	 */
	void ActivateDecoder() noexcept;

	/**
	 * Wrapper for MultipleOutputs::Open().  Upon failure, it
	 * pauses the player.
	 *
	 * Caller must lock the mutex.
	 *
	 * @return true on success
	 */
	bool OpenOutput() noexcept;

	std::string UnlockAnalyzeMixRamp(const MusicPipe &pipe,
					 const AudioFormat &audio_format,
					 MixRampDirection direction) noexcept;

	/**
	 * @return false if more chunks of the next song are needed to
	 * scan for MixRamp data
	 */
	[[nodiscard]]
	bool MixRampScannerReady() noexcept;

	void CheckCrossFade() noexcept;

	/**
	 * Obtains the next chunk from the music pipe, optionally applies
	 * cross-fading, and sends it to all audio outputs.
	 *
	 * @return true on success, false on error (playback will be stopped)
	 */
	bool PlayNextChunk() noexcept;

	unsigned UnlockCheckOutputs() noexcept {
		const ScopeUnlock unlock(pc.mutex);
		return pc.outputs.CheckPipe();
	}

	/**
	 * Player lock must be held before calling.
	 *
	 * @return false to stop playback
	 */
	bool ProcessCommand(std::unique_lock<Mutex> &lock) noexcept;

	/**
	 * This is called at the border between two songs: the audio output
	 * has consumed all chunks of the current song, and we should start
	 * sending chunks from the next one.
	 *
	 * Caller must lock the mutex.
	 */
	void SongBorder() noexcept;

public:
	/*
	 * The main loop of the player thread, during playback.  This
	 * is basically a state machine, which multiplexes data
	 * between the decoder thread and the output threads.
	 */
	void Run() noexcept;
};

void
Player::StartDecoder(std::unique_lock<Mutex> &lock,
		     std::shared_ptr<MusicPipe> _pipe,
		     bool initial_seek_essential) noexcept
{
	assert(!decoder_starting);
	assert(queued || pc.command == PlayerCommand::SEEK);
	assert(pc.next_song != nullptr);

	/* copy ReplayGain parameters to the decoder */
	dc.replay_gain_mode = pc.replay_gain_mode;

	SongTime start_time = pc.next_song->GetStartTime() + pc.seek_time;

	dc.Start(lock, std::make_unique<DetachedSong>(*pc.next_song),
		 start_time, pc.next_song->GetEndTime(),
		 initial_seek_essential,
		 buffer, std::move(_pipe));
}

void
Player::StopDecoder(std::unique_lock<Mutex> &lock) noexcept
{
	const PlayerControl::ScopeOccupied occupied(pc);

	dc.Stop(lock);

	if (dc.pipe != nullptr) {
		/* clear and free the decoder pipe */

		dc.pipe->Clear();
		dc.pipe.reset();

		/* just in case we've been cross-fading: cancel it
		   now, because we just deleted the new song's decoder
		   pipe */
		ResetCrossFade();
	}

	decoder_starting = false;
}

inline bool
Player::ForwardDecoderError() noexcept
{
	if (dc.HasFailed()) {
		pc.SetError(PlayerError::DECODER, dc.GetError());
		return false;
	}

	return true;
}

void
Player::ActivateDecoder() noexcept
{
	assert(queued || pc.command == PlayerCommand::SEEK);
	assert(pc.next_song != nullptr);

	queued = false;

	pc.ClearTaggedSong();

	song = std::exchange(pc.next_song, nullptr);

	elapsed_time = pc.seek_time;

	/* set the "starting" flag, which will be cleared by
	   CheckDecoderStartup() */
	decoder_starting = true;
	pending_seek = SongTime::zero();

	/* update PlayerControl's song information */
	pc.total_time = song->GetDuration();
	pc.bit_rate = 0;
	pc.audio_format.Clear();

	{
		/* call playlist::SyncWithPlayer() in the main thread */
		const ScopeUnlock unlock(pc.mutex);
		pc.listener.OnPlayerSync();
	}
}

/**
 * Returns the real duration of the song, comprising the duration
 * indicated by the decoder plugin.
 */
static SignedSongTime
real_song_duration(const DetachedSong &song,
		   SignedSongTime decoder_duration) noexcept
{
	if (decoder_duration.IsNegative())
		/* the decoder plugin didn't provide information; fall
		   back to Song::GetDuration() */
		return song.GetDuration();

	const SongTime start_time = song.GetStartTime();
	const SongTime end_time = song.GetEndTime();

	if (end_time.IsPositive() && end_time < SongTime(decoder_duration))
		return {end_time - start_time};

	return {SongTime(decoder_duration) - start_time};
}

std::string
Player::UnlockAnalyzeMixRamp(const MusicPipe &_pipe,
			    const AudioFormat &audio_format,
			    MixRampDirection direction) noexcept
{
	const ScopeUnlock unlock(pc.mutex);
	return AnalyzeMixRamp(_pipe, audio_format, direction);
}

inline bool
Player::MixRampScannerReady() noexcept
{
	assert(pipe);
	assert(dc.pipe);

	if (!pc.cross_fade.IsMixRampEnabled())
		return true;

	if (!pc.config.mixramp_analyzer)
		/* always ready if the scanner is disabled */
		return true;

	if (dc.GetMixRampPreviousEnd() == nullptr) {
		// TODO: scan incrementally backwards until mixrampdb is reached
		auto s = UnlockAnalyzeMixRamp(*pipe, play_audio_format,
					      MixRampDirection::END);
		if (!s.empty()) {
			FmtDebug(player_domain, "Analyzed MixRamp end: {}", s);
			dc.SetMixRampPreviousEnd(std::move(s));
		}

		if (dc.GetMixRampStart() == nullptr)
			/* scan the next song in the next call; first,
			   let the main loop submit a few more chunks
			   to the outputs for playback to avoid
			   xrun */
			return false;
	}

	if (dc.GetMixRampStart() == nullptr) {
		const std::size_t want_pipe_bytes =
			dc.out_audio_format.TimeToSize(std::chrono::seconds{20});
		const std::size_t want_pipe_chunks =
			std::min((want_pipe_bytes + sizeof(MusicChunk::data) - 1)
				 / sizeof(MusicChunk::data),
				 buffer.GetSize() / std::size_t{3});

		if (dc.pipe->GetSize() < want_pipe_chunks) {
			/* need more data */
			if (!buffer.IsFull()) {
				decoder_woken = true;
				dc.Signal();
			}

			return false;
		}

		// TODO: scan incrementally until mixrampdb is reached
		auto s = UnlockAnalyzeMixRamp(*dc.pipe, dc.out_audio_format,
					      MixRampDirection::START);
		if (!s.empty()) {
			FmtDebug(player_domain, "Analyzed MixRamp start: {}", s);
			dc.SetMixRampStart(std::move(s));
		}
	}

	return true;
}

bool
Player::OpenOutput() noexcept
{
	assert(play_audio_format.IsDefined());
	assert(pc.state == PlayerState::PLAY ||
	       pc.state == PlayerState::PAUSE);

	try {
		const ScopeUnlock unlock(pc.mutex);
		pc.outputs.Open(play_audio_format);
	} catch (...) {
		auto error = std::current_exception();
		LogError(error);

		output_open = false;

		/* pause: the user may resume playback as soon as an
		   audio output becomes available */
		paused = true;

		pc.SetOutputError(std::move(error));

		return false;
	}

	output_open = true;
	paused = false;

	pc.state = PlayerState::PLAY;
	pc.listener.OnPlayerStateChanged();

	return true;
}

inline bool
Player::CheckDecoderStartup(std::unique_lock<Mutex> &lock) noexcept
{
	assert(decoder_starting);

	if (!ForwardDecoderError()) {
		/* the decoder failed */
		return false;
	} else if (!dc.IsStarting()) {
		/* the decoder is ready and ok */

		decoder_starting = false;

		pc.total_time = real_song_duration(*dc.song,
						   dc.total_time);

		if (pending_seek > SongTime::zero()) {
			assert(pc.seeking);

			bool success = SeekDecoder(lock, pending_seek);
			pc.seeking = false;
			pc.ClientSignal();
			if (!success)
				return false;

			/* re-fill the buffer after seeking */
			buffering = true;
		} else if (pc.seeking) {
			pc.seeking = false;
			pc.ClientSignal();

			/* re-fill the buffer after seeking */
			buffering = true;
		}

		if (output_open)
			/* keep the current output format until SongBorder()
			   switches the pipe to the next song */
			return true;

		pc.audio_format = dc.in_audio_format;
		play_audio_format = dc.out_audio_format;

		const size_t buffer_before_play_size =
			play_audio_format.TimeToSize(buffer_before_play_duration);
		buffer_before_play =
			(buffer_before_play_size + sizeof(MusicChunk::data) - 1)
			/ sizeof(MusicChunk::data);

		pc.listener.OnPlayerStateChanged();

		if (!paused && !OpenOutput()) {
			FmtError(player_domain,
					 "problems opening audio device "
					 "while playing {:?}",
					 dc.song->GetURI());
			return true;
		}

		return true;
	} else {
		/* the decoder is not yet ready; wait
		   some more */
		dc.WaitForDecoder(lock);

		return true;
	}
}

bool
Player::SeekDecoder(std::unique_lock<Mutex> &lock, SongTime seek_time) noexcept
{
	assert(song);
	assert(!decoder_starting);

	if (!pc.total_time.IsNegative()) {
		const SongTime total_time(pc.total_time);
		if (seek_time > total_time)
			seek_time = total_time;
	}

	try {
		const PlayerControl::ScopeOccupied occupied(pc);

		dc.Seek(lock, song->GetStartTime() + seek_time);
	} catch (...) {
		/* decoder failure */
		pc.SetError(PlayerError::DECODER, std::current_exception());
		return false;
	}

	elapsed_time = seek_time;
	return true;
}

inline bool
Player::SeekDecoder(std::unique_lock<Mutex> &lock) noexcept
{
	assert(lock.mutex() == &pc.mutex);
	assert(pc.next_song != nullptr);

	if (pc.seek_time > SongTime::zero() && // TODO: allow this only if the song duration is known
	    dc.IsUnseekableCurrentSong(*pc.next_song)) {
		/* seeking into the current song; but we already know
		   it's not seekable, so let's fail early */
		/* note the seek_time>0 check: if seeking to the
		   beginning, we can simply restart the decoder */
		pc.next_song.reset();
		pc.SetError(PlayerError::DECODER,
			    std::make_exception_ptr(std::runtime_error("Not seekable")));
		pc.CommandFinished();
		return true;
	}

	CancelPendingSeek();

	/*
	 * A user seek invalidates any planned or active cross-fade/automix
	 * transition.  Automix may already have moved xfade_state to ENABLED
	 * or ACTIVE before the seek command arrives; keeping that state across
	 * the seek makes the old transition resume at the wrong song position
	 * and also trips the historical UNKNOWN assertions below.
	 */
	ResetCrossFade();
	cross_fade_tag.reset();

	{
		const ScopeUnlock unlock{lock};
		pc.outputs.Cancel();
	}

	pc.listener.OnPlayerStateChanged();

	if (!dc.IsSeekableCurrentSong(*pc.next_song)) {
		/* the decoder is already decoding the "next" song -
		   stop it and start the previous song again */

		StopDecoder(lock);

		/* clear music chunks which might still reside in the
		   pipe */
		pipe->Clear();

		/* re-start the decoder */
		StartDecoder(lock, pipe, true);
		ActivateDecoder();

		pc.seeking = true;
		pc.CommandFinished();

		if (xfade_state != CrossFadeState::UNKNOWN)
			ResetCrossFade();

		return true;
	} else {
		if (!IsDecoderAtCurrentSong()) {
			/* the decoder is already decoding the "next" song,
			   but it is the same song file; exchange the pipe */
			ReplacePipe(dc.pipe);
		}

		pc.next_song.reset();
		queued = false;

		if (decoder_starting) {
			/* wait for the decoder to complete
			   initialization; postpone the SEEK
			   command */

			pending_seek = pc.seek_time;
			pc.seeking = true;
			pc.CommandFinished();
			return true;
		} else {
			/* send the SEEK command */

			if (!SeekDecoder(lock, pc.seek_time)) {
				pc.CommandFinished();
				return false;
			}
		}
	}

	pc.CommandFinished();

	if (xfade_state != CrossFadeState::UNKNOWN)
		ResetCrossFade();

	/* re-fill the buffer after seeking */
	buffering = true;

	{
		/* call syncPlaylistWithQueue() in the main thread */
		const ScopeUnlock unlock{lock};
		pc.listener.OnPlayerSync();
	}

	return true;
}

inline bool
Player::ProcessCommand(std::unique_lock<Mutex> &lock) noexcept
{
	assert(lock.mutex() == &pc.mutex);

	switch (pc.command) {
	case PlayerCommand::NONE:
		break;

	case PlayerCommand::STOP:
	case PlayerCommand::EXIT:
	case PlayerCommand::CLOSE_AUDIO:
		return false;

	case PlayerCommand::UPDATE_AUDIO:
		{
			const ScopeUnlock unlock{lock};
			pc.outputs.EnableDisable();
		}

		pc.CommandFinished();
		break;

	case PlayerCommand::QUEUE:
		assert(pc.next_song != nullptr);
		assert(!queued);
		assert(!IsDecoderAtNextSong());

		queued = true;
		pc.CommandFinished();

		if (!decoder_starting && dc.IsIdle())
			StartDecoder(lock, std::make_shared<MusicPipe>(),
				     false);

		break;

	case PlayerCommand::PAUSE:
		paused = !paused;
		if (paused) {
			pc.state = PlayerState::PAUSE;

			const ScopeUnlock unlock{lock};
			pc.outputs.Pause();
		} else if (!play_audio_format.IsDefined()) {
			/* the decoder hasn't provided an audio format
			   yet - don't open the audio device yet */
			pc.state = PlayerState::PLAY;
		} else {
			OpenOutput();
		}

		pc.CommandFinished();
		break;

	case PlayerCommand::SEEK:
		return SeekDecoder(lock);

	case PlayerCommand::CANCEL:
		if (pc.next_song == nullptr)
			/* the cancel request arrived too late, we're
			   already playing the queued song...  stop
			   everything now */
			return false;

		if (IsDecoderAtNextSong())
			/* the decoder is already decoding the song -
			   stop it and reset the position */
			StopDecoder(lock);

		pc.next_song.reset();
		queued = false;
		pc.CommandFinished();
		break;

	case PlayerCommand::REFRESH:
		if (output_open && !paused) {
			const ScopeUnlock unlock{lock};
			pc.outputs.CheckPipe();
		}

		if (const auto outputs_time = pc.outputs.GetElapsedTime();
		    !outputs_time.IsNegative())
			pc.elapsed_time = static_cast<SongTime>(outputs_time);
		else
			pc.elapsed_time = elapsed_time;

		pc.CommandFinished();
		break;
	}

	return true;
}

inline void
Player::CheckCrossFade() noexcept
{
	if (xfade_state != CrossFadeState::UNKNOWN)
		/* already decided */
		return;

	if (pc.border_pause) {
		/* no cross-fading if MPD is going to pause at the end
		   of the current song */
		return;
	}

	const auto chunk_duration =
		play_audio_format.SizeToTime<FloatDuration>(sizeof(MusicChunk::data));

	if (pc.GetAutomix()) {
		if (song == nullptr || !queued || pc.next_song == nullptr)
			return;

		/*
		 * Automix must decide before the next decoder has necessarily
		 * started.  The normal crossfade path waits for dc.pipe to contain
		 * decoded audio from the next song, but automix uses tag/beat data and
		 * can choose the transition point from pc.next_song alone.  Waiting for
		 * IsDecoderAtNextSong() here creates a dead zone for songs with long
		 * fade-outs: the next decoder is not started until the current song is
		 * over, so no transition can happen.
		 */
		FmtDebug(player_domain,
			 "automix check-crossfade current={:?} next={:?} play_format={} next_out_format={} pipe_size={} next_pipe_size={} buffer_before_play={} decoder_next={}",
			 song->GetURI(), pc.next_song->GetURI(), play_audio_format,
			 dc.out_audio_format, pipe->GetSize(),
			 IsDecoderAtNextSong() && dc.pipe != nullptr ? dc.pipe->GetSize() : 0,
			 buffer_before_play, IsDecoderAtNextSong());

		const auto plan = GetAutomixPlan(*song, *pc.next_song);
		if (plan) {
			automix_time_ratio = plan->time_ratio;
			automix_eq_strength = plan->eq_strength;

			const auto song_duration = song->GetDuration();
			if (song_duration.IsNegative())
				return;

			const auto cross_fade_duration =
				std::max(SongTime::zero(), SongTime::Cast(song_duration) - plan->transition_start);
			FmtDebug(player_domain,
				 "automix duration current_duration={} transition_start={} cross_fade_duration={} chunk_duration={}",
				 song_duration.ToDoubleS(), plan->transition_start.ToDoubleS(),
				 cross_fade_duration.ToDoubleS(), chunk_duration.count());
			cross_fade_chunks =
				std::lround(cross_fade_duration.ToDoubleS() /
					    chunk_duration.count());
			const unsigned max_chunks = buffer.GetSize() - buffer_before_play;
			if (cross_fade_chunks > max_chunks)
				cross_fade_chunks = max_chunks;
			if (plan->time_ratio)
				FmtDebug(player_domain,
					 "automix ratio accepted current={:?} next={:?} ratio={} eq_strength={}",
					 song->GetURI(), pc.next_song->GetURI(), *plan->time_ratio,
					 plan->eq_strength);
			else
				FmtDebug(player_domain,
					 "automix ratio rejected current={:?} next={:?} eq_strength={}",
					 song->GetURI(), pc.next_song->GetURI(), plan->eq_strength);
			LogAutomixPlan(*song, *pc.next_song, *plan, cross_fade_chunks);
			if (cross_fade_chunks > 0)
				xfade_state = CrossFadeState::ENABLED;
			else {
				xfade_state = CrossFadeState::DISABLED;
				ResetAutomixTransition();
			}
			return;
		}

		FmtDebug(player_domain,
			 "automix plan unavailable current={:?} next={:?}",
			 song->GetURI(), pc.next_song->GetURI());
		ResetAutomixTransition();
	}

	if (!IsDecoderAtNextSong() || dc.IsStarting() || dc.pipe->IsEmpty())
		/* we need information about the next song before we
		   can decide */
		/* the "pipe.empty" check is here so we wait for all
		   (ReplayGain/MixRamp) metadata to appear, which some
		   decoders parse only after reporting readiness */
		return;

	if (!MixRampScannerReady())
		/* need more chunks for the MixRamp scanner */
		return;

	if (!pc.cross_fade.CanCrossFade(pc.total_time, dc.total_time,
						 dc.out_audio_format,
						 play_audio_format)) {
		/* cross fading is disabled or the next song is too
		   short */
		xfade_state = CrossFadeState::DISABLED;
		ResetAutomixTransition();
		return;
	}

	/* enable cross fading in this song?  if yes, calculate how
	   many chunks will be required for it */
	cross_fade_chunks =
		pc.cross_fade.Calculate(dc.replay_gain_db,
					dc.replay_gain_prev_db,
					dc.GetMixRampStart(),
					dc.GetMixRampPreviousEnd(),
					play_audio_format,
					buffer.GetSize() -
					buffer_before_play);
	if (cross_fade_chunks > 0)
		xfade_state = CrossFadeState::ENABLED;
	else
		xfade_state = CrossFadeState::DISABLED;
	ResetAutomixTransition();
}

inline void
PlayerControl::LockUpdateSongTag(DetachedSong &song,
				 const Tag &new_tag) noexcept
{
	if (song.IsFile())
		/* don't update tags of local files, only remote
		   streams may change tags dynamically */
		return;

	if (new_tag != song.GetTag()) {
		song.SetTag(new_tag);

		LockSetTaggedSong(song);

		/* the main thread will update the playlist version when he
		   receives this event */
		listener.OnPlayerTagModified();
	}
}

inline void
PlayerControl::PlayChunk(DetachedSong &song, MusicChunkPtr chunk,
			 const AudioFormat &format)
{
	assert(chunk->CheckFormat(format));

	if (chunk->tag != nullptr)
		LockUpdateSongTag(song, *chunk->tag);

	if (chunk->IsEmpty())
		return;

	{
		const std::lock_guard lock{mutex};
		bit_rate = chunk->bit_rate;
	}

	/* send the chunk to the audio outputs */

	const double chunk_length(chunk->length);

	outputs.Play(std::move(chunk));
	total_play_time += format.SizeToTime<decltype(total_play_time)>(chunk_length);
}

inline bool
Player::PlayNextChunk() noexcept
{
	if (!pc.LockWaitOutputConsumed(64))
		/* the output pipe is still large enough, don't send
		   another chunk */
		return true;

	/* activate cross-fading? */
	if (xfade_state == CrossFadeState::ENABLED &&
	    IsDecoderAtNextSong() &&
	    pipe->GetSize() <= cross_fade_chunks) {
		/* beginning of the cross fade - adjust
		   cross_fade_chunks which might be bigger than the
		   remaining number of chunks in the old song */
		cross_fade_chunks = pipe->GetSize();
		xfade_state = CrossFadeState::ACTIVE;
	}

	MusicChunkPtr chunk;
	if (xfade_state == CrossFadeState::ACTIVE) {
		/* perform cross fade */

		assert(IsDecoderAtNextSong());

		unsigned cross_fade_position = pipe->GetSize();
		assert(cross_fade_position <= cross_fade_chunks);
		FmtDebug(player_domain,
			 "automix xfade active position={} chunks={} pipe_size={} next_pipe_size={} play_format={} next_out_format={} time_ratio={} eq_strength={}",
			 cross_fade_position, cross_fade_chunks, pipe->GetSize(), dc.pipe->GetSize(),
			 play_audio_format, dc.out_audio_format,
			 automix_time_ratio ? *automix_time_ratio : 1.0f,
			 automix_eq_strength ? *automix_eq_strength : 0.0f);

		const auto *current_peek = pipe->Peek();
		const bool automix_pending_ready = pc.GetAutomix() && automix_eq_strength &&
			current_peek != nullptr && current_peek->length > 0 &&
			automix_eq_incoming.automix_pending.size() >= current_peek->length;
		if (automix_pending_ready)
			FmtDebug(player_domain,
				 "automix using pending-only incoming pending_bytes={} current_bytes={} next_pipe_size={}",
				 automix_eq_incoming.automix_pending.size(), current_peek->length, dc.pipe->GetSize());

		auto other_chunk = automix_pending_ready ? nullptr : dc.pipe->Shift();
		if (other_chunk != nullptr || automix_pending_ready) {
			LogAutomixChunk("xfade-other-shifted", other_chunk.get(), dc.out_audio_format);
			chunk = pipe->Shift();
			assert(chunk != nullptr);
			assert(chunk->other == nullptr);
			LogAutomixChunk("xfade-current-shifted", chunk.get(), play_audio_format);

			/* don't send the tags of the new song (which
			   is being faded in) yet; postpone it until
			   the current song is faded out */
			if (other_chunk != nullptr)
				cross_fade_tag = Tag::Merge(std::move(cross_fade_tag),
						    std::move(other_chunk->tag));

			if (pc.cross_fade.mixramp_delay <= FloatDuration::zero()) {
				chunk->mix_ratio = static_cast<float>(cross_fade_position)
					     / cross_fade_chunks;
			} else {
				chunk->mix_ratio = -1;
			}

			const float automix_phase = 1.0f - static_cast<float>(cross_fade_position)
								    / static_cast<float>(cross_fade_chunks);
			const float automix_next_phase = 1.0f - static_cast<float>(cross_fade_position > 0
										 ? cross_fade_position - 1
										 : 0)
								    / static_cast<float>(cross_fade_chunks);

			if (pc.GetAutomix() && automix_time_ratio) {
				chunk->automix = true;
				chunk->automix_time_ratio = *automix_time_ratio;
				FmtDebug(player_domain, "automix mark-current ratio={}", *automix_time_ratio);
			}

			if (other_chunk && other_chunk->IsEmpty()) {
				FmtDebug(player_domain, "automix drop empty incoming tag-only chunk");
				/* the "other" chunk was a MusicChunk
				   which had only a tag, but no music
				   data - we cannot cross-fade that;
				   but since this happens only at the
				   beginning of the new song, we can
				   easily recover by throwing it away
				   now */
				other_chunk.reset();
			}

			if ((other_chunk || automix_pending_ready) && pc.GetAutomix() && automix_eq_strength) {
				FmtDebug(player_domain, "automix try player-side mix");
				/*
				 * Automix owns the transition completely: convert both sides to
				 * one format, EQ them, apply one sample-accurate fade clock and
				 * emit a plain chunk.  Do not attach chunk->other; the generic
				 * output cross-fader cannot see automix's format/EQ/timing state
				 * and small mismatches there were the source of audible glitches.
				 */
				auto mixed = AutomixMixChunk(buffer, *chunk, other_chunk.get(),
							       play_audio_format, dc.out_audio_format,
							       play_audio_format, *dc.pipe,
							       automix_phase, automix_next_phase,
							       *automix_eq_strength,
							       automix_eq_outgoing,
							       automix_eq_incoming);
				if (mixed != nullptr) {
					FmtDebug(player_domain, "automix player-side mix succeeded");
					chunk = std::move(mixed);
					other_chunk.reset();
				} else
					FmtDebug(player_domain, "automix player-side mix failed; falling back to transformed other");
			}

			if (other_chunk && pc.GetAutomix() && automix_eq_strength) {
				FmtDebug(player_domain, "automix try transform incoming");
				MusicChunkPtr transformed;
#ifdef ENABLE_RUBBERBAND
				if (automix_time_ratio) {
					const float ramped_time_ratio =
						1.0f + (*automix_time_ratio - 1.0f) * automix_phase;
					transformed = AutomixChunk(buffer, *other_chunk,
							   dc.out_audio_format,
							   play_audio_format,
							   ramped_time_ratio,
							   *automix_eq_strength,
							   automix_phase,
							   automix_eq_incoming);
					if (transformed == nullptr)
						transformed = AutomixEqChunk(buffer, *other_chunk,
								dc.out_audio_format,
								play_audio_format,
								automix_phase,
								*automix_eq_strength,
								automix_eq_incoming,
								true);
				} else
					transformed = AutomixEqChunk(buffer, *other_chunk,
							     dc.out_audio_format,
							     play_audio_format,
							     automix_phase,
							     *automix_eq_strength,
							     automix_eq_incoming,
							     true);
#else
				transformed = AutomixEqChunk(buffer, *other_chunk,
							     dc.out_audio_format,
							     play_audio_format,
							     automix_phase,
							     *automix_eq_strength,
							     automix_eq_incoming,
							     true);
#endif
				if (transformed != nullptr) {
					FmtDebug(player_domain, "automix transform incoming succeeded");
					other_chunk = std::move(transformed);
				} else if (automix_time_ratio) {
					FmtDebug(player_domain, "automix transform incoming failed; dropping because ratio path required conversion");
					/*
					 * Automix may cross-fade tracks whose decoder/output
					 * formats differ.  In that case the incoming chunk must
					 * be converted to play_audio_format before it is attached
					 * as chunk->other; otherwise AudioOutputSource will try to
					 * mix two different formats and hit its format assertion.
					 * If both RubberBand and the EQ-only fallback failed, skip
					 * this incoming chunk instead of passing raw decoder PCM.
					 */
					other_chunk.reset();
				}
			}

#ifndef NDEBUG
			if (other_chunk != nullptr &&
			    !other_chunk->CheckFormat(play_audio_format)) {
				FmtDebug(player_domain,
					 "automix dropping incoming chunk with mismatched format");
				other_chunk.reset();
			}
#endif

			LogAutomixChunk("xfade-current-before-output", chunk.get(), play_audio_format);
			LogAutomixChunk("xfade-other-before-output", other_chunk.get(), play_audio_format);
			chunk->other = std::move(other_chunk);
		} else {
			FmtDebug(player_domain,
				 "automix xfade no incoming chunk position={} current_pipe_size={} next_pipe_size={}",
				 cross_fade_position, pipe->GetSize(), dc.pipe->GetSize());
			/* there are not enough decoded chunks yet */

			std::unique_lock lock{pc.mutex};

			if (dc.IsIdle()) {
				/* the decoder isn't running, abort
				   cross fading */
				xfade_state = CrossFadeState::DISABLED;
			} else {
				/* wait for the decoder */
				dc.Signal();
				dc.WaitForDecoder(lock);

				return true;
			}
		}
	}

	if (chunk == nullptr)
		chunk = pipe->Shift();

	assert(chunk != nullptr);

	/* insert the postponed tag if cross-fading is finished */

	if (xfade_state != CrossFadeState::ACTIVE && cross_fade_tag != nullptr) {
		chunk->tag = Tag::Merge(std::move(chunk->tag),
					std::move(cross_fade_tag));
		cross_fade_tag = nullptr;
	}

	/* play the current chunk */

	try {
		pc.PlayChunk(*song, std::move(chunk),
			     play_audio_format);
	} catch (...) {
		auto error = std::current_exception();
		LogError(error);

		chunk.reset();

		/* pause: the user may resume playback as soon as an
		   audio output becomes available */
		paused = true;

		pc.LockSetOutputError(std::move(error));

		return false;
	}

	const std::lock_guard lock{pc.mutex};

	/* this formula should prevent that the decoder gets woken up
	   with each chunk; it is more efficient to make it decode a
	   larger block at a time */
	if (!dc.IsIdle() && dc.pipe->GetSize() <= decoder_wakeup_threshold) {
		if (!decoder_woken) {
			decoder_woken = true;
			dc.Signal();
		}
	} else
		decoder_woken = false;

	return true;
}

inline void
Player::SongBorder() noexcept
{
	{
		const ScopeUnlock unlock(pc.mutex);

		FmtNotice(player_domain, "played {:?}", song->GetURI());

		ReplacePipe(dc.pipe);

		pc.outputs.SongBorder();
	}

	ActivateDecoder();

	pc.audio_format = dc.in_audio_format;
	play_audio_format = dc.out_audio_format;
	const size_t buffer_before_play_size =
		play_audio_format.TimeToSize(buffer_before_play_duration);
	buffer_before_play =
		(buffer_before_play_size + sizeof(MusicChunk::data) - 1)
		/ sizeof(MusicChunk::data);

	const bool border_pause = pc.ApplyBorderPause();
	if (border_pause) {
		const ScopeUnlock unlock(pc.mutex);

		paused = true;

		pc.listener.OnBorderPause();

		/* drain all outputs to guarantee the current song is
		   really being played to the end; without this, the
		   Pause() call would drop all ring buffers */
		pc.outputs.Drain();

		pc.outputs.Pause();
		pc.listener.OnPlayerStateChanged();
	} else if (!paused && !OpenOutput()) {
		FmtError(player_domain,
			 "problems opening audio device while playing {:?}",
			 song->GetURI());
	}
}

inline void
Player::Run() noexcept
{
	pipe = std::make_shared<MusicPipe>();

	std::unique_lock lock{pc.mutex};

	StartDecoder(lock, pipe, true);
	ActivateDecoder();

	pc.state = PlayerState::PLAY;

	pc.CommandFinished();

	while (ProcessCommand(lock)) {
		if (decoder_starting) {
			/* wait until the decoder is initialized completely */

			if (!CheckDecoderStartup(lock))
				break;

			continue;
		}

		if (buffering) {
			/* buffering at the start of the song - wait
			   until the buffer is large enough, to
			   prevent stuttering on slow machines */

			if (pipe->GetSize() < buffer_before_play &&
			    !dc.IsIdle() && !buffer.IsFull()) {
				/* not enough decoded buffer space yet */

				dc.WaitForDecoder(lock);
				continue;
			} else {
				/* buffering is complete */
				buffering = false;
			}
		}

		if (dc.IsIdle() && queued && IsDecoderAtCurrentSong()) {
			/* the decoder has finished the current song;
			   make it decode the next song */

			assert(dc.pipe == nullptr || dc.pipe == pipe);

			StartDecoder(lock, std::make_shared<MusicPipe>(),
				     false);
		}

		if (pc.GetAutomix() && queued && IsDecoderAtCurrentSong() &&
		    dc.IsIdle() && dc.pipe == pipe &&
		    xfade_state == CrossFadeState::ENABLED &&
		    pipe->GetSize() <= cross_fade_chunks + buffer_before_play) {
			/*
			 * Only re-arm the decoder after the current decoder has fully
			 * finished and its current-song pipe is no longer being written.
			 * Starting the next decoder while the current decoder is still
			 * initializing/decoding can swap dc.pipe underneath DecoderControl;
			 * the decoder thread may then call SetReady() with buffered chunks in
			 * the replacement pipe, tripping DecoderControl::SetReady()'s
			 * pipe->IsEmpty() assertion.
			 */
			FmtDebug(player_domain,
				 "automix start next decoder pipe_size={} threshold={}",
				 pipe->GetSize(), cross_fade_chunks + buffer_before_play);
			StartDecoder(lock, std::make_shared<MusicPipe>(),
				     false);
		}

		CheckCrossFade();

		if (paused) {
			if (pc.command == PlayerCommand::NONE)
				pc.Wait(lock);
		} else if (!pipe->IsEmpty()) {
			/* at least one music chunk is ready - send it
			   to the audio output */

			const ScopeUnlock unlock{lock};
			PlayNextChunk();
		} else if (UnlockCheckOutputs() > 0) {
			/* not enough data from decoder, but the
			   output thread is still busy, so it's
			   okay */

			/* wake up the decoder (just in case it's
			   waiting for space in the MusicBuffer) and
			   wait for it */
			// TODO: eliminate this kludge
			dc.Signal();

			dc.WaitForDecoder(lock);
		} else if (IsDecoderAtNextSong()) {
			/* at the beginning of a new song */

			SongBorder();
		} else if (dc.IsIdle()) {
			if (queued)
				/* the decoder has just stopped,
				   between the two IsIdle() checks,
				   probably while UnlockCheckOutputs()
				   left the mutex unlocked; to restart
				   the decoder instead of stopping
				   playback completely, let's re-enter
				   this loop */
				continue;

			/* check the size of the pipe again, because
			   the decoder thread may have added something
			   since we last checked */
			if (pipe->IsEmpty()) {
				/* wait for the hardware to finish
				   playback */
				const ScopeUnlock unlock{lock};
				pc.outputs.Drain();
				break;
			}
		} else if (output_open) {
			/* the decoder is too busy and hasn't provided
			   new PCM data in time: wait for the
			   decoder */

			/* wake up the decoder (just in case it's
			   waiting for space in the MusicBuffer) and
			   wait for it */
			// TODO: eliminate this kludge
			dc.Signal();

			dc.WaitForDecoder(lock);
		}
	}

	CancelPendingSeek();
	StopDecoder(lock);

	pipe.reset();

	cross_fade_tag.reset();

	if (song != nullptr) {
		FmtNotice(player_domain, "played {:?}", song->GetURI());
		song.reset();
	}

	pc.ClearTaggedSong();

	if (queued) {
		assert(pc.next_song != nullptr);
		pc.next_song.reset();
	}

	pc.state = PlayerState::STOP;
}

static void
do_play(PlayerControl &pc, DecoderControl &dc,
	MusicBuffer &buffer) noexcept
{
	Player player(pc, dc, buffer);
	player.Run();
}

void
PlayerControl::RunThread() noexcept
try {
	SetThreadName("player");

	DecoderControl dc(mutex, cond,
			  input_cache,
			  config.audio_format,
			  config.replay_gain);
	dc.StartThread();

	MusicBuffer buffer{config.buffer_chunks};

	std::unique_lock lock{mutex};

	while (true) {
		switch (command) {
		case PlayerCommand::SEEK:
		case PlayerCommand::QUEUE:
			assert(next_song != nullptr);

			{
				const ScopeUnlock unlock{lock};

				/* allocate physical RAM for the whole
				   buffer to reduce page faults
				   later */
				buffer.PopulateMemory();

				do_play(*this, dc, buffer);

				/* give the main thread a chance to
				   queue another song, just in case
				   we've stopped playback
				   spuriously */
				listener.OnPlayerSync();
			}

			break;

		case PlayerCommand::STOP:
			{
				const ScopeUnlock unlock{lock};
				outputs.Cancel();
			}

			/* fall through */
			[[fallthrough]];

		case PlayerCommand::PAUSE:
			next_song.reset();

			CommandFinished();
			break;

		case PlayerCommand::CLOSE_AUDIO:
			{
				const ScopeUnlock unlock{lock};
				outputs.Release();
			}

			CommandFinished();

			assert(buffer.IsEmptyUnsafe());
			buffer.DiscardMemory();

			break;

		case PlayerCommand::UPDATE_AUDIO:
			{
				const ScopeUnlock unlock{lock};
				outputs.EnableDisable();
			}

			CommandFinished();
			break;

		case PlayerCommand::EXIT:
			{
				const ScopeUnlock unlock{lock};
				dc.Quit();
				outputs.Close();
			}

			CommandFinished();
			return;

		case PlayerCommand::CANCEL:
			next_song.reset();

			CommandFinished();
			break;

		case PlayerCommand::REFRESH:
			/* no-op when not playing */
			CommandFinished();
			break;

		case PlayerCommand::NONE:
			Wait(lock);
			break;
		}
	}
} catch (...) {
	/* exceptions caught here are thrown during initialization;
	   the main loop doesn't throw */

	LogError(std::current_exception());

	/* TODO: what now? How will the main thread learn about this
	   failure? */
}
