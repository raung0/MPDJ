// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright The Music Player Daemon Project

#ifndef MPD_SONG_ANALYSIS_HXX
#define MPD_SONG_ANALYSIS_HXX

#include "fs/Path.hxx"
#include "fs/AllocatedPath.hxx"
#include "Chrono.hxx"
#include "thread/Cond.hxx"
#include "thread/Mutex.hxx"
#include "thread/Thread.hxx"
#include "tag/Tag.hxx"

#include <chrono>
#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

struct Tag;
class ArchiveFile;
struct Directory;

class SongAnalysisQueue;

struct SongAnalysis final {
	std::optional<double> bpm;
	std::string key;
	std::vector<double> beats;
};

struct SongAnalysisResult final {
	std::string uri;
	std::chrono::system_clock::time_point mtime;
	SongTime start_time;
	SongTime end_time;
	SongAnalysis analysis;
};

class SongAnalysisQueue final {
private:
	struct Task final {
		bool is_path;
		AllocatedPath path_fs;
		std::string uri;
		Tag tag;
		std::optional<double> bpm_override;
		std::chrono::system_clock::time_point mtime;
		SongTime start_time;
		SongTime end_time;
	};

	std::vector<std::unique_ptr<Thread>> workers;
	Mutex mutex;
	Cond cond;
	Cond idle_cond;
	std::deque<Task> queue;
	std::deque<SongAnalysisResult> results;
	std::atomic_bool stop = false;
	unsigned pending = 0;

	void Worker() noexcept;

public:
	explicit SongAnalysisQueue(unsigned worker_count) noexcept;
	~SongAnalysisQueue() noexcept;

	SongAnalysisQueue(const SongAnalysisQueue &) = delete;
	SongAnalysisQueue &operator=(const SongAnalysisQueue &) = delete;

	bool Submit(AllocatedPath path_fs, std::string uri, const Tag &tag,
		    const std::optional<double> &bpm_override,
		    std::chrono::system_clock::time_point mtime,
		    SongTime start_time, SongTime end_time) noexcept;

	bool Submit(std::string uri, const Tag &tag,
		    const std::optional<double> &bpm_override,
		    std::chrono::system_clock::time_point mtime,
		    SongTime start_time, SongTime end_time) noexcept;

	void WaitIdle() noexcept;
	void Stop() noexcept;
	bool IsStopped() const noexcept {
		return stop;
	}

	std::vector<SongAnalysisResult> CollectResults() noexcept;
};

void
SetSongAnalysisQueue(SongAnalysisQueue *queue) noexcept;

SongAnalysisQueue *
GetSongAnalysisQueue() noexcept;

bool
ApplySongAnalysisResults(Directory &root,
			 const std::vector<SongAnalysisResult> &results) noexcept;

bool
AnalyzeSong(Path path_fs, const Tag &tag,
		const std::optional<double> &bpm_override,
		SongAnalysis &analysis) noexcept;

bool
AnalyzeArchive(ArchiveFile &archive, std::string_view path,
		 const Tag &tag,
		 const std::optional<double> &bpm_override,
		 SongAnalysis &analysis) noexcept;

bool
AnalyzeUri(std::string_view uri, const Tag &tag,
	  const std::optional<double> &bpm_override,
	  SongAnalysis &analysis) noexcept;

#endif
