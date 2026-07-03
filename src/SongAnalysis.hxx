// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright The Music Player Daemon Project

#ifndef MPD_SONG_ANALYSIS_HXX
#define MPD_SONG_ANALYSIS_HXX

#include "fs/Path.hxx"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

struct Tag;
class ArchiveFile;

struct SongAnalysis final {
	std::optional<double> bpm;
	std::string key;
	std::vector<double> beats;
};

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
