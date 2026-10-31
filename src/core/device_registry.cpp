#include "device_registry.hpp"

#include <algorithm>

namespace blehr {

std::string SavedDevice::displayName() const
{
	if (!alias.empty())
		return alias;
	if (!lastKnownName.empty())
		return lastKnownName;
	return "Sensor " + formatAddress(identity.address);
}

void DeviceRegistry::setPersist(PersistFn fn)
{
	std::lock_guard<std::mutex> lock(mutex_);
	persist_ = std::move(fn);
}

void DeviceRegistry::load(std::vector<SavedDevice> devices)
{
	std::lock_guard<std::mutex> lock(mutex_);
	devices_.clear();
	for (auto &d : devices) {
		// Drop duplicates and entries without a usable address.
		if (d.identity.address == 0)
			continue;
		const auto key = d.key();
		if (std::none_of(devices_.begin(), devices_.end(), [&](const SavedDevice &e) { return e.key() == key; }))
			devices_.push_back(std::move(d));
	}
}

std::vector<SavedDevice> DeviceRegistry::list() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	return devices_;
}

std::optional<SavedDevice> DeviceRegistry::find(const std::string &key) const
{
	std::lock_guard<std::mutex> lock(mutex_);
	for (const auto &d : devices_)
		if (d.key() == key)
			return d;
	return std::nullopt;
}

bool DeviceRegistry::save(const SavedDevice &device)
{
	if (device.identity.address == 0)
		return false;
	std::unique_lock<std::mutex> lock(mutex_);
	const auto key = device.key();
	auto it = std::find_if(devices_.begin(), devices_.end(), [&](const SavedDevice &d) { return d.key() == key; });
	if (it == devices_.end()) {
		devices_.push_back(device);
	} else {
		it->alias = device.alias;
		if (!device.lastKnownName.empty())
			it->lastKnownName = device.lastKnownName;
		if (!device.identity.windowsDeviceId.empty())
			it->identity.windowsDeviceId = device.identity.windowsDeviceId;
	}
	return persistLocked(lock);
}

bool DeviceRegistry::rename(const std::string &key, const std::string &alias)
{
	std::unique_lock<std::mutex> lock(mutex_);
	auto it = std::find_if(devices_.begin(), devices_.end(), [&](const SavedDevice &d) { return d.key() == key; });
	if (it == devices_.end())
		return false;
	it->alias = alias;
	return persistLocked(lock);
}

bool DeviceRegistry::forget(const std::string &key)
{
	std::unique_lock<std::mutex> lock(mutex_);
	auto it = std::remove_if(devices_.begin(), devices_.end(), [&](const SavedDevice &d) { return d.key() == key; });
	if (it == devices_.end())
		return false;
	devices_.erase(it, devices_.end());
	return persistLocked(lock);
}

void DeviceRegistry::noteConnected(const std::string &key, int64_t unixTime, const std::string &name,
				   const std::string &windowsDeviceId)
{
	std::unique_lock<std::mutex> lock(mutex_);
	auto it = std::find_if(devices_.begin(), devices_.end(), [&](const SavedDevice &d) { return d.key() == key; });
	if (it == devices_.end())
		return;
	bool changed = false;
	if (!name.empty() && name != it->lastKnownName) {
		it->lastKnownName = name;
		changed = true;
	}
	if (!windowsDeviceId.empty() && windowsDeviceId != it->identity.windowsDeviceId) {
		it->identity.windowsDeviceId = windowsDeviceId;
		changed = true;
	}
	if (unixTime - it->lastConnectedUnix >= 3600)
		changed = true;
	it->lastConnectedUnix = unixTime;
	if (changed)
		persistLocked(lock);
}

bool DeviceRegistry::persistLocked(std::unique_lock<std::mutex> &lock)
{
	// Snapshot under the data lock, then release it BEFORE waiting for the file: readers
	// (e.g. OBS's graphics thread resolving a sensor) must never wait on disk I/O. Writes
	// are ordered by version so an older snapshot can never overwrite a newer one.
	auto copy = devices_;
	auto fn = persist_;
	const uint64_t version = ++version_;
	lock.unlock();

	std::lock_guard<std::mutex> persistLock(persistMutex_);
	if (version <= writtenVersion_)
		return true; // A newer snapshot was already written.
	writtenVersion_ = version;
	return fn ? fn(copy) : true;
}

} // namespace blehr
