// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright The Music Player Daemon Project

#include "Playlist.hxx"
#include "PlaylistError.hxx"
#include "db/Interface.hxx"
#include "song/LightSong.hxx"
#include "song/DetachedSong.hxx"
#include "time/ChronoUtil.hxx"
#include "Log.hxx"

static bool
UpdatePlaylistSong(const Database &db, DetachedSong &song)
{
	if (!song.IsInDatabase() || !song.IsFile())
		/* only update Songs instances that are "detached"
		   from the Database */
		return false;

	const LightSong *original;
	try {
		original = db.GetSong(song.GetURI());
	} catch (...) {
		/* not found - shouldn't happen, because the update
		   thread should ensure that all stale Song instances
		   have been purged */
		return false;
	}

	assert(original != nullptr);

	const auto original_added = IsNegative(original->added)
		? original->mtime
		: original->added;

	if (original->mtime == song.GetLastModified() &&
	    original_added == song.GetAdded() &&
	    original->tag == song.GetTag() &&
	    original->bpm == song.GetBpm() &&
	    original->key == song.GetKey() &&
	    original->beats == song.GetBeats()) {
		/* not modified */
		db.ReturnSong(original);
		return false;
	}

	song.SetLastModified(original->mtime);
	song.SetAdded(original->added);
	song.SetTag(original->tag);
	song.SetBpm(original->bpm);
	song.SetKey(original->key);
	song.SetBeats(original->beats);

	FmtDebug(playlist_domain,
		 "updated analysis in live queue {:?}: bpm={} beats={}",
		 song.GetURI(),
		 song.GetBpm().has_value() ? "yes" : "no",
		 song.GetBeats().size());

	db.ReturnSong(original);
	return true;
}

void
playlist::DatabaseModified(const Database &db)
{
	bool modified = false;

	for (unsigned i = 0, n = queue.GetLength(); i != n; ++i) {
		if (UpdatePlaylistSong(db, queue.Get(i))) {
			queue.ModifyAtPosition(i);
			modified = true;
		}
	}

	if (modified)
		OnModified();
}
