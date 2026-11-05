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

#pragma once

namespace d2gl::modules {

// The printable ASCII characters of one system font, drawn once into a texture. The game's own
// text is placed for the view from above and cannot be put over a spot in the 3D view.
class FpText {
	struct Glyph {
		float u0 = 0, v0 = 0, u1 = 0, v1 = 0;
		float w = 0; // advance, as a fraction of the line height
	};
	GLuint m_texture = 0;
	bool m_tried = false;
	Glyph m_glyphs[128];

public:
	struct Vertex {
		float x, y, z;
		uint8_t r, g, b, a;
		float u, v;
	};

	static FpText& Instance()
	{
		static FpText instance;
		return instance;
	}

	// Makes the texture on first use (render thread). False when the font could not be drawn.
	bool ready();
	GLuint texture() const { return m_texture; }

	// Width of a line of text whose height is 1.
	float width(const std::string& text) const;
	// Adds the text's quads: `x`, `y` is the lower left corner, `height` the line height, `stretch`
	// the width of a unit of height (all in the caller's coordinates; clip space is not square).
	void push(std::vector<Vertex>& out, const std::string& text, float x, float y, float height, float stretch, glm::vec3 color) const;
};

}
