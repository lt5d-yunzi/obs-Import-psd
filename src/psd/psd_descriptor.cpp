// SPDX-License-Identifier: GPL-2.0-or-later
#include "psd_descriptor.h"

#include <cmath>
#include <cstring>

namespace psd {
namespace {

constexpr size_t kMaxDepth = 24; // descriptors nest a handful of levels at most

uint16_t be16(const uint8_t *p) { return uint16_t((uint16_t(p[0]) << 8) | p[1]); }
uint32_t be32(const uint8_t *p)
{
	return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}
int32_t be32s(const uint8_t *p) { return int32_t(be32(p)); }
uint64_t be64(const uint8_t *p) { return (uint64_t(be32(p)) << 32) | uint64_t(be32(p + 4)); }
double be_f64(const uint8_t *p)
{
	const uint64_t bits = be64(p);
	double v;
	std::memcpy(&v, &bits, sizeof v);
	return v;
}

struct Reader {
	const uint8_t *d;
	size_t n;
	size_t p = 0;
	bool ok = true;

	bool need(size_t k)
	{
		if (!ok || k > n - p) {
			ok = false;
			return false;
		}
		return true;
	}
	uint8_t u8()
	{
		if (!need(1))
			return 0;
		return d[p++];
	}
	uint16_t u16()
	{
		if (!need(2))
			return 0;
		const uint16_t v = be16(d + p);
		p += 2;
		return v;
	}
	uint32_t u32()
	{
		if (!need(4))
			return 0;
		const uint32_t v = be32(d + p);
		p += 4;
		return v;
	}
	int32_t i32()
	{
		if (!need(4))
			return 0;
		const int32_t v = be32s(d + p);
		p += 4;
		return v;
	}
	double f64()
	{
		if (!need(8))
			return 0;
		const double v = be_f64(d + p);
		p += 8;
		return v;
	}
	std::string str(size_t len)
	{
		if (!need(len))
			return {};
		std::string s(reinterpret_cast<const char *>(d + p), len);
		p += len;
		return s;
	}
	// Pascal-ish unicode string: 4-byte length in UTF-16 code units.
	std::string unicode()
	{
		const uint32_t units = u32();
		if (!ok || uint64_t(units) * 2u > n - p)
			return {};
		std::string out;
		out.reserve(units);
		for (uint32_t i = 0; i < units; ++i) {
			const uint32_t cp = u16();
			if (cp < 0x80) {
				out.push_back(char(cp));
			} else if (cp < 0x800) {
				out.push_back(char(0xC0 | (cp >> 6)));
				out.push_back(char(0x80 | (cp & 0x3F)));
			} else {
				out.push_back(char(0xE0 | (cp >> 12)));
				out.push_back(char(0x80 | ((cp >> 6) & 0x3F)));
				out.push_back(char(0x80 | (cp & 0x3F)));
			}
		}
		return out;
	}
	void align4() { p += (4 - (p % 4)) % 4; }
};

Descriptor::Unit unit_from(const char *u)
{
	if (std::memcmp(u, "#Pxl", 4) == 0)
		return Descriptor::Unit::Pixels;
	if (std::memcmp(u, "#Pnt", 4) == 0)
		return Descriptor::Unit::Points;
	if (std::memcmp(u, "#Prc", 4) == 0)
		return Descriptor::Unit::Percent;
	if (std::memcmp(u, "#Ang", 4) == 0)
		return Descriptor::Unit::Angle;
	if (std::memcmp(u, "#Rsl", 4) == 0)
		return Descriptor::Unit::Density;
	if (std::memcmp(u, "#Dst", 4) == 0)
		return Descriptor::Unit::Distance;
	return Descriptor::Unit::Other;
}

bool read_value(Reader &r, Descriptor &out, size_t depth);

// Reads the class-id header of an OBJC value: name + classID + key count.
bool read_class_header(Reader &r, std::string &name, std::string &class_id, uint32_t &count)
{
	name = r.unicode();
	if (!r.ok)
		return false;
	class_id = r.str(4);
	count = r.u32();
	return r.ok;
}

bool read_items(Reader &r, Descriptor &out, uint32_t count, size_t depth)
{
	for (uint32_t i = 0; i < count; ++i) {
		// Keys are length-prefixed (u32 + bytes) in the descriptor layout that
		// layer effects (lfx2) use. Some writers store a plain 4-character key
		// instead, which the length-prefixed form also covers when the first
		// byte is 0 and the length is 0..255 with ASCII text behind it.
		const uint32_t klen = r.u32();
		if (!r.ok || klen > 4096)
			return false;
		std::string key = r.str(klen);
		if (!r.ok)
			return false;
		Descriptor v;
		if (!read_value(r, v, depth + 1))
			return false;
		out.items.emplace_back(key, std::move(v));
	}
	return true;
}

bool read_value(Reader &r, Descriptor &out, size_t depth)
{
	if (depth > kMaxDepth)
		return false;
	const std::string type = r.str(4);
	if (!r.ok)
		return false;

	if (type == "bool") {
		out.kind = Descriptor::Kind::Bool;
		out.bool_value = r.u8() != 0;
	} else if (type == "long") {
		out.kind = Descriptor::Kind::Integer;
		out.int_value = r.i32();
	} else if (type == "comp") {
		out.kind = Descriptor::Kind::Integer;
		out.int_value = int64_t(be64(r.d + r.p));
		r.p += 8;
	} else if (type == "doub") {
		out.kind = Descriptor::Kind::Number;
		out.number_value = r.f64();
		out.unit = Descriptor::Unit::None;
	} else if (type == "UntF") {
		out.kind = Descriptor::Kind::Number;
		const std::string u = r.str(4);
		if (r.ok)
			out.unit = unit_from(u.c_str());
		out.number_value = r.f64();
	} else if (type == "TEXT") {
		out.kind = Descriptor::Kind::Text;
		out.text = r.unicode();
	} else if (type == "enum") {
		out.kind = Descriptor::Kind::Enum;
		out.text = r.str(4);
		out.enum_value = r.str(4);
	} else if (type == "Clss") {
		out.kind = Descriptor::Kind::Class;
		std::string name, class_id;
		uint32_t count = 0;
		if (!read_class_header(r, name, class_id, count))
			return false;
		if (count != 0)
			return false; // a class reference has no items
		out.text = name;
		out.class_id = class_id;
	} else if (type == "Objc") {
		out.kind = Descriptor::Kind::Object;
		std::string name, class_id;
		uint32_t count = 0;
		if (!read_class_header(r, name, class_id, count))
			return false;
		out.text = name;
		out.class_id = class_id;
		if (!read_items(r, out, count, depth))
			return false;
	} else if (type == "VlLs") {
		out.kind = Descriptor::Kind::List;
		const uint32_t count = r.u32();
		if (!r.ok || count > 100000)
			return false;
		out.values.reserve(count);
		for (uint32_t i = 0; i < count; ++i) {
			Descriptor v;
			if (!read_value(r, v, depth + 1))
				return false;
			out.values.push_back(std::move(v));
		}
	} else if (type == "obj ") {
		// Reference: class id then a list of forms; we do not need them.
		out.kind = Descriptor::Kind::Raw;
		const uint32_t count = r.u32();
		if (!r.ok || count > 10000)
			return false;
		for (uint32_t i = 0; i < count; ++i) {
			const std::string form = r.str(4);
			if (!r.ok)
				return false;
			if (form == "prop") {
				r.unicode();
				r.str(4);
				r.str(4);
			} else if (form == "Clss") {
				r.unicode();
				r.str(4);
			} else if (form == "name") {
				r.unicode();
				r.unicode();
				r.str(4);
			} else if (form == "indx") {
				r.u32();
			} else if (form == "rele") {
				r.i32();
			} else if (form == "Idnt") {
				r.i32();
			} else if (form == "offf") {
				r.u32();
			} else {
				return false; // unknown form: cannot stay in sync
			}
		}
	} else if (type == "tdta") {
		out.kind = Descriptor::Kind::Raw;
		const uint32_t len = r.u32();
		if (!r.ok)
			return false;
		out.text = r.str(len);
	} else if (type == "alis") {
		out.kind = Descriptor::Kind::Raw;
		const uint32_t len = r.u32();
		if (!r.ok)
			return false;
		out.text = r.str(len);
	} else if (type == "UnFl") {
		out.kind = Descriptor::Kind::Raw;
		const uint32_t count = r.u32();
		if (!r.ok)
			return false;
		r.str(size_t(count) * 8u);
	} else if (type == "Scl " || type == "SclN") {
		// Photoshop writes a "Scl " (scale) class here followed by a complete
		// unit-float value ("UntF" + unit + double). Consume it as one number.
		const std::string inner = r.str(4);
		out.kind = Descriptor::Kind::Number;
		if (inner == "UntF") {
			const std::string u = r.str(4);
			if (r.ok)
				out.unit = unit_from(u.c_str());
			out.number_value = r.f64();
		} else if (inner == "doub") {
			out.number_value = r.f64();
		} else if (inner == "long") {
			out.kind = Descriptor::Kind::Integer;
			out.int_value = r.i32();
		}
	} else if (type == "SclC") {
		out.kind = Descriptor::Kind::Number;
	} else {
		// Unknown type: we cannot know its size, so stop.
		return false;
	}
	return r.ok;
}

} // namespace

const Descriptor *Descriptor::get(const std::string &key) const
{
	for (const auto &kv : items)
		if (kv.first == key)
			return &kv.second;
	return nullptr;
}

double Descriptor::as_number(double fallback) const
{
	if (kind == Kind::Number)
		return number_value;
	if (kind == Kind::Integer)
		return double(int_value);
	return fallback;
}

bool Descriptor::as_bool(bool fallback) const
{
	if (kind == Kind::Bool)
		return bool_value;
	return fallback;
}

std::string Descriptor::as_string() const
{
	if (kind == Kind::Text)
		return text;
	if (kind == Kind::Enum)
		return enum_value;
	if (kind == Kind::Class || kind == Kind::Object)
		return class_id;
	return {};
}

const Descriptor *Descriptor::get_path(std::initializer_list<const char *> path) const
{
	const Descriptor *cur = this;
	for (const char *key : path) {
		if (!cur)
			return nullptr;
		cur = cur->get(key);
	}
	return cur;
}

static size_t parse_descriptor_body(const uint8_t *data, size_t size, size_t at, Descriptor &out);

size_t parse_descriptor(const uint8_t *data, size_t size, size_t at, Descriptor &out)
{
	out = Descriptor();
	if (data == nullptr || at >= size)
		return 0;
	// lfx2 is wrapped: u32 version (0), u32 descriptor length, u32 reserved (1),
	// u32 name length (0), then classID + item count. The name length field is
	// part of the descriptor header, so parsing starts 12 bytes in and the
	// header's leading zero is consumed here.
	// The wrapper is 14 bytes: u32 version (0), u32 length, u32 reserved (1),
	// u16 padding, then the descriptor (whose first u32 is the name length 0).
	if (size - at >= 18 && be32(data + at) == 0 && be32(data + at + 8) == 1 && be16(data + at + 12) == 0 &&
	    be32(data + at + 14) == 0) {
		Descriptor probe;
		const size_t n = parse_descriptor_body(data, size, at + 14, probe);
		if (n != 0) {
			out = std::move(probe);
			return n + 14;
		}
	}
	return parse_descriptor_body(data, size, at, out);
}

static size_t parse_descriptor_body(const uint8_t *data, size_t size, size_t at, Descriptor &out)
{
	out = Descriptor();
	if (data == nullptr || at >= size)
		return 0;
	Reader r{data, size, at};

	std::string name, class_id;
	uint32_t count = 0;
	if (!read_class_header(r, name, class_id, count))
		return 0;
	out.kind = Descriptor::Kind::Object;
	out.text = name;
	out.class_id = class_id;
	if (!read_items(r, out, count, 0))
		return 0;
	return r.p - at;
}

} // namespace psd
