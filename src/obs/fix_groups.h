// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

namespace obspsd {

// Pins every group item of the active scene to the canvas origin with unit
// scale, restoring absolute (document) coordinates for the layers inside.
void fix_group_offsets();

} // namespace obspsd
