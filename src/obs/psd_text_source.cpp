// SPDX-License-Identifier: GPL-2.0-or-later
#include "psd_text_source.h"

#include "text_render.h"
#include "obs_include.h"

#include <algorithm>
#include <cstring>
#include <mutex>
#include <vector>

namespace obspsd {

namespace {

struct PsdText {
	obs_source_t *source = nullptr;

	// Settings, mirrored so video_render does not touch obs_data_t.
	std::string text;
	std::string font;
	double font_size = 100.0;
	uint32_t color = 0xFFFFFFFF;      // ABGR
	bool stroke = false;
	double stroke_width = 0.0;
	uint32_t stroke_color = 0xFFFFFFFF;
	int align = 0;                    // 0 left, 1 right, 2 centre
	int offset_x = 0;                 // document position of the layer rect
	int offset_y = 0;
	int box_width = 0;                // layer rectangle size in document px
	int box_height = 0;
	int pad = 0;                      // rendered bitmap padding (stroke overhang)
	// When both are set the frame is the whole document canvas with the glyphs
	// painted at (offset_x, offset_y). That is exactly how the image layers are
	// written, so the scene item can sit at the origin and still line up - no
	// padding/offset arithmetic, no dependence on when OBS lays the item out.
	int canvas_width = 0;
	int canvas_height = 0;

	double color_r() const { return double((color >> 16) & 0xFF) / 255.0; }
	double color_g() const { return double((color >> 8) & 0xFF) / 255.0; }
	double color_b() const { return double(color & 0xFF) / 255.0; }
	double color_a() const { return double((color >> 24) & 0xFF) / 255.0; }

	// Rasterised bitmap plus the texture built from it.
	std::vector<uint8_t> rgba;        // straight alpha, width*height*4
	int width = 0;
	int height = 0;
	bool dirty = true;

	gs_texture_t *texture = nullptr;
	struct obs_source_frame frame = {};
	size_t frame_bytes = 0;

	// First frames pushed from create() can land before OBS has a graphics
	// context and get dropped, which left the layer invisible until the user
	// nudged a property. video_tick retries until a push has been observed.
	bool frame_pushed = false;
	int repush = 0;   // ticks left to re-send the current frame
};

std::mutex g_tex_mutex;

// obs_data colours are ABGR with alpha in the high byte.
void unpack_color(uint32_t abgr, double out[4])
{
	out[0] = double((abgr >> 16) & 0xFF) / 255.0; // R
	out[1] = double((abgr >> 8) & 0xFF) / 255.0;  // G
	out[2] = double(abgr & 0xFF) / 255.0;         // B
	out[3] = double((abgr >> 24) & 0xFF) / 255.0; // A
	if (out[3] <= 0.0)
		out[3] = 1.0;
}

void psd_text_push_frame(PsdText *ctx);

void rasterise(PsdText *ctx)
{
	blog(LOG_INFO, "[obs-psd-text] rasterise: text='%s' font='%s' size=%.1f", ctx->text.c_str(),
	     ctx->font.c_str(), ctx->font_size);
	if (ctx->text.empty()) {
		ctx->rgba.clear();
		ctx->width = 0;
		ctx->height = 0;
		return;
	}

	psd::TextRenderRequest req;
	req.text = ctx->text;
	req.font = ctx->font;
	req.font_size = ctx->font_size > 0 ? ctx->font_size : 100.0;
	unpack_color(ctx->color, req.color);
	req.justification = ctx->align;
	req.stroke = ctx->stroke;
	req.stroke_width = ctx->stroke_width;
	unpack_color(ctx->stroke_color, req.stroke_color);

	psd::TextRenderResult res;
	if (!psd::render_text_layer(req, res)) {
		blog(LOG_WARNING, "[obs-psd-text] render_text_layer failed");
		ctx->rgba.clear();
		ctx->width = 0;
		ctx->height = 0;
		return;
	}
	ctx->pad = res.pad;
	if (ctx->canvas_width > 0 && ctx->canvas_height > 0) {
		// Full-canvas frame: the same convention the image layers use, so the
		// scene item can stay at (0,0) with scale 1 and be pixel-accurate.
		std::vector<uint8_t> canvas(size_t(ctx->canvas_width) * size_t(ctx->canvas_height) * 4u, 0);
		const int base_x = ctx->offset_x - res.pad;
		const int base_y = ctx->offset_y - res.pad;
		for (int y = 0; y < res.height; ++y) {
			const int dy = base_y + y;
			if (dy < 0 || dy >= ctx->canvas_height)
				continue;
			for (int x = 0; x < res.width; ++x) {
				const int dx = base_x + x;
				if (dx < 0 || dx >= ctx->canvas_width)
					continue;
				const uint8_t *src = &res.rgba[(size_t(y) * size_t(res.width) + size_t(x)) * 4u];
				if (src[3] == 0)
					continue;
				uint8_t *dst = &canvas[(size_t(dy) * size_t(ctx->canvas_width) + size_t(dx)) * 4u];
				dst[0] = src[0];
				dst[1] = src[1];
				dst[2] = src[2];
				dst[3] = src[3];
			}
		}
		ctx->rgba = std::move(canvas);
		ctx->width = ctx->canvas_width;
		ctx->height = ctx->canvas_height;
		blog(LOG_INFO, "[obs-psd-text] rasterised onto %dx%d canvas at (%d,%d) text='%s'",
		     ctx->width, ctx->height, ctx->offset_x, ctx->offset_y, ctx->text.c_str());
		return;
	}
	ctx->rgba = std::move(res.rgba);
	ctx->width = res.width;
	ctx->height = res.height;
	blog(LOG_INFO, "[obs-psd-text] rasterised -> %dx%d font='%s'%s", res.width, res.height,
	     res.used_font.c_str(), res.font_substituted ? " (substituted)" : "");
}

// --------------------------------------------------------------------- source

const char *psd_text_get_name(void *)
{
	return obs_module_text("psd_text.name");
}

void psd_text_apply_settings(PsdText *ctx, obs_data_t *settings)
{
	ctx->dirty = true;
	const char *text = obs_data_get_string(settings, "text");
	ctx->text = text ? text : "";

	// obs_properties_add_font() stores a nested object
	// { face, style, size, flags }; reading it as a string returns "" and the
	// layer silently falls back to a default face. Older scene files (and the
	// importer before this change) wrote a plain PostScript name, so accept both.
	// Font resolution order, most specific first:
	//   1. "font_name" — the PostScript name the importer stored verbatim. This
	//      is the only value that survives every OBS version and is what the
	//      renderer needs.
	//   2. the nested "font" object written by obs_properties_add_font, whose
	//      "face" is a family/face name the user may have just picked.
	// Reading the object first (as the previous version did) lost the PSD font
	// whenever OBS rewrote or stripped the nested object.
	ctx->font.clear();
	const char *stored = obs_data_get_string(settings, "font_name");
	if (stored != nullptr && stored[0] != '\0')
		ctx->font = stored;
	obs_data_t *font_obj = obs_data_get_obj(settings, "font");
	char obj_face[256] = {};
	char obj_style[128] = {};
	if (font_obj != nullptr) {
		blog(LOG_INFO, "[obs-psd-text] font obj json: %s", obs_data_get_json(font_obj));
		const char *face = obs_data_get_string(font_obj, "face");
		const char *style = obs_data_get_string(font_obj, "style");
		if (face != nullptr && face[0] != '\0') {
			snprintf(obj_face, sizeof obj_face, "%s", face);
			if (style != nullptr && style[0] != '\0')
				snprintf(obj_style, sizeof obj_style, "%s", style);
		}
		obs_data_release(font_obj);
	}
	// A face the user picked in the properties replaces the imported name.
	if (obj_face[0] != '\0' && std::string(obj_face) != ctx->font) {
		std::string candidate = obj_face;
		if (!psd::font_is_usable(candidate) && obj_style[0] != '\0')
			candidate += std::string("-") + obj_style;
		if (psd::font_is_usable(candidate))
			ctx->font = candidate;
	}
	blog(LOG_INFO, "[obs-psd-text] apply: font_name='%s' obj_face='%s' -> font='%s'",
	     stored ? stored : "", obj_face, ctx->font.c_str());

	ctx->font_size = obs_data_get_double(settings, "size");
	ctx->color = uint32_t(obs_data_get_int(settings, "color1"));
	blog(LOG_INFO, "[obs-psd-text] color1 raw=0x%08X -> RGBA(%.2f,%.2f,%.2f,%.2f)", ctx->color,
	     ctx->color_r(), ctx->color_g(), ctx->color_b(), ctx->color_a());
	ctx->stroke = obs_data_get_bool(settings, "stroke");
	ctx->stroke_width = obs_data_get_double(settings, "stroke_width");
	ctx->stroke_color = uint32_t(obs_data_get_int(settings, "stroke_color"));
	ctx->align = int(obs_data_get_int(settings, "align"));
	ctx->offset_x = int(obs_data_get_int(settings, "offset_x"));
	ctx->offset_y = int(obs_data_get_int(settings, "offset_y"));
	// width/height of the layer rectangle, used to size the frame like the PSD
	// box rather than the padded bitmap.
	ctx->box_width = int(obs_data_get_int(settings, "box_width"));
	ctx->box_height = int(obs_data_get_int(settings, "box_height"));
	ctx->canvas_width = int(obs_data_get_int(settings, "canvas_width"));
	ctx->canvas_height = int(obs_data_get_int(settings, "canvas_height"));
}

void *psd_text_create(obs_data_t *settings, obs_source_t *source)
{
	auto *ctx = new PsdText();
	ctx->source = source;
	// Fill the fields straight from the settings we were created with: asking
	// obs_source_update() to do it would leave get_width() (called right after
	// creation by the caller) looking at an empty text field.
	psd_text_apply_settings(ctx, settings);
	// Rasterise immediately so get_width()/get_height() report the real size
	// before the first frame is rendered.
	rasterise(ctx);
	psd_text_push_frame(ctx);
	ctx->repush = 8;  // see video_tick: the first push often lands too early
	blog(LOG_INFO, "[obs-psd-text] create: '%s' -> %dx%d font='%s' id=%s", ctx->text.c_str(),
	     ctx->width, ctx->height, ctx->font.c_str(), obs_source_get_id(source));
	return ctx;
}

void psd_text_destroy(void *data)
{
	auto *ctx = static_cast<PsdText *>(data);
	std::lock_guard<std::mutex> lock(g_tex_mutex);
	if (ctx->texture != nullptr) {
		gs_texture_destroy(ctx->texture);
		ctx->texture = nullptr;
	}
	free(ctx->frame.data[0]);
	ctx->frame.data[0] = nullptr;
	delete ctx;
}

uint32_t psd_text_get_width(void *data)
{
	auto *ctx = static_cast<PsdText *>(data);
	// create()/update() already rasterised synchronously so the size is known
	// before OBS asks; rasterising here as well would race with video_tick.
	return uint32_t(std::max(0, ctx->width));
}

// Logs the texture state once per render so an invisible source is diagnosable.
// Async video: OBS owns the texture. We hand it one frame per change, which
// avoids creating GL textures ourselves (gs_texture_create returned NULL for
// this source in every format) and gets scaling/filters for free.
void psd_text_push_frame(PsdText *ctx)
{
	if (ctx->width <= 0 || ctx->height <= 0 || ctx->rgba.empty())
		return;
	const size_t pixel_bytes = size_t(ctx->width) * size_t(ctx->height) * 4u;
	if (ctx->frame.data[0] == nullptr || ctx->frame_bytes != pixel_bytes) {
		free(ctx->frame.data[0]);
		ctx->frame.data[0] = static_cast<uint8_t *>(malloc(pixel_bytes));
		ctx->frame_bytes = pixel_bytes;
	}
	// OBS expects BGRA for GS_BGRA-style async frames; CoreText hands us RGBA.
	const uint8_t *src = ctx->rgba.data();
	uint8_t *dst = ctx->frame.data[0];
	for (size_t i = 0; i + 3 < pixel_bytes; i += 4) {
		const unsigned a = src[i + 3];
		dst[i + 0] = uint8_t((unsigned(src[i + 2]) * a + 127u) / 255u);
		dst[i + 1] = uint8_t((unsigned(src[i + 1]) * a + 127u) / 255u);
		dst[i + 2] = uint8_t((unsigned(src[i + 0]) * a + 127u) / 255u);
		dst[i + 3] = uint8_t(a);
	}
	ctx->frame.width = uint32_t(ctx->width);
	ctx->frame.height = uint32_t(ctx->height);
	ctx->frame.format = VIDEO_FORMAT_BGRA;
	ctx->frame.linesize[0] = uint32_t(ctx->width) * 4u;
	ctx->frame.timestamp = 0;
	// Count opaque pixels: a text layer whose font is missing would otherwise
	// look identical to a positioning bug once it reaches OBS.
	size_t opaque = 0;
	for (size_t i = 3; i < pixel_bytes; i += 4)
		if (dst[i] != 0)
			++opaque;
	obs_source_output_video(ctx->source, &ctx->frame);
	blog(LOG_INFO, "[obs-psd-text] pushed frame %ux%u opaque=%zu text='%s'", ctx->width, ctx->height,
	     opaque, ctx->text.c_str());
	ctx->dirty = false;
	ctx->frame_pushed = true;
}

void psd_text_video_tick(void *data, float)
{
	auto *ctx = static_cast<PsdText *>(data);
	// A frame pushed during create() can arrive before OBS has bound the source
	// to a render path and be dropped, which showed up as "the layer only
	// appears after you change a property". Keep pushing for the first few ticks
	// so a dropped frame is always followed by one that lands.
	static const int kRepushTicks = 8;
	if (ctx->dirty) {
		rasterise(ctx);
		ctx->dirty = false;
		ctx->repush = kRepushTicks;
	}
	if (ctx->repush > 0) {
		--ctx->repush;
		psd_text_push_frame(ctx);
	}
}

uint32_t psd_text_get_height(void *data)
{
	auto *ctx = static_cast<PsdText *>(data);
	return uint32_t(std::max(0, ctx->height));
}

void psd_text_get_defaults(obs_data_t *settings)
{
	obs_data_set_default_string(settings, "text", "");
	obs_data_set_default_string(settings, "font_name", "");
	obs_data_t *font_defaults = obs_data_create();
	obs_data_set_obj(settings, "font", font_defaults);
	obs_data_release(font_defaults);
	obs_data_set_default_double(settings, "size", 100.0);
	obs_data_set_default_int(settings, "color1", 0xFFFFFFFF);
	obs_data_set_default_bool(settings, "stroke", false);
	obs_data_set_default_double(settings, "stroke_width", 16.0);
	obs_data_set_default_int(settings, "stroke_color", 0xFFFFFFFF);
	obs_data_set_default_int(settings, "align", 0);
	obs_data_set_default_int(settings, "offset_x", 0);
	obs_data_set_default_int(settings, "offset_y", 0);
}

void psd_text_activate(void *data)
{
	auto *ctx = static_cast<PsdText *>(data);
	blog(LOG_INFO, "[obs-psd-text] activate '%s'", ctx->text.c_str());
}

void psd_text_deactivate(void *data)
{
	auto *ctx = static_cast<PsdText *>(data);
	blog(LOG_INFO, "[obs-psd-text] deactivate '%s'", ctx->text.c_str());
}

void psd_text_update(void *data, obs_data_t *settings)
{
	auto *ctx = static_cast<PsdText *>(data);
	psd_text_apply_settings(ctx, settings);
	rasterise(ctx);
	psd_text_push_frame(ctx);
	ctx->repush = 8;
}

obs_properties_t *psd_text_get_properties(void *data)
{
	auto *ctx = static_cast<PsdText *>(data);
	obs_properties_t *props = obs_properties_create();

	obs_properties_add_text(props, "text", obs_module_text("psd_text.text"), OBS_TEXT_MULTILINE);
	// A plain string list bound to "font_name". obs_properties_add_font() stores a
	// nested object that does not survive the round trip through
	// obs_source_create() here (it always came back empty), and the renderer only
	// needs the PostScript name anyway.
	obs_property_t *font_list = obs_properties_add_list(props, "font_name",
							    obs_module_text("psd_text.font"),
							    OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	for (const std::string &family : psd_text_font_families())
		obs_property_list_add_string(font_list, family.c_str(), family.c_str());
	obs_properties_add_float(props, "size", obs_module_text("psd_text.size"), 1.0, 1000.0, 1.0);
	obs_properties_add_color_alpha(props, "color1", obs_module_text("psd_text.color"));
	obs_properties_add_bool(props, "stroke", obs_module_text("psd_text.stroke"));
	obs_properties_add_float(props, "stroke_width", obs_module_text("psd_text.stroke_width"), 0.0, 200.0, 1.0);
	obs_properties_add_color_alpha(props, "stroke_color", obs_module_text("psd_text.stroke_color"));
	if (ctx != nullptr && !ctx->font.empty()) {
		obs_properties_add_text(props, "psd_font_name", obs_module_text("psd_text.psd_font"), OBS_TEXT_INFO);
	}
	return props;
}

const obs_source_info kPsdTextInfo = [] {
	obs_source_info info = {};
	info.id = kPsdTextSourceId;
	info.type = OBS_SOURCE_TYPE_INPUT;
	// Plain VIDEO (not CUSTOM_DRAW): OBS wraps the render in its standard
	// filter/tick path, which is what a simple texture source needs.
	info.output_flags = OBS_SOURCE_ASYNC_VIDEO | OBS_SOURCE_SRGB;
	info.get_name = psd_text_get_name;
	info.create = psd_text_create;
	info.destroy = psd_text_destroy;
	info.get_width = psd_text_get_width;
	info.get_height = psd_text_get_height;
	info.get_defaults = psd_text_get_defaults;
	info.get_properties = psd_text_get_properties;
	info.update = psd_text_update;
	info.activate = psd_text_activate;
	info.deactivate = psd_text_deactivate;
	info.video_tick = psd_text_video_tick;
	return info;
}();

} // namespace

void register_psd_text_source()
{
	obs_register_source(&kPsdTextInfo);
}

std::vector<std::string> psd_text_font_families()
{
	return psd::installed_font_families();
}

int psd_text_source_pad(struct obs_source *source)
{
	if (source == nullptr)
		return 0;
	auto *ctx = static_cast<PsdText *>(obs_obj_get_data(source));
	return ctx != nullptr ? ctx->pad : 0;
}

// Reads the document offset a PSD import stored on a text source instance.
bool psd_text_source_offset(struct obs_source *source, int &x, int &y, int *pad)
{
	if (source == nullptr)
		return false;
	// Identify our source by the data it carries rather than by id: the id this
	// build reports (obs_source_get_id()) is not reliably the unversioned one.
	obs_data_t *settings = obs_source_get_settings(source);
	const bool ours = obs_data_has_user_value(settings, "offset_x") ||
			  obs_data_has_user_value(settings, "font_name");
	if (!ours) {
		obs_data_release(settings);
		return false;
	}
	x = int(obs_data_get_int(settings, "offset_x"));
	y = int(obs_data_get_int(settings, "offset_y"));
	obs_data_release(settings);
	// The padding depends on the stroke width, so ask the live source.
	if (pad != nullptr) {
		auto *ctx = static_cast<PsdText *>(obs_obj_get_data(source));
		*pad = ctx != nullptr ? ctx->pad : 0;
	}
	return true;
}

} // namespace obspsd
