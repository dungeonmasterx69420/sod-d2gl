/*
	Scrolls of Diablo: "Scrolls of", painted on a scroll, beside the main menu's logo.
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

#include "d2/structs.h"

namespace d2gl::modules {

// The menus draw the game's logo from a few animated pictures. Whenever one of them goes out, the
// scroll is drawn right after the logo (before the buttons and the cursor, which stay on top), so
// it shows on every menu screen that shows the logo and nowhere else.
class MenuTitle {
	// game thread
	bool m_logo_this_frame = false;
	bool m_pending = false; // the logo was drawn and the scroll not yet
	bool m_key_dump = false;
	bool m_dump_armed = false;
	bool m_dumping = false;

	struct DumpRecord {
		uint8_t fn;
		bool logo;
		int x, y, mode;
		uintptr_t file;
		uint32_t dirs, cells, cell_no, width, height;
		int xoffs, yoffs;
	};
	std::vector<DumpRecord> m_dump;

	// render thread
	bool m_gl_tried = false;
	uint32_t m_program = 0, m_vao = 0, m_vbo = 0, m_texture = 0;
	int m_u_size = -1, m_u_texture = -1;
	float m_tex_w = 0.0f, m_tex_h = 0.0f;

	MenuTitle() = default;
	bool initGL();
	void push();

public:
	static MenuTitle& Instance()
	{
		static MenuTitle instance;
		return instance;
	}

	// game thread
	void beginFrame();
	void endFrame();
	void beforeDraw();
	void onImage(uint8_t fn, d2::CellContext* cell, int x, int y, int draw_mode);

	// render thread: draws into the game's frame buffer, which is bound
	void render(glm::uvec2 game_size, uint32_t game_fbo, uint32_t attachment_count);
};

}
