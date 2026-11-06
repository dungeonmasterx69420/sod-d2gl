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

#include "pch.h"
#include "first_person_sprites.h"
#include <nlohmann/json.hpp>
#include <stb/stb_image.h>

namespace d2gl::modules {

namespace {

const char* ART_DIR = "sod-art\\";
const int SPRITE_TEXTURE_UNIT = 30;
const DWORD POLL_MS = 250;
const DWORD LATE_MS = 3000;

using json = nlohmann::json;

bool fileExists(const std::string& path)
{
	const DWORD attributes = GetFileAttributesA(path.c_str());
	return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
}

// Only names that are safe as a file name are ever asked for.
bool safeName(const std::string& name)
{
	if (name.empty() || name.size() > 32)
		return false;
	for (const char c : name)
		if (!(isalnum((unsigned char)c) || c == '_' || c == '-'))
			return false;
	return true;
}

json readJson(const std::string& path)
{
	std::ifstream file(path);
	if (!file.is_open())
		return json();
	return json::parse(file, nullptr, false);
}

// Fills the layout part of a sprite from the service's description of a sheet.
bool parseLayout(const json& doc, Sprite& sprite)
{
	if (!doc.is_object() || !doc.contains("frames") || !doc.contains("atlas") || !doc.contains("anchor"))
		return false;

	const float atlas_w = doc["atlas"].value("w", 0.0f), atlas_h = doc["atlas"].value("h", 0.0f);
	const float anchor_x = doc["anchor"].value("x", 0.0f), anchor_y = doc["anchor"].value("y", 0.0f);
	const auto& rows = doc["frames"];
	if (atlas_w <= 0.0f || atlas_h <= 0.0f || !rows.is_array() || rows.empty() || !rows[0].is_array() || rows[0].empty())
		return false;

	sprite.directions = (int)rows.size();
	sprite.frames = (int)rows[0].size();
	sprite.cells.assign((size_t)sprite.directions * sprite.frames, SpriteCell());
	float top = 0.0f, bottom = 0.0f;
	for (int d = 0; d < sprite.directions; d++) {
		const auto& row = rows[d];
		for (int f = 0; f < sprite.frames && f < (int)row.size(); f++) {
			const auto& frame = row[f];
			if (!frame.is_object() || !frame.contains("box") || !frame["box"].is_array() || frame["box"].size() != 4)
				continue;
			const float x = frame.value("x", 0.0f), y = frame.value("y", 0.0f);
			const float bx = frame["box"][0].get<float>(), by = frame["box"][1].get<float>();
			const float bw = frame["box"][2].get<float>(), bh = frame["box"][3].get<float>();
			if (bw <= 0.0f || bh <= 0.0f)
				continue;

			SpriteCell& cell = sprite.cells[(size_t)d * sprite.frames + f];
			cell.valid = true;
			// half a texel in from the edges: the sheet is filtered and frames sit edge to edge
			cell.u0 = (x + bx + 0.5f) / atlas_w;
			cell.u1 = (x + bx + bw - 0.5f) / atlas_w;
			cell.v0 = (y + by + 0.5f) / atlas_h;
			cell.v1 = (y + by + bh - 0.5f) / atlas_h;
			cell.cx = bx + bw * 0.5f - anchor_x;
			cell.cy = anchor_y - by - bh * 0.5f;
			cell.w = bw;
			cell.h = bh;
			if (d == 0) {
				top = glm::max(top, anchor_y - by);
				bottom = glm::max(bottom, by + bh - anchor_y);
			}
		}
	}
	sprite.top = glm::max(top, 8.0f);
	sprite.bottom = bottom;

	for (int i = 0; i < 64; i++)
		sprite.dir64[i] = (uint8_t)(i * sprite.directions / 64);
	if (doc.contains("dir64ToIndex") && doc["dir64ToIndex"].is_array() && doc["dir64ToIndex"].size() == 64)
		for (int i = 0; i < 64; i++)
			sprite.dir64[i] = (uint8_t)glm::clamp(doc["dir64ToIndex"][i].get<int>(), 0, sprite.directions - 1);

	sprite.iso_yaw = doc.value("isoCameraYaw", 3.926991f);
	sprite.fps = doc.value("fps", 12.5f);
	sprite.mode = doc.value("mode", std::string());
	return true;
}

}

SpriteStore::~SpriteStore()
{
	if (m_worker_started) {
		{
			std::lock_guard<std::mutex> lock(m_lock);
			m_quit = true;
		}
		m_wake.notify_all();
		// The process is on its way out when this runs; the worker only reads files, so it is left alone.
		m_worker.detach();
	}
}

// ---- worker thread: every file access happens here ----

bool SpriteStore::loadIndexes()
{
	const std::string dir = std::string(ART_DIR) + "index\\";
	const json monsters = readJson(dir + "monsters.json");
	const json objects = readJson(dir + "objects.json");
	const json missiles = readJson(dir + "missiles.json");
	const json items = readJson(dir + "items.json");
	if (!monsters.is_object() || !monsters.contains("monsters") || !objects.is_object() || !objects.contains("objects"))
		return false;

	for (const auto& item : monsters["monsters"].items()) {
		const uint32_t txt = (uint32_t)atoi(item.key().c_str());
		if (txt > 100000)
			continue;
		if (m_monsters.size() <= txt)
			m_monsters.resize(txt + 1);
		const auto& row = item.value();
		MonsterInfo& info = m_monsters[txt];
		info.token = row.value("code", std::string());
		if (row.contains("name") && row["name"].is_string())
			info.name = row["name"].get<std::string>();
		info.hidden = row.value("hidden", false);
		info.ally = row.value("ally", false);
		if (row.contains("overlayHeight") && row["overlayHeight"].is_number_integer())
			info.overlay_height = row["overlayHeight"].get<int>();
		if (row.contains("sprite") && row["sprite"].is_object()) {
			const auto& as = row["sprite"];
			info.kind = as.value("kind", std::string("monster"));
			info.token = as.value("token", info.token);
			info.shade = as.value("shade", 1.0f);
		}
	}

	for (const auto& item : objects["objects"].items()) {
		const auto& row = item.value();
		ObjectInfo info;
		info.token = row.value("token", std::string());
		info.draw = row.value("draw", 1) != 0;
		for (int i = 0; i < 8; i++) {
			if (row.contains("frames") && row["frames"].size() > (size_t)i)
				info.frames[i] = row["frames"][i].get<int>();
			if (row.contains("delta") && row["delta"].size() > (size_t)i)
				info.delta[i] = row["delta"][i].get<int>();
			if (row.contains("cycle") && row["cycle"].size() > (size_t)i)
				info.cycle[i] = row["cycle"][i].get<bool>();
		}
		m_objects[(uint32_t)atoi(item.key().c_str())] = info;
	}

	if (missiles.is_object() && missiles.contains("missiles")) {
		for (const auto& item : missiles["missiles"].items()) {
			const auto& row = item.value();
			MissileInfo info;
			if (row.contains("cel") && row["cel"].is_string())
				info.cel = row["cel"].get<std::string>();
			info.additive = row.value("trans", 0) > 0;
			info.anim_rate = row.value("animRate", 1024);
			m_missiles[(uint32_t)atoi(item.key().c_str())] = info;
		}
	}

	if (items.is_object() && items.contains("items")) {
		for (const auto& item : items["items"].items()) {
			const auto& row = item.value();
			if (row.contains("flippy") && row["flippy"].is_string())
				m_item_flippies[item.key()] = row["flippy"].get<std::string>();
			if (row.contains("name") && row["name"].is_string())
				m_item_names[item.key()] = row["name"].get<std::string>();
			if (row.contains("type") && row["type"].is_string())
				m_item_types[item.key()] = row["type"].get<std::string>();
		}
	}
	return true;
}

bool SpriteStore::loadOverlays()
{
	const json doc = readJson(std::string(ART_DIR) + "index\\overlays.json");
	if (!doc.is_object() || !doc.contains("overlays") || !doc.contains("states") || !doc["overlays"].is_object() || !doc["states"].is_object())
		return false;

	m_overlays.clear();
	m_states.clear();
	m_skill_casts.clear();
	for (const auto& item : doc["overlays"].items()) {
		const auto& row = item.value();
		OverlayInfo info;
		info.token = row.value("token", std::string());
		if (info.token.empty())
			continue;
		// the game's overlay clock: AnimRate 16 is a frame every game frame, 25 a second
		info.fps = glm::clamp(25.0f * (float)row.value("animRate", 16) / 16.0f, 1.0f, 60.0f);
		info.loop_wait = (float)row.value("loopWait", 0) / 25.0f;
		info.trans = row.value("trans", 3);
		info.pre_draw = row.value("preDraw", false);
		info.xoff = (float)row.value("xoff", 0);
		info.yoff = (float)row.value("yoff", 0);
		if (row.contains("height") && row["height"].is_array())
			for (size_t i = 0; i < 4 && i < row["height"].size(); i++)
				info.height[i] = row["height"][i].get<float>();
		m_overlays[item.key()] = info;
	}
	const auto find = [this](const json& name) -> const OverlayInfo* {
		if (!name.is_string())
			return nullptr;
		const auto it = m_overlays.find(name.get<std::string>());
		return it == m_overlays.end() ? nullptr : &it->second;
	};
	for (const auto& item : doc["states"].items()) {
		const uint32_t id = (uint32_t)atoi(item.key().c_str());
		if (id >= 1024)
			continue;
		if (m_states.size() <= id)
			m_states.resize(id + 1);
		const auto& row = item.value();
		StateInfo& state = m_states[id];
		if (row.contains("overlays") && row["overlays"].is_array())
			for (const auto& name : row["overlays"])
				if (const OverlayInfo* info = find(name))
					state.overlays.push_back(info);
		if (row.contains("cast"))
			state.cast = find(row["cast"]);
		if (row.contains("remove"))
			state.remove = find(row["remove"]);
		state.aura = row.value("aura", false);
		state.curse = row.value("curse", false);
	}
	if (doc.contains("skills") && doc["skills"].is_object())
		for (const auto& item : doc["skills"].items())
			if (const OverlayInfo* info = item.value().contains("cast") ? find(item.value()["cast"]) : nullptr)
				m_skill_casts[(uint32_t)atoi(item.key().c_str())] = info;
	return true;
}

SpriteStore::Result SpriteStore::runJob(const Job& job)
{
	Result result;
	result.key = job.key;

	const std::string base = std::string(ART_DIR) + "sprite\\" + job.kind + "\\" + job.token + "\\" + job.mode + "\\act" + std::to_string(job.act);
	if (fileExists(base + ".json")) {
		const json doc = readJson(base + ".json");
		if (!parseLayout(doc, result.sprite)) {
			result.outcome = 2;
			return result;
		}
		if (result.sprite.mode.empty())
			result.sprite.mode = job.mode;

		int channels = 0;
		stbi_set_flip_vertically_on_load_thread(0);
		result.pixels = stbi_load((base + ".png").c_str(), &result.width, &result.height, &channels, 4);
		result.outcome = result.pixels ? 1 : 2;
		if (result.pixels && job.kind == "overlay") {
			// its colour, for when the picture itself cannot be shown (the player's own aura): the
			// brighter pixels count for more, as they do when the game adds the picture to the view
			double r = 0.0, g = 0.0, b = 0.0;
			const size_t count = (size_t)result.width * result.height;
			for (size_t i = 0; i < count; i++) {
				const uint8_t* px = result.pixels + i * 4;
				if (px[3] < 128)
					continue;
				const double weight = (double)glm::max(px[0], glm::max(px[1], px[2]));
				r += px[0] * weight;
				g += px[1] * weight;
				b += px[2] * weight;
			}
			const double peak = glm::max(r, glm::max(g, b));
			if (peak > 0.0)
				result.sprite.tint = { (float)(r / peak), (float)(g / peak), (float)(b / peak) };
		}
		return result;
	}
	if (fileExists(base + ".fail")) {
		result.outcome = 2;
		return result;
	}

	// not there yet: ask the service for it (an empty file whose name is the request)
	const std::string dir = std::string(ART_DIR) + "requests";
	CreateDirectoryA(dir.c_str(), NULL);
	const std::string request = dir + "\\" + job.kind + "__" + job.token + "__" + job.mode + "__" + std::to_string(job.act) + ".req";
	if (!fileExists(request)) {
		const HANDLE file = CreateFileA(request.c_str(), GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
		if (file != INVALID_HANDLE_VALUE)
			CloseHandle(file);
	}
	return result;
}

void SpriteStore::workerMain()
{
	// the lookup tables come first; without them nothing can be asked for
	while (!loadIndexes()) {
		m_monsters.clear();
		m_objects.clear();
		m_missiles.clear();
		m_item_flippies.clear();
		m_item_names.clear();
		m_item_types.clear();
		std::unique_lock<std::mutex> lock(m_lock);
		if (m_wake.wait_for(lock, std::chrono::seconds(2), [this] { return m_quit; }))
			return;
	}
	m_index_ready = true;
	m_overlays_ready = loadOverlays();

	for (;;) {
		Job job;
		{
			std::unique_lock<std::mutex> lock(m_lock);
			if (m_overlays_ready)
				m_wake.wait(lock, [this] { return m_quit || !m_jobs.empty(); });
			else if (!m_wake.wait_for(lock, std::chrono::seconds(3), [this] { return m_quit || !m_jobs.empty(); })) {
				// the overlay index may come after the others (a service that is still writing them)
				lock.unlock();
				m_overlays_ready = loadOverlays();
				continue;
			}
			if (m_quit)
				return;
			job = m_jobs.front();
			m_jobs.pop_front();
		}
		Result result = runJob(job);
		std::lock_guard<std::mutex> lock(m_lock);
		m_results.push_back(std::move(result));
	}
}

// ---- render thread ----

void SpriteStore::pump()
{
	if (!m_worker_started) {
		m_worker_started = true;
		m_worker = std::thread(&SpriteStore::workerMain, this);
	}

	static std::vector<Result> results;
	results.clear();
	{
		std::lock_guard<std::mutex> lock(m_lock);
		results.swap(m_results);
	}
	if (results.empty())
		return;

	GLint prev_unit = 0;
	glGetIntegerv(GL_ACTIVE_TEXTURE, &prev_unit);
	glActiveTexture(GL_TEXTURE0 + SPRITE_TEXTURE_UNIT);

	const DWORD now = GetTickCount();
	for (auto& result : results) {
		Entry& entry = m_entries[result.key];
		entry.job_queued = false;
		if (result.outcome == 0) {
			entry.next_poll = now + POLL_MS;
			continue;
		}
		if (result.outcome == 2 || !result.pixels) {
			entry.state = SpriteState::Failed;
			if (result.pixels)
				stbi_image_free(result.pixels);
			continue;
		}

		GLuint id = 0;
		glGenTextures(1, &id);
		glBindTexture(GL_TEXTURE_2D, id);
		glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, result.width, result.height, 0, GL_RGBA, GL_UNSIGNED_BYTE, result.pixels);
		glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
		// smooth when close; no mipmaps, frames sit edge to edge in the sheet (see the texel inset)
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		stbi_image_free(result.pixels);

		entry.sprite = std::move(result.sprite);
		entry.sprite.texture = id;
		entry.state = SpriteState::Ready;
	}

	glActiveTexture(prev_unit);
}

SpriteState SpriteStore::get(const char* kind, const std::string& token, const char* mode, int act, const Sprite** sprite)
{
	*sprite = nullptr;
	if (!m_index_ready || !safeName(token))
		return SpriteState::Failed;

	act = glm::clamp(act, 1, 5);
	std::string key = kind;
	key += '/';
	key += token;
	key += '/';
	key += mode;
	key += '/';
	key += (char)('0' + act);

	const DWORD now = GetTickCount();
	auto it = m_entries.find(key);
	if (it == m_entries.end()) {
		it = m_entries.emplace(key, Entry()).first;
		it->second.first_asked = now;
	}
	Entry& entry = it->second;

	if (entry.state == SpriteState::Ready) {
		*sprite = &entry.sprite;
		return SpriteState::Ready;
	}
	if (entry.state == SpriteState::Failed)
		return SpriteState::Failed;

	if (!entry.job_queued && (int32_t)(now - entry.next_poll) >= 0) {
		entry.job_queued = true;
		{
			std::lock_guard<std::mutex> lock(m_lock);
			m_jobs.push_back({ key, kind, token, mode, act });
		}
		m_wake.notify_one();
	}
	return now - entry.first_asked > LATE_MS ? SpriteState::Late : SpriteState::Loading;
}

const MonsterInfo* SpriteStore::monster(uint32_t txt) const
{
	if (!m_index_ready || txt >= m_monsters.size() || m_monsters[txt].token.empty())
		return nullptr;
	return &m_monsters[txt];
}

const ObjectInfo* SpriteStore::object(uint32_t txt) const
{
	if (!m_index_ready)
		return nullptr;
	const auto it = m_objects.find(txt);
	return it == m_objects.end() ? nullptr : &it->second;
}

const MissileInfo* SpriteStore::missile(uint32_t txt) const
{
	if (!m_index_ready)
		return nullptr;
	const auto it = m_missiles.find(txt);
	return it == m_missiles.end() ? nullptr : &it->second;
}

const std::string* SpriteStore::itemName(const std::string& code) const
{
	const auto it = m_item_names.find(code);
	return it == m_item_names.end() ? nullptr : &it->second;
}

const std::string* SpriteStore::itemType(const std::string& code) const
{
	const auto it = m_item_types.find(code);
	return it == m_item_types.end() ? nullptr : &it->second;
}

const StateInfo* SpriteStore::state(uint32_t id) const
{
	if (!m_overlays_ready || id >= m_states.size())
		return nullptr;
	const StateInfo& state = m_states[id];
	return state.overlays.empty() && !state.cast && !state.remove ? nullptr : &state;
}

const OverlayInfo* SpriteStore::skillCast(uint32_t skill) const
{
	if (!m_overlays_ready)
		return nullptr;
	const auto it = m_skill_casts.find(skill);
	return it == m_skill_casts.end() ? nullptr : it->second;
}

const std::string* SpriteStore::itemFlippy(const std::string& code) const
{
	if (!m_index_ready)
		return nullptr;
	const auto it = m_item_flippies.find(code);
	return it == m_item_flippies.end() ? nullptr : &it->second;
}

}
