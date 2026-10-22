// SPDX-License-Identifier: GPL-2.0-or-later
//
// obs-psd :: compile_check.cpp
//
// Compile-only / link-only verification of every OBS (libobs + obs-frontend-api)
// API used by src/obs/{source_render,scene_build}.{h,cpp}.
//
// How to verify (macOS arm64, OBS 32.x):
//
//   SYSROOT=/Library/Developer/CommandLineTools/SDKs/MacOSX15.4.sdk
//   clang++ -std=c++17 -O2 -fPIC -dynamiclib -shared-libgcc \
//     -Ithird_party/obs-headers/libobs -Ithird_party/obs-headers/frontend-api -Ithird_party/simde \
//     -isysroot $SYSROOT \
//     -o /tmp/libobs-psd-check.dylib \
//     src/obs/source_render.cpp src/obs/scene_build.cpp src/obs/compile_check.cpp \
//     src/psd/psd_read.cpp src/psd/psd_write.cpp src/psd/rle.cpp \
//     -F/Applications/OBS.app/Contents/Frameworks -framework libobs \
//     /Applications/OBS.app/Contents/Frameworks/obs-frontend-api.dylib \
//     -Xlinker -ld_classic
//
// This translation unit only *references* the symbols (addresses are taken so
// nothing is optimised away); obs_psd_link_check() is never meant to be run.
// The resulting dylib therefore names libobs + obs-frontend-api as dylib
// dependencies and leaves no OBS symbol undefined.
//
// (Kept in one section so --dead_strip / LTO cannot elide the references; the
// function is exported and compiled -O2 without LTO.)
// =============================================================================

#include <obs.h>
#include <obs-frontend-api.h>

#include <graphics/graphics.h>
#include <graphics/bounds.h>
#include <graphics/matrix4.h>
#include <graphics/vec2.h>
#include <graphics/vec3.h>
#include <graphics/vec4.h>
#include <media-io/video-io.h>
#include <media-io/video-frame.h>
#include <util/bmem.h>
#include <util/platform.h>
#include <util/threading.h>
#include <callback/calldata.h>
#include <callback/signal.h>
#include <callback/proc.h>

#include <cstdio>
#include <cstring>

// The modules under test (also exercises their declarations).
#include "source_render.h"
#include "scene_build.h"

namespace {

// One call per API, using the exact signature from the vendored headers.
class ApiSurface {
public:
	// ----- graphics / texrender / stagesurface (libobs, graphics.h) -----
	static void graphics()
	{
		obs_enter_graphics();

		gs_texrender_t *tr = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
		tr = (tr == nullptr) ? gs_texrender_create(gs_get_format_from_space(GS_CS_SRGB), GS_ZS_NONE) : tr;
		const bool begun = gs_texrender_begin_with_color_space(tr, 16, 16, GS_CS_SRGB);
		if (begun) {
			struct vec4 zero;
			vec4_zero(&zero);
			gs_clear(GS_CLEAR_COLOR, &zero, 0.0f, 0);
			gs_ortho(0.0f, 16.0f, 0.0f, 16.0f, -100.0f, 100.0f);
			gs_blend_state_push();
			gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);
			gs_blend_state_pop();
			gs_texrender_end(tr);
		}
		gs_texture_t *tex = gs_texrender_get_texture(tr);
		gs_stagesurf_t *surf = gs_stagesurface_create(16, 16, GS_RGBA);
		if (tex && surf)
			gs_stage_texture(surf, tex);
		uint8_t *data = nullptr;
		uint32_t linesize = 0;
		if (surf && gs_stagesurface_map(surf, &data, &linesize))
			gs_stagesurface_unmap(surf);
		gs_stagesurface_destroy(surf);
		gs_texrender_destroy(tr);

		obs_leave_graphics();
	}

	// ----- source / frame / view (obs.h, obs-source.h) -----
	static void sources()
	{
		obs_source_t *src = obs_get_source_by_name("obs-psd-check");
		if (src) {
			// async-frame inspection path (obs.h: obs_source_get_frame)
			struct obs_source_frame *frame = obs_source_get_frame(src);
			if (frame)
				obs_source_release_frame(src, frame);
			obs_source_release(src);
		}

		(void)obs_source_get_width(nullptr);
		(void)obs_source_get_height(nullptr);
		(void)obs_source_get_base_width(nullptr);
		(void)obs_source_get_base_height(nullptr);
		(void)obs_source_get_color_space(nullptr, 0, nullptr);
		(void)obs_source_get_texcoords_centered(nullptr);
		(void)obs_source_get_output_flags(nullptr);
		(void)obs_source_get_type(nullptr);
		(void)obs_source_get_id(nullptr);
		(void)obs_source_get_name(nullptr);
		(void)obs_source_removed(nullptr);
		(void)obs_source_get_ref(nullptr);
		obs_source_inc_showing(nullptr);
		obs_source_dec_showing(nullptr);
		obs_source_video_render(nullptr);
		obs_source_release(nullptr);
		(void)obs_source_create_private("image_source", "x", nullptr);
		(void)obs_source_create("image_source", "x", nullptr, nullptr);

		obs_view_t *view = obs_view_create();
		obs_view_render(view);
		obs_view_destroy(view);

		(void)obs_in_task_thread(OBS_TASK_GRAPHICS);
		obs_queue_task(OBS_TASK_GRAPHICS, [](void *) {}, nullptr, false);

		struct obs_video_info ovi;
		std::memset(&ovi, 0, sizeof(ovi));
		(void)obs_get_video_info(&ovi);
		obs_canvas_t *canvas = obs_get_main_canvas();
		obs_canvas_release(canvas);
	}

	// ----- scenes (obs.h, obs-scene.h) -----
	static void scenes()
	{
		obs_scene_t *scene = obs_scene_create("obs-psd-check");
		if (!scene)
			return;

		obs_source_t *scene_src = obs_scene_get_source(scene);

		obs_sceneitem_t *grp = obs_scene_add_group(scene, "group");
		obs_scene_t *inner = grp ? obs_sceneitem_group_get_scene(grp) : nullptr;
		(void)inner;
		if (grp)
			obs_sceneitem_set_visible(grp, true);
		if (inner) {
			obs_scene_enum_items(
				inner, [](obs_scene_t *, obs_sceneitem_t *, void *) -> bool { return true; }, nullptr);
		}

		struct obs_transform_info info;
		std::memset(&info, 0, sizeof(info));
		vec2_set(&info.pos, 0.0f, 0.0f);
		vec2_set(&info.scale, 1.0f, 1.0f);
		vec2_set(&info.bounds, 1.0f, 1.0f);
		info.rot = 0.0f;
		info.alignment = OBS_ALIGN_LEFT | OBS_ALIGN_TOP;
		info.bounds_type = OBS_BOUNDS_NONE;
		info.bounds_alignment = OBS_ALIGN_LEFT | OBS_ALIGN_TOP;
		info.crop_to_bounds = false;

		obs_sceneitem_t *item = obs_scene_add(scene, scene_src);
		if (item) {
			obs_sceneitem_set_info2(item, &info);
			obs_sceneitem_get_info2(item, &info);
			obs_sceneitem_set_visible(item, false);
			obs_sceneitem_set_blending_method(item, OBS_BLEND_METHOD_DEFAULT);
			obs_sceneitem_set_blending_mode(item, OBS_BLEND_MULTIPLY);
			obs_sceneitem_set_blending_mode(item, OBS_BLEND_SCREEN);
			obs_sceneitem_set_blending_mode(item, OBS_BLEND_NORMAL);
			(void)obs_sceneitem_get_blending_mode(item);
			(void)obs_sceneitem_visible(item);
			obs_source_t *item_src = obs_sceneitem_get_source(item);
			(void)item_src;
			obs_sceneitem_release(item);
		}

		(void)obs_scene_from_source(scene_src);
		(void)obs_scene_is_group(scene);
		obs_scene_enum_items(
			scene, [](obs_scene_t *, obs_sceneitem_t *, void *) -> bool { return true; }, nullptr);
		obs_scene_release(scene);
	}

	// ----- obs_data (image source settings) -----
	static void data()
	{
		obs_data_t *settings = obs_data_create();
		obs_data_set_string(settings, "file", "/tmp/x.png");
		obs_data_set_bool(settings, "unload", false);
		obs_data_set_bool(settings, "linear_alpha", false);
		(void)obs_data_get_string(settings, "file");
		(void)obs_data_get_bool(settings, "unload");
		obs_data_release(settings);
	}

	// ----- obs_frontend_api.h -----
	static void frontend()
	{
		struct obs_frontend_source_list list;
		std::memset(&list, 0, sizeof(list));
		obs_frontend_get_scenes(&list);
		obs_frontend_source_list_free(&list);

		char **names = obs_frontend_get_scene_names();
		bfree(names);

		obs_source_t *cur = obs_frontend_get_current_scene();
		obs_frontend_set_current_scene(cur);
		obs_source_release(cur);

		obs_frontend_add_tools_menu_item("obs-psd check", [](void *) {}, nullptr);
		obs_frontend_add_event_callback([](enum obs_frontend_event, void *) {}, nullptr);
		obs_frontend_remove_event_callback([](enum obs_frontend_event, void *) {}, nullptr);
	}

	// ----- util / callbacks used around the modules -----
	static void util()
	{
		blog(LOG_INFO, "[obs-psd] compile check");
		(void)os_gettime_ns();
		(void)getenv("TMPDIR");
		(void)bstrdup("x");
		bfree(bstrdup("x"));
		obs_add_tick_callback([](void *, float) {}, nullptr);
		obs_remove_tick_callback([](void *, float) {}, nullptr);
	}
};

} // namespace

// Never executed in production: its only purpose is to make the linker
// resolve every OBS symbol the modules reference, at -O2, without LTO.
extern "C" void obs_psd_link_check(void)
{
	ApiSurface::graphics();
	ApiSurface::sources();
	ApiSurface::scenes();
	ApiSurface::data();
	ApiSurface::frontend();
	ApiSurface::util();

	// The modules' own entry points, called with types that need resolving.
	typedef bool (*RenderFn)(obs_source_t *, std::vector<uint8_t> &, int &, int &, std::string &);
	typedef bool (*BuildFn)(const psd::Document &, const char *, std::string &, obs_source_t **,
				const obspsd::SceneBuildOptions &);
	typedef bool (*WaitFn)(obs_scene_t *, int);

	RenderFn r = &obspsd::render_source_to_rgba;
	RenderFn rb = &obspsd::render_source_to_rgba_blocking;
	BuildFn b = &obspsd::build_scene_from_psd;
	WaitFn w = &obspsd::wait_scene_images_loaded;
	void (*pin)() = &obspsd::keep_generated_image_files_alive;
	void (*unpremul)(uint8_t *, size_t) = &obspsd::unpremultiply_rgba_in_place;
	bool (*on_gt)() = &obspsd::on_graphics_thread;

	std::printf("%p %p %p %p %p %p %p\n", (void *)r, (void *)rb, (void *)b, (void *)w, (void *)pin,
		    (void *)unpremul, (void *)on_gt);
}
