#pragma once

#include <string>

namespace blehr {

// Opens the modal "Scan / Manage Sensors" dialog (UI thread only). Returns true when the
// user picked a sensor; `chosenKey` then holds its key (may be "" for "no sensor").
bool runSensorDialog(const std::string &currentKey, std::string &chosenKey);

} // namespace blehr
