// Global, persisted list of saved sensors. Shared by all sources; thread safe.
// Persistence is injected so the logic stays free of OBS dependencies.
#pragma once

#include "device_identity.hpp"

#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace blehr {

struct SavedDevice {
	DeviceIdentity identity;
	std::string alias;          // Friendly name chosen by the user.
	std::string lastKnownName;  // Advertised name last seen.
	int64_t lastConnectedUnix = 0; // Seconds since epoch of last successful connection.

	std::string key() const { return identity.key(); }
	std::string displayName() const;
};

class DeviceRegistry {
public:
	using PersistFn = std::function<bool(const std::vector<SavedDevice> &)>;

	void setPersist(PersistFn fn);
	void load(std::vector<SavedDevice> devices); // Replaces content without persisting.

	std::vector<SavedDevice> list() const;
	std::optional<SavedDevice> find(const std::string &key) const;

	// Adds or updates a device (alias and name). Returns true if persisted.
	bool save(const SavedDevice &device);
	bool rename(const std::string &key, const std::string &alias);
	bool forget(const std::string &key);
	// Records a successful connection. Only persists when something meaningful changed
	// or the timestamp is older than an hour, to avoid needless disk writes.
	void noteConnected(const std::string &key, int64_t unixTime, const std::string &name,
			   const std::string &windowsDeviceId);

private:
	bool persistLocked(std::unique_lock<std::mutex> &lock);

	mutable std::mutex mutex_;
	std::mutex persistMutex_;
	std::vector<SavedDevice> devices_;
	PersistFn persist_;
};

} // namespace blehr
