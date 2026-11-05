/*
	Scrolls of Diablo: first-person arms and weapons.
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
#include "first_person_viewmodel.h"
#include <glm/gtc/constants.hpp>
#include <nlohmann/json.hpp>

namespace d2gl::modules {

namespace {

const char* MODEL_BIN = "sod-art\\viewmodel\\viewmodel.bin";
const char* MODEL_JSON = "sod-art\\viewmodel\\viewmodel.json";
const uint32_t MAX_BONES = 64;
const uint32_t VERTEX_SIZE = 36; // position 3f, normal 3f, colour 4ub, bones 4ub, weights 4ub

const float FOV_DEGREES = 62.0f;
const float LENS_SHIFT = 0.13f;  // of half the screen height
const float IDLE_PERIOD = 3.4f; // seconds per idle breathing cycle
const float FADE = 0.1f;        // seconds to blend out of the previous clip
const glm::vec3 PIVOT = { 0.08f, -0.24f, -0.42f }; // sway pivot, roughly between the hands

// the game's player animation modes, by number
const char* PLAYER_MODES[20] = { "DT", "NU", "WL", "RN", "GH", "TN", "TW", "A1", "A2", "BL", "SC", "TH", "KK", "S1", "S2", "S3", "S4", "DD", "SQ", "KB" };

const char* VS_SRC = R"(#version 330 core
layout(location = 0) in vec3 a_Position;
layout(location = 1) in vec3 a_Normal;
layout(location = 2) in vec4 a_Color;
layout(location = 3) in ivec4 a_Bones;
layout(location = 4) in vec4 a_Weights;
uniform mat4 u_Bones[64];
uniform mat4 u_Model;
uniform mat4 u_Projection;
out vec3 v_Normal;
out vec4 v_Color;
out vec3 v_View;
void main() {
	mat4 skin = u_Bones[a_Bones.x] * a_Weights.x + u_Bones[a_Bones.y] * a_Weights.y + u_Bones[a_Bones.z] * a_Weights.z + u_Bones[a_Bones.w] * a_Weights.w;
	vec4 position = u_Model * skin * vec4(a_Position, 1.0);
	v_Normal = mat3(u_Model) * mat3(skin) * a_Normal;
	v_Color = a_Color;
	v_View = position.xyz;
	gl_Position = u_Projection * position;
})";

const char* FS_SRC = R"(#version 330 core
in vec3 v_Normal;
in vec4 v_Color;
in vec3 v_View;
uniform float u_Light; // brightness of the place the player stands in
uniform float u_Glow;  // pulse of the parts that light themselves
layout(location = 0) out vec4 FragColor;
void main() {
	vec3 n = normalize(v_Normal);
	if (!gl_FrontFacing)
		n = -n;
	vec3 base = v_Color.rgb;
	if (v_Color.a < 0.75) { // lights itself (crystal, gem)
		FragColor = vec4(base * (1.1 + u_Glow), 1.0);
		return;
	}
	// a key light from the upper left (where a carried torch would be) and a warm fill from the right
	float key = max(dot(n, normalize(vec3(-0.45, 0.65, 0.6))), 0.0);
	float fill = max(dot(n, normalize(vec3(0.7, 0.1, 0.5))), 0.0);
	vec3 color = base * (0.34 + 0.62 * key) + base * vec3(1.0, 0.72, 0.5) * 0.22 * fill;
	// a little sheen towards the viewer so metal reads as metal
	float rim = pow(max(dot(n, normalize(-v_View)), 0.0), 8.0);
	color += vec3(0.10) * rim * dot(base, vec3(0.333));
	FragColor = vec4(color * u_Light, 1.0);
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
		error_log("ViewModel: shader compile failed: %s", msg);
		glDeleteShader(id);
		return 0;
	}
	return id;
}

std::string lower(std::string text)
{
	for (auto& c : text)
		c = (char)tolower((unsigned char)c);
	return text;
}

}

bool ViewModel::load()
{
	std::ifstream meta_file(MODEL_JSON);
	std::ifstream bin_file(MODEL_BIN, std::ios::binary);
	if (!meta_file.is_open() || !bin_file.is_open())
		return false;

	const auto meta = nlohmann::json::parse(meta_file, nullptr, false);
	if (meta.is_discarded() || !meta.is_object() || !meta.contains("parts") || !meta.contains("classes") || !meta.contains("clipNames"))
		return false;

	std::vector<char> blob((std::istreambuf_iterator<char>(bin_file)), std::istreambuf_iterator<char>());
	if (blob.size() < 28 || memcmp(blob.data(), "SODV", 4) != 0)
		return false;
	uint32_t header[6];
	memcpy(header, blob.data() + 4, sizeof(header));
	const uint32_t version = header[0], vertex_count = header[1], index_count = header[2];
	m_bones = header[3];
	m_clips = header[4];
	m_samples = header[5];
	const size_t vertex_bytes = (size_t)vertex_count * VERTEX_SIZE, index_bytes = (size_t)index_count * 4;
	const size_t matrix_floats = ((size_t)m_clips * m_samples + 1) * m_bones * 12;
	if (version != 1 || !m_bones || m_bones > MAX_BONES || m_samples < 2 || blob.size() != 28 + vertex_bytes + index_bytes + matrix_floats * 4)
		return false;

	m_matrices.resize(matrix_floats);
	memcpy(m_matrices.data(), blob.data() + 28 + vertex_bytes + index_bytes, matrix_floats * 4);

	// ---- tables ----
	std::unordered_map<std::string, int> clip_numbers;
	int number = 0;
	for (const auto& name : meta["clipNames"])
		clip_numbers[name.get<std::string>()] = number++;
	for (const auto& part : meta["parts"])
		m_parts[part.value("name", std::string())] = { part.value("first", 0u), part.value("count", 0u) };
	for (const auto& item : meta["classes"].items()) {
		WeaponClass cls;
		const auto& row = item.value();
		if (row.contains("nodes"))
			for (const auto& node : row["nodes"])
				cls.nodes.push_back(node.get<std::string>());
		cls.shield = row.value("shield", false);
		if (row.contains("clips"))
			for (const auto& clip : row["clips"].items()) {
				const auto it = clip_numbers.find(clip.value().get<std::string>());
				if (it != clip_numbers.end())
					cls.clips[clip.key()] = it->second;
			}
		m_classes[item.key()] = cls;
	}
	if (meta.contains("modeClips"))
		for (const auto& item : meta["modeClips"].items())
			m_mode_clips[item.key()] = item.value().get<std::string>();
	m_shield_node = meta.value("shieldNode", std::string("W_shield"));
	if (m_classes.find("hth") == m_classes.end() || m_parts.find("SOD_Arms") == m_parts.end())
		return false;

	// ---- GL ----
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
		error_log("ViewModel: shader link failed.");
		return false;
	}
	m_u_bones = glGetUniformLocation(m_program, "u_Bones");
	m_u_model = glGetUniformLocation(m_program, "u_Model");
	m_u_projection = glGetUniformLocation(m_program, "u_Projection");
	m_u_light = glGetUniformLocation(m_program, "u_Light");
	m_u_glow = glGetUniformLocation(m_program, "u_Glow");

	glGenVertexArrays(1, &m_vao);
	glGenBuffers(1, &m_vbo);
	glGenBuffers(1, &m_ibo);
	glBindVertexArray(m_vao);
	glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
	glBufferData(GL_ARRAY_BUFFER, vertex_bytes, blob.data() + 28, GL_STATIC_DRAW);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_ibo);
	glBufferData(GL_ELEMENT_ARRAY_BUFFER, index_bytes, blob.data() + 28 + vertex_bytes, GL_STATIC_DRAW);
	for (int i = 0; i < 5; i++)
		glEnableVertexAttribArray(i);
	glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, VERTEX_SIZE, (const void*)0);
	glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, VERTEX_SIZE, (const void*)12);
	glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, VERTEX_SIZE, (const void*)24);
	glVertexAttribIPointer(3, 4, GL_UNSIGNED_BYTE, VERTEX_SIZE, (const void*)28);
	glVertexAttribPointer(4, 4, GL_UNSIGNED_BYTE, GL_TRUE, VERTEX_SIZE, (const void*)32);

	trace_log("ViewModel: loaded (%u vertices, %u bones, %u clips).", vertex_count, m_bones, m_clips);
	return true;
}

// Bone matrices of a clip at a time in 0..1, as rows of 12 floats.
void ViewModel::sample(int clip, float time, float* out) const
{
	const size_t stride = (size_t)m_bones * 12;
	if (clip < 0 || clip >= (int)m_clips) { // the rest pose sits after the last clip
		memcpy(out, m_matrices.data() + (size_t)m_clips * m_samples * stride, stride * sizeof(float));
		return;
	}
	const float position = glm::clamp(time, 0.0f, 0.9999f) * (float)(m_samples - 1);
	const int index = (int)position;
	const float blend = position - (float)index;
	const float* a = m_matrices.data() + ((size_t)clip * m_samples + index) * stride;
	const float* b = a + stride;
	for (size_t i = 0; i < stride; i++)
		out[i] = a[i] + (b[i] - a[i]) * blend;
}

void ViewModel::render(const State& state, glm::uvec2 size)
{
	if (!m_ready) {
		const DWORD tick = GetTickCount();
		if (m_tried && (int32_t)(tick - m_next_try) < 0)
			return;
		m_tried = true;
		m_next_try = tick + 5000; // the bake may not have been run yet
		m_ready = load();
		if (!m_ready)
			return;
	}

	LARGE_INTEGER counter, frequency;
	QueryPerformanceCounter(&counter);
	QueryPerformanceFrequency(&frequency);
	const double now = (double)counter.QuadPart / (double)frequency.QuadPart;
	const float dt = m_last_frame > 0.0 ? glm::clamp((float)(now - m_last_frame), 0.0f, 0.1f) : 0.0f;
	m_last_frame = now;
	m_time += dt;

	// ---- which clip, and where in it ----
	std::string cls = lower(state.weapon_class);
	if (m_classes.find(cls) == m_classes.end())
		cls = "hth";
	const WeaponClass& weapon = m_classes[cls];
	if (cls != m_class) { // a different weapon: snap, no blend between different rigs' poses
		m_class = cls;
		m_clip = m_prev_clip = -1;
		m_fade = 0.0f;
	}

	const char* mode = state.mode >= 0 && state.mode < 20 ? PLAYER_MODES[state.mode] : "NU";
	std::string key = "idle";
	const auto mapped = m_mode_clips.find(mode);
	if (mapped != m_mode_clips.end())
		key = mapped->second;
	auto found = weapon.clips.find(key);
	if (found == weapon.clips.end()) {
		key = "idle";
		found = weapon.clips.find(key);
	}
	const int clip = found != weapon.clips.end() ? found->second : -1;

	if (state.mode != m_last_mode) {
		if (state.mode == 4) // got hit
			m_flinch = 1.0f;
		m_last_mode = state.mode;
		m_shown_progress = state.progress;
	}
	m_flinch = glm::max(0.0f, m_flinch - dt * 3.2f);

	float time;
	if (key == "idle") {
		m_idle_time = fmodf(m_idle_time + dt / IDLE_PERIOD, 1.0f);
		time = m_idle_time;
	} else if (state.mode == 17) // dead: the end of the death clip
		time = 1.0f;
	else {
		// the game reports progress in steps; glide towards it so the swing does not stutter
		if (state.progress < m_shown_progress - 0.2f)
			m_shown_progress = state.progress; // the animation started over
		else
			m_shown_progress += (state.progress - m_shown_progress) * glm::min(1.0f, dt * 30.0f);
		time = m_shown_progress;
	}

	if (clip != m_clip) {
		m_prev_clip = m_clip;
		m_prev_time = m_clip_time;
		m_fade = m_clip >= 0 ? 1.0f : 0.0f;
		m_clip = clip;
	}
	m_clip_time = time;
	m_fade = glm::max(0.0f, m_fade - dt / FADE);

	static float current[MAX_BONES * 12], previous[MAX_BONES * 12];
	sample(m_clip, m_clip_time, current);
	if (m_fade > 0.0f && m_prev_clip >= 0) {
		sample(m_prev_clip, m_prev_time, previous);
		for (uint32_t i = 0; i < m_bones * 12; i++)
			current[i] += (previous[i] - current[i]) * m_fade;
	}
	static glm::mat4 bones[MAX_BONES];
	for (uint32_t b = 0; b < m_bones; b++) {
		const float* m = current + b * 12;
		bones[b] = glm::mat4(m[0], m[4], m[8], 0.0f, m[1], m[5], m[9], 0.0f, m[2], m[6], m[10], 0.0f, m[3], m[7], m[11], 1.0f);
	}

	// ---- procedural layer: walk bob, idle drift, look sway, hit shake ----
	const bool walking = state.mode == 2 || state.mode == 3 || state.mode == 6;
	const float speed = state.mode == 3 ? 1.15f : 0.8f;
	m_bob_amount += ((walking ? speed : 0.0f) - m_bob_amount) * glm::min(1.0f, dt * 6.0f);
	if (walking)
		m_bob_phase += dt * (state.mode == 3 ? 11.0f : 7.5f);
	const float b = m_bob_amount, t = (float)m_time;
	const float bob_x = sinf(m_bob_phase) * 0.02f * b + sinf(t * 1.1f) * 0.004f * (1.0f - b);
	const float bob_y = -fabsf(cosf(m_bob_phase)) * 0.026f * b + sinf(t * 1.7f) * 0.005f * (1.0f - b);
	const float decay = expf(-dt * 9.0f);
	m_sway.x = glm::clamp(m_sway.x * decay - state.look_yaw * 0.25f, -0.07f, 0.07f);
	m_sway.y = glm::clamp(m_sway.y * decay + state.look_pitch * 0.25f, -0.07f, 0.07f);
	const float shake = m_flinch > 0.0f ? sinf(t * 70.0f) * 0.012f * m_flinch : 0.0f;

	glm::mat4 model = glm::translate(glm::mat4(1.0f), PIVOT + glm::vec3(bob_x + m_sway.x * 0.8f + shake, bob_y + m_sway.y * 0.8f, m_flinch * 0.02f));
	model = glm::rotate(model, m_sway.y * 0.9f + m_flinch * 0.06f, glm::vec3(1.0f, 0.0f, 0.0f));
	model = glm::rotate(model, m_sway.x * 0.9f, glm::vec3(0.0f, 1.0f, 0.0f));
	model = glm::rotate(model, bob_x * 1.5f + shake * 0.5f, glm::vec3(0.0f, 0.0f, 1.0f));
	model = glm::translate(model, -PIVOT);

	glm::mat4 projection = glm::perspective(glm::radians(FOV_DEGREES), (float)size.x / (float)size.y, 0.01f, 20.0f);
	projection[2][1] = -LENS_SHIFT; // the game's panel covers the bottom of the screen: lift the hands clear of it

	// ---- draw ----
	glClear(GL_DEPTH_BUFFER_BIT);
	glEnable(GL_DEPTH_TEST);
	glDepthMask(GL_TRUE);
	glDisable(GL_BLEND);
	glUseProgram(m_program);
	glUniformMatrix4fv(m_u_bones, m_bones, GL_FALSE, &bones[0][0][0]);
	glUniformMatrix4fv(m_u_model, 1, GL_FALSE, &model[0][0]);
	glUniformMatrix4fv(m_u_projection, 1, GL_FALSE, &projection[0][0]);
	glUniform1f(m_u_light, state.brightness);
	const bool casting = state.mode == 10;
	glUniform1f(m_u_glow, sinf(t * 3.0f) * 0.25f + (casting ? 1.2f * sinf(glm::min(1.0f, time / 0.7f) * glm::half_pi<float>()) : 0.0f));

	glBindVertexArray(m_vao);
	const auto draw = [this](const std::string& name) {
		const auto it = m_parts.find(name);
		if (it != m_parts.end() && it->second.count)
			glDrawElements(GL_TRIANGLES, it->second.count, GL_UNSIGNED_INT, (const void*)(size_t)(it->second.first * 4));
	};
	draw("SOD_Arms");
	for (const auto& node : weapon.nodes)
		draw(node);
	if (weapon.shield)
		draw(m_shield_node);
}

}
