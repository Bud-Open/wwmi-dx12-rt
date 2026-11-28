/*
 * Copyright (C) 2014 Patrick Mours
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * WWMI-DX12-RT offline host override: the paired in-game ReShade host must
 * never initiate network requests (WinINet / GitHub checks belong exclusively
 * in the out-of-game Setup utility).
 */

#include "runtime.hpp"

unsigned int reshade::runtime::s_latest_version[3] = {};

void reshade::runtime::check_for_update()
{
	// Intentionally disabled to guarantee zero networking inside the game process.
}
