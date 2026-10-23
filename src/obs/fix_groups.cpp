// SPDX-License-Identifier: GPL-2.0-or-later
//
// Diagnostic + repair action for imported PSD scenes.
//
// OBS anchors a group scene's coordinate system at the group item's transform
// and re-derives that transform from the members' bounding box whenever items
// are added to the group. Layers inside a group are therefore positioned
// relative to the group, not relative to the canvas.
//
// `fix_group_offsets()` walks the current scene and pins every group item to
// the canvas origin with unit scale, which makes member coordinates absolute
// again. It logs what it changed so the effect is verifiable.
#include "obs/fix_groups.h"

#include <obs-frontend-api.h>

#include <cstring>

#include <graphics/vec2.h>

namespace {

struct FixState {
	int groups_seen = 0;
	int groups_fixed = 0;
};

bool fix_item(obs_scene_t *scene, obs_sceneitem_t *item, void *param);

// Recurses into a group's inner scene.
void fix_group_inner(obs_sceneitem_t *group_item, FixState &st)
{
	obs_scene_t *inner = obs_sceneitem_group_get_scene(group_item);
	if (!inner)
		return;
	obs_scene_enum_items(inner, [](obs_scene_t *, obs_sceneitem_t *it, void *p) -> bool {
		fix_item(obs_sceneitem_get_scene(it), it, p);
		return true;
	}, &st);
}

bool fix_item(obs_scene_t *scene, obs_sceneitem_t *item, void *param)
{
	if (!item)
		return true;
	FixState &st = *static_cast<FixState *>(param);

	obs_source_t *src = obs_sceneitem_get_source(item);
	if (!src)
		return true;

	const char *id = obs_source_get_id(src);
	if (id && std::strcmp(id, "group") == 0) {
		++st.groups_seen;

		struct obs_transform_info before = {};
		obs_sceneitem_get_info2(item, &before);

		// Pin the group to the canvas origin so its inner coordinate system
		// matches the document/canvas coordinates its members were placed with.
		obs_sceneitem_set_alignment(item, OBS_ALIGN_LEFT | OBS_ALIGN_TOP);
		obs_sceneitem_set_bounds_type(item, OBS_BOUNDS_NONE);
		obs_sceneitem_set_bounds_alignment(item, OBS_ALIGN_LEFT | OBS_ALIGN_TOP);
		struct vec2 zero;
		vec2_set(&zero, 0.0f, 0.0f);
		obs_sceneitem_set_pos(item, &zero);
		struct vec2 one;
		vec2_set(&one, 1.0f, 1.0f);
		obs_sceneitem_set_scale(item, &one);

		struct obs_transform_info after = {};
		obs_sceneitem_get_info2(item, &after);

		blog(LOG_INFO,
		     "[obs-psd] fix: group source '%s' pos (%.2f,%.2f) -> (%.2f,%.2f), scale (%.3f,%.3f) -> (%.3f,%.3f)",
		     obs_source_get_name(src), before.pos.x, before.pos.y, after.pos.x, after.pos.y, before.scale.x,
		     before.scale.y, after.scale.x, after.scale.y);

		if (before.pos.x != after.pos.x || before.pos.y != after.pos.y)
			++st.groups_fixed;

		fix_group_inner(item, st);
	}

	(void)scene;
	return true;
}

} // namespace

namespace obspsd {

void fix_group_offsets()
{
	obs_source_t *scene_src = obs_frontend_get_current_scene();
	if (!scene_src) {
		blog(LOG_WARNING, "[obs-psd] fix: no active scene");
		return;
	}
	obs_scene_t *scene = obs_scene_from_source(scene_src);
	if (!scene) {
		blog(LOG_WARNING, "[obs-psd] fix: current source is not a scene");
		obs_source_release(scene_src);
		return;
	}

	FixState st;
	obs_scene_enum_items(scene, fix_item, &st);

	blog(LOG_INFO, "[obs-psd] fix: %d group(s) seen, %d moved back to the origin", st.groups_seen,
	     st.groups_fixed);
	obs_source_release(scene_src);
}

} // namespace obspsd
