#include "winrt_backend.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <combaseapi.h>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Devices.Bluetooth.h>
#include <winrt/Windows.Devices.Bluetooth.Advertisement.h>
#include <winrt/Windows.Devices.Bluetooth.GenericAttributeProfile.h>
#include <winrt/Windows.Devices.Enumeration.h>
#include <winrt/Windows.Devices.Radios.h>
#include <winrt/Windows.Storage.Streams.h>

#include <algorithm>
#include <chrono>
#include <mutex>
#include <vector>

namespace blehr {

namespace {

using namespace std::chrono_literals;
using winrt::Windows::Foundation::AsyncStatus;
using winrt::Windows::Foundation::IAsyncInfo;
namespace wdb = winrt::Windows::Devices::Bluetooth;
namespace adv = winrt::Windows::Devices::Bluetooth::Advertisement;
namespace gatt = winrt::Windows::Devices::Bluetooth::GenericAttributeProfile;
namespace radios = winrt::Windows::Devices::Radios;
namespace enumeration = winrt::Windows::Devices::Enumeration;
namespace streams = winrt::Windows::Storage::Streams;

enum class WaitResult { Completed, Failed, Cancelled, TimedOut };

// Waits for a WinRT async operation without blocking forever, so that link cancellation
// (source removed, device switched, OBS shutting down) takes effect within ~200 ms (few
// wake-ups while waiting), and
// cancels the OS operation on timeout or cancellation.
//
// The Completed handler may be assigned only once per operation (C++/WinRT's wait_for()
// assigns it on every call, so it cannot be used in a polling loop). The handler owns a
// shared reference to the event, so a late completion after we gave up stays safe.
template<typename Async> WaitResult waitFor(const Async &op, CancelToken *cancel, std::chrono::milliseconds timeout)
{
	auto event = std::make_shared<winrt::handle>(CreateEventW(nullptr, TRUE, FALSE, nullptr));
	if (!*event)
		return WaitResult::Failed;
	op.Completed([event](const auto &, AsyncStatus) { SetEvent(event->get()); });

	const auto deadline = std::chrono::steady_clock::now() + timeout;
	while (WaitForSingleObject(event->get(), 200) == WAIT_TIMEOUT) {
		const bool cancelled = cancel && cancel->cancelled();
		if (cancelled || std::chrono::steady_clock::now() >= deadline) {
			try {
				op.Cancel();
			} catch (...) {
			}
			return cancelled ? WaitResult::Cancelled : WaitResult::TimedOut;
		}
	}
	return op.Status() == AsyncStatus::Completed ? WaitResult::Completed : WaitResult::Failed;
}

std::string narrow(const winrt::hstring &s)
{
	return winrt::to_string(s);
}

class ScanGate {
public:
	explicit ScanGate(BleBackend::AdvertisementFn fn) : fn_(std::move(fn)) {}
	void deliver(const Advertisement &a)
	{
		std::lock_guard<std::mutex> lock(mutex_);
		if (fn_)
			fn_(a);
	}
	void kill()
	{
		std::lock_guard<std::mutex> lock(mutex_);
		fn_ = nullptr;
	}
	std::atomic<bool> aborted{false};

private:
	std::mutex mutex_;
	BleBackend::AdvertisementFn fn_;
};

// Standard SIG heart-rate sources, in order of preference.
struct Profile {
	MeasurementKind kind;
	uint16_t service;
	uint16_t characteristic;
	bool indicate; // Uses indications instead of notifications.
};
constexpr Profile kProfiles[] = {
	{MeasurementKind::HeartRateService, 0x180D, 0x2A37, false},
	{MeasurementKind::IndoorBike, 0x1826, 0x2AD2, false},
	{MeasurementKind::Treadmill, 0x1826, 0x2ACD, false},
	{MeasurementKind::Rower, 0x1826, 0x2AD1, false},
	{MeasurementKind::CrossTrainer, 0x1826, 0x2ACE, false},
	{MeasurementKind::PlxContinuous, 0x1822, 0x2A5F, false},
	{MeasurementKind::PlxSpotCheck, 0x1822, 0x2A5E, true},
};

winrt::guid uuid16(uint16_t shortId)
{
	return gatt::GattDeviceService::ConvertShortIdToUuid(shortId);
}

bool isHeartRateCapableService(const winrt::guid &uuid)
{
	return uuid == uuid16(0x180D) || uuid == uuid16(0x1826) || uuid == uuid16(0x1822);
}

struct Selection {
	const Profile *profile = nullptr;
	gatt::GattDeviceService service{nullptr};
	gatt::GattCharacteristic characteristic{nullptr};
};

ConnectStatus communicationStatus(gatt::GattCommunicationStatus status, std::string &detail, const char *what)
{
	switch (status) {
	case gatt::GattCommunicationStatus::Success:
		return ConnectStatus::Connected;
	case gatt::GattCommunicationStatus::Unreachable:
		detail = "not in range";
		return ConnectStatus::NotFound;
	case gatt::GattCommunicationStatus::AccessDenied:
		detail.clear();
		return ConnectStatus::AccessDenied;
	default:
		detail = std::string(what) + " error";
		return ConnectStatus::Failed;
	}
}

// Picks the first supported profile the device offers. Returns Connected on success,
// Unsupported if the device has none, or a communication failure. Unused service objects
// are closed so no extra OS resources are held.
ConnectStatus selectProfile(const wdb::BluetoothLEDevice &device, wdb::BluetoothCacheMode mode, CancelToken &cancel,
			    Selection &out, std::string &detail)
{
	auto svcOp = device.GetGattServicesAsync(mode);
	const WaitResult w = waitFor(svcOp, &cancel, 20000ms);
	if (w == WaitResult::Cancelled)
		return ConnectStatus::Cancelled;
	if (w != WaitResult::Completed) {
		detail = "service discovery timed out";
		return ConnectStatus::NotFound;
	}
	const auto services = svcOp.GetResults();
	ConnectStatus status = communicationStatus(services.Status(), detail, "service discovery");
	if (status != ConnectStatus::Connected)
		return status;

	std::vector<gatt::GattDeviceService> all;
	for (const auto &s : services.Services())
		all.push_back(s);
	auto closeAllExcept = [&all](const gatt::GattDeviceService &keep) {
		for (auto &s : all) {
			try {
				if (s != keep)
					s.Close();
			} catch (...) {
			}
		}
	};

	for (const Profile &profile : kProfiles) {
		for (const auto &service : all) {
			if (service.Uuid() != uuid16(profile.service))
				continue;
			auto chrOp = service.GetCharacteristicsForUuidAsync(uuid16(profile.characteristic), mode);
			const WaitResult cw = waitFor(chrOp, &cancel, 15000ms);
			if (cw == WaitResult::Cancelled) {
				closeAllExcept(nullptr);
				return ConnectStatus::Cancelled;
			}
			if (cw != WaitResult::Completed)
				continue;
			const auto chars = chrOp.GetResults();
			if (chars.Status() != gatt::GattCommunicationStatus::Success) {
				status = communicationStatus(chars.Status(), detail, "characteristic discovery");
				if (status == ConnectStatus::NotFound || status == ConnectStatus::AccessDenied) {
					closeAllExcept(nullptr);
					return status;
				}
				continue;
			}
			const auto needed = profile.indicate ? gatt::GattCharacteristicProperties::Indicate
							     : gatt::GattCharacteristicProperties::Notify;
			for (const auto &c : chars.Characteristics()) {
				if ((c.CharacteristicProperties() & needed) == needed) {
					out.profile = &profile;
					out.service = service;
					out.characteristic = c;
					closeAllExcept(service);
					return ConnectStatus::Connected;
				}
			}
		}
	}
	closeAllExcept(nullptr);
	detail = "checked Heart Rate 0x180D, Fitness Machine 0x1826 and Pulse Oximeter 0x1822";
	return ConnectStatus::Unsupported;
}

// One connected + subscribed heart-rate characteristic. Destruction revokes the event
// handlers first (the SessionSink was already detached by the manager), then releases
// the OS objects so Windows can drop the connection.
class WinRtSession : public BleSession {
public:
	~WinRtSession() override
	{
		try {
			if (characteristic && valueToken.value)
				characteristic.ValueChanged(valueToken);
		} catch (...) {
		}
		try {
			if (device && statusToken.value)
				device.ConnectionStatusChanged(statusToken);
		} catch (...) {
		}
		try {
			if (connectionParameters)
				connectionParameters.Close(); // Back to Windows defaults.
		} catch (...) {
		}
		try {
			if (session) {
				session.MaintainConnection(false);
				session.Close();
			}
		} catch (...) {
		}
		try {
			if (service)
				service.Close();
		} catch (...) {
		}
		try {
			if (device)
				device.Close();
		} catch (...) {
		}
	}

	wdb::BluetoothLEDevice device{nullptr};
	gatt::GattSession session{nullptr};
	gatt::GattDeviceService service{nullptr};
	gatt::GattCharacteristic characteristic{nullptr};
	wdb::BluetoothLEPreferredConnectionParametersRequest connectionParameters{nullptr};
	winrt::event_token statusToken{};
	winrt::event_token valueToken{};
};

class WinRtBackend : public BleBackend {
public:
	WinRtBackend()
	{
		// Keep the process MTA alive so WinRT calls made from threads that never
		// initialized COM (e.g. OBS's video thread releasing a source) are valid.
		if (FAILED(CoIncrementMTAUsage(&mtaCookie_)))
			mtaCookie_ = nullptr;
	}

	~WinRtBackend() override
	{
		stopScan();
		{
			std::lock_guard<std::mutex> lock(adapterMutex_);
			adapter_ = nullptr; // Release WinRT objects while the MTA is still held.
			radio_ = nullptr;
		}
		if (mtaCookie_)
			CoDecrementMTAUsage(mtaCookie_);
	}

	void threadBegin() override
	{
		// The plugin's own threads (one per sensor in use, plus a short-lived paired-device
		// query) only wait on Bluetooth operations, so they run at the scheduler's minimum:
		//  - background processing mode: lowest CPU priority class plus very low disk-I/O
		//    and memory priority;
		//  - EcoQoS (Windows 11): efficiency mode, preferring E-cores at low clock speed.
		// This is safe because nothing time-critical waits for these threads: the OBS
		// render path reads their results through a single atomic word, never a lock.
		SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);
		THREAD_POWER_THROTTLING_STATE throttling{};
		throttling.Version = THREAD_POWER_THROTTLING_CURRENT_VERSION;
		throttling.ControlMask = THREAD_POWER_THROTTLING_EXECUTION_SPEED;
		throttling.StateMask = THREAD_POWER_THROTTLING_EXECUTION_SPEED;
		SetThreadInformation(GetCurrentThread(), ThreadPowerThrottling, &throttling, sizeof(throttling));

		const HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
		threadInitialized() = SUCCEEDED(hr);
	}

	void threadEnd() override
	{
		if (threadInitialized())
			CoUninitialize();
		threadInitialized() = false;
		SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_END);
	}

	bool startScan(const ScanParams &params, AdvertisementFn onAdvertisement) override
	{
		std::lock_guard<std::mutex> lock(scanMutex_);
		stopScanLocked();
		try {
			auto gate = std::make_shared<ScanGate>(std::move(onAdvertisement));
			adv::BluetoothLEAdvertisementWatcher watcher;
			// Passive unless the user is looking at the scan dialog: passive scanning only
			// listens and never transmits scan requests to other people's devices.
			watcher.ScanningMode(params.active ? adv::BluetoothLEScanningMode::Active
							   : adv::BluetoothLEScanningMode::Passive);

			const bool all = params.allDevices;
			const std::vector<uint64_t> wanted = params.addresses; // sorted
			receivedToken_ = watcher.Received([gate, all, wanted](const adv::BluetoothLEAdvertisementWatcher &,
									     const adv::BluetoothLEAdvertisementReceivedEventArgs &args) {
				try {
					const uint64_t address = args.BluetoothAddress();
					// Cheapest possible rejection of unrelated devices (no string/UUID work).
					if (!all && !std::binary_search(wanted.begin(), wanted.end(), address))
						return;
					Advertisement a;
					a.identity.address = address;
					a.identity.addressType = args.BluetoothAddressType() == wdb::BluetoothAddressType::Random
									 ? AddressType::Random
									 : AddressType::Public;
					const auto advertisement = args.Advertisement();
					a.name = sanitizeDeviceName(narrow(advertisement.LocalName()));
					for (const auto &uuid : advertisement.ServiceUuids())
						if (isHeartRateCapableService(uuid))
							a.advertisesHeartRate = true;
					a.rssi = args.RawSignalStrengthInDBm();
					gate->deliver(a);
				} catch (...) {
				}
			});
			stoppedToken_ = watcher.Stopped([gate](const adv::BluetoothLEAdvertisementWatcher &,
							       const adv::BluetoothLEAdvertisementWatcherStoppedEventArgs &) {
				gate->aborted = true;
			});
			watcher.Start();
			watcher_ = watcher;
			gate_ = gate;
			return watcher.Status() != adv::BluetoothLEAdvertisementWatcherStatus::Aborted;
		} catch (...) {
			stopScanLocked();
			return false;
		}
	}

	void stopScan() override
	{
		std::lock_guard<std::mutex> lock(scanMutex_);
		stopScanLocked();
	}

	bool scanHealthy() override
	{
		std::lock_guard<std::mutex> lock(scanMutex_);
		if (!watcher_ || !gate_ || gate_->aborted)
			return false;
		try {
			const auto status = watcher_.Status();
			return status == adv::BluetoothLEAdvertisementWatcherStatus::Started ||
			       status == adv::BluetoothLEAdvertisementWatcherStatus::Created;
		} catch (...) {
			return false;
		}
	}

	AdapterState adapterState() override
	{
		// The adapter and radio objects are looked up once and cached: afterwards each check
		// is a cheap property read instead of two OS activations per retry. If a USB adapter
		// is unplugged the cached objects throw, and the lookup simply runs again.
		std::lock_guard<std::mutex> lock(adapterMutex_);
		for (int attempt = 0; attempt < 2; ++attempt) {
			try {
				if (!adapter_) {
					auto adapterOp = wdb::BluetoothAdapter::GetDefaultAsync();
					if (waitFor(adapterOp, nullptr, 5000ms) != WaitResult::Completed)
						return AdapterState::Ready; // Unknown: let the connection attempt decide.
					adapter_ = adapterOp.GetResults();
					radio_ = nullptr;
					if (!adapter_)
						return AdapterState::NoAdapter; // Not cached: re-checked next time.
				}
				if (!adapter_.IsLowEnergySupported())
					return AdapterState::NoLowEnergy;
				if (!radio_) {
					auto radioOp = adapter_.GetRadioAsync();
					if (waitFor(radioOp, nullptr, 5000ms) != WaitResult::Completed)
						return AdapterState::Ready;
					radio_ = radioOp.GetResults();
				}
				return (!radio_ || radio_.State() == radios::RadioState::On) ? AdapterState::Ready
											       : AdapterState::Off;
			} catch (...) {
				adapter_ = nullptr;
				radio_ = nullptr;
			}
		}
		return AdapterState::Ready;
	}

	std::vector<Advertisement> pairedDevices() override
	{
		std::vector<Advertisement> out;
		try {
			auto findOp = enumeration::DeviceInformation::FindAllAsync(
				wdb::BluetoothLEDevice::GetDeviceSelectorFromPairingState(true));
			if (waitFor(findOp, nullptr, 10000ms) != WaitResult::Completed)
				return out;
			for (const auto &info : findOp.GetResults()) {
				if (out.size() >= 64)
					break;
				try {
					auto devOp = wdb::BluetoothLEDevice::FromIdAsync(info.Id());
					if (waitFor(devOp, nullptr, 5000ms) != WaitResult::Completed)
						continue;
					auto device = devOp.GetResults();
					if (!device)
						continue;
					Advertisement a;
					a.identity.address = device.BluetoothAddress();
					a.identity.addressType = device.BluetoothAddressType() == wdb::BluetoothAddressType::Random
									 ? AddressType::Random
									 : AddressType::Public;
					a.identity.windowsDeviceId = narrow(device.DeviceId());
					a.name = sanitizeDeviceName(narrow(device.Name()));
					device.Close(); // Only metadata was needed; no radio connection is kept.
					out.push_back(std::move(a));
				} catch (...) {
				}
			}
		} catch (...) {
		}
		return out;
	}

	ConnectResult connect(const DeviceIdentity &identity, CancelToken &cancel, std::shared_ptr<SessionSink> sink,
			      const PhaseFn &onPhase) override
	{
		ConnectResult r;
		auto fail = [&r](ConnectStatus status, std::string detail) -> ConnectResult {
			r.status = status;
			r.detail = std::move(detail);
			r.session.reset();
			return std::move(r);
		};
		auto waitFail = [&](WaitResult w, const char *what) -> ConnectResult {
			if (w == WaitResult::Cancelled)
				return fail(ConnectStatus::Cancelled, {});
			if (w == WaitResult::TimedOut)
				return fail(ConnectStatus::NotFound, std::string(what) + " timed out");
			return fail(ConnectStatus::Failed, std::string(what) + " failed");
		};

		try {
			onPhase(ConnectPhase::Connecting);
			auto session = std::make_unique<WinRtSession>();

			// 1. Resolve the device by its identity address (works for unpaired sensors);
			//    fall back to the OS device id recorded on a previous connection.
			auto devOp = wdb::BluetoothLEDevice::FromBluetoothAddressAsync(
				identity.address, identity.addressType == AddressType::Random
							  ? wdb::BluetoothAddressType::Random
							  : wdb::BluetoothAddressType::Public);
			WaitResult w = waitFor(devOp, &cancel, 15000ms);
			if (w == WaitResult::Cancelled)
				return fail(ConnectStatus::Cancelled, {});
			if (w == WaitResult::Completed)
				session->device = devOp.GetResults();
			if (!session->device && !identity.windowsDeviceId.empty()) {
				auto idOp = wdb::BluetoothLEDevice::FromIdAsync(winrt::to_hstring(identity.windowsDeviceId));
				w = waitFor(idOp, &cancel, 15000ms);
				if (w == WaitResult::Cancelled)
					return fail(ConnectStatus::Cancelled, {});
				if (w == WaitResult::Completed)
					session->device = idOp.GetResults();
			}
			if (!session->device)
				return fail(ConnectStatus::NotFound, "not in range");

			r.deviceName = sanitizeDeviceName(narrow(session->device.Name()));
			r.windowsDeviceId = narrow(session->device.DeviceId());

			// Ask Windows to keep the link up and re-establish it while we hold the session.
			try {
				auto gsOp = gatt::GattSession::FromDeviceIdAsync(session->device.BluetoothDeviceId());
				if (waitFor(gsOp, &cancel, 5000ms) == WaitResult::Completed) {
					session->session = gsOp.GetResults();
					if (session->session)
						session->session.MaintainConnection(true);
				}
			} catch (...) {
			}
			if (cancel.cancelled())
				return fail(ConnectStatus::Cancelled, {});

			session->statusToken = session->device.ConnectionStatusChanged(
				[sink](const wdb::BluetoothLEDevice &device, const winrt::Windows::Foundation::IInspectable &) {
					try {
						if (device.ConnectionStatus() == wdb::BluetoothConnectionStatus::Disconnected)
							sink->lost("connection lost");
					} catch (...) {
					}
				});

			// 2. Find a standard heart-rate source. Cached GATT data first (no radio
			//    traffic when Windows already knows the device), the device itself only if
			//    the cache has nothing useful.
			onPhase(ConnectPhase::DiscoveringServices);
			Selection sel;
			ConnectStatus status = ConnectStatus::Connected;
			std::string detail;
			for (const auto mode : {wdb::BluetoothCacheMode::Cached, wdb::BluetoothCacheMode::Uncached}) {
				status = selectProfile(session->device, mode, cancel, sel, detail);
				if (status != ConnectStatus::Unsupported || mode == wdb::BluetoothCacheMode::Uncached)
					break;
			}
			if (status != ConnectStatus::Connected)
				return fail(status, detail);
			session->service = sel.service;
			session->characteristic = sel.characteristic;
			r.kind = sel.profile->kind;

			// 3. Register the handler before enabling notifications so no packet is missed,
			//    then write the CCCD (exactly one subscription per connection). This is the
			//    only write this plugin ever makes to a device.
			onPhase(ConnectPhase::Subscribing);
			const MeasurementKind kind = sel.profile->kind;
			session->valueToken = session->characteristic.ValueChanged(
				[sink, kind](const gatt::GattCharacteristic &, const gatt::GattValueChangedEventArgs &args) {
					try {
						const auto buffer = args.CharacteristicValue();
						// Measurement packets are tiny; refuse anything absurd early.
						if (buffer.Length() > 512)
							return;
						// Zero-copy: parse straight from Windows' buffer (no DataReader
						// object, no copy, no allocation per packet).
						sink->data(kind, buffer.data(), buffer.Length());
					} catch (...) {
					}
				});
			auto cccdOp = session->characteristic.WriteClientCharacteristicConfigurationDescriptorAsync(
				sel.profile->indicate ? gatt::GattClientCharacteristicConfigurationDescriptorValue::Indicate
						      : gatt::GattClientCharacteristicConfigurationDescriptorValue::Notify);
			w = waitFor(cccdOp, &cancel, 15000ms);
			if (w != WaitResult::Completed)
				return waitFail(w, "subscription");
			switch (cccdOp.GetResults()) {
			case gatt::GattCommunicationStatus::Success:
				break;
			case gatt::GattCommunicationStatus::Unreachable:
				return fail(ConnectStatus::NotFound, "not in range");
			case gatt::GattCommunicationStatus::AccessDenied:
				return fail(ConnectStatus::AccessDenied, {});
			default:
				return fail(ConnectStatus::Failed, "could not subscribe to heart-rate data");
			}

			// 4. Heart rate needs ~1 small packet per second: ask Windows 11 (22H2+) for a
			//    power-optimized connection to save radio time on the PC and battery on the
			//    sensor. Older Windows lacks the API; the default parameters are kept.
			try {
				session->connectionParameters = session->device.RequestPreferredConnectionParameters(
					wdb::BluetoothLEPreferredConnectionParameters::PowerOptimized());
			} catch (...) {
			}

			r.status = ConnectStatus::Connected;
			r.session = std::move(session);
			return std::move(r);
		} catch (const winrt::hresult_error &e) {
			const HRESULT code = e.code();
			const HRESULT notAvailable = HRESULT_FROM_WIN32(ERROR_DEVICE_NOT_AVAILABLE);
			const HRESULT notReady = HRESULT_FROM_WIN32(ERROR_NOT_READY);
			if (code == E_ACCESSDENIED)
				return fail(ConnectStatus::AccessDenied, {});
			if (code == notAvailable || code == notReady)
				return fail(ConnectStatus::BluetoothOff, {});
			return fail(ConnectStatus::Failed, narrow(e.message()));
		} catch (...) {
			return fail(ConnectStatus::Failed, "unexpected error");
		}
	}

private:
	static bool &threadInitialized()
	{
		thread_local bool initialized = false;
		return initialized;
	}

	void stopScanLocked()
	{
		// Kill the gate first: once kill() returns no Received handler is running into
		// the manager, so it is safe for the manager to go away afterwards.
		if (gate_)
			gate_->kill();
		if (watcher_) {
			try {
				watcher_.Received(receivedToken_);
				watcher_.Stopped(stoppedToken_);
				watcher_.Stop();
			} catch (...) {
			}
		}
		watcher_ = nullptr;
		gate_.reset();
	}

	CO_MTA_USAGE_COOKIE mtaCookie_ = nullptr;
	std::mutex adapterMutex_;
	wdb::BluetoothAdapter adapter_{nullptr};
	radios::Radio radio_{nullptr};
	std::mutex scanMutex_;
	adv::BluetoothLEAdvertisementWatcher watcher_{nullptr};
	std::shared_ptr<ScanGate> gate_;
	winrt::event_token receivedToken_{};
	winrt::event_token stoppedToken_{};
};

} // namespace

std::unique_ptr<BleBackend> createWinRtBackend()
{
	return std::make_unique<WinRtBackend>();
}

} // namespace blehr
