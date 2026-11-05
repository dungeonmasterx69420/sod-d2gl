/*
	Scrolls of Diablo: the game's own map tiles for the first-person view.
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

#include <mutex>
#include <unordered_set>

namespace d2gl::modules {

// The rooms around the player, drawn with the tiles the game itself placed there.
//
// Every room of the game keeps a list of its floor, wall and roof tiles: where each one is and
// which picture it shows. The game thread reads those lists, takes each picture from the game's
// tile files, and lays the floors flat and stands the walls up. The render thread draws the
// result. Rooms this cannot be done for are left to the caller, which draws them from the
// collision map instead.
class TileWorld {
public:
	struct Vertex {
		float x, y, z;
		uint8_t r, g, b, a;
		float u, v;
	};
	struct Rect {
		int x, y, w, h; // subtiles
	};

private:
	static const int PAGE = 2048;   // atlas page size, pixels
	static const int MAX_PAGES = 6;

	// a picture in the atlas
	struct Art {
		bool valid = false;
		int kind = 0;       // 0 floor, 1 wall across x (stands on a line of constant y), 2 wall across y, 3 upright, 4 roof
		int page = 0;
		int x = 0, y = 0, w = 0, h = 0; // rectangle in the page
		int left = 0;       // the picture's first column, in tile pixels (0..159); upright: from the tile's middle
		int low = 0;        // upright: rows the picture reaches below the line it stands on (negative: it hangs above)
		int foot = 0;       // upright: the column it stands on (the middle of its lowest rows), from the tile's middle
		uint32_t orientation = 0, main = 0;
		// what the game's record said when this was made, to notice a reused address
		uint32_t data_offset = 0, name_hash = 0;
	};
	struct Upright {
		float x, z;   // where it stands
		float w, h;   // size, subtiles
		float cx;     // centre of the picture, right of the standing point
		float bottom; // the picture's lower edge above the ground
		int page;
		float u0, v0, u1, v1;
	};
	// an upright picture placed on a tile, before the halves of one thing are joined
	struct Piece {
		int tx, ty;
		const Art* art;
	};
	struct Page {
		std::vector<uint8_t> pixels; // PAGE * PAGE * 4
		int shelf_x = 0, shelf_y = 0, shelf_h = 0;
		int dirty_y0 = PAGE, dirty_y1 = 0;
	};

	std::mutex m_lock;

	// ---- game thread ----
	uint32_t m_palette[256] = { 0 }; // RGBA bytes
	bool m_palette_set = false;
	uint32_t m_palette_hash = 0;
	uint32_t m_signature = 0;
	DWORD m_next_try = 0;
	bool m_atlas_full = false; // a picture did not fit: start the atlas over with what is needed now
	DWORD m_atlas_reset_at = 0;
	std::unordered_map<uintptr_t, Art> m_art;
	std::unordered_map<std::string, std::vector<uint8_t>> m_files;

	// ---- shared, under m_lock ----
	std::vector<Page> m_pages;
	std::vector<Vertex> m_flat[MAX_PAGES]; // floors and walls, by page
	std::vector<Upright> m_uprights;
	std::vector<Rect> m_rects;
	std::unordered_set<uint32_t> m_standing; // tiles with a wall, a tree or a roof of the game's own
	uint32_t m_version = 0;
	bool m_reset_textures = false;

	// ---- render thread ----
	GLuint m_textures[MAX_PAGES] = { 0 };
	GLuint m_vao = 0, m_vbo = 0, m_vao_dynamic = 0, m_vbo_dynamic = 0;
	uint32_t m_gl_version = 0;
	uint32_t m_first[MAX_PAGES] = { 0 }, m_count[MAX_PAGES] = { 0 };
	std::vector<Upright> m_gl_uprights;
	std::vector<Rect> m_gl_rects;
	std::unordered_set<uint32_t> m_gl_standing;
	std::vector<Vertex> m_scratch;

	void clearScene();
	void forget();
	const std::vector<uint8_t>* file(const char* name);
	bool place(int w, int h, int* page, int* x, int* y);
	Art make(uintptr_t tile_ptr, const void* tile_copy);

public:
	static TileWorld& Instance()
	{
		static TileWorld instance;
		return instance;
	}

	// ---- game thread ----
	void setPalette(const uint32_t* argb);
	void capture(bool active);

	// ---- render thread ----
	// Takes over what the game thread has built since the last call. Call once a frame, first.
	void sync();
	// One more whenever the set of rooms drawn here changes.
	uint32_t coverage() const { return m_gl_version; }
	// True when the room with this subtile rectangle is drawn here.
	bool covers(int x, int y, int w, int h) const;
	// True when the game has put a wall, a tree or a roof on this tile (tile coordinates). Where it
	// has not, what blocks the way is painted into the floor (the rock of a cave), and the caller
	// builds the wall from the collision map.
	bool standing(int tx, int ty) const { return m_gl_standing.count(((uint32_t)tx & 0xFFFF) << 16 | ((uint32_t)ty & 0xFFFF)) != 0; }
	// Draws with the caller's program bound. `yaw` turns the uprights (trees, posts) to the camera.
	void draw(GLint u_textured, GLint u_texture, int texture_unit, float yaw);
};

}
