// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright The Music Player Daemon Project

#include "song/DetachedSong.hxx"
#include "db/Features.hxx" // for ENABLE_DATABASE
#include "db/plugins/simple/Song.hxx"
#include "db/plugins/simple/Directory.hxx"
#include "storage/StorageInterface.hxx"
#include "storage/FileInfo.hxx"
#include "input/InputStream.hxx"
#include "input/WaitReady.hxx"
#include "decoder/DecoderList.hxx"
#include "protocol/Verify.hxx"
#include "fs/AllocatedPath.hxx"
#include "fs/FileInfo.hxx"
#include "thread/Mutex.hxx"
#include "tag/Builder.hxx"
#include "SongAnalysis.hxx"
#include "TagFile.hxx"
#include "TagStream.hxx"
#include "util/UriExtract.hxx"

#include "archive/Features.h" // for ENABLE_ARCHIVE
#ifdef ENABLE_ARCHIVE
#include "TagArchive.hxx"
#endif

#include <cassert>
#include <utility>

#include <string.h>

#ifdef ENABLE_DATABASE

bool
Song::IsPluginAvailable() const noexcept
{
	const char *suffix = GetFilenameSuffix();
	return suffix != nullptr &&
		decoder_plugins_supports_suffix(suffix);
}

SongPtr
Song::LoadFile(Storage &storage, std::string_view path_utf8,
	       const StorageFileInfo &info, Directory &parent)
{
	assert(!uri_has_scheme(path_utf8));
	assert(VerifyRelativePathUTF8(path_utf8));

	auto song = std::make_unique<Song>(path_utf8, parent);
	if (!song->UpdateFile(storage, info))
		return nullptr;

	return song;
}

#endif

#ifdef ENABLE_DATABASE

bool
Song::UpdateFile(Storage &storage, const StorageFileInfo &info)
{
	assert(info.IsRegular());

	const auto &relative_uri = GetURI();
	const auto path_fs = storage.MapFS(relative_uri);

	TagBuilder tag_builder;
	auto new_audio_format = AudioFormat::Undefined();
	SongAnalysis analysis;

	try {
		if (path_fs.IsNull()) {
			Mutex mutex;
			const auto is = storage.OpenFile(relative_uri, mutex);
			LockWaitReady(*is);
			if (!tag_stream_scan(*is, tag_builder,
					     &new_audio_format))
				return false;
		} else {
			if (!ScanFileTagsWithGeneric(path_fs, tag_builder,
						     &new_audio_format))
				return false;
		}
	} catch (...) {
		// TODO: log or propagate I/O errors?
		return false;
	}

	const auto old_mtime = mtime;
	mtime = info.mtime;
	audio_format = new_audio_format;
	const auto bpm_override = tag_builder.GetBpm();
	tag = tag_builder.Commit();
	const bool already_analyzed = bpm.has_value() && !key.empty() && !beats.empty() && old_mtime == info.mtime;
	if (already_analyzed) {
		/* keep existing analysis */
	} else if (auto *analysis_queue = GetSongAnalysisQueue(); analysis_queue != nullptr) {
		const bool queued = path_fs.IsNull()
			? analysis_queue->Submit(std::string(relative_uri), tag, bpm_override,
						 mtime, start_time, end_time)
			: analysis_queue->Submit(path_fs, std::string(relative_uri), tag,
						 bpm_override, mtime, start_time, end_time);
		if (queued) {
			bpm.reset();
			key.clear();
			beats.clear();
		}
	} else {
		if (path_fs.IsNull())
			AnalyzeUri(relative_uri, tag, bpm_override, analysis);
		else
			AnalyzeSong(path_fs, tag, bpm_override, analysis);
		bpm = analysis.bpm;
		key = std::move(analysis.key);
		beats = std::move(analysis.beats);
	}
	return true;
}

#ifdef ENABLE_ARCHIVE

SongPtr
Song::LoadFromArchive(ArchiveFile &archive, std::string_view name_utf8,
		      Directory &parent) noexcept
{
	assert(!uri_has_scheme(name_utf8));
	assert(VerifyRelativePathUTF8(name_utf8));

	auto song = std::make_unique<Song>(name_utf8, parent);
	if (!song->UpdateFileInArchive(archive))
		return nullptr;

	return song;
}

bool
Song::UpdateFileInArchive(ArchiveFile &archive) noexcept
{
	assert(parent.device == DEVICE_INARCHIVE);

	std::string path_utf8(filename);

	for (const Directory *directory = &parent;
	     directory->parent != nullptr &&
		     directory->parent->device == DEVICE_INARCHIVE;
	     directory = directory->parent) {
		path_utf8.insert(path_utf8.begin(), '/');
		path_utf8.insert(0, directory->GetName());
	}

	TagBuilder tag_builder;
	SongAnalysis analysis;
	if (!tag_archive_scan(archive, path_utf8.c_str(), tag_builder))
		return false;

	const auto bpm_override = tag_builder.GetBpm();
	tag = tag_builder.Commit();
	AnalyzeArchive(archive, path_utf8, tag, bpm_override, analysis);
	bpm = analysis.bpm;
	key = std::move(analysis.key);
	beats = std::move(analysis.beats);
	return true;
}

#endif

#endif /* ENABLE_DATABASE */

bool
DetachedSong::LoadFile(Path path)
{
	const FileInfo fi(path);
	if (!fi.IsRegular())
		return false;

	TagBuilder tag_builder;
	auto new_audio_format = AudioFormat::Undefined();
	SongAnalysis analysis;

	try {
		if (!ScanFileTagsWithGeneric(path, tag_builder, &new_audio_format))
			return false;
	} catch (...) {
		// TODO: log or propagate I/O errors?
		return false;
	}

	const auto old_mtime = mtime;
	mtime = fi.GetModificationTime();
	audio_format = new_audio_format;
	const auto bpm_override = tag_builder.GetBpm();
	tag = tag_builder.Commit();
	if (bpm.has_value() && !key.empty() && !beats.empty() && old_mtime == mtime) {
		/* keep existing analysis */
	} else {
		AnalyzeSong(path, tag, bpm_override, analysis);
		bpm = analysis.bpm;
		key = std::move(analysis.key);
		beats = std::move(analysis.beats);
	}
	return true;
}

bool
DetachedSong::Update()
{
	if (IsAbsoluteFile()) {
		const AllocatedPath path_fs =
			AllocatedPath::FromUTF8Throw(GetRealURI());

		return LoadFile(path_fs);
	} else if (IsRemote()) {
		TagBuilder tag_builder;
		auto new_audio_format = AudioFormat::Undefined();
		SongAnalysis analysis;

		try {
			if (!tag_stream_scan(uri, tag_builder,
					     &new_audio_format))
				return false;
		} catch (...) {
			// TODO: log or propagate I/O errors?
			return false;
		}

		const auto old_mtime = mtime;
		mtime = std::chrono::system_clock::time_point::min();
		audio_format = new_audio_format;
		const auto bpm_override = tag_builder.GetBpm();
		tag = tag_builder.Commit();
		if (bpm.has_value() && !key.empty() && !beats.empty() && old_mtime == mtime) {
			/* keep existing analysis */
		} else {
			AnalyzeUri(uri, tag, bpm_override, analysis);
			bpm = analysis.bpm;
			key = std::move(analysis.key);
			beats = std::move(analysis.beats);
		}
		return true;
	} else
		// TODO: implement
		return false;
}
