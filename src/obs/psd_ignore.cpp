// SPDX-License-Identifier: GPL-2.0-or-later
#include "obs_include.h"
#include "psd_ignore.h"

#include <algorithm>
#include <cctype>
#include <cstring>

namespace obspsd {

namespace {

std::string lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return s;
}

std::vector<std::string> split_tokens(const std::string &s)
{
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == ',') {
            if (!cur.empty())
                out.push_back(lower(cur));
            cur.clear();
        } else if (!std::isspace(static_cast<unsigned char>(c))) {
            cur.push_back(c);
        }
    }
    if (!cur.empty())
        out.push_back(lower(cur));
    return out;
}

bool contains_marker(const std::string &name, const char *marker)
{
    return name.find(marker) != std::string::npos;
}

} // namespace

const std::vector<std::string> &builtin_ignored_ids()
{
    static const std::vector<std::string> ids = {
        // Browser
        "browser_source", "browser_source_custom", "obs_browser_source",
        // Video / media playback
        "ffmpeg_source", "vlc_source", "media_source", "gstreamer-source", "gstreamer_source",
        // OS / device capture (macOS + generic)
        "screen_capture", "window_capture", "game_capture", "monitor_capture", "xshm_input", "xcomposite_input",
        "display_capture", "duplicator-monitor-capture", "syphon-input", "syphon", "ndi_source", "obs_ndi_source",
        "av_capture_input", "av_capture_input_v2", "mac-avcapture", "decklink-input", "aja_output",
        "dshow_input", "dshow_output",
        // Virtual / generated live content
        "virtualcam", "webcam",
    };
    return ids;
}

std::string source_type_label(obs_source_t *source)
{
    if (!source)
        return {};
    const char *id = obs_source_get_id(source);
    const char *disp = id ? obs_source_get_display_name(id) : nullptr;
    return disp ? std::string(disp) : std::string(id ? id : "");
}

bool source_should_be_ignored(obs_source_t *source, const IgnoreRules &rules, const std::string &layer_name)
{
    return !ignore_reason(source, rules, layer_name).empty();
}

std::string ignore_reason(obs_source_t *source, const IgnoreRules &rules, const std::string &layer_name)
{
    // Explicit user markers win over everything else.
    const std::string lower_name = lower(layer_name);
    if (contains_marker(lower_name, "[keep]"))
        return {};
    if (contains_marker(lower_name, "[skip]") || contains_marker(lower_name, "[ignore]"))
        return "layer name marker";

    if (!source)
        return "null source";

    const char *id_raw = obs_source_get_id(source);
    const std::string id = id_raw ? lower(id_raw) : std::string();

    // A group or scene is not a capture source; its children are checked.
    if (obs_source_get_type(source) == OBS_SOURCE_TYPE_SCENE)
        return {};

    for (const std::string &ignored : builtin_ignored_ids()) {
        if (id == ignored)
            return "capture/browser/video source: " + id;
    }

    for (const std::string &extra : split_tokens(rules.extra_ids)) {
        if (!extra.empty() && id.find(extra) != std::string::npos)
            return "user rule: " + extra;
    }

    return {};
}

} // namespace obspsd
