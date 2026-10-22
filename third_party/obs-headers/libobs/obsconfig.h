/* Hand-written replacement for the CMake-generated obsconfig.h so plugins can be
 * built against a stock macOS OBS.app install without a full OBS source tree.
 * Mirrors libobs/obsconfig.h.in with macOS-appropriate values. */
#pragma once

#define OBS_DATA_PATH "data"
#define OBS_PLUGIN_PATH "obs-plugins"
#define OBS_PLUGIN_DESTINATION "obs-plugins"

#define OBS_RELEASE_CANDIDATE 0
#define OBS_BETA 0
