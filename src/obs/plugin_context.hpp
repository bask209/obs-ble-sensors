// Process-wide plugin state: the saved-device registry and the shared BLE manager.
#pragma once

#include "core/ble_manager.hpp"
#include "core/device_registry.hpp"

#include <memory>

namespace blehr::plugin {

void initialize(); // obs_module_load
void shutdown();   // OBS exit / obs_module_unload. Idempotent.

DeviceRegistry &registry();
// nullptr once shut down. Callers keep the returned pointer only briefly.
std::shared_ptr<BleManager> manager();

bool devModeActive();

} // namespace blehr::plugin
