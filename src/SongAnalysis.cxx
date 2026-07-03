// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright The Music Player Daemon Project

#include "SongAnalysis.hxx"

#include "archive/ArchiveFile.hxx"
#include "db/DatabaseLock.hxx"
#include "db/plugins/simple/Directory.hxx"
#include "db/plugins/simple/Song.hxx"
#include "decoder/Client.hxx"
#include "decoder/DecoderList.hxx"
#include "decoder/DecoderPlugin.hxx"
#include "fs/Traits.hxx"
#include "input/InputStream.hxx"
#include "input/LocalOpen.hxx"
#include "pcm/Convert.hxx"
#include "tag/Builder.hxx"
#include "tag/Tag.hxx"
#include "util/CNumberParser.hxx"
#include "util/SpanCast.hxx"
#include "util/StringCompare.hxx"
#include "util/UriExtract.hxx"
#include "thread/Mutex.hxx"
#include "thread/Cond.hxx"

#include <aubio/aubio.h>
#include <keyfinder/keyfinder.h>

#include <algorithm>
#include <deque>
#include <cstring>
#include <exception>
#include <memory>
#include <stdexcept>
#include <thread>
#include <utility>

using std::string_view_literals::operator""sv;

static SongAnalysisQueue *current_song_analysis_queue = nullptr;

void
SetSongAnalysisQueue(SongAnalysisQueue *queue) noexcept
{
	current_song_analysis_queue = queue;
}

SongAnalysisQueue *
GetSongAnalysisQueue() noexcept
{
	return current_song_analysis_queue;
}

SongAnalysisQueue::SongAnalysisQueue(unsigned worker_count) noexcept
{
	if (worker_count == 0)
		worker_count = 2;

	workers.reserve(worker_count);
	for (unsigned i = 0; i < worker_count; ++i)
		workers.emplace_back(std::make_unique<Thread>(BIND_THIS_METHOD(Worker)));
	for (auto &worker : workers)
		worker->Start();
}

SongAnalysisQueue::~SongAnalysisQueue() noexcept
{
	Stop();

	for (auto &worker : workers)
		worker->Join();
}

bool
SongAnalysisQueue::Submit(AllocatedPath path_fs, std::string uri, const Tag &tag,
			  const std::optional<double> &bpm_override,
			  std::chrono::system_clock::time_point mtime,
			  SongTime start_time, SongTime end_time) noexcept
{
	const std::lock_guard lock{mutex};
	if (stop)
		return false;
	queue.emplace_back(Task{true, std::move(path_fs), std::move(uri), tag,
				 bpm_override, mtime, start_time, end_time});
	++pending;
	cond.notify_one();
	return true;
}

bool
SongAnalysisQueue::Submit(std::string uri, const Tag &tag,
			  const std::optional<double> &bpm_override,
			  std::chrono::system_clock::time_point mtime,
			  SongTime start_time, SongTime end_time) noexcept
{
	const std::lock_guard lock{mutex};
	if (stop)
		return false;
	queue.emplace_back(Task{false, AllocatedPath{nullptr}, std::move(uri), tag,
				 bpm_override, mtime, start_time, end_time});
	++pending;
	cond.notify_one();
	return true;
}

void
SongAnalysisQueue::WaitIdle() noexcept
{
	std::unique_lock lock{mutex};
	idle_cond.wait(lock, [this]{ return pending == 0 || stop; });
}

void
SongAnalysisQueue::Stop() noexcept
{
	const std::lock_guard lock{mutex};
	stop = true;
	pending -= queue.size();
	queue.clear();
	cond.notify_all();
	idle_cond.notify_all();
}

std::vector<SongAnalysisResult>
SongAnalysisQueue::CollectResults() noexcept
{
	const std::lock_guard lock{mutex};
	std::vector<SongAnalysisResult> out;
	out.reserve(results.size());
	while (!results.empty()) {
		out.push_back(std::move(results.front()));
		results.pop_front();
	}
	return out;
}

void
SongAnalysisQueue::Worker() noexcept
{
	for (;;) {
		Task task{false, AllocatedPath{nullptr}, {}, {}, {}, {}, {}, {}};
		bool ok = false;
		SongAnalysis analysis;
		{
			std::unique_lock lock{mutex};
			cond.wait(lock, [this]{ return stop || !queue.empty(); });
			if (stop && queue.empty())
				return;

			task = std::move(queue.front());
			queue.pop_front();
		}

		try {
			ok = task.is_path
				? AnalyzeSong(task.path_fs, task.tag, task.bpm_override, analysis)
				: AnalyzeUri(task.uri, task.tag, task.bpm_override, analysis);
		} catch (...) {
		}

		{
			const std::lock_guard lock{mutex};
			if (ok) {
				try {
					results.push_back(SongAnalysisResult{
						std::move(task.uri), task.mtime,
						task.start_time, task.end_time,
						std::move(analysis),
					});
				} catch (...) {
					ok = false;
				}
			}

			if (--pending == 0)
				idle_cond.notify_all();
		}
	}
}

namespace {

static constexpr AudioFormat analysis_format{44100, SampleFormat::FLOAT, 1};

static std::optional<double>
ParseBpmString(std::string_view value) noexcept
{
	char *end = nullptr;
	const std::string value_s(value);
	const double bpm = ParseDouble(value_s.c_str(), &end);
	if (end != value_s.c_str() + value_s.size())
		return std::nullopt;

	return bpm;
}

static std::optional<double>
GetTaggedBpm(const Tag &tag) noexcept
{
	for (const auto &item : tag) {
		if (item.type == TAG_COMMENT) {
			if (const char *prefix = StringAfterPrefixIgnoreCase(item.value, "BPM_FLOAT="sv);
			    prefix != nullptr && *prefix != '\0') {
				if (const auto bpm = ParseBpmString(prefix);
				    bpm.has_value())
					return bpm;
			}
		}
	}

	return std::nullopt;
}

static std::string
KeyToString(KeyFinder::key_t key)
{
	switch (key) {
	case KeyFinder::A_MAJOR: return "A major";
	case KeyFinder::A_MINOR: return "A minor";
	case KeyFinder::B_FLAT_MAJOR: return "B flat major";
	case KeyFinder::B_FLAT_MINOR: return "B flat minor";
	case KeyFinder::B_MAJOR: return "B major";
	case KeyFinder::B_MINOR: return "B minor";
	case KeyFinder::C_MAJOR: return "C major";
	case KeyFinder::C_MINOR: return "C minor";
	case KeyFinder::D_FLAT_MAJOR: return "D flat major";
	case KeyFinder::D_FLAT_MINOR: return "D flat minor";
	case KeyFinder::D_MAJOR: return "D major";
	case KeyFinder::D_MINOR: return "D minor";
	case KeyFinder::E_FLAT_MAJOR: return "E flat major";
	case KeyFinder::E_FLAT_MINOR: return "E flat minor";
	case KeyFinder::E_MAJOR: return "E major";
	case KeyFinder::E_MINOR: return "E minor";
	case KeyFinder::F_MAJOR: return "F major";
	case KeyFinder::F_MINOR: return "F minor";
	case KeyFinder::G_FLAT_MAJOR: return "G flat major";
	case KeyFinder::G_FLAT_MINOR: return "G flat minor";
	case KeyFinder::G_MAJOR: return "G major";
	case KeyFinder::G_MINOR: return "G minor";
	case KeyFinder::A_FLAT_MAJOR: return "A flat major";
	case KeyFinder::A_FLAT_MINOR: return "A flat minor";
	case KeyFinder::SILENCE: return {};
	}

	return {};
}

class AnalysisDecoderClient final : public DecoderClient {
	bool ready = false;
	std::unique_ptr<PcmConvert> convert;
	std::vector<float> samples;

protected:
	std::exception_ptr error;

public:
	Mutex mutex;

	void Reset() noexcept
	{
		ready = false;
		convert.reset();
		samples.clear();
		error = {};
	}

	bool IsReady() const noexcept {
		return ready && !error;
	}

	const std::vector<float> &GetSamples() const noexcept {
		return samples;
	}

	void Ready(AudioFormat audio_format,
		   bool, [[maybe_unused]] SignedSongTime duration) noexcept override
	{
		try {
			if (audio_format != analysis_format)
				convert = std::make_unique<PcmConvert>(audio_format,
										analysis_format);
			ready = true;
		} catch (...) {
			error = std::current_exception();
		}
	}

	DecoderCommand GetCommand() noexcept override {
		return error ? DecoderCommand::STOP : DecoderCommand::NONE;
	}

	void CommandFinished() noexcept override {}

	SongTime GetSeekTime() noexcept override {
		return SongTime::zero();
	}

	uint64_t GetSeekFrame() noexcept override {
		return 0;
	}

	void SeekError(std::exception_ptr &&e={}) noexcept override {
		error = e ? std::move(e)
			: std::make_exception_ptr(std::runtime_error{"analysis seek failed"});
	}

	InputStreamPtr OpenUri(std::string_view uri) override
	{
		return InputStream::OpenReady(uri, mutex);
	}

	size_t Read(InputStream &is, std::span<std::byte> dest) noexcept override
	{
		try {
			return is.LockRead(dest);
		} catch (...) {
			error = std::current_exception();
			return 0;
		}
	}

	void SubmitTimestamp(FloatDuration) noexcept override {}

	DecoderCommand SubmitAudio(InputStream *,
				   std::span<const std::byte> audio,
				   uint16_t) noexcept override
	{
		try {
			if (convert)
				audio = convert->Convert(audio);

			const auto floats = FromBytesStrict<const float>(audio);
			samples.insert(samples.end(), floats.begin(), floats.end());
		} catch (...) {
			error = std::current_exception();
		}

		return GetCommand();
	}

	DecoderCommand SubmitTag(InputStream *, Tag &&) noexcept override
	{
		return GetCommand();
	}

	void SubmitReplayGain(const ReplayGainInfo *) noexcept override {}
	void SubmitMixRamp(MixRampInfo &&) noexcept override {}
};

static void
AnalyzeSamples(const std::vector<float> &samples,
		  const std::optional<double> &bpm_override,
		  SongAnalysis &analysis)
{
	analysis.beats.clear();
	analysis.key.clear();
	analysis.bpm.reset();

	if (samples.empty())
		return;

	constexpr uint_t win_s = 1024;
	constexpr uint_t hop_s = 512;

	const uint_t samplerate = analysis_format.sample_rate;
	aubio_tempo_t *tempo = new_aubio_tempo("default", win_s, hop_s, samplerate);
	fvec_t *input = new_fvec(hop_s);
	fvec_t *beat = new_fvec(1);

	if (!tempo || !input || !beat) {
		if (tempo)
			del_aubio_tempo(tempo);
		if (input)
			del_fvec(input);
		if (beat)
			del_fvec(beat);
		return;
	}

	for (std::size_t offset = 0; offset < samples.size(); offset += hop_s) {
		if (auto *queue = GetSongAnalysisQueue(); queue != nullptr && queue->IsStopped())
			break;

		const std::size_t n = std::min<std::size_t>(hop_s, samples.size() - offset);
		fvec_zeros(input);
		for (std::size_t i = 0; i < n; ++i)
			input->data[i] = samples[offset + i];

		aubio_tempo_do(tempo, input, beat);
		if (aubio_tempo_was_tatum(tempo) == 2)
			analysis.beats.push_back(double(aubio_tempo_get_last_s(tempo)));
	}

	if (bpm_override.has_value() && *bpm_override > 0.0) {
		analysis.bpm = bpm_override;
	} else {
		const double bpm = double(aubio_tempo_get_bpm(tempo));
		if (bpm > 0.0)
			analysis.bpm = bpm;
	}

	if (auto *queue = GetSongAnalysisQueue(); queue != nullptr && queue->IsStopped()) {
		del_aubio_tempo(tempo);
		del_fvec(input);
		del_fvec(beat);
		return;
	}

	KeyFinder::AudioData audio;
	audio.setFrameRate(samplerate);
	audio.setChannels(1);
	audio.addToSampleCount(samples.size());
	for (std::size_t i = 0; i < samples.size(); ++i)
		audio.setSample(i, samples[i]);

	KeyFinder::KeyFinder k;
	const auto key = k.keyOfAudio(audio);
	analysis.key = KeyToString(key);

	del_aubio_tempo(tempo);
	del_fvec(input);
	del_fvec(beat);
}

static bool
AnalyzePlugin(const DecoderPlugin &plugin, Path path_fs,
		 const std::optional<double> &bpm_override,
		 SongAnalysis &analysis) noexcept
{
	AnalysisDecoderClient client;

	auto run = [&](auto &&decode) noexcept {
		client.Reset();
		try {
			decode();
		} catch (...) {
			client.SeekError(std::current_exception());
		}

		if (!client.IsReady())
			return false;

		AnalyzeSamples(client.GetSamples(), bpm_override, analysis);
		return true;
	};

	if (plugin.file_decode != nullptr && run([&]{ plugin.FileDecode(client, path_fs); }))
		return true;

	if (plugin.stream_decode != nullptr) {
		Mutex mutex;
		auto is = OpenLocalInputStream(path_fs, mutex);
		if (run([&]{ plugin.StreamDecode(client, *is); }))
			return true;
	}

	return false;
}

static bool
AnalyzeStreamPlugin(const DecoderPlugin &plugin, std::string_view uri,
		   const std::optional<double> &bpm_override,
		   SongAnalysis &analysis) noexcept
{
	if (plugin.stream_decode == nullptr)
		return false;

	AnalysisDecoderClient client;
	client.Reset();

	try {
		Mutex mutex;
		auto is = InputStream::OpenReady(uri, mutex);
		plugin.StreamDecode(client, *is);
	} catch (...) {
		client.SeekError(std::current_exception());
	}

	if (!client.IsReady())
		return false;

	AnalyzeSamples(client.GetSamples(), bpm_override, analysis);
	return true;
}

static bool
AnalyzePathImpl(Path path_fs, const std::optional<double> &bpm_override,
		SongAnalysis &analysis) noexcept
{
	const auto *suffix_fs = path_fs.GetExtension();
	if (suffix_fs == nullptr)
		return false;

	const auto suffix = Path::FromFS(suffix_fs).ToUTF8();
	if (suffix.empty())
		return false;

	for (const auto &plugin : GetEnabledDecoderPlugins()) {
		if (!plugin.SupportsSuffix(suffix))
			continue;

		if (AnalyzePlugin(plugin, path_fs, bpm_override, analysis))
			return true;
	}

	return false;
}

static bool
AnalyzeUriImpl(std::string_view uri, const std::optional<double> &bpm_override,
		 SongAnalysis &analysis) noexcept
{
	const auto suffix = uri_get_suffix(uri);

	for (const auto &plugin : GetEnabledDecoderPlugins()) {
		if (!suffix.empty() && !plugin.SupportsSuffix(suffix) && !plugin.SupportsUri(uri))
			continue;

		if (AnalyzeStreamPlugin(plugin, uri, bpm_override, analysis))
			return true;
	}

	return false;
}

} // namespace

bool
AnalyzeSong(Path path_fs, const Tag &tag,
	   const std::optional<double> &bpm_override,
	   SongAnalysis &analysis) noexcept
{
	const auto _bpm = bpm_override.has_value() ? bpm_override
		: GetTaggedBpm(tag);
	try {
		return AnalyzePathImpl(path_fs, _bpm, analysis);
	} catch (...) {
		return false;
	}
}

bool
AnalyzeArchive(ArchiveFile &archive, std::string_view path,
		 const Tag &tag,
		 const std::optional<double> &bpm_override,
		 SongAnalysis &analysis) noexcept
{
	const auto _bpm = bpm_override.has_value() ? bpm_override
		: GetTaggedBpm(tag);
	try {
		AnalysisDecoderClient client;
		const std::string path_s(path);
		const auto suffix = PathTraitsUTF8::GetFilenameSuffix(path_s.c_str());
		if (suffix == nullptr)
			return false;

		const auto suffix_utf8 = Path::FromFS(suffix).ToUTF8();
		for (const auto &plugin : GetEnabledDecoderPlugins()) {
			if (!plugin.SupportsSuffix(suffix_utf8))
				continue;

			if (plugin.stream_decode == nullptr)
				continue;

			client.Reset();
			Mutex mutex;
			auto is = archive.OpenStream(path_s.c_str(), mutex);
			plugin.StreamDecode(client, *is);

			if (client.IsReady()) {
				AnalyzeSamples(client.GetSamples(), _bpm, analysis);
				return true;
			}
		}
	} catch (...) {
	}

	return false;
}

bool
AnalyzeUri(std::string_view uri, const Tag &tag,
	  const std::optional<double> &bpm_override,
	  SongAnalysis &analysis) noexcept
{
	const auto _bpm = bpm_override.has_value() ? bpm_override
		: GetTaggedBpm(tag);
	try {
		return AnalyzeUriImpl(uri, _bpm, analysis);
	} catch (...) {
		return false;
	}
}

static bool
ApplySongAnalysisResult(Directory &root, const SongAnalysisResult &result) noexcept
{
	const auto lr = root.LookupDirectory(result.uri);
	Song *song = lr.directory->FindSong(lr.rest);
	if (song == nullptr)
		return false;

	if (song->mtime != result.mtime ||
	    song->start_time != result.start_time ||
	    song->end_time != result.end_time)
		return false;

	song->bpm = result.analysis.bpm;
	song->key = result.analysis.key;
	song->beats = result.analysis.beats;
	return true;
}

bool
ApplySongAnalysisResults(Directory &root,
			 const std::vector<SongAnalysisResult> &results) noexcept
{
	bool modified = false;
	const ScopeDatabaseLock protect;
	for (const auto &result : results)
		modified |= ApplySongAnalysisResult(root, result);
	return modified;
}
