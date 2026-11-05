/*
	Scrolls of Diablo: small text for the first-person view (labels over what lies on the ground).
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
#include "first_person_text.h"

namespace d2gl::modules {

bool FpText::ready()
{
	if (m_texture)
		return true;
	if (m_tried)
		return false;
	m_tried = true;

	// Windows draws the characters into a bitmap; their brightness becomes the texture's alpha.
	const int W = 512, H = 128, LINE = 26;
	BITMAPINFO info = {};
	info.bmiHeader.biSize = sizeof(info.bmiHeader);
	info.bmiHeader.biWidth = W;
	info.bmiHeader.biHeight = -H; // top down
	info.bmiHeader.biPlanes = 1;
	info.bmiHeader.biBitCount = 32;
	info.bmiHeader.biCompression = BI_RGB;
	void* bits = nullptr;
	HDC dc = CreateCompatibleDC(nullptr);
	HBITMAP bitmap = dc ? CreateDIBSection(dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0) : nullptr;
	HFONT font = CreateFontW(-(LINE - 6), 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Tahoma");
	if (!dc || !bitmap || !bits || !font) {
		if (font)
			DeleteObject(font);
		if (bitmap)
			DeleteObject(bitmap);
		if (dc)
			DeleteDC(dc);
		return false;
	}
	const HGDIOBJ old_bitmap = SelectObject(dc, bitmap), old_font = SelectObject(dc, font);
	memset(bits, 0, (size_t)W * H * 4);
	SetBkMode(dc, TRANSPARENT);
	SetTextColor(dc, RGB(255, 255, 255));

	int x = 2, y = 2;
	for (int c = 32; c < 127; c++) {
		const wchar_t ch = (wchar_t)c;
		SIZE size = {};
		GetTextExtentPoint32W(dc, &ch, 1, &size);
		if (x + size.cx + 3 > W) {
			x = 2;
			y += LINE + 2;
		}
		if (y + LINE > H)
			break;
		TextOutW(dc, x, y, &ch, 1);
		Glyph& glyph = m_glyphs[c];
		glyph.u0 = (float)(x - 1) / W;
		glyph.u1 = (float)(x + size.cx + 1) / W;
		glyph.v0 = (float)y / H;
		glyph.v1 = (float)(y + LINE) / H;
		glyph.w = (float)(size.cx + 2) / LINE;
		x += size.cx + 4;
	}
	GdiFlush();

	std::vector<uint8_t> pixels((size_t)W * H * 4);
	const uint8_t* source = (const uint8_t*)bits;
	for (size_t i = 0; i < (size_t)W * H; i++) {
		pixels[i * 4 + 0] = pixels[i * 4 + 1] = pixels[i * 4 + 2] = 255;
		pixels[i * 4 + 3] = source[i * 4 + 1]; // green: a grey level
	}
	SelectObject(dc, old_font);
	SelectObject(dc, old_bitmap);
	DeleteObject(font);
	DeleteObject(bitmap);
	DeleteDC(dc);

	GLint prev_texture = 0;
	glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev_texture);
	glGenTextures(1, &m_texture);
	glBindTexture(GL_TEXTURE_2D, m_texture);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glBindTexture(GL_TEXTURE_2D, prev_texture);
	return true;
}

float FpText::width(const std::string& text) const
{
	float w = 0.0f;
	for (const unsigned char c : text)
		w += m_glyphs[c < 127 && c >= 32 ? c : '?'].w;
	return w;
}

void FpText::push(std::vector<Vertex>& out, const std::string& text, float x, float y, float height, float stretch, glm::vec3 color) const
{
	const uint8_t r = (uint8_t)(glm::clamp(color.r, 0.0f, 1.0f) * 255.0f), g = (uint8_t)(glm::clamp(color.g, 0.0f, 1.0f) * 255.0f), b = (uint8_t)(glm::clamp(color.b, 0.0f, 1.0f) * 255.0f);
	for (const unsigned char c : text) {
		const Glyph& glyph = m_glyphs[c < 127 && c >= 32 ? c : '?'];
		const float w = glyph.w * height * stretch;
		const Vertex quad[4] = { { x, y, 0, r, g, b, 255, glyph.u0, glyph.v1 }, { x + w, y, 0, r, g, b, 255, glyph.u1, glyph.v1 }, { x + w, y + height, 0, r, g, b, 255, glyph.u1, glyph.v0 },
			{ x, y + height, 0, r, g, b, 255, glyph.u0, glyph.v0 } };
		static const int order[6] = { 0, 1, 2, 0, 2, 3 };
		for (const int i : order)
			out.push_back(quad[i]);
		x += w;
	}
}

}
