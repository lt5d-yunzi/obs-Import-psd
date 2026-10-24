// SPDX-License-Identifier: GPL-2.0-or-later
//
// EngineData reader for PSD type layers.
//
// Photoshop stores the text engine description of a type layer as Apple's
// "EngineData" format: a PostScript-like tree of dictionaries (<< >>), arrays
// ([ ]), keys (/Name), strings ((...)) and numbers. This file extracts the few
// values a plugin needs to re-create the text: the string itself, the font of
// the first style run, its size, fill colour and justification.
//
// The format is not self describing beyond that; unknown constructs are skipped
// so a file with newer features still yields the text.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace psd {

struct TextStyle {
	std::string text;     // UTF-8, without the trailing carriage return
	std::string font;     // PostScript family name of the first run
	double font_size = 0; // points, as stored by Photoshop
	double color[4] = {0, 0, 0, 1}; // RGBA, 0..1
	int justification = 0; // 0 = left, 1 = right, 2 = centre
	double tracking = 0;   // 1/1000 em
	bool valid = false;
};

// Parses one EngineData blob. Returns false when no text was found.
bool parse_engine_data(const uint8_t *data, size_t size, TextStyle &out);

// Parses the text descriptor embedded in a `8BIM/TySh` block body
// (the 52-byte header is skipped internally).
bool parse_tysh(const uint8_t *body, size_t size, TextStyle &out);

} // namespace psd
