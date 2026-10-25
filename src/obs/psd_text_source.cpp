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

	// Rasterised bitmap plus the texture built from it.
	std::vector<uint8_t> rgba;        // straight alpha, width*height*4
	int width = 0;
	int height = 0;
	bool dirty = true;

	gs_texture_t *texture = nullptr;
	struct obs_source_frame frame = {};
	size_t frame_bytes = 0;
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

// CoreText produces premultiplied RGBA; OBS' default texrender expects straight
// alpha for GS_BGRA with the standard shader, so premultiply here instead.
void premultiply_into(const std::vector<uint8_t> &straight, std::vector<uint8_t> &out)
{
	out.resize(straight.size());
	for (size_t i = 0; i + 3 < straight.size(); i += 4) {
		const unsigned a = straight[i + 3];
		out[i + 0] = uint8_t((unsigned(straight[i + 0]) * a + 127u) / 255u);
		out[i + 1] = uint8_t((unsigned(straight[i + 1]) * a + 127u) / 255u);
		out[i + 2] = uint8_t((unsigned(straight[i + 2]) * a + 127u) / 255u);
		out[i + 3] = uint8_t(a);
	}
}

void rasterise(PsdText *ctx)
{
	blog(LOG_INFO, "[obs-psd-text] rasterise: text='%s' font='%s' size=%.1f", ctx->text.c_str(),
	     ctx->font.c_str(), ctx->font_size);
	if (ctx->text.empty()) {
		ctx->rgba.clear();
		ctx->width = 0;
		ctx->height = 0;
		ctx->dirty = false;
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
		ctx->dirty = false;
		return;
	}
	ctx->rgba = std::move(res.rgba);
	ctx->width = res.width;
	ctx->height = res.height;
	ctx->dirty = false;
	blog(LOG_INFO, "[obs-psd-text] rasterised -> %dx%d font='%s'", res.width, res.height,
	     res.used_font.c_str());
}

// --------------------------------------------------------------------- source

const char *psd_text_get_name(void *)
{
	return obs_module_text("psd_text.name");
}

void psd_text_apply_settings(PsdText *ctx, obs_data_t *settings)
{
	const char *text = obs_data_get_string(settings, "text");
	const char *font = obs_data_get_string(settings, "font");
	ctx->text = text ? text : "";
	ctx->font = font ? font : "";
	ctx->font_size = obs_data_get_double(settings, "size");
	ctx->color = uint32_t(obs_data_get_int(settings, "color1"));
	ctx->stroke = obs_data_get_bool(settings, "stroke");
	ctx->stroke_width = obs_data_get_double(settings, "stroke_width");
	ctx->stroke_color = uint32_t(obs_data_get_int(settings, "stroke_color"));
	ctx->align = int(obs_data_get_int(settings, "align"));
	ctx->dirty = true;
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
	blog(LOG_INFO, "[obs-psd-text] create: '%s' -> %dx%d", ctx->text.c_str(), ctx->width, ctx->height);
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
	if (ctx->dirty)
		rasterise(ctx);
	return uint32_t(std::max(0, ctx->width));
}

// Logs the texture state once per render so an invisible source is diagnosable.
// Async video: OBS owns the texture. We hand it one frame per change, which
// avoids creating GL textures ourselves (gs_texture_create returned NULL for
// this source in every format) and gets scaling/filters for free.
void psd_text_video_tick(void *data, float)
{
	auto *ctx = static_cast<PsdText *>(data);
	if (!ctx->dirty)
		return;
	rasterise(ctx);
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
	obs_source_output_video(ctx->source, &ctx->frame);
	blog(LOG_INFO, "[obs-psd-text] pushed frame %ux%u", ctx->width, ctx->height);
}

uint32_t psd_text_get_height(void *data)
{
	auto *ctx = static_cast<PsdText *>(data);
	if (ctx->dirty)
		rasterise(ctx);
	return uint32_t(std::max(0, ctx->height));
}

void psd_text_get_defaults(obs_data_t *settings)
{
	obs_data_set_default_string(settings, "text", "");
	obs_data_set_default_string(settings, "font", "");
	obs_data_set_default_double(settings, "size", 100.0);
	obs_data_set_default_int(settings, "color1", 0xFFFFFFFF);
	obs_data_set_default_bool(settings, "stroke", false);
	obs_data_set_default_double(settings, "stroke_width", 16.0);
	obs_data_set_default_int(settings, "stroke_color", 0xFFFFFFFF);
	obs_data_set_default_int(settings, "align", 0);
}

void psd_text_update(void *data, obs_data_t *settings)
{
	psd_text_apply_settings(static_cast<PsdText *>(data), settings);
}

obs_properties_t *psd_text_get_properties(void *data)
{
	auto *ctx = static_cast<PsdText *>(data);
	obs_properties_t *props = obs_properties_create();

	obs_properties_add_text(props, "text", obs_module_text("psd_text.text"), OBS_TEXT_MULTILINE);
	obs_properties_add_font(props, "font", obs_module_text("psd_text.font"));
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
	info.video_tick = psd_text_video_tick;
	return info;
}();

} // namespace

void register_psd_text_source()
{
	obs_register_source(&kPsdTextInfo);
}

} // namespace obspsd
