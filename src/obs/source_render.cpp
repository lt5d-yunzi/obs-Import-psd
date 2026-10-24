// SPDX-License-Identifier: GPL-2.0-or-later
//
// obs-psd :: source_render.cpp
//
// =============================================================================
// DESIGN NOTE - pixel path
// =============================================================================
//
// Goal: take ANY obs_source_t* (window/screen capture, video capture device,
// media source, image_source, scenes, groups, browser, text, ...) and turn one
// frame into a CPU buffer of straight-alpha RGBA8 at the source's *native*
// (base) size, independent of
//   - whether the source is inside the current scene, and
//   - OBS's main canvas resolution.
//
// The pixel path is the one OBS's own frontend uses for "source screenshot"
// (frontend/utility/ScreenshotObj.cpp @ 32.x):
//
//   1. cx = obs_source_get_width(src), cy = obs_source_get_height(src)
//      -> obs-source.c get_recurse_width/height: for a source with filters this
//         walks to the first filter and uses its base size, so the size we use
//         is exactly what obs_source_video_render() will composite.
//   2. space = GS_CS_SRGB (tonemap HDR->SDR; formats: gs_get_format_from_space)
//      gs_texrender_create(GS_RGBA, GS_ZS_NONE)
//      gs_stagesurface_create(cx, cy, GS_RGBA)
//   3. gs_texrender_begin_with_color_space(tr, cx, cy, GS_CS_SRGB)
//        merge blend ONE/ZERO, clear to zero, gs_ortho(0,cx,0,cy,-100,100),
//        obs_source_inc_showing(src); obs_source_video_render(src);
//        obs_source_dec_showing(src);
//      gs_texrender_end()
//   4. gs_stage_texture(surf, gs_texrender_get_texture(tr))
//   5. gs_stagesurface_map() -> memcpy rows (honouring linesize) -> unmap
//
// Why obs_source_video_render() and not obs_source_get_frame():
//   * obs_source_video_render() dispatches to the same internal renderer libobs
//     uses for the canvas (libobs render_video()): async sources go through
//     obs_source_update_async_video() + obs_source_render_async_video(), which
//     applies obs_source_frame::format / color_matrix / full_range / trc /
//     flip / async rotation, the GPU YUV->RGB conversion and *all* filters.
//     That is libobs "source_effective_width" behaviour, for free.
//   * obs_source_get_frame() hands out a *private* copy of that same frame
//     (ref-counted, frame->data[] = cached planes) but does NOT apply colour
//     conversion, flip, rotation or filters, and the frame is often one tick
//     behind what obs_source_video_render() shows (async_rendered flag).  Doing
//     YUV by hand would silently diverge from what the user sees.
//   * A raw async frame also can't represent scenes/groups/synchronous sources,
//     so a "fast path" would need a *second*, differently-behaving decoder.
//   Therefore this module has exactly ONE pixel path; the async-frame path is
//   reached transparently inside obs_source_video_render().
//
// Alpha: everything in OBS renders premultiplied (image_source uses blend
// ONE/INVSRCALPHA, async video the same).  With blend ONE/ZERO in the
// texrender the result is the premultiplied colour as-is; we then un-premultiply
// (straight = min(255, round(c*255/a))) so callers get STRAIGHT alpha.
//
// Note on colour: the render target is GS_CS_SRGB, so obs_source_video_render()
// is given an SDR target; libobs' source_render()/async renderer pick the
// matching tonemap/convert technique itself.  We deliberately do not try to
// reproduce HDR-to-SCRGB here (OBS does that only for JXR screenshots).
//
// =============================================================================
// DESIGN NOTE - threading
// =============================================================================
//
// * All graphics calls happen between obs_enter_graphics() and
//   obs_leave_graphics() on the SAME thread, inside render_graphics_thread_.
// * obs_source_video_render() is normally called from the OBS graphics thread
//   (it is what obs_view_render() does); graphics contexts are not re-entrant
//   across threads, so:
//     - on_graphics_thread() == true (obs_in_task_thread(OBS_TASK_GRAPHICS),
//       only true on that thread ever since obs_graphics_thread() sets the
//       thread-local): render inline.
//     - otherwise: obs_queue_task(OBS_TASK_GRAPHICS, task, &job, /*wait*/ true)
//       and block.  This is the legal way to use the GPU from the Qt main (UI)
//       thread (menu callbacks run there).  It also works from OBS's video tick
//       thread (where Qt's frontend normally performs UI actions).
// * OBS_SHUTDOWN: obs_queue_task(OBS_TASK_GRAPHICS) is a no-op when libobs
//   shut down (it requires obs->video.video && graphics), so the waiter's
//   timeout covers that case and returns an error instead of hanging.
// * Resources: texrender+stagesurface are created and destroyed per call
//   (screenshots are rare); no cached GPU objects => no cross-context lifetime
//   issues when the graphics module is reset (obs_reset_video).
// * Stagesurface mapping forces a GPU sync (as in ScreenshotObj::Download),
//   so the data is ready when map() returns.
// =============================================================================

#include "source_render.h"

#include <obs.h>
#include <graphics/graphics.h>
#include <media-io/video-io.h>
#include <util/threading.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

namespace obspsd {

bool on_graphics_thread()
{
	return obs_in_task_thread(OBS_TASK_GRAPHICS);
}

void unpremultiply_rgba_in_place(uint8_t *pixels, size_t count)
{
	if (!pixels)
		return;

	// 256-entry reciprocal table: straight = premult * 255 / alpha.
	// Table is built lazily; benign races only recompute identical values.
	static uint16_t recip[256];
	static std::atomic<bool> ready{false};
	if (!ready.load(std::memory_order_acquire)) {
		// NOTE: a == 0 must be special-cased.  The original loop started at 0
		// and divided by it, which is an integer division-by-zero: on Windows
		// (MSVC) that raised a structured exception inside the graphics thread
		// and killed the queued render task, which then timed out the caller.
		recip[0] = 0;
		for (int a = 1; a < 256; ++a)
			recip[a] = (uint16_t)((255 * 255 + a / 2) / a); // round(255*255/a)
		ready.store(true, std::memory_order_release);
	}

	for (size_t i = 0; i < count; ++i, pixels += 4) {
		const uint8_t a = pixels[3];
		if (a == 255)
			continue;
		if (a == 0) {
			pixels[0] = pixels[1] = pixels[2] = 0;
			continue;
		}
		const uint16_t r = recip[a];
		for (int c = 0; c < 3; ++c) {
			uint32_t v = (uint32_t)pixels[c] * r / 255u;
			pixels[c] = (uint8_t)(v > 255u ? 255u : v);
		}
	}
}

namespace {

struct RenderJob {
	obs_source_t *source = nullptr;
	std::vector<uint8_t> rgba;
	int w = 0;
	int h = 0;
	std::string err;
	bool done = false;

	std::mutex mutex;
	std::condition_variable cv;
};

// Reads the 8-bit RGBA texrender into `out`.  Graphics context must be entered
// by the caller.  Rows are copied honouring the stagesurface linesize.
bool map_texrender(gs_texrender_t *tr, gs_stagesurf_t *surf, const int w, const int h, std::vector<uint8_t> &out,
		   std::string &err)
{
	gs_texture_t *tex = gs_texrender_get_texture(tr);
	if (!tex) {
		err = "texrender produced no texture";
		return false;
	}

	gs_stage_texture(surf, tex);

	uint8_t *data = nullptr;
	uint32_t linesize = 0;
	if (!gs_stagesurface_map(surf, &data, &linesize)) {
		err = "gs_stagesurface_map failed";
		return false;
	}
	if (!data || linesize < (uint32_t)w * 4u) {
		gs_stagesurface_unmap(surf);
		err = "stagesurface returned an invalid layout";
		return false;
	}

	out.resize((size_t)w * (size_t)h * 4u);
	for (int y = 0; y < h; ++y)
		std::memcpy(out.data() + (size_t)y * (size_t)w * 4u, data + (size_t)y * (size_t)linesize,
			    (size_t)w * 4u);

	gs_stagesurface_unmap(surf);
	return true;
}

// The whole pixel path.  Called with the graphics context ALREADY entered by
// the caller (this function itself never enters/leaves).
bool render_graphics_thread_(obs_source_t *src, std::vector<uint8_t> &rgba, int &w, int &h, std::string &err)
{
	if (!src) {
		err = "null source";
		return false;
	}
	if (obs_source_removed(src)) {
		err = "source was removed";
		return false;
	}

	const uint32_t cx = obs_source_get_width(src);
	const uint32_t cy = obs_source_get_height(src);
	if (!cx || !cy) {
		err = "source has no video size (not started yet, or no video output)";
		return false;
	}

	// Same target OBS uses for "source screenshot" (tonemap to SDR).
	const enum gs_color_space space = GS_CS_SRGB;
	const enum gs_color_format format = gs_get_format_from_space(space);

	gs_texrender_t *tr = gs_texrender_create(format, GS_ZS_NONE);
	// Use the *generalised* format for the readback surface: on Metal every
	// 8-bit format maps to MTLPixelFormatRGBA8Unorm anyway, but GS_RGBA is the
	// documented SDR format and round-trips through both GL and Metal.
	gs_stagesurf_t *surf = gs_stagesurface_create(cx, cy, gs_generalize_format(format));
	bool ok = false;

	if (!tr || !surf) {
		err = "could not allocate texrender/stagesurface";
	} else if (!gs_texrender_begin_with_color_space(tr, cx, cy, space)) {
		err = "gs_texrender_begin_with_color_space failed";
	} else {
		struct vec4 zero;
		vec4_zero(&zero);
		gs_clear(GS_CLEAR_COLOR, &zero, 0.0f, 0);

		// The source renders with top-left at (0,0), y down.
		gs_ortho(0.0f, (float)cx, 0.0f, (float)cy, -100.0f, 100.0f);

		gs_blend_state_push();
		gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);

		// Keep the source "showing" for the duration of the render so that
		// sources which tear down on hide() do not deactivate mid-capture.
		obs_source_inc_showing(src);
		obs_source_video_render(src);
		obs_source_dec_showing(src);

		gs_blend_state_pop();
		gs_texrender_end(tr);

		ok = map_texrender(tr, surf, (int)cx, (int)cy, rgba, err);
		}

	if (surf)
		gs_stagesurface_destroy(surf);
	if (tr)
		gs_texrender_destroy(tr);

	if (ok) {
		// OBS composites premultiplied; hand out straight alpha.
		unpremultiply_rgba_in_place(rgba.data(), (size_t)cx * (size_t)cy);
		w = (int)cx;
		h = (int)cy;
	}
	return ok;
}

// obs_queue_task(OBS_TASK_GRAPHICS) callback.
void run_render_job(void *param)
{
	auto *job = static_cast<RenderJob *>(param);
	std::string err;
	std::vector<uint8_t> rgba;
	int w = 0, h = 0;
	bool ok = false;

	obs_enter_graphics();
	ok = render_graphics_thread_(job->source, rgba, w, h, err);
	obs_leave_graphics();

	{
		std::lock_guard<std::mutex> lock(job->mutex);
		job->done = true;
		if (ok) {
			job->rgba = std::move(rgba);
			job->w = w;
			job->h = h;
		} else {
			job->err = err;
		}
	}
	job->cv.notify_all();
}

} // namespace

bool render_source_to_rgba(obs_source_t *src, std::vector<uint8_t> &rgba, int &w, int &h, std::string &err)
{
	err.clear();
	if (!on_graphics_thread()) {
		err = "render_source_to_rgba() must be called on the OBS graphics thread; "
		      "use render_source_to_rgba_blocking() from the UI thread";
		return false;
	}

	obs_enter_graphics();
	const bool ok = render_graphics_thread_(src, rgba, w, h, err);
	obs_leave_graphics();
	return ok;
}

bool render_source_to_rgba_blocking(obs_source_t *src, std::vector<uint8_t> &rgba, int &w, int &h, std::string &err)
{
	err.clear();
	if (!src) {
		err = "null source";
		return false;
	}
	if (obs_source_removed(src)) {
		err = "source was removed";
		return false;
	}

	RenderJob job;
	job.source = obs_source_get_ref(src);
	if (!job.source) {
		err = "could not reference source";
		return false;
	}

	if (on_graphics_thread()) {
		// Already where we need to be: plain graphics-scope render.
		obs_enter_graphics();
		const bool ok = render_graphics_thread_(job.source, rgba, w, h, err);
		obs_leave_graphics();
		obs_source_release(job.source);
		return ok;
	}

	obs_queue_task(OBS_TASK_GRAPHICS, run_render_job, &job, false);

	// If libobs has shut the graphics thread down, obs_queue_task() is a
	// no-op; bound the wait so we report an error instead of hanging.
	bool completed = false;
	{
		std::unique_lock<std::mutex> lock(job.mutex);
		completed = job.cv.wait_for(lock, std::chrono::seconds(10), [&job] { return job.done; });
	}
	obs_source_release(job.source);

	if (!completed) {
		err = "timed out waiting for the OBS graphics thread (OBS shutting down?)";
		return false;
	}
	if (job.rgba.empty()) {
		err = job.err.empty() ? "render failed" : job.err;
		return false;
	}

	rgba = std::move(job.rgba);
	w = job.w;
	h = job.h;
	return true;
}

} // namespace obspsd
