/*
	Scrolls of Diablo: first-person world stage for D2GL.
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
#include "first_person.h"
#include "first_person_sprites.h"
#include "first_person_text.h"
#include "first_person_tiles.h"
#include "first_person_hands.h"
#include "d2/common.h"
#include "d2/funcs.h"
#include "helpers.h"
#include "option/menu.h"
#include <nlohmann/json.hpp>
#include <stb/stb_image.h>
#include <glm/gtc/constants.hpp>

namespace d2gl::modules {

// ---- the state region written by sod.dll (layout: Scrolls of Diablo docs/PROTOCOL.md section 4) ----
namespace {

const wchar_t* STATE_NAME = L"Local\\SoD_State_v1";
const uint32_t STATE_MAGIC = 0x53446F53;
const uint32_t STATE_VERSION = 1;

#pragma pack(push, 1)
struct StateHeader {
	uint32_t magic, version, headerSize, status, online, heartbeat, dllVersion, flags;
	uint32_t maxUnits, unitRecordSize, playerRecordSize, maxRooms, roomEntrySize, collArenaBytes, stringAreaBytes, frameBufferSize;
	uint32_t frameBuf0Off, frameBuf1Off, roomSectionOff, totalSize;
	uint32_t frameSeq, frameIndex, roomSeq, roomEpoch;
};
struct FrameMeta {
	uint32_t valid, tick, timeMs;
	int32_t levelNo, act;
	uint32_t town, online, levelNameOff;
	uint16_t levelNameLen, unitCount;
	uint32_t stringUsed, status;
	uint8_t inside; // an indoor level: a cave, a crypt, a sewer
	uint8_t reserved[19];
};
struct PlayerRecord {
	uint32_t id, classId;
	int32_t levelStat;
	uint32_t px, py;
	int16_t dir64;
	uint16_t mode;
	uint32_t gfxFrame, frameCount; // animation frame and frame count, x 256
	uint32_t hpRaw, hpMaxRaw, mpRaw, mpMaxRaw, stamRaw, stamMaxRaw;
	int32_t leftSkill, rightSkill;
	uint32_t nameOff;
	uint16_t nameLen, pad0;
	uint32_t wclassOff;
	uint16_t wclassLen, pad1;
	struct {
		char code[4];     // items.txt code, zero padded; all zero for an empty hand (and from an older sod.dll)
		uint16_t file;    // unique or set row
		uint8_t quality;  // 5 set, 7 unique
		uint8_t flags;    // bit 0 ethereal
	} held[2];            // right hand, left hand
};
struct UnitRecord {
	uint8_t type, pad0;
	int16_t dir64;
	uint32_t id, txt;
	uint16_t mode, rawFlags;
	uint32_t px, py;
	uint32_t gfxFrame, frameCount; // animation frame and frame count, x 256
	uint32_t hp, hpMax;
	uint16_t nameIndex;
	uint8_t itemQuality, pad1;
	uint32_t nameOff;
	uint16_t nameLen, itemCodeLen;
	uint32_t itemCodeOff;
	int32_t owner;
	uint32_t reserved;
};
struct RoomSectionHeader {
	uint32_t roomCount, arenaUsed;
	uint8_t reserved[56];
};
struct RoomEntry {
	int32_t levelNo;
	uint32_t x, y, w, h, collOff, collLen, reserved;
};
#pragma pack(pop)

static_assert(sizeof(FrameMeta) == 64, "frame meta size");
static_assert(sizeof(PlayerRecord) == 96, "player record size");
static_assert(sizeof(UnitRecord) == 64, "unit record size");
static_assert(sizeof(RoomSectionHeader) == 64, "room header size");
static_assert(sizeof(RoomEntry) == 32, "room entry size");

const uint32_t STATUS_ACTIVE = 2;

// ---- the command channel read by sod.dll (Scrolls of Diablo docs/PROTOCOL.md section 2) ----
const wchar_t* CMD_NAME = L"Local\\SoD_Cmd_v2";
const uint32_t CMD_MAGIC = 0x43446F53;
const uint32_t CMD_VERSION = 2;
const uint32_t CMD_FLAG_LOOK = 4;
const uint32_t CMD_DLL_DIRECT = 1; // dllFlags bit0: sod.dll walks the player with the game's move calls, no pointer
enum : uint32_t { OP_ATTACK_UNIT = 1, OP_CAST_AT = 2, OP_INTERACT = 3, OP_STOP_AT = 12 };

#pragma pack(push, 1)
struct CmdHeader {
	volatile uint32_t magic, version, writeIndex, readIndex, capacity, recordSize, dllStatus, dllHeartbeat;
	volatile float moveX, moveY;
	volatile uint32_t moveSeq, flags;
	volatile LONG uiFlags;
	volatile uint32_t dllVersion, lastError, dllFlags;
};
struct CmdRecord {
	uint32_t opcode, seq, a0, a1;
	float x, y;
	uint32_t a2, a3;
	uint8_t reserved[32];
};
#pragma pack(pop)
static_assert(sizeof(CmdHeader) == 64, "command header size");
static_assert(sizeof(CmdRecord) == 64, "command record size");

const float LOOK_SENSITIVITY = 0.0025f;
const float PITCH_LIMIT = 1.25f;
const DWORD REPEAT_MS = 280;
// subtiles: E uses nothing further away. A use is a click on the thing in the game's own view from
// above, which shows about 25 subtiles out in every direction.
const float USE_REACH = 20.0f;
const DWORD USE_HOLD_MS = 1200;  // the longest W A S D still held from before E are kept back

// The real foreground window. GetForegroundWindow is not used: sod.dll answers it on the game's
// behalf while the game is kept alive in the background.
bool isForeground()
{
	GUITHREADINFO info = { sizeof(GUITHREADINFO) };
	if (!GetGUIThreadInfo(0, &info))
		return false;
	return info.hwndActive == App.hwnd || info.hwndFocus == App.hwnd;
}

inline bool keyDown(int vk)
{
	return (GetAsyncKeyState(vk) & 0x8000) != 0;
}

// collision cell bits
const uint16_t C_BLOCK_WALK = 0x0001;
const uint16_t C_BLOCK_LOS = 0x0002;
const uint16_t C_WALL = 0x0004;
const uint16_t C_ALT_FLOOR = 0x0010;
const uint16_t C_BLANK = 0x0020;

enum Cell : uint8_t { CELL_FLOOR, CELL_ALT, CELL_LOW, CELL_WALL, CELL_VOID };

inline Cell classify(uint16_t c)
{
	if (c & C_BLANK)
		return CELL_VOID;
	if ((c & C_WALL) || ((c & C_BLOCK_WALK) && (c & C_BLOCK_LOS)))
		return CELL_WALL;
	if (c & C_BLOCK_WALK)
		return CELL_LOW;
	return (c & C_ALT_FLOOR) ? CELL_ALT : CELL_FLOOR;
}

// world units are subtiles; x = game x, z = game y, y = up
const float WALL_HEIGHT = 7.0f;
const float LOW_HEIGHT = 1.6f;
const float RELIEF_HEIGHT = 1.3f; // a thing painted into the floor that the character cannot walk on
const float EYE_HEIGHT = 3.2f;
const float FOG_FAR = 90.0f;

struct FpVertex {
	float x, y, z;
	uint8_t r, g, b, a;
	float u, v;
};

inline void pushQuad(std::vector<FpVertex>& out, const glm::vec3& a, const glm::vec3& b, const glm::vec3& c, const glm::vec3& d, glm::vec3 color, float shade)
{
	const auto to8 = [](float v) { return (uint8_t)glm::clamp(v * 255.0f, 0.0f, 255.0f); };
	const uint8_t r = to8(color.r * shade), g = to8(color.g * shade), bl = to8(color.b * shade);
	const FpVertex va = { a.x, a.y, a.z, r, g, bl, 255, 0.0f, 0.0f }, vb = { b.x, b.y, b.z, r, g, bl, 255, 0.0f, 0.0f };
	const FpVertex vc = { c.x, c.y, c.z, r, g, bl, 255, 0.0f, 0.0f }, vd = { d.x, d.y, d.z, r, g, bl, 255, 0.0f, 0.0f };
	out.push_back(va);
	out.push_back(vb);
	out.push_back(vc);
	out.push_back(va);
	out.push_back(vc);
	out.push_back(vd);
}

// A quad with texture coordinates and a brightness per corner (a, b, c, d in order around it).
inline void pushQuadUV(std::vector<FpVertex>& out, const glm::vec3 p[4], const glm::vec2 uv[4], const float shade[4])
{
	FpVertex v[4];
	for (int i = 0; i < 4; i++) {
		const uint8_t s = (uint8_t)glm::clamp(shade[i] * 200.0f, 0.0f, 255.0f); // 200 = 1.0, leaves headroom above
		v[i] = { p[i].x, p[i].y, p[i].z, s, s, s, 255, uv[i].x, uv[i].y };
	}
	out.push_back(v[0]);
	out.push_back(v[1]);
	out.push_back(v[2]);
	out.push_back(v[0]);
	out.push_back(v[2]);
	out.push_back(v[3]);
}

// ---- level art: floor / wall atlases cut from the player's own game files by tools/art/export_leveltex.py ----
const int TILE = 5;               // subtiles per game tile
const float SQUARE = (float)TILE; // a painted-rock wall square covers one tile
const int ART_TEXTURE_UNIT = 30;

struct ArtTile {
	float x, y, w, h, weight;
};

struct LevelArt {
	bool floor = false, wall = false, wall_square = false;
	GLuint floor_texture = 0, wall_texture = 0;
	float floor_w = 1.0f, floor_h = 1.0f, wall_w = 1.0f, wall_h = 1.0f;
	std::vector<ArtTile> floor_tiles, wall_tiles[2]; // wall_tiles[0] faces along x, [1] along y
	float floor_total = 0.0f, wall_total[2] = { 0.0f, 0.0f };
	DWORD tried_at = 0; // when loading was last attempted
};

std::unordered_map<int, LevelArt> g_level_art; // render thread only; an entry without art is retried later

inline float hash3(int a, int b, int c)
{
	uint32_t h = ((uint32_t)a * 374761393u) ^ ((uint32_t)b * 668265263u) ^ ((uint32_t)c * 1274126177u);
	h = (h ^ (h >> 13)) * 1274126177u;
	h ^= h >> 16;
	return (float)((double)h / 4294967296.0);
}

inline const ArtTile& weighted(const std::vector<ArtTile>& list, float total, float r)
{
	float x = r * total;
	for (const auto& t : list) {
		x -= t.weight;
		if (x < 0.0f)
			return t;
	}
	return list.back();
}

// ---- mounds: blocked cells of the floor that rise out of the ground (built in rebuildWorld) ----
// A mound has no sides. It climbs out of the ground at the edge of the blocked cells and is
// level on top only where the blocked patch is wide: a thin line of blocked cells makes a low
// ridge, a broad heap a full one.
struct ReliefMap {
	static constexpr float REACH = 1.5f; // how far in from its edge a mound reaches its full height
	static constexpr float SINK = 0.03f; // the foot starts just under the ground, so the two meet in a clean line

	int x0 = 0, y0 = 0, w = 0, h = 0; // subtiles
	std::vector<uint8_t> cells;       // 1 = rises

	bool at(int gx, int gy) const
	{
		const int x = gx - x0, y = gy - y0;
		return x >= 0 && y >= 0 && x < w && y < h && cells[(size_t)y * w + x];
	}

	// how far a point is from the nearest cell that does not rise (up to REACH)
	float edgeDistance(float px, float pz) const
	{
		float best = REACH;
		const int cx0 = (int)floorf(px - REACH), cx1 = (int)floorf(px + REACH);
		const int cz0 = (int)floorf(pz - REACH), cz1 = (int)floorf(pz + REACH);
		for (int cz = cz0; cz <= cz1; cz++)
			for (int cx = cx0; cx <= cx1; cx++) {
				if (at(cx, cz))
					continue;
				const float dx = glm::max(glm::max((float)cx - px, px - (float)(cx + 1)), 0.0f);
				const float dz = glm::max(glm::max((float)cz - pz, pz - (float)(cz + 1)), 0.0f);
				best = glm::min(best, sqrtf(dx * dx + dz * dz));
			}
		return best;
	}

	// soft lumps, so a mound is not a smooth cushion; the same at a point whichever cell asks
	static float lump(float px, float pz)
	{
		const float sx = px * 1.3f, sz = pz * 1.3f;
		const int ix = (int)floorf(sx), iz = (int)floorf(sz);
		float fx = sx - (float)ix, fz = sz - (float)iz;
		fx = fx * fx * (3.0f - 2.0f * fx);
		fz = fz * fz * (3.0f - 2.0f * fz);
		const float a = hash3(ix, iz, 7), b = hash3(ix + 1, iz, 7), c = hash3(ix, iz + 1, 7), d = hash3(ix + 1, iz + 1, 7);
		return glm::mix(glm::mix(a, b, fx), glm::mix(c, d, fx), fz) * 2.0f - 1.0f;
	}

	// the mound's surface at a point, and how far up its side the point is (0 foot .. 1 top);
	// a little under the ground at the foot
	float surface(float px, float pz, float* rise = nullptr) const
	{
		const float t = edgeDistance(px, pz) / REACH;
		const float s = 0.5f * (sinf(t * 1.5707963f) + t * t * (3.0f - 2.0f * t)); // steep at the foot, level on top
		if (rise)
			*rise = s;
		return RELIEF_HEIGHT * s * (1.0f + 0.2f * lump(px, pz)) - SINK;
	}

	// where something standing at a point stands: on a mound, its top; elsewhere the ground
	float ground(float px, float pz) const
	{
		if (!at((int)floorf(px), (int)floorf(pz)))
			return 0.0f;
		return glm::max(surface(px, pz), 0.0f);
	}
};
ReliefMap g_relief; // render thread only

// A mesh drawn from a list of indices: the mounds, where each point is shared by up to four quads.
struct IndexedPart {
	std::vector<FpVertex> verts;
	std::vector<uint32_t> indices;
};

// What rebuildWorld builds the world in. The game is a 32-bit program without the large address
// flag, so it has 2 GB, and the game and the video driver use much of it. Once the world is on
// the card these lists are given back, not kept for the next build.
struct WorldParts {
	std::vector<FpVertex> plain;
	std::map<int, std::vector<FpVertex>> floor_art, wall_art; // by level
	std::map<int, IndexedPart> relief_art;                    // by page of the map tiles' atlas

	void release()
	{
		std::vector<FpVertex>().swap(plain);
		floor_art.clear();
		wall_art.clear();
		relief_art.clear();
	}
};
WorldParts g_world_parts; // render thread only

GLuint loadArtTexture(const std::string& path, float* width, float* height)
{
	int w = 0, h = 0, channels = 0;
	stbi_set_flip_vertically_on_load(0);
	uint8_t* pixels = stbi_load(path.c_str(), &w, &h, &channels, 4);
	if (!pixels)
		return 0;

	GLint prev_unit = 0;
	glGetIntegerv(GL_ACTIVE_TEXTURE, &prev_unit);
	glActiveTexture(GL_TEXTURE0 + ART_TEXTURE_UNIT);

	GLuint id = 0;
	glGenTextures(1, &id);
	glBindTexture(GL_TEXTURE_2D, id);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 3); // the atlas gutters are 8 px wide
	glGenerateMipmap(GL_TEXTURE_2D);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	if (GLEW_EXT_texture_filter_anisotropic)
		glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY_EXT, 8.0f);

	glActiveTexture(prev_unit);
	stbi_image_free(pixels);
	*width = (float)w;
	*height = (float)h;
	return id;
}

// The art of a level, loading it on first use. Returns an entry with floor = wall = false when the
// level has not been exported (yet).
LevelArt& levelArt(int level_no)
{
	auto it = g_level_art.find(level_no);
	const DWORD now = GetTickCount();
	if (it != g_level_art.end() && (it->second.floor || it->second.wall || now - it->second.tried_at < 4000))
		return it->second;

	LevelArt& art = g_level_art[level_no];
	art = LevelArt(); // a fresh attempt: nothing left over from an earlier, incomplete one
	art.tried_at = now ? now : 1;
	const std::string dir = "sod-art\\leveltex\\" + std::to_string(level_no) + "\\";
	std::ifstream file(dir + "level.json");
	if (!file.is_open())
		return art;

	const auto doc = nlohmann::json::parse(file, nullptr, false);
	if (doc.is_discarded() || !doc.is_object())
		return art;

	const auto tile = [](const nlohmann::json& t) {
		return ArtTile { t.value("x", 0.0f), t.value("y", 0.0f), t.value("w", 1.0f), t.value("h", 1.0f), glm::max(t.value("weight", 1.0f), 0.01f) };
	};

	if (doc.contains("floor") && doc["floor"].is_object() && doc["floor"].contains("tiles")) {
		for (const auto& t : doc["floor"]["tiles"]) {
			art.floor_tiles.push_back(tile(t));
			art.floor_total += art.floor_tiles.back().weight;
		}
		if (!art.floor_tiles.empty()) {
			art.floor_texture = loadArtTexture(dir + "floor.png", &art.floor_w, &art.floor_h);
			art.floor = art.floor_texture != 0;
		}
	}
	if (doc.contains("wall") && doc["wall"].is_object() && doc["wall"].contains("tiles")) {
		art.wall_square = doc["wall"].value("mode", std::string("tile")) == "square";
		for (const auto& t : doc["wall"]["tiles"]) {
			const std::string axis = t.value("axis", std::string("any"));
			for (int a = 0; a < 2; a++) {
				if (axis == "any" || axis == (a == 0 ? "x" : "y")) {
					art.wall_tiles[a].push_back(tile(t));
					art.wall_total[a] += art.wall_tiles[a].back().weight;
				}
			}
		}
		if (!art.wall_tiles[0].empty() || !art.wall_tiles[1].empty()) {
			art.wall_texture = loadArtTexture(dir + "wall.png", &art.wall_w, &art.wall_h);
			art.wall = art.wall_texture != 0;
		}
	}
	trace_log("FirstPerson: level %d art loaded (floor %d, wall %d).", level_no, (int)art.floor, (int)art.wall);
	return art;
}

// Wall art for a level. Towns have none of their own (their walls are fences, tents and wagons,
// which are objects in the game), so they borrow the walls of the area just outside.
int wallArtLevel(int level_no)
{
	for (int offset = 0; offset <= 3; offset++) {
		const LevelArt& art = levelArt(level_no + offset);
		if (art.wall)
			return level_no + offset;
	}
	return -1;
}

// ---- unit sprites ----
const float SPRITE_SCALE = 0.047f; // subtiles per sheet pixel (the art's own scale)
const char* MONSTER_MODES[16] = { "DT", "NU", "WL", "GH", "A1", "A2", "BL", "SC", "S1", "S2", "S3", "S4", "DD", "KB", "SQ", "RN" };
const char* OBJECT_MODES[8] = { "NU", "OP", "ON", "S1", "S2", "S3", "S4", "S5" };

// The sheet row for a unit facing `dir64` (the game's 64 directions), seen from the camera. The art
// was drawn for the isometric camera, so the facing is taken relative to the line of sight and
// re-expressed for that camera.
inline int spriteDirection(const Sprite& sprite, int dir64, float ux, float uz, float cam_x, float cam_z)
{
	if (sprite.directions <= 1 || dir64 < 0)
		return 0;
	const float step = glm::two_pi<float>() / 64.0f;
	const float facing = ((float)dir64 + 0.5f) * step + glm::quarter_pi<float>();
	const float sight = atan2f(uz - cam_z, ux - cam_x);
	const float relative = facing - sight + sprite.iso_yaw;
	int d = (int)roundf((relative - glm::quarter_pi<float>()) / step - 0.5f) % 64;
	if (d < 0)
		d += 64;
	return sprite.dir64[d];
}

// The sheet frame from the unit's own animation clock, or from the time when it has none.
inline int spriteFrame(const Sprite& sprite, uint32_t gfx_frame, uint32_t frame_count, bool loop, double time)
{
	const int count = sprite.frames;
	if (count <= 1)
		return 0;

	const float frame = frame_count >= 256 ? (float)gfx_frame / 256.0f : (float)gfx_frame;
	const int frames = frame_count >= 256 ? (int)(frame_count / 256) : (int)frame_count;
	int f;
	if (frames > 0)
		f = frames == count ? (int)frame : (int)(frame / (float)frames * (float)count);
	else if (loop)
		f = (int)(time * sprite.fps);
	else
		return count - 1;
	return loop ? ((f % count) + count) % count : glm::clamp(f, 0, count - 1);
}

// An upright picture standing on the ground at (ux, uz), turned to face the camera.
inline void pushBillboard(std::vector<FpVertex>& out, const SpriteCell& cell, float ux, float uz, float base, float cam_x, float cam_z, float shade)
{
	if (!cell.valid)
		return;
	glm::vec2 sight = { ux - cam_x, uz - cam_z };
	const float len = glm::length(sight);
	if (len < 0.05f)
		return;
	sight /= len;
	const glm::vec3 right = { -sight.y, 0.0f, sight.x }, up = { 0.0f, 1.0f, 0.0f };
	const glm::vec3 centre = glm::vec3(ux, base, uz) + right * (cell.cx * SPRITE_SCALE) + up * (cell.cy * SPRITE_SCALE);
	const glm::vec3 half_w = right * (cell.w * SPRITE_SCALE * 0.5f), half_h = up * (cell.h * SPRITE_SCALE * 0.5f);
	const glm::vec3 p[4] = { centre - half_w - half_h, centre + half_w - half_h, centre + half_w + half_h, centre - half_w + half_h };
	const glm::vec2 uv[4] = { { cell.u0, cell.v1 }, { cell.u1, cell.v1 }, { cell.u1, cell.v0 }, { cell.u0, cell.v0 } };
	const float shades[4] = { shade, shade, shade, shade };
	pushQuadUV(out, p, uv, shades);
}

// A picture lying on the ground (dropped items): its top points away from the camera. The art was
// drawn for the isometric view, where depth is squashed to half, so it is stretched back out.
inline void pushGroundSprite(std::vector<FpVertex>& out, const SpriteCell& cell, float ux, float uz, float cam_x, float cam_z, float shade)
{
	if (!cell.valid)
		return;
	glm::vec2 sight = { ux - cam_x, uz - cam_z };
	const float len = glm::length(sight);
	if (len < 0.05f)
		return;
	sight /= len;
	const glm::vec3 right = { -sight.y, 0.0f, sight.x }, away = { sight.x, 0.0f, sight.y };
	const glm::vec3 centre = glm::vec3(ux, 0.06f, uz) + right * (cell.cx * SPRITE_SCALE) + away * (cell.cy * SPRITE_SCALE * 2.0f);
	const glm::vec3 half_w = right * (cell.w * SPRITE_SCALE * 0.5f), half_h = away * (cell.h * SPRITE_SCALE);
	const glm::vec3 p[4] = { centre - half_w - half_h, centre + half_w - half_h, centre + half_w + half_h, centre - half_w + half_h };
	const glm::vec2 uv[4] = { { cell.u0, cell.v1 }, { cell.u1, cell.v1 }, { cell.u1, cell.v0 }, { cell.u0, cell.v0 } };
	const float shades[4] = { shade, shade, shade, shade };
	pushQuadUV(out, p, uv, shades);
}

// A picture the game paints on the ground around a unit (an aura's ring, a shout's circle), laid
// flat: each pixel goes back to the spot of ground the isometric view showed there, subtile (x, y)
// being drawn at ((x - y) * 16, (x + y) * 8). `sx` / `sy` shift it in screen pixels as the game would.
inline void pushIsoGround(std::vector<FpVertex>& out, const SpriteCell& cell, float ux, float uz, float y, float sx, float sy, float shade)
{
	if (!cell.valid)
		return;
	const float left = cell.cx - cell.w * 0.5f + sx, right = cell.cx + cell.w * 0.5f + sx;
	const float top = -(cell.cy + cell.h * 0.5f) + sy, bottom = -(cell.cy - cell.h * 0.5f) + sy; // screen y grows downwards
	const auto ground = [&](float px, float py) { return glm::vec3(ux + px / 32.0f + py / 16.0f, y, uz + py / 16.0f - px / 32.0f); };
	const glm::vec3 p[4] = { ground(left, bottom), ground(right, bottom), ground(right, top), ground(left, top) };
	const glm::vec2 uv[4] = { { cell.u0, cell.v1 }, { cell.u1, cell.v1 }, { cell.u1, cell.v0 }, { cell.u0, cell.v0 } };
	const float shades[4] = { shade, shade, shade, shade };
	pushQuadUV(out, p, uv, shades);
}

// Modes in which a unit uses a skill: attacks, casting, throwing, kicking, the special moves.
inline bool skillMode(bool player, uint16_t mode)
{
	if (player)
		return mode == 7 || mode == 8 || (mode >= 10 && mode <= 16); // A1 A2 SC TH KK S1-S4
	return mode == 4 || mode == 5 || (mode >= 7 && mode <= 11);      // A1 A2 SC S1-S4
}

// Where the camera is, published for tools that measure how the view behaves.
#pragma pack(push, 1)
struct ViewBlock {
	uint32_t seq;        // one more every frame drawn
	float cam_x, cam_y;  // camera, in subtiles
	float body_x, body_y; // the character under it
	float yaw, pitch;
	float move_x, move_y; // where the player is steering, zero when no key is held
};
#pragma pack(pop)

void publishView(const ViewBlock& view)
{
	static ViewBlock* block = nullptr;
	static bool tried = false;
	if (!tried) {
		tried = true;
		if (HANDLE map = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(ViewBlock), L"Local\\SoD_View_v1"))
			block = (ViewBlock*)MapViewOfFile(map, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(ViewBlock));
	}
	if (block) {
		const uint32_t seq = block->seq + 1;
		*block = view;
		block->seq = seq;
	}
}

// The loaded rooms with every cell classified, kept from the last world build (render thread).
struct Room {
	int level, x, y, w, h;
	std::vector<uint8_t> cls;
};
std::vector<Room> g_rooms;

// True where the character can stand: a floor cell of a loaded room.
inline bool walkable(float x, float y)
{
	const int gx = (int)floorf(x), gy = (int)floorf(y);
	for (const auto& room : g_rooms)
		if (gx >= room.x && gy >= room.y && gx < room.x + room.w && gy < room.y + room.h) {
			const uint8_t cls = room.cls[(size_t)(gy - room.y) * room.w + (gx - room.x)];
			return cls == CELL_FLOOR || cls == CELL_ALT;
		}
	return false;
}

// An axis-aligned box from (x0, 0, z0) to (x1, h, z1): top and four sides.
inline void pushBox(std::vector<FpVertex>& out, float x0, float z0, float x1, float z1, float y0, float h, glm::vec3 color)
{
	pushQuad(out, { x0, h, z0 }, { x1, h, z0 }, { x1, h, z1 }, { x0, h, z1 }, color, 1.0f);
	pushQuad(out, { x0, y0, z0 }, { x1, y0, z0 }, { x1, h, z0 }, { x0, h, z0 }, color, 0.62f);
	pushQuad(out, { x0, y0, z1 }, { x1, y0, z1 }, { x1, h, z1 }, { x0, h, z1 }, color, 0.74f);
	pushQuad(out, { x0, y0, z0 }, { x0, y0, z1 }, { x0, h, z1 }, { x0, h, z0 }, color, 0.5f);
	pushQuad(out, { x1, y0, z0 }, { x1, y0, z1 }, { x1, h, z1 }, { x1, h, z0 }, color, 0.86f);
}

const char* VS_SRC = R"(#version 330 core
layout(location = 0) in vec3 a_Position;
layout(location = 1) in vec4 a_Color;
layout(location = 2) in vec2 a_TexCoord;
uniform mat4 u_MVP;
out vec4 v_Color;
out vec3 v_World;
out vec2 v_TexCoord;
void main() {
	v_Color = a_Color;
	v_World = a_Position;
	v_TexCoord = a_TexCoord;
	gl_Position = u_MVP * vec4(a_Position, 1.0);
})";

const char* FS_SRC = R"(#version 330 core
in vec4 v_Color;
in vec3 v_World;
in vec2 v_TexCoord;
uniform vec3 u_Eye;
uniform float u_Textured; // 1 = sample the level art
uniform float u_Additive; // 1 = drawn with additive blending (glowing missiles)
uniform sampler2D u_Texture;
uniform vec4 u_Fog; // rgb = colour, a = far distance
uniform float u_Sharp; // 1 = keep the art's own pixels crisp when it is drawn larger than it was made
uniform float u_Flat; // 1 = plain colour (overlay), 2 = text: the colour, with the texture's alpha, 3 = edge glow
layout(location = 0) out vec4 FragColor;
void main() {
	if (u_Flat > 2.5) {
		// the colour along the edges of the view, a rim about a tenth of it deep (added)
		vec2 p = abs(v_World.xy);
		float edge = smoothstep(0.78, 1.12, pow(pow(p.x, 4.0) + pow(p.y, 4.0), 0.25));
		FragColor = vec4(v_Color.rgb * edge, 1.0);
		return;
	}
	if (u_Flat > 1.5) {
		FragColor = vec4(v_Color.rgb, v_Color.a * texture(u_Texture, v_TexCoord).a);
		return;
	}
	if (u_Flat > 0.5) {
		FragColor = vec4(v_Color.rgb, v_Color.a);
		return;
	}
	vec3 color = v_Color.rgb;
	if (u_Textured > 0.5) {
		// The game's art is small: a monster is some eighty pixels high. Stretched over half the
		// screen with plain filtering it turns to blur. Here each pixel of the art stays a clean
		// square, with only its edge smoothed over one pixel of the screen; from further away,
		// where several pixels of the art fall into one of the screen, it is filtered as before.
		vec2 uv = v_TexCoord;
		if (u_Sharp > 0.5) {
			vec2 size = vec2(textureSize(u_Texture, 0));
			vec2 at = uv * size;
			vec2 span = max(fwidth(at), vec2(0.0001));
			vec2 seam = floor(at + 0.5);
			uv = (seam + clamp((at - seam) / span, -0.5, 0.5)) / size;
		}
		vec4 art = texture(u_Texture, uv);
		if (art.a < 0.5)
			discard; // wall art keeps its own outline
		color = art.rgb * v_Color.rgb * (255.0 / 200.0) * 1.35; // vertex 200 = full brightness; the art is authored dark
	} else {
		// faint subtile grid so flat surfaces read as ground / wall, not as a solid fill
		vec2 g = abs(fract(v_World.xz * 0.2) - 0.5);
		float line = smoothstep(0.47, 0.5, max(g.x, g.y));
		float band = smoothstep(0.46, 0.5, abs(fract(v_World.y * 0.5) - 0.5));
		color *= 1.0 - 0.18 * line - 0.10 * band;
	}
	float dist = length(v_World - u_Eye);
	float fog = clamp(dist / u_Fog.a, 0.0, 1.0);
	color = u_Additive > 0.5 ? color * (1.0 - fog * fog) : mix(color, u_Fog.rgb, fog * fog);
	FragColor = vec4(color, 1.0);
})";

GLuint compile(GLenum type, const char* src)
{
	GLuint id = glCreateShader(type);
	glShaderSource(id, 1, &src, NULL);
	glCompileShader(id);
	GLint ok = GL_FALSE;
	glGetShaderiv(id, GL_COMPILE_STATUS, &ok);
	if (!ok) {
		char msg[512] = { 0 };
		glGetShaderInfoLog(id, sizeof(msg) - 1, NULL, msg);
		error_log("FirstPerson: shader compile failed: %s", msg);
		glDeleteShader(id);
		return 0;
	}
	return id;
}

}

// ---- state mapping ----

bool FirstPerson::openMapping()
{
	if (m_base)
		return true;

	const DWORD now = GetTickCount();
	if (m_next_open_try && (int32_t)(now - m_next_open_try) < 0)
		return false;
	m_next_open_try = now + 1000;

	HANDLE map = OpenFileMappingW(FILE_MAP_READ, FALSE, STATE_NAME);
	if (!map)
		return false;

	const uint8_t* base = (const uint8_t*)MapViewOfFile(map, FILE_MAP_READ, 0, 0, 0);
	if (!base) {
		CloseHandle(map);
		return false;
	}

	const auto hdr = (const StateHeader*)base;
	const bool ok = hdr->magic == STATE_MAGIC && hdr->version == STATE_VERSION && hdr->unitRecordSize == sizeof(UnitRecord)
		&& hdr->playerRecordSize == sizeof(PlayerRecord) && hdr->roomEntrySize == sizeof(RoomEntry) && hdr->maxUnits <= 4096 && hdr->maxRooms <= 4096;
	if (!ok) {
		UnmapViewOfFile(base);
		CloseHandle(map);
		return false;
	}

	m_map = map;
	m_base = base;
	trace_log("FirstPerson: attached to the game state (%u bytes).", hdr->totalSize);
	return true;
}

bool FirstPerson::isActive()
{
	if (!m_enabled || App.game.screen != GameScreen::InGame)
		return false;
	if (!openMapping())
		return false;

	const auto hdr = (const StateHeader*)m_base;
	if (hdr->status != STATUS_ACTIVE)
		return false;
	const uint32_t off = hdr->frameIndex ? hdr->frameBuf1Off : hdr->frameBuf0Off;
	return ((const FrameMeta*)(m_base + off))->valid == 1;
}

void FirstPerson::toggle()
{
	m_enabled = !m_enabled;
}

// ---- draw trace ----

void FirstPerson::traceBegin()
{
	m_tracing = m_trace_armed;
	m_trace_armed = false;
	if (m_tracing)
		m_trace.clear();
}

void FirstPerson::traceEnd(uint32_t vertices)
{
	if (!m_tracing)
		return;
	m_tracing = false;

	static const char* names[] = { "?", "Image", "PerspectiveImage", "ShiftedImage", "VerticalCropImage", "ClippedImage", "ImageFast", "Shadow",
		"SolidRect", "Line", "GroundTile", "WallTile", "TransWallTile", "ShadowTile" };
	static const char* stages[] = { "World", "UI", "Map", "HUD", "CursorItem", "Cursor" };

	FILE* file = nullptr;
	if (fopen_s(&file, "sod-fp-debug.txt", "w") != 0 || !file)
		return;

	const glm::ivec2 cursor = d2::getCursorPos();
	fprintf(file, "first person frame=%d looking=%d panel=%u game=%ux%u cursor=(%d,%d) vertices=%u records=%u\n", (int)m_frame_active, (int)m_look,
		*d2::screen_shift, App.game.size.x, App.game.size.y, cursor.x, cursor.y, vertices, (uint32_t)m_trace.size());
	fprintf(file, "camera yaw=%.5f pitch=%.5f player=(%.3f,%.3f)\n", (float)m_yaw, (float)m_pitch, (float)m_player_x, (float)m_player_y);

	// summary: calls per stage and function
	uint32_t counts[6][14] = {};
	for (const auto& r : m_trace)
		if (r.fn < 14 && r.stage < 6)
			counts[r.stage][r.fn]++;
	for (int s = 0; s < 6; s++)
		for (int f = 1; f < 14; f++)
			if (counts[s][f])
				fprintf(file, "summary stage=%s fn=%s calls=%u\n", stages[s], names[f], counts[s][f]);

	// detail: every stage change, and every draw outside the world stage
	for (const auto& r : m_trace) {
		if (r.fn >= 100)
			fprintf(file, "-- stage -> %s (vertices so far %u)\n", stages[glm::min((int)r.fn - 100, 5)], r.vertices);
		else if (r.stage != 0 && r.fn < 14)
			fprintf(file, "%s %s x=%d y=%d\n", stages[glm::min((int)r.stage, 5)], names[r.fn], r.x, r.y);
	}
	fclose(file);
}

// ---- input (game thread) ----

bool FirstPerson::openCommands()
{
	if (m_cmd)
		return true;

	const DWORD now = GetTickCount();
	if (m_next_cmd_try && (int32_t)(now - m_next_cmd_try) < 0)
		return false;
	m_next_cmd_try = now + 1000;

	HANDLE map = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, CMD_NAME);
	if (!map)
		return false;
	uint8_t* base = (uint8_t*)MapViewOfFile(map, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, 0);
	const auto hdr = (const CmdHeader*)base;
	if (!base || hdr->magic != CMD_MAGIC || hdr->version != CMD_VERSION || hdr->recordSize != sizeof(CmdRecord) || !hdr->capacity || hdr->capacity > 4096) {
		if (base)
			UnmapViewOfFile(base);
		CloseHandle(map);
		return false;
	}

	m_cmd_map = map;
	m_cmd = base;
	trace_log("FirstPerson: attached to the command channel.");
	return true;
}

void FirstPerson::writeMove(float x, float y)
{
	if (!m_cmd)
		return;
	const auto hdr = (CmdHeader*)m_cmd;
	hdr->moveX = x;
	hdr->moveY = y;
	MemoryBarrier();
	hdr->moveSeq = hdr->moveSeq + 1;
}

// The move keys came up (or the way is shut, or look mode ended). With sod.dll's direct movement
// it is told where the camera is first, and walks the character there: the camera runs a little
// ahead so it can start at once, and this way the character catches up with it instead of the
// view sliding back to wherever the game would have stopped the character.
void FirstPerson::stopMoving()
{
	if (m_moving && m_cmd) {
		const bool direct = ((const CmdHeader*)m_cmd)->dllFlags & CMD_DLL_DIRECT;
		const DWORD drawn = m_cam_spot_tick;
		// a camera that is not being drawn is not where the player is (signed: the render thread's
		// tick can be a step ahead of this thread's)
		if (direct && drawn && (int32_t)(GetTickCount() - drawn) < 250)
			pushRecord(OP_STOP_AT, 0, 0, m_cam_spot_x, m_cam_spot_y, 0);
		writeMove(0.0f, 0.0f);
	}
	m_moving = false;
	m_move_x = m_move_y = 0.0f;
}

void FirstPerson::pushRecord(uint32_t opcode, uint32_t a0, uint32_t a1, float x, float y, uint32_t a2)
{
	if (!m_cmd)
		return;
	const auto hdr = (CmdHeader*)m_cmd;
	const uint32_t index = hdr->writeIndex;
	CmdRecord rec = {};
	rec.opcode = opcode;
	rec.seq = index;
	rec.a0 = a0;
	rec.a1 = a1;
	rec.x = x;
	rec.y = y;
	rec.a2 = a2;
	memcpy(m_cmd + sizeof(CmdHeader) + (index % hdr->capacity) * sizeof(CmdRecord), &rec, sizeof(rec));
	MemoryBarrier();
	hdr->writeIndex = index + 1;
}

void FirstPerson::setLook(bool on)
{
	if (m_look == on)
		return;
	m_look = on;

	if (m_cmd) {
		const auto hdr = (CmdHeader*)m_cmd;
		hdr->flags = on ? (hdr->flags | CMD_FLAG_LOOK) : (hdr->flags & ~CMD_FLAG_LOOK);
	}
	if (!on) {
		stopMoving();
		m_lmb = m_rmb = false;
	}
}

// One click of a mouse button: the unit under the crosshair if there is one, otherwise the ground ahead.
void FirstPerson::fire(uint32_t hand)
{
	m_fire_tick = GetTickCount();
	const float yaw = m_yaw;
	if (m_target_valid) {
		const uint32_t type = m_target_type, id = m_target_id;
		const bool hostile = type == 1 && !(m_target_flags & 0x0010);
		if (hostile || hand == 1)
			pushRecord(OP_ATTACK_UNIT, hand, type, m_target_x, m_target_y, id);
		else
			pushRecord(OP_INTERACT, 0, type, m_target_x, m_target_y, id);
		return;
	}
	const float reach = hand == 0 ? 3.0f : 12.0f;
	pushRecord(OP_CAST_AT, hand, 0, m_player_x + cosf(yaw) * reach, m_player_y + sinf(yaw) * reach, 0);
}

// E: use what the view points at (the render thread picks it, see USE_REACH): talk to an NPC, open
// the stash or a chest, break a jar, take a waypoint or a portal, pick up an item, go through an
// exit. It is the same as a click on it, so the game walks the character up to it first.
void FirstPerson::use()
{
	AcquireSRWLockShared(&m_use_lock);
	const bool valid = m_use_valid;
	const uint32_t type = m_use_type, id = m_use_id;
	const float x = m_use_x, y = m_use_y;
	ReleaseSRWLockShared(&m_use_lock);
	if (!valid)
		return;
	// A held W would walk the character off its way there: stop first, and hold the keys back
	// until they are let go (update).
	stopMoving();
	m_use_hold = true;
	m_use_tick = GetTickCount();
	pushRecord(OP_INTERACT, 0, type, x, y, id);
}

// ---- what blocks the way, read from the game's own collision map (game thread) ----
namespace {

const float STEER_DISTANCE = 12.0f; // sod.dll's steer_distance: a move vector of length 1 puts the pointer this far ahead
const uint16_t C_STOPS_PLAYER = 0x0001 | 0x0008 | 0x0400 | 0x0800; // no walking, no players, an object, a closed door

struct SteerRoom {
	int x, y, w, h;
	uint32_t cells;
};
std::vector<SteerRoom> g_steer_rooms;
DWORD g_steer_rooms_tick = 0;

bool peek(void* dst, uint32_t src, size_t size)
{
	if (src < 0x10000)
		return false;
	__try {
		memcpy(dst, (const void*)(uintptr_t)src, size);
		return true;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
}

// The character's own place, to a fraction of a subtile.
bool playerPlace(glm::vec2* pos)
{
	const uint32_t player = (uint32_t)(uintptr_t)d2::getPlayerUnit();
	uint32_t path = 0, xy[2] = { 0, 0 };
	if (!player || !peek(&path, player + 0x2C, 4) || !peek(xy, path, 8))
		return false;
	*pos = { (float)xy[0] / 65536.0f, (float)xy[1] / 65536.0f };
	return true;
}

void addSteerRoom(uint32_t ptr)
{
	uint32_t coll = 0, map[10];
	if (!ptr || !peek(&coll, ptr + 0x20, 4) || !peek(map, coll, sizeof(map)))
		return;
	if (!map[2] || map[2] > 512 || !map[3] || map[3] > 512 || !map[8])
		return;
	for (const auto& known : g_steer_rooms)
		if (known.cells == map[8])
			return;
	g_steer_rooms.push_back({ (int)map[0], (int)map[1], (int)map[2], (int)map[3], map[8] });
}

// The collision maps of the character's room and the rooms around it.
void steerRooms()
{
	const DWORD now = GetTickCount();
	if (!g_steer_rooms.empty() && now - g_steer_rooms_tick < 100)
		return;
	g_steer_rooms_tick = now;
	g_steer_rooms.clear();

	const uint32_t player = (uint32_t)(uintptr_t)d2::getPlayerUnit();
	uint32_t path = 0, room = 0, list = 0, count = 0, around[64];
	if (!player || !peek(&path, player + 0x2C, 4) || !peek(&room, path + 0x1C, 4) || !room)
		return;
	addSteerRoom(room);
	if (peek(&list, room, 4) && peek(&count, room + 0x24, 4) && count && count <= 64 && peek(around, list, count * 4))
		for (uint32_t i = 0; i < count; i++)
			addSteerRoom(around[i]);
}

// True when the character cannot stand on this subtile. What is not loaded counts as blocked.
bool stopsPlayer(int x, int y)
{
	for (const auto& room : g_steer_rooms)
		if (x >= room.x && y >= room.y && x < room.x + room.w && y < room.y + room.h) {
			uint16_t cell = 0;
			if (!peek(&cell, room.cells + (uint32_t)((y - room.y) * room.w + (x - room.x)) * 2, 2))
				return true;
			return (cell & C_STOPS_PLAYER) != 0;
		}
	return true;
}

// How far the way is clear from `from` towards `dir`, up to `max`.
float clearAhead(glm::vec2 from, glm::vec2 dir, float max)
{
	for (float d = 0.75f; d <= max; d += 0.25f) {
		const glm::vec2 p = from + dir * d;
		if (stopsPlayer((int)floorf(p.x), (int)floorf(p.y)))
			return d - 0.25f;
	}
	return max;
}

}

// How far ahead the game's pointer may be held while walking in a direction: `reach` subtiles, or
// less. The game treats a press on a unit as "talk to / attack / pick up", so the pointer is kept
// off every unit's sprite.
float FirstPerson::steerReach(float dir_x, float dir_y, float reach)
{
	const auto hdr = (const StateHeader*)m_base;
	const uint32_t size = sizeof(FrameMeta) + sizeof(PlayerRecord) + hdr->maxUnits * sizeof(UnitRecord);
	if (m_input_copy.size() != size)
		m_input_copy.resize(size);

	bool ok = false;
	for (int i = 0; i < 3 && !ok; i++) {
		const uint32_t seq = hdr->frameSeq;
		memcpy(m_input_copy.data(), m_base + (hdr->frameIndex ? hdr->frameBuf1Off : hdr->frameBuf0Off), size);
		MemoryBarrier();
		ok = hdr->frameSeq == seq;
	}
	const auto meta = (const FrameMeta*)m_input_copy.data();
	if (!ok || meta->valid != 1)
		return reach;

	const auto player = (const PlayerRecord*)(m_input_copy.data() + sizeof(FrameMeta));
	const auto units = (const UnitRecord*)(m_input_copy.data() + sizeof(FrameMeta) + sizeof(PlayerRecord));
	const float px = (float)player->px / 65536.0f, py = (float)player->py / 65536.0f;
	const uint32_t count = glm::min((uint32_t)meta->unitCount, hdr->maxUnits);

	const float candidates[] = { reach, reach * 0.75f, reach * 0.55f, reach * 0.35f, 1.8f, 1.2f };
	for (const float candidate : candidates) {
		if (candidate > reach)
			continue;
		const float tx = px + dir_x * candidate, ty = py + dir_y * candidate;
		bool blocked = false;
		for (uint32_t i = 0; i < count && !blocked; i++) {
			const UnitRecord& u = units[i];
			if (u.type == 3 || (u.type == 1 && (u.mode == 0 || u.mode == 12)))
				continue;
			// The player's own creatures run alongside, and a click on one only walks up to it.
			// Counted as in the way, a Shadow kept the pointer within a step or two of the
			// character, where a direction can only be named to the nearest 15 degrees or so.
			if (u.type == 1) {
				const MonsterInfo* info = SpriteStore::Instance().monster(u.txt);
				if (info && (info->ally || info->hidden))
					continue;
			}
			const bool dynamic = u.type == 0 || u.type == 1;
			const float ux = dynamic ? (float)u.px / 65536.0f : (float)u.px + 0.5f;
			const float uy = dynamic ? (float)u.py / 65536.0f : (float)u.py + 0.5f;
			// pointer position relative to the unit's feet, in the game's isometric screen pixels
			const float sx = ((tx - ux) - (ty - uy)) * 16.0f;
			const float sy = ((tx - ux) + (ty - uy)) * 8.0f;
			blocked = fabsf(sx) < 56.0f && sy > -112.0f && sy < 30.0f;
		}
		if (!blocked)
			return candidate;
	}
	return glm::min(reach, 1.2f);
}

// Walks the character the way `dir` points (game thread, once a frame while a move key is down).
// sod.dll turns the vector into the game's own move calls (move=direct), or, without those, into
// a held click with the pointer this far ahead (move=vmouse). The rest of this note is about the
// click; a direct target is a whole subtile too, so the line and the trim help it the same way.
//
// The game moves a character by clicks, and takes a click as a whole subtile. Two things follow.
// A click close by cannot name a direction well: 4 subtiles out, the nearest whole subtile is up
// to 7 degrees off. And each new click is measured from wherever the character has got to, so the
// errors add up to a path that wanders. So the pointer is held far ahead, and it is held on one
// line: the line through the place where this direction was first taken. A character that has
// drifted off the line is steered back to it.
//
// The pointer never goes through a wall. The game would find a way round, and the character
// would run off sideways to get there. It stops short of what blocks the way, and when the way is
// blocked right in front, the character slides along the wall instead, the way a first-person
// game does. Walls in the game's world run along x and y, so that is where sliding goes.
void FirstPerson::steer(glm::vec2 dir)
{
	// With sod.dll's direct movement there is no pointer, so nothing to keep off units: the reach
	// only has to stop short of walls.
	const bool direct = m_cmd && (((const CmdHeader*)m_cmd)->dllFlags & CMD_DLL_DIRECT);
	glm::vec2 pos;
	if (!playerPlace(&pos)) {
		const float reach = direct ? 4.0f : steerReach(dir.x, dir.y, 4.0f);
		writeMove(dir.x * reach / STEER_DISTANCE, dir.y * reach / STEER_DISTANCE);
		m_moving = true;
		m_move_x = dir.x;
		m_move_y = dir.y;
		return;
	}
	steerRooms();

	// the line: started anew when the direction turns by a degree, or the character is far off it
	const glm::vec2 off = pos - m_line_from;
	const float along = glm::dot(off, m_line_dir);
	const float aside = glm::length(off - m_line_dir * along);
	if (!m_moving || m_sliding || glm::dot(dir, m_line_dir) < 0.99985f || aside > 2.5f || along < -1.0f) {
		m_line_from = pos;
		m_line_dir = dir;
	}
	const glm::vec2 foot = m_line_from + m_line_dir * glm::max(glm::dot(pos - m_line_from, m_line_dir), 0.0f);

	// Where the game takes a click is not quite where it was aimed: measured, the subtile it
	// walks to lies a fraction of a subtile off, always the same way on screen. Left alone, that
	// bends every walk by a few degrees, more in some directions than in others. So the aim is
	// trimmed by how far the character is off its line (twice over, to come back quickly), and
	// what stays off is added up into a standing correction that outlasts the line: it belongs
	// to the screen, not to the direction.
	const glm::vec2 across = { -m_line_dir.y, m_line_dir.x };
	const float astray = glm::dot(pos - m_line_from, across);
	const DWORD tick = GetTickCount();
	const float dt = m_steer_tick ? glm::min((float)(tick - m_steer_tick) / 1000.0f, 0.1f) : 0.0f;
	m_steer_tick = tick;
	if (m_moving && !m_sliding && m_way_clear && fabsf(astray) < 1.5f) {
		m_aim_trim += across * (astray * 4.0f * dt);
		const float trim = glm::length(m_aim_trim);
		if (trim > 1.5f)
			m_aim_trim *= 1.5f / trim;
	}
	const glm::vec2 to = foot + m_line_dir * STEER_DISTANCE - across * (astray * 2.0f) - m_aim_trim - pos;
	const float want = glm::min(glm::length(to), STEER_DISTANCE);
	glm::vec2 go = glm::normalize(to);

	float clear = clearAhead(pos, go, want);
	m_way_clear = clear >= want - 0.5f; // nothing in the way: being off the line is the click's doing, not a wall's
	m_sliding = false;
	if (clear < 1.75f) {
		// blocked right in front: along the wall, whichever way the keys lean more
		const glm::vec2 sides[2] = { { dir.x > 0.0f ? 1.0f : -1.0f, 0.0f }, { 0.0f, dir.y > 0.0f ? 1.0f : -1.0f } };
		const float lean[2] = { fabsf(dir.x), fabsf(dir.y) };
		int pick = -1;
		float pick_clear = 0.0f;
		for (int i = 0; i < 2; i++) {
			if (lean[i] < 0.2f)
				continue; // straight at the wall: stand, do not pick a side for the player
			const float side_clear = clearAhead(pos, sides[i], 6.0f);
			if (side_clear >= 1.75f && (pick < 0 || lean[i] > lean[pick])) {
				pick = i;
				pick_clear = side_clear;
			}
		}
		if (pick < 0) {
			// nowhere to go: stand at the wall, and keep the camera from leaning into it
			stopMoving();
			return;
		}
		go = sides[pick];
		clear = pick_clear;
		m_sliding = true;
	}

	const float reach = direct ? glm::max(clear - 0.5f, 1.0f) : steerReach(go.x, go.y, glm::max(clear - 0.5f, 1.0f));
	writeMove(go.x * reach / STEER_DISTANCE, go.y * reach / STEER_DISTANCE);
	m_moving = true;
	m_move_x = go.x;
	m_move_y = go.y;
}

void FirstPerson::capturePositions()
{
	glm::vec2 player = { 0.0f, 0.0f };
	const bool valid = m_frame_active && MotionPrediction::Instance().getPredictedPlayer(&player);
	if (valid)
		MotionPrediction::Instance().getPredictedUnits(m_pred_units_scratch);
	else
		m_pred_units_scratch.clear();

	// the map tiles of the rooms around the player (F7 switches them off, for comparing)
	TileWorld::Instance().capture(m_frame_active && m_real_tiles, m_inside_level);
	captureStates();

	std::lock_guard<std::mutex> lock(m_pred_lock);
	m_pred_player_valid = valid;
	m_pred_player = player;
	m_pred_units.swap(m_pred_units_scratch);
	m_unit_states.swap(m_unit_states_scratch);
}

// The states (auras, curses, shouts, armours) and the skill in use of every player and monster the
// game client has, read the way the game itself reads them (1.13c, docs/GAME-MEMORY.md of Scrolls
// of Diablo). The client's own units are what it draws its overlays for. Game thread.
void FirstPerson::captureStates()
{
	m_unit_states_scratch.clear();
	if (!m_frame_active || isVerNot(V_113c))
		return;

	static const uintptr_t client = (uintptr_t)GetModuleHandleA("D2Client.dll");
	static const uintptr_t common = (uintptr_t)GetModuleHandleA("D2Common.dll");
	if (!client || !common)
		return;
	// D2Common: the data tables; +0xC4 = how many states states.txt has
	const uintptr_t tables = *(const uintptr_t*)(common + 0x99E1C);
	if (!tables)
		return;
	const uint32_t words = glm::min((*(const uint32_t*)(tables + 0xC4) + 31) / 32, 8u);

	// D2Client: the units the game is showing (the player among them), 128 buckets per unit type,
	// each a chain through unit +0xE4. The table at +0x109A08 beside it holds the client's own
	// helpers (no players, no monsters).
	const uintptr_t* table = (const uintptr_t*)(client + 0x10A608);
	for (uint32_t type = 0; type < 2; type++) {
		for (uint32_t bucket = 0; bucket < 128; bucket++) {
			uint32_t guard = 0;
			for (uintptr_t unit = table[type * 128 + bucket]; unit && guard < 512; unit = *(const uintptr_t*)(unit + 0xE4), guard++) {
				UnitStates entry = {};
				entry.key = (type << 24) | (*(const uint32_t*)(unit + 0x0C) & 0x00FFFFFF);
				entry.mode = (uint16_t)*(const uint32_t*)(unit + 0x10);
				entry.frame = *(const uint32_t*)(unit + 0x44);
				entry.skill = 0xFFFF;
				// unit +0x5C: stat list; its +0x10 sign bit marks the extended kind, whose +0x58 points
				// at the state bits (bit n & 31 of dword n >> 5), as D2Common's GetUnitState reads them
				if (const uintptr_t stats = *(const uintptr_t*)(unit + 0x5C)) {
					if (*(const int32_t*)(stats + 0x10) < 0) {
						if (const uint32_t* flags = *(const uint32_t* const*)(stats + 0x58)) {
							for (uint32_t w = 0; w < words; w++)
								entry.bits[w] = flags[w];
						}
					}
				}
				// unit +0xA8: skill list; +0x10 the skill being used, whose first dword is its skills.txt record (u16 id first)
				if (const uintptr_t skills = *(const uintptr_t*)(unit + 0xA8)) {
					if (const uintptr_t used = *(const uintptr_t*)(skills + 0x10)) {
						if (const uintptr_t record = *(const uintptr_t*)used)
							entry.skill = *(const uint16_t*)record;
					}
				}
				m_unit_states_scratch.push_back(entry);
			}
		}
	}
}

// Follows each unit's states from frame to frame: a state that comes on plays its cast overlay once
// (a curse landing, an aura starting), one that goes off its remove overlay, and a skill used plays
// the skill's own cast overlay (the flames of Fire Ball, the bolt of Chain Lightning). Render thread.
void FirstPerson::trackOverlays(double now)
{
	SpriteStore& store = SpriteStore::Instance();
	const bool ready = store.overlaysReady();
	for (const UnitStates& s : m_unit_states_render) {
		auto found = m_overlay_tracks.find(s.key);
		const bool fresh = found == m_overlay_tracks.end();
		OverlayTrack& track = fresh ? m_overlay_tracks[s.key] : found->second;
		if (!fresh && ready) {
			for (uint32_t w = 0; w < 8; w++) {
				const uint32_t changed = s.bits[w] ^ track.bits[w];
				for (uint32_t b = 0; changed && b < 32; b++) {
					if (!((changed >> b) & 1))
						continue;
					const StateInfo* state = store.state(w * 32 + b);
					const OverlayInfo* once = !state ? nullptr : ((s.bits[w] >> b) & 1) ? state->cast : state->remove;
					if (once)
						track.shots.push_back({ once, now });
				}
			}
			const bool player = (s.key >> 24) == 0;
			if (s.skill != 0xFFFF && skillMode(player, s.mode) && (s.mode != track.mode || s.skill != track.skill || s.frame < track.frame))
				if (const OverlayInfo* cast = store.skillCast(s.skill))
					track.shots.push_back({ cast, now });
		}
		memcpy(track.bits, s.bits, sizeof(track.bits));
		track.mode = s.mode;
		track.skill = s.skill;
		track.frame = s.frame;
		track.seen = now;
	}
	// a unit the client no longer has is forgotten; a one-off overlay never outlives a few seconds
	for (auto it = m_overlay_tracks.begin(); it != m_overlay_tracks.end();) {
		if (now - it->second.seen > 1.0) {
			it = m_overlay_tracks.erase(it);
			continue;
		}
		auto& shots = it->second.shots;
		shots.erase(std::remove_if(shots.begin(), shots.end(), [now](const OverlayShot& shot) { return now - shot.start > 4.0; }), shots.end());
		++it;
	}
}

void FirstPerson::notice(const char* text)
{
	strncpy_s(m_notice, text, _TRUNCATE);
	m_notice_tick = GetTickCount();
}

// sod-view.ini, in the game's folder: "fov=<degrees>" and "sharp=<0 or 1>".
void FirstPerson::loadView()
{
	m_view_loaded = true;
	FILE* file = nullptr;
	if (fopen_s(&file, "sod-view.ini", "r") != 0 || !file)
		return;
	char line[64];
	while (fgets(line, sizeof(line), file)) {
		float value = 0.0f;
		if (sscanf_s(line, "fov=%f", &value) == 1 && value >= 50.0f && value <= 100.0f)
			m_fov = value;
		if (sscanf_s(line, "sharp=%f", &value) == 1)
			m_sharp = value != 0.0f;
	}
	fclose(file);
}

void FirstPerson::saveView()
{
	FILE* file = nullptr;
	if (fopen_s(&file, "sod-view.ini", "w") != 0 || !file)
		return;
	fprintf(file, "fov=%d\nsharp=%d\n", (int)m_fov, m_sharp ? 1 : 0);
	fclose(file);
}

namespace {

// PD2's stash has a search box, and while it has the keyboard E is a letter in it, not a key that
// closes the stash. ProjectDiablo.dll keeps that in one u32 of its data (0 = the box is not being
// typed in), found by comparing memory with the box in and out of use. The place holds for that
// build only: on any other one this answers false, and E leaves the stash to Esc.
bool stashSearchIdle()
{
	static uintptr_t flag = 0;
	static bool looked = false;
	if (!looked) {
		const auto base = (uintptr_t)GetModuleHandleW(L"ProjectDiablo.dll");
		if (!base)
			return false;
		looked = true;
		IMAGE_DOS_HEADER dos;
		IMAGE_NT_HEADERS32 nt;
		if (peek(&dos, (uint32_t)base, sizeof(dos)) && peek(&nt, (uint32_t)(base + dos.e_lfanew), sizeof(nt)) &&
			nt.FileHeader.TimeDateStamp == 0x6AE709CB && nt.OptionalHeader.SizeOfImage == 0x64D000)
			flag = base + 0x4BD7D0;
		trace_log("FirstPerson: stash search flag %s.", flag ? "found" : "unknown in this PD2 build (E will not close the stash)");
	}
	uint32_t value = 1;
	return flag && peek(&value, (uint32_t)flag, sizeof(value)) && value == 0;
}

} // namespace

void FirstPerson::update(bool active)
{
	m_frame_active = active;
	const bool foreground = isForeground();

	// F9: first person on / off.  (F10, the cursor, is further down: it needs to know about panels.)
	const bool f9 = foreground && keyDown(VK_F9);
	if (f9 && !m_key_f9)
		toggle();
	// The rest take Ctrl: F1 to F8 on their own are the game's skill keys.
	//   Ctrl+F5 / Ctrl+F6: narrower / wider field of view.
	//   Ctrl+F7: the game's own map tiles off / on.  Ctrl+F8: write a trace of the next frame.
	if (!m_view_loaded)
		loadView();
	const bool ctrl = foreground && keyDown(VK_CONTROL);
	const bool f5 = ctrl && keyDown(VK_F5), f6 = ctrl && keyDown(VK_F6);
	if ((f5 && !m_key_f5) || (f6 && !m_key_f6)) {
		m_fov = glm::clamp(m_fov + (f6 ? 5.0f : -5.0f), 50.0f, 100.0f);
		saveView();
		char line[48];
		sprintf_s(line, "FIELD OF VIEW %d", (int)m_fov);
		notice(line);
	}
	m_key_f5 = f5;
	m_key_f6 = f6;
	//   Ctrl+F4: crisp pixels up close / smooth
	const bool f4 = ctrl && keyDown(VK_F4);
	if (f4 && !m_key_f4) {
		m_sharp = !m_sharp;
		saveView();
		notice(m_sharp ? "CRISP PIXELS" : "SMOOTH PIXELS");
	}
	m_key_f4 = f4;
	const bool f7 = ctrl && keyDown(VK_F7);
	if (f7 && !m_key_f7) {
		m_real_tiles = !m_real_tiles;
		notice(m_real_tiles ? "MAP TILES ON" : "MAP TILES OFF");
	}
	m_key_f7 = f7;
	const bool f8 = ctrl && keyDown(VK_F8);
	if (f8 && !m_key_f8)
		m_trace_armed = true; // trace the next frame
	m_key_f8 = f8;
	m_key_f9 = f9;

	if (!m_raw_registered && App.hwnd) {
		RAWINPUTDEVICE device = { 0x01, 0x02, 0, App.hwnd }; // generic desktop / mouse
		m_raw_registered = RegisterRawInputDevices(&device, 1, sizeof(device)) == TRUE;
	}

	// Anything of the game's that needs the pointer or the keyboard gives them back: side panels,
	// the Esc menu, an NPC's menu or shop, the waypoint list, the stash, the chat line, ...
	bool panel = !active || *d2::screen_shift != SCREENPANEL_NONE || *d2::esc_menu_open || Menu::instance().isVisible();
	if (!panel && isVer(V_113c)) {
		// The game keeps one flag per panel in an array; d2gl's esc_menu_open is its entry 9
		// (help_menu_open is entry 0x21 and skill_menu_open entry 3 of the same array).
		const uint32_t* flags = (const uint32_t*)d2::esc_menu_open - 9;
		// Not 0x10 and 0x11: those are the buttons the game flashes for new stat points and a
		// changed quest log, not panels. With them here, the mouse stopped turning the view on
		// entering the Den of Evil, where the quest log button comes up.
		// Not 0x13 either: it is set for as long as a summoned creature's or a hireling's portrait
		// is on screen. With it here, summoning a Shadow Master gave the cursor back for good.
		static const uint8_t panels[] = { 0x01, 0x02, 0x04, 0x05, 0x08, 0x09, 0x0B, 0x0C, 0x0E, 0x0F, 0x14, 0x16, 0x17, 0x18, 0x19, 0x1A, 0x21 };
		for (const uint8_t id : panels)
			panel = panel || flags[id] != 0;
	}
	// E closes again what E opens: an NPC's menu (0x08) or shop (0x0C), the waypoint list (0x14),
	// the stash (0x19). Not while something takes typing, where E is a letter: the chat line
	// (0x05), the stash's search box. Nor under the Esc menu.
	m_panel_closable = false;
	if (active && m_enabled && isVer(V_113c) && !*d2::esc_menu_open && !Menu::instance().isVisible()) {
		const uint32_t* flags = (const uint32_t*)d2::esc_menu_open - 9;
		if (!flags[0x05])
			m_panel_closable = flags[0x19] ? stashSearchIdle() : (flags[0x08] || flags[0x0C] || flags[0x14]);
	}

	// F10 hands the cursor over and takes it back. What it does always shows: a line on screen
	// says whose the mouse is, and stays there for as long as the cursor is free. Pressed while a
	// panel is open it changes nothing (the panel has the cursor anyway); it used to flip the
	// setting unseen, and closing the panel then left the player with a cursor and no way to tell
	// why the view no longer turned.
	const bool f10 = m_f10_pressed;
	m_f10_pressed = false;
	if (!active || !m_enabled) {
		m_look_free = false; // a new game starts with the view under the mouse
	} else if (f10) {
		if (m_look_free) {
			m_look_free = false;
			notice("MOUSE LOOK");
		} else if (!panel) {
			m_look_free = true;
		} else {
			notice("CLOSE THE PANEL TO LOOK AROUND");
		}
	}
	if (m_look_free && !panel)
		notice("CURSOR FREE - F10 TO LOOK AROUND");

	const bool want = m_enabled && active && foreground && !m_look_free && !panel && m_raw_registered && openCommands();
	setLook(want);
	if (!m_look)
		return;

	// ---- movement: W A S D relative to where the camera looks ----
	const float forward = (keyDown('W') ? 1.0f : 0.0f) - (keyDown('S') ? 1.0f : 0.0f);
	const float strafe = (keyDown('D') ? 1.0f : 0.0f) - (keyDown('A') ? 1.0f : 0.0f);
	// After E the game walks the character to what it uses; keys still held from before wait
	// until they are let go, or for a moment at most.
	const bool move_keys = keyDown('W') || keyDown('A') || keyDown('S') || keyDown('D');
	if (m_use_hold && (!move_keys || GetTickCount() - m_use_tick > USE_HOLD_MS))
		m_use_hold = false;
	if (m_use_hold) {
		// waiting: neither steer nor stop (a stop would cancel the walk there)
	} else if (forward != 0.0f || strafe != 0.0f) {
		const float yaw = m_yaw, c = cosf(yaw), s = sinf(yaw);
		steer(glm::normalize(glm::vec2(forward * c - strafe * s, forward * s + strafe * c)));
	} else {
		stopMoving();
	}

	// ---- attacks: held buttons repeat ----
	const DWORD now = GetTickCount();
	if (m_lmb && now - m_lmb_time >= REPEAT_MS) {
		m_lmb_time = now;
		fire(0);
	}
	if (m_rmb && now - m_rmb_time >= REPEAT_MS) {
		m_rmb_time = now;
		fire(1);
	}
}

bool FirstPerson::onMessage(UINT msg, WPARAM wParam, LPARAM lParam)
{
	if (msg == WM_INPUT) {
		RAWINPUT raw;
		UINT size = sizeof(raw);
		if (GetRawInputData((HRAWINPUT)lParam, RID_INPUT, &raw, &size, sizeof(RAWINPUTHEADER)) == (UINT)-1 || raw.header.dwType != RIM_TYPEMOUSE)
			return false;

		const RAWMOUSE& mouse = raw.data.mouse;
		if (m_look) {
			if (!(mouse.usFlags & MOUSE_MOVE_ABSOLUTE)) {
				float yaw = m_yaw + (float)mouse.lLastX * LOOK_SENSITIVITY;
				if (yaw > glm::pi<float>())
					yaw -= glm::two_pi<float>();
				if (yaw < -glm::pi<float>())
					yaw += glm::two_pi<float>();
				m_yaw = yaw;
				m_pitch = glm::clamp(m_pitch - (float)mouse.lLastY * LOOK_SENSITIVITY, -PITCH_LIMIT, PITCH_LIMIT);
			}
			if (mouse.usButtonFlags & RI_MOUSE_LEFT_BUTTON_DOWN) {
				m_lmb = true;
				m_lmb_time = GetTickCount();
				fire(0);
			}
			if (mouse.usButtonFlags & RI_MOUSE_RIGHT_BUTTON_DOWN) {
				m_rmb = true;
				m_rmb_time = GetTickCount();
				fire(1);
			}
		}
		if (mouse.usButtonFlags & RI_MOUSE_LEFT_BUTTON_UP)
			m_lmb = false;
		if (mouse.usButtonFlags & RI_MOUSE_RIGHT_BUTTON_UP)
			m_rmb = false;
		return false; // the default handler still has to see WM_INPUT
	}

	// F10 is ours while the first-person view is up. Taken from the message, not from the key's
	// state: a press is one message however long the key is held, and neither the game (F10 fades
	// its automap) nor Windows (F10 opens the window menu) gets to act on it.
	if ((msg == WM_SYSKEYDOWN || msg == WM_KEYDOWN || msg == WM_SYSKEYUP || msg == WM_KEYUP) && wParam == VK_F10 && m_frame_active) {
		if ((msg == WM_SYSKEYDOWN || msg == WM_KEYDOWN) && !(lParam & (1 << 30)))
			m_f10_pressed = true;
		return true;
	}

	// E uses what the view points at; pressed again while the stash, the waypoint list or an NPC's
	// menu it opened is up, it closes that (as Esc would). One use per press: a held key's
	// repeats are taken and dropped.
	if (msg == WM_KEYDOWN && wParam == 'E' && m_frame_active && (m_look || m_panel_closable)) {
		if (!(lParam & (1 << 30))) {
			if (m_look) {
				use();
			} else {
				PostMessageW(App.hwnd, WM_KEYDOWN, VK_ESCAPE, 1 | (0x01 << 16));
				PostMessageW(App.hwnd, WM_KEYUP, VK_ESCAPE, 1 | (0x01 << 16) | (1u << 30) | (1u << 31));
			}
		}
		m_eat_e_char = true; // its WM_CHAR comes next, and must not reach the game either
		return true;
	}
	if (msg == WM_KEYDOWN)
		m_eat_e_char = false; // any other key: an E typed into the chat line later is the player's
	if (msg == WM_CHAR && (wParam == 'e' || wParam == 'E') && m_eat_e_char) {
		m_eat_e_char = false;
		return true;
	}

	if (!m_look)
		return false;

	// W A S D move the character while looking; the game's own bindings for them stay quiet.
	// Real mouse messages are held back by sod.dll, which also feeds the game its virtual mouse.
	switch (msg) {
		case WM_KEYDOWN:
		case WM_KEYUP:
			return wParam == 'W' || wParam == 'A' || wParam == 'S' || wParam == 'D' || wParam == 'E';
		case WM_CHAR:
			return wParam == 'w' || wParam == 'a' || wParam == 's' || wParam == 'd' || wParam == 'e' || wParam == 'W' || wParam == 'A' || wParam == 'S' || wParam == 'D' || wParam == 'E';
	}
	return false;
}

bool FirstPerson::readFrame()
{
	const auto hdr = (const StateHeader*)m_base;
	const uint32_t records = sizeof(FrameMeta) + sizeof(PlayerRecord) + hdr->maxUnits * sizeof(UnitRecord);
	const uint32_t size = glm::max(records, glm::min(hdr->frameBufferSize, records + 1024u * 1024u)); // records + strings
	if (m_frame_copy.size() != size)
		m_frame_copy.resize(size);

	for (int i = 0; i < 4; i++) {
		const uint32_t seq = hdr->frameSeq;
		const uint32_t off = hdr->frameIndex ? hdr->frameBuf1Off : hdr->frameBuf0Off;
		memcpy(m_frame_copy.data(), m_base + off, size);
		MemoryBarrier();
		if (hdr->frameSeq == seq)
			return true;
	}
	return false;
}

bool FirstPerson::readRooms(uint32_t* epoch)
{
	const auto hdr = (const StateHeader*)m_base;
	const uint32_t size = sizeof(RoomSectionHeader) + hdr->maxRooms * sizeof(RoomEntry) + hdr->collArenaBytes;
	if (m_room_copy.size() != size)
		m_room_copy.resize(size);

	for (int i = 0; i < 4; i++) {
		const uint32_t seq = hdr->roomSeq;
		if (seq & 1)
			continue;
		const uint32_t ep = hdr->roomEpoch;
		memcpy(m_room_copy.data(), m_base + hdr->roomSectionOff, size);
		MemoryBarrier();
		if (hdr->roomSeq == seq) {
			*epoch = ep;
			return true;
		}
	}
	return false;
}

// ---- GL ----

bool FirstPerson::initGL()
{
	if (m_gl_ready)
		return true;
	if (m_gl_failed)
		return false;

	const GLuint vs = compile(GL_VERTEX_SHADER, VS_SRC);
	const GLuint fs = compile(GL_FRAGMENT_SHADER, FS_SRC);
	if (!vs || !fs) {
		m_gl_failed = true;
		return false;
	}

	m_program = glCreateProgram();
	glAttachShader(m_program, vs);
	glAttachShader(m_program, fs);
	glLinkProgram(m_program);
	glDeleteShader(vs);
	glDeleteShader(fs);

	GLint ok = GL_FALSE;
	glGetProgramiv(m_program, GL_LINK_STATUS, &ok);
	if (!ok) {
		error_log("FirstPerson: shader link failed.");
		m_gl_failed = true;
		return false;
	}
	m_u_mvp = glGetUniformLocation(m_program, "u_MVP");
	m_u_eye = glGetUniformLocation(m_program, "u_Eye");
	m_u_fog = glGetUniformLocation(m_program, "u_Fog");
	m_u_flat = glGetUniformLocation(m_program, "u_Flat");
	m_u_textured = glGetUniformLocation(m_program, "u_Textured");
	m_u_additive = glGetUniformLocation(m_program, "u_Additive");
	m_u_sharp = glGetUniformLocation(m_program, "u_Sharp");
	m_u_texture = glGetUniformLocation(m_program, "u_Texture");

	GLint prev_vao = 0, prev_vbo = 0;
	glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prev_vao);
	glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &prev_vbo);

	GLuint* vaos[2] = { &m_vao_world, &m_vao_units };
	GLuint* vbos[2] = { &m_vbo_world, &m_vbo_units };
	for (int i = 0; i < 2; i++) {
		glGenVertexArrays(1, vaos[i]);
		glGenBuffers(1, vbos[i]);
		glBindVertexArray(*vaos[i]);
		glBindBuffer(GL_ARRAY_BUFFER, *vbos[i]);
		glEnableVertexAttribArray(0);
		glEnableVertexAttribArray(1);
		glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(FpVertex), (const void*)offsetof(FpVertex, x));
		glVertexAttribPointer(1, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(FpVertex), (const void*)offsetof(FpVertex, r));
		glEnableVertexAttribArray(2);
		glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(FpVertex), (const void*)offsetof(FpVertex, u));
	}
	// the world's index list belongs to the world's vertex array
	glGenBuffers(1, &m_ibo_world);
	glBindVertexArray(m_vao_world);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_ibo_world);

	glBindVertexArray(prev_vao);
	glBindBuffer(GL_ARRAY_BUFFER, prev_vbo);

	m_gl_ready = true;
	trace_log("FirstPerson: renderer ready.");
	return true;
}

void FirstPerson::ensureTarget(glm::uvec2 size)
{
	if (m_fbo && m_fbo_size == size)
		return;

	if (m_fbo) {
		glDeleteFramebuffers(1, &m_fbo);
		glDeleteRenderbuffers(1, &m_color);
		glDeleteRenderbuffers(1, &m_depth);
	}

	glGenFramebuffers(1, &m_fbo);
	glGenRenderbuffers(1, &m_color);
	glGenRenderbuffers(1, &m_depth);

	glBindRenderbuffer(GL_RENDERBUFFER, m_color);
	glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, size.x, size.y);
	glBindRenderbuffer(GL_RENDERBUFFER, m_depth);
	glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, size.x, size.y);
	glBindRenderbuffer(GL_RENDERBUFFER, 0);

	glBindFramebuffer(GL_FRAMEBUFFER, m_fbo);
	glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, m_color);
	glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, m_depth);
	if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
		error_log("FirstPerson: render target incomplete (%ux%u).", size.x, size.y);

	m_fbo_size = size;
}

// A world too big for the memory the game has left must not end the game (an allocation that fails
// throws, and nothing above here catches it). It is built again without the mounds, the largest
// part, and failing that it is not drawn until the rooms change.
void FirstPerson::rebuildWorld()
{
	for (int attempt = 0; attempt < 2; attempt++) {
		try {
			buildWorld(attempt == 0);
			return;
		} catch (const std::bad_alloc&) {
			g_world_parts.release();
			g_relief.w = g_relief.h = 0;
			std::vector<uint8_t>().swap(g_relief.cells);
			error_log("FirstPerson: out of memory building the world%s.", attempt == 0 ? ", building it again without mounds" : "");
		}
	}
	m_batches.clear();
	m_world_vertex_count = 0;
}

void FirstPerson::buildWorld(bool mounds)
{
	const auto hdr = (const StateHeader*)m_base;
	const auto section = (const RoomSectionHeader*)m_room_copy.data();
	const auto entries = (const RoomEntry*)(m_room_copy.data() + sizeof(RoomSectionHeader));
	const uint8_t* arena = m_room_copy.data() + sizeof(RoomSectionHeader) + hdr->maxRooms * sizeof(RoomEntry);

	const glm::vec3 floor_color = { 0.36f, 0.31f, 0.25f };
	const glm::vec3 alt_color = { 0.30f, 0.33f, 0.27f };
	const glm::vec3 wall_color = { 0.47f, 0.44f, 0.40f };
	const glm::vec3 low_color = { 0.40f, 0.33f, 0.24f };

	// ---- classify every cell once ----
	auto& rooms = g_rooms;
	rooms.clear();
	const uint32_t count = glm::min(section->roomCount, hdr->maxRooms);
	for (uint32_t r = 0; r < count; r++) {
		const RoomEntry& entry = entries[r];
		if (!entry.w || !entry.h || entry.w > 1024 || entry.h > 1024)
			continue;
		if (entry.collLen < entry.w * entry.h * 2 || (uint64_t)entry.collOff + entry.collLen > hdr->collArenaBytes)
			continue;

		Room room = { entry.levelNo, (int)entry.x, (int)entry.y, (int)entry.w, (int)entry.h, {} };
		room.cls.resize(entry.w * entry.h);
		const uint16_t* cells = (const uint16_t*)(arena + entry.collOff);
		for (uint32_t i = 0; i < entry.w * entry.h; i++)
			room.cls[i] = classify(cells[i]);
		rooms.push_back(std::move(room));
	}

	// class of any cell, also outside the room being built (neighbouring rooms); unknown = void
	const auto classAt = [&](int gx, int gy) -> uint8_t {
		for (const auto& room : rooms)
			if (gx >= room.x && gy >= room.y && gx < room.x + room.w && gy < room.y + room.h)
				return room.cls[(gy - room.y) * room.w + (gx - room.x)];
		return CELL_VOID;
	};
	const auto isOpen = [](uint8_t c) { return c == CELL_FLOOR || c == CELL_ALT || c == CELL_LOW; };
	const auto floorDiv = [](int v) { return v >= 0 ? v / TILE : -((-v + TILE - 1) / TILE); };

	g_world_parts.release();
	auto& plain = g_world_parts.plain;
	auto& floor_art = g_world_parts.floor_art;
	auto& wall_art = g_world_parts.wall_art;
	auto& relief_art = g_world_parts.relief_art;

	const TileWorld& tile_world = TileWorld::Instance();

	// A building's wall is one picture on one tile, but what it blocks often reaches into the
	// tiles next to it. Those tiles have wall cells and nothing standing on them, and a box built
	// there shows as a dark block beside the building. So: the tiles with wall cells and nothing
	// standing are gathered into patches, and a small patch that touches something standing is a
	// spill-over and gets no box. A large one (a cliff, the rock of a cave) keeps its walls.
	static std::unordered_set<uint32_t> bare, spill, visited;
	static std::vector<uint32_t> stack, patch;
	bare.clear();
	spill.clear();
	visited.clear();
	const auto tileKey = [](int tx, int ty) { return ((uint32_t)tx & 0xFFFF) << 16 | ((uint32_t)ty & 0xFFFF); };
	for (const auto& room : rooms) {
		if (!tile_world.covers(room.x, room.y, room.w, room.h))
			continue;
		for (int y = 0; y < room.h; y++)
			for (int x = 0; x < room.w; x++)
				if (room.cls[y * room.w + x] == CELL_WALL) {
					const int tx = floorDiv(room.x + x), ty = floorDiv(room.y + y);
					if (!tile_world.standing(tx, ty))
						bare.insert(tileKey(tx, ty));
				}
	}
	for (const uint32_t first : bare) {
		if (!visited.insert(first).second)
			continue;
		patch.clear();
		stack.assign(1, first);
		bool touches = false;
		while (!stack.empty()) {
			const uint32_t key = stack.back();
			stack.pop_back();
			patch.push_back(key);
			const int tx = (int)(key >> 16), ty = (int)(key & 0xFFFF);
			for (int dy = -1; dy <= 1; dy++)
				for (int dx = -1; dx <= 1; dx++) {
					if (!dx && !dy)
						continue;
					touches = touches || tile_world.standing(tx + dx, ty + dy);
					const uint32_t next = tileKey(tx + dx, ty + dy);
					if ((!dx || !dy) && bare.count(next) && visited.insert(next).second)
						stack.push_back(next);
				}
		}
		if (touches && patch.size() <= 12)
			spill.insert(patch.begin(), patch.end());
	}

	// The game paints many things into the floor: a forge and its anvil, a heap of rubble, a
	// camp's litter, a rock. In its own view they look solid. Laid flat they are a picture on the
	// ground, and the character stops at something the player cannot see. Where the character
	// cannot walk and nothing of the game's stands (no wall, no tree), that part of the floor
	// rises into a mound that carries the floor's own picture. Not water or lava.
	// (ReliefMap says how.) The picture lies on the mound from above, so none of it is pulled
	// down a wall into stripes.
	const auto raised = [&](int gx, int gy) -> bool {
		if (classAt(gx, gy) != CELL_LOW)
			return false;
		const int tx = floorDiv(gx), ty = floorDiv(gy);
		if (tile_world.standing(tx, ty))
			return false;
		const TileWorld::TileFloors* on_tile = tile_world.floors(tx, ty);
		return on_tile && on_tile->count > 0 && !on_tile->liquid;
	};

	// which cells rise, once, in one grid over the rooms drawn from their tiles
	int lift_x0 = INT_MAX, lift_y0 = INT_MAX, lift_x1 = INT_MIN, lift_y1 = INT_MIN;
	for (const auto& room : rooms)
		if (tile_world.covers(room.x, room.y, room.w, room.h)) {
			lift_x0 = glm::min(lift_x0, room.x);
			lift_y0 = glm::min(lift_y0, room.y);
			lift_x1 = glm::max(lift_x1, room.x + room.w);
			lift_y1 = glm::max(lift_y1, room.y + room.h);
		}
	ReliefMap& relief = g_relief;
	relief.x0 = lift_x0;
	relief.y0 = lift_y0;
	relief.w = mounds && lift_x1 > lift_x0 ? lift_x1 - lift_x0 : 0;
	relief.h = mounds && lift_y1 > lift_y0 ? lift_y1 - lift_y0 : 0;
	relief.cells.assign((size_t)relief.w * relief.h, 0);
	for (const auto& room : rooms) {
		if (!mounds || !tile_world.covers(room.x, room.y, room.w, room.h))
			continue;
		for (int y = 0; y < room.h; y++)
			for (int x = 0; x < room.w; x++)
				if (room.cls[y * room.w + x] == CELL_LOW && raised(room.x + x, room.y + y))
					relief.cells[(size_t)(room.y + y - relief.y0) * relief.w + (room.x + x - relief.x0)] = 1;
	}

	const int PIECES = 4; // a cell's mound is PIECES x PIECES quads
	const glm::vec3 SUN = glm::normalize(glm::vec3(-0.5f, 1.0f, -0.3f));
	const float page_size = TileWorld::pageSize();
	const int SPAN = PIECES + 3; // the cell's points and one ring beyond, for the slopes
	const int POINTS = (PIECES + 1) * (PIECES + 1);
	float height[SPAN][SPAN], rise[SPAN][SPAN];

	// A cell's mound is a grid of points, each shared by the quads around it, drawn from a list of
	// indices: about two fifths of the memory of a list of separate triangles. Each page's lists
	// are made their full size at once: a list that grows needs its old block and a bigger new one
	// at the same time, and in the docks of Kurast these run to tens of megabytes.
	{
		std::map<int, size_t> layers;
		for (int gy = relief.y0; gy < relief.y0 + relief.h; gy++)
			for (int gx = relief.x0; gx < relief.x0 + relief.w; gx++)
				if (relief.at(gx, gy)) {
					const TileWorld::TileFloors& on_tile = *tile_world.floors(floorDiv(gx), floorDiv(gy));
					for (int layer = 0; layer < on_tile.count; layer++)
						layers[on_tile.page[layer]]++;
				}
		for (const auto& item : layers) {
			IndexedPart& part = relief_art[item.first];
			part.verts.reserve(item.second * POINTS);
			part.indices.reserve(item.second * PIECES * PIECES * 6);
		}
	}

	for (int gy = relief.y0; gy < relief.y0 + relief.h; gy++)
		for (int gx = relief.x0; gx < relief.x0 + relief.w; gx++) {
			if (!relief.at(gx, gy))
				continue;
			const int tx = floorDiv(gx), ty = floorDiv(gy);
			const TileWorld::TileFloors& on_tile = *tile_world.floors(tx, ty);
			const float step = 1.0f / PIECES;
			for (int j = 0; j < SPAN; j++)
				for (int i = 0; i < SPAN; i++)
					height[j][i] = relief.surface((float)gx + (i - 1) * step, (float)gy + (j - 1) * step, &rise[j][i]);

			// each point's brightness: lit from one side, a little darker low on the slope
			float shade[PIECES + 1][PIECES + 1];
			for (int j = 0; j <= PIECES; j++)
				for (int i = 0; i <= PIECES; i++) {
					const float dx = (height[j + 1][i + 2] - height[j + 1][i]) / (2.0f * step);
					const float dz = (height[j + 2][i + 1] - height[j][i + 1]) / (2.0f * step);
					const glm::vec3 n = glm::normalize(glm::vec3(-dx, 1.0f, -dz));
					const float sun = glm::clamp(glm::dot(n, SUN) / SUN.y, 0.45f, 1.15f);
					const float foot = 0.85f + 0.15f * glm::min(rise[j + 1][i + 1] * 2.0f, 1.0f);
					shade[j][i] = sun * foot;
				}

			for (int layer = 0; layer < on_tile.count; layer++) {
				// where a point of the tile is in its picture: the tile is a diamond there
				const auto at = [&](float wx, float wz) {
					const float fx = (wx - (float)(tx * TILE)) / TILE, fz = (wz - (float)(ty * TILE)) / TILE;
					return glm::vec2((on_tile.u[layer] + 80.0f + (fx - fz) * 79.25f) / page_size, (on_tile.v[layer] + 0.75f + (fx + fz) * 39.25f) / page_size);
				};
				const float above = 0.004f * (float)layer;
				IndexedPart& part = relief_art[on_tile.page[layer]];
				const uint32_t first = (uint32_t)part.verts.size();
				for (int j = 0; j <= PIECES; j++)
					for (int i = 0; i <= PIECES; i++) {
						const float px = (float)gx + i * step, pz = (float)gy + j * step;
						const glm::vec2 uv = at(px, pz);
						const uint8_t s = (uint8_t)glm::clamp(shade[j][i] * 200.0f, 0.0f, 255.0f); // as pushQuadUV
						part.verts.push_back({ px, height[j + 1][i + 1] + above, pz, s, s, s, 255, uv.x, uv.y });
					}
				// each quad as pushQuadUV makes it: corners a b c d around, triangles a b c and a c d
				for (int j = 0; j < PIECES; j++)
					for (int i = 0; i < PIECES; i++) {
						const uint32_t a = first + (uint32_t)(j * (PIECES + 1) + i), b = a + 1, d = a + (PIECES + 1), c = d + 1;
						for (const uint32_t k : { a, b, c, a, c, d })
							part.indices.push_back(k);
					}
			}
		}

	for (const auto& room : rooms) {
		// A room drawn from its own tiles needs nothing from here but the walls those tiles do not
		// have: in a cave the rock is painted into the floor, and there is no wall tile to stand up.
		const bool tiled = tile_world.covers(room.x, room.y, room.w, room.h);
		// Without wall art for the level (a cliff at the edge of a desert), such a wall takes the
		// colour of the ground it rises from rather than a grey that belongs nowhere.
		glm::vec3 bare_wall = wall_color, earth;
		if (tiled && tile_world.ground(&earth))
			bare_wall = glm::min(earth * 1.1f, glm::vec3(1.0f));
		LevelArt& art = levelArt(room.level);
		const int wall_level = wallArtLevel(room.level);
		const LevelArt* wall = wall_level >= 0 ? &g_level_art[wall_level] : nullptr;
		const auto at = [&](int x, int y) -> uint8_t {
			if (x >= 0 && y >= 0 && x < room.w && y < room.h)
				return room.cls[y * room.w + x];
			return classAt(room.x + x, room.y + y);
		};

		// an indoor level has a roof over it: one dark sheet over a room that is not drawn from its tiles
		if (!tiled && m_inside_level) {
			const float x0 = (float)room.x, x1 = (float)(room.x + room.w), z0 = (float)room.y, z1 = (float)(room.y + room.h);
			pushQuad(plain, { x0, WALL_HEIGHT, z0 }, { x1, WALL_HEIGHT, z0 }, { x1, WALL_HEIGHT, z1 }, { x0, WALL_HEIGHT, z1 }, wall_color, 0.45f);
		}

		// ---- floors and low blocks: runs along x, cut at tile borders so each piece lies in one tile ----
		for (int y = 0; y < room.h && !tiled; y++) {
			int x = 0;
			while (x < room.w) {
				const uint8_t cls = room.cls[y * room.w + x];
				const int tx = floorDiv(room.x + x), ty = floorDiv(room.y + y);
				const int tile_end = (tx + 1) * TILE - room.x;
				int end = x + 1;
				while (end < room.w && end < tile_end && room.cls[y * room.w + end] == cls)
					end++;

				const float x0 = (float)(room.x + x), x1 = (float)(room.x + end);
				const float z0 = (float)(room.y + y), z1 = z0 + 1.0f;
				if (cls == CELL_FLOOR || cls == CELL_ALT) {
					if (art.floor) {
						const ArtTile& t = weighted(art.floor_tiles, art.floor_total, hash3(tx, ty, 17));
						const float fx0 = (x0 - tx * TILE) / TILE, fx1 = (x1 - tx * TILE) / TILE;
						const float fz0 = (z0 - ty * TILE) / TILE, fz1 = (z1 - ty * TILE) / TILE;
						const float u0 = (t.x + fx0 * t.w) / art.floor_w, u1 = (t.x + fx1 * t.w) / art.floor_w;
						const float v0 = (t.y + fz0 * t.h) / art.floor_h, v1 = (t.y + fz1 * t.h) / art.floor_h;
						const float shade = cls == CELL_ALT ? 0.84f : 1.0f;
						const glm::vec3 p[4] = { { x0, 0, z0 }, { x1, 0, z0 }, { x1, 0, z1 }, { x0, 0, z1 } };
						const glm::vec2 uv[4] = { { u0, v0 }, { u1, v0 }, { u1, v1 }, { u0, v1 } };
						const float shades[4] = { shade, shade, shade, shade };
						pushQuadUV(floor_art[room.level], p, uv, shades);
					} else
						pushQuad(plain, { x0, 0, z0 }, { x1, 0, z0 }, { x1, 0, z1 }, { x0, 0, z1 }, cls == CELL_ALT ? alt_color : floor_color, 1.0f);
				} else if (cls == CELL_LOW) {
					// the top of a low obstacle (fence, rubble, hedge); its sides are faces, like the walls'
					if (wall && !wall->wall_tiles[0].empty()) {
						const ArtTile& t = weighted(wall->wall_tiles[0], wall->wall_total[0], hash3(tx, ty, 91));
						const float fx0 = (x0 - tx * TILE) / TILE, fx1 = (x1 - tx * TILE) / TILE;
						const float fz0 = (z0 - ty * TILE) / TILE, fz1 = (z1 - ty * TILE) / TILE;
						const float u0 = (t.x + fx0 * t.w) / wall->wall_w, u1 = (t.x + fx1 * t.w) / wall->wall_w;
						const float v0 = (t.y + t.h * (0.45f + 0.5f * fz0)) / wall->wall_h, v1 = (t.y + t.h * (0.45f + 0.5f * fz1)) / wall->wall_h;
						const glm::vec3 p[4] = { { x0, LOW_HEIGHT, z0 }, { x1, LOW_HEIGHT, z0 }, { x1, LOW_HEIGHT, z1 }, { x0, LOW_HEIGHT, z1 } };
						const glm::vec2 uv[4] = { { u0, v0 }, { u1, v0 }, { u1, v1 }, { u0, v1 } };
						const float shades[4] = { 0.95f, 0.95f, 0.95f, 0.95f };
						pushQuadUV(wall_art[wall_level], p, uv, shades);
					} else
						pushQuad(plain, { x0, LOW_HEIGHT, z0 }, { x1, LOW_HEIGHT, z0 }, { x1, LOW_HEIGHT, z1 }, { x0, LOW_HEIGHT, z1 }, low_color, 1.0f);
				}
				x = end;
			}
		}

		// ---- walls: one face wherever a wall cell borders an open cell, cut at tile borders ----
		// axis 0: faces along x (normal +-z).  axis 1: faces along y (normal +-x).
		const auto emit = [&](int axis, int sign, int line, int s0, int s1, float height) {
			const auto p = [&](float s, float y) { return axis == 0 ? glm::vec3(s, y, (float)line) : glm::vec3((float)line, y, s); };
			const float face = axis == 0 ? 0.92f : 0.74f;
			if (!wall || wall->wall_tiles[axis].empty()) {
				pushQuad(plain, p((float)s0, 0), p((float)s1, 0), p((float)s1, height), p((float)s0, height), height < WALL_HEIGHT ? low_color : bare_wall, face);
				return;
			}

			const int along = floorDiv(s0);
			const float f0 = (float)(s0 - along * TILE) / TILE, f1 = (float)(s1 - along * TILE) / TILE;
			// texture direction so the art reads left to right from the open side
			const bool flip = axis == 0 ? sign < 0 : sign > 0;
			const float ua = flip ? 1.0f - f0 : f0, ub = flip ? 1.0f - f1 : f1;
			const int blocks = wall->wall_square ? (int)ceilf(height / SQUARE - 1e-4f) : 1;
			for (int k = 0; k < blocks; k++) {
				const ArtTile& t = weighted(wall->wall_tiles[axis], wall->wall_total[axis], hash3(along, line * 2 + (sign > 0 ? 1 : 0), (axis == 0 ? 31 : 57) + k));
				const float u_a = (t.x + ua * t.w) / wall->wall_w, u_b = (t.x + ub * t.w) / wall->wall_w;
				float y0, y1, v_bottom, v_top;
				if (wall->wall_square) {
					y0 = k * SQUARE;
					y1 = glm::min(height, (k + 1) * SQUARE);
					v_bottom = (t.y + t.h) / wall->wall_h;
					v_top = (t.y + t.h * (1.0f - (y1 - y0) / SQUARE)) / wall->wall_h;
				} else {
					y0 = 0.0f;
					y1 = height;
					v_bottom = (t.y + t.h) / wall->wall_h;
					v_top = (t.y + t.h * (1.0f - height / WALL_HEIGHT)) / wall->wall_h; // a low face shows the foot of the wall art
				}
				const float bottom = face * (0.62f + 0.38f * (y0 / WALL_HEIGHT)), top = face * (0.62f + 0.38f * (y1 / WALL_HEIGHT));
				const glm::vec3 corners[4] = { p((float)s0, y0), p((float)s1, y0), p((float)s1, y1), p((float)s0, y1) };
				const glm::vec2 uv[4] = { { u_a, v_bottom }, { u_b, v_bottom }, { u_b, v_top }, { u_a, v_top } };
				const float shades[4] = { bottom, bottom, top, top };
				pushQuadUV(wall_art[wall_level], corners, uv, shades);
			}
		};

		const int dirs[4][2] = { { 0, 1 }, { 0, -1 }, { 1, 0 }, { -1, 0 } };
		for (int pass = 0; pass < (tiled ? 1 : 2); pass++) // 0: walls, 1: low obstacles
		for (const auto& dir : dirs) {
			const int nx = dir[0], nz = dir[1];
			const bool along_x = nz != 0;
			const int outer = along_x ? room.h : room.w, inner = along_x ? room.w : room.h;
			const int axis = along_x ? 0 : 1, sign = along_x ? nz : nx;
			for (int a = 0; a < outer; a++) {
				const int line = along_x ? room.y + a + (nz > 0 ? 1 : 0) : room.x + a + (nx > 0 ? 1 : 0);
				const int base = along_x ? room.x : room.y;
				int start = -1;
				for (int b = 0; b <= inner; b++) {
					bool face = false;
					if (b < inner) {
						const int x = along_x ? b : a, y = along_x ? a : b;
						const uint8_t here = room.cls[y * room.w + x], there = at(x + nx, y + nz);
						face = pass == 0 ? here == CELL_WALL && isOpen(there) : here == CELL_LOW && (there == CELL_FLOOR || there == CELL_ALT);
						if (face && tiled) {
							const int tx = floorDiv(room.x + x), ty = floorDiv(room.y + y);
							if (tile_world.standing(tx, ty) || spill.count(tileKey(tx, ty)))
								face = false; // the game's own wall stands here, or reaches over from the next tile
						}
					}
					const bool boundary = ((base + b) % TILE + TILE) % TILE == 0;
					if (start >= 0 && (!face || boundary)) {
						emit(axis, sign, line, base + start, base + b, pass == 0 ? WALL_HEIGHT : LOW_HEIGHT);
						start = -1;
					}
					if (face && start < 0)
						start = b;
				}
			}
		}
	}

	// ---- onto the card: one buffer of points, one of indices, one batch per texture ----
	// Each part is copied straight to its place in the buffers; no copy of the whole world is made.
	struct Upload {
		const std::vector<FpVertex>* verts;
		std::vector<uint32_t>* indices; // null: a list of triangles
		GLuint texture;
	};
	std::vector<Upload> uploads;
	uploads.push_back({ &plain, nullptr, 0 });
	for (const auto& item : floor_art)
		uploads.push_back({ &item.second, nullptr, g_level_art[item.first].floor_texture });
	for (const auto& item : wall_art)
		uploads.push_back({ &item.second, nullptr, g_level_art[item.first].wall_texture });
	for (auto& item : relief_art)
		if (const GLuint texture = tile_world.texture(item.first))
			uploads.push_back({ &item.second.verts, &item.second.indices, texture });
	size_t vertex_total = 0, index_total = 0;
	for (const auto& up : uploads) {
		vertex_total += up.verts->size();
		index_total += up.indices ? up.indices->size() : 0;
	}

	m_batches.clear();
	m_world_vertex_count = 0;
	for (int i = 0; i < 16 && glGetError() != GL_NO_ERROR; i++) {} // older errors are not this one's
	GLint prev_vao = 0;
	glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prev_vao);
	glBindVertexArray(m_vao_world); // the index buffer binding is the vertex array's
	glBindBuffer(GL_ARRAY_BUFFER, m_vbo_world);
	glBufferData(GL_ARRAY_BUFFER, vertex_total * sizeof(FpVertex), nullptr, GL_STATIC_DRAW);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_ibo_world);
	glBufferData(GL_ELEMENT_ARRAY_BUFFER, index_total * sizeof(uint32_t), nullptr, GL_STATIC_DRAW);
	if (glGetError() == GL_OUT_OF_MEMORY) {
		glBindVertexArray(prev_vao);
		throw std::bad_alloc();
	}
	uint32_t vertex_at = 0, index_at = 0;
	for (const auto& up : uploads) {
		const uint32_t n = (uint32_t)up.verts->size();
		if (!n)
			continue;
		glBufferSubData(GL_ARRAY_BUFFER, (GLintptr)vertex_at * sizeof(FpVertex), (GLsizeiptr)n * sizeof(FpVertex), up.verts->data());
		if (up.indices) {
			for (uint32_t& k : *up.indices)
				k += vertex_at;
			const uint32_t m = (uint32_t)up.indices->size();
			glBufferSubData(GL_ELEMENT_ARRAY_BUFFER, (GLintptr)index_at * sizeof(uint32_t), (GLsizeiptr)m * sizeof(uint32_t), up.indices->data());
			m_batches.push_back({ up.texture, index_at, m, true });
			index_at += m;
		} else
			m_batches.push_back({ up.texture, vertex_at, n, false });
		vertex_at += n;
	}
	glBindVertexArray(prev_vao);
	m_world_vertex_count = vertex_at;
	g_world_parts.release();
	trace_log("FirstPerson: world rebuilt (%u rooms, %u vertices, %u indices, %u batches%s).", count, vertex_at, index_at, (uint32_t)m_batches.size(), mounds ? "" : ", no mounds");
}

namespace {

// What the E prompt calls the thing E would use: an NPC by name, an item by its name, an object by
// the name the game shows for it, with a few made shorter ("Your Private Stash" -> STASH). A txt
// name from an older sod.dll is made readable too ("LargeUrn" -> LARGE URN, "Chest3" -> CHEST).
std::string useLabel(const UnitRecord& u, const char* strings, size_t strings_size)
{
	SpriteStore& store = SpriteStore::Instance();
	std::string text;
	switch (u.type) {
		case 0: return "CORPSE";
		case 5: return "ENTER";
		case 1: {
			const MonsterInfo* info = store.monster(u.txt);
			text = info && !info->name.empty() ? info->name : "TALK";
		} break;
		case 4: {
			text = "PICK UP";
			if (u.itemCodeLen && (size_t)u.itemCodeOff + u.itemCodeLen <= strings_size) {
				std::string code(strings + u.itemCodeOff, u.itemCodeLen);
				while (!code.empty() && (code.back() == ' ' || code.back() == 0))
					code.pop_back();
				if (const std::string* name = store.itemName(code))
					text += " " + *name;
			}
		} break;
		case 2: {
			if (u.txt == 59 || u.txt == 60)
				return "TOWN PORTAL";
			std::string raw;
			if (u.nameLen && (size_t)u.nameOff + u.nameLen <= strings_size)
				raw.assign(strings + u.nameOff, u.nameLen);
			while (!raw.empty() && (raw.back() == 0 || raw.back() == ' '))
				raw.pop_back();
			std::string lower = raw;
			for (char& ch : lower)
				ch = (char)tolower((unsigned char)ch);
			// sod.dll sends the name the game shows; an older one sent the txt name
			if (lower == "bank" || lower == "your private stash")
				return "STASH";
			if (lower == "trappdoor")
				return "TRAPDOOR";
			if (lower == "taintedsunaltar")
				return "ALTAR";
			if (lower.find("door") != std::string::npos && lower.find("trap") == std::string::npos)
				return "DOOR"; // the game calls one "Closed Door" even when it stands open
			for (size_t k = 0; k < raw.size(); k++) {
				if (k > 0 && isupper((unsigned char)raw[k]) && islower((unsigned char)raw[k - 1]))
					text += ' ';
				text += raw[k];
			}
			while (!text.empty() && (isdigit((unsigned char)text.back()) || text.back() == ' '))
				text.pop_back();
			if (text.empty())
				text = "USE";
		} break;
		default: return "";
	}
	for (char& ch : text)
		ch = (char)toupper((unsigned char)ch);
	return text;
}

} // namespace

void FirstPerson::render(glm::uvec2 game_size, uint32_t game_fbo, uint32_t attachment_count)
{
	if (!m_base || !initGL() || !readFrame())
		return;

	const auto meta = (const FrameMeta*)m_frame_copy.data();
	const auto player = (const PlayerRecord*)(m_frame_copy.data() + sizeof(FrameMeta));
	const auto units = (const UnitRecord*)(m_frame_copy.data() + sizeof(FrameMeta) + sizeof(PlayerRecord));
	if (meta->valid != 1)
		return;

	// ---- save the state the wrapper caches ----
	GLint prev_program = 0, prev_vao = 0, prev_vbo = 0, prev_viewport[4] = { 0 }, prev_texture_unit = 0;
	glGetIntegerv(GL_ACTIVE_TEXTURE, &prev_texture_unit);
	glGetIntegerv(GL_CURRENT_PROGRAM, &prev_program);
	glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prev_vao);
	glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &prev_vbo);
	glGetIntegerv(GL_VIEWPORT, prev_viewport);
	const GLboolean prev_blend = glIsEnabled(GL_BLEND);
	const GLboolean prev_depth = glIsEnabled(GL_DEPTH_TEST);
	const GLboolean prev_cull = glIsEnabled(GL_CULL_FACE);
	const GLboolean prev_scissor = glIsEnabled(GL_SCISSOR_TEST);

	ensureTarget(game_size);

	uint32_t epoch = m_room_epoch;
	const auto hdr = (const StateHeader*)m_base;
	// art that was not on disk when its level was first built (the export may still be running)
	bool art_arrived = false;
	const DWORD tick_now = GetTickCount();
	if (tick_now - m_art_retry_time > 4500) {
		m_art_retry_time = tick_now;
		for (auto& item : g_level_art) {
			if (!item.second.floor && !item.second.wall) {
				const LevelArt& retried = levelArt(item.first);
				art_arrived = art_arrived || retried.floor || retried.wall;
			}
		}
	}

	// Rooms drawn from the game's own tiles are left out of the world built from the collision map.
	TileWorld& tiles = TileWorld::Instance();
	m_inside_level = meta->inside != 0;
	tiles.sync();
	if ((hdr->roomEpoch != m_room_epoch || meta->levelNo != m_level_no || art_arrived || tiles.coverage() != m_tile_coverage) && readRooms(&epoch)) {
		m_tile_coverage = tiles.coverage();
		rebuildWorld();
		m_room_epoch = epoch;
		m_level_no = meta->levelNo;
	}

	// ---- camera ----
	// The game moves the character 25 times a second; the view is drawn far more often. Carry the
	// camera along at the character's speed between game steps instead of jumping with each one.
	LARGE_INTEGER counter, frequency;
	QueryPerformanceCounter(&counter);
	QueryPerformanceFrequency(&frequency);
	const double now = (double)counter.QuadPart / (double)frequency.QuadPart;
	const glm::vec2 sample = { (float)player->px / 65536.0f, (float)player->py / 65536.0f };
	if (!m_cam_valid || glm::distance(sample, m_cam_sample) > 8.0f) {
		m_cam_valid = true;
		m_cam_tick = meta->tick;
		m_cam_sample = m_cam_pos = sample;
		m_cam_velocity = { 0.0f, 0.0f };
		m_cam_sample_time = m_cam_last_time = now;
	} else if (meta->tick != m_cam_tick) {
		const float dt = glm::clamp((float)(now - m_cam_sample_time), 0.02f, 0.12f);
		m_cam_velocity = (sample - m_cam_sample) / dt;
		m_cam_tick = meta->tick;
		m_cam_sample = sample;
		m_cam_sample_time = now;
	}
	// Preferred source: the positions the renderer itself advances every drawn frame from the game's
	// own unit data. The shared state above arrives unevenly (one to three game steps at a time),
	// which made the view judder; it is only the fallback now.
	bool smooth = false;
	m_pred_lookup.clear();
	{
		std::lock_guard<std::mutex> lock(m_pred_lock);
		if (m_pred_player_valid && glm::distance(m_pred_player, sample) < 4.0f) {
			m_cam_pos = m_pred_player;
			smooth = true;
			for (const auto& unit : m_pred_units)
				m_pred_lookup[unit.type_id] = { unit.x, unit.y };
		}
		m_unit_states_render = m_unit_states;
	}
	trackOverlays(now);
	if (!smooth) {
		const glm::vec2 predicted = m_cam_sample + m_cam_velocity * glm::min((float)(now - m_cam_sample_time), 0.05f);
		const float blend = 1.0f - expf(-(float)(now - m_cam_last_time) / 0.035f);
		m_cam_pos += (predicted - m_cam_pos) * glm::clamp(blend, 0.0f, 1.0f);
	}
	m_cam_last_time = now;

	// A unit's position: the smooth one when the renderer tracks it, else the shared state's.
	const auto unitPos = [&](const UnitRecord& u) -> glm::vec2 {
		const bool dynamic = u.type == 0 || u.type == 1 || u.type == 3;
		const glm::vec2 raw = dynamic ? glm::vec2((float)u.px / 65536.0f, (float)u.py / 65536.0f) : glm::vec2((float)u.px + 0.5f, (float)u.py + 0.5f);
		if (dynamic) {
			const auto it = m_pred_lookup.find(u.id | ((uint32_t)u.type << 24));
			if (it != m_pred_lookup.end() && glm::distance(it->second, raw) < 4.0f)
				return it->second;
		}
		return raw;
	};

	// ---- the camera leads the character ----
	// The game acts on input at its next step, so the character starts a little after a key goes
	// down: 40 to 90 ms when sod.dll walks it directly, 60 to 100 ms when it clicks. The camera
	// hides that. While a key is held it sits ahead of the character by about the ground covered
	// in that time, so it starts at once. On release it stays where it is and the character walks
	// up to it: sod.dll sends the character to the camera's spot (STOP_AT, see stopMoving). When it
	// clicks instead, the character walks on to the nearest point a click can send it to, 1.5 to
	// 2.8 subtiles, and the lead is longer to match.
	const bool direct_move = m_cmd && (((const CmdHeader*)m_cmd)->dllFlags & CMD_DLL_DIRECT);
	const float REST_GAP = 0.75f; // a stopped character this close to the camera leaves it where it is
	const glm::vec2 body = m_cam_pos;
	const float frame_dt = glm::clamp((float)(now - m_lead_time), 0.0f, 0.1f);
	m_lead_time = now;
	if (!m_lead_valid || glm::distance(body, m_lead_body) > 6.0f) { // first frame, or a teleport
		m_lead_body = body;
		m_lead_off = { 0.0f, 0.0f };
		m_lead_valid = true;
	}
	const glm::vec2 step = body - m_lead_body;
	m_lead_body = body;
	if (frame_dt > 0.0005f) {
		const float speed = glm::length(step) / frame_dt;
		if (speed > 2.0f)
			m_lead_moved = now;
		if (speed > 2.0f && speed < 40.0f)
			m_lead_speed += (speed - m_lead_speed) * glm::min(1.0f, frame_dt * 4.0f);
	}
	const glm::vec2 intent = { m_move_x, m_move_y };
	const bool steering = intent.x != 0.0f || intent.y != 0.0f;
	if (steering && !m_lead_steering)
		m_lead_press = now;
	m_lead_steering = steering;
	if (steering) {
		const float lead = glm::clamp(m_lead_speed * (direct_move ? 0.08f : 0.16f), 0.5f, 2.6f);
		// Not into a wall: as far as the way ahead is open, less the room the character itself
		// needs (it stops a good subtile short of anything solid). Not into a monster or a
		// townsman either: the character stops in front of them.
		const float margin = 1.25f;
		float clear = 0.0f;
		for (float d = 0.25f; d <= lead + margin; d += 0.25f) {
			if (!walkable(body.x + intent.x * d, body.y + intent.y * d))
				break;
			clear = d;
		}
		const uint32_t lead_units = glm::min((uint32_t)meta->unitCount, hdr->maxUnits);
		for (uint32_t i = 0; i < lead_units; i++) {
			const UnitRecord& u = units[i];
			if (u.type != 1 || u.mode == 0 || u.mode == 12)
				continue; // only what stands in the way: monsters and townsfolk that are alive
			const glm::vec2 rel = unitPos(u) - body;
			const float along = rel.x * intent.x + rel.y * intent.y;
			if (along <= 0.0f || along > lead + margin + 1.5f || fabsf(rel.x * intent.y - rel.y * intent.x) > 1.5f)
				continue;
			clear = glm::min(clear, glm::max(along - 0.75f, 0.0f));
		}
		// A character that does not get going (something is in the way) is not led.
		const bool going = now - m_lead_press < 0.16 || now - m_lead_moved < 0.15;
		const glm::vec2 target = going ? intent * glm::clamp(clear - margin, 0.0f, lead) : glm::vec2(0.0f);
		// Ease towards it, but never faster than the character runs: until the character gets
		// going, the camera moves the way a character that started at once would.
		glm::vec2 move = (target - m_lead_off) * (1.0f - expf(-frame_dt / 0.1f));
		const float most = m_lead_speed * frame_dt, len = glm::length(move);
		if (len > most && len > 0.0f)
			move *= most / len;
		m_lead_off += move;
	} else {
		// Stay put while the character closes the gap. A character that comes to rest a little past
		// the camera (the game stops it in the middle of a subtile) leaves the camera where it is;
		// one that walks on further, or off to the side, takes the camera along.
		const float before = glm::length(m_lead_off);
		m_lead_off -= step;
		const float after = glm::length(m_lead_off);
		const float keep = glm::max(before, REST_GAP);
		if (after > keep && after > 0.0001f)
			m_lead_off *= keep / after;
		// The character has stopped. Where it stopped short, the camera does not creep back to it:
		// three quarters of a subtile either way is nothing to see (the middle of the subtile under
		// the camera is never further off than that), a view that slides after every stop is. Only
		// a larger gap (the character never got there) is closed.
		const float rest = glm::length(m_lead_off);
		if (now - m_lead_moved > 0.1 && rest > REST_GAP)
			m_lead_off *= (REST_GAP + (rest - REST_GAP) * expf(-frame_dt / 0.15f)) / rest;
	}
	m_lead_cam = body + m_lead_off;
	m_cam_spot_x = m_lead_cam.x;
	m_cam_spot_y = m_lead_cam.y;
	m_cam_spot_tick = GetTickCount() | 1;

	const float px = m_lead_cam.x, pz = m_lead_cam.y;
	if (!m_yaw_valid && player->dir64 >= 0) {
		// start out facing the way the character faces; after that the mouse owns the camera
		float start = ((float)player->dir64 + 0.5f) / 64.0f * glm::two_pi<float>() + glm::quarter_pi<float>();
		if (start > glm::pi<float>())
			start -= glm::two_pi<float>();
		m_yaw = start;
		m_yaw_valid = true;
	}
	m_player_x = sample.x;
	m_player_y = sample.y;
	const float yaw = m_yaw, pitch = m_pitch;
	publishView({ 0, px, pz, body.x, body.y, yaw, pitch, intent.x, intent.y });
	const bool looking = m_look;
	const glm::vec3 eye = { px, EYE_HEIGHT, pz };
	const glm::vec3 dir = { cosf(yaw) * cosf(pitch), sinf(pitch), sinf(yaw) * cosf(pitch) };
	const glm::mat4 proj = glm::perspective(glm::radians((float)m_fov), (float)game_size.x / (float)game_size.y, 0.15f, 400.0f);
	const glm::mat4 view = glm::lookAt(eye, eye + dir, glm::vec3(0.0f, 1.0f, 0.0f));
	const glm::mat4 mvp = proj * view;

	const glm::vec3 fog = meta->town ? glm::vec3(0.42f, 0.47f, 0.55f) : glm::vec3(0.035f, 0.035f, 0.05f);

	// ---- units ----
	static std::vector<FpVertex> unit_verts;
	unit_verts.clear();
	const uint32_t unit_count = glm::min((uint32_t)meta->unitCount, hdr->maxUnits);

	// the unit under the crosshair: the nearest one the view direction passes through (D2 aims on the ground plane)
	int target = -1;
	float target_dist = 60.0f;
	const float fx = cosf(yaw), fz = sinf(yaw);
	for (uint32_t i = 0; i < unit_count; i++) {
		const UnitRecord& u = units[i];
		if (u.type == 3 || (u.type == 1 && (u.mode == 0 || u.mode == 12)) || (u.type == 0 && u.id == player->id))
			continue;
		if (u.type == 2 && !(u.rawFlags & 0x0080))
			continue; // scenery (a torch, a banner): nothing to use, and it must not shield what stands behind it
		const glm::vec2 upos = unitPos(u);
		const float dx = upos.x - px, dz = upos.y - pz;
		const float along = dx * fx + dz * fz;
		if (along < 0.4f || along >= target_dist)
			continue;
		const float across = fabsf(dx * fz - dz * fx);
		if (across > 1.3f + along * 0.03f)
			continue;
		target = (int)i;
		target_dist = along;
	}
	if (target >= 0) {
		const UnitRecord& t = units[target];
		const bool moves = t.type == 0 || t.type == 1 || t.type == 3;
		m_target_type = t.type;
		m_target_id = t.id;
		m_target_flags = t.rawFlags;
		m_target_x = moves ? (float)t.px / 65536.0f : (float)t.px + 0.5f;
		m_target_y = moves ? (float)t.py / 65536.0f : (float)t.py + 0.5f;
		InterlockedExchange(&m_target_valid, 1);
	} else
		InterlockedExchange(&m_target_valid, 0);

	// What E would use: of the things that can be talked to, opened, picked up or gone through, the
	// one most straight ahead, a nearer one first when two are about as straight ahead. Wider than
	// the crosshair's line, so a jar at the feet or a chest just off centre still counts.
	int use_target = -1;
	if (looking) {
		float best = 1e9f;
		for (uint32_t i = 0; i < unit_count; i++) {
			const UnitRecord& u = units[i];
			bool usable = false;
			switch (u.type) {
				case 0: usable = u.id != player->id && (u.mode == 0 || u.mode == 17); break; // a corpse: its gear
				case 1: usable = (u.rawFlags & 0x0010) && u.mode != 0 && u.mode != 12; break; // an NPC
				case 2: usable = (u.rawFlags & 0x0080) != 0; break; // usable in the state it is in now
				case 4: // an item on the ground
				case 5: usable = true; break; // an exit
			}
			if (!usable)
				continue;
			const glm::vec2 upos = unitPos(u);
			const float dx = upos.x - px, dz = upos.y - pz;
			const float along = dx * fx + dz * fz;
			if (along < 0.3f || along > USE_REACH)
				continue;
			const float across = fabsf(dx * fz - dz * fx);
			if (across > 0.9f + along * 0.45f)
				continue;
			const float score = across / glm::max(along, 1.0f) * 6.0f + along * 0.15f;
			if (score < best) {
				best = score;
				use_target = (int)i;
			}
		}
	}
	AcquireSRWLockExclusive(&m_use_lock);
	m_use_valid = use_target >= 0;
	if (use_target >= 0) {
		const UnitRecord& t = units[use_target];
		const bool moves = t.type == 0 || t.type == 1 || t.type == 3;
		m_use_type = t.type;
		m_use_id = t.id;
		m_use_x = moves ? (float)t.px / 65536.0f : (float)t.px + 0.5f;
		m_use_y = moves ? (float)t.py / 65536.0f : (float)t.py + 0.5f;
	}
	ReleaseSRWLockExclusive(&m_use_lock);

	// names and codes of this frame's units (weapon class, item codes)
	const char* strings = (const char*)m_frame_copy.data() + sizeof(FrameMeta) + sizeof(PlayerRecord) + hdr->maxUnits * sizeof(UnitRecord);
	const size_t strings_size = m_frame_copy.size() - (sizeof(FrameMeta) + sizeof(PlayerRecord) + hdr->maxUnits * sizeof(UnitRecord));
	// the line under the crosshair: what E would use
	const std::string use_prompt = use_target >= 0 ? useLabel(units[use_target], strings, strings_size) : std::string();

	// The crosshair turns red on a monster a click would hit at once: any monster for a bow or a
	// crossbow, one within arm's reach otherwise. Further away the character walks up to it first.
	bool in_reach = false;
	if (target >= 0 && units[target].type == 1 && !(units[target].rawFlags & 0x0010)) {
		bool ranged = false;
		if (player->wclassLen && (size_t)player->wclassOff + player->wclassLen <= strings_size) {
			const std::string wclass(strings + player->wclassOff, player->wclassLen);
			ranged = wclass == "bow" || wclass == "xbw";
		}
		in_reach = ranged || glm::distance(unitPos(units[target]), sample) < 5.5f; // blows land at about 4 on a small monster, further on a large one
	}

	// The hit marker: a monster lost life, or died, shortly after a click that attacks. In first
	// person the character's own swing is not on screen to show that the blow landed.
	{
		bool hit = false, kill = false;
		m_unit_life_next.clear();
		for (uint32_t i = 0; i < unit_count; i++) {
			const UnitRecord& u = units[i];
			if (u.type != 1 || (u.rawFlags & 0x0010))
				continue;
			const auto before = m_unit_life.find(u.id);
			if (u.mode == 0 || u.mode == 12) { // dying or dead: a kill if it was alive a frame ago
				if (before != m_unit_life.end())
					hit = kill = true;
				continue;
			}
			if (before != m_unit_life.end() && u.hp < before->second)
				hit = true;
			m_unit_life_next[u.id] = u.hp;
		}
		m_unit_life.swap(m_unit_life_next);
		if (hit && GetTickCount() - m_fire_tick < 1500) {
			m_hit_time = now;
			m_hit_kill = kill;
		}
	}

	// what lies on the ground gets its name written over it, always: there is no pointer to hover with
	struct ItemLabel {
		std::string text;
		float x, y, away;
		glm::vec3 color;
		bool targeted;
	};
	static std::vector<ItemLabel> item_labels;
	item_labels.clear();

	// Units are drawn with the game's own sprites when the sprite service has supplied them, as
	// plain boxes otherwise (service not running, or a graphic the game does not have).
	SpriteStore& store = SpriteStore::Instance();
	store.pump();
	static std::unordered_map<GLuint, std::vector<FpVertex>> sprite_verts, glow_verts; // by sheet
	for (auto& item : sprite_verts)
		item.second.clear();
	for (auto& item : glow_verts)
		item.second.clear();
	const int act = glm::clamp(meta->act + 1, 1, 5);

	// One overlay picture on a unit standing at (ux, uz). A ring the game paints on the ground lies
	// flat around the feet; anything else stands up like the unit, just in front of it, or just behind
	// it when the game draws it first. `age` is seconds into its animation; a one-off (`once`) ends
	// after its last frame, a lasting one loops. Returns false once a one-off is over.
	// `ground_only`: only a picture that lies on the ground is drawn (the player's own: the unit is the camera).
	const auto pushOverlay = [&](const OverlayInfo& info, double age, bool once, float ux, float uz, int height_class, float strength,
								 bool ground_only, const Sprite** shown) -> bool {
		const Sprite* sheet = nullptr;
		if (store.get("overlay", info.token, "NU", act, &sheet) != SpriteState::Ready)
			return !once || age < 1.0; // a one-off that is late to arrive is let go
		if (shown)
			*shown = sheet;
		const float cycle = (float)sheet->frames / info.fps;
		float t = (float)age;
		if (once) {
			if (t >= cycle)
				return false;
		} else {
			t = fmodf(t, cycle + info.loop_wait);
			if (t >= cycle)
				return true; // resting between loops
		}
		const int frame = glm::clamp((int)(t * info.fps), 0, sheet->frames - 1);
		const SpriteCell& cell = sheet->cell(0, frame);
		const float raise = -(info.yoff + (height_class >= 1 && height_class <= 4 ? info.height[height_class - 1] : 0.0f)); // screen pixels up
		const float see_through[3] = { 0.25f, 0.5f, 0.75f };
		const float shade = strength * (info.trans >= 0 && info.trans <= 2 ? see_through[info.trans] : 1.0f);
		auto& batch = info.trans == 5 ? sprite_verts[sheet->texture] : glow_verts[sheet->texture];
		if (sheet->bottom >= 0.3f * (sheet->top + sheet->bottom)) {
			pushIsoGround(batch, cell, ux, uz, g_relief.ground(ux, uz) + 0.08f, info.xoff, -raise, shade);
			return true;
		}
		if (ground_only)
			return true;
		glm::vec2 sight = { ux - px, uz - pz };
		const float len = glm::length(sight);
		if (len < 0.3f)
			return true;
		sight /= len;
		const float depth = info.pre_draw ? 0.22f : -0.22f; // behind or in front of the unit's own picture
		const glm::vec2 right = { -sight.y, sight.x };
		const glm::vec2 at = glm::vec2(ux, uz) + sight * depth + right * (info.xoff * SPRITE_SCALE);
		pushBillboard(batch, cell, at.x, at.y, raise * SPRITE_SCALE, px, pz, shade);
		return true;
	};
	// Every overlay a unit has this frame: its states' lasting ones and the one-offs playing on it.
	const auto pushUnitOverlays = [&](uint32_t key, float ux, float uz, int height_class, bool own, glm::vec3* glow) {
		if (!store.overlaysReady())
			return;
		auto found = m_overlay_tracks.find(key);
		if (found == m_overlay_tracks.end() || found->second.seen != now)
			return;
		OverlayTrack& track = found->second;
		const double phase = (double)(key % 89) * 0.37; // units under the same aura do not pulse in step
		const float pulse = 0.85f + 0.15f * sinf((float)now * 2.2f);
		for (uint32_t w = 0; w < 8; w++) {
			for (uint32_t b = 0; track.bits[w] && b < 32; b++) {
				if (!((track.bits[w] >> b) & 1))
					continue;
				const StateInfo* state = store.state(w * 32 + b);
				if (!state)
					continue;
				for (const OverlayInfo* info : state->overlays) {
					const Sprite* shown = nullptr;
					pushOverlay(*info, now + phase, false, ux, uz, height_class, own ? 0.7f : 1.0f, own, &shown);
					if (glow && shown)
						*glow += shown->tint * 0.1f * pulse;
				}
			}
		}
		auto& shots = track.shots;
		for (size_t s = 0; s < shots.size();) {
			const Sprite* shown = nullptr;
			const double age = now - shots[s].start;
			if (!pushOverlay(*shots[s].info, age, true, ux, uz, height_class, own ? 0.8f : 1.0f, own, &shown)) {
				shots.erase(shots.begin() + s);
				continue;
			}
			if (glow && shown) {
				const float cycle = (float)shown->frames / shots[s].info->fps;
				*glow += shown->tint * 0.25f * glm::clamp(1.0f - (float)age / glm::max(cycle, 0.1f), 0.0f, 1.0f);
			}
			s++;
		}
	};

	for (uint32_t i = 0; i < unit_count; i++) {
		const UnitRecord& u = units[i];
		const glm::vec2 upos = unitPos(u);
		const float ux = upos.x, uz = upos.y;
		if (fabsf(ux - px) > 120.0f || fabsf(uz - pz) > 120.0f)
			continue;

		// the player's own unit (a corpse, after dying) sits on the camera: never draw it
		if (u.type == 0 && (u.id == player->id || (fabsf(ux - px) < 1.5f && fabsf(uz - pz) < 1.5f)))
			continue;

		const bool targeted = ((int)i == target || (int)i == use_target) && looking;
		const float shade = targeted ? 1.25f : 1.0f;
		const bool dead = u.type == 1 && (u.mode == 0 || u.mode == 12);

		// ---- sprite ----
		const Sprite* sprite = nullptr;
		SpriteState state = SpriteState::Failed;
		bool drawn = false;
		switch (u.type) {
			case 1: {
				const MonsterInfo* info = store.monster(u.txt);
				if (!info || u.mode >= 16)
					break;
				if (info->hidden)
					continue; // the game shows nothing for it either
				const char* mode = MONSTER_MODES[u.mode];
				state = store.get(info->kind.c_str(), info->token, mode, act, &sprite);
				if (state != SpriteState::Ready)
					break;
				int frame;
				if (u.mode == 12 && sprite->mode == "DT")
					frame = sprite->frames - 1; // a corpse drawn from the death animation: its last frame
				else if (sprite->mode != mode)
					frame = spriteFrame(*sprite, 0, 0, true, now); // a substitute animation runs on its own clock
				else
					frame = spriteFrame(*sprite, u.gfxFrame, u.frameCount, u.mode != 0, now);
				pushBillboard(sprite_verts[sprite->texture], sprite->cell(spriteDirection(*sprite, u.dir64, ux, uz, px, pz), frame), ux, uz, 0.0f, px, pz, shade * info->shade);
				drawn = true;
			} break;
			case 2: {
				const ObjectInfo* info = store.object(u.txt);
				if (!info)
					break;
				if (!info->draw)
					continue; // an invisible helper object
				const int mode_index = u.mode < 8 ? u.mode : 0;
				state = store.get("object", info->token, OBJECT_MODES[mode_index], act, &sprite);
				if (state != SpriteState::Ready)
					break;
				int frame;
				if (u.frameCount > 256 || (u.frameCount > 1 && u.frameCount < 256))
					frame = spriteFrame(*sprite, u.gfxFrame, u.frameCount, info->cycle[mode_index], now);
				else {
					// objects.txt says how many frames a mode uses (a closed chest is 1 frame of a 2-frame sheet)
					const int count = info->frames[mode_index] > 0 ? glm::min(sprite->frames, info->frames[mode_index]) : sprite->frames;
					const float fps = info->delta[mode_index] ? 25.0f * (float)info->delta[mode_index] / 256.0f : sprite->fps;
					if (count <= 1)
						frame = 0;
					else if (info->cycle[mode_index])
						frame = (int)(now * fps) % count;
					else
						frame = mode_index == 0 ? 0 : count - 1; // a one-shot animation with no clock: show where it ends
				}
				// a thing on a mound (a camp's fire on its heap of stones) stands on top of it
				pushBillboard(sprite_verts[sprite->texture], sprite->cell(spriteDirection(*sprite, u.dir64, ux, uz, px, pz), frame), ux, uz, g_relief.ground(ux, uz), px, pz, shade);
				drawn = true;
			} break;
			case 3: {
				const MissileInfo* info = store.missile(u.txt);
				if (!info)
					break;
				if (info->cel.empty())
					continue; // a missile with no picture: the game uses those to carry an effect, unseen
				state = store.get("missile", info->cel, "NU", act, &sprite);
				if (state != SpriteState::Ready)
					break;
				int frame;
				if (u.frameCount > 1)
					frame = spriteFrame(*sprite, u.gfxFrame, u.frameCount, true, now);
				else
					frame = sprite->frames > 1 ? (int)(now * 25.0 * (double)info->anim_rate / 1024.0) % sprite->frames : 0;
				auto& batch = info->additive ? glow_verts[sprite->texture] : sprite_verts[sprite->texture];
				pushBillboard(batch, sprite->cell(spriteDirection(*sprite, u.dir64, ux, uz, px, pz), frame), ux, uz, 0.0f, px, pz, 1.0f);
				drawn = true;
			} break;
			case 4: {
				if (!u.itemCodeLen || (size_t)u.itemCodeOff + u.itemCodeLen > strings_size)
					break;
				std::string code(strings + u.itemCodeOff, u.itemCodeLen);
				while (!code.empty() && (code.back() == ' ' || code.back() == 0))
					code.pop_back();
				// its name over it, in the colour of its quality
				if (const std::string* name = store.itemName(code)) {
					const glm::vec4 clip = mvp * glm::vec4(ux, 0.9f, uz, 1.0f);
					const float away = glm::distance(glm::vec2(ux, uz), glm::vec2(px, pz));
					if (clip.w > 0.2f && away < 34.0f && fabsf(clip.x) < clip.w * 1.1f && fabsf(clip.y) < clip.w * 1.1f) {
						const std::string* type = store.itemType(code);
						glm::vec3 color = { 0.92f, 0.92f, 0.92f };
						switch (u.itemQuality) {
							case 1: color = { 0.62f, 0.62f, 0.62f }; break;  // cracked, crude
							case 4: color = { 0.45f, 0.50f, 1.00f }; break;  // magic
							case 5: color = { 0.20f, 0.95f, 0.20f }; break;  // set
							case 6: color = { 1.00f, 1.00f, 0.42f }; break;  // rare
							case 7: color = { 0.86f, 0.76f, 0.48f }; break;  // unique
							case 8: color = { 1.00f, 0.66f, 0.10f }; break;  // crafted
						}
						if (type && *type == "rune")
							color = { 1.00f, 0.66f, 0.10f };
						ItemLabel label;
						label.text = *name;
						for (char& ch : label.text)
							ch = (char)toupper((unsigned char)ch);
						label.x = clip.x / clip.w;
						label.y = clip.y / clip.w;
						label.away = away;
						label.color = color;
						label.targeted = targeted;
						item_labels.push_back(label);
					}
				}
				const std::string* flippy = store.itemFlippy(code);
				if (!flippy)
					break;
				state = store.get("item", *flippy, "NU", act, &sprite);
				if (state != SpriteState::Ready)
					break;
				pushGroundSprite(sprite_verts[sprite->texture], sprite->cell(0, sprite->frames - 1), ux, uz, px, pz, shade);
				drawn = true;
			} break;
		}
		// auras, curses, shouts and cast flashes on characters and monsters
		if (u.type == 0 || u.type == 1) {
			const MonsterInfo* info = u.type == 1 ? store.monster(u.txt) : nullptr;
			pushUnitOverlays(((uint32_t)u.type << 24) | (u.id & 0x00FFFFFF), ux, uz, info ? info->overlay_height : 2, false, nullptr);
		}
		if (drawn || state == SpriteState::Loading || dead)
			continue; // drawn, about to be, or a corpse with no art (no box for those)

		// ---- box ----
		float half = 0.5f, height = 1.0f, base = 0.0f;
		glm::vec3 color = { 1.0f, 1.0f, 1.0f };
		switch (u.type) {
			case 0: half = 0.7f, height = 3.4f, color = { 0.25f, 0.55f, 0.95f }; break; // other players
			case 1:
				half = 0.8f, height = 3.4f;
				color = (u.rawFlags & 0x0010) ? glm::vec3(0.30f, 0.75f, 0.45f) : glm::vec3(0.80f, 0.16f, 0.12f); // npc : monster
				if (u.rawFlags & 0x004A) // unique / superunique / boss
					half = 1.0f, height = 4.2f, color = { 0.90f, 0.65f, 0.10f };
				break;
			case 2: half = 0.6f, height = 1.4f, color = { 0.62f, 0.50f, 0.28f }; break; // objects
			case 3: half = 0.25f, height = 0.5f, base = 2.0f, color = { 1.0f, 0.85f, 0.35f }; break; // missiles
			case 4: half = 0.3f, height = 0.3f, color = { 0.85f, 0.85f, 0.95f }; break; // items on the ground
			case 5: // exits: a slim marker where the map tiles already show the cave mouth or the stairs
				if (tiles.covers((int)ux, (int)uz, 1, 1))
					half = 0.3f, height = 2.4f;
				else
					half = 1.0f, height = 5.0f;
				color = { 0.45f, 0.25f, 0.75f };
				break;
			default: continue;
		}
		if (targeted)
			color = glm::min(color * 1.25f + glm::vec3(0.18f), glm::vec3(1.0f));
		pushBox(unit_verts, ux - half, uz - half, ux + half, uz + half, base, base + height, color);
	}

	// The player's own overlays: the character is the camera and is never drawn. What its aura or
	// shout paints on the ground lies around the camera's feet (seen when looking down), and every
	// overlay on it tints the edges of the view in its own colour: softly while it lasts, brighter
	// for a moment as it starts or as a skill is cast.
	glm::vec3 own_glow = { 0.0f, 0.0f, 0.0f };
	pushUnitOverlays(player->id & 0x00FFFFFF, px, pz, 2, true, &own_glow);

	// ---- draw into our own target (it has a depth buffer; the game framebuffer does not) ----
	glBindFramebuffer(GL_FRAMEBUFFER, m_fbo);
	glViewport(0, 0, game_size.x, game_size.y);
	glDisable(GL_BLEND);
	glDisable(GL_CULL_FACE);
	glDisable(GL_SCISSOR_TEST);
	glEnable(GL_DEPTH_TEST);
	glDepthFunc(GL_LESS);
	glDepthMask(GL_TRUE);
	glClearColor(fog.r, fog.g, fog.b, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

	glUseProgram(m_program);
	glUniformMatrix4fv(m_u_mvp, 1, GL_FALSE, &mvp[0][0]);
	glUniform3f(m_u_eye, eye.x, eye.y, eye.z);
	glUniform4f(m_u_fog, fog.r, fog.g, fog.b, FOG_FAR);
	glUniform1f(m_u_flat, 0.0f);
	glUniform1f(m_u_additive, 0.0f);
	glUniform1f(m_u_sharp, m_sharp ? 1.0f : 0.0f);

	if (m_world_vertex_count) {
		glBindVertexArray(m_vao_world);
		glActiveTexture(GL_TEXTURE0 + ART_TEXTURE_UNIT);
		glUniform1i(m_u_texture, ART_TEXTURE_UNIT);
		for (const auto& batch : m_batches) {
			if (batch.texture)
				glBindTexture(GL_TEXTURE_2D, batch.texture);
			glUniform1f(m_u_textured, batch.texture ? 1.0f : 0.0f);
			if (batch.indexed)
				glDrawElements(GL_TRIANGLES, batch.count, GL_UNSIGNED_INT, (const void*)((size_t)batch.first * sizeof(uint32_t)));
			else
				glDrawArrays(GL_TRIANGLES, batch.first, batch.count);
		}
		glUniform1f(m_u_textured, 0.0f);
	}
	tiles.draw(m_u_textured, m_u_texture, ART_TEXTURE_UNIT, yaw);
	if (!unit_verts.empty()) {
		glBindVertexArray(m_vao_units);
		glBindBuffer(GL_ARRAY_BUFFER, m_vbo_units);
		glBufferData(GL_ARRAY_BUFFER, unit_verts.size() * sizeof(FpVertex), unit_verts.data(), GL_STREAM_DRAW);
		glDrawArrays(GL_TRIANGLES, 0, (GLsizei)unit_verts.size());
	}

	// sprites: one draw per sheet, then the glowing ones added on top
	{
		glBindVertexArray(m_vao_units);
		glBindBuffer(GL_ARRAY_BUFFER, m_vbo_units);
		glActiveTexture(GL_TEXTURE0 + ART_TEXTURE_UNIT);
		glUniform1i(m_u_texture, ART_TEXTURE_UNIT);
		glUniform1f(m_u_textured, 1.0f);
		const auto drawSheets = [](std::unordered_map<GLuint, std::vector<FpVertex>>& sheets) {
			for (auto& item : sheets) {
				if (item.second.empty())
					continue;
				glBindTexture(GL_TEXTURE_2D, item.first);
				glBufferData(GL_ARRAY_BUFFER, item.second.size() * sizeof(FpVertex), item.second.data(), GL_STREAM_DRAW);
				glDrawArrays(GL_TRIANGLES, 0, (GLsizei)item.second.size());
			}
		};
		drawSheets(sprite_verts);

		GLint blend_src_rgb = GL_ONE, blend_dst_rgb = GL_ZERO, blend_src_alpha = GL_ONE, blend_dst_alpha = GL_ZERO;
		glGetIntegerv(GL_BLEND_SRC_RGB, &blend_src_rgb);
		glGetIntegerv(GL_BLEND_DST_RGB, &blend_dst_rgb);
		glGetIntegerv(GL_BLEND_SRC_ALPHA, &blend_src_alpha);
		glGetIntegerv(GL_BLEND_DST_ALPHA, &blend_dst_alpha);
		glEnable(GL_BLEND);
		glBlendFunc(GL_ONE, GL_ONE);
		glDepthMask(GL_FALSE);
		glUniform1f(m_u_additive, 1.0f);
		drawSheets(glow_verts);
		glUniform1f(m_u_additive, 0.0f);
		glDepthMask(GL_TRUE);
		glDisable(GL_BLEND);
		glBlendFuncSeparate(blend_src_rgb, blend_dst_rgb, blend_src_alpha, blend_dst_alpha); // the wrapper caches its blend state
		glUniform1f(m_u_textured, 0.0f);
	}

	// ---- the player's own hands and what they hold, over everything in the world ----
	{
		Hands::State arms;
		if (player->wclassLen && (size_t)player->wclassOff + player->wclassLen <= strings_size)
			arms.weapon_class.assign(strings + player->wclassOff, player->wclassLen);
		arms.player_class = player->classId;
		arms.act = meta->act + 1;
		Hands::Held* hands[2] = { &arms.right, &arms.left };
		for (int i = 0; i < 2; i++) {
			const auto& held = player->held[i];
			hands[i]->code.assign(held.code, strnlen(held.code, sizeof(held.code)));
			hands[i]->file = held.file;
			hands[i]->quality = held.quality;
			hands[i]->ethereal = (held.flags & 1) != 0;
		}
		arms.mode = player->mode;
		const float frame = player->frameCount >= 256 ? (float)player->gfxFrame / 256.0f : (float)player->gfxFrame;
		const float frames = player->frameCount >= 256 ? (float)(player->frameCount / 256) : (float)player->frameCount;
		arms.progress = frames > 0.0f ? glm::clamp(frame / frames, 0.0f, 1.0f) : 0.0f;
		float turned = yaw - m_arms_yaw;
		if (turned > glm::pi<float>())
			turned -= glm::two_pi<float>();
		if (turned < -glm::pi<float>())
			turned += glm::two_pi<float>();
		arms.look_yaw = m_arms_look_valid ? turned : 0.0f;
		arms.look_pitch = m_arms_look_valid ? pitch - m_arms_pitch : 0.0f;
		m_arms_yaw = yaw;
		m_arms_pitch = pitch;
		m_arms_look_valid = true;
		arms.brightness = meta->town ? 1.0f : 0.85f;
		Hands::Instance().render(arms, game_size, m_fbo);

		// back to the world's program and buffers for the overlay below
		glUseProgram(m_program);
		glBindVertexArray(m_vao_units);
		glBindBuffer(GL_ARRAY_BUFFER, m_vbo_units);
	}

	// ---- the player's own auras and spells: a glow at the edges of the view ----
	own_glow = glm::min(own_glow, glm::vec3(0.3f));
	if (own_glow.r + own_glow.g + own_glow.b > 0.004f) {
		static std::vector<FpVertex> glow_quad;
		glow_quad.clear();
		pushQuad(glow_quad, { -1.0f, -1.0f, 0.0f }, { 1.0f, -1.0f, 0.0f }, { 1.0f, 1.0f, 0.0f }, { -1.0f, 1.0f, 0.0f }, own_glow, 1.0f);
		GLint blend_src_rgb = GL_ONE, blend_dst_rgb = GL_ZERO, blend_src_alpha = GL_ONE, blend_dst_alpha = GL_ZERO;
		glGetIntegerv(GL_BLEND_SRC_RGB, &blend_src_rgb);
		glGetIntegerv(GL_BLEND_DST_RGB, &blend_dst_rgb);
		glGetIntegerv(GL_BLEND_SRC_ALPHA, &blend_src_alpha);
		glGetIntegerv(GL_BLEND_DST_ALPHA, &blend_dst_alpha);
		const glm::mat4 identity(1.0f);
		glDisable(GL_DEPTH_TEST);
		glEnable(GL_BLEND);
		glBlendFuncSeparate(GL_ONE, GL_ONE, GL_ZERO, GL_ONE);
		glUniformMatrix4fv(m_u_mvp, 1, GL_FALSE, &identity[0][0]);
		glUniform1f(m_u_flat, 3.0f);
		glBufferData(GL_ARRAY_BUFFER, glow_quad.size() * sizeof(FpVertex), glow_quad.data(), GL_STREAM_DRAW);
		glDrawArrays(GL_TRIANGLES, 0, (GLsizei)glow_quad.size());
		glUniform1f(m_u_flat, 0.0f);
		glUniformMatrix4fv(m_u_mvp, 1, GL_FALSE, &mvp[0][0]);
		glDisable(GL_BLEND);
		glBlendFuncSeparate(blend_src_rgb, blend_dst_rgb, blend_src_alpha, blend_dst_alpha); // the wrapper caches its blend state
		glEnable(GL_DEPTH_TEST);
	}

	// ---- the names of what lies on the ground ----
	FpText& font = FpText::Instance();
	const bool show_notice = m_notice[0] && GetTickCount() - m_notice_tick < 1800;
	if ((!item_labels.empty() || show_notice || !use_prompt.empty()) && font.ready()) {
		static std::vector<FpVertex> plates;
		static std::vector<FpText::Vertex> letters;
		plates.clear();
		letters.clear();
		// nearest first, so a nearer name keeps its place and a further one moves up out of its way
		std::sort(item_labels.begin(), item_labels.end(), [](const ItemLabel& a, const ItemLabel& b) { return a.away < b.away; });
		const float line = 2.0f * 11.0f / (float)game_size.y; // eleven game pixels high
		const float stretch = (float)game_size.y / (float)game_size.x, pad_y = line * 0.18f, pad_x = line * stretch * 0.35f;
		struct Box {
			float x0, y0, x1, y1;
		};
		static std::vector<Box> boxes;
		boxes.clear();
		for (const ItemLabel& label : item_labels) {
			const float w = font.width(label.text) * line * stretch;
			Box box = { label.x - w * 0.5f - pad_x, label.y - pad_y, label.x + w * 0.5f + pad_x, label.y + line + pad_y };
			for (int tries = 0; tries < 12; tries++) {
				bool clash = false;
				for (const Box& other : boxes)
					clash = clash || (box.x0 < other.x1 && box.x1 > other.x0 && box.y0 < other.y1 && box.y1 > other.y0);
				if (!clash)
					break;
				box.y0 += line + pad_y * 2.5f;
				box.y1 += line + pad_y * 2.5f;
			}
			boxes.push_back(box);
			const uint8_t shade = label.targeted ? 60 : 0, alpha = label.targeted ? 215 : 165;
			const FpVertex corner[4] = { { box.x0, box.y0, 0, shade, shade, shade, alpha, 0, 0 }, { box.x1, box.y0, 0, shade, shade, shade, alpha, 0, 0 }, { box.x1, box.y1, 0, shade, shade, shade, alpha, 0, 0 },
				{ box.x0, box.y1, 0, shade, shade, shade, alpha, 0, 0 } };
			static const int order[6] = { 0, 1, 2, 0, 2, 3 };
			for (const int i : order)
				plates.push_back(corner[i]);
			font.push(letters, label.text, box.x0 + pad_x, box.y0 + pad_y, line, stretch, label.targeted ? glm::min(label.color * 1.2f + glm::vec3(0.1f), glm::vec3(1.0f)) : label.color);
		}
		if (show_notice) {
			// a setting that was just changed, near the top of the view
			const std::string text(m_notice);
			const float tall = line * 1.25f, w = font.width(text) * tall * stretch, y = 0.62f;
			const FpVertex corner[4] = { { -w * 0.5f - pad_x * 2, y - pad_y * 2, 0, 0, 0, 0, 170, 0, 0 }, { w * 0.5f + pad_x * 2, y - pad_y * 2, 0, 0, 0, 0, 170, 0, 0 },
				{ w * 0.5f + pad_x * 2, y + tall + pad_y * 2, 0, 0, 0, 0, 170, 0, 0 }, { -w * 0.5f - pad_x * 2, y + tall + pad_y * 2, 0, 0, 0, 0, 170, 0, 0 } };
			static const int order[6] = { 0, 1, 2, 0, 2, 3 };
			for (const int i : order)
				plates.push_back(corner[i]);
			font.push(letters, text, -w * 0.5f, y, tall, stretch, { 0.95f, 0.88f, 0.62f });
		}
		if (!use_prompt.empty()) {
			// under the crosshair: the key on a lighter cap, then what it would use
			const auto plate = [&](float x0, float y0, float x1, float y1, uint8_t shade, uint8_t alpha) {
				const FpVertex corner[4] = { { x0, y0, 0, shade, shade, shade, alpha, 0, 0 }, { x1, y0, 0, shade, shade, shade, alpha, 0, 0 },
					{ x1, y1, 0, shade, shade, shade, alpha, 0, 0 }, { x0, y1, 0, shade, shade, shade, alpha, 0, 0 } };
				static const int order[6] = { 0, 1, 2, 0, 2, 3 };
				for (const int i : order)
					plates.push_back(corner[i]);
			};
			const float tall = line * 1.15f, y = -0.12f;
			const float key_w = font.width("E") * tall * stretch, cap_w = key_w + pad_x * 3.0f, gap = pad_x * 2.0f;
			const float name_w = font.width(use_prompt) * tall * stretch;
			const float x0 = -(cap_w + gap + name_w) * 0.5f;
			plate(x0 - pad_x * 2.0f, y - pad_y * 2.5f, x0 + cap_w + gap + name_w + pad_x * 2.0f, y + tall + pad_y * 2.5f, 0, 150);
			plate(x0, y - pad_y, x0 + cap_w, y + tall + pad_y, 90, 230);
			font.push(letters, "E", x0 + (cap_w - key_w) * 0.5f, y, tall, stretch, { 1.0f, 0.86f, 0.40f });
			font.push(letters, use_prompt, x0 + cap_w + gap, y, tall, stretch, { 0.94f, 0.94f, 0.94f });
		}

		GLint blend_src_rgb = 0, blend_dst_rgb = 0, blend_src_alpha = 0, blend_dst_alpha = 0;
		glGetIntegerv(GL_BLEND_SRC_RGB, &blend_src_rgb);
		glGetIntegerv(GL_BLEND_DST_RGB, &blend_dst_rgb);
		glGetIntegerv(GL_BLEND_SRC_ALPHA, &blend_src_alpha);
		glGetIntegerv(GL_BLEND_DST_ALPHA, &blend_dst_alpha);
		const glm::mat4 identity(1.0f);
		glDisable(GL_DEPTH_TEST);
		glEnable(GL_BLEND);
		glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ZERO, GL_ONE);
		glUniformMatrix4fv(m_u_mvp, 1, GL_FALSE, &identity[0][0]);
		glBindVertexArray(m_vao_units);
		glBindBuffer(GL_ARRAY_BUFFER, m_vbo_units);
		glUniform1f(m_u_flat, 1.0f);
		glBufferData(GL_ARRAY_BUFFER, plates.size() * sizeof(FpVertex), plates.data(), GL_STREAM_DRAW);
		glDrawArrays(GL_TRIANGLES, 0, (GLsizei)plates.size());
		glUniform1f(m_u_flat, 2.0f);
		glActiveTexture(GL_TEXTURE0 + ART_TEXTURE_UNIT);
		glUniform1i(m_u_texture, ART_TEXTURE_UNIT);
		glBindTexture(GL_TEXTURE_2D, font.texture());
		glBufferData(GL_ARRAY_BUFFER, letters.size() * sizeof(FpText::Vertex), letters.data(), GL_STREAM_DRAW);
		glDrawArrays(GL_TRIANGLES, 0, (GLsizei)letters.size());
		glUniform1f(m_u_flat, 0.0f);
		glDisable(GL_BLEND);
		glBlendFuncSeparate(blend_src_rgb, blend_dst_rgb, blend_src_alpha, blend_dst_alpha); // the wrapper caches its blend state
		glEnable(GL_DEPTH_TEST);
	}

	if (looking) {
		// crosshair, in clip space
		static std::vector<FpVertex> cross;
		cross.clear();
		const float h = 0.022f, w = h * (float)game_size.y / (float)game_size.x, t = 0.14f;
		const glm::vec3 tint = in_reach ? glm::vec3(0.95f, 0.16f, 0.12f) : target >= 0 ? glm::vec3(1.0f, 0.82f, 0.30f) : glm::vec3(0.92f, 0.92f, 0.92f);
		pushQuad(cross, { -w, -h * t, 0 }, { w, -h * t, 0 }, { w, h * t, 0 }, { -w, h * t, 0 }, tint, 1.0f);
		pushQuad(cross, { -w * t, -h, 0 }, { w * t, -h, 0 }, { w * t, h, 0 }, { -w * t, h, 0 }, tint, 1.0f);

		// hit marker: four short strokes on the diagonals, opening out as they go; red and larger for a kill
		const float age = (float)(now - m_hit_time), life = m_hit_kill ? 0.32f : 0.20f;
		if (age >= 0.0f && age < life) {
			const float grow = age / life, size = m_hit_kill ? 1.45f : 1.0f;
			const float inner = h * size * (1.05f + 0.55f * grow), outer = h * size * (2.0f + 0.55f * grow), thick = h * 0.16f * size;
			const float aspect = (float)game_size.y / (float)game_size.x, d = 0.70710678f;
			const glm::vec3 mark = m_hit_kill ? glm::vec3(1.0f, 0.25f, 0.18f) : glm::vec3(1.0f, 1.0f, 1.0f);
			for (int k = 0; k < 4; k++) {
				const float sx = (k & 1) ? 1.0f : -1.0f, sy = (k & 2) ? 1.0f : -1.0f;
				const glm::vec2 dir = { sx * d, sy * d }, side = { -dir.y * thick, dir.x * thick };
				const glm::vec2 a = dir * inner, b = dir * outer;
				pushQuad(cross, { (a.x - side.x) * aspect, a.y - side.y, 0 }, { (b.x - side.x) * aspect, b.y - side.y, 0 }, { (b.x + side.x) * aspect, b.y + side.y, 0 },
					{ (a.x + side.x) * aspect, a.y + side.y, 0 }, mark, 1.0f);
			}
		}
		const glm::mat4 identity(1.0f);
		glDisable(GL_DEPTH_TEST);
		glUniformMatrix4fv(m_u_mvp, 1, GL_FALSE, &identity[0][0]);
		glUniform1f(m_u_flat, 1.0f);
		glBindVertexArray(m_vao_units);
		glBindBuffer(GL_ARRAY_BUFFER, m_vbo_units);
		glBufferData(GL_ARRAY_BUFFER, cross.size() * sizeof(FpVertex), cross.data(), GL_STREAM_DRAW);
		glDrawArrays(GL_TRIANGLES, 0, (GLsizei)cross.size());
		glUniform1f(m_u_flat, 0.0f);
	}

	// ---- copy into the game's colour layer and clear its side layers ----
	glBindFramebuffer(GL_READ_FRAMEBUFFER, m_fbo);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, game_fbo);
	const GLenum first = GL_COLOR_ATTACHMENT0;
	glDrawBuffers(1, &first);
	glBlitFramebuffer(0, 0, game_size.x, game_size.y, 0, 0, game_size.x, game_size.y, GL_COLOR_BUFFER_BIT, GL_NEAREST);

	GLenum all[8];
	for (uint32_t i = 0; i < attachment_count && i < 8; i++)
		all[i] = GL_COLOR_ATTACHMENT0 + i;
	glDrawBuffers(glm::min(attachment_count, 8u), all);
	const float zero[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
	for (uint32_t i = 1; i < attachment_count && i < 8; i++)
		glClearBufferfv(GL_COLOR, i, zero);

	// ---- restore ----
	glBindFramebuffer(GL_FRAMEBUFFER, game_fbo);
	glViewport(prev_viewport[0], prev_viewport[1], prev_viewport[2], prev_viewport[3]);
	glDisable(GL_DEPTH_TEST);
	if (prev_depth)
		glEnable(GL_DEPTH_TEST);
	if (prev_blend)
		glEnable(GL_BLEND);
	if (prev_cull)
		glEnable(GL_CULL_FACE);
	if (prev_scissor)
		glEnable(GL_SCISSOR_TEST);
	glActiveTexture(prev_texture_unit);
	glUseProgram(prev_program);
	glBindVertexArray(prev_vao);
	glBindBuffer(GL_ARRAY_BUFFER, prev_vbo);
}

}
