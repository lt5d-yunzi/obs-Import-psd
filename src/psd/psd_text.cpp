// SPDX-License-Identifier: GPL-2.0-or-later
#include "psd_text.h"

#include <cmath>
#include <cstdio>
#include <cstring>

namespace psd {
namespace {

// The subset of EngineData we care about. Parsing is deliberately shallow:
// the tree is walked token by token and every recognised key updates `style`.
struct Cursor {
	const uint8_t *p;
	const uint8_t *end;

	bool eof() const { return p >= end; }
	uint8_t peek() const { return p < end ? *p : 0; }
	void skip_space()
	{
		while (p < end) {
			const uint8_t c = *p;
			if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
				++p;
			else
				break;
		}
	}
	bool accept(const char *token)
	{
		skip_space();
		const size_t n = std::strlen(token);
		if (size_t(end - p) < n || std::memcmp(p, token, n) != 0)
			return false;
		p += n;
		return true;
	}
};

bool read_number(Cursor &c, double &out)
{
	c.skip_space();
	const uint8_t *start = c.p;
	if (c.p < c.end && (*c.p == '-' || *c.p == '+'))
		++c.p;
	bool any = false;
	while (c.p < c.end && ((*c.p >= '0' && *c.p <= '9') || *c.p == '.')) {
		++c.p;
		any = true;
	}
	if (!any) {
		c.p = start;
		return false;
	}
	// Copy into a small buffer; EngineData numbers are short.
	char buf[64];
	const size_t n = size_t(c.p - start) < sizeof buf - 1 ? size_t(c.p - start) : sizeof buf - 1;
	std::memcpy(buf, start, n);
	buf[n] = '\0';
	out = std::strtod(buf, nullptr);
	return true;
}

// Reads a string body up to the closing ')'. Photoshop writes the raw bytes
// straight after '(' (a UTF-16BE BOM usually follows); there is no length
// prefix inside the parentheses.
bool read_paren_string(Cursor &c, std::string &out)
{
	const uint8_t *start = c.p;
	const uint8_t *p = start;
	while (p < c.end && *p != ')')
		++p;
	if (p >= c.end)
		return false;
	out.assign(reinterpret_cast<const char *>(start), size_t(p - start));
	c.p = p; // caller consumes ')'
	return true;
}

// Converts a UTF-16BE buffer (Photoshop writes text with a BOM) into UTF-8.
std::string utf16be_to_utf8(const std::string &raw)
{
	std::string s = raw;
	size_t start = 0;
	if (s.size() >= 2 && (uint8_t(s[0]) == 0xFE && uint8_t(s[1]) == 0xFF))
		start = 2; // BOM
	std::string out;
	out.reserve(s.size());
	for (size_t i = start; i + 1 < s.size(); i += 2) {
		uint32_t cp = (uint32_t(uint8_t(s[i])) << 8) | uint32_t(uint8_t(s[i + 1]));
		if (cp >= 0xD800 && cp <= 0xDBFF && i + 3 < s.size()) {
			const uint32_t lo = (uint32_t(uint8_t(s[i + 2])) << 8) | uint32_t(uint8_t(s[i + 3]));
			if (lo >= 0xDC00 && lo <= 0xDFFF) {
				cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
				i += 2;
			}
		}
		if (cp == '\r' || cp == '\n')
			continue; // EngineData terminates each paragraph with CR
		if (cp < 0x80) {
			out.push_back(char(cp));
		} else if (cp < 0x800) {
			out.push_back(char(0xC0 | (cp >> 6)));
			out.push_back(char(0x80 | (cp & 0x3F)));
		} else if (cp < 0x10000) {
			out.push_back(char(0xE0 | (cp >> 12)));
			out.push_back(char(0x80 | ((cp >> 6) & 0x3F)));
			out.push_back(char(0x80 | (cp & 0x3F)));
		} else {
			out.push_back(char(0xF0 | (cp >> 18)));
			out.push_back(char(0x80 | ((cp >> 12) & 0x3F)));
			out.push_back(char(0x80 | ((cp >> 6) & 0x3F)));
			out.push_back(char(0x80 | (cp & 0x3F)));
		}
	}
	return out;
}

// Skips one value of any kind (used to walk past keys we do not need).
bool skip_value(Cursor &c, int depth);

bool skip_until_close(Cursor &c, const char *close, int depth)
{
	while (!c.eof()) {
		c.skip_space();
		if (c.accept(close))
			return true;
		if (!skip_value(c, depth))
			return false;
	}
	return false;
}

bool skip_value(Cursor &c, int depth)
{
	if (depth > 64)
		return false;
	c.skip_space();
	if (c.eof())
		return false;
	if (c.accept("<<"))
		return skip_until_close(c, ">>", depth + 1);
	if (c.accept("["))
		return skip_until_close(c, "]", depth + 1);
	if (c.peek() == '(') {
		++c.p;
		std::string tmp;
		if (!read_paren_string(c, tmp))
			return false;
		c.accept(")");
		return true;
	}
	if (c.peek() == '/') {
		++c.p;
		while (!c.eof() && c.peek() != ' ' && c.peek() != '\t' && c.peek() != '\r' && c.peek() != '\n' &&
		       c.peek() != '>' && c.peek() != ']')
			++c.p;
		return true;
	}
	double num = 0;
	if (read_number(c, num))
		return true;
	// Unknown token: consume one byte so the walk always terminates.
	++c.p;
	return true;
}

// Reads a key name after '/'.
std::string read_key(Cursor &c)
{
	c.skip_space();
	if (c.peek() != '/')
		return std::string();
	++c.p;
	const uint8_t *start = c.p;
	while (!c.eof() && c.peek() != ' ' && c.peek() != '\t' && c.peek() != '\r' && c.peek() != '\n' &&
	       c.peek() != '>' && c.peek() != ']')
		++c.p;
	return std::string(reinterpret_cast<const char *>(start), size_t(c.p - start));
}

// Reads the first `<<key value>>` colour dictionary and returns its 4 values.
// The key and dictionary are separated by length bytes that are not whitespace,
// so skip forward to the first "<<" before parsing.
bool read_color_values(Cursor &c, double out[4])
{
	c.skip_space();
	for (int skip = 0; skip < 64 && size_t(c.end - c.p) >= 2; ++skip) {
		if (c.p[0] == '<' && c.p[1] == '<') {
			c.p += 2;
			break;
		}
		++c.p;
	}
	int got = 0;
	const uint8_t *limit = c.end;
	for (int guard = 0; guard < 64 && !c.eof() && c.p < limit; ++guard) {
		c.skip_space();
		if (c.accept(">>"))
			break;
		if (c.accept("/Values")) {
			if (!c.accept("["))
				return false;
			while (got < 4 && !c.eof()) {
				double v = 0;
				if (!read_number(c, v))
					break;
				out[got++] = v;
			}
			// consume the rest of the array
			while (!c.eof() && !c.accept("]")) {
				if (!skip_value(c, 1))
					break;
			}
			continue;
		}
		if (!skip_value(c, 1))
			break;
	}
	return got >= 3;
}

// Reads the first font name from the /FontSet array.
std::string read_first_font(Cursor &c)
{
	if (!c.accept("["))
		return std::string();
	std::string name;
	for (int guard = 0; guard < 32 && !c.eof(); ++guard) {
		c.skip_space();
		if (c.accept("]"))
			break;
		if (c.accept("<<")) {
			int inner = 0;
			while (inner < 32 && !c.eof()) {
				c.skip_space();
				if (c.accept(">>"))
					break;
				const std::string key = read_key(c);
				if (key == "Name") {
					c.skip_space();
					if (c.peek() == '(') {
						++c.p;
						std::string raw;
						if (read_paren_string(c, raw) && name.empty())
							name = utf16be_to_utf8(raw);
						c.accept(")");
					}
					continue;
				}
				if (!skip_value(c, 2))
					break;
				++inner;
			}
			continue;
		}
		if (!skip_value(c, 1))
			break;
	}
	return name;
}

} // namespace

bool parse_engine_data(const uint8_t *data, size_t size, TextStyle &out)
{
	out = TextStyle();
	if (data == nullptr || size == 0)
		return false;

	Cursor c{data, data + size};
	bool have_text = false;
	std::string font;

	// EngineData is a tree, but every key we need lives in a leaf dictionary, so
	// a flat walk over "/Key value" pairs is enough. Structural tokens (<</>>,
	// [/]) are consumed as they appear.
	for (int guard = 0; guard < 200000 && !c.eof(); ++guard) {
		c.skip_space();
		if (c.eof())
			break;
		if (c.accept("<<") || c.accept(">>") || c.accept("]") || c.accept("["))
			continue;
		if (c.peek() != '/') {
			// Unknown filler byte between structural tokens (EngineData pads
			// some containers): step over it and keep walking.
			if (c.peek() < 0x20 || c.peek() > 0x7E) {
				++c.p;
				continue;
			}
			if (!skip_value(c, 0))
				break;
			continue;
		}
		const std::string key = read_key(c);
		if (key == "Text" && !have_text) {
			c.skip_space();
			if (c.peek() == '(') {
				++c.p;
				std::string raw;
				if (read_paren_string(c, raw)) {
					std::string txt = utf16be_to_utf8(raw);
					if (!txt.empty()) {
						out.text = txt;
						have_text = true;
					}
				}
				c.accept(")");
			}
			continue;
		}
		if (key == "FontSize") {
			double v = 0;
			if (read_number(c, v) && out.font_size <= 0)
				out.font_size = v;
			continue;
		}
		if (key == "Justification") {
			double v = 0;
			if (read_number(c, v))
				out.justification = int(v);
			continue;
		}
		if (key == "FillColor") {
			double v[4] = {0, 0, 0, 1};
			if (read_color_values(c, v)) {
				// Photoshop stores text fill colour as [alpha, R, G, B].
				out.color[0] = v[1];
				out.color[1] = v[2];
				out.color[2] = v[3];
				out.color[3] = v[0] <= 0.0 ? 1.0 : v[0];
			}
			continue;
		}
		if (key == "FontSet") {
			if (font.empty())
				font = read_first_font(c);
			continue;
		}
		// Containers are walked through rather than skipped so nested keys such
		// as /Text and /FillColor are still seen; only scalar values are skipped.
		c.skip_space();
		if (c.accept("<<") || c.accept("[")) {
			continue;
		}
		if (!skip_value(c, 0))
			break;
	}

	// Fallback: some writers place /Text inside structures the walk above cannot
	// reach; a direct scan for "/Text (" is unambiguous because the key is
	// always followed by a parenthesised UTF-16 string.
	if (!have_text) {
		const char needle[] = "/Text (";
		for (size_t at = 0; at + sizeof needle - 1 < size; ++at) {
			if (std::memcmp(data + at, needle, sizeof needle - 1) != 0)
				continue;
			Cursor t{data + at + sizeof needle - 1, data + size};
			std::string raw;
			if (!read_paren_string(t, raw))
				continue;
			std::string txt = utf16be_to_utf8(raw);
			if (!txt.empty()) {
				out.text = txt;
				have_text = true;
				break;
			}
		}
	}
	// Same direct-scan approach for the scalar values.
	if (out.font_size == 0) {
		const char needle[] = "/FontSize ";
		for (size_t at = 0; at + sizeof needle - 1 < size; ++at) {
			if (std::memcmp(data + at, needle, sizeof needle - 1) != 0)
				continue;
			Cursor t{data + at + sizeof needle - 1, data + size};
			double v = 0;
			if (read_number(t, v) && v > 0) {
				out.font_size = v;
				break;
			}
		}
	}
	if (font.empty()) {
		const char needle[] = "/FontSet [";
		for (size_t at = 0; at + sizeof needle - 1 < size; ++at) {
			if (std::memcmp(data + at, needle, sizeof needle - 1) != 0)
				continue;
			Cursor t{data + at + sizeof needle - 2, data + size}; // at '[' again
			std::string f = read_first_font(t);
			if (!f.empty()) {
				font = f;
				break;
			}
		}
	}
	{
		bool color_ok = false;
		const char needle[] = "/FillColor";
		for (size_t at = 0; at + sizeof needle - 1 < size && !color_ok; ++at) {
			if (std::memcmp(data + at, needle, sizeof needle - 1) != 0)
				continue;
			Cursor t{data + at + sizeof needle - 1, data + size};
			// The colour dictionary starts a little further on.
			for (int skip = 0; skip < 32 && !t.eof(); ++skip) {
				t.skip_space();
				if (t.accept("<<")) {
					double v[4] = {0, 0, 0, 1};
					Cursor u{t.p, data + size};
					(void)u;
					// read_color_values() expects to start at '<<'.
					Cursor w{data + at + sizeof needle - 1, data + size};
					for (int k2 = 0; k2 < 32 && !w.eof(); ++k2) {
						w.skip_space();
						if (w.accept("<<")) {
							if (read_color_values(w, v)) {
								for (int i = 0; i < 4; ++i)
									out.color[0] = v[1];
									out.color[1] = v[2];
									out.color[2] = v[3];
									out.color[3] = v[0] <= 0.0 ? 1.0 : v[0];
								color_ok = true;
							}
							break;
						}
						if (!skip_value(w, 0))
							break;
					}
					break;
				}
				if (!skip_value(t, 0))
					break;
			}
		}
	}
	if (!have_text)
		return false;
	out.font = font;
	out.valid = true;
	return true;
}

bool parse_tysh(const uint8_t *body, size_t size, TextStyle &out)
{
	out = TextStyle();
	// TySh: uint16 version, 6 doubles (transform), uint16 text version, then a
	// descriptor list holding a length-prefixed "EngineData" blob ("tdta").
	if (body == nullptr || size < 54)
		return false;
	const size_t desc_at = 2 + 48 + 2;
	for (size_t i = desc_at; i + 14 < size; ++i) {
		if (std::memcmp(body + i, "EngineData", 10) != 0)
			continue;
		size_t q = i + 10;
		if (q + 8 > size || std::memcmp(body + q, "tdta", 4) != 0)
			continue;
		const uint32_t n = (uint32_t(body[q + 4]) << 24) | (uint32_t(body[q + 5]) << 16) |
				   (uint32_t(body[q + 6]) << 8) | uint32_t(body[q + 7]);
		q += 8;
		if (uint64_t(n) > uint64_t(size - q))
			continue;
		if (parse_engine_data(body + q, n, out))
			return true;
	}
	return false;
}

} // namespace psd
