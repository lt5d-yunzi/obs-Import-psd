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
		if (CFStringGetCString(actual, buf, sizeof buf, kCFStringEncodingUTF8)) {
			// A missing font silently resolves to Helvetica. Accept the exact
			// name, and also the "<requested>-Regular" form that some
			// installed fonts report as their PostScript name.
			const std::string resolved(buf);
			ok = (resolved == font) || (resolved.rfind(font + "-", 0) == 0) || (font.rfind(resolved, 0) == 0);

		}
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

std::string font_family(const std::string &font)
{
	if (font.empty())
		return {};
	CFStringRef name = cf_string(font);
	if (name == nullptr)
		return {};
	CTFontRef f = CTFontCreateWithName(name, 12.0, nullptr);
	CFRelease(name);
	if (f == nullptr)
		return {};
	std::string out;
	CFStringRef fam = CTFontCopyFamilyName(f);
	if (fam != nullptr) {
		char buf[256] = {};
		if (CFStringGetCString(fam, buf, sizeof buf, kCFStringEncodingUTF8))
			out = buf;
		CFRelease(fam);
	}
	CFRelease(f);
	return out;
}

std::string font_style(const std::string &font)
{
	if (font.empty())
		return "Regular";
	// Derive from the PostScript name: faces report trailing tokens such as
	// "-Bold", "-Regular", "Bold", "Italic". Fall back to Regular.
	std::string tail = font;
	const size_t dash = tail.rfind('-');
	if (dash != std::string::npos)
		tail = tail.substr(dash + 1);
	if (tail.find("Bold") != std::string::npos)
		return "Bold";
	if (tail.find("Italic") != std::string::npos)
		return "Italic";
	if (tail.find("Light") != std::string::npos)
		return "Light";
	if (tail.find("Medium") != std::string::npos)
		return "Medium";
	return "Regular";
}

bool render_text_layer(const TextRenderRequest &req, TextRenderResult &out)
{
	out = TextRenderResult();
	if (req.text.empty())
		return false;

	const std::vector<UniChar> utf16 = to_utf16(req.text);
	if (utf16.empty())
		return false;

	// Photoshop's point size is an em size; the glyphs of a CJK face only occupy
	// about 70% of that em, so rendering at the raw value overflows the layer
	// rectangle. Measured against the sample artwork (100pt in a 275x82 box,
	// "Google名人名言" -> 280px at size 70), 0.7 reproduces the layer box exactly.
	double size = req.font_size > 0 ? req.font_size * 0.7 : 12.0;
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
			std::string resolved;
			const bool got_name = actual && CFStringGetCString(actual, buf, sizeof buf, kCFStringEncodingUTF8);
			if (got_name)
				resolved = buf;
			const bool same = got_name && (resolved == req.font || resolved.rfind(req.font + "-", 0) == 0 ||
						       req.font.rfind(resolved, 0) == 0);
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
	// Tracking: PSD stores it in 1/1000 em, CoreText expects an absolute point
	// offset between glyphs, so scale it by the rendered size.
	CFNumberRef kern = nullptr;
	if (std::fabs(req.tracking) > 0.0001) {
		// PSD tracking is in 1/1000 em and Photoshop applies it between every
		// pair; CoreText's kern is a point offset per gap. Using the raw value
		// over-tightened the text (215px for a 275px box), so apply a tenth of it.
		const double kern_value = req.tracking / 1000.0 * size * 0.10;
		kern = CFNumberCreate(kCFAllocatorDefault, kCFNumberDoubleType, &kern_value);
	}
	const void *keys[3] = {kCTFontAttributeName, kCTForegroundColorAttributeName, kCTKernAttributeName};
	const void *values[3] = {font, color, kern != nullptr ? (const void *)kern : (const void *)font};
	const size_t attr_count = kern != nullptr ? 3 : 2;
	CFDictionaryRef attrs = CFDictionaryCreate(kCFAllocatorDefault, keys, values, CFIndex(attr_count),
						   &kCFTypeDictionaryKeyCallBacks,
						   &kCFTypeDictionaryValueCallBacks);
	CFAttributedStringRef attr = CFAttributedStringCreate(kCFAllocatorDefault, text, attrs);
	CTLineRef line = CTLineCreateWithAttributedString(attr);

	CGFloat ascent = 0, descent = 0, leading = 0;
	const double line_width = CTLineGetTypographicBounds(line, &ascent, &descent, &leading);

	// Bitmap size: the PSD layer rectangle when given, otherwise the text box.
	// A stroke effect paints outside the glyphs, so the bitmap grows by the
	// stroke width; the caller blits it back at (left - pad, top - pad).
	// The PSD stores the stroke size relative to the layer's point size (100pt in
	// the sample files); scale it by the size we actually render at.
	double stroke_px = 0;
	if (req.stroke && req.stroke_width > 0) {
		const double ref = req.font_size > 0 ? req.font_size : 100.0;
		stroke_px = req.stroke_width * (size / ref);
		if (stroke_px < 1.0)
			stroke_px = 1.0;
	}
	const int pad = stroke_px > 0 ? int(std::ceil(stroke_px)) + 1 : 0;
	int w = (req.width > 0 ? req.width : int(std::ceil(line_width)) + 4) + pad * 2;
	int h = (req.height > 0 ? req.height : int(std::ceil(ascent + descent + leading)) + 4) + pad * 2;
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

	// Baseline placement. The PSD layer rectangle is the text's bounding box, so
	// place the glyphs by their own ascent/descent inside it: horizontally by
	// the justification, vertically so the text's box top sits at the layer top.
	double pen_x = double(pad);
	if (req.width > 0) {
		if (req.justification == 2)
			pen_x = double(pad) + (double(req.width) - line_width) * 0.5;
		else if (req.justification == 1)
			pen_x = double(pad) + double(req.width) - line_width;
	}
	// CoreGraphics places text from the baseline with y growing upwards, so the
	// baseline sits `descent` above the bottom of the line box; centre that box
	// inside the layer rectangle.
	const double baseline_y =
		req.height > 0
			? double(pad) + (double(req.height) - (double(ascent) + double(descent))) * 0.5 +
				  double(descent)
			: double(pad) + 2.0 + double(descent);

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
	if (kern != nullptr)
		CFRelease(kern);
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

	// Stroke (PSD "Stroke" layer effect, outside): dilate the alpha mask by
	// `stroke_width` pixels and composite the fill on top of it. Doing this
	// ourselves beats CoreText's percentage-based stroke because the PSD stores
	// the width in pixels.
	if (stroke_px >= 1.0) {
		const int radius = int(stroke_px + 0.5);
		std::vector<uint8_t> mask(size_t(w) * size_t(h));
		for (size_t i = 0; i < mask.size(); ++i)
			mask[i] = out.rgba[i * 4 + 3];

		// Separable dilation: horizontal pass then vertical pass, which is
		// O(w*h*radius) instead of O(w*h*radius^2).
		std::vector<uint8_t> tmp(mask.size(), 0);
		for (int y = 0; y < h; ++y) {
			const uint8_t *row = &mask[size_t(y) * size_t(w)];
			uint8_t *dst_row = &tmp[size_t(y) * size_t(w)];
			for (int x = 0; x < w; ++x) {
				uint8_t best = 0;
				const int lo = std::max(0, x - radius);
				const int hi = std::min(w - 1, x + radius);
				for (int k = lo; k <= hi; ++k)
					best = std::max(best, row[k]);
				dst_row[x] = best;
			}
		}
		std::vector<uint8_t> dilated(mask.size(), 0);
		for (int x = 0; x < w; ++x) {
			for (int y = 0; y < h; ++y) {
				uint8_t best = 0;
				const int lo = std::max(0, y - radius);
				const int hi = std::min(h - 1, y + radius);
				for (int k = lo; k <= hi; ++k)
					best = std::max(best, tmp[size_t(k) * size_t(w) + size_t(x)]);
				dilated[size_t(y) * size_t(w) + size_t(x)] = best;
			}
		}

		const uint8_t sc[4] = {uint8_t(std::max(0.0, std::min(1.0, req.stroke_color[0])) * 255.0),
				       uint8_t(std::max(0.0, std::min(1.0, req.stroke_color[1])) * 255.0),
				       uint8_t(std::max(0.0, std::min(1.0, req.stroke_color[2])) * 255.0),
				       uint8_t(std::max(0.0, std::min(1.0, req.stroke_color[3] <= 0.0 ? 1.0
										   : req.stroke_color[3])) *
					       255.0)};
		std::vector<uint8_t> merged(out.rgba.size(), 0);
		for (size_t i = 0; i < mask.size(); ++i) {
			const unsigned sa = dilated[i];
			const unsigned fa = out.rgba[i * 4 + 3];
			// Stroke first, then the filled glyph over it (both straight alpha).
			const unsigned aa = sa * sc[3] / 255u;
			unsigned br = out.rgba[i * 4 + 0];
			unsigned bg = out.rgba[i * 4 + 1];
			unsigned bb = out.rgba[i * 4 + 2];
			unsigned ba = fa;
			const unsigned out_a = ba + aa - (ba * aa + 127u) / 255u;
			if (out_a == 0)
				continue;
			auto over = [&](unsigned bc, unsigned ac) {
				return (bc * ba + (ac * aa * 255u + 127u) / 255u * (255u - ba) / 255u + 127u) / out_a;
			};
			merged[i * 4 + 0] = uint8_t(std::min(255u, over(br, sc[0])));
			merged[i * 4 + 1] = uint8_t(std::min(255u, over(bg, sc[1])));
			merged[i * 4 + 2] = uint8_t(std::min(255u, over(bb, sc[2])));
			merged[i * 4 + 3] = uint8_t(out_a);
		}
		out.rgba.swap(merged);
	}

	out.width = w;
	out.height = h;
	out.pad = pad;
	out.used_font = chosen;
	out.font_substituted = substituted;
	return true;
}

} // namespace psd
