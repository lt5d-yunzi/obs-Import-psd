// SPDX-License-Identifier: GPL-2.0-or-later
//
// obs_psd_text: an editable OBS text source for PSD type layers.
//
// Why this exists: OBS' own text_ft2_source_v2 renders nothing (0x0) on macOS
// for fonts reported through CoreText (see README, "未修复的 Bug"), so a PSD
// import cannot hand type layers to it. This source rasterises through psd::
// render_text_layer() — the same CoreText path used for the bitmap import — and
// uploads the result as a texture, while exposing text, font, size, colour,
// stroke and alignment in the source properties so the layer stays editable.
#pragma once

#include <string>
#include <vector>

struct obs_source;

namespace obspsd {

// Registers the source type. Call from obs_module_load().
void register_psd_text_source();

// The source type id, also used as the "unversioned" id in scene files.
constexpr const char *kPsdTextSourceId = "obs_psd_text_source";

// Document offset stored on a text source by the PSD import. Returns false when
// `source` is not one of ours.
bool psd_text_source_offset(struct obs_source *source, int &x, int &y, int *pad = nullptr);

// Installed font families that can render CJK, for the source's font dropdown.
std::vector<std::string> psd_text_font_families();

} // namespace obspsd
