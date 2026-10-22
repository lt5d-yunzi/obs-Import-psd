// SPDX-License-Identifier: GPL-2.0-or-later
//
// Scene -> PSD export.
//
// Walks an OBS scene (recursively through scene groups), renders every scene
// item into an RGBA8 layer and writes one PSD whose layer order matches the OBS
// "Sources" list order bottom-most first -- i.e. identical to how Photoshop
// stacks layers (topmost OBS item = topmost PSD layer).
#pragma once

#include "psd/psd.h"

#include <string>
#include <vector>

struct obs_scene;
typedef struct obs_scene obs_scene_t;

namespace obspsd {

struct ExportOptions {
    // Output document size in pixels. 0 = use the OBS canvas resolution.
    int32_t width = 0;
    int32_t height = 0;
    // Extra source ids (comma separated) to treat as live capture and skip.
    std::string extra_ignore_ids;

    // When true every skipped source is listed in the report.
    bool report_skipped = true;

    // 1.0 = document pixels map 1:1 onto the canvas.
    double scale = 1.0;

    // Fails the export instead of silently producing an empty layer when a
    // source renders nothing.
    bool fail_on_empty_layer = false;
};

struct ExportItemReport {
    std::string name;
    std::string type;
    bool skipped = false;
    std::string reason;
};

struct ExportResult {
    bool ok = false;
    std::string error;
    std::vector<ExportItemReport> items;
    size_t exported = 0;
    size_t skipped = 0;
};

// Renders `scene` into `doc` (which gets width/height set).
// Threading: safe to call from the Qt main (UI) thread; every source render is
// marshalled to the graphics thread by render_source_to_rgba_blocking().
bool scene_to_document(obs_scene_t *scene, const ExportOptions &opts, psd::Document &doc, ExportResult &result);

// Convenience: build the document and write it to `path`.
bool export_scene_to_psd(obs_scene_t *scene, const std::string &path, const ExportOptions &opts,
                         ExportResult &result);

} // namespace obspsd
