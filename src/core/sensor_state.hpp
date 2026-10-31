// Sensor connection state machine and the strict visibility rule.
//
// Rendering is driven only by a fresh valid measurement, never by connection state
// alone: NO FRESH VALID HEART-RATE DATA = NOTHING RENDERED.
#pragma once

#include "heart_rate_parser.hpp"

#include <cstdint>
#include <string>

namespace blehr {

// Valid readings required in a connection before anything is shown.
constexpr uint32_t kReadingsToConfirm = 2;

enum class SensorState {
	NotConfigured,
	Searching,
	Connecting,
	DiscoveringServices,
	Subscribing,
	Live,  // Subscribed; may or may not have a fresh measurement.
	Stale, // Subscribed, but data stopped or the sensor reports no contact.
	Disconnected,
	Unsupported,
	BluetoothOff,
	Error
};

const char *stateName(SensorState state);

struct SensorSnapshot {
	SensorState state = SensorState::NotConfigured;
	bool hasMeasurement = false; // A valid measurement from the current connection exists.
	uint16_t bpm = 0;
	int64_t lastValidNs = 0;  // Monotonic timestamp of the last displayable measurement.
	int64_t lastPacketNs = 0; // Monotonic timestamp of any notification, valid or not.
	bool contactLost = false;
	uint64_t connectionGeneration = 0; // Increments for every new connection.
	uint64_t malformedPackets = 0;
	uint64_t validPackets = 0;      // Lifetime count of displayed (confirmed) measurements.
	uint32_t validInConnection = 0; // Valid readings received in the current connection.
	std::string detail; // Short troubleshooting text for the settings UI only.
};

// The single visibility rule used by the renderer.
bool shouldRender(const SensorSnapshot &snapshot, int64_t nowNs, int64_t timeoutNs);

// State as presented to a user with a given stale timeout (Live becomes Stale once data
// is older than the timeout).
SensorState effectiveState(const SensorSnapshot &snapshot, int64_t nowNs, int64_t timeoutNs);

// Non-technical, one-line status for the settings UI.
std::string friendlyStatus(const SensorSnapshot &snapshot, int64_t nowNs, int64_t timeoutNs);

// Pure state machine. Not thread safe; the owner serializes access.
class SensorLinkMachine {
public:
	const SensorSnapshot &snapshot() const { return snap_; }

	void searching(const std::string &detail = {});
	void connecting();
	void discoveringServices();
	void subscribing();
	void subscribed(int64_t nowNs);
	void measurement(const HeartRateMeasurement &m, int64_t nowNs);
	void disconnected(const std::string &detail = {});
	void unsupported(const std::string &detail);
	void bluetoothOff(const std::string &detail = {});
	void error(const std::string &detail);
	void stopped(); // Back to NotConfigured.

private:
	void invalidate();
	SensorSnapshot snap_;
};

} // namespace blehr
