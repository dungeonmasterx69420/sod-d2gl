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

#include "pch.h"
#include "menu_title.h"
#include "d2/funcs.h"
#include <stb/stb_image.h>

namespace d2gl::modules {

namespace {

const char* PICTURE = "sod-art\\menu\\scroll-of.png";
const char* DUMP_FILE = "sod-menu-debug.txt";
const int TEXTURE_UNIT = 30;

// Where the scroll sits on the 800 x 600 menu: left of the logo, as in the launcher.
const float MENU_W = 800.0f, MENU_H = 600.0f;
const float SCROLL_X = 16.0f, SCROLL_Y = 52.0f, SCROLL_W = 212.0f;

const char* VS_SRC = R"(#version 330 core
layout(location = 0) in vec2 a_Pos;
layout(location = 1) in vec2 a_UV;
uniform vec2 u_Size;
out vec2 v_UV;
void main() {
	v_UV = a_UV;
	gl_Position = vec4(a_Pos.x / u_Size.x * 2.0 - 1.0, 1.0 - a_Pos.y / u_Size.y * 2.0, 0.0, 1.0);
})";

const char* FS_SRC = R"(#version 330 core
in vec2 v_UV;
uniform sampler2D u_Texture;
layout(location = 0) out vec4 FragColor;
void main() {
	FragColor = texture(u_Texture, v_UV);
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
		error_log("MenuTitle: shader compile failed: %s", msg);
		glDeleteShader(id);
		return 0;
	}
	return id;
}

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

// The menus draw the logo as four animated halves (fire and black, left and right), each one
// direction of 30 frames, all at (400, 120) on the 800 x 600 menu. The title screen and the main
// menu show it; the screens behind them do not.
bool isLogo(d2::CellContext* cell, int x, int y)
{
	const auto file = d2::getCellFile(cell);
	return file && x == 400 && y == 120 && file->numdirs == 1 && file->numcells >= 10;
}

}

void MenuTitle::beginFrame()
{
	m_pending = false;
	m_logo_this_frame = false;
	m_dumping = m_dump_armed;
	m_dump_armed = false;
	if (m_dumping)
		m_dump.clear();

	// Ctrl+F8 on a menu writes what it drew to sod-menu-debug.txt (the same keys trace a frame in game).
	const bool key = App.game.screen == GameScreen::Menu && isForeground() && keyDown(VK_CONTROL) && keyDown(VK_F8);
	if (key && !m_key_dump)
		m_dump_armed = true;
	m_key_dump = key;
}

void MenuTitle::push()
{
	m_pending = false;
	App.context->flushVertices();
	App.context->getCommandBuffer()->pushCommand(CommandType::MenuTitle);
}

void MenuTitle::beforeDraw()
{
	if (m_pending)
		push();
}

void MenuTitle::onImage(uint8_t fn, d2::CellContext* cell, int x, int y, int draw_mode)
{
	if (App.game.screen != GameScreen::Menu || !cell || !ISGLIDE3X())
		return;

	const bool logo = isLogo(cell, x, y);
	if (m_pending && !logo)
		push();
	if (logo) {
		m_pending = true;
		m_logo_this_frame = true;
	}

	if (m_dumping) {
		DumpRecord r = { fn, logo, x, y, draw_mode, 0, 0, 0, 0, 0, 0, 0, 0 };
		const auto file = d2::getCellFile(cell);
		r.cell_no = d2::getCellNo(cell);
		if (file) {
			r.file = (uintptr_t)file;
			r.dirs = file->numdirs;
			r.cells = file->numcells;
			const uint32_t index = r.cell_no;
			if (index < file->numdirs * file->numcells && index < 255 && file->cells[index]) {
				const auto gfx = file->cells[index];
				r.width = gfx->width;
				r.height = gfx->height;
				r.xoffs = (int)gfx->xoffs;
				r.yoffs = (int)gfx->yoffs;
			}
		}
		m_dump.push_back(r);
	}
}

void MenuTitle::endFrame()
{
	if (m_pending)
		push();

	if (!m_dumping)
		return;
	m_dumping = false;

	FILE* file = nullptr;
	if (fopen_s(&file, DUMP_FILE, "w") != 0 || !file)
		return;
	fprintf(file, "menu frame game=%ux%u logo=%d draws=%u\n", App.game.size.x, App.game.size.y, (int)m_logo_this_frame, (uint32_t)m_dump.size());
	fprintf(file, "fn x y mode file dirs cells cell w h xoffs yoffs logo\n");
	for (const auto& r : m_dump) {
		fprintf(file, "%u %d %d %d %08X %u %u %u %u %u %d %d %d\n", r.fn, r.x, r.y, r.mode, (uint32_t)r.file, r.dirs, r.cells, r.cell_no, r.width, r.height,
			r.xoffs, r.yoffs, (int)r.logo);
	}
	fclose(file);
}

// ---- render thread ----

bool MenuTitle::initGL()
{
	if (m_program)
		return m_texture != 0;
	if (m_gl_tried)
		return false;
	m_gl_tried = true;

	int w = 0, h = 0, channels = 0;
	stbi_set_flip_vertically_on_load(0);
	uint8_t* pixels = stbi_load(PICTURE, &w, &h, &channels, 4);
	if (!pixels) {
		error_log("MenuTitle: %s not found, the menu shows no scroll.", PICTURE);
		return false;
	}

	GLuint vs = compile(GL_VERTEX_SHADER, VS_SRC), fs = compile(GL_FRAGMENT_SHADER, FS_SRC);
	if (!vs || !fs) {
		stbi_image_free(pixels);
		return false;
	}
	m_program = glCreateProgram();
	glAttachShader(m_program, vs);
	glAttachShader(m_program, fs);
	glLinkProgram(m_program);
	glDeleteShader(vs);
	glDeleteShader(fs);
	GLint linked = GL_FALSE;
	glGetProgramiv(m_program, GL_LINK_STATUS, &linked);
	if (!linked) {
		error_log("MenuTitle: shader link failed.");
		stbi_image_free(pixels);
		return false;
	}
	m_u_size = glGetUniformLocation(m_program, "u_Size");
	m_u_texture = glGetUniformLocation(m_program, "u_Texture");

	GLint prev_unit = 0, prev_vao = 0, prev_vbo = 0;
	glGetIntegerv(GL_ACTIVE_TEXTURE, &prev_unit);
	glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prev_vao);
	glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &prev_vbo);

	// The picture ships at twice the size it is shown at; the mip chain shrinks it cleanly.
	glActiveTexture(GL_TEXTURE0 + TEXTURE_UNIT);
	glGenTextures(1, &m_texture);
	glBindTexture(GL_TEXTURE_2D, m_texture);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
	glGenerateMipmap(GL_TEXTURE_2D);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	stbi_image_free(pixels);
	m_tex_w = (float)w;
	m_tex_h = (float)h;

	glGenVertexArrays(1, &m_vao);
	glGenBuffers(1, &m_vbo);
	glBindVertexArray(m_vao);
	glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
	glBufferData(GL_ARRAY_BUFFER, sizeof(float) * 4 * 6, NULL, GL_DYNAMIC_DRAW);
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(float) * 4, (void*)0);
	glEnableVertexAttribArray(1);
	glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(float) * 4, (void*)(sizeof(float) * 2));

	glBindVertexArray(prev_vao);
	glBindBuffer(GL_ARRAY_BUFFER, prev_vbo);
	glActiveTexture(prev_unit);
	return true;
}

void MenuTitle::render(glm::uvec2 game_size, uint32_t game_fbo, uint32_t attachment_count)
{
	if (!initGL() || game_size.x == 0 || game_size.y == 0)
		return;

	GLint prev_program = 0, prev_vao = 0, prev_vbo = 0, prev_unit = 0;
	glGetIntegerv(GL_CURRENT_PROGRAM, &prev_program);
	glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prev_vao);
	glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &prev_vbo);
	glGetIntegerv(GL_ACTIVE_TEXTURE, &prev_unit);
	const GLboolean prev_blend = glIsEnabled(GL_BLEND);
	GLint src_rgb = GL_ONE, dst_rgb = GL_ZERO, src_alpha = GL_ONE, dst_alpha = GL_ZERO;
	if (App.gl_caps.independent_blending) {
		glGetIntegeri_v(GL_BLEND_SRC_RGB, 0, &src_rgb);
		glGetIntegeri_v(GL_BLEND_DST_RGB, 0, &dst_rgb);
		glGetIntegeri_v(GL_BLEND_SRC_ALPHA, 0, &src_alpha);
		glGetIntegeri_v(GL_BLEND_DST_ALPHA, 0, &dst_alpha);
	} else {
		glGetIntegerv(GL_BLEND_SRC_RGB, &src_rgb);
		glGetIntegerv(GL_BLEND_DST_RGB, &dst_rgb);
		glGetIntegerv(GL_BLEND_SRC_ALPHA, &src_alpha);
		glGetIntegerv(GL_BLEND_DST_ALPHA, &dst_alpha);
	}

	// The menu is 800 x 600, centred when the game runs at another size.
	const float x0 = ((float)game_size.x - MENU_W) * 0.5f + SCROLL_X;
	const float y0 = ((float)game_size.y - MENU_H) * 0.5f + SCROLL_Y;
	const float x1 = x0 + SCROLL_W;
	const float y1 = y0 + SCROLL_W * m_tex_h / m_tex_w;
	const float quad[6][4] = {
		{ x0, y0, 0.0f, 0.0f }, { x1, y0, 1.0f, 0.0f }, { x1, y1, 1.0f, 1.0f },
		{ x0, y0, 0.0f, 0.0f }, { x1, y1, 1.0f, 1.0f }, { x0, y1, 0.0f, 1.0f },
	};

	// only the colour layer: the game's map and mask layers stay as they are
	glBindFramebuffer(GL_FRAMEBUFFER, game_fbo);
	const GLenum first = GL_COLOR_ATTACHMENT0;
	glDrawBuffers(1, &first);

	glEnable(GL_BLEND);
	if (App.gl_caps.independent_blending)
		glBlendFuncSeparatei(0, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
	else
		glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

	glUseProgram(m_program);
	glUniform2f(m_u_size, (float)game_size.x, (float)game_size.y);
	glUniform1i(m_u_texture, TEXTURE_UNIT);
	glActiveTexture(GL_TEXTURE0 + TEXTURE_UNIT);
	glBindTexture(GL_TEXTURE_2D, m_texture);
	glBindVertexArray(m_vao);
	glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
	glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(quad), quad);
	glDrawArrays(GL_TRIANGLES, 0, 6);

	// ---- restore ----
	GLenum all[8] = { GL_COLOR_ATTACHMENT0 };
	for (uint32_t i = 0; i < attachment_count && i < 8; i++)
		all[i] = GL_COLOR_ATTACHMENT0 + i;
	glDrawBuffers(glm::max(glm::min(attachment_count, 8u), 1u), all);
	if (App.gl_caps.independent_blending)
		glBlendFuncSeparatei(0, src_rgb, dst_rgb, src_alpha, dst_alpha);
	else
		glBlendFuncSeparate(src_rgb, dst_rgb, src_alpha, dst_alpha);
	if (!prev_blend)
		glDisable(GL_BLEND);
	glActiveTexture(prev_unit);
	glUseProgram(prev_program);
	glBindVertexArray(prev_vao);
	glBindBuffer(GL_ARRAY_BUFFER, prev_vbo);
}

}
