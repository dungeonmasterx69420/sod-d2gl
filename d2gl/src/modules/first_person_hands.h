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

#pragma once

#include "first_person_sprites.h"

namespace d2gl::modules {

// The hands are drawn into a small picture of about 200 lines and scaled up by a whole number with
// no smoothing, so they keep the game's own pixel size. Every item is its own inventory picture from
// the game (tinted the way the game tints a unique or set item), turned and put into a fist; the
// fists and forearms are pre-drawn pixel art (sod-art\hands, from models/hands in the source tree)
// coloured by the character's class. Each weapon class has clips of key poses (rig.json) that the
// game's animation progress steps through.
//
// tools/art/preview_hands.py draws the same thing outside the game; keep the two in step.
class Hands {
public:
	struct Held {
		std::string code;      // items.txt code; empty for an empty hand
		uint32_t file = 0;     // unique or set row
		uint8_t quality = 0;   // 5 set, 7 unique
		bool ethereal = false;
	};
	struct State {
		std::string weapon_class; // "hth", "1hs", ...
		uint32_t player_class = 0;
		int mode = 1;             // the game's player mode number
		float progress = 0.0f;    // 0..1 through the current animation
		float look_yaw = 0.0f;    // radians turned since the last frame
		float look_pitch = 0.0f;
		float brightness = 1.0f;
		int act = 1;
		Held right, left;
	};

	static Hands& Instance()
	{
		static Hands instance;
		return instance;
	}

	// Draws over what is in `target` (a framebuffer of `size`); leaves `target` bound and the viewport
	// covering it.
	void render(const State& state, glm::uvec2 size, GLuint target);

private:
	enum { R = 0, L = 1 };

	struct Pose {
		bool valid = false;
		float x = 0, y = 0, angle = 0; // buffer pixels from the bottom centre, degrees counter-clockwise from up
		int pose = -1;                 // hand picture, -1 = the default for what it holds
	};
	struct Key {
		float t = 0;
		Pose side[2];
		int drop = -1; // the hand that has let go of its item (a throw)
	};
	using Clip = std::vector<Key>;
	struct ClassRig {
		bool two = false, bow = false; // held in both hands; a bow (held in the left hand)
		Pose rest[2], rest_shield[2];
		bool has_elbow[2] = { false, false };
		glm::vec2 elbow[2];
		std::unordered_map<std::string, Clip> clips;
	};
	struct HandFrame {
		int x = 0, y = 0, w = 0, h = 0;
		float ax = 0, ay = 0;        // the anchor: where the item's grip point goes
		float wrist_x = 0, wrist_y = 0;
		float angle = 0, fingers = 0;
	};
	struct ArmPiece {
		int x = 0, y = 0, w = 0, h = 0;
		float wx = 0, wy = 0, angle = 180;
	};
	struct Grip {
		float f = 0.15f;
		bool has_two = false;
		float two[2] = { 0, 0 };
		bool has_guard = false;
		float guard = 0;
		bool fingers = false; // turned the way the fingers point (claws), not the fist
		bool center = false;  // held at its middle (shields)
	};
	struct Look {
		int ramp[4] = { 0, 0, 0, 0 }; // ramp numbers by material
		bool sleeve = false;
	};
	struct ItemPic {
		const Sprite* sprite = nullptr;
		bool shield = false, ethereal = false;
		const Grip* grip = nullptr;
	};
	struct Quad {
		GLuint texture;
		int kind;          // 0 item, 1 hand art
		float alpha;
		glm::vec2 pos[4];  // buffer pixels, y down
		glm::vec2 uv[4];
	};

	bool m_ready = false, m_tried = false;
	DWORD m_next_try = 0;

	// rig
	int m_lines = 200;
	glm::vec2 m_elbow[2];
	std::unordered_map<std::string, ClassRig> m_classes;
	std::unordered_map<std::string, std::string> m_mode_clips;
	std::unordered_map<std::string, Grip> m_grips;
	std::vector<std::string> m_class_looks;
	std::unordered_map<std::string, Look> m_looks;
	std::vector<glm::vec3> m_ramps; // 16 colours per ramp
	std::vector<std::string> m_pose_names;
	int m_pose_grip = 0, m_pose_fist = 0;
	std::vector<HandFrame> m_frames[2][8]; // by side and pose number
	ArmPiece m_arms[2][2];                 // by side, then bare (0) or sleeve (1)
	int m_angles = 32;

	// GL
	GLuint m_program = 0, m_vao = 0, m_vbo = 0, m_sampler = 0;
	GLuint m_atlas = 0;
	int m_atlas_w = 0, m_atlas_h = 0;
	GLuint m_fbo = 0, m_color = 0;
	glm::ivec2 m_buffer = { 0, 0 };
	GLint m_u_size = -1, m_u_texture = -1, m_u_ramp = -1, m_u_light = -1;

	// animation
	double m_last_frame = 0.0, m_time = 0.0;
	float m_idle_time = 0.0f;
	float m_bob_phase = 0.0f, m_bob_amount = 0.0f;
	glm::vec2 m_sway = { 0.0f, 0.0f };

	std::vector<Quad> m_quads;

	Hands() = default;
	bool load();
	bool loadRig(const std::string& rig_text, const std::string& hands_text);
	bool loadGL(const uint8_t* pixels, int width, int height);
	bool ensureBuffer(glm::uvec2 size, int factor);

	const ClassRig& weaponClass(const std::string& name) const;
	const Clip* clip(const ClassRig& cls, const std::string& name) const;
	Pose sample(const Clip* keys, int side, float t, const Pose& rest, bool* dropped) const;
	const HandFrame* frameFor(int side, int pose, float angle, bool fingers) const;
	bool itemPicture(const Held& held, ItemPic* out);

	void pushPicture(GLuint texture, int kind, float alpha, int tex_w, int tex_h, float sx, float sy, float sw, float sh,
	                 float ax, float ay, float tx, float ty, float degrees);
};

}
