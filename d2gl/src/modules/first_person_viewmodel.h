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

#pragma once

namespace d2gl::modules {

// The player's own arms and weapon, drawn in front of the camera. The model is baked by Scrolls of
// Diablo's tools/art/bake_viewmodel.py (sod-art/viewmodel): one skinned vertex buffer and, for each
// clip, a matrix per bone per sample. Attack, cast and hit clips are scrubbed by the character's
// own animation progress, so the strike lands when the game says it does. Render thread only.
class ViewModel {
public:
	struct State {
		std::string weapon_class; // "hth", "1hs", "bow", ... (the game's weapon class token)
		int mode = 1;             // the player's animation mode (game numbering)
		float progress = 0.0f;    // 0..1 through the current animation
		float look_yaw = 0.0f, look_pitch = 0.0f; // how far the camera turned since the last frame
		float brightness = 1.0f;
	};

private:
	struct Part {
		uint32_t first = 0, count = 0;
	};
	struct WeaponClass {
		std::vector<std::string> nodes;
		bool shield = false;
		std::unordered_map<std::string, int> clips; // "idle", "attack1", ... -> clip number
	};

	bool m_tried = false, m_ready = false;
	DWORD m_next_try = 0;

	GLuint m_program = 0, m_vao = 0, m_vbo = 0, m_ibo = 0;
	GLint m_u_bones = -1, m_u_model = -1, m_u_projection = -1, m_u_light = -1, m_u_glow = -1;

	uint32_t m_bones = 0, m_clips = 0, m_samples = 0;
	std::vector<float> m_matrices; // (clips + 1) * samples-or-1 * bones * 12
	std::unordered_map<std::string, Part> m_parts;
	std::unordered_map<std::string, WeaponClass> m_classes;
	std::unordered_map<std::string, std::string> m_mode_clips; // "A1" -> "attack1"
	std::string m_shield_node = "W_shield";

	// animation state
	std::string m_class;
	int m_clip = -1, m_prev_clip = -1;
	float m_clip_time = 0.0f, m_prev_time = 0.0f, m_fade = 0.0f;
	float m_idle_time = 0.0f, m_shown_progress = 0.0f;
	int m_last_mode = -1;
	double m_last_frame = 0.0, m_time = 0.0;
	float m_bob_amount = 0.0f, m_bob_phase = 0.0f, m_flinch = 0.0f;
	glm::vec2 m_sway = { 0.0f, 0.0f };

	ViewModel() = default;

	bool load();
	void sample(int clip, float time, float* out) const; // bones * 12 floats

public:
	static ViewModel& Instance()
	{
		static ViewModel instance;
		return instance;
	}

	// Draws over whatever is in the bound framebuffer (the depth buffer is cleared first, so the
	// arms never poke into a wall). Leaves the GL program, vertex array and array buffer changed.
	void render(const State& state, glm::uvec2 size);
};

}
