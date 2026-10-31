#include "plugin_context.hpp"

#include "registry_store.hpp"
#include "windows/winrt_backend.hpp"

#ifdef OBS_BLE_SENSORS_DEV_MOCK
#include "dev/dev_simulator.hpp"
#endif

#include <obs-module.h>
#include <plugin-support.h>

#include <cstdlib>
#include <mutex>

namespace blehr::plugin {

namespace {
std::mutex g_mutex;
std::shared_ptr<BleManager> g_manager;
bool g_devMode = false;
#ifdef OBS_BLE_SENSORS_DEV_MOCK
std::unique_ptr<DevSimulator> g_simulator;
#endif
} // namespace

DeviceRegistry &registry()
{
	// Intentionally leaked: sources may be destroyed after obs_module_unload().
	static DeviceRegistry *instance = new DeviceRegistry();
	return *instance;
}

std::shared_ptr<BleManager> manager()
{
	std::lock_guard<std::mutex> lock(g_mutex);
	return g_manager;
}

bool devModeActive()
{
	return g_devMode;
}

void initialize()
{
	const std::string path = registryFilePath();
	registry().load(loadRegistryFile(path));
	registry().setPersist([path](const std::vector<SavedDevice> &devices) {
		const bool ok = saveRegistryFile(path, devices);
		if (!ok)
			obs_log(LOG_WARNING, "failed to save sensor list to %s", path.c_str());
		return ok;
	});
	obs_log(LOG_INFO, "loaded %zu saved sensor(s)", registry().list().size());

	std::unique_ptr<BleBackend> backend;
#ifdef OBS_BLE_SENSORS_DEV_MOCK
	const char *env = std::getenv("OBS_BLE_SENSORS_MOCK");
	if (env && std::string(env) == "1") {
		g_simulator = std::make_unique<DevSimulator>();
		backend = g_simulator->takeBackend();
		g_devMode = true;
		obs_log(LOG_WARNING, "DEVELOPMENT MODE: using the simulated BLE backend (OBS_BLE_SENSORS_MOCK=1)");
	}
#endif
	if (!backend)
		backend = createWinRtBackend();

	auto mgr = BleManager::create(std::move(backend));
	mgr->setConnectedCallback([](const std::string &key, const std::string &name, const std::string &winId) {
		registry().noteConnected(key, unixTimeSeconds(), name, winId);
		obs_log(LOG_INFO, "sensor %s connected (%s)", key.c_str(), name.c_str());
	});
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		g_manager = std::move(mgr);
	}
#ifdef OBS_BLE_SENSORS_DEV_MOCK
	if (g_simulator)
		g_simulator->start();
#endif
}

void shutdown()
{
	std::shared_ptr<BleManager> mgr;
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		mgr = std::move(g_manager);
	}
#ifdef OBS_BLE_SENSORS_DEV_MOCK
	if (g_simulator)
		g_simulator->stop();
#endif
	if (mgr) {
		mgr->shutdown(); // Cancels connections and joins every worker thread.
		obs_log(LOG_INFO, "BLE manager stopped");
	}
	// Subscriptions still held by sources keep only weak references to the manager.
	mgr.reset();
#ifdef OBS_BLE_SENSORS_DEV_MOCK
	g_simulator.reset();
#endif
}

} // namespace blehr::plugin
