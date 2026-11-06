// SPDX-License-Identifier: GPL-2.0-or-later
//
// File-open helper for the PSD codec.
//
// The codec deals in UTF-8 paths everywhere (Qt hands us UTF-8, PSD files
// reference UTF-8 names, the rest of the plugin speaks UTF-8).  Windows'
// fopen() interprets the path in the active ANSI code page, so a UTF-8 path
// with non-ASCII characters (the normal case for Chinese users) fails with
// "file not found".  Convert to UTF-16 and use _wfopen there.
#pragma once

#include <cstdio>
#include <string>

#ifdef _WIN32
// Keep the Windows headers from defining min()/max() macros (they break
// std::min/std::max in the codec and in every consumer of this header).
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace psd {
namespace detail {

inline FILE *fopen_utf8(const std::string &path, const char *mode)
{
#ifdef _WIN32
	const int need = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
	if (need <= 1)
		return nullptr;
	std::wstring wpath(size_t(need), L'\0');
	if (MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, wpath.data(), need) != need)
		return nullptr;

	wchar_t wmode[8] = {};
	size_t i = 0;
	for (; mode[i] != '\0' && i < 7; ++i)
		wmode[i] = wchar_t(mode[i]);
	wmode[i] = L'\0';
	return _wfopen(wpath.c_str(), wmode);
#else
	return std::fopen(path.c_str(), mode);
#endif
}

inline int remove_utf8(const std::string &path)
{
#ifdef _WIN32
	const int need = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
	if (need <= 1)
		return -1;
	std::wstring wpath(size_t(need), L'\0');
	if (MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, wpath.data(), need) != need)
		return -1;
	return _wremove(wpath.c_str());
#else
	return std::remove(path.c_str());
#endif
}

} // namespace detail
} // namespace psd
