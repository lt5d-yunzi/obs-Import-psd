// SPDX-License-Identifier: GPL-2.0-or-later
//
// obs-psd :: scene_build.cpp
//
// =============================================================================
// DESIGN NOTE - pixel path
// =============================================================================
//
// Each PSD layer becomes exactly one OBS scene item:
//
//   psd::Layer::rgba (straight RGBA8, unreferenced by the layer, tight rows)
//     -> PNG on disk (stb_image_write, lossless, alpha preserved)
//     -> obs_source_create_private("image_source", <layer name>, {"file": ...})
//
// Why the built-in "image_source" (id is still "image_source" in OBS 32; it is
// served by the "image-source" module, plugins/image-source/image-source.c):
//  * it decodes PNG through gs_image_file4 just like the UI does, and
//  * its video_render premultiplies correctly (blend ONE/INVSRCALPHA with
//    GS_IMAGE_ALPHA_PREMULTIPLY decode), so straight-alpha PNG layers composite
//    exactly like in Photoshop,
//  * it renders on the GPU at scene-item scale, so scaling to the canvas stays
//    crisp/asynchronous and the layers remain live (re-editable).
//
// PNG vs EXR: image_source supports PNG (and JPEG/GIF/TGA/BMP) and keeps the
// alpha channel of PNG - transparent pixels survive because we write the straight
// RGBA out verbatim and set GS_IMAGE_ALPHA_PREMULTIPLY on the source side.  EXR is
// NOT used: 8-bit PSD layers carry no extra range, and OBS' image loader for EXR
// goes through a different path (gs_image_file3 / 16f) whose HDR output is not
// what a PSD/8-bit scene wants.  Storing 8-bit PNGs keeps RAM/disk small; if a
// future PSD depth==16 read exists, switch to 16-bit PNG via stbi_write_png_16.
// (stb_image_write is header-only and already vendored in third_party/.)
//
// IMPORTANT: image_source decodes the PNG synchronously inside
// image_source_update() (which obs_source_create_private() calls), but creates
// the GPU texture only later on the graphics thread (image_source_tick ->
// image_source_load_texture).  Until then get_width()/get_height() report 0.
// build_scene_from_psd() therefore polls the *sources* (not the stage items),
// because invisible stage items are not updated by libobs.
//
// Ordering / geometry:
//  * psd::Document::layers is bottom-most first; obs_scene_add() prepends, so
//    iterating bottom->top reproduces the PSD stacking order.
//  * group semantics of the PSD reader/writer (see psd_write.cpp): a layer with
//    blend_key == "group" is followed (in bottom->top order) by the layers it
//    contains, i.e. the layers with a HIGHER index until the next "group" layer.
//    Those become a real OBS group (obs_sceneitem_group_add_item) named after
//    the layer, and the group is itself placed in the parent in PSD order.
//  * Each item is placed with obs_sceneitem_set_info2(): alignment TOP|LEFT,
//    pos = top-left corner, scale = (1,1) for Place_DocPixels or
//    fit = min(canvas_w/doc_w, canvas_h/doc_h) for Place_FitCanvas with the
//    document centred on the canvas.  bounds = NONE (the item rect is the layer
//    rect * fit), so PSD document coordinates map 1:1 (times fit) onto canvas
//    coordinates, including layers that stick out of the document bounds.
//  * visibility: obs_sceneitem_set_visible(layer.visible).  Group visibility is
//    the OR of its members so a visible child never ends up inside a hidden group.
//  * opacity: PSD layer opacity is 0..100 and OBS' scene item transform has no
//    opacity field.  Where OBS can express it we use it; where it cannot we
//    combine the opacity into the exported PNG's alpha channel (straight-alpha
//    multiply, fully Photoshop-equivalent for Normal blending, an approximation
//    for Screen/Multiply).  Item-level opacity APIs do not exist for
//    obs_scene_item in the public headers, so the PNG bake is the default.
//  * blending: PSD "mult"/"scrn" map to obs_sceneitem_set_blending_mode()
//    OBS_BLEND_MULTIPLY / OBS_BLEND_SCREEN; everything else is OBS_BLEND_NORMAL.
//  * clipped layers (clipping mask) have no OBS equivalent; they are imported
//    as ordinary sibling items and reported through `blog()` warnings.
//
// =============================================================================
// DESIGN NOTE - scene creation & threading
// =============================================================================
//
// Public API check (third_party/obs-headers/frontend-api/obs-frontend-api.h,
// OBS 32 x): there is NO obs_frontend_add_scene()/obs_frontend_create_scene().
// The only frontend scene accessors are obs_frontend_get_scenes() and
// obs_frontend_set_current_scene().  OBS's own UI (OBSBasic::AddScene/
// AddSceneData) creates scenes with obs_scene_create(name) and then publishes
// them with obs_source_dosignal(..., "source_create") - a libobs-internal
// helper that plugin code cannot link against.  What is actually observable by
// the frontend is libobs' global source list: obs_source_create() (non-private)
// inserts the source into obs->data.public_sources before returning, so
// obs_frontend_get_scenes() already lists the new scene the next time the UI
// enumerates it (LIFO order, i.e. prepended: OBSBasic::OnEvent(
// OBS_FRONTEND_EVENT_SCENE_LIST_CHANGED) -> RefreshScenes() shows it on top).
// We therefore use obs_scene_create(name) (public) - NOT create_private, which
// would be invisible to the frontend - and let the caller switch to it with
// obs_frontend_set_current_scene().
//
// Threading: scene/UI bookkeeping runs on the calling thread; it must be the
// Qt main (UI) thread (or any thread while the frontend is idle), because
// image_source_create/update load files and the tick callback is the graphics
// thread.  The only thing that needs the graphics thread is the texture upload,
// which libobs performs itself in image_source_tick(); we merely wait for it.
// =============================================================================

#include "scene_build.h"
#include "obs/text_render.h"

#include <obs.h>
#include <obs-frontend-api.h>
#include <graphics/graphics.h>
#include <util/bmem.h>
#include <util/platform.h>
#include <util/threading.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

// Third-party (vendored) header: silence its unrelated diagnostics only.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wmissing-field-initializers"
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
#pragma clang diagnostic pop

namespace obspsd {

namespace {

// ---------------------------------------------------------------------------
// Generated image files: unique per layer, kept alive for the process lifetime.
// ---------------------------------------------------------------------------

std::mutex g_files_mutex;
std::vector<std::string> *g_kept_files = nullptr; // heap: no static dtor racing OBS shutdown
std::string g_scene_image_dir;                    // where this scene's PNGs live
std::vector<MissingFont> g_missing_fonts;        // from the most recent build

// Directory that holds the PNGs of one imported scene:
//   <user config>/obs-studio/obs-psd/<scene>/
// The files must outlive the OBS process because the scene stores their paths;
// writing to $TMPDIR made every layer disappear after a restart.
std::string make_scene_image_dir(const std::string &scene_name)
{
	char *base = os_get_config_path_ptr("obs-studio/obs-psd");
	std::string dir;
	if (base != nullptr) {
		dir = base;
		bfree(base);
	}
	if (dir.empty())
		dir = "/tmp/obs-psd";

	// Sanitise the scene name: it becomes a directory component.
	std::string safe;
	safe.reserve(scene_name.size());
	for (char c : scene_name) {
		const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' ||
				c == '_' || c == ' ';
		safe.push_back(ok ? c : '_');
	}
	while (!safe.empty() && safe.back() == ' ')
		safe.pop_back();
	if (safe.empty())
		safe = "scene";

	dir += "/" + safe;
	os_mkdirs(dir.c_str());
	return dir;
}

std::string make_scene_image_path(const std::string &dir)
{
	static std::atomic<uint64_t> counter{0};
	const uint64_t n = counter.fetch_add(1, std::memory_order_relaxed);
	const uint64_t stamp = (uint64_t)os_gettime_ns();

	char name[128];
	std::snprintf(name, sizeof(name), "layer-%llu-%llu.png", (unsigned long long)stamp, (unsigned long long)n);
	return dir + "/" + name;
}

// Bakes the PSD layer opacity into the straight-alpha buffer (see design note).
void apply_opacity(std::vector<uint8_t> &rgba, float opacity)
{
	if (opacity >= 99.999f)
		return;
	const float f = std::max(0.0f, std::min(1.0f, opacity / 100.0f));
	for (size_t i = 3; i < rgba.size(); i += 4)
		rgba[i] = (uint8_t)std::lround((float)rgba[i] * f);
}

// Writes `layer` as a lossless PNG with its alpha channel intact.
// Writes one layer as a PNG that covers the WHOLE canvas, with the layer's
// pixels blitted at its document position. Importing a full-canvas image means
// the scene item is always placed at (0,0) with scale 1, so the result does not
// depend on how OBS interprets per-item transforms (which shifted items when a
// group or a bounds mode was involved).
bool write_layer_png(const psd::Layer &layer, int canvas_w, int canvas_h, bool full_canvas,
		     std::string &path_out, std::string &err)
{
	const int w = layer.width();
	const int h = layer.height();
	if (w <= 0 || h <= 0) {
		err = "layer '" + layer.name + "' has an empty rectangle";
		return false;
	}
	if (layer.rgba.size() != (size_t)w * (size_t)h * 4u) {
		err = "layer '" + layer.name + "' has an inconsistent RGBA buffer";
		return false;
	}

	std::vector<uint8_t> pixels = layer.rgba;
	apply_opacity(pixels, layer.opacity);

	int out_w = w;
	int out_h = h;
	if (full_canvas && canvas_w > 0 && canvas_h > 0) {
		// Blit the layer into a transparent canvas-sized buffer.
		std::vector<uint8_t> canvas(size_t(canvas_w) * size_t(canvas_h) * 4u, 0);
		for (int y = 0; y < h; ++y) {
			const int dy = layer.top + y;
			if (dy < 0 || dy >= canvas_h)
				continue;
			for (int x = 0; x < w; ++x) {
				const int dx = layer.left + x;
				if (dx < 0 || dx >= canvas_w)
					continue;
				const uint8_t *src = &pixels[(size_t(y) * size_t(w) + size_t(x)) * 4u];
				uint8_t *dst = &canvas[(size_t(dy) * size_t(canvas_w) + size_t(dx)) * 4u];
				dst[0] = src[0];
				dst[1] = src[1];
				dst[2] = src[2];
				dst[3] = src[3];
			}
		}
		pixels.swap(canvas);
		out_w = canvas_w;
		out_h = canvas_h;
	}

	std::string path;
	{
		std::lock_guard<std::mutex> lock(g_files_mutex);
		path = make_scene_image_path(g_scene_image_dir);
	}

	if (!stbi_write_png(path.c_str(), out_w, out_h, 4, pixels.data(), out_w * 4)) {
		err = "could not write layer PNG: " + path;
		return false;
	}

	{
		std::lock_guard<std::mutex> lock(g_files_mutex);
		if (!g_kept_files)
			g_kept_files = new std::vector<std::string>();
		g_kept_files->push_back(path);
	}

	path_out = path;
	return true;
}

// ---------------------------------------------------------------------------
// Geometry
// ---------------------------------------------------------------------------

struct Placement {
	double fit = 1.0;      // document pixels -> canvas pixels
	double origin_x = 0.0; // canvas coordinate of document pixel (0, 0)
	double origin_y = 0.0;
	int canvas_w = 0;      // OBS canvas size, used to render full-canvas layers
	int canvas_h = 0;
};

Placement compute_placement(const psd::Document &doc, SceneBuildOptions::Placement mode)
{
	Placement p;
	if (mode == SceneBuildOptions::Place_DocPixels || doc.width <= 0 || doc.height <= 0)
		return p;

	struct obs_video_info ovi = {};
	uint32_t canvas_w = 0, canvas_h = 0;
	if (obs_get_video_info(&ovi)) {
		canvas_w = ovi.base_width;
		canvas_h = ovi.base_height;
	}
	if (!canvas_w || !canvas_h)
		return p; // no video configured: fall back to 1:1

	p.canvas_w = int(canvas_w);
	p.canvas_h = int(canvas_h);
	p.fit = std::min((double)canvas_w / (double)doc.width, (double)canvas_h / (double)doc.height);
	p.origin_x = ((double)canvas_w - (double)doc.width * p.fit) * 0.5;
	p.origin_y = ((double)canvas_h - (double)doc.height * p.fit) * 0.5;
	return p;
}

void place_item(obs_sceneitem_t *item, const psd::Layer &layer, const Placement &p)
{
	// Layers are written as full-canvas images (see write_layer_png), so the
	// item's own position inside its image is already correct: pin it to the
	// canvas origin and only apply the document->canvas scale. This avoids every
	// per-item coordinate computation OBS performs for partial images.
	(void)layer;
	obs_sceneitem_set_alignment(item, OBS_ALIGN_LEFT | OBS_ALIGN_TOP);
	obs_sceneitem_set_bounds_type(item, OBS_BOUNDS_NONE);

	struct vec2 pos;
	vec2_set(&pos, 0.0f, 0.0f);
	obs_sceneitem_set_pos(item, &pos);

	struct vec2 scale;
	vec2_set(&scale, (float)p.fit, (float)p.fit);
	obs_sceneitem_set_scale(item, &scale);
}

void apply_blending(obs_sceneitem_t *item, const psd::Layer &layer)
{
	// PSD stored keys as produced by psd::detail::blend_code_from_key(): "mul ", "scrn".
	if (layer.blend_key == "mul " || layer.blend_mode == 'M')
		obs_sceneitem_set_blending_mode(item, OBS_BLEND_MULTIPLY);
	else if (layer.blend_key == "scrn" || layer.blend_mode == 'S')
		obs_sceneitem_set_blending_mode(item, OBS_BLEND_SCREEN);
	else
		obs_sceneitem_set_blending_mode(item, OBS_BLEND_NORMAL);
}

// ---------------------------------------------------------------------------
// Build state
// ---------------------------------------------------------------------------

struct BuildState {
	std::vector<obs_source_t *> images; // every image source we created (refs held)
	std::string err;
	// Text styling, taken from SceneBuildOptions when the user set it.
	bool text_style_valid = false;
	double text_color[4] = {0.8, 0.6, 0.8, 1.0};
	bool text_stroke = true;
	double text_stroke_width = 16.0;
	double text_stroke_color[4] = {1, 1, 1, 1};
	std::vector<MissingFont> missing_fonts;
	bool text_force_obs_source = false;
};

// Builds a plain OBS text source for a PSD type layer. Used when the PSD's font
// is not installed: OBS picks a font it can actually render (and the user can
// edit the text, font and colour directly in OBS), instead of our renderer
// silently substituting a face.
obs_source_t *create_obs_text_source(BuildState &st, const psd::Layer &layer, double fit)
{
	obs_data_t *settings = obs_data_create();
	obs_data_set_string(settings, "text", layer.text.c_str());
	// text_ft2_source_v2 does not render anything with an empty font name, so
	// pick a real one: the PSD font when it is installed, otherwise a system
	// CJK face that always exists on macOS.
	std::string font = layer.text_font;
	if (font.empty() || !psd::font_is_usable(font)) {
		const char *candidates[] = {"PingFangSC-Semibold", "STHeitiSC-Medium", "HiraginoSansGB-W3",
					    "Helvetica"};
		font.clear();
		for (const char *c : candidates) {
			if (psd::font_is_usable(c)) {
				font = c;
				break;
			}
		}
	}
	if (font.empty())
		font = "Helvetica";
	obs_data_set_string(settings, "font", font.c_str());

	// text_ft2_source_v2 stores the size in its "size" setting; leaving it unset
	// leaves the source with no glyphs at all. PSD point sizes are em sizes, so
	// scale by the same 0.7 the renderer uses, and by the canvas fit.
	const double px = (layer.text_size > 0 ? layer.text_size * 0.7 : 48.0) * (fit > 0 ? fit : 1.0);
	obs_data_set_int(settings, "size", (long long)(px > 1.0 ? px : 1.0));

	// OBS colours are ABGR with the alpha in the high byte.
	const double *c = st.text_style_valid ? st.text_color : layer.text_color;
	const uint32_t r = uint32_t(std::max(0.0, std::min(1.0, c[0])) * 255.0);
	const uint32_t g = uint32_t(std::max(0.0, std::min(1.0, c[1])) * 255.0);
	const uint32_t b = uint32_t(std::max(0.0, std::min(1.0, c[2])) * 255.0);
	const uint32_t color = (0xFFu << 24) | (r << 16) | (g << 8) | b;
	obs_data_set_int(settings, "color1", color);
	obs_data_set_int(settings, "color2", color);
	obs_data_set_bool(settings, "outline", st.text_style_valid && st.text_stroke);
	obs_data_set_bool(settings, "drop_shadow", false);
	obs_data_set_int(settings, "align", OBS_ALIGN_LEFT);
	obs_data_set_bool(settings, "word_wrap", false);
	obs_data_set_int(settings, "antialiasing", 1);
	obs_data_set_int(settings, "log_mode", 0);

	// Create the source first and then apply the settings, which is exactly what
	// the OBS UI does. text_ft2_source_v2 builds its glyph cache in
	// update(), and a source created with settings in one shot can end up with
	// an empty cache here (the source rendered nothing at all).
	obs_source_t *src = obs_source_create("text_ft2_source_v2", layer.name.c_str(), nullptr, nullptr);
	if (!src) {
		obs_data_release(settings);
		st.err = "could not create text_ft2_source_v2 for layer '" + layer.name + "'";
		return nullptr;
	}
	obs_source_update(src, settings);
	obs_data_release(settings);

	blog(LOG_INFO, "[obs-psd] OBS text source '%s' '%s' size=%lld -> %ux%u", layer.name.c_str(),
	     layer.text.c_str(), (long long)px, obs_source_get_width(src), obs_source_get_height(src));
	st.images.push_back(src);
	return src;
}

// Renders a PSD type layer with CoreText and imports the bitmap as an image
// source. A text source would show missing-glyph boxes whenever the PSD's font
// is absent (common), so rendering gives a faithful, font-independent picture.
obs_source_t *create_text_source(BuildState &st, const psd::Layer &layer, double fit, int canvas_w, int canvas_h)
{
	psd::TextRenderRequest req;
	req.text = layer.text;
	req.font = layer.text_font;
	req.font_size = layer.text_size;
	for (int i = 0; i < 4; ++i)
		req.color[i] = st.text_style_valid ? st.text_color[i] : layer.text_color[i];
	req.justification = 0;
	req.tracking = layer.text_tracking;
	req.width = layer.width();
	req.height = layer.height();
	req.stroke = st.text_style_valid ? st.text_stroke : layer.stroke_on;
	req.stroke_width = st.text_style_valid ? st.text_stroke_width : layer.stroke_size;
	for (int i = 0; i < 4; ++i)
		req.stroke_color[i] = st.text_style_valid ? st.text_stroke_color[i] : layer.stroke_color[i];

	// PSD fonts are frequently not installed. When that happens the user asked
	// for an OBS text source instead of a rendered bitmap: it stays editable and
	// OBS provides a font it can actually display.
	const bool psd_font_ok = layer.text_font.empty() || psd::font_is_usable(layer.text_font);
	blog(LOG_INFO, "[obs-psd] layer '%s' font='%s' usable=%d force=%d", layer.name.c_str(),
	     layer.text_font.c_str(), int(psd_font_ok), int(st.text_force_obs_source));
	if (st.text_force_obs_source || !psd_font_ok) {
		if (psd_font_ok)
			blog(LOG_INFO, "[obs-psd] layer '%s' forced to an OBS text source", layer.name.c_str());
		else
			blog(LOG_WARNING,
			     "[obs-psd] layer '%s' uses font '%s' which is not installed; using an OBS text "
			     "source (install the font and re-import to keep the original styling)",
			     layer.name.c_str(), layer.text_font.c_str());
		if (obs_source_t *fallback = create_obs_text_source(st, layer, fit))
			return fallback;
		// Fall through to the renderer if OBS could not create the source.
	}

	psd::TextRenderResult res;
	if (!psd::render_text_layer(req, res)) {
		blog(LOG_WARNING, "[obs-psd] could not render text layer '%s' (text='%s', font='%s')",
		     layer.name.c_str(), layer.text.c_str(), layer.text_font.c_str());
		return nullptr;
	}
	if (res.font_substituted) {
		blog(LOG_WARNING,
		     "[obs-psd] text layer '%s' wants font '%s', which is missing here; rendered with '%s'",
		     layer.name.c_str(), layer.text_font.c_str(), res.used_font.c_str());
		st.missing_fonts.push_back(MissingFont{layer.name, layer.text_font, res.used_font});
	} else {
		blog(LOG_INFO, "[obs-psd] text layer '%s' rendered with font '%s'", layer.name.c_str(),
		     res.used_font.c_str());
	}

	// Same full-canvas convention as image layers: blit the rendered text at the
	// layer's document position so the scene item can always sit at (0,0).
	std::vector<uint8_t> canvas;
	const uint8_t *pixels = res.rgba.data();
	int out_w = res.width, out_h = res.height;
	if (canvas_w > 0 && canvas_h > 0) {
		canvas.assign(size_t(canvas_w) * size_t(canvas_h) * 4u, 0);
		for (int y = 0; y < res.height; ++y) {
			const int dy = layer.top + y;
			if (dy < 0 || dy >= canvas_h)
				continue;
			for (int x = 0; x < res.width; ++x) {
				const int dx = layer.left + x;
				if (dx < 0 || dx >= canvas_w)
					continue;
				const uint8_t *s2 = &res.rgba[(size_t(y) * size_t(res.width) + size_t(x)) * 4u];
				uint8_t *d2 = &canvas[(size_t(dy) * size_t(canvas_w) + size_t(dx)) * 4u];
				const unsigned sa = s2[3];
				if (sa == 0)
					continue;
				d2[0] = uint8_t(std::min(255u, (unsigned(s2[0]) * sa + 127u) / 255u));
				d2[1] = uint8_t(std::min(255u, (unsigned(s2[1]) * sa + 127u) / 255u));
				d2[2] = uint8_t(std::min(255u, (unsigned(s2[2]) * sa + 127u) / 255u));
				d2[3] = uint8_t(sa);
			}
		}
		pixels = canvas.data();
		out_w = canvas_w;
		out_h = canvas_h;
	}

	std::string path;
	{
		std::lock_guard<std::mutex> lock(g_files_mutex);
		path = make_scene_image_path(g_scene_image_dir);
	}
	if (!stbi_write_png(path.c_str(), out_w, out_h, 4, pixels, out_w * 4)) {
		st.err = "could not write rendered text PNG: " + path;
		return nullptr;
	}
	{
		std::lock_guard<std::mutex> lock(g_files_mutex);
		if (!g_kept_files)
			g_kept_files = new std::vector<std::string>();
		g_kept_files->push_back(path);
	}

	obs_data_t *settings = obs_data_create();
	obs_data_set_string(settings, "file", path.c_str());
	obs_data_set_bool(settings, "unload", false);
	obs_data_set_bool(settings, "linear_alpha", false);
	obs_source_t *src = obs_source_create("image_source", layer.name.c_str(), settings, nullptr);
	obs_data_release(settings);
	if (!src) {
		st.err = "could not create image_source for rendered text layer '" + layer.name + "'";
		return nullptr;
	}
	st.images.push_back(src);
	return src;
}

// Builds an image source for one PSD layer (PNG written lazily on first use).
obs_source_t *create_layer_source(BuildState &st, const psd::Layer &layer, int canvas_w, int canvas_h)
{
	std::string png;
	if (!write_layer_png(layer, canvas_w, canvas_h, true, png, st.err))
		return nullptr;

	obs_data_t *settings = obs_data_create();
	obs_data_set_string(settings, "file", png.c_str());
	obs_data_set_bool(settings, "unload", false); // keep the texture alive while hidden
	obs_data_set_bool(settings, "linear_alpha", false);

	obs_source_t *src = obs_source_create("image_source", layer.name.c_str(), settings, nullptr);
	obs_data_release(settings);

	if (!src) {
		st.err = "could not create image_source for layer '" + layer.name + "'";
		return nullptr;
	}
	st.images.push_back(src);
	return src;
}

// Index after the last member of the group starting at `group_index`, i.e. the
// first index whose layer is another group (or layers.size()).
size_t group_end(const psd::Document &doc, size_t group_index)
{
	size_t i = group_index + 1;
	while (i < doc.layers.size() && doc.layers[i].blend_key != "group")
		++i;
	return i;
}

bool is_group(const psd::Layer &l)
{
	return l.blend_key == "group";
}

// Adds one layer to `parent` (the root scene or a group's inner scene).

// Adds one layer to `parent` (the root scene or a group's inner scene).
bool add_layer(BuildState &st, obs_scene_t *parent, const psd::Layer &layer, const Placement &p)
{
	const bool is_text = !layer.text.empty();
	if (layer.empty() || (layer.rgba.empty() && !is_text)) {
		// Vector shape layers store no pixels in a PSD (Photoshop renders them on
		// the fly) and carry no text, so there is nothing to import for them.
		blog(LOG_INFO, "[obs-psd] layer '%s' has no pixel data (vector shape); skipped", layer.name.c_str());
		return true;
	}
	if (layer.clipped)
		blog(LOG_WARNING, "[obs-psd] layer '%s' uses a clipping mask; imported as a plain layer",
		     layer.name.c_str());

	obs_source_t *src = is_text ? create_text_source(st, layer, p.fit, p.canvas_w, p.canvas_h)
					     : create_layer_source(st, layer, p.canvas_w, p.canvas_h);
	if (!src)
		return false;

	obs_sceneitem_t *item = obs_scene_add(parent, src);
	if (!item) {
		st.err = "obs_scene_add failed for layer '" + layer.name + "'";
		return false;
	}

	place_item(item, layer, p);
	apply_blending(item, layer);
	obs_sceneitem_set_visible(item, layer.visible);
	return true;
}

// Adds a group layer and recursively everything it contains (see design note).
bool add_group(BuildState &st, obs_scene_t *parent, const psd::Document &doc, size_t group_index, size_t end,
	       const Placement &p, bool *any_visible_out)
{
	const psd::Layer &gl = doc.layers[group_index];

	obs_sceneitem_t *group_item = obs_scene_add_group(parent, gl.name.c_str());
	if (!group_item) {
		st.err = "obs_scene_add_group failed for group '" + gl.name + "'";
		return false;
	}
	obs_scene_t *inner = obs_sceneitem_group_get_scene(group_item);
	if (!inner) {
		st.err = "group '" + gl.name + "' has no inner scene";
		return false;
	}

	// OBS anchors a group scene's coordinate system at the group item's
	// transform, and obs_scene_add_group() re-fits that transform to the
	// members' bounding box whenever members are added. Members use DOCUMENT
	// coordinates here, so pin the group at the placement origin with unit
	// scale *before* adding anything; otherwise the group's derived offset is
	// applied on top of every member position (which is exactly what made
	// layers land tens of pixels off).
	obs_sceneitem_set_alignment(group_item, OBS_ALIGN_LEFT | OBS_ALIGN_TOP);
	obs_sceneitem_set_bounds_type(group_item, OBS_BOUNDS_NONE);
	obs_sceneitem_set_bounds_alignment(group_item, OBS_ALIGN_LEFT | OBS_ALIGN_TOP);
	{
		struct vec2 gpos;
		vec2_set(&gpos, (float)p.origin_x, (float)p.origin_y);
		obs_sceneitem_set_pos(group_item, &gpos);
		struct vec2 gscale;
		vec2_set(&gscale, (float)p.fit, (float)p.fit);
		obs_sceneitem_set_scale(group_item, &gscale);
	}

	blog(LOG_INFO, "[obs-psd] group '%s' contains %zu layer(s)", gl.name.c_str(), end - group_index - 1);

	bool any_visible = false;
	size_t i = group_index + 1;
	while (i < end) {
		if (is_group(doc.layers[i])) {
			const size_t sub_end = group_end(doc, i);
			bool sub_visible = false;
			if (!add_group(st, inner, doc, i, sub_end, p, &sub_visible))
				return false;
			any_visible = any_visible || sub_visible;
			i = sub_end;
		} else {
			if (!add_layer(st, inner, doc.layers[i], p))
				return false;
			any_visible = any_visible || (doc.layers[i].visible && !doc.layers[i].empty());
			++i;
		}
	}

	// Group items have no opacity of their own in OBS (members already carry the
	// baked opacity); the group rectangle is only a Photoshop metadata hint.
	obs_sceneitem_set_visible(group_item, any_visible);
	if (any_visible_out)
		*any_visible_out = any_visible;
	return true;
}

bool any_pixels(const psd::Document &doc)
{
	for (const auto &l : doc.layers)
		if (!l.empty() && !l.rgba.empty())
			return true;
	return false;
}

} // namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void keep_generated_image_files_alive()
{
	std::lock_guard<std::mutex> lock(g_files_mutex);
	if (!g_kept_files)
		g_kept_files = new std::vector<std::string>();
}

std::vector<MissingFont> missing_fonts()
{
	return g_missing_fonts;
}

std::vector<std::string> generated_image_files()
{
	std::lock_guard<std::mutex> lock(g_files_mutex);
	return g_kept_files ? *g_kept_files : std::vector<std::string>();
}

bool build_scene_from_psd(const psd::Document &doc, const char *scene_name, std::string &err, obs_source_t **scene_out,
			  const SceneBuildOptions &options)
{
	err.clear();
	if (scene_out)
		*scene_out = nullptr;

	if (!scene_name || !*scene_name) {
		err = "empty scene name";
		return false;
	}
	// Layer PNGs are written next to the OBS config so the saved scene can find
	// them after a restart.
	g_scene_image_dir = make_scene_image_dir(scene_name);
	blog(LOG_INFO, "[obs-psd] layer images -> %s", g_scene_image_dir.c_str());
	if (doc.width <= 0 || doc.height <= 0) {
		err = "document has no size";
		return false;
	}
	if (!any_pixels(doc)) {
		err = "document has no layer pixels to import";
		return false;
	}

	BuildState st;
	st.text_style_valid = options.text_style_valid;
	for (int i = 0; i < 4; ++i) {
		st.text_color[i] = options.text_color[i];
		st.text_stroke_color[i] = options.text_stroke_color[i];
	}
	st.text_stroke = options.text_stroke;
	st.text_stroke_width = options.text_stroke_width;
	st.text_force_obs_source = options.text_force_obs_source;
	const Placement p = compute_placement(doc, options.placement);

	// Public scene (NOT private): obs_scene_create() inserts the scene source into
	// libobs' public source list, which is what obs_frontend_get_scenes() walks.
	obs_scene_t *scene = obs_scene_create(scene_name);
	if (!scene) {
		err = "obs_scene_create failed";
		return false;
	}

	bool ok = true;
	size_t i = 0;
	while (i < doc.layers.size()) {
		if (is_group(doc.layers[i])) {
			const size_t end = group_end(doc, i);
			if (!add_group(st, scene, doc, i, end, p, nullptr)) {
				ok = false;
				break;
			}
			i = end;
		} else {
			if (!add_layer(st, scene, doc.layers[i], p)) {
				ok = false;
				break;
			}
			++i;
		}
	}

	if (!ok) {
		err = st.err.empty() ? "failed to build scene" : st.err;
		for (obs_source_t *s : st.images)
			obs_source_release(s);
		obs_scene_release(scene); // removes the scene source (not yet published)
		return false;
	}

	// image_source uploads its texture on the graphics thread; wait for it so
	// callers can rely on get_width()/get_height() right after this returns.
	if (options.image_load_timeout_ms > 0) {
		if (!wait_scene_images_loaded(scene, options.image_load_timeout_ms)) {
			err = "timed out waiting for layer images to load";
			for (obs_source_t *s : st.images)
				obs_source_release(s);
			obs_scene_release(scene);
			return false;
		}
	}

	g_missing_fonts = st.missing_fonts;

	// The scene items hold their own references; drop ours.
	for (obs_source_t *s : st.images)
		obs_source_release(s);

	blog(LOG_INFO, "[obs-psd] built scene '%s' from %zu PSD layer(s), %s placement, fit %.3f", scene_name,
	     doc.layers.size(), options.placement == SceneBuildOptions::Place_FitCanvas ? "fit-canvas" : "doc-pixels",
	     p.fit);

	// Reference bookkeeping: obs_scene_create() returns 1 scene reference and
	// the scene source is additionally registered in libobs' public source list
	// (core-owned).  If the caller wants the source, publish the scene pointer
	// under scene_out BEFORE releasing the scene reference: releasing the last
	// scene reference would destroy exactly the source we just handed out.
	// When scene_out is null nothing is handed out and the scene lives on as a
	// public source, i.e. it shows up in obs_frontend_get_scenes().
	if (scene_out)
		*scene_out = obs_scene_get_source(scene);
	obs_scene_release(scene);
	return true;
}

bool wait_scene_images_loaded(obs_scene_t *scene, int timeout_ms)
{
	if (!scene)
		return false;

	std::vector<obs_source_t *> images;
	obs_scene_enum_items(
		scene,
		[](obs_scene_t *, obs_sceneitem_t *item, void *param) -> bool {
			auto *out = static_cast<std::vector<obs_source_t *> *>(param);
			obs_source_t *src = obs_sceneitem_get_source(item);
			const char *id = src ? obs_source_get_id(src) : nullptr;
			if (id && std::strcmp(id, "image_source") == 0)
				out->push_back(obs_source_get_ref(src)); // own ref while polling
			return true;
		},
		&images);

	auto all_loaded = [&images]() {
		for (obs_source_t *s : images)
			if (!s || obs_source_removed(s) || obs_source_get_width(s) == 0 || obs_source_get_height(s) == 0)
				return false;
		return true;
	};

	const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
	bool ok = true;
	for (;;) {
		if (all_loaded())
			break;
		if (std::chrono::steady_clock::now() >= deadline) {
			ok = false;
			break;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(20));
	}

	for (obs_source_t *s : images)
		obs_source_release(s);
	return ok;
}

bool wait_scene_images_loaded(obs_source_t *scene_source, int timeout_ms)
{
	if (!scene_source)
		return false;
	obs_scene_t *scene = obs_scene_from_source(scene_source);
	if (!scene)
		return false;
	return wait_scene_images_loaded(scene, timeout_ms);
}

} // namespace obspsd
