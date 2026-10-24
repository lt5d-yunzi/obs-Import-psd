// SPDX-License-Identifier: GPL-2.0-or-later
//
// Renders PSD type layers into RGBA bitmaps with macOS CoreText.
//
// A PSD references whatever font the designer used; that font is frequently
// missing on the user's machine (this repo's test file wants
// "ShanghaiFace-Zen-BoldGB2.0", which no stock Mac has). Rendering the text
// with CoreText and importing the result as an image source gives a faithful,
// platform-independent picture and avoids the missing-glyph boxes an OBS text
// source would show. When the requested font is missing we fall back through a
// list of CJK-capable system fonts and report the substitution.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace psd {

struct TextRenderRequest {
	std::string text;      // UTF-8
	std::string font;      // PostScript name from the PSD (may be missing)
	double font_size = 100; // points
	double color[4] = {0, 0, 0, 1}; // RGBA 0..1 straight (non-premultiplied)
	int justification = 0; // 0 left, 1 right, 2 centre
	double tracking = 0;   // letter spacing in 1/1000 em (PSD Tracking)
	int width = 0;         // target bitmap width in px (0 = fit the text)
	int height = 0;        // target bitmap height in px (0 = fit the text)
	// Optional stroke (PSD layer effect "Stroke"): outside the glyph outline.
	bool stroke = false;
	double stroke_width = 0;            // px
	double stroke_color[4] = {1, 1, 1, 1}; // RGBA 0..1
};

struct TextRenderResult {
	std::vector<uint8_t> rgba; // width*height*4, straight alpha
	int width = 0;
	int height = 0;
	int pad = 0;           // bitmap extends `pad` px past the layer rect on each side
	std::string used_font;     // PostScript name actually used
	bool font_substituted = false;
};

// Renders `req` into a bitmap. Returns false when no glyph could be produced.
bool render_text_layer(const TextRenderRequest &req, TextRenderResult &out);

// True when `font` exists on this machine and covers the CJK block.
bool font_is_usable(const std::string &font);

// Family name for a PostScript name ("PingFangSC-Semibold" -> "PingFang SC").
// OBS' text sources select the typeface by family name. Empty when unknown.
std::string font_family(const std::string &font);

} // namespace psd
