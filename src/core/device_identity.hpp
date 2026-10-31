// Bluetooth LE device identity helpers.
//
// A saved sensor is keyed by its Bluetooth identity address and address type, e.g.
// "ble:public:A0:9E:1A:12:34:56". For unpaired devices Windows derives its own
// DeviceInformation Id from exactly this address, and for bonded devices that use
// resolvable private addresses Windows reports the resolved identity address, so the
// key stays stable across restarts. The Windows device id is kept as extra metadata.
// Devices are never matched by display name.
#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace blehr {

enum class AddressType { Public, Random };

struct DeviceIdentity {
	uint64_t address = 0; // 48-bit address in the low bits.
	AddressType addressType = AddressType::Public;
	std::string windowsDeviceId; // Optional, OS provided (BluetoothLEDevice::DeviceId).

	std::string key() const;
	bool sameDevice(const DeviceIdentity &other) const
	{
		return address == other.address && addressType == other.addressType;
	}
};

std::string formatAddress(uint64_t address);
std::optional<uint64_t> parseAddress(const std::string &text);
std::optional<DeviceIdentity> identityFromKey(const std::string &key);

} // namespace blehr
