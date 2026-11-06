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

#pragma once

#include "motion_prediction.h"
#include <mutex>

namespace d2gl::modules {

// Replaces the isometric world layer with a 3D first-person view built from the game state that
// sod.dll publishes (Local\SoD_State_v1). The automap, HUD, panels and cursor are untouched: the
// game draws them after the world stage, on top of whatever this module rendered.
class FirstPerson {
	// ---- shared between threads ----
	volatile bool m_enabled = true;

	// ---- state mapping (opened lazily, read-only) ----
	HANDLE m_map = nullptr;
	const uint8_t* m_base = nullptr;
	DWORD m_next_open_try = 0;

	// ---- render thread ----
	bool m_gl_ready = false;
	bool m_gl_failed = false;
	GLuint m_program = 0;
	GLint m_u_mvp = -1, m_u_eye = -1, m_u_fog = -1, m_u_flat = -1;
	GLuint m_fbo = 0, m_color = 0, m_depth = 0;
	glm::uvec2 m_fbo_size = { 0, 0 };
	GLuint m_vao_world = 0, m_vbo_world = 0;
	GLuint m_vao_units = 0, m_vbo_units = 0;
	uint32_t m_world_vertex_count = 0;
	struct Batch {
		GLuint texture; // 0 = flat colour
		uint32_t first, count;
	};
	std::vector<Batch> m_batches;
	GLint m_u_textured = -1, m_u_texture = -1, m_u_additive = -1, m_u_sharp = -1;
	DWORD m_art_retry_time = 0;
	float m_arms_yaw = 0.0f, m_arms_pitch = 0.0f;
	bool m_arms_look_valid = false;
	uint32_t m_room_epoch = 0xFFFFFFFF;
	int32_t m_level_no = -2;
	// ---- camera and input (written on the game thread, read on the render thread) ----
	volatile float m_yaw = 0.0f;
	volatile float m_pitch = -0.08f;
	volatile bool m_yaw_valid = false;
	volatile bool m_frame_active = false; // this frame's world layer is replaced
	volatile bool m_look = false;      // mouse look is on: the real mouse turns the camera
	bool m_look_free = false;          // the player asked for the cursor back (F10)
	bool m_raw_registered = false;
	bool m_lmb = false, m_rmb = false;
	DWORD m_lmb_time = 0, m_rmb_time = 0;
	bool m_moving = false;
	bool m_sliding = false;                  // blocked in front: going along the wall
	glm::vec2 m_line_from = { 0.0f, 0.0f }; // the line the character is being walked along
	glm::vec2 m_line_dir = { 1.0f, 0.0f };
	glm::vec2 m_aim_trim = { 0.0f, 0.0f };  // standing correction to where clicks are aimed, subtiles
	bool m_way_clear = false;                // last frame the pointer reached its full distance
	DWORD m_steer_tick = 0;
	bool m_key_f9 = false;
	bool m_f10_pressed = false; // F10 went down since the last frame (window procedure sets, update takes)
	HANDLE m_cmd_map = nullptr;
	uint8_t* m_cmd = nullptr;
	DWORD m_next_cmd_try = 0;

	// ---- crosshair target and player position (written on the render thread) ----
	volatile LONG m_target_valid = 0;
	volatile uint32_t m_target_type = 0, m_target_id = 0, m_target_flags = 0;
	volatile float m_target_x = 0.0f, m_target_y = 0.0f; // where it is: ids are not unique, the place tells units apart

	// ---- hit marker ----
	volatile DWORD m_fire_tick = 0;                  // when a mouse button last sent an attack (game thread writes)
	std::unordered_map<uint32_t, uint32_t> m_unit_life, m_unit_life_next; // living monsters' life, last frame (render thread)
	double m_hit_time = -10.0;
	bool m_hit_kill = false;
	volatile float m_player_x = 0.0f, m_player_y = 0.0f;
	std::vector<uint8_t> m_frame_copy;
	// ---- one-frame draw trace (F8), written to sod-fp-debug.txt next to the game ----
	struct TraceRecord {
		uint8_t stage, fn;
		int16_t x, y;
		uint32_t vertices;
	};
	bool m_key_f8 = false;
	bool m_key_f7 = false, m_key_f5 = false, m_key_f6 = false;

	// ---- field of view (Ctrl+F5 / Ctrl+F6), kept in sod-view.ini next to the game ----
	volatile float m_fov = 70.0f; // vertical, degrees
	volatile bool m_sharp = true; // crisp pixels up close (Ctrl+F4), also kept in sod-view.ini
	bool m_key_f4 = false;
	bool m_view_loaded = false;
	// a line shown at the top of the view for a moment (game thread writes, render thread reads)
	char m_notice[48] = { 0 };
	volatile DWORD m_notice_tick = 0;
	void notice(const char* text);
	void loadView();
	void saveView();
	volatile bool m_real_tiles = true; // draw the rooms around the player from the game's own map tiles
	uint32_t m_tile_coverage = 0;      // render thread: the tile world's coverage the collision world was built against
	volatile bool m_inside_level = false; // the player's level is an indoor one (render thread writes, game thread reads)
	bool m_trace_armed = false, m_tracing = false;
	std::vector<TraceRecord> m_trace;

	std::vector<uint8_t> m_input_copy; // game thread's own copy of the frame, for steering

	// ---- smooth positions captured on the game thread each frame (see MotionPrediction) ----
	std::mutex m_pred_lock;
	bool m_pred_player_valid = false;
	glm::vec2 m_pred_player = { 0.0f, 0.0f };
	std::vector<MotionPrediction::PredictedUnit> m_pred_units;        // guarded by m_pred_lock
	std::vector<MotionPrediction::PredictedUnit> m_pred_units_scratch; // game thread only
	std::unordered_map<uint32_t, glm::vec2> m_pred_lookup;             // render thread only

	// ---- camera position smoothing (render thread): the game moves units 25 times a second ----
	bool m_cam_valid = false;
	uint32_t m_cam_tick = 0;
	double m_cam_sample_time = 0.0, m_cam_last_time = 0.0;
	glm::vec2 m_cam_sample = { 0.0f, 0.0f }, m_cam_velocity = { 0.0f, 0.0f }, m_cam_pos = { 0.0f, 0.0f };

	// ---- camera lead: hides the game's input latency (see render) ----
	volatile float m_move_x = 0.0f, m_move_y = 0.0f; // where the player is steering, game thread writes
	bool m_lead_valid = false, m_lead_steering = false;
	glm::vec2 m_lead_cam = { 0.0f, 0.0f }, m_lead_body = { 0.0f, 0.0f }, m_lead_off = { 0.0f, 0.0f };
	float m_lead_speed = 12.0f;
	double m_lead_time = 0.0, m_lead_press = 0.0, m_lead_moved = 0.0;
	// where the camera is, as last drawn (render thread writes; the game thread sends it as STOP_AT)
	volatile float m_cam_spot_x = 0.0f, m_cam_spot_y = 0.0f;
	volatile DWORD m_cam_spot_tick = 0; // when it was drawn there; 0 = never
	std::vector<uint8_t> m_room_copy;

	FirstPerson() = default;

	bool openMapping();
	bool openCommands();
	void setLook(bool on);
	void writeMove(float x, float y);
	void stopMoving();
	void pushRecord(uint32_t opcode, uint32_t a0, uint32_t a1, float x, float y, uint32_t a2);
	void fire(uint32_t hand);
	float steerReach(float dir_x, float dir_y, float reach);
	void steer(glm::vec2 dir);
	bool readFrame();
	bool readRooms(uint32_t* epoch);
	bool initGL();
	void ensureTarget(glm::uvec2 size);
	void rebuildWorld();

public:
	static FirstPerson& Instance()
	{
		static FirstPerson instance;
		return instance;
	}

	// Game thread. True when the world layer of the current frame should be replaced.
	bool isActive();
	void toggle();
	inline bool isLooking() const { return m_look; }
	inline bool isFrameActive() const { return m_frame_active; }

	// Draw trace: which game draw call happens in which stage. fn 1-13 = draw hooks, 100+ = stage change.
	inline void traceDraw(uint8_t fn, int x, int y, uint32_t vertices = 0)
	{
		if (m_tracing && m_trace.size() < 20000)
			m_trace.push_back({ (uint8_t)App.game.draw_stage, fn, (int16_t)x, (int16_t)y, vertices });
	}
	void traceBegin();
	void traceEnd(uint32_t vertices);

	// Game thread, once per frame: hotkeys, look mode, movement and attacks.
	void update(bool active);
	// Game thread, after the motion prediction has advanced: copy this frame's smooth positions.
	void capturePositions();
	// Window procedure. True = the message is ours and the game must not see it.
	bool onMessage(UINT msg, WPARAM wParam, LPARAM lParam);

	// Render thread, with the game framebuffer bound. Draws the 3D view into colour attachment 0.
	void render(glm::uvec2 game_size, uint32_t game_fbo, uint32_t attachment_count);
};

}
