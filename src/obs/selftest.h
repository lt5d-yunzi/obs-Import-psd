// SPDX-License-Identifier: GPL-2.0-or-later
//
// Developer self-test hook (built only when OBS_PSD_SELFTEST is defined).
//
// OBS_PSD_SELFTEST=<file.psd> makes the plugin import that document once the
// frontend has finished loading, save the scene collection, and optionally
// export the new scene back to a PSD named by OBS_PSD_SELFTEST_OUT.  A report is
// written to OBS_PSD_SELFTEST_REPORT (plain text).  This exists so the import
// pipeline can be exercised without driving Qt dialogs by hand; it is never
// compiled into a normal release build.
#pragma once

namespace obspsd {

bool selftest_requested();
void run_selftest();

} // namespace obspsd
