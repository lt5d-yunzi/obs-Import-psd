// SPDX-License-Identifier: GPL-2.0-or-later
// obs-psd :: rendering an arbitrary OBS source into a CPU RGBA8 buffer.
//
// See source_render.cpp for the design note (pixel path + threading).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

typedef struct obs_source obs_source_t;

namespace obspsd {

// Renders `src` at its *native* (base) resolution into `rgba` as 8-bit
// straight-alpha RGBA, top row first, tightly packed (w*h*4 bytes).
// Exactly one frame is rendered (no warm-up loop), so for live sources
// (capture cards, windows) the caller is responsible for pacing/retrying.
//
// * `rgba`, `w`, `h` are only written on success.
// * `err` receives a human readable reason on failure (never null-derefs).
// * Returns false for null/removed sources, zero-sized sources, sources
//   without video output, and when the graphics subsystem is not ready.
//
// THREADING: must be called with an active graphics context available, i.e.
// from the graphics thread or with graphics already entered.  For the Qt main
// (UI) thread use `render_source_to_rgba_blocking()` instead, which hops to the
// graphics thread for you.
bool render_source_to_rgba(obs_source_t *src, std::vector<uint8_t> &rgba, int &w, int &h, std::string &err);

// Whether calling render_source_to_rgba() directly on the *current* thread is
// legal (true on the graphics thread, or when called inside an obs_enter_graphics()
// scope on this same thread).  The "inside enter scope" case cannot be probed
// through the public API; it is only detected for the graphics thread itself.
bool on_graphics_thread();

// Thread-safe entry point: hands the request to the graphics thread with
// obs_queue_task(OBS_TASK_GRAPHICS, ..., wait = true) and blocks until the
// render finished (or until OBS shut the graphics thread down).  Safe to call
// from the Qt main (UI) thread, e.g. from a tools-menu action.  Must NOT be
// called from the graphics thread while OBS is tearing down.
bool render_source_to_rgba_blocking(obs_source_t *src, std::vector<uint8_t> &rgba, int &w, int &h, std::string &err);

// Converts premultiplied RGBA8 to straight RGBA8 in place.
//   out = min(255, round(in * 255 / a)), a == 0 -> out RGB = 0.
void unpremultiply_rgba_in_place(uint8_t *pixels, size_t count);

} // namespace obspsd
