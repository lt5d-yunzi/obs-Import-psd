// SPDX-License-Identifier: GPL-2.0-or-later
#import <CoreFoundation/CoreFoundation.h>
#import <CoreGraphics/CoreGraphics.h>
#import <CoreText/CoreText.h>

#include "obs/text_render.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace psd {
namespace {

// Fonts tried in order when the PSD's own font cannot render the text. All of
// these ship with macOS and cover Simplified Chinese.
const char *const kFallbackFonts[] = {
	"PingFangSC-Semibold",
	"STHeitiSC-Medium",
	"HiraginoSansGB-W3",
	"Helvetica",
};

CFStringRef cf_string(const std::string &s)
{
	return CFStringCreateWithBytes(kCFAllocatorDefault, reinterpret_cast<const UInt8 *>(s.data()),
				       CFIndex(s.size()), kCFStringEncodingUTF8, false);
}

// UTF-8 -> UTF-16 so CoreText can measure and draw it.
std::vector<UniChar> to_utf16(const std::string &s)
{
	CFStringRef str = cf_string(s);
	if (str == nullptr)
		return {};
	const CFIndex len = CFStringGetLength(str);
	std::vector<UniChar> buf(static_cast<size_t>(len));
	CFStringGetCharacters(str, CFRangeMake(0, len), buf.data());
	CFRelease(str);
	return buf;
}

// True when the font exists (CoreText did not silently substitute Helvetica)
// and covers the first CJK character of `utf16`.
bool font_covers(CTFontRef font, const std::vector<UniChar> &utf16)
{
	if (font == nullptr)
		return false;
	for (UniChar u : utf16) {
		if (u < 0x2E80)
			continue; // non-CJK: any font works
		CGGlyph g = 0;
		if (!CTFontGetGlyphsForCharacters(font, &u, &g, 1) || g == 0)
			return false;
	}
	return true;
}

} // namespace

bool font_is_usable(const std::string &font)
{
	CFStringRef name = cf_string(font);
	if (name == nullptr)
		return false;
	CTFontRef f = CTFontCreateWithName(name, 12.0, nullptr);
	CFRelease(name);
	if (f == nullptr)
		return false;
	CFStringRef actual = CTFontCopyPostScriptName(f);
	bool ok = false;
	if (actual != nullptr) {
		char buf[256] = {};
		if (CFStringGetCString(actual, buf, sizeof buf, kCFStringEncodingUTF8))
			ok = (font == buf); // a substitution means the font is missing
		CFRelease(actual);
	}
	if (ok) {
		const UniChar probe = 0x540D; // 名
		CGGlyph g = 0;
		ok = CTFontGetGlyphsForCharacters(f, &probe, &g, 1) && g != 0;
	}
	CFRelease(f);
	return ok;
}

bool render_text_layer(const TextRenderRequest &req, TextRenderResult &out)
{
	out = TextRenderResult();
	if (req.text.empty())
		return false;

	const std::vector<UniChar> utf16 = to_utf16(req.text);
	if (utf16.empty())
		return false;

	// The PSD's point size and its layer rectangle do not share a scale: the
	// layer box is what the user sees, so when a box is given, choose the
	// largest size that fits inside it instead of trusting the stored points.
	double size = req.font_size > 0 ? req.font_size : 12.0;
	if (req.width > 0 && req.height > 0) {
		const double by_height = double(req.height) * 0.78;
		CTFontRef probe = CTFontCreateWithName(CFSTR("Helvetica"), CGFloat(by_height), nullptr);
		if (probe != nullptr) {
			CFStringRef t = CFStringCreateWithCharacters(kCFAllocatorDefault, utf16.data(),
								     CFIndex(utf16.size()));
			if (t != nullptr) {
				const void *k[] = {kCTFontAttributeName};
				const void *v[] = {probe};
				CFDictionaryRef a = CFDictionaryCreate(kCFAllocatorDefault, k, v, 1,
								       &kCFTypeDictionaryKeyCallBacks,
								       &kCFTypeDictionaryValueCallBacks);
				CFAttributedStringRef at = CFAttributedStringCreate(kCFAllocatorDefault, t, a);
				CTLineRef ln = CTLineCreateWithAttributedString(at);
				const double w = CTLineGetTypographicBounds(ln, nullptr, nullptr, nullptr);
				if (w > double(req.width) && w > 0)
					size = by_height * double(req.width) / w;
				else
					size = by_height;
				CFRelease(ln);
				CFRelease(at);
				CFRelease(a);
				CFRelease(t);
			}
			CFRelease(probe);
		}
	}
	if (size < 1.0)
		size = 1.0;

	// Pick a font: the PSD one when it can render this text, else the first
	// fallback that covers it.
	CTFontRef font = nullptr;
	std::string chosen;
	bool substituted = false;

	CFStringRef want = cf_string(req.font);
	if (!req.font.empty() && want != nullptr) {
		CTFontRef f = CTFontCreateWithName(want, CGFloat(size), nullptr);
		if (f != nullptr) {
			CFStringRef actual = CTFontCopyPostScriptName(f);
			char buf[256] = {};
			const bool same = actual && CFStringGetCString(actual, buf, sizeof buf, kCFStringEncodingUTF8) &&
					  req.font == buf;
			if (actual)
				CFRelease(actual);
			if (same && font_covers(f, utf16)) {
				font = f;
				chosen = req.font;
			} else {
				CFRelease(f);
			}
		}
	}
	if (want != nullptr)
		CFRelease(want);

	if (font == nullptr) {
		substituted = true;
		for (const char *cand : kFallbackFonts) {
			CFStringRef n = cf_string(cand);
			if (n == nullptr)
				continue;
			CTFontRef f = CTFontCreateWithName(n, CGFloat(size), nullptr);
			CFRelease(n);
			if (f == nullptr)
				continue;
			if (font_covers(f, utf16)) {
				font = f;
				chosen = cand;
				break;
			}
			CFRelease(f);
		}
	}
	if (font == nullptr)
		return false;

	// Lay the text out.
	CFStringRef text = CFStringCreateWithCharacters(kCFAllocatorDefault, utf16.data(), CFIndex(utf16.size()));
	if (text == nullptr) {
		CFRelease(font);
		return false;
	}

	const CGFloat r = CGFloat(std::max(0.0, std::min(1.0, req.color[0])));
	const CGFloat g = CGFloat(std::max(0.0, std::min(1.0, req.color[1])));
	const CGFloat b = CGFloat(std::max(0.0, std::min(1.0, req.color[2])));
	const CGFloat a = CGFloat(std::max(0.0, std::min(1.0, req.color[3] <= 0.0 ? 1.0 : req.color[3])));

	CGColorRef color = CGColorCreateGenericRGB(r, g, b, a);
	const void *keys[] = {kCTFontAttributeName, kCTForegroundColorAttributeName};
	const void *values[] = {font, color};
	CFDictionaryRef attrs = CFDictionaryCreate(kCFAllocatorDefault, keys, values, 2,
						   &kCFTypeDictionaryKeyCallBacks,
						   &kCFTypeDictionaryValueCallBacks);
	CFAttributedStringRef attr = CFAttributedStringCreate(kCFAllocatorDefault, text, attrs);
	CTLineRef line = CTLineCreateWithAttributedString(attr);

	CGFloat ascent = 0, descent = 0, leading = 0;
	const double line_width = CTLineGetTypographicBounds(line, &ascent, &descent, &leading);

	// Bitmap size: the PSD layer rectangle when given, otherwise the text box.
	int w = req.width > 0 ? req.width : int(std::ceil(line_width)) + 4;
	int h = req.height > 0 ? req.height : int(std::ceil(ascent + descent + leading)) + 4;
	w = std::max(w, 1);
	h = std::max(h, 1);
	if (size_t(w) * size_t(h) > 64u * 1024u * 1024u) {
		CFRelease(line);
		CFRelease(attr);
		CFRelease(attrs);
		CFRelease(text);
		CFRelease(color);
		CFRelease(font);
		return false; // implausibly large
	}

	void *buffer = std::calloc(size_t(w) * size_t(h), 4);
	if (buffer == nullptr) {
		CFRelease(line);
		CFRelease(attr);
		CFRelease(attrs);
		CFRelease(text);
		CFRelease(color);
		CFRelease(font);
		return false;
	}

	CGContextRef ctx = CGBitmapContextCreate(buffer, size_t(w), size_t(h), 8, size_t(w) * 4,
						 CGColorSpaceCreateWithName(kCGColorSpaceSRGB),
						 uint32_t(kCGImageAlphaPremultipliedLast));
	if (ctx == nullptr) {
		std::free(buffer);
		CFRelease(line);
		CFRelease(attr);
		CFRelease(attrs);
		CFRelease(text);
		CFRelease(color);
		CFRelease(font);
		return false;
	}

	// Baseline placement. PSD text sits on the layer rectangle with a small top
	// inset; centre it vertically inside the requested height.
	double pen_x = 0;
	if (req.width > 0) {
		if (req.justification == 2)
			pen_x = (double(w) - line_width) * 0.5;
		else if (req.justification == 1)
			pen_x = double(w) - line_width;
	}
	const double baseline_y = req.height > 0 ? (double(h) - (double(ascent) + double(descent))) * 0.5 + double(descent)
						 : 2.0 + double(descent);

	CGContextSetTextMatrix(ctx, CGAffineTransformIdentity);
	CGContextSetTextPosition(ctx, CGFloat(pen_x), CGFloat(baseline_y));
	CTLineDraw(line, ctx);

	CGContextRelease(ctx);
	CFRelease(line);
	CFRelease(attr);
	CFRelease(attrs);
	CFRelease(text);
	CFRelease(color);
	CFRelease(font);

	// CoreGraphics produced premultiplied RGBA; convert to straight alpha.
	const uint8_t *src = static_cast<const uint8_t *>(buffer);
	out.rgba.assign(size_t(w) * size_t(h) * 4, 0);
	uint8_t *dst = out.rgba.data();
	bool any = false;
	for (size_t i = 0; i < size_t(w) * size_t(h); ++i) {
		const unsigned sa = src[i * 4 + 3];
		if (sa == 0) {
			continue;
		}
		any = true;
		for (int c = 0; c < 3; ++c) {
			const unsigned p = src[i * 4 + size_t(c)];
			dst[i * 4 + size_t(c)] = uint8_t(std::min(255u, (p * 255u + sa / 2u) / sa));
		}
		dst[i * 4 + 3] = uint8_t(sa);
	}
	std::free(buffer);

	if (!any)
		return false;

	out.width = w;
	out.height = h;
	out.used_font = chosen;
	out.font_substituted = substituted;
	return true;
}

} // namespace psd
