/* This code is subject to the Mozilla Public License, v.2.0. http://mozilla.org/MPL/2.0/. */
#pragma once
#include <windows.h>
#include "settings.h"
namespace cst {
bool editAdvancedSettings(HWND owner, HFONT font, Settings &settings);
bool advancedSettingsSmoke(HWND owner, HFONT font, const Settings &initial = {});
} // namespace cst
