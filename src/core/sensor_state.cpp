#include "sensor_state.hpp"

namespace blehr {

const char *stateName(SensorState state)
{
	switch (state) {
	case SensorState::NotConfigured:
		return "NotConfigured";
	case SensorState::Searching:
		return "Searching";
	case SensorState::Connecting:
		return "Connecting";
	case SensorState::DiscoveringServices:
		return "DiscoveringServices";
	case SensorState::Subscribing:
		return "Subscribing";
	case SensorState::Live:
		return "Live";
	case SensorState::Stale:
		return "Stale";
	case SensorState::Disconnected:
		return "Disconnected";
	case SensorState::Unsupported:
		return "Unsupported";
	case SensorState::BluetoothOff:
		return "BluetoothOff";
	case SensorState::Error:
		return "Error";
	}
	return "Unknown";
}

bool shouldRender(const SensorSnapshot &s, int64_t nowNs, int64_t timeoutNs)
{
	if (s.state != SensorState::Live || !s.hasMeasurement || s.contactLost)
		return false;
	if (s.bpm < kMinValidBpm || s.bpm > kMaxValidBpm)
		return false;
	if (s.lastValidNs <= 0 || nowNs < s.lastValidNs)
		return false;
	return (nowNs - s.lastValidNs) < timeoutNs;
}

SensorState effectiveState(const SensorSnapshot &s, int64_t nowNs, int64_t timeoutNs)
{
	if (s.state == SensorState::Live && !shouldRender(s, nowNs, timeoutNs))
		return SensorState::Stale;
	return s.state;
}

std::string friendlyStatus(const SensorSnapshot &s, int64_t nowNs, int64_t timeoutNs)
{
	std::string text;
	switch (effectiveState(s, nowNs, timeoutNs)) {
	case SensorState::NotConfigured:
		text = "No sensor selected";
		break;
	case SensorState::Searching:
		text = "Looking for the sensor... make sure it is switched on and worn";
		break;
	case SensorState::Connecting:
	case SensorState::DiscoveringServices:
	case SensorState::Subscribing:
		text = "Connecting...";
		break;
	case SensorState::Live:
		text = "Connected - receiving heart rate (" + std::to_string(s.bpm) + " BPM)";
		break;
	case SensorState::Stale:
		if (s.contactLost)
			text = "Connected, but the sensor reports no skin contact";
		else
			text = "Connected - waiting for heart-rate data";
		break;
	case SensorState::Disconnected:
		text = "Disconnected - will reconnect automatically";
		break;
	case SensorState::Unsupported:
		text = "This device does not provide standard Bluetooth heart-rate data";
		break;
	case SensorState::BluetoothOff:
		text = "Bluetooth is turned off or unavailable";
		break;
	case SensorState::Error:
		text = "Problem connecting";
		break;
	}
	if (!s.detail.empty() && s.state != SensorState::Live)
		text += " (" + s.detail + ")";
	return text;
}

void SensorLinkMachine::invalidate()
{
	snap_.hasMeasurement = false;
	snap_.bpm = 0;
	snap_.lastValidNs = 0;
	snap_.contactLost = false;
}

void SensorLinkMachine::searching(const std::string &detail)
{
	invalidate();
	snap_.state = SensorState::Searching;
	snap_.detail = detail;
}

void SensorLinkMachine::connecting()
{
	invalidate();
	snap_.state = SensorState::Connecting;
	snap_.detail.clear();
}

void SensorLinkMachine::discoveringServices()
{
	invalidate();
	snap_.state = SensorState::DiscoveringServices;
}

void SensorLinkMachine::subscribing()
{
	// A new connection starts here: notifications may arrive as soon as the CCCD write
	// is issued, before the backend reports completion.
	invalidate();
	snap_.connectionGeneration++;
	snap_.validInConnection = 0;
	snap_.state = SensorState::Subscribing;
}

void SensorLinkMachine::subscribed(int64_t nowNs)
{
	if (snap_.state != SensorState::Subscribing && snap_.state != SensorState::Live)
		subscribing(); // Backend skipped the phase report.
	if (snap_.state == SensorState::Subscribing)
		snap_.state = SensorState::Live;
	if (snap_.lastPacketNs < nowNs && !snap_.hasMeasurement)
		snap_.lastPacketNs = nowNs; // Start the "no notifications" watchdog from here.
	snap_.detail.clear();
}

void SensorLinkMachine::measurement(const HeartRateMeasurement &m, int64_t nowNs)
{
	if (snap_.state != SensorState::Subscribing && snap_.state != SensorState::Live &&
	    snap_.state != SensorState::Stale)
		return; // Late packet from a torn-down connection.

	snap_.lastPacketNs = nowNs;

	switch (m.status) {
	case ParseStatus::Malformed:
		// Ignore the packet; it neither refreshes nor hides. Freshness timeout applies.
		snap_.malformedPackets++;
		return;
	case ParseStatus::NoHeartRate:
		// Fitness machine without a heart-rate source right now: nothing to show; the
		// freshness timeout hides any previous value.
		return;
	case ParseStatus::InvalidValue:
		invalidate();
		snap_.state = SensorState::Live;
		return;
	case ParseStatus::Ok:
		break;
	}

	if (m.contact == ContactStatus::NotDetected) {
		invalidate();
		snap_.contactLost = true;
		snap_.state = SensorState::Live; // Connected; effectiveState() reports Stale.
		return;
	}

	// The first reading of a connection is not trusted on its own: some devices replay
	// their last stored value when an app subscribes, even when they are no longer
	// measuring. Only a second reading in the same connection proves live streaming.
	snap_.state = SensorState::Live;
	if (++snap_.validInConnection < kReadingsToConfirm)
		return;
	snap_.hasMeasurement = true;
	snap_.contactLost = false;
	snap_.bpm = m.bpm;
	snap_.lastValidNs = nowNs;
	snap_.validPackets++;
}

void SensorLinkMachine::disconnected(const std::string &detail)
{
	invalidate();
	snap_.state = SensorState::Disconnected;
	snap_.detail = detail;
}

void SensorLinkMachine::unsupported(const std::string &detail)
{
	invalidate();
	snap_.state = SensorState::Unsupported;
	snap_.detail = detail;
}

void SensorLinkMachine::bluetoothOff(const std::string &detail)
{
	invalidate();
	snap_.state = SensorState::BluetoothOff;
	snap_.detail = detail;
}

void SensorLinkMachine::error(const std::string &detail)
{
	invalidate();
	snap_.state = SensorState::Error;
	snap_.detail = detail;
}

void SensorLinkMachine::stopped()
{
	invalidate();
	snap_.state = SensorState::NotConfigured;
	snap_.detail.clear();
}

} // namespace blehr

namespace blehr {

namespace {
constexpr uint64_t kTimeMask = (uint64_t(1) << 40) - 1;
constexpr int kBpmShift = 40;
constexpr uint64_t kBpmMask = 0x1FF;
constexpr int kShowShift = 49;
constexpr int kGenShift = 50;
constexpr uint64_t kGenMask = 0x3FFF;
} // namespace

uint64_t RenderWord::pack(const SensorSnapshot &s)
{
	const bool show = s.state == SensorState::Live && s.hasMeasurement && !s.contactLost &&
			  s.bpm >= kMinValidBpm && s.bpm <= kMaxValidBpm && s.lastValidNs > 0;
	if (!show)
		return (s.connectionGeneration & kGenMask) << kGenShift;
	const uint64_t ms = uint64_t(s.lastValidNs / 1000000) & kTimeMask;
	return ms | (uint64_t(s.bpm & kBpmMask) << kBpmShift) | (uint64_t(1) << kShowShift) |
	       ((s.connectionGeneration & kGenMask) << kGenShift);
}

bool RenderWord::evaluate(uint64_t word, int64_t nowNs, int64_t timeoutNs, uint16_t &bpm, uint64_t &generation)
{
	if (!((word >> kShowShift) & 1u))
		return false;
	const int64_t lastMs = int64_t(word & kTimeMask);
	const int64_t nowMs = nowNs / 1000000;
	if (lastMs <= 0 || nowMs < lastMs || (nowMs - lastMs) * 1000000 >= timeoutNs)
		return false;
	bpm = uint16_t((word >> kBpmShift) & kBpmMask);
	generation = (word >> kGenShift) & kGenMask;
	return true;
}

} // namespace blehr
