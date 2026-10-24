// SPDX-License-Identifier: GPL-2.0-or-later
//
// See selftest.h.  Compiled only when OBS_PSD_SELFTEST is defined.
#include "obs/selftest.h"

#include "obs/obs_include.h"
#include "obs/scene_build.h"
#include "obs/scene_export.h"
#include "obs/source_render.h"
#include "psd/psd.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <string>
#include <vector>

namespace obspsd {
namespace {

std::ofstream *g_report = nullptr;

void report(const char *fmt, ...)
{
	char buf[2048];
	va_list args;
	va_start(args, fmt);
	vsnprintf(buf, sizeof buf, fmt, args);
	va_end(args);
	blog(LOG_INFO, "[obs-psd][selftest] %s", buf);
	if (g_report)
		*g_report << buf << "\n";
}

size_t count_scene_items(obs_scene_t *scene);

bool count_cb(obs_scene_t *scene, obs_sceneitem_t *item, void *param)
{
	size_t *n = static_cast<size_t *>(param);
	++*n;
	obs_source_t *src = obs_sceneitem_get_source(item);
	if (src && obs_source_get_type(src) == OBS_SOURCE_TYPE_SCENE) {
		obs_scene_t *inner = obs_scene_from_source(src);
		if (inner)
			*n += count_scene_items(inner);
	}
	return true;
}

size_t count_scene_items(obs_scene_t *scene)
{
	size_t n = 0;
	obs_scene_enum_items(scene, count_cb, &n);
	return n;
}

void write_missing_fonts()
{
	const std::vector<MissingFont> missing = missing_fonts();
	report("missing fonts: %zu", missing.size());
	for (const MissingFont &f : missing)
		report("  layer '%s' wanted '%s' used '%s'", f.layer.c_str(), f.wanted.c_str(), f.used.c_str());
}

// The self-test creates a real scene; drop scenes left behind by earlier runs so
// repeated tests do not accumulate in the user's scene collection.
void remove_old_selftest_scenes()
{
	struct obs_frontend_source_list list = {};
	obs_frontend_get_scenes(&list);
	for (size_t i = 0; i < list.sources.num; ++i) {
		obs_source_t *s = list.sources.array[i];
		const char *name = s ? obs_source_get_name(s) : nullptr;
		if (name && std::strncmp(name, "obs-psd selftest", 16) == 0) {
			report("removing scene left by an earlier run: '%s'", name);
			obs_source_remove(s);
		}
	}
	obs_frontend_source_list_free(&list);
}

} // namespace

bool selftest_requested()
{
	const char *p = std::getenv("OBS_PSD_SELFTEST");
	return p && *p;
}

void run_selftest()
{
	const char *input = std::getenv("OBS_PSD_SELFTEST");
	const char *output = std::getenv("OBS_PSD_SELFTEST_OUT");
	const char *report_path = std::getenv("OBS_PSD_SELFTEST_REPORT");

	if (report_path && *report_path)
		g_report = new std::ofstream(report_path, std::ios::out | std::ios::trunc);

	report("=== obs-psd selftest ===");
	report("input: %s", input);

	remove_old_selftest_scenes();

	psd::Document doc;
	std::string err;
	if (!psd::read_file(input, doc, err)) {
		report("FAIL read: %s", err.c_str());
		goto done;
	}
	{
		int text_layers = 0;
		for (const auto &l : doc.layers)
			if (!l.text.empty())
				++text_layers;
		report("parsed: %d x %d, %zu layer(s), %d text layer(s)", doc.width, doc.height, doc.layers.size(),
		       text_layers);
	}

	{
		SceneBuildOptions opts;
		opts.placement = SceneBuildOptions::Place_FitCanvas;
		opts.image_load_timeout_ms = 15000;
		std::string build_err;
		obs_source_t *scene_src = nullptr;
		const bool ok = build_scene_from_psd(doc, "obs-psd selftest", build_err, &scene_src, opts);
		if (!ok) {
			report("FAIL build: %s", build_err.c_str());
			goto done;
		}
		report("build: ok, scene '%s'", obs_source_get_name(scene_src));
		report("scene items (incl. groups): %zu", count_scene_items(obs_scene_from_source(scene_src)));

		const std::vector<std::string> files = generated_image_files();
		unsigned long long bytes = 0;
		for (const std::string &f : files) {
			FILE *fp = fopen(f.c_str(), "rb");
			if (!fp)
				continue;
			fseek(fp, 0, SEEK_END);
			bytes += (unsigned long long)ftell(fp);
			fclose(fp);
		}
		report("layer PNGs: %zu file(s), %.2f MB", files.size(), double(bytes) / (1024.0 * 1024.0));
		if (!files.empty())
			report("first PNG: %s", files.front().c_str());

		write_missing_fonts();

		report("on graphics thread: %d", int(on_graphics_thread()));

		if (output && *output) {
			ExportOptions eo;
			ExportResult er;
			const bool eok = export_scene_to_psd(obs_scene_from_source(scene_src), output, eo, er);
			report("export: %s (%zu exported, %zu skipped) -> %s", eok ? "ok" : "FAIL", er.exported, er.skipped,
			       output);
			if (!eok)
				report("export error: %s", er.error.c_str());
			for (const auto &it : er.items)
				report("  item '%s' [%s]%s%s", it.name.c_str(), it.type.c_str(), it.skipped ? " SKIPPED" : "",
				       it.skipped ? (": " + it.reason).c_str() : "");
		}

		// Mirror what the import dialog does: make the new scene current so the
		// frontend adopts it before the collection is written.
		obs_frontend_set_current_scene(scene_src);

		{
			struct obs_frontend_source_list list = {};
			obs_frontend_get_scenes(&list);
			report("frontend scenes after import: %zu", list.sources.num);
			for (size_t i = 0; i < list.sources.num; ++i) {
				obs_source_t *s = list.sources.array[i];
				report("  scene[%zu] '%s'", i, s ? obs_source_get_name(s) : "(null)");
			}
			obs_frontend_source_list_free(&list);
		}

		obs_frontend_save();
		report("scene collection '%s' saved", obs_frontend_get_current_scene_collection());
		obs_source_release(scene_src);
	}

done:
	report("=== selftest done ===");
	if (g_report) {
		g_report->flush();
		g_report->close();
		delete g_report;
		g_report = nullptr;
	}
}

} // namespace obspsd
