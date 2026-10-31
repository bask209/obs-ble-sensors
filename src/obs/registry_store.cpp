#include "registry_store.hpp"

#include <obs-module.h>
#include <util/platform.h>

namespace blehr {

namespace {
constexpr int kFormatVersion = 1;
constexpr int64_t kMaxFileBytes = 1024 * 1024;
}

std::string registryFilePath()
{
	char *dir = obs_module_config_path("");
	if (dir) {
		os_mkdirs(dir);
		bfree(dir);
	}
	char *file = obs_module_config_path("devices.json");
	std::string path = file ? file : "devices.json";
	bfree(file);
	return path;
}

std::vector<SavedDevice> loadRegistryFile(const std::string &path)
{
	std::vector<SavedDevice> devices;
	// The file is small by design; refuse anything implausibly large (corruption or
	// tampering) instead of parsing it into memory.
	for (const std::string &candidate : {path, path + ".bak"}) {
		const int64_t size = os_get_file_size(candidate.c_str());
		if (size > kMaxFileBytes) {
			blog(LOG_WARNING, "[obs-ble-sensors] %s is too large (%lld bytes); ignoring the saved sensor list",
			     candidate.c_str(), (long long)size);
			return devices;
		}
	}
	// Falls back to the ".bak" copy written by obs_data_save_json_safe if the main file
	// is missing or corrupt.
	obs_data_t *root = obs_data_create_from_json_file_safe(path.c_str(), "bak");
	if (!root)
		return devices;

	obs_data_array_t *array = obs_data_get_array(root, "devices");
	const size_t count = array ? obs_data_array_count(array) : 0;
	for (size_t i = 0; i < count; ++i) {
		obs_data_t *item = obs_data_array_item(array, i);
		auto identity = identityFromKey(obs_data_get_string(item, "key"));
		if (identity) {
			SavedDevice d;
			d.identity = *identity;
			d.identity.windowsDeviceId = obs_data_get_string(item, "windows_device_id");
			d.alias = obs_data_get_string(item, "alias");
			d.lastKnownName = obs_data_get_string(item, "last_name");
			d.lastConnectedUnix = obs_data_get_int(item, "last_connected");
			devices.push_back(std::move(d));
		}
		obs_data_release(item);
	}
	obs_data_array_release(array);
	obs_data_release(root);
	return devices;
}

bool saveRegistryFile(const std::string &path, const std::vector<SavedDevice> &devices)
{
	obs_data_t *root = obs_data_create();
	obs_data_set_int(root, "version", kFormatVersion);
	obs_data_array_t *array = obs_data_array_create();
	for (const auto &d : devices) {
		obs_data_t *item = obs_data_create();
		obs_data_set_string(item, "key", d.key().c_str());
		obs_data_set_string(item, "address", formatAddress(d.identity.address).c_str());
		obs_data_set_string(item, "address_type",
				    d.identity.addressType == AddressType::Random ? "random" : "public");
		obs_data_set_string(item, "windows_device_id", d.identity.windowsDeviceId.c_str());
		obs_data_set_string(item, "alias", d.alias.c_str());
		obs_data_set_string(item, "last_name", d.lastKnownName.c_str());
		obs_data_set_int(item, "last_connected", d.lastConnectedUnix);
		obs_data_array_push_back(array, item);
		obs_data_release(item);
	}
	obs_data_set_array(root, "devices", array);
	obs_data_array_release(array);
	// Writes to a temp file, then atomically replaces the target, keeping a backup.
	const bool ok = obs_data_save_json_pretty_safe(root, path.c_str(), "tmp", "bak");
	obs_data_release(root);
	return ok;
}

} // namespace blehr
