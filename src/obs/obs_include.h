// SPDX-License-Identifier: GPL-2.0-or-later
// Central include for the OBS-side code. Keeps the compiler happy about the
// C linkage of OBS headers when compiling C++.
#pragma once

#ifdef __cplusplus
extern "C" {
#endif
#include <obs.h>
#include <obs-module.h>
#include <obs-frontend-api.h>
#include <util/platform.h>
#include <graphics/graphics.h>
#ifdef __cplusplus
}
#endif
