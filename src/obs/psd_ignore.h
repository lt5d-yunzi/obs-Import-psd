// SPDX-License-Identifier: GPL-2.0-or-later
//
// Decides which scene items are skipped when exporting a scene to PSD.
//
// Rules (agreed behaviour):
//   * Only sources that represent live capture are ignored: browser sources,
//     video / media sources and OS capture (screen, window, game, display).
//   * Images and text layers are always exported.
//   * A source can be forced to be ignored / kept with the marker tokens in its
//     layer name:  "[skip]" / "[ignore]" -> always ignored,
//                 "[keep]"              -> always kept.
//   * The user-configurable id list lives in the source settings.
#pragma once

#include <string>
#include <vector>


namespace obspsd {

// Comma separated extra ids, e.g. "vlc_source,gstreamer-input"
struct IgnoreRules {
    std::string extra_ids;

    bool any() const { return !extra_ids.empty(); }
};

// Returns true when the source will NOT be exported.
bool source_should_be_ignored(obs_source_t *source, const IgnoreRules &rules, const std::string &layer_name);

// Human readable reason (for logs), empty when not ignored.
std::string ignore_reason(obs_source_t *source, const IgnoreRules &rules, const std::string &layer_name);

// Built-in capture / media ids used by the rules above.
const std::vector<std::string> &builtin_ignored_ids();

// Type-name shown in the export report, e.g. "Window Capture (ScreenCaptureKit)".
std::string source_type_label(obs_source_t *source);

} // namespace obspsd
