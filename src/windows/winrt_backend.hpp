// Windows Bluetooth LE backend built on C++/WinRT (Windows.Devices.Bluetooth). Uses only
// OS APIs; no drivers, helper processes or extra libraries.
#pragma once

#include "core/ble_backend.hpp"

#include <memory>

namespace blehr {

std::unique_ptr<BleBackend> createWinRtBackend();

} // namespace blehr
