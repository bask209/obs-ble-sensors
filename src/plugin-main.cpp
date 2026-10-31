/*
OBS BLE Sensors
Copyright (C) 2026 bask209

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program. If not, see <https://www.gnu.org/licenses/>
*/

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <plugin-support.h>

#include "obs/heart_rate_source.hpp"
#include "obs/plugin_context.hpp"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")

MODULE_EXPORT const char *obs_module_name(void)
{
	return "OBS BLE Sensors";
}

MODULE_EXPORT const char *obs_module_description(void)
{
	return "Shows live heart rate from standard Bluetooth LE heart-rate sensors as a native source.";
}

static void onFrontendEvent(enum obs_frontend_event event, void *)
{
	// Stop Bluetooth work as early as possible during OBS shutdown, while the UI and all
	// sources still exist. obs_module_unload repeats this (idempotent).
	if (event == OBS_FRONTEND_EVENT_EXIT)
		blehr::plugin::shutdown();
}

bool obs_module_load(void)
{
	blehr::plugin::initialize();
	blehr::registerHeartRateSource();
	obs_frontend_add_event_callback(onFrontendEvent, nullptr);
	obs_log(LOG_INFO, "plugin loaded successfully (version %s)", PLUGIN_VERSION);
	return true;
}

void obs_module_unload(void)
{
	obs_frontend_remove_event_callback(onFrontendEvent, nullptr);
	blehr::plugin::shutdown();
	obs_log(LOG_INFO, "plugin unloaded");
}
