/*
	Scrolls of Diablo: sprite sheets for the first-person view.
	Copyright (C) 2026  The Scrolls of Diablo contributors

	This program is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	This program is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with this program.  If not, see <https://www.gnu.org/licenses/>.
*/

#pragma once

#include <condition_variable>
#include <mutex>

namespace d2gl::modules {

// One frame of a sheet. Sizes and offsets are in sheet pixels, relative to the unit's feet.
struct SpriteCell {
	bool valid = false;
	float u0 = 0, v0 = 0, u1 = 0, v1 = 0; // v0 = top of the picture
	float cx = 0, cy = 0;                 // centre of the picture: right of / above the feet
	float w = 0, h = 0;
};

struct Sprite {
	GLuint texture = 0;
	int directions = 1, frames = 1;
	std::vector<SpriteCell> cells; // directions * frames, a direction after another
	uint8_t dir64[64] = { 0 };     // game direction (0..63) -> direction row
	float iso_yaw = 3.926991f;     // the yaw of the isometric camera the art was drawn for
	float fps = 12.5f;
	float top = 60.0f;             // pixels from the feet to the top of the tallest frame
	std::string mode;              // the mode actually drawn (a missing one is served by a substitute)

	inline const SpriteCell& cell(int direction, int frame) const
	{
		direction = glm::clamp(direction, 0, directions - 1);
		frame = glm::clamp(frame, 0, frames - 1);
		return cells[direction * frames + frame];
	}
};

enum class SpriteState {
	Loading, // asked for; nothing to draw yet
	Late,    // still not there after a while (the sprite service may not be running)
	Ready,
	Failed,  // the game has no such graphic
};

struct ObjectInfo {
	std::string token;
	bool draw = true;
	int frames[8] = { 0 };
	int delta[8] = { 0 };
	bool cycle[8] = { false };
};

struct MissileInfo {
	std::string cel;
	bool additive = false;
	int anim_rate = 1024;
};

// Sheets are cut from the player's own game archives by tools/art/sprite_service.py (Scrolls of
// Diablo). This class asks for them through request files, reads what arrives on a worker thread
// and hands out ready textures. Everything except the worker runs on the render thread.
class SpriteStore {
	struct Entry {
		SpriteState state = SpriteState::Loading;
		Sprite sprite;
		DWORD first_asked = 0, next_poll = 0;
		bool job_queued = false;
	};
	struct Job {
		std::string key, kind, token, mode;
		int act = 1;
	};
	struct Result {
		std::string key;
		int outcome = 0; // 0 not there yet, 1 loaded, 2 failed
		Sprite sprite;
		uint8_t* pixels = nullptr;
		int width = 0, height = 0;
	};

	std::unordered_map<std::string, Entry> m_entries;

	std::thread m_worker;
	bool m_worker_started = false;
	std::mutex m_lock;
	std::condition_variable m_wake;
	std::deque<Job> m_jobs;       // guarded by m_lock
	std::vector<Result> m_results; // guarded by m_lock
	bool m_quit = false;

	// lookup tables, written by the worker before m_index_ready is set and read-only afterwards
	volatile bool m_index_ready = false;
	std::vector<std::string> m_monster_tokens;
	std::unordered_map<uint32_t, ObjectInfo> m_objects;
	std::unordered_map<uint32_t, MissileInfo> m_missiles;
	std::unordered_map<std::string, std::string> m_item_flippies;

	SpriteStore() = default;
	~SpriteStore();

	void workerMain();
	bool loadIndexes();
	Result runJob(const Job& job);

public:
	static SpriteStore& Instance()
	{
		static SpriteStore instance;
		return instance;
	}

	// Once per frame: start the worker, upload sheets that finished loading.
	void pump();
	inline bool indexReady() const { return m_index_ready; }

	// The sheet of a graphic in a mode ("NU", "WL", ...) with an act's palette (1..5).
	SpriteState get(const char* kind, const std::string& token, const char* mode, int act, const Sprite** sprite);

	const std::string* monsterToken(uint32_t txt) const;
	const ObjectInfo* object(uint32_t txt) const;
	const MissileInfo* missile(uint32_t txt) const;
	const std::string* itemFlippy(const std::string& code) const;
};

}
