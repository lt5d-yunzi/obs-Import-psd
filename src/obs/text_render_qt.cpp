// SPDX-License-Identifier: GPL-2.0-or-later
//
// Renders PSD type layers into RGBA bitmaps with Qt (QPainter / QFontDatabase).
//
// This is the Windows counterpart of text_render_macos.mm: same interface, same
// geometry and stroke model, but it rasterises through Qt because the plugin
// already links Qt for its dialogs and Qt is guaranteed to be loaded inside the
// OBS process.  Vitally, it does not require a separate DirectWrite or GDI+
// setup, and Qt's font fallback handles CJK text when the PSD font is missing.
#include "obs/text_render.h"

#include <QFont>
#include <QFontDatabase>
#include <QFontInfo>
#include <QFontMetricsF>
#include <QGuiApplication>
#include <QImage>
#include <QPainter>
#include <QString>
#include <QStringList>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace psd {
namespace {

// Tried in order when the PSD's own font cannot render the text.  All of these
// ship with Windows 10/11 and cover Simplified Chinese.
const char *const kFallbackFonts[] = {
	"Microsoft YaHei UI",
	"Microsoft YaHei",
	"SimHei",
	"SimSun",
	"Segoe UI",
	"Arial",
};

std::string lower_ascii(std::string s)
{
	for (char &c : s) {
		if (c >= 'A' && c <= 'Z')
			c = char(c - 'A' + 'a');
	}
	return s;
}

// Font names survive enough mangling between Photoshop (PostScript name),
// DirectWrite (family) and Qt (family) that exact comparison is useless:
// "PingFangSC-Semibold" vs "PingFang SC".  Normalise to letters+digits+case.
std::string norm(const std::string &s)
{
	std::string out;
	out.reserve(s.size());
	for (unsigned char c : s) {
		if (c >= 'A' && c <= 'Z')
			out.push_back(char(c - 'A' + 'a'));
		else if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))
			out.push_back(char(c));
	}
	return out;
}

// Split "Family-Style" into a family/weight pair when the family alone does not
// resolve.  Returns false when the name has no recognisable style suffix.
bool split_style(const std::string &name, std::string &family, QFont::Weight &weight, bool &italic)
{
	const size_t dash = name.rfind('-');
	if (dash == std::string::npos || dash == 0 || dash + 1 >= name.size())
		return false;
	const std::string tail = lower_ascii(name.substr(dash + 1));
	if (tail.find("bold") != std::string::npos) {
		weight = tail.find("semi") != std::string::npos ? QFont::DemiBold : QFont::Bold;
	} else if (tail.find("black") != std::string::npos || tail.find("heavy") != std::string::npos) {
		weight = QFont::Black;
	} else if (tail.find("light") != std::string::npos) {
		weight = QFont::Light;
	} else if (tail.find("medium") != std::string::npos) {
		weight = QFont::Medium;
	} else if (tail.find("regular") != std::string::npos || tail.find("normal") != std::string::npos) {
		weight = QFont::Normal;
	} else {
		return false;
	}
	italic = tail.find("italic") != std::string::npos || tail.find("oblique") != std::string::npos;
	family = name.substr(0, dash);
	return true;
}

// Cached family list from Qt, normalised name -> real family name.
const std::vector<std::pair<std::string, std::string>> &family_cache()
{
	static std::vector<std::pair<std::string, std::string>> cache;
	if (cache.empty()) {
		const QStringList families = QFontDatabase::families();
		cache.reserve(size_t(families.size()));
		for (const QString &f : families) {
			// Localised family names (e.g. "仿宋") normalise to an empty
			// string, which would otherwise match every request.
			const std::string key = norm(f.toStdString());
			if (key.size() >= 3)
				cache.emplace_back(key, f.toStdString());
		}
	}
	return cache;
}

// Resolves a PSD font name to an installed family.  Returns an empty string when
// nothing matches (the caller then falls back).
std::string resolve_family(const std::string &name)
{
	if (name.empty() || QGuiApplication::instance() == nullptr)
		return std::string();

	const std::string want = norm(name);
	if (want.empty())
		return std::string();
	for (const auto &kv : family_cache()) {
		if (kv.first == want)
			return kv.second;
	}

	// Try "Family-Style": match the family part, remember the style via the
	// font object built by the caller.
	std::string fam;
	QFont::Weight weight = QFont::Normal;
	bool italic = false;
	if (split_style(name, fam, weight, italic)) {
		const std::string fam_norm = norm(fam);
		for (const auto &kv : family_cache()) {
			if (kv.first == fam_norm)
				return kv.second;
		}
	}

	// Last attempt: does any family contain the requested name (and vice
	// versa)?  Handles vendor prefixes/suffixes such as "MT" or "GB2.0".
	if (want.size() >= 5) {
		for (const auto &kv : family_cache()) {
			if (kv.first.find(want) != std::string::npos || want.find(kv.first) != std::string::npos)
				return kv.second;
		}
	}
	return std::string();
}

QFont make_font(const std::string &family, double pixel_size, const std::string &psd_name)
{
	QFont font(QString::fromStdString(family));
	font.setPixelSize(std::max(1, int(std::lround(pixel_size))));
	font.setStyleStrategy(QFont::PreferAntialias);

	std::string fam;
	QFont::Weight weight = QFont::Normal;
	bool italic = false;
	if (split_style(psd_name, fam, weight, italic)) {
		font.setWeight(weight);
		font.setItalic(italic);
	}
	return font;
}

bool has_cjk(const QString &s)
{
	for (QChar c : s) {
		if (c.unicode() >= 0x2E80)
			return true;
	}
	return false;
}

// True when `font` really renders every CJK character in `text` (Qt silently
// substitutes a fallback face for missing glyphs, which is exactly what we are
// trying to detect).
bool font_covers(const QFont &font, const QString &text)
{
	QFontMetricsF fm(font);
	for (QChar c : text) {
		if (c.unicode() < 0x2E80)
			continue;
		if (!fm.inFont(c))
			return false;
	}
	return true;
}

std::string pick_fallback(const QString &text)
{
	for (const char *cand : kFallbackFonts) {
		const QString fam = QString::fromLatin1(cand);
		if (!QFontDatabase::families().contains(fam))
			continue;
		QFont f(fam);
		f.setPixelSize(12);
		if (font_covers(f, text))
			return cand;
	}
	// Nothing covers CJK: use the platform's default sans face.
	return std::string();
}

} // namespace

bool font_is_usable(const std::string &font)
{
	if (QGuiApplication::instance() == nullptr)
		return false;
	const std::string family = resolve_family(font);
	if (family.empty())
		return false;
	QFont f(QString::fromStdString(family));
	f.setPixelSize(12);
	// The sample documents are Chinese; a CJK-capable face is what the import
	// path needs.  Latin-only faces fall through to the fallback list.
	QFontMetricsF fm(f);
	return fm.inFont(QChar(0x540D)); // 名
}

std::string font_family(const std::string &font)
{
	return resolve_family(font);
}

std::string font_style(const std::string &font)
{
	if (font.empty())
		return "Regular";
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
	if (QGuiApplication::instance() == nullptr)
		return false;

	QString text = QString::fromUtf8(req.text.c_str(), int(req.text.size()));
	text.replace(QChar('\r'), QChar(' '));
	text.replace(QChar('\n'), QChar(' '));
	if (text.isEmpty())
		return false;

	// PSD's point size is an em size; CJK glyphs occupy about 70% of it, so
	// rendering at the raw value overflows the layer rectangle.  Same factor as
	// the CoreText path (measured against the sample artwork).
	const double size = std::max(1.0, (req.font_size > 0 ? req.font_size : 12.0) * 0.7);

	std::string family;
	bool substituted = false;
	if (!req.font.empty()) {
		family = resolve_family(req.font);
		if (!family.empty()) {
			QFont f = make_font(family, size, req.font);
			if (!font_covers(f, text))
				family.clear();
		}
	}
	if (family.empty()) {
		substituted = true;
		family = pick_fallback(text);
		if (family.empty())
			family = QFontDatabase::systemFont(QFontDatabase::GeneralFont).family().toStdString();
	}

	QFont font = make_font(family, size, req.font);
	QFontMetricsF fm(font);

	// Tracking: PSD stores 1/1000 em; Qt takes a pixel offset.  The CoreText
	// path applies a tenth of the raw value (raw over-tightens the sample), so
	// keep the same empirical factor for identical output.
	const double tracking_px = std::fabs(req.tracking) > 0.0001 ? req.tracking / 1000.0 * size * 0.10 : 0.0;
	if (tracking_px != 0.0)
		font.setLetterSpacing(QFont::AbsoluteSpacing, tracking_px);

	QFontMetricsF metrics(font);
	const double line_width = metrics.horizontalAdvance(text);
	const double ascent = metrics.ascent();
	const double descent = metrics.descent();

	// Stroke paints outside the glyphs, so the bitmap grows by the stroke width
	// and the caller blits it back at (left - pad, top - pad).
	double stroke_px = 0;
	if (req.stroke && req.stroke_width > 0) {
		const double ref = req.font_size > 0 ? req.font_size : 100.0;
		stroke_px = req.stroke_width * (size / ref);
		if (stroke_px < 1.0)
			stroke_px = 1.0;
	}
	const int pad = stroke_px > 0 ? int(std::ceil(stroke_px)) + 1 : 0;
	int w = (req.width > 0 ? req.width : int(std::ceil(line_width)) + 4) + pad * 2;
	int h = (req.height > 0 ? req.height : int(std::ceil(ascent + descent)) + 4) + pad * 2;
	w = std::max(w, 1);
	h = std::max(h, 1);
	if (size_t(w) * size_t(h) > 64u * 1024u * 1024u)
		return false;

	QImage image(w, h, QImage::Format_ARGB32_Premultiplied);
	image.fill(Qt::transparent);

	{
		QPainter painter(&image);
		painter.setRenderHint(QPainter::Antialiasing, true);
		painter.setRenderHint(QPainter::TextAntialiasing, true);
		painter.setFont(font);

		QColor color = QColor::fromRgbF(float(std::clamp(req.color[0], 0.0, 1.0)),
						float(std::clamp(req.color[1], 0.0, 1.0)),
						float(std::clamp(req.color[2], 0.0, 1.0)),
						float(req.color[3] > 0.0 ? std::clamp(req.color[3], 0.0, 1.0) : 1.0));
		painter.setPen(color);

		double pen_x = double(pad);
		if (req.width > 0) {
			if (req.justification == 2)
				pen_x = double(pad) + (double(req.width) - line_width) * 0.5;
			else if (req.justification == 1)
				pen_x = double(pad) + double(req.width) - line_width;
		}
		// The layer rectangle is the text's bounding box: centre the line box
		// vertically inside it, baseline = top + ascent.
		const double baseline_y =
			req.height > 0
				? double(pad) + (double(req.height) - (ascent + descent)) * 0.5 + ascent
				: double(pad) + 2.0 + ascent;
		painter.drawText(QPointF(pen_x, baseline_y), text);
	}

	const uchar *bits = image.constBits();
	const int bpl = image.bytesPerLine();
	std::vector<uint8_t> rgba(size_t(w) * size_t(h) * 4, 0);
	bool any = false;
	for (int y = 0; y < h; ++y) {
		const QRgb *row = reinterpret_cast<const QRgb *>(bits + ptrdiff_t(y) * bpl);
		for (int x = 0; x < w; ++x) {
			const unsigned a = unsigned(qAlpha(row[x]));
			if (a == 0)
				continue;
			any = true;
			uint8_t *dst = &rgba[(size_t(y) * size_t(w) + size_t(x)) * 4];
			dst[0] = uint8_t(std::min(255u, (unsigned(qRed(row[x])) * 255u + a / 2u) / a));
			dst[1] = uint8_t(std::min(255u, (unsigned(qGreen(row[x])) * 255u + a / 2u) / a));
			dst[2] = uint8_t(std::min(255u, (unsigned(qBlue(row[x])) * 255u + a / 2u) / a));
			dst[3] = uint8_t(a);
		}
	}
	if (!any)
		return false;
	out.rgba.swap(rgba);

	// Stroke (PSD "Stroke" layer effect, outside): dilate the alpha mask by
	// `stroke_px` and composite the fill over it.  Identical algorithm to the
	// macOS renderer.
	if (stroke_px >= 1.0) {
		const int radius = int(stroke_px + 0.5);
		std::vector<uint8_t> mask(size_t(w) * size_t(h));
		for (size_t i = 0; i < mask.size(); ++i)
			mask[i] = out.rgba[i * 4 + 3];

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

		const uint8_t sc[4] = {uint8_t(std::clamp(req.stroke_color[0], 0.0, 1.0) * 255.0),
				       uint8_t(std::clamp(req.stroke_color[1], 0.0, 1.0) * 255.0),
				       uint8_t(std::clamp(req.stroke_color[2], 0.0, 1.0) * 255.0),
				       uint8_t((req.stroke_color[3] <= 0.0 ? 1.0 : std::clamp(req.stroke_color[3], 0.0, 1.0)) * 255.0)};
		std::vector<uint8_t> merged(out.rgba.size(), 0);
		for (size_t i = 0; i < mask.size(); ++i) {
			const unsigned sa = dilated[i];
			const unsigned fa = out.rgba[i * 4 + 3];
			const unsigned aa = sa * sc[3] / 255u;
			const unsigned br = out.rgba[i * 4 + 0];
			const unsigned bg = out.rgba[i * 4 + 1];
			const unsigned bb = out.rgba[i * 4 + 2];
			const unsigned ba = fa;
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
	out.used_font = family;
	out.font_substituted = substituted;
	return true;
}

} // namespace psd
