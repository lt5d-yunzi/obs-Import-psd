// SPDX-License-Identifier: GPL-2.0-or-later
//
// Reader for Adobe's "descriptor" binary structure.
//
// Photoshop stores layer styles (lfx2), text engine settings (TySh) and many
// image resources as descriptors: a class-id header followed by an ordered list
// of typed key/value pairs. Values nest (OBJC objects and VLLS lists), so the
// reader is a small recursive descent parser that builds an in-memory tree.
//
// Only what obs-psd needs is modelled: numbers (with unit), strings, booleans,
// enums, class references, objects and lists. Anything unknown becomes
// Value::Unknown and is skipped, so newer files still parse.
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace psd {

struct Descriptor {
	// Unit of a number value.
	enum class Unit {
		None,
		Pixels,    // #Pxl
		Points,    // #Pnt
		Percent,   // #Prc
		Angle,     // #Ang
		Density,   // #Rsl
		Distance,  // #Dst
		Other
	};

	enum class Kind {
		Unknown,
		Bool,
		Integer,
		Number,   // with unit
		Text,     // Unicode string
		Enum,     // type + value
		Class,    // class id + name
		Object,   // class + items
		List,     // array of values
		Raw       // alias / reference / data payloads
	};

	Kind kind = Kind::Unknown;

	bool bool_value = false;
	int64_t int_value = 0;
	double number_value = 0.0;
	Unit unit = Unit::None;

	std::string text;      // Text, Enum type, Class name
	std::string enum_value; // Enum value
	std::string class_id;   // Object/Class id

	std::vector<std::pair<std::string, Descriptor>> items; // Object
	std::vector<Descriptor> values;                        // List

	// Convenience accessors. They never throw; a missing or mistyped item
	// returns the fallback.
	const Descriptor *get(const std::string &key) const;
	double as_number(double fallback = 0.0) const;
	bool as_bool(bool fallback = false) const;
	std::string as_string() const;

	// Resolve a chain of keys: get_path({"DropShadow", "Opacity"}).
	const Descriptor *get_path(std::initializer_list<const char *> path) const;

	bool valid() const { return kind != Kind::Unknown; }
};

// Parses one descriptor starting at `at`. Returns the number of bytes consumed,
// or 0 on failure.
size_t parse_descriptor(const uint8_t *data, size_t size, size_t at, Descriptor &out);

} // namespace psd
