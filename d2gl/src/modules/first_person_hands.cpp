/*
	Scrolls of Diablo: the player's hands and what they hold, drawn as pixel art.
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
#include "first_person_hands.h"
#include <nlohmann/json.hpp>
#include <stb/stb_image.h>

namespace d2gl::modules {

namespace {

using json = nlohmann::json;

const char* RIG_JSON = "sod-art\\hands\\rig.json";
const char* HANDS_JSON = "sod-art\\hands\\hands.json";
const char* HANDS_PNG = "sod-art\\hands\\hands.png";
const int TEXTURE_UNIT = 30;      // the first-person view's own unit
const float IDLE_PERIOD = 3.4f;   // seconds per idle cycle

// the game's player animation modes, by number
const char* PLAYER_MODES[20] = { "DT", "NU", "WL", "RN", "GH", "TN", "TW", "A1", "A2", "BL", "SC", "TH", "KK", "S1", "S2", "S3", "S4", "DD", "SQ", "KB" };

const char* VS_SRC = R"(#version 330 core
layout(location = 0) in vec2 a_Pos;
layout(location = 1) in vec2 a_UV;
layout(location = 2) in vec2 a_Kind;
uniform vec2 u_Size;
out vec2 v_UV;
flat out int v_Kind;
out float v_Alpha;
void main() {
	v_UV = a_UV;
	v_Kind = int(a_Kind.x + 0.5);
	v_Alpha = a_Kind.y;
	gl_Position = vec4(a_Pos.x / u_Size.x * 2.0 - 1.0, 1.0 - a_Pos.y / u_Size.y * 2.0, 0.0, 1.0);
})";

// kind 0: an item picture as it is; 1: hand art, shade and material coloured from the class's ramps;
// 2: the finished small picture onto the screen (premultiplied)
const char* FS_SRC = R"(#version 330 core
in vec2 v_UV;
flat in int v_Kind;
in float v_Alpha;
uniform sampler2D u_Texture;
uniform vec3 u_Ramp[64];
uniform float u_Light;
layout(location = 0) out vec4 FragColor;
void main() {
	vec4 c = texture(u_Texture, v_UV);
	if (v_Kind == 2) {
		FragColor = c;
		return;
	}
	if (c.a < 0.5)
		discard;
	if (v_Kind == 1) {
		int level = clamp(int(c.r * 255.0 / 17.0 + 0.5), 0, 15);
		int material = clamp(int(c.g * 255.0 / 64.0 + 0.5), 0, 3);
		FragColor = vec4(u_Ramp[material * 16 + level] * u_Light, 1.0);
	} else
		FragColor = vec4(c.rgb * u_Light, v_Alpha);
})";

struct Vertex {
	float x, y, u, v, kind, alpha;
};

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
		error_log("Hands: shader compile failed: %s", msg);
		glDeleteShader(id);
		return 0;
	}
	return id;
}

std::string readText(const char* path)
{
	std::ifstream file(path, std::ios::binary);
	if (!file.is_open())
		return std::string();
	return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

std::string lower(std::string text)
{
	for (auto& c : text)
		c = (char)tolower((unsigned char)c);
	return text;
}

float num(const json& v, float fallback = 0.0f)
{
	return v.is_number() ? v.get<float>() : fallback;
}

// degrees in -180..180
float wrapDegrees(float d)
{
	d = fmodf(d + 180.0f, 360.0f);
	if (d < 0.0f)
		d += 360.0f;
	return d - 180.0f;
}

// Degrees counter-clockwise from straight up of a picture vector (y down).
float screenAngle(float dx, float dy)
{
	return glm::degrees(atan2f(-dx, -dy));
}

bool isDual(const std::string& wclass)
{
	return wclass == "ht2" || wclass == "1ss" || wclass == "1js" || wclass == "1jt" || wclass == "1st";
}

}

// ---- loading ----

bool Hands::loadRig(const std::string& rig_text, const std::string& hands_text)
{
	const json rig = json::parse(rig_text, nullptr, false);
	const json hands = json::parse(hands_text, nullptr, false);
	if (!rig.is_object() || !hands.is_object() || !rig.contains("classes") || !hands.contains("poses") || !hands.contains("arms"))
		return false;

	// ---- the hand pictures ----
	m_angles = glm::max(1, hands.value("angles", 32));
	m_pose_names.clear();
	for (int side = 0; side < 2; side++)
		for (auto& list : m_frames[side])
			list.clear();
	for (const auto& item : hands["poses"].items()) {
		const std::string& key = item.key();
		if (key.size() < 3 || key[1] != '/' || (key[0] != 'R' && key[0] != 'L') || !item.value().is_array())
			continue;
		const int side = key[0] == 'R' ? R : L;
		const std::string name = key.substr(2);
		int pose = (int)(std::find(m_pose_names.begin(), m_pose_names.end(), name) - m_pose_names.begin());
		if (pose == (int)m_pose_names.size()) {
			if (pose >= 8)
				continue;
			m_pose_names.push_back(name);
		}
		for (const auto& row : item.value()) {
			HandFrame f;
			f.x = row.value("x", 0);
			f.y = row.value("y", 0);
			f.w = row.value("w", 0);
			f.h = row.value("h", 0);
			f.ax = num(row.value("ax", json()));
			f.ay = num(row.value("ay", json()));
			f.angle = num(row.value("angle", json()));
			f.fingers = num(row.value("fingers", json()));
			if (row.contains("wrist") && row["wrist"].is_array() && row["wrist"].size() == 2) {
				f.wrist_x = num(row["wrist"][0]);
				f.wrist_y = num(row["wrist"][1]);
			}
			m_frames[side][pose].push_back(f);
		}
	}
	const auto poseNumber = [this](const char* name) {
		const auto it = std::find(m_pose_names.begin(), m_pose_names.end(), name);
		return it == m_pose_names.end() ? -1 : (int)(it - m_pose_names.begin());
	};
	m_pose_grip = poseNumber("grip");
	m_pose_fist = poseNumber("fist");
	if (m_pose_grip < 0 || m_pose_fist < 0)
		return false;

	for (const auto& item : hands["arms"].items()) {
		const std::string& key = item.key();
		if (key.size() < 3 || key[1] != '/' || (key[0] != 'R' && key[0] != 'L'))
			continue;
		ArmPiece& arm = m_arms[key[0] == 'R' ? R : L][key.substr(2) == "sleeve" ? 1 : 0];
		const auto& row = item.value();
		arm.x = row.value("x", 0);
		arm.y = row.value("y", 0);
		arm.w = row.value("w", 0);
		arm.h = row.value("h", 0);
		arm.wx = num(row.value("wx", json()));
		arm.wy = num(row.value("wy", json()));
		arm.angle = num(row.value("angle", json()), 180.0f);
	}

	// ---- colours ----
	std::vector<std::string> materials;
	if (hands.contains("materials") && hands["materials"].is_array())
		for (const auto& m : hands["materials"])
			materials.push_back(m.is_string() ? m.get<std::string>() : std::string());
	std::unordered_map<std::string, int> ramp_numbers;
	m_ramps.clear();
	if (rig.contains("ramps") && rig["ramps"].is_object())
		for (const auto& item : rig["ramps"].items()) {
			if (!item.value().is_array() || item.value().size() != 16)
				continue;
			ramp_numbers[item.key()] = (int)(m_ramps.size() / 16);
			for (const auto& hex : item.value()) {
				unsigned int rgb = 0;
				if (hex.is_string() && hex.get<std::string>().size() == 7)
					rgb = (unsigned int)strtoul(hex.get<std::string>().c_str() + 1, nullptr, 16);
				m_ramps.push_back(glm::vec3((float)((rgb >> 16) & 255), (float)((rgb >> 8) & 255), (float)(rgb & 255)) / 255.0f);
			}
		}
	m_looks.clear();
	if (rig.contains("looks") && rig["looks"].is_object())
		for (const auto& item : rig["looks"].items()) {
			Look look;
			const auto& row = item.value();
			for (size_t m = 0; m < materials.size() && m < 4; m++) {
				const std::string name = row.value(materials[m], std::string());
				const auto it = ramp_numbers.find(name);
				look.ramp[m] = it == ramp_numbers.end() ? 0 : it->second;
			}
			look.sleeve = row.value("arm", std::string()) == "sleeve";
			m_looks[item.key()] = look;
		}
	m_class_looks.clear();
	if (rig.contains("classNames") && rig["classNames"].is_array())
		for (const auto& name : rig["classNames"])
			m_class_looks.push_back(name.is_string() ? name.get<std::string>() : std::string());
	if (m_ramps.empty() || m_looks.empty())
		return false;

	// ---- grips, modes ----
	m_grips.clear();
	if (rig.contains("grips") && rig["grips"].is_object())
		for (const auto& item : rig["grips"].items()) {
			Grip g;
			const auto& row = item.value();
			g.f = num(row.value("f", json()), 0.15f);
			if (row.contains("two") && row["two"].is_array() && row["two"].size() == 2) {
				g.has_two = true;
				g.two[0] = num(row["two"][0]);
				g.two[1] = num(row["two"][1]);
			}
			if (row.contains("guard") && row["guard"].is_number()) {
				g.has_guard = true;
				g.guard = row["guard"].get<float>();
			}
			g.fingers = row.value("along", std::string()) == "fingers";
			g.center = row.value("center", false);
			m_grips[item.key()] = g;
		}
	if (m_grips.find("weap") == m_grips.end())
		m_grips["weap"] = Grip();
	m_mode_clips.clear();
	if (rig.contains("modes") && rig["modes"].is_object())
		for (const auto& item : rig["modes"].items())
			if (item.value().is_string())
				m_mode_clips[item.key()] = item.value().get<std::string>();

	// ---- where the arms come from, and every weapon class's poses ----
	m_lines = glm::clamp(rig.value("lines", 200), 60, 1000);
	const auto vec2 = [](const json& v, glm::vec2& out) {
		if (!v.is_array() || v.size() != 2 || !v[0].is_number() || !v[1].is_number())
			return false;
		out = { v[0].get<float>(), v[1].get<float>() };
		return true;
	};
	m_elbow[R] = { 150.0f, -70.0f };
	m_elbow[L] = { -150.0f, -70.0f };
	if (rig.contains("elbow") && rig["elbow"].is_object()) {
		vec2(rig["elbow"].value("R", json()), m_elbow[R]);
		vec2(rig["elbow"].value("L", json()), m_elbow[L]);
	}

	const auto pose = [this](const json& v) {
		Pose p;
		if (!v.is_array() || v.size() < 3 || !v[0].is_number() || !v[1].is_number() || !v[2].is_number())
			return p;
		p.valid = true;
		p.x = v[0].get<float>();
		p.y = v[1].get<float>();
		p.angle = v[2].get<float>();
		if (v.size() > 3 && v[3].is_string()) {
			const auto it = std::find(m_pose_names.begin(), m_pose_names.end(), v[3].get<std::string>());
			p.pose = it == m_pose_names.end() ? -1 : (int)(it - m_pose_names.begin());
		}
		return p;
	};
	// a clip is a list of keys, or {"same": other} or {"mirror": other} (the other hand, the other way)
	std::function<Clip(const json&, const std::string&, int)> readClip = [&](const json& clips, const std::string& name, int depth) {
		Clip out;
		if (depth > 4 || !clips.contains(name))
			return out;
		const json& c = clips[name];
		if (c.is_object()) {
			if (c.contains("same") && c["same"].is_string())
				return readClip(clips, c["same"].get<std::string>(), depth + 1);
			if (c.contains("mirror") && c["mirror"].is_string()) {
				for (const Key& k : readClip(clips, c["mirror"].get<std::string>(), depth + 1)) {
					Key m;
					m.t = k.t;
					for (int side = 0; side < 2; side++) {
						Pose p = k.side[side];
						p.x = -p.x;
						p.angle = -p.angle;
						m.side[1 - side] = p;
					}
					m.drop = k.drop < 0 ? -1 : 1 - k.drop;
					out.push_back(m);
				}
			}
			return out;
		}
		if (!c.is_array())
			return out;
		for (const auto& row : c) {
			if (!row.is_object())
				continue;
			Key k;
			k.t = num(row.value("t", json()));
			k.side[R] = pose(row.value("R", json()));
			k.side[L] = pose(row.value("L", json()));
			const std::string drop = row.value("drop", std::string());
			k.drop = drop == "R" ? R : drop == "L" ? L : -1;
			out.push_back(k);
		}
		return out;
	};

	m_classes.clear();
	const json& classes = rig["classes"];
	for (const auto& item : classes.items()) {
		const json* row = &item.value();
		if (row->contains("alias") && (*row)["alias"].is_string() && classes.contains((*row)["alias"].get<std::string>()))
			row = &classes[(*row)["alias"].get<std::string>()];
		if (!row->is_object())
			continue;
		ClassRig cls;
		cls.two = row->value("two", false);
		cls.bow = row->value("bow", false);
		for (int side = 0; side < 2; side++) {
			const char* name = side == R ? "R" : "L";
			if (row->contains("rest") && (*row)["rest"].is_object())
				cls.rest[side] = pose((*row)["rest"].value(name, json()));
			if (row->contains("restShield") && (*row)["restShield"].is_object())
				cls.rest_shield[side] = pose((*row)["restShield"].value(name, json()));
			if (row->contains("elbow") && (*row)["elbow"].is_object())
				cls.has_elbow[side] = vec2((*row)["elbow"].value(name, json()), cls.elbow[side]);
		}
		if (row->contains("clips") && (*row)["clips"].is_object())
			for (const auto& clip : (*row)["clips"].items())
				cls.clips[clip.key()] = readClip((*row)["clips"], clip.key(), 0);
		m_classes[item.key()] = cls;
	}
	return m_classes.find("hth") != m_classes.end();
}

bool Hands::loadGL(const uint8_t* pixels, int width, int height)
{
	const GLuint vs = compile(GL_VERTEX_SHADER, VS_SRC), fs = compile(GL_FRAGMENT_SHADER, FS_SRC);
	if (!vs || !fs)
		return false;
	m_program = glCreateProgram();
	glAttachShader(m_program, vs);
	glAttachShader(m_program, fs);
	glLinkProgram(m_program);
	glDeleteShader(vs);
	glDeleteShader(fs);
	GLint ok = GL_FALSE;
	glGetProgramiv(m_program, GL_LINK_STATUS, &ok);
	if (!ok) {
		error_log("Hands: shader link failed.");
		return false;
	}
	m_u_size = glGetUniformLocation(m_program, "u_Size");
	m_u_texture = glGetUniformLocation(m_program, "u_Texture");
	m_u_ramp = glGetUniformLocation(m_program, "u_Ramp");
	m_u_light = glGetUniformLocation(m_program, "u_Light");

	glGenVertexArrays(1, &m_vao);
	glGenBuffers(1, &m_vbo);
	glBindVertexArray(m_vao);
	glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
	glEnableVertexAttribArray(0);
	glEnableVertexAttribArray(1);
	glEnableVertexAttribArray(2);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), (const void*)0);
	glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), (const void*)8);
	glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), (const void*)16);

	// every picture here is sampled pixel by pixel, whatever filter its texture was made with
	glGenSamplers(1, &m_sampler);
	glSamplerParameteri(m_sampler, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glSamplerParameteri(m_sampler, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glSamplerParameteri(m_sampler, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glSamplerParameteri(m_sampler, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

	GLint prev_unit = 0;
	glGetIntegerv(GL_ACTIVE_TEXTURE, &prev_unit);
	glActiveTexture(GL_TEXTURE0 + TEXTURE_UNIT);
	glGenTextures(1, &m_atlas);
	glBindTexture(GL_TEXTURE_2D, m_atlas);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glActiveTexture(prev_unit);
	m_atlas_w = width;
	m_atlas_h = height;
	return true;
}

bool Hands::load()
{
	const std::string rig_text = readText(RIG_JSON), hands_text = readText(HANDS_JSON);
	if (rig_text.empty() || hands_text.empty() || !loadRig(rig_text, hands_text))
		return false;
	int width = 0, height = 0, channels = 0;
	stbi_set_flip_vertically_on_load_thread(0);
	uint8_t* pixels = stbi_load(HANDS_PNG, &width, &height, &channels, 4);
	if (!pixels)
		return false;
	const bool ok = loadGL(pixels, width, height);
	stbi_image_free(pixels);
	if (ok)
		trace_log("Hands: loaded (%u weapon classes, %u poses).", (uint32_t)m_classes.size(), (uint32_t)m_pose_names.size());
	return ok;
}

bool Hands::ensureBuffer(glm::uvec2 size, int factor)
{
	const glm::ivec2 want = { ((int)size.x + factor - 1) / factor, ((int)size.y + factor - 1) / factor };
	if (m_fbo && want == m_buffer)
		return true;
	if (!m_fbo) {
		glGenFramebuffers(1, &m_fbo);
		glGenTextures(1, &m_color);
	}
	GLint prev_unit = 0;
	glGetIntegerv(GL_ACTIVE_TEXTURE, &prev_unit);
	glActiveTexture(GL_TEXTURE0 + TEXTURE_UNIT);
	glBindTexture(GL_TEXTURE_2D, m_color);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, want.x, want.y, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glActiveTexture(prev_unit);
	glBindFramebuffer(GL_FRAMEBUFFER, m_fbo);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_color, 0);
	const GLenum draw = GL_COLOR_ATTACHMENT0;
	glDrawBuffers(1, &draw);
	m_buffer = want;
	return glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
}

// ---- the rig ----

const Hands::ClassRig& Hands::weaponClass(const std::string& name) const
{
	const auto it = m_classes.find(name);
	return it != m_classes.end() ? it->second : m_classes.at("hth");
}

const Hands::Clip* Hands::clip(const ClassRig& cls, const std::string& name) const
{
	const auto it = cls.clips.find(name);
	return it == cls.clips.end() || it->second.empty() ? nullptr : &it->second;
}

// Where one hand is at t: between the keys around it; the rest pose when the clip leaves it out.
Hands::Pose Hands::sample(const Clip* keys, int side, float t, const Pose& rest, bool* dropped) const
{
	*dropped = false;
	const Key* k0 = nullptr;
	const Key* k1 = nullptr;
	if (keys)
		for (const Key& k : *keys) {
			if (!k.side[side].valid)
				continue;
			if (!k0 || k.t <= t)
				k0 = &k;
			else {
				k1 = &k;
				break;
			}
		}
	if (!k0)
		return rest;
	const Pose& a = k0->side[side];
	*dropped = k0->drop == side;
	if (!k1 || k1->t <= k0->t)
		return a;
	const Pose& b = k1->side[side];
	const float u = (t - k0->t) / (k1->t - k0->t);
	Pose p = a;
	p.x = a.x + (b.x - a.x) * u;
	p.y = a.y + (b.y - a.y) * u;
	p.angle = a.angle + (b.angle - a.angle) * u;
	return p;
}

// The hand picture nearest an angle: the fist's own, or (claws) the way the fingers point.
const Hands::HandFrame* Hands::frameFor(int side, int pose, float angle, bool fingers) const
{
	if (pose < 0 || pose >= 8 || m_frames[side][pose].empty())
		pose = m_pose_grip;
	const auto& frames = m_frames[side][pose];
	if (frames.empty())
		return nullptr;
	if (fingers) {
		const HandFrame* best = &frames[0];
		for (const auto& f : frames)
			if (fabsf(wrapDegrees(f.fingers - angle)) < fabsf(wrapDegrees(best->fingers - angle)))
				best = &f;
		return best;
	}
	const float step = 360.0f / (float)m_angles;
	const int n = (int)frames.size();
	const int index = (((int)floorf(angle / step + 0.5f)) % n + n) % n;
	return &frames[index];
}

// The picture of what a hand holds: a unique or set item's own (or the base item's, in its colour).
bool Hands::itemPicture(const Held& held, ItemPic* out)
{
	if (held.code.empty())
		return false;
	SpriteStore& store = SpriteStore::Instance();
	const ItemArt* art = store.itemArt(held.code);
	if (!art)
		return false;
	for (const auto& type : art->types)
		if (type == "bowq" || type == "xboq") // a quiver rides on the back, not in a hand
			return false;
	std::string token = art->inv, mode = "NU";
	const ItemVariant* variant = held.quality == 7 ? store.unique(held.file) : held.quality == 5 ? store.setItem(held.file) : nullptr;
	if (variant && variant->code == held.code) { // a row that does not fit the item is not trusted
		const std::string& alt = held.quality == 7 ? art->uinv : art->sinv;
		token = !variant->inv.empty() ? variant->inv : !alt.empty() ? alt : token;
		if (variant->colour >= 0 && art->inv_trans > 0 && art->inv_trans <= 9 && variant->colour < 100)
			mode = "T" + std::to_string(art->inv_trans) + "C" + std::to_string(variant->colour);
	}
	if (token.empty())
		return false;
	// the inventory is drawn with the same palette in every act
	const Sprite* sprite = nullptr;
	SpriteState got = store.get("item", token, mode.c_str(), 1, &sprite);
	if (got == SpriteState::Failed && mode != "NU")
		got = store.get("item", token, "NU", 1, &sprite);
	if (got != SpriteState::Ready || !sprite || !sprite->held.valid || sprite->atlas_w <= 0 || sprite->atlas_h <= 0)
		return false;

	out->sprite = sprite;
	out->ethereal = held.ethereal;
	out->shield = std::find(art->types.begin(), art->types.end(), "shld") != art->types.end();
	out->grip = &m_grips.at("weap");
	for (const auto& type : art->types) {
		const auto it = m_grips.find(type);
		if (it != m_grips.end()) {
			out->grip = &it->second;
			break;
		}
	}
	return true;
}

// ---- drawing ----

// A part (sx, sy, sw, sh) of a texture with its point (ax, ay) at buffer point (tx, ty), turned
// `degrees` counter-clockwise; y down everywhere.
void Hands::pushPicture(GLuint texture, int kind, float alpha, int tex_w, int tex_h, float sx, float sy, float sw, float sh,
                        float ax, float ay, float tx, float ty, float degrees)
{
	Quad q;
	q.texture = texture;
	q.kind = kind;
	q.alpha = alpha;
	const float th = glm::radians(degrees), c = cosf(th), s = sinf(th);
	const glm::vec2 corners[4] = { { -ax, -ay }, { sw - ax, -ay }, { sw - ax, sh - ay }, { -ax, sh - ay } };
	for (int i = 0; i < 4; i++) {
		const float x = corners[i].x, y = corners[i].y;
		q.pos[i] = { tx + x * c + y * s, ty - x * s + y * c };
		// a hair inside: a pixel centre that lands on a texel edge takes the texel past it, as floor() does
		q.uv[i] = { (sx + ax + x + 0.001f) / (float)tex_w, (sy + ay + y + 0.001f) / (float)tex_h };
	}
	m_quads.push_back(q);
}

void Hands::render(const State& state, glm::uvec2 size, GLuint target)
{
	if (!m_ready) {
		const DWORD tick = GetTickCount();
		if (m_tried && (int32_t)(tick - m_next_try) < 0)
			return;
		m_tried = true;
		m_next_try = tick + 5000; // the art service may not have put the hands in place yet
		m_ready = load();
		if (!m_ready)
			return;
	}
	if (size.x < 16 || size.y < 16)
		return;

	LARGE_INTEGER counter, frequency;
	QueryPerformanceCounter(&counter);
	QueryPerformanceFrequency(&frequency);
	const double now = (double)counter.QuadPart / (double)frequency.QuadPart;
	const float dt = m_last_frame > 0.0 ? glm::clamp((float)(now - m_last_frame), 0.0f, 0.1f) : 0.0f;
	m_last_frame = now;
	m_time += dt;

	// the small picture: about m_lines high, a whole number of screen pixels per pixel
	const int factor = glm::max(1, (int)floorf((float)size.y / (float)m_lines + 0.5f));
	const GLint prev_fbo = (GLint)target;
	if (!ensureBuffer(size, factor)) {
		glBindFramebuffer(GL_FRAMEBUFFER, target);
		return;
	}
	const float W = (float)m_buffer.x, H = (float)m_buffer.y;

	// ---- who holds what ----
	ItemPic pics[2];
	const bool has[2] = { itemPicture(state.right, &pics[R]), itemPicture(state.left, &pics[L]) };
	const auto isWeapon = [&](int slot) { return has[slot] && !pics[slot].shield; };
	const auto isShield = [&](int slot) { return has[slot] && pics[slot].shield; };
	// sod.dll reports the right hand's own class; a weapon in each hand is the game's dual-wield class
	std::string wclass = lower(state.weapon_class);
	if (isWeapon(R) && isWeapon(L)) {
		if (wclass == "ht1")
			wclass = "ht2";
		else if (wclass == "1hs" || wclass == "1ht")
			wclass = "1ss";
	}
	const ClassRig& cls = weaponClass(wclass);
	const ItemPic* hold[2] = { nullptr, nullptr };
	const ItemPic* first_weapon = isWeapon(R) ? &pics[R] : isWeapon(L) ? &pics[L] : nullptr;
	const ItemPic* first_shield = isShield(R) ? &pics[R] : isShield(L) ? &pics[L] : nullptr;
	if (cls.bow)
		hold[L] = first_weapon;
	else if (isDual(wclass)) {
		hold[R] = isWeapon(R) ? &pics[R] : nullptr;
		hold[L] = isWeapon(L) ? &pics[L] : nullptr;
	} else {
		hold[R] = first_weapon;
		if (!cls.two)
			hold[L] = first_shield;
	}
	const bool shield_left = hold[L] && hold[L]->shield;

	// ---- which clip, and where in it ----
	const char* mode = state.mode >= 0 && state.mode < 20 ? PLAYER_MODES[state.mode] : "NU";
	const auto mapped = m_mode_clips.find(mode);
	const std::string name = mapped != m_mode_clips.end() ? mapped->second : "idle";
	const Clip* keys = shield_left ? clip(cls, name + "_shield") : nullptr;
	if (!keys)
		keys = clip(cls, name);
	if (!keys)
		keys = clip(cls, "idle");
	Pose rest[2] = { cls.rest[R], cls.rest[L] };
	if (shield_left)
		for (int side = 0; side < 2; side++)
			if (cls.rest_shield[side].valid)
				rest[side] = cls.rest_shield[side];

	float t;
	if (name == "idle") {
		m_idle_time = fmodf(m_idle_time + dt / IDLE_PERIOD, 1.0f);
		t = m_idle_time;
	} else if (state.mode == 17) // dead: the end of the death clip
		t = 1.0f;
	else
		t = glm::clamp(state.progress, 0.0f, 1.0f); // stepped as the game steps its own animation

	// ---- walking bob and look sway, in whole pixels ----
	const bool walking = state.mode == 2 || state.mode == 3 || state.mode == 6;
	const bool running = state.mode == 3;
	m_bob_amount += ((walking ? (running ? 1.0f : 0.7f) : 0.0f) - m_bob_amount) * glm::min(1.0f, dt * 6.0f);
	if (walking)
		m_bob_phase += dt * (running ? 11.0f : 7.5f);
	const float decay = expf(-dt * 9.0f);
	m_sway.x = glm::clamp(m_sway.x * decay - state.look_yaw * 60.0f, -10.0f, 10.0f);
	m_sway.y = glm::clamp(m_sway.y * decay + state.look_pitch * 60.0f, -8.0f, 8.0f);
	const float b = m_bob_amount, clock = (float)m_time;
	const glm::vec2 offset = {
		floorf(sinf(m_bob_phase) * 3.0f * b + m_sway.x + 0.5f),
		floorf(-fabsf(cosf(m_bob_phase)) * 4.0f * b + sinf(clock * 1.7f) * 1.2f * (1.0f - b) + m_sway.y + 0.5f),
	};

	// ---- place the hands and what they hold ----
	struct Placed {
		bool valid = false;
		float hx = 0, hy = 0;
		const HandFrame* frame = nullptr;
	};
	struct ItemDraw {
		const ItemPic* item;
		float ax, ay, tx, ty, rot;
		bool over;
	};
	Placed placed[2];
	std::vector<ItemDraw> draws[2];
	bool has_first = false;
	ItemDraw first = {};

	for (int side : { (int)R, (int)L }) {
		bool dropped = false;
		const Pose st = sample(keys, side, t, rest[side], &dropped);
		if (!st.valid)
			continue;
		const ItemPic* item = hold[side];
		if (side == L && cls.two && hold[R] && placed[R].valid)
			continue; // put on the weapon below
		const float hx = floorf(W / 2.0f + st.x + offset.x + 0.5f);
		const float hy = floorf(H - st.y - offset.y + 0.5f);
		const bool fingers = item && item->grip->fingers;
		const int pose = st.pose >= 0 ? st.pose : (item ? m_pose_grip : m_pose_fist);
		const HandFrame* fr = frameFor(side, pose, st.angle, fingers);
		if (!fr)
			continue;
		placed[side] = { true, hx, hy, fr };
		if (!item || dropped)
			continue;
		const HeldShape& shape = item->sprite->held;
		if (item->shield) {
			const float rot = fabsf(fr->angle) < 180.0f ? fr->angle : fr->angle - 360.0f;
			draws[side].push_back({ item, shape.center.x, shape.center.y, hx, hy, rot, true });
			continue;
		}
		// where along the item (0 bottom .. 1 top) this hand and, held in two hands, the other go
		const Grip& grip = *item->grip;
		const bool both = cls.two && side == R;
		float f1 = grip.f, f2 = grip.f;
		if (both && grip.has_two) {
			f1 = grip.two[0];
			f2 = grip.two[1];
		}
		if (grip.has_guard) {
			const float g = shape.guard > 0.0f ? shape.guard : grip.guard;
			f1 *= g;
			f2 *= g;
		}
		const float target_angle = fingers ? fr->fingers : fr->angle;
		const glm::vec2 g1 = shape.pointAt(f1);
		const float rot = target_angle - screenAngle(shape.axis.x, shape.axis.y);
		if (both) {
			has_first = true;
			first = { item, g1.x, g1.y, hx, hy, rot, false };
			const glm::vec2 d = shape.pointAt(f2) - g1;
			const float th = glm::radians(rot), c = cosf(th), s = sinf(th);
			const HandFrame* lfr = frameFor(L, m_pose_grip, fr->angle, fingers);
			if (lfr)
				placed[L] = { true, floorf(hx + d.x * c + d.y * s + 0.5f), floorf(hy - d.x * s + d.y * c + 0.5f), lfr };
		} else
			draws[side].push_back({ item, g1.x, g1.y, hx, hy, rot, false });
	}

	// ---- the list of pictures, back to front ----
	m_quads.clear();
	const auto pushItem = [this](const ItemDraw& d) {
		const Sprite* sp = d.item->sprite;
		pushPicture(sp->texture, 0, d.item->ethereal ? 0.5f : 1.0f, sp->atlas_w, sp->atlas_h, 0.0f, 0.0f, (float)sp->atlas_w,
		            (float)sp->atlas_h, d.ax, d.ay, d.tx, d.ty, d.rot);
	};
	uint32_t look_class = state.player_class;
	const std::string look_name = look_class < m_class_looks.size() ? m_class_looks[look_class] : std::string();
	const auto look_it = m_looks.find(look_name);
	const Look& look = look_it != m_looks.end() ? look_it->second : m_looks.begin()->second;

	if (has_first)
		pushItem(first);
	for (int side : { (int)L, (int)R }) {
		if (!placed[side].valid)
			continue;
		const Placed& p = placed[side];
		const HandFrame& fr = *p.frame;
		for (const auto& d : draws[side])
			if (!d.over)
				pushItem(d);
		pushPicture(m_atlas, 1, 1.0f, m_atlas_w, m_atlas_h, (float)fr.x, (float)fr.y, (float)fr.w, (float)fr.h, fr.ax, fr.ay, p.hx, p.hy, 0.0f);
		// the forearm, from the wrist towards the elbow beyond the edge of the screen
		const ArmPiece& arm = m_arms[side][look.sleeve ? 1 : 0];
		if (arm.w > 0 && arm.h > 0) {
			const float wx = p.hx + fr.wrist_x - fr.ax, wy = p.hy + fr.wrist_y - fr.ay;
			const glm::vec2 elbow = cls.has_elbow[side] ? cls.elbow[side] : m_elbow[side];
			const float ex = W / 2.0f + elbow.x + offset.x, ey = H - elbow.y - offset.y;
			const float direction = screenAngle(ex - wx, ey - wy);
			pushPicture(m_atlas, 1, 1.0f, m_atlas_w, m_atlas_h, (float)arm.x, (float)arm.y, (float)arm.w, (float)arm.h, arm.wx, arm.wy, wx, wy,
			            direction - arm.angle);
		}
		for (const auto& d : draws[side])
			if (d.over)
				pushItem(d);
	}
	if (m_quads.empty()) {
		glBindFramebuffer(GL_FRAMEBUFFER, target);
		return;
	}

	// ---- draw: into the small picture, then that onto the screen ----
	static std::vector<Vertex> verts;
	verts.clear();
	const auto pushVerts = [](const glm::vec2* pos, const glm::vec2* uv, float kind, float alpha) {
		const int order[6] = { 0, 1, 2, 0, 2, 3 };
		for (int i : order)
			verts.push_back({ pos[i].x, pos[i].y, uv[i].x, uv[i].y, kind, alpha });
	};
	for (const auto& q : m_quads)
		pushVerts(q.pos, q.uv, (float)q.kind, q.alpha);
	// the finished picture, bottom centre, a whole number of screen pixels per pixel
	const float out_w = W * (float)factor, out_h = H * (float)factor;
	const float left = floorf(((float)size.x - out_w) * 0.5f), top = (float)size.y - out_h;
	const glm::vec2 screen_pos[4] = { { left, top }, { left + out_w, top }, { left + out_w, top + out_h }, { left, top + out_h } };
	const glm::vec2 screen_uv[4] = { { 0.0f, 1.0f }, { 1.0f, 1.0f }, { 1.0f, 0.0f }, { 0.0f, 0.0f } };
	pushVerts(screen_pos, screen_uv, 2.0f, 1.0f);

	GLint prev_unit = 0;
	glGetIntegerv(GL_ACTIVE_TEXTURE, &prev_unit);
	GLint blend_src_rgb = GL_ONE, blend_dst_rgb = GL_ZERO, blend_src_alpha = GL_ONE, blend_dst_alpha = GL_ZERO;
	glGetIntegerv(GL_BLEND_SRC_RGB, &blend_src_rgb);
	glGetIntegerv(GL_BLEND_DST_RGB, &blend_dst_rgb);
	glGetIntegerv(GL_BLEND_SRC_ALPHA, &blend_src_alpha);
	glGetIntegerv(GL_BLEND_DST_ALPHA, &blend_dst_alpha);
	const GLboolean depth_test = glIsEnabled(GL_DEPTH_TEST);
	const GLboolean blend = glIsEnabled(GL_BLEND);

	glDisable(GL_DEPTH_TEST);
	glBindFramebuffer(GL_FRAMEBUFFER, m_fbo);
	glViewport(0, 0, m_buffer.x, m_buffer.y);
	glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
	glClear(GL_COLOR_BUFFER_BIT);

	glUseProgram(m_program);
	glUniform2f(m_u_size, W, H);
	glUniform1i(m_u_texture, TEXTURE_UNIT);
	glUniform1f(m_u_light, state.brightness);
	static glm::vec3 ramp[64];
	for (int m = 0; m < 4; m++)
		for (int level = 0; level < 16; level++) {
			const size_t at = (size_t)look.ramp[m] * 16 + level;
			ramp[m * 16 + level] = at < m_ramps.size() ? m_ramps[at] : glm::vec3(1.0f, 0.0f, 1.0f);
		}
	glUniform3fv(m_u_ramp, 64, &ramp[0][0]);

	glBindVertexArray(m_vao);
	glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
	glBufferData(GL_ARRAY_BUFFER, verts.size() * sizeof(Vertex), verts.data(), GL_STREAM_DRAW);
	glActiveTexture(GL_TEXTURE0 + TEXTURE_UNIT);
	glBindSampler(TEXTURE_UNIT, m_sampler);
	glEnable(GL_BLEND);
	glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA); // leaves premultiplied colour
	GLint run_start = 0;
	for (size_t i = 0; i < m_quads.size(); i++) {
		const bool last = i + 1 == m_quads.size() || m_quads[i + 1].texture != m_quads[i].texture;
		if (!last)
			continue;
		glBindTexture(GL_TEXTURE_2D, m_quads[i].texture);
		glDrawArrays(GL_TRIANGLES, run_start, (GLsizei)((i + 1) * 6 - run_start));
		run_start = (GLint)((i + 1) * 6);
	}

	glBindFramebuffer(GL_FRAMEBUFFER, prev_fbo);
	glViewport(0, 0, size.x, size.y);
	glUniform2f(m_u_size, (float)size.x, (float)size.y);
	glBindTexture(GL_TEXTURE_2D, m_color);
	glBlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
	glDrawArrays(GL_TRIANGLES, run_start, 6);

	glBindSampler(TEXTURE_UNIT, 0);
	glActiveTexture(prev_unit);
	if (!blend)
		glDisable(GL_BLEND);
	glBlendFuncSeparate(blend_src_rgb, blend_dst_rgb, blend_src_alpha, blend_dst_alpha); // the wrapper caches its blend state
	if (depth_test)
		glEnable(GL_DEPTH_TEST);
}

}
