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

#include "pch.h"
#include "first_person_tiles.h"
#include "d2/common.h"
#include "d2/funcs.h"
#include "helpers.h"
#include <unordered_set>

namespace d2gl::modules {

namespace {

// Subtiles per pixel of height: the scale the unit sprites are drawn at, so a door fits the
// character that walks through it.
const float PX = 0.047f;
const int TILE = 5;         // subtiles along a tile's edge
const float CEILING = 7.0f; // where an indoor level's ceiling lies: the height of the walls built from the collision map
const int TILE_W = 160, TILE_H = 80; // a floor tile's diamond, pixels

// Where things are in the game's memory (1.13c).
const uint32_t UNIT_ACT = 0x18, UNIT_PATH = 0x2C, PATH_ROOM = 0x1C;
const uint32_t ROOM_NEAR = 0x00, ROOM_TILES = 0x08, ROOM_NEAR_COUNT = 0x24, ROOM_COORDS = 0x4C;

#pragma pack(push, 1)
struct RoomTiles {
	uint32_t walls, wall_count, floors, floor_count, roofs, roof_count;
};
// One placed tile: `tx`, `ty` count tiles from the room's corner.
struct TileEntry {
	int32_t px, py, tx, ty;
	uint32_t unknown, flags, tile, type, pad[4];
};
// A tile's picture is cut into blocks of up to 32 x 32 pixels.
struct BlockHeader {
	int16_t x, y;
	uint16_t zero1;
	uint8_t grid_x, grid_y;
	uint16_t format;
	uint32_t length;
	uint16_t zero2;
	uint32_t offset; // from the first block header of the tile
};
#pragma pack(pop)

struct RoomInfo {
	uint32_t ptr;
	int32_t coords[8]; // subtile x, y, w, h, then tile x, y, w, h
	RoomTiles tiles;
};

// Copies game memory that may have been freed since the address was read.
bool safeCopy(void* dst, uint32_t src, size_t size)
{
	if (src < 0x10000 || !size)
		return false;
	__try {
		memcpy(dst, (const void*)(uintptr_t)src, size);
		return true;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
}

inline uint32_t mix(uint32_t h, uint32_t v)
{
	h ^= v + 0x9e3779b9u + (h << 6) + (h >> 2);
	return h;
}

// Decodes a tile's blocks into `image` (160 wide, rows y0 .. y1 of the tile's own coordinates).
bool decodeTile(const std::vector<uint8_t>& file, uint32_t offset, uint32_t block_count, const uint32_t* palette, std::vector<uint32_t>& image, int* y0_out, int* rows_out)
{
	if (!block_count || block_count > 512 || (uint64_t)offset + (uint64_t)block_count * sizeof(BlockHeader) > file.size())
		return false;
	const BlockHeader* blocks = (const BlockHeader*)(file.data() + offset);

	int y0 = INT_MAX, y1 = INT_MIN;
	for (uint32_t i = 0; i < block_count; i++) {
		const int height = blocks[i].format == 1 ? 16 : 32;
		y0 = glm::min(y0, (int)blocks[i].y);
		y1 = glm::max(y1, (int)blocks[i].y + height);
	}
	const int rows = y1 - y0;
	if (rows <= 0 || rows > 1024)
		return false;
	image.assign((size_t)TILE_W * rows, 0);

	const auto put = [&](int x, int y, uint8_t index) {
		if (x >= 0 && x < TILE_W && y >= 0 && y < rows)
			image[(size_t)y * TILE_W + x] = palette[index];
	};
	for (uint32_t i = 0; i < block_count; i++) {
		const BlockHeader& block = blocks[i];
		if ((uint64_t)offset + block.offset + block.length > file.size())
			continue;
		const uint8_t* data = file.data() + offset + block.offset;
		const int bx = block.x, by = block.y - y0;
		if (block.format == 1) {
			// a floor diamond: 15 rows, each centred, no run lengths
			if (block.length != 256)
				continue;
			static const int skip[15] = { 14, 12, 10, 8, 6, 4, 2, 0, 2, 4, 6, 8, 10, 12, 14 };
			static const int count[15] = { 4, 8, 12, 16, 20, 24, 28, 32, 28, 24, 20, 16, 12, 8, 4 };
			for (int row = 0; row < 15; row++)
				for (int n = 0; n < count[row]; n++)
					put(bx + skip[row] + n, by + row, *data++);
		} else {
			// run lengths: (skip, count, pixels...), (0, 0) ends a row
			int x = 0, y = 0;
			int left = (int)block.length;
			while (left >= 2) {
				const int b1 = data[0], b2 = data[1];
				data += 2;
				left -= 2;
				if (!b1 && !b2) {
					x = 0;
					y++;
					continue;
				}
				x += b1;
				if (b2 > left)
					break;
				for (int n = 0; n < b2; n++)
					put(bx + x + n, by + y, data[n]);
				data += b2;
				left -= b2;
				x += b2;
			}
		}
	}
	*y0_out = y0;
	*rows_out = rows;
	return true;
}

inline void pushQuad(std::vector<TileWorld::Vertex>& out, const glm::vec3 p[4], const glm::vec2 uv[4], uint8_t shade)
{
	static const int order[6] = { 0, 1, 2, 0, 2, 3 };
	for (const int i : order)
		out.push_back({ p[i].x, p[i].y, p[i].z, shade, shade, shade, 255, uv[i].x, uv[i].y });
}

}

const std::vector<uint8_t>* TileWorld::file(const char* name)
{
	const auto it = m_files.find(name);
	if (it != m_files.end())
		return it->second.empty() ? nullptr : &it->second;

	std::vector<uint8_t> bytes;
	void* handle = nullptr;
	char path[MAX_PATH] = { 0 };
	strncpy_s(path, name, _TRUNCATE);
	if (d2::mpqOpenFile && d2::mpqOpenFile(path, &handle) && handle) {
		const uint32_t size = d2::mpqGetFileSize(handle, nullptr);
		if (size && size < 64u * 1024u * 1024u) {
			bytes.resize(size);
			uint32_t read = 0;
			if (!d2::mpqReadFile(handle, bytes.data(), size, &read, 0, 0, 0) || read != size)
				bytes.clear();
		}
		d2::mpqCloseFile(handle);
	}
	auto& slot = m_files[name];
	slot.swap(bytes);
	return slot.empty() ? nullptr : &slot;
}

// A free rectangle in the atlas (shelves, page after page).
bool TileWorld::place(int w, int h, int* page, int* x, int* y)
{
	if (w > PAGE || h > PAGE)
		return false;
	for (int attempt = 0; attempt < 2; attempt++) {
		if (m_pages.empty() || attempt == 1) {
			if ((int)m_pages.size() >= MAX_PAGES)
				return false;
			m_pages.emplace_back();
			m_pages.back().pixels.assign((size_t)PAGE * PAGE * 4, 0);
		}
		Page& p = m_pages.back();
		if (p.shelf_x + w > PAGE) {
			p.shelf_y += p.shelf_h;
			p.shelf_x = 0;
			p.shelf_h = 0;
		}
		if (p.shelf_y + h > PAGE)
			continue;
		*page = (int)m_pages.size() - 1;
		*x = p.shelf_x;
		*y = p.shelf_y;
		p.shelf_x += w;
		p.shelf_h = glm::max(p.shelf_h, h);
		p.dirty_y0 = glm::min(p.dirty_y0, *y);
		p.dirty_y1 = glm::max(p.dirty_y1, *y + h);
		return true;
	}
	return false;
}

// Turns one of the game's tile records into a picture in the atlas.
TileWorld::Art TileWorld::make(uintptr_t, const void* tile_copy)
{
	const d2::TileContext& tile = *(const d2::TileContext*)tile_copy;
	Art art;
	art.data_offset = tile.dwDataPtr;
	art.name_hash = (uint32_t)(uintptr_t)tile.szTileName;

	// What stands where. A tile is a diamond on screen: its top corner is the world corner with
	// the smallest x and y. "Left" walls stand on the edge that runs from there along y, "right"
	// walls on the edge that runs along x.
	const uint32_t o = tile.dwOrientation;
	if (o == 0)
		art.kind = 0;
	else if (o == 15)
		art.kind = 4;
	else if (o == 2 || o == 3 || o == 6 || o == 9)
		art.kind = 1;
	else if (o == 1 || o == 4 || o == 5 || o == 8)
		art.kind = 2;
	else if (o == 7 || o == 12 || o == 14)
		art.kind = 3;
	else
		return art; // shadows, markers, the walls of pits: not drawn

	char name[160] = { 0 };
	for (int i = 0; i < 159; i++) {
		if (!safeCopy(&name[i], (uint32_t)(uintptr_t)tile.szTileName + i, 1) || !name[i])
			break;
	}
	const std::vector<uint8_t>* bytes = name[0] ? file(name) : nullptr;
	if (!bytes)
		return art;

	static std::vector<uint32_t> image, out;
	int y0 = 0, rows = 0;
	if (!decodeTile(*bytes, tile.dwDataPtr, tile.dwSubBlocks, m_palette, image, &y0, &rows))
		return art;
	const auto at = [&](int x, int y) -> uint32_t { // y in the tile's own coordinates
		const int row = y - y0;
		return x >= 0 && x < TILE_W && row >= 0 && row < rows ? image[(size_t)row * TILE_W + x] : 0;
	};

	int w = 0, h = 0;
	if (art.kind == 0 || art.kind == 4) {
		// The diamond, with each row stretched to the picture's edges so that neighbouring tiles
		// meet without a seam. Only where the tile fills its diamond: an overlay keeps its shape.
		w = TILE_W;
		h = TILE_H;
		out.assign((size_t)w * h, 0);
		int filled = 0;
		for (int y = 0; y < h; y++)
			for (int x = 0; x < w; x++) {
				out[(size_t)y * w + x] = at(x, y);
				filled += out[(size_t)y * w + x] ? 1 : 0;
			}
		art.roof = tile.wRoofIndex;
		if (art.kind == 0 && filled > TILE_W * TILE_H / 2 * 9 / 10) {
			for (int y = 0; y < h; y++) {
				uint32_t* row = &out[(size_t)y * w];
				int first = -1, last = -1;
				for (int x = 0; x < w; x++)
					if (row[x]) {
						if (first < 0)
							first = x;
						last = x;
					}
				if (first < 0) {
					if (y > 0)
						memcpy(row, row - w, (size_t)w * 4);
					continue;
				}
				for (int x = 0; x < first; x++)
					row[x] = row[first];
				for (int x = last + 1; x < w; x++)
					row[x] = row[last];
				for (int x = first; x <= last; x++)
					if (!row[x])
						row[x] = row[x - 1];
			}
		} else if (!filled) {
			return art;
		}
	} else if (art.kind == 1 || art.kind == 2) {
		// Stand the wall up. On screen its foot runs along the tile's edge, one pixel down for
		// every two across; here every column is moved so that the foot is level.
		const auto ground = [&](int x) { return art.kind == 1 ? -120 + x / 2 : -40 - (x + 1) / 2; };
		const int full = -40 - y0; // the tallest column
		if (full <= 0 || full > 1024)
			return art;
		static std::vector<uint32_t> upright;
		upright.assign((size_t)TILE_W * full, 0);
		int c0 = TILE_W, c1 = 0, v0 = full;
		for (int x = 0; x < TILE_W; x++)
			for (int v = 0; v < full; v++) {
				const uint32_t pixel = at(x, ground(x) - full + v);
				if (!pixel)
					continue;
				upright[(size_t)v * TILE_W + x] = pixel;
				c0 = glm::min(c0, x);
				c1 = glm::max(c1, x + 1);
				v0 = glm::min(v0, v);
			}
		if (c1 <= c0)
			return art;
		w = c1 - c0;
		h = full - v0;
		out.assign((size_t)w * h, 0);
		for (int y = 0; y < h; y++)
			memcpy(&out[(size_t)y * w], &upright[(size_t)(v0 + y) * TILE_W + c0], (size_t)w * 4);
		art.left = c0;
	} else {
		// An upright thing: the picture as it is, standing on the corner (a corner post) or the
		// middle (a tree, a pillar) of its tile.
		const int ax = 80, ay = o == 7 ? -80 : -40;
		int c0 = TILE_W, c1 = 0, r0 = rows, r1 = 0;
		for (int row = 0; row < rows; row++)
			for (int x = 0; x < TILE_W; x++)
				if (image[(size_t)row * TILE_W + x]) {
					c0 = glm::min(c0, x);
					c1 = glm::max(c1, x + 1);
					r0 = glm::min(r0, row);
					r1 = glm::max(r1, row + 1);
				}
		if (c1 <= c0 || r1 <= r0)
			return art;
		w = c1 - c0;
		h = r1 - r0;
		out.assign((size_t)w * h, 0);
		for (int y = 0; y < h; y++)
			memcpy(&out[(size_t)y * w], &image[(size_t)(r0 + y) * TILE_W + c0], (size_t)w * 4);
		art.left = c0 - ax;
		art.low = (y0 + r1) - ay;
		// what it stands on: the middle of its lowest rows (a trunk, not the whole crown)
		int f0 = TILE_W, f1 = 0;
		for (int row = glm::max(r0, r1 - 8); row < r1; row++)
			for (int x = c0; x < c1; x++)
				if (image[(size_t)row * TILE_W + x]) {
					f0 = glm::min(f0, x);
					f1 = glm::max(f1, x + 1);
				}
		art.foot = (f0 + f1) / 2 - ax;
	}
	art.orientation = o;
	art.main = tile.dwMainIndex;

	// into the atlas, with a border: the picture's own edge for floors, nothing for the rest
	int page = 0, px = 0, py = 0;
	if (!place(w + 2, h + 2, &page, &px, &py)) {
		m_atlas_full = true;
		return art;
	}
	uint8_t* pixels = m_pages[page].pixels.data();
	for (int y = -1; y <= h; y++) {
		uint32_t* row = (uint32_t*)(pixels + ((size_t)(py + 1 + y) * PAGE + px) * 4);
		const int sy = glm::clamp(y, 0, h - 1);
		for (int x = -1; x <= w; x++) {
			const bool inside = x >= 0 && x < w && y >= 0 && y < h;
			row[x + 1] = inside || art.kind == 0 || art.kind == 4 ? out[(size_t)sy * w + glm::clamp(x, 0, w - 1)] : 0;
		}
	}
	art.page = page;
	art.x = px + 1;
	art.y = py + 1;
	art.w = w;
	art.h = h;
	art.valid = true;
	return art;
}

void TileWorld::clearScene()
{
	if (!m_signature && m_rects.empty())
		return;
	std::lock_guard<std::mutex> lock(m_lock);
	for (auto& list : m_flat)
		list.clear();
	m_uprights.clear();
	m_rects.clear();
	m_standing.clear();
	m_signature = 0;
	m_version++;
}

// Drops every picture: they were made with another act's palette, or the game has reloaded its tiles.
void TileWorld::forget()
{
	std::lock_guard<std::mutex> lock(m_lock);
	m_art.clear();
	m_files.clear();
	m_pages.clear();
	for (auto& list : m_flat)
		list.clear();
	m_uprights.clear();
	m_rects.clear();
	m_reset_textures = true;
	m_signature = 0;
	m_version++;
}

void TileWorld::setPalette(const uint32_t* rgba)
{
	const uint32_t hash = helpers::hash(rgba, sizeof(uint32_t) * 256);
	if (m_palette_set && hash == m_palette_hash)
		return;
	forget();
	memcpy(m_palette, rgba, sizeof(m_palette));
	m_palette_hash = hash;
	m_palette_set = true;
}

void TileWorld::capture(bool active, bool inside)
{
	if (!active || helpers::getVersion() != Version::V_113c) {
		clearScene();
		return;
	}
	const DWORD now = GetTickCount();
	if ((int32_t)(now - m_next_try) < 0)
		return;
	m_next_try = now + 50;

	const uint32_t player = (uint32_t)(uintptr_t)d2::getPlayerUnit();
	uint32_t act = 0, path = 0, room = 0;
	if (!player || !safeCopy(&act, player + UNIT_ACT, 4) || act > 4 || !safeCopy(&path, player + UNIT_PATH, 4) || !safeCopy(&room, path + PATH_ROOM, 4) || !room) {
		clearScene();
		return;
	}

	// ---- the act's colours ----
	{
		static uint32_t loaded_act = UINT_MAX;
		if (act != loaded_act || !m_palette_set) {
			char name[64];
			sprintf_s(name, "data\\global\\palette\\ACT%u\\pal.dat", act + 1);
			m_files.erase(name);
			const std::vector<uint8_t>* pal = file(name);
			if (!pal || pal->size() < 768) {
				m_next_try = now + 2000;
				clearScene();
				return;
			}
			uint32_t rgba[256];
			for (int i = 0; i < 256; i++) // stored blue, green, red
				rgba[i] = (uint32_t)(*pal)[i * 3 + 2] | ((uint32_t)(*pal)[i * 3 + 1] << 8) | ((uint32_t)(*pal)[i * 3] << 16) | 0xFF000000u;
			setPalette(rgba);
			loaded_act = act;
		}
	}

	// ---- the rooms: the player's, the ones around it, and the ones around those ----
	static std::vector<RoomInfo> rooms;
	rooms.clear();
	const auto add = [&](uint32_t ptr) {
		if (!ptr || rooms.size() >= 64)
			return;
		for (const auto& known : rooms)
			if (known.ptr == ptr)
				return;
		RoomInfo info = {};
		info.ptr = ptr;
		uint32_t tiles = 0;
		if (!safeCopy(info.coords, ptr + ROOM_COORDS, sizeof(info.coords)) || !safeCopy(&tiles, ptr + ROOM_TILES, 4) || !safeCopy(&info.tiles, tiles, sizeof(info.tiles)))
			return;
		if (info.coords[2] <= 0 || info.coords[2] > 400 || info.coords[3] <= 0 || info.coords[3] > 400)
			return;
		if (info.tiles.wall_count > 8192 || info.tiles.floor_count > 8192)
			return;
		rooms.push_back(info);
	};
	add(room);
	size_t begin = 0;
	for (int depth = 0; depth < 2; depth++) {
		const size_t end = rooms.size();
		for (size_t i = begin; i < end; i++) {
			uint32_t list = 0, count = 0, around[64];
			if (!safeCopy(&list, rooms[i].ptr + ROOM_NEAR, 4) || !safeCopy(&count, rooms[i].ptr + ROOM_NEAR_COUNT, 4) || !count || count > 64)
				continue;
			if (!safeCopy(around, list, count * 4))
				continue;
			for (uint32_t n = 0; n < count; n++)
				add(around[n]);
		}
		begin = end;
	}

	uint32_t signature = 0x1234u;
	for (const auto& info : rooms) {
		signature = mix(signature, info.ptr);
		signature = mix(signature, info.tiles.walls);
		signature = mix(signature, info.tiles.wall_count);
		signature = mix(signature, info.tiles.floors);
		signature = mix(signature, info.tiles.floor_count);
	}
	signature = mix(signature, inside ? 1u : 0u);
	if (!signature)
		signature = 1;
	if (signature == m_signature)
		return;
	m_inside = inside;

	// ---- rebuild ----
	std::lock_guard<std::mutex> lock(m_lock);
	for (auto& list : m_flat)
		list.clear();
	m_uprights.clear();
	m_rects.clear();
	m_standing.clear();
	const auto stands = [&](int tx, int ty) { m_standing.insert(((uint32_t)tx & 0xFFFF) << 16 | ((uint32_t)ty & 0xFFFF)); };

	static std::unordered_set<uint64_t> placed;
	static std::unordered_map<uint32_t, int> layers;
	static std::vector<TileEntry> entries;
	static std::vector<Piece> pieces;
	placed.clear();
	layers.clear();
	pieces.clear();
	const float page_size = (float)PAGE;

	for (const auto& info : rooms) {
		int floors = 0;
		const uint32_t lists[3][2] = { { info.tiles.floors, info.tiles.floor_count }, { info.tiles.walls, info.tiles.wall_count }, { info.tiles.roofs, info.tiles.roof_count } };
		for (const auto& list : lists) {
			if (!list[1] || list[1] > 8192)
				continue;
			entries.resize(list[1]);
			if (!safeCopy(entries.data(), list[0], (size_t)list[1] * sizeof(TileEntry)))
				continue;
			for (const TileEntry& entry : entries) {
				d2::TileContext tile;
				if (!entry.tile || !safeCopy(&tile, entry.tile, sizeof(tile)))
					continue;
				auto it = m_art.find(entry.tile);
				if (it == m_art.end() || it->second.data_offset != tile.dwDataPtr || it->second.name_hash != (uint32_t)(uintptr_t)tile.szTileName) {
					m_art[entry.tile] = make(entry.tile, &tile);
					it = m_art.find(entry.tile);
				}
				const Art& art = it->second;
				const int tx = info.coords[4] + entry.tx, ty = info.coords[5] + entry.ty;
				if (tile.dwOrientation != 0 && tile.dwOrientation != 10 && tile.dwOrientation != 11 && tile.dwOrientation != 13) {
					// a wall also keeps the tile across its edge clear of walls built from the collision map
					stands(tx, ty);
					if (art.kind == 1)
						stands(tx, ty - 1);
					else if (art.kind == 2)
						stands(tx - 1, ty);
				}
				if (!art.valid)
					continue;

				const uint64_t key = ((uint64_t)entry.tile << 32) | (uint64_t)(((uint32_t)tx & 0xFFFF) << 16 | ((uint32_t)ty & 0xFFFF));
				if (!placed.insert(key).second)
					continue; // rooms share the tiles on their border
				const float x0 = (float)(tx * TILE), z0 = (float)(ty * TILE);
				const float u0 = (float)art.x, v0 = (float)art.y, uw = (float)art.w, vh = (float)art.h;

				if (art.kind == 0) {
					const int layer = layers[((uint32_t)tx & 0xFFFF) << 16 | ((uint32_t)ty & 0xFFFF)]++;
					const float y = 0.004f * (float)layer;
					const glm::vec3 p[4] = { { x0, y, z0 }, { x0 + TILE, y, z0 }, { x0 + TILE, y, z0 + TILE }, { x0, y, z0 + TILE } };
					// the diamond's corners: top, right, bottom, left
					const glm::vec2 uv[4] = { { (u0 + 80.0f) / page_size, (v0 + 0.75f) / page_size }, { (u0 + 159.25f) / page_size, (v0 + 40.0f) / page_size },
						{ (u0 + 80.0f) / page_size, (v0 + 79.25f) / page_size }, { (u0 + 0.75f) / page_size, (v0 + 40.0f) / page_size } };
					pushQuad(m_flat[art.page], p, uv, 200);
					floors++;
					if (m_inside && layer == 0) {
						// the ceiling: the same rock overhead, in the dark
						const glm::vec3 top[4] = { { x0, CEILING, z0 }, { x0 + TILE, CEILING, z0 }, { x0 + TILE, CEILING, z0 + TILE }, { x0, CEILING, z0 + TILE } };
						pushQuad(m_flat[art.page], top, uv, 95);
					}
				} else if (art.kind == 4) {
					// a roof, flat at its own height
					const float y = (float)glm::max(art.roof, 40) * PX;
					const glm::vec3 p[4] = { { x0, y, z0 }, { x0 + TILE, y, z0 }, { x0 + TILE, y, z0 + TILE }, { x0, y, z0 + TILE } };
					const glm::vec2 uv[4] = { { (u0 + 80.0f) / page_size, (v0 + 0.75f) / page_size }, { (u0 + 159.25f) / page_size, (v0 + 40.0f) / page_size },
						{ (u0 + 80.0f) / page_size, (v0 + 79.25f) / page_size }, { (u0 + 0.75f) / page_size, (v0 + 40.0f) / page_size } };
					pushQuad(m_flat[art.page], p, uv, 185);
				} else if (art.kind == 1 || art.kind == 2) {
					const float height = vh * PX;
					const float a = (float)(art.left - 80) / 16.0f, b = (float)(art.left + art.w - 80) / 16.0f; // along the edge, from the tile's top corner
					const glm::vec2 uv[4] = { { (u0 + 0.5f) / page_size, (v0 + vh - 0.5f) / page_size }, { (u0 + uw - 0.5f) / page_size, (v0 + vh - 0.5f) / page_size },
						{ (u0 + uw - 0.5f) / page_size, (v0 + 0.5f) / page_size }, { (u0 + 0.5f) / page_size, (v0 + 0.5f) / page_size } };
					if (art.kind == 1) {
						const glm::vec3 p[4] = { { x0 + a, 0.0f, z0 }, { x0 + b, 0.0f, z0 }, { x0 + b, height, z0 }, { x0 + a, height, z0 } };
						pushQuad(m_flat[art.page], p, uv, 200);
					} else {
						const glm::vec3 p[4] = { { x0, 0.0f, z0 - a }, { x0, 0.0f, z0 - b }, { x0, height, z0 - b }, { x0, height, z0 - a } };
						pushQuad(m_flat[art.page], p, uv, 170);
					}
				} else {
					pieces.push_back({ tx, ty, &art });
				}
			}
		}
		if (floors)
			m_rects.push_back({ info.coords[0], info.coords[1], info.coords[2], info.coords[3] });
	}

	// ---- uprights ----
	// A tree is wider than a tile, and the game keeps it as two halves on tiles that are side by
	// side on screen: (x, y) and (x + 1, y - 1). Halves of one picture are put back together and
	// stand on one spot, so that the whole tree turns to the camera as one.
	std::sort(pieces.begin(), pieces.end(), [](const Piece& a, const Piece& b) {
		if (a.art->orientation != b.art->orientation)
			return a.art->orientation < b.art->orientation;
		if (a.art->name_hash != b.art->name_hash)
			return a.art->name_hash < b.art->name_hash;
		if (a.art->main != b.art->main)
			return a.art->main < b.art->main;
		if (a.tx + a.ty != b.tx + b.ty)
			return a.tx + a.ty < b.tx + b.ty;
		return a.tx - a.ty < b.tx - b.ty;
	});
	for (size_t first = 0; first < pieces.size();) {
		size_t last = first + 1;
		const auto joins = [&](const Piece& a, const Piece& b) {
			return a.art->orientation != 7 && a.art->orientation == b.art->orientation && a.art->name_hash == b.art->name_hash && a.art->main == b.art->main
				&& a.tx + a.ty == b.tx + b.ty && (b.tx - b.ty) - (a.tx - a.ty) == 2;
		};
		while (last < pieces.size() && last - first < 4 && joins(pieces[last - 1], pieces[last]))
			last++;

		// Columns are counted from the middle of the first piece's tile; a tile further right
		// on screen starts 160 columns on. The group stands where its lowest piece does.
		const Piece& head = pieces[first];
		const bool corner = head.art->orientation == 7;
		int low = INT_MIN, foot = 0;
		for (size_t i = first; i < last; i++)
			if (pieces[i].art->low > low) {
				low = pieces[i].art->low;
				foot = (int)(i - first) * 160 + pieces[i].art->foot;
			}
		if (corner)
			foot = 0; // a corner post stands on the corner
		const float lift = low < -24 ? (float)(-low) * PX : 0.0f; // something that hangs stays up; the rest stands on the ground
		const float along = (float)foot / 32.0f; // a pixel across the screen is 1/32 subtile along x and back along y
		const float stand_x = (float)(head.tx * TILE) + (corner ? 0.0f : 2.5f) + along;
		const float stand_z = (float)(head.ty * TILE) + (corner ? 0.0f : 2.5f) - along;
		for (size_t i = first; i < last; i++) {
			const Art& art = *pieces[i].art;
			const float u0 = (float)art.x, v0 = (float)art.y, uw = (float)art.w, vh = (float)art.h;
			Upright up;
			up.x = stand_x;
			up.z = stand_z;
			up.w = uw * PX;
			up.h = vh * PX;
			up.cx = ((float)((int)(i - first) * 160 + art.left - foot) + uw * 0.5f) * PX;
			up.bottom = (float)(low - art.low) * PX + lift;
			up.page = art.page;
			up.u0 = (u0 + 0.5f) / page_size;
			up.v0 = (v0 + 0.5f) / page_size;
			up.u1 = (u0 + uw - 0.5f) / page_size;
			up.v1 = (v0 + vh - 0.5f) / page_size;
			m_uprights.push_back(up);
		}
		first = last;
	}
	m_signature = signature;
	m_version++;

	// The atlas holds the pictures of every room walked through since it was started. When it
	// is full, start it over: the next pass fills it with what the rooms here need. Not more
	// often than every ten seconds, in case that alone does not fit.
	if (m_atlas_full) {
		m_atlas_full = false;
		if (now - m_atlas_reset_at > 10000) {
			m_atlas_reset_at = now;
			m_art.clear();
			m_pages.clear();
			m_reset_textures = true;
			m_signature = 0;
		}
	}
}

void TileWorld::sync()
{
	std::unique_lock<std::mutex> lock(m_lock, std::try_to_lock);
	if (!lock.owns_lock() || m_gl_version == m_version)
		return;

	GLint prev_vao = 0, prev_vbo = 0, prev_unit = 0, prev_texture = 0;
	glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prev_vao);
	glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &prev_vbo);
	glGetIntegerv(GL_ACTIVE_TEXTURE, &prev_unit);
	glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev_texture);

	if (!m_vao) {
		GLuint* vaos[2] = { &m_vao, &m_vao_dynamic };
		GLuint* vbos[2] = { &m_vbo, &m_vbo_dynamic };
		for (int i = 0; i < 2; i++) {
			glGenVertexArrays(1, vaos[i]);
			glGenBuffers(1, vbos[i]);
			glBindVertexArray(*vaos[i]);
			glBindBuffer(GL_ARRAY_BUFFER, *vbos[i]);
			glEnableVertexAttribArray(0);
			glEnableVertexAttribArray(1);
			glEnableVertexAttribArray(2);
			glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), (const void*)offsetof(Vertex, x));
			glVertexAttribPointer(1, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(Vertex), (const void*)offsetof(Vertex, r));
			glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), (const void*)offsetof(Vertex, u));
		}
	}

	// ---- atlas pages ----
	if (m_reset_textures) {
		for (GLuint& texture : m_textures) {
			if (texture)
				glDeleteTextures(1, &texture);
			texture = 0;
		}
		m_reset_textures = false;
	}
	for (size_t i = 0; i < m_pages.size() && i < MAX_PAGES; i++) {
		Page& page = m_pages[i];
		if (!m_textures[i]) {
			glGenTextures(1, &m_textures[i]);
			glBindTexture(GL_TEXTURE_2D, m_textures[i]);
			glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, PAGE, PAGE, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
			page.dirty_y0 = 0;
			page.dirty_y1 = glm::max(page.dirty_y1, page.shelf_y + page.shelf_h);
		}
		if (page.dirty_y1 > page.dirty_y0) {
			glBindTexture(GL_TEXTURE_2D, m_textures[i]);
			glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
			glTexSubImage2D(GL_TEXTURE_2D, 0, 0, page.dirty_y0, PAGE, page.dirty_y1 - page.dirty_y0, GL_RGBA, GL_UNSIGNED_BYTE, page.pixels.data() + (size_t)page.dirty_y0 * PAGE * 4);
			page.dirty_y0 = PAGE;
			page.dirty_y1 = 0;
		}
	}

	// ---- geometry ----
	m_scratch.clear();
	for (int i = 0; i < MAX_PAGES; i++) {
		m_first[i] = (uint32_t)m_scratch.size();
		m_count[i] = (uint32_t)m_flat[i].size();
		m_scratch.insert(m_scratch.end(), m_flat[i].begin(), m_flat[i].end());
	}
	glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
	glBufferData(GL_ARRAY_BUFFER, m_scratch.size() * sizeof(Vertex), m_scratch.data(), GL_STATIC_DRAW);
	m_gl_uprights = m_uprights;
	m_gl_rects = m_rects;
	m_gl_standing = m_standing;
	m_gl_version = m_version;
	trace_log("FirstPerson: map tiles rebuilt (%u rooms, %u vertices, %u uprights, %u atlas pages).", (uint32_t)m_gl_rects.size(), (uint32_t)m_scratch.size(), (uint32_t)m_gl_uprights.size(), (uint32_t)m_pages.size());

	glBindTexture(GL_TEXTURE_2D, prev_texture);
	glActiveTexture(prev_unit);
	glBindVertexArray(prev_vao);
	glBindBuffer(GL_ARRAY_BUFFER, prev_vbo);
}

bool TileWorld::covers(int x, int y, int w, int h) const
{
	const int cx = x + w / 2, cy = y + h / 2;
	for (const Rect& rect : m_gl_rects)
		if (cx >= rect.x && cy >= rect.y && cx < rect.x + rect.w && cy < rect.y + rect.h)
			return true;
	return false;
}

void TileWorld::draw(GLint u_textured, GLint u_texture, int texture_unit, float yaw)
{
	if (!m_vao || m_gl_rects.empty())
		return;

	glActiveTexture(GL_TEXTURE0 + texture_unit);
	glUniform1i(u_texture, texture_unit);
	glUniform1f(u_textured, 1.0f);

	glBindVertexArray(m_vao);
	for (int i = 0; i < MAX_PAGES; i++) {
		if (!m_count[i] || !m_textures[i])
			continue;
		glBindTexture(GL_TEXTURE_2D, m_textures[i]);
		glDrawArrays(GL_TRIANGLES, m_first[i], m_count[i]);
	}

	// uprights face the camera, turning about their own foot
	if (!m_gl_uprights.empty()) {
		const glm::vec3 right = { -sinf(yaw), 0.0f, cosf(yaw) };
		glBindVertexArray(m_vao_dynamic);
		glBindBuffer(GL_ARRAY_BUFFER, m_vbo_dynamic);
		for (int i = 0; i < MAX_PAGES; i++) {
			if (!m_textures[i])
				continue;
			m_scratch.clear();
			for (const Upright& up : m_gl_uprights) {
				if (up.page != i)
					continue;
				const glm::vec3 foot = glm::vec3(up.x, 0.0f, up.z) + right * up.cx;
				const glm::vec3 half = right * (up.w * 0.5f);
				const float bottom = up.bottom, top = up.bottom + up.h;
				const glm::vec3 p[4] = { foot - half + glm::vec3(0.0f, bottom, 0.0f), foot + half + glm::vec3(0.0f, bottom, 0.0f), foot + half + glm::vec3(0.0f, top, 0.0f),
					foot - half + glm::vec3(0.0f, top, 0.0f) };
				const glm::vec2 uv[4] = { { up.u0, up.v1 }, { up.u1, up.v1 }, { up.u1, up.v0 }, { up.u0, up.v0 } };
				pushQuad(m_scratch, p, uv, 200);
			}
			if (m_scratch.empty())
				continue;
			glBindTexture(GL_TEXTURE_2D, m_textures[i]);
			glBufferData(GL_ARRAY_BUFFER, m_scratch.size() * sizeof(Vertex), m_scratch.data(), GL_STREAM_DRAW);
			glDrawArrays(GL_TRIANGLES, 0, (GLsizei)m_scratch.size());
		}
	}
	glUniform1f(u_textured, 0.0f);
}

}
