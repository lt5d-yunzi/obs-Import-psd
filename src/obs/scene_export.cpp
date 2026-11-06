// SPDX-License-Identifier: GPL-2.0-or-later
#include "scene_export.h"

#include "obs_include.h"
#include "psd_ignore.h"
#include "source_render.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace obspsd {

namespace {

struct Collected {
    obs_sceneitem_t *item;
    bool in_group;
};

// Box transform of an item in scene space, without the scene->canvas scale.
void item_box(obs_sceneitem_t *item, const struct vec2 &base_size, const struct vec2 &parent_scale,
              float &x0, float &y0, float &x1, float &y1)
{
    struct vec2 pos;
    obs_sceneitem_get_pos(item, &pos);
    struct vec2 scale;
    obs_sceneitem_get_scale(item, &scale);
    const float rot = obs_sceneitem_get_rot(item);
    const uint32_t align = obs_sceneitem_get_alignment(item);

    const float w = base_size.x * scale.x;
    const float h = base_size.y * scale.y;

    // OBS aligns the unscaled box; compute the top-left corner accordingly.
    float left = pos.x;
    float top = pos.y;
    if (align & OBS_ALIGN_LEFT)
        /* left stays */;
    else if (align & OBS_ALIGN_RIGHT)
        left = pos.x - w;
    else
        left = pos.x - w * 0.5f;

    if (align & OBS_ALIGN_TOP)
        /* top stays */;
    else if (align & OBS_ALIGN_BOTTOM)
        top = pos.y - h;
    else
        top = pos.y - h * 0.5f;

    // Rotation happens around the alignment anchor; we approximate the
    // bounding box after rotation so the exported layer keeps its footprint.
    if (std::fabs(rot) > 0.0001f) {
        const double rad = double(rot) * 3.14159265358979323846 / 180.0;
        const double cx = left + w * 0.5;
        const double cy = top + h * 0.5;
        const double hw = w * 0.5, hh = h * 0.5;
        const double c = std::fabs(std::cos(rad)), s = std::fabs(std::sin(rad));
        const double nw = hw * c + hh * s;
        const double nh = hw * s + hh * c;
        x0 = float(cx - nw);
        y0 = float(cy - nh);
        x1 = float(cx + nw);
        y1 = float(cy + nh);
    } else {
        x0 = left;
        y0 = top;
        x1 = left + w;
        y1 = top + h;
    }
    x0 *= parent_scale.x;
    x1 *= parent_scale.x;
    y0 *= parent_scale.y;
    y1 *= parent_scale.y;
}

// Collects scene items in "bottom-most first" order, recursing into groups.
void collect_items(obs_scene_t *scene, bool in_group,
                   const struct vec2 &parent_scale, const struct vec2 &parent_offset, std::vector<Collected> &out,
                   int depth)
{
    if (depth > 8)
        return;
    std::vector<obs_sceneitem_t *> items;
    obs_scene_enum_items(
        scene,
        [](obs_scene_t *, obs_sceneitem_t *it, void *param) {
            auto *v = static_cast<std::vector<obs_sceneitem_t *> *>(param);
            obs_sceneitem_addref(it);
            v->push_back(it);
            return true;
        },
        &items);
    for (obs_sceneitem_t *it : items) {
        obs_scene_t *group = obs_sceneitem_group_get_scene(it);
        if (group) {
            collect_items(group, true, parent_scale, parent_offset, out, depth + 1);
        } else {
            out.push_back({it, in_group});
        }
    }
    for (obs_sceneitem_t *it : items)
        obs_sceneitem_release(it);
}

} // namespace

bool scene_to_document(obs_scene_t *scene, const ExportOptions &opts, psd::Document &doc, ExportResult &result)
{
    result = ExportResult{};
    if (!scene) {
        result.error = "no scene";
        return false;
    }

    int32_t W = opts.width, H = opts.height;
    if (W <= 0 || H <= 0) {
        struct obs_video_info ovi;
        if (obs_get_video_info(&ovi)) {
            W = int32_t(ovi.base_width);
            H = int32_t(ovi.base_height);
        } else {
            W = 1920;
            H = 1080;
        }
    }
    if (W <= 0 || H <= 0) {
        result.error = "invalid output size";
        return false;
    }

    IgnoreRules rules;
    rules.extra_ids = opts.extra_ignore_ids;

    struct vec2 parent_scale = {float(opts.scale), float(opts.scale)};
    struct vec2 parent_offset = {0.0f, 0.0f};

    std::vector<Collected> collected;
    collect_items(scene, false, parent_scale, parent_offset, collected, 0);

    doc = psd::Document{};
    doc.width = W;
    doc.height = H;

    for (const Collected &c : collected) {
        obs_sceneitem_t *item = c.item;
        obs_source_t *src = obs_sceneitem_get_source(item);
        if (!src)
            continue;
        const char *raw_name = obs_source_get_name(src);
        const std::string name = raw_name ? raw_name : "";
        ExportItemReport rep;
        rep.name = name;
        rep.type = source_type_label(src);

        const bool hidden = !obs_sceneitem_visible(item);
        const std::string reason = ignore_reason(src, rules, name);
        // Hidden items are exported as invisible layers (keeps the document
        // faithful); skipped capture sources are dropped entirely.
        const bool skip = !reason.empty() && !hidden;
        if (skip) {
            rep.skipped = true;
            rep.reason = reason;
            ++result.skipped;
            if (opts.report_skipped)
                result.items.push_back(rep);
            continue;
        }

        uint32_t sw = obs_source_get_width(src);
        uint32_t sh = obs_source_get_height(src);
        if (sw == 0 || sh == 0) {
            rep.skipped = true;
            rep.reason = "source has no video output";
            ++result.skipped;
            if (opts.report_skipped)
                result.items.push_back(rep);
            continue;
        }

        struct vec2 base_size = {float(sw), float(sh)};
        float x0, y0, x1, y1;
        item_box(item, base_size, parent_scale, x0, y0, x1, y1);

        int32_t left = int32_t(std::floor(x0 - 0.5f));
        int32_t top = int32_t(std::floor(y0 - 0.5f));
        int32_t right = int32_t(std::ceil(x1 - 0.5f));
        int32_t bottom = int32_t(std::ceil(y1 - 0.5f));
        left = std::max(left, 0);
        top = std::max(top, 0);
        right = std::min(right, W);
        bottom = std::min(bottom, H);

        psd::Layer layer;
        layer.name = name.empty() ? std::string("Layer") : name;
        layer.visible = !hidden;
        layer.opacity = 100.0f;

        if (right <= left || bottom <= top) {
            // Fully outside the canvas: keep the layer recorded but empty.
            layer.left = layer.top = layer.right = layer.bottom = 0;
            doc.layers.push_back(std::move(layer));
            rep.reason = "outside canvas";
            ++result.exported;
            result.items.push_back(rep);
            continue;
        }

        std::vector<uint8_t> px;
        std::string err;
        int rw = int(sw), rh = int(sh);
        if (!render_source_to_rgba_blocking(src, px, rw, rh, err)) {
            // A graphics-thread timeout means every following item will fail the
            // same way; abort instead of stalling for 10 s per layer.
            if (err.find("timed out") != std::string::npos) {
                result.error = err;
                return false;
            }
            rep.skipped = true;
            rep.reason = err;
            ++result.skipped;
            if (opts.report_skipped)
                result.items.push_back(rep);
            continue;
        }
        if (rw <= 0 || rh <= 0 || px.size() != size_t(rw) * size_t(rh) * 4) {
            rep.skipped = true;
            rep.reason = "unexpected render size";
            ++result.skipped;
            if (opts.report_skipped)
                result.items.push_back(rep);
            continue;
        }
        sw = uint32_t(rw);
        sh = uint32_t(rh);

        // Resample the rendered source into the layer rectangle.
        const int32_t lw = right - left;
        const int32_t lh = bottom - top;
        layer.left = left;
        layer.top = top;
        layer.right = right;
        layer.bottom = bottom;
        layer.rgba.assign(size_t(lw) * size_t(lh) * 4, 0);
        for (int32_t y = 0; y < lh; ++y) {
            const float sy = (top + y + 0.5f - y0) / std::max(1e-3f, (y1 - y0)) * float(sh);
            int32_t iy = std::clamp(int32_t(sy), 0, int32_t(sh) - 1);
            for (int32_t x = 0; x < lw; ++x) {
                const float sx = (left + x + 0.5f - x0) / std::max(1e-3f, (x1 - x0)) * float(sw);
                int32_t ix = std::clamp(int32_t(sx), 0, int32_t(sw) - 1);
                std::memcpy(&layer.rgba[(size_t(y) * size_t(lw) + size_t(x)) * 4],
                            &px[(size_t(iy) * size_t(sw) + size_t(ix)) * 4], 4);
            }
        }

        if (opts.fail_on_empty_layer) {
            bool any = false;
            for (size_t i = 3; i < layer.rgba.size(); i += 4)
                if (layer.rgba[i] != 0) {
                    any = true;
                    break;
                }
            if (!any) {
                result.error = "layer rendered empty: " + layer.name;
                return false;
            }
        }

        doc.layers.push_back(std::move(layer));
        ++result.exported;
        result.items.push_back(rep);
    }

    // Composite (preview) image: simple top-down alpha blend of all layers.
    doc.composite.assign(size_t(W) * size_t(H) * 4, 0);
    for (const psd::Layer &L : doc.layers) {
        if (!L.visible || L.rgba.empty())
            continue;
        for (int32_t y = 0; y < L.height(); ++y) {
            const int32_t dy = L.top + y;
            if (dy < 0 || dy >= H)
                continue;
            for (int32_t x = 0; x < L.width(); ++x) {
                const int32_t dx = L.left + x;
                if (dx < 0 || dx >= W)
                    continue;
                const uint8_t *s = &L.rgba[(size_t(y) * size_t(L.width()) + size_t(x)) * 4];
                uint8_t *d = &doc.composite[(size_t(dy) * size_t(W) + size_t(dx)) * 4];
                const float sa = s[3] / 255.0f;
                if (sa <= 0.0f)
                    continue;
                const float da = d[3] / 255.0f;
                const float oa = sa + da * (1.0f - sa);
                for (int k = 0; k < 3; ++k)
                    d[k] = uint8_t((s[k] * sa + d[k] * da * (1.0f - sa)) / oa + 0.5f);
                d[3] = uint8_t(oa * 255.0f + 0.5f);
            }
        }
    }

    result.ok = true;
    return true;
}

bool export_scene_to_psd(obs_scene_t *scene, const std::string &path, const ExportOptions &opts,
                         ExportResult &result)
{
    psd::Document doc;
    if (!scene_to_document(scene, opts, doc, result))
        return false;
    std::string err;
    if (!psd::write_file(path, doc, err)) {
        result.ok = false;
        result.error = err;
        return false;
    }
    return true;
}

} // namespace obspsd
