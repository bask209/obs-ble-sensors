// Crash-safe persistence of the saved-device registry in the OBS plugin config folder
// (%APPDATA%\obs-studio\plugin_config\obs-ble-sensors\devices.json).
#pragma once

#include "core/device_registry.hpp"

#include <string>
#include <vector>

namespace blehr {

std::string registryFilePath();
std::vector<SavedDevice> loadRegistryFile(const std::string &path);
bool saveRegistryFile(const std::string &path, const std::vector<SavedDevice> &devices);

} // namespace blehr
