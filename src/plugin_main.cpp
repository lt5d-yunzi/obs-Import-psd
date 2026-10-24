// SPDX-License-Identifier: GPL-2.0-or-later
//
// obs-psd : PSD (Photoshop Document) import / export for OBS Studio.
//
// Import  : PSD layers -> one OBS scene, bottom-most first, exactly like the
//           layer order in Photoshop.
// Export  : OBS scene -> PSD, one layer per scene item (bottom-most first).
//           Live-capture sources (browser, video, screen/window/device capture)
//           are skipped unless their layer name carries a "[keep]" marker.
//           Image and text layers are always exported.
#include "obs/obs_include.h"

#include "obs/psd_ignore.h"
#include "psd/psd.h"
#include "ui/psd_ui.h"

#ifdef OBS_PSD_SELFTEST
#include "obs/selftest.h"
#include <QTimer>
#endif

#include <string>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("obs-psd", "en-US")

namespace {

void on_import_psd(void *)
{
    obspsd::run_import_dialog();
}

void on_export_psd(void *)
{
    obspsd::run_export_dialog();
}

void on_frontend_event(enum obs_frontend_event event, void *)
{
    if (event == OBS_FRONTEND_EVENT_FINISHED_LOADING) {
        obs_frontend_add_tools_menu_item(obs_module_text("tools_menu.import"), on_import_psd, nullptr);
        obs_frontend_add_tools_menu_item(obs_module_text("tools_menu.export"), on_export_psd, nullptr);
        blog(LOG_INFO, "[obs-psd] Tools menu: '%s', '%s'", obs_module_text("tools_menu.import"),
             obs_module_text("tools_menu.export"));
#ifdef OBS_PSD_SELFTEST
        if (obspsd::selftest_requested()) {
            // Let OBS finish its start-up (video thread, frontend event loop)
            // before touching graphics from the UI thread.
            QTimer::singleShot(3000, []() { obspsd::run_selftest(); });
        }
#endif
    }
}

} // namespace

bool obs_module_load(void)
{
    blog(LOG_INFO, "[obs-psd] loaded (PSD import/export)");
    obs_frontend_add_event_callback(on_frontend_event, nullptr);
    return true;
}

void obs_module_unload(void)
{
    obs_frontend_remove_event_callback(on_frontend_event, nullptr);
    blog(LOG_INFO, "[obs-psd] unloaded");
}
