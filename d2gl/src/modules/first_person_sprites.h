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

// Where a hand holds an inventory picture (tools/assets/held.py), in picture pixels, y down.
struct HeldShape {
	bool valid = false;
	glm::vec2 axis = { 0.0f, -1.0f }; // unit vector from the bottom end to the top end
	glm::vec2 line[17];               // centre line, bottom end (0) to top end (16)
	float guard = -1.0f;              // crossguard, 0..1 from the bottom; < 0 none
	glm::vec2 center = { 0.0f, 0.0f };

	glm::vec2 pointAt(float f) const
	{
		const float pos = glm::clamp(f, 0.0f, 1.0f) * 16.0f;
		const int k = glm::min((int)pos, 15);
		return line[k] + (line[k + 1] - line[k]) * (pos - (float)k);
	}
};

struct Sprite {
	GLuint texture = 0;
	int atlas_w = 0, atlas_h = 0;  // the sheet's size in pixels
	HeldShape held;                // inventory pictures only
	int directions = 1, frames = 1;
	std::vector<SpriteCell> cells; // directions * frames, a direction after another
	uint8_t dir64[64] = { 0 };     // game direction (0..63) -> direction row
	float iso_yaw = 3.926991f;     // the yaw of the isometric camera the art was drawn for
	float fps = 12.5f;
	float top = 60.0f;             // pixels from the feet to the top of the tallest frame
	float bottom = 0.0f;           // pixels the lowest frame reaches below the feet (a ring on the ground)
	glm::vec3 tint = { 1.0f, 1.0f, 1.0f }; // the picture's colour, brightest channel 1 (overlay sheets only)
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

struct MonsterInfo {
	std::string token;
	std::string name;              // as the game shows it (Akara, Deckard Cain), for the use prompt
	std::string kind = "monster"; // "player" for a monster drawn from a character's graphics
	float shade = 1.0f;            // an Assassin's shadow is her own figure, dark
	bool hidden = false;           // something the game never shows (a spawner, a vine under the ground)
	bool ally = false;             // fights on the player's side: a summoned creature, a trap, a hireling
	int overlay_height = 2;        // monstats2 OverlayHeight: which of an overlay's four heights fits it
};

// A picture the game lays over a unit (overlay.txt): an aura, a curse, a shout, a cast flash.
struct OverlayInfo {
	std::string token;
	float fps = 25.0f;
	float loop_wait = 0.0f;   // seconds between loops
	int trans = 3;            // the game's draw mode: 5 normal, 0-2 see-through, 3 and up glowing
	bool pre_draw = false;    // drawn behind the unit
	float xoff = 0.0f, yoff = 0.0f; // screen pixels
	float height[4] = { 0.0f, 0.0f, 0.0f, 0.0f }; // more y offset, by the unit's overlay height 1-4
};

// What a state (states.txt) lays on a unit while it lasts, and once as it starts and ends.
struct StateInfo {
	std::vector<const OverlayInfo*> overlays;
	const OverlayInfo* cast = nullptr;
	const OverlayInfo* remove = nullptr;
	bool aura = false, curse = false;
};

// What the first-person hands need to know of an item kind (items.json).
struct ItemArt {
	std::string inv, uinv, sinv;     // inventory pictures: plain, unique, set
	std::vector<std::string> types;  // its type and every type it counts as, nearest first
	int inv_trans = 0;               // the palette shift file a coloured unique or set item uses
};

// A unique or set item row: its base item, its own picture (may be empty) and its colour (-1 none).
struct ItemVariant {
	std::string code, inv;
	int colour = -1;
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
	std::vector<MonsterInfo> m_monsters;
	std::unordered_map<uint32_t, ObjectInfo> m_objects;
	std::unordered_map<uint32_t, MissileInfo> m_missiles;
	std::unordered_map<std::string, std::string> m_item_flippies;
	std::unordered_map<std::string, std::string> m_item_names, m_item_types;
	std::unordered_map<std::string, ItemArt> m_item_art;
	std::vector<ItemVariant> m_uniques, m_sets;
	// overlays.json: optional (an older art service does not write it), so it is tried again later
	volatile bool m_overlays_ready = false;
	std::unordered_map<std::string, OverlayInfo> m_overlays;
	std::vector<StateInfo> m_states; // by state number
	std::unordered_map<uint32_t, const OverlayInfo*> m_skill_casts;

	SpriteStore() = default;
	~SpriteStore();

	void workerMain();
	bool loadIndexes();
	bool loadOverlays();
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

	const MonsterInfo* monster(uint32_t txt) const;
	const ObjectInfo* object(uint32_t txt) const;
	const MissileInfo* missile(uint32_t txt) const;
	const std::string* itemFlippy(const std::string& code) const;
	// An item's plain name ("Short Sword") and its type ("rune", "gold", ...) by item code.
	const std::string* itemName(const std::string& code) const;
	const std::string* itemType(const std::string& code) const;
	// Inventory pictures and types of an item kind; a unique or set row by its number.
	const ItemArt* itemArt(const std::string& code) const;
	const ItemVariant* unique(uint32_t row) const;
	const ItemVariant* setItem(uint32_t row) const;
	// A state's overlays by state number, and the overlay a skill flashes on whoever uses it.
	const StateInfo* state(uint32_t id) const;
	const OverlayInfo* skillCast(uint32_t skill) const;
	inline bool overlaysReady() const { return m_overlays_ready; }
};

}
