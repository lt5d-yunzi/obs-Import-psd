// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

namespace obspsd {

// Opens a file dialog, reads the PSD and creates an OBS scene from its layers.
void run_import_dialog();

// Renders the current scene and writes it to a PSD chosen by the user.
void run_export_dialog();

} // namespace obspsd
