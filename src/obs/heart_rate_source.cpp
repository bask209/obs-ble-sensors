// "BLE Heart Rate" OBS input source.
//
// Rendering: the visible text is drawn by a private OBS "Text (GDI+)" child source that
// this source owns and renders inside its own video_render. The child is never added to
// a scene. This source decides every frame whether to draw it at all:
//
//   NO FRESH VALID HEART-RATE DATA = NOTHING RENDERED (fully transparent output)
//
// Threads: create/destroy/get_properties run on the UI thread; update (deferred by
// libobs for video sources), video_tick and video_render run on the graphics thread.
// BLE work happens on BleManager worker threads; the only shared state is the
// subscription, read through a small mutex-protected snapshot.

#include "heart_rate_source.hpp"

#include "core/display_format.hpp"
#include "obs/plugin_context.hpp"
#include "ui/sensor_dialog.hpp"

#include <obs-module.h>
#include <plugin-support.h>

#include <algorithm>
#include <atomic>
#include <mutex>
#include <string>

namespace blehr {

namespace {

constexpr const char *kSourceId = "ble_heart_rate_source";

constexpr const char *S_DEVICE = "device_key";
constexpr const char *S_MANAGE = "manage_sensors";
constexpr const char *S_STATUS = "status_info";
constexpr const char *S_REFRESH = "refresh_status";
constexpr const char *S_RECONNECT = "reconnect_now";
constexpr const char *S_AUTO_RECONNECT = "auto_reconnect";
constexpr const char *S_TIMEOUT = "stale_timeout";
constexpr const char *S_STYLE = "display_style";
constexpr const char *S_FONT = "font";
constexpr const char *S_COLOR = "color";
constexpr const char *S_OUTLINE = "outline";
constexpr const char *S_OUTLINE_SIZE = "outline_size";
constexpr const char *S_OUTLINE_COLOR = "outline_color";

constexpr int64_t kSecondNs = 1000000000LL;

const char *T(const char *key)
{
	return obs_module_text(key);
}

class HeartRateSource {
public:
	HeartRateSource(obs_data_t *settings, obs_source_t *source) : self_(source)
	{
		createTextChild(settings);
		applySettings(settings);
	}

	~HeartRateSource()
	{
		{
			std::lock_guard<std::mutex> lock(subMutex_);
			sub_.reset(); // Releases the shared BLE link without blocking.
		}
		if (text_)
			obs_source_release(text_);
	}

	void applySettings(obs_data_t *settings)
	{
		const std::string key = obs_data_get_string(settings, S_DEVICE);
		const bool autoReconnect = obs_data_get_bool(settings, S_AUTO_RECONNECT);
		const int64_t timeout = std::clamp<int64_t>(obs_data_get_int(settings, S_TIMEOUT), 2, 15);
		timeoutNs_.store(timeout * kSecondNs);
		style_.store(int(displayStyleFromInt(obs_data_get_int(settings, S_STYLE))));

		{
			std::lock_guard<std::mutex> lock(subMutex_);
			if (key != deviceKey_) {
				sub_.reset();
				deviceKey_ = key;
				if (!key.empty()) {
					std::optional<DeviceIdentity> identity;
					if (auto saved = plugin::registry().find(key))
						identity = saved->identity;
					else
						identity = identityFromKey(key);
					auto mgr = plugin::manager();
					if (identity && mgr)
						sub_ = mgr->subscribe(*identity, autoReconnect);
				}
				deviceChanged_.store(true); // Never show text rendered for the old device.
			} else if (sub_) {
				sub_->setAutoReconnect(autoReconnect);
			}
		}

		updateTextStyle(settings);
	}

	void tick()
	{
		if (!text_)
			return;
		tickCount_++;

		uint16_t bpm = 0;
		uint64_t generation = 0;
		const bool live = liveBpm(bpm, generation);
		const DisplayStyle style = DisplayStyle(style_.load());

		const bool styleChanged = styleChanged_.exchange(false);
		if (deviceChanged_.exchange(false) || styleChanged) {
			appliedLive_ = false;
			requestedText_.clear();
		}
		if (styleChanged)
			remeasureTick_ = tickCount_ + 2; // Child applies the new style on its next tick.
		if (live && generation != shownGeneration_) {
			appliedLive_ = false; // New connection: wait for text from this connection.
			shownGeneration_ = generation;
			requestedText_.clear();
		}

		// While invisible the child holds a wide sample text purely so the source keeps
		// a stable size; it is never drawn.
		// Only build a string when what should be shown changed (no per-frame allocation).
		const int64_t textKey = live ? (int64_t(style) << 16 | bpm) : -1 - int64_t(style);
		if (!requestedText_.empty() && textKey == requestedKey_) {
			// Unchanged.
		} else if (const std::string desired = live ? formatBpm(style, bpm) : layoutSample(style);
			   desired != requestedText_) {
			requestedKey_ = textKey;
			if (!live || !requestedLive_)
				appliedLive_ = false;
			setChildText(desired);
			requestedText_ = desired;
			requestedLive_ = live;
			// libobs applies the child's update during its next video_tick; by the next
			// frame of ours it has certainly happened.
			readyTick_ = tickCount_ + 1;
		}
		if (!live)
			appliedLive_ = false;
		else if (requestedLive_ && tickCount_ >= readyTick_)
			appliedLive_ = true;

		renderAllowed_.store(appliedLive_);
		renderGeneration_.store(shownGeneration_);

		// Stable layout: never shrink while the style is unchanged.
		if (remeasureTick_ && tickCount_ >= remeasureTick_) {
			remeasureTick_ = 0;
			width_.store(0);
			height_.store(0);
		}
		const uint32_t w = obs_source_get_width(text_);
		const uint32_t h = obs_source_get_height(text_);
		if (w > width_.load())
			width_.store(w);
		if (h > height_.load())
			height_.store(h);
	}

	void render()
	{
		if (!text_ || !renderAllowed_.load())
			return;
		// Re-check freshness at draw time so a disconnect or timeout is honoured on the
		// very next frame, independent of tick ordering.
		uint16_t bpm = 0;
		uint64_t generation = 0;
		if (!liveBpm(bpm, generation) || generation != renderGeneration_.load())
			return;
		obs_source_video_render(text_);
	}

	uint32_t width() const { return width_.load(); }
	uint32_t height() const { return height_.load(); }

	std::string statusText()
	{
		std::lock_guard<std::mutex> lock(subMutex_);
		if (deviceKey_.empty())
			return T("Status.NoSensor");
		if (!sub_)
			return T("Status.Unavailable");
		const SensorSnapshot snap = sub_->snapshot();
		return friendlyStatus(snap, monotonicNs(), timeoutNs_.load());
	}

	void reconnectNow()
	{
		std::lock_guard<std::mutex> lock(subMutex_);
		if (sub_)
			sub_->reconnectNow();
	}

	obs_source_t *source() const { return self_; }

private:
	bool liveBpm(uint16_t &bpm, uint64_t &generation)
	{
		std::lock_guard<std::mutex> lock(subMutex_);
		return sub_ && sub_->liveBpm(monotonicNs(), timeoutNs_.load(), bpm, generation);
	}

	void createTextChild(obs_data_t *settings)
	{
		const char *id = obs_get_latest_input_type_id("text_gdiplus");
		if (!id) {
			obs_log(LOG_ERROR, "Text (GDI+) source is not available; BLE Heart Rate cannot draw text");
			return;
		}
		obs_data_t *childSettings = obs_data_create();
		fillTextStyle(childSettings, settings);
		obs_data_set_string(childSettings, "text", layoutSample(displayStyleFromInt(obs_data_get_int(settings, S_STYLE))).c_str());
		text_ = obs_source_create_private(id, "BLE Heart Rate Text", childSettings);
		obs_data_release(childSettings);
	}

	static void fillTextStyle(obs_data_t *child, obs_data_t *settings)
	{
		// Copy the font as explicit values: the stored object may hold only defaults, and
		// obs_data_apply() (used by obs_source_update) does not carry nested defaults over.
		obs_data_t *font = obs_data_get_obj(settings, S_FONT);
		obs_data_t *childFont = obs_data_create();
		const char *face = font ? obs_data_get_string(font, "face") : "";
		const int64_t size = font ? obs_data_get_int(font, "size") : 0;
		obs_data_set_string(childFont, "face", (face && *face) ? face : "Arial");
		obs_data_set_string(childFont, "style", font ? obs_data_get_string(font, "style") : "");
		obs_data_set_int(childFont, "size", size > 0 ? size : 96);
		obs_data_set_int(childFont, "flags", font ? obs_data_get_int(font, "flags") : 0);
		obs_data_set_obj(child, "font", childFont);
		obs_data_release(childFont);
		obs_data_release(font);
		obs_data_set_int(child, "color", obs_data_get_int(settings, S_COLOR));
		obs_data_set_int(child, "opacity", 100);
		obs_data_set_bool(child, "outline", obs_data_get_bool(settings, S_OUTLINE));
		obs_data_set_int(child, "outline_size", obs_data_get_int(settings, S_OUTLINE_SIZE));
		obs_data_set_int(child, "outline_color", obs_data_get_int(settings, S_OUTLINE_COLOR));
		obs_data_set_int(child, "outline_opacity", 100);
		obs_data_set_int(child, "bk_opacity", 0); // Transparent background, always.
		obs_data_set_bool(child, "extents", false);
		obs_data_set_bool(child, "antialiasing", true);
	}

	void updateTextStyle(obs_data_t *settings)
	{
		if (!text_)
			return;
		obs_data_t *child = obs_data_create();
		fillTextStyle(child, settings);
		obs_data_set_int(child, "style_id", style_.load());
		const char *json = obs_data_get_json(child);
		std::string signature = json ? json : "";
		obs_data_erase(child, "style_id");
		// Only touch the child when the look actually changed (avoids re-rendering the
		// GDI+ texture and resizing on unrelated setting changes).
		if (signature != styleSignature_) {
			styleSignature_ = std::move(signature);
			obs_source_update(text_, child);
			styleChanged_.store(true);
		}
		obs_data_release(child);
	}

	void setChildText(const std::string &text)
	{
		obs_data_t *child = obs_data_create();
		obs_data_set_string(child, "text", text.c_str());
		obs_source_update(text_, child);
		obs_data_release(child);
	}

	obs_source_t *self_;
	obs_source_t *text_ = nullptr;

	std::mutex subMutex_;
	std::unique_ptr<BleManager::Subscription> sub_;
	std::string deviceKey_;

	std::atomic<int64_t> timeoutNs_{5 * kSecondNs};
	std::atomic<int> style_{int(kDefaultDisplayStyle)};
	std::atomic<bool> deviceChanged_{false};
	std::atomic<bool> styleChanged_{false};
	std::atomic<bool> renderAllowed_{false};
	std::atomic<uint64_t> renderGeneration_{0};
	std::atomic<uint32_t> width_{0};
	std::atomic<uint32_t> height_{0};

	// Graphics-thread only.
	uint64_t tickCount_ = 0;
	uint64_t readyTick_ = 0;
	uint64_t shownGeneration_ = 0;
	uint64_t remeasureTick_ = 0;
	std::string styleSignature_; // Touched by create (UI) then update (graphics), never concurrently.
	std::string requestedText_;
	int64_t requestedKey_ = 0;
	bool requestedLive_ = false;
	bool appliedLive_ = false;
};

// --- property callbacks -----------------------------------------------------------------

void fillDeviceList(obs_property_t *list, const std::string &currentKey)
{
	obs_property_list_clear(list);
	obs_property_list_add_string(list, T("Device.None"), "");
	bool found = currentKey.empty();
	for (const auto &d : plugin::registry().list()) {
		obs_property_list_add_string(list, d.displayName().c_str(), d.key().c_str());
		if (d.key() == currentKey)
			found = true;
	}
	if (!found) {
		// Selected sensor was forgotten: keep it selectable but never switch silently.
		const std::string label = std::string(T("Device.Removed")) + " (" + currentKey + ")";
		obs_property_list_add_string(list, label.c_str(), currentKey.c_str());
	}
}

bool onManageClicked(obs_properties_t *props, obs_property_t *, void *data)
{
	auto *self = static_cast<HeartRateSource *>(data);
	obs_weak_source_t *weak = obs_source_get_weak_source(self->source());

	std::string currentKey;
	{
		obs_data_t *settings = obs_source_get_settings(self->source());
		currentKey = obs_data_get_string(settings, S_DEVICE);
		obs_data_release(settings);
	}

	std::string chosen;
	const bool accepted = runSensorDialog(currentKey, chosen); // Modal; BLE keeps running.

	obs_source_t *source = obs_weak_source_get_source(weak);
	obs_weak_source_release(weak);
	if (!source)
		return false; // Source went away while the dialog was open.

	if (accepted && chosen != currentKey) {
		obs_data_t *update = obs_data_create();
		obs_data_set_string(update, S_DEVICE, chosen.c_str());
		obs_source_update(source, update);
		obs_data_release(update);
	}
	// Saved devices may have been added/renamed/removed: refresh the dropdown.
	obs_data_t *settings = obs_source_get_settings(source);
	fillDeviceList(obs_properties_get(props, S_DEVICE), obs_data_get_string(settings, S_DEVICE));
	obs_data_release(settings);
	obs_source_release(source);
	return true;
}

bool onRefreshClicked(obs_properties_t *, obs_property_t *, void *)
{
	return true; // Rebuilds the properties, including the status line.
}

bool onReconnectClicked(obs_properties_t *, obs_property_t *, void *data)
{
	static_cast<HeartRateSource *>(data)->reconnectNow();
	return true;
}

bool onOutlineModified(obs_properties_t *props, obs_property_t *, obs_data_t *settings)
{
	const bool outline = obs_data_get_bool(settings, S_OUTLINE);
	obs_property_set_visible(obs_properties_get(props, S_OUTLINE_SIZE), outline);
	obs_property_set_visible(obs_properties_get(props, S_OUTLINE_COLOR), outline);
	return true;
}

obs_properties_t *getProperties(void *data)
{
	auto *self = static_cast<HeartRateSource *>(data);
	obs_properties_t *props = obs_properties_create();

	std::string currentKey;
	if (self) {
		obs_data_t *settings = obs_source_get_settings(self->source());
		currentKey = obs_data_get_string(settings, S_DEVICE);
		obs_data_release(settings);
	}

	obs_property_t *list =
		obs_properties_add_list(props, S_DEVICE, T("Device"), OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	fillDeviceList(list, currentKey);
	obs_properties_add_button(props, S_MANAGE, T("ManageSensors"), onManageClicked);

	const std::string status = std::string(T("Status")) + ": " + (self ? self->statusText() : T("Status.NoSensor"));
	obs_properties_add_text(props, S_STATUS, status.c_str(), OBS_TEXT_INFO);
	obs_properties_add_button(props, S_REFRESH, T("RefreshStatus"), onRefreshClicked);
	obs_properties_add_button(props, S_RECONNECT, T("ReconnectNow"), onReconnectClicked);
	obs_properties_add_bool(props, S_AUTO_RECONNECT, T("AutoReconnect"));
	obs_property_t *timeout = obs_properties_add_int_slider(props, S_TIMEOUT, T("StaleTimeout"), 2, 15, 1);
	obs_property_int_set_suffix(timeout, " s");

	obs_property_t *style =
		obs_properties_add_list(props, S_STYLE, T("DisplayStyle"), OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(style, T("DisplayStyle.NumberOnly"), int(DisplayStyle::NumberOnly));
	obs_property_list_add_int(style, T("DisplayStyle.NumberUnit"), int(DisplayStyle::NumberUnit));
	obs_property_list_add_int(style, T("DisplayStyle.HeartNumber"), int(DisplayStyle::HeartNumber));
	obs_property_list_add_int(style, T("DisplayStyle.HeartNumberUnit"), int(DisplayStyle::HeartNumberUnit));

	obs_properties_add_font(props, S_FONT, T("Font"));
	obs_properties_add_color(props, S_COLOR, T("Color"));
	obs_property_t *outline = obs_properties_add_bool(props, S_OUTLINE, T("Outline"));
	obs_property_set_modified_callback(outline, onOutlineModified);
	obs_properties_add_int(props, S_OUTLINE_SIZE, T("OutlineSize"), 1, 20, 1);
	obs_properties_add_color(props, S_OUTLINE_COLOR, T("OutlineColor"));
	return props;
}

void getDefaults(obs_data_t *settings)
{
	obs_data_set_default_string(settings, S_DEVICE, "");
	obs_data_set_default_bool(settings, S_AUTO_RECONNECT, true);
	obs_data_set_default_int(settings, S_TIMEOUT, 5);
	obs_data_set_default_int(settings, S_STYLE, int(kDefaultDisplayStyle));

	obs_data_t *font = obs_data_create();
	obs_data_set_default_string(font, "face", "Arial");
	obs_data_set_default_string(font, "style", "Bold");
	obs_data_set_default_int(font, "size", 96);
	obs_data_set_default_int(font, "flags", OBS_FONT_BOLD);
	obs_data_set_default_obj(settings, S_FONT, font);
	obs_data_release(font);

	obs_data_set_default_int(settings, S_COLOR, 0xFFFFFFFF);
	obs_data_set_default_bool(settings, S_OUTLINE, false);
	obs_data_set_default_int(settings, S_OUTLINE_SIZE, 4);
	obs_data_set_default_int(settings, S_OUTLINE_COLOR, 0xFF000000);
}

} // namespace

void registerHeartRateSource()
{
	obs_source_info info = {};
	info.id = kSourceId;
	info.type = OBS_SOURCE_TYPE_INPUT;
	info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW | OBS_SOURCE_SRGB;
	info.icon_type = OBS_ICON_TYPE_TEXT;
	info.get_name = [](void *) { return T("SourceName"); };
	info.create = [](obs_data_t *settings, obs_source_t *source) -> void * {
		return new HeartRateSource(settings, source);
	};
	info.destroy = [](void *data) { delete static_cast<HeartRateSource *>(data); };
	info.update = [](void *data, obs_data_t *settings) { static_cast<HeartRateSource *>(data)->applySettings(settings); };
	info.get_defaults = getDefaults;
	info.get_properties = getProperties;
	info.video_tick = [](void *data, float) { static_cast<HeartRateSource *>(data)->tick(); };
	info.video_render = [](void *data, gs_effect_t *) { static_cast<HeartRateSource *>(data)->render(); };
	info.get_width = [](void *data) { return static_cast<HeartRateSource *>(data)->width(); };
	info.get_height = [](void *data) { return static_cast<HeartRateSource *>(data)->height(); };
	obs_register_source(&info);
}

} // namespace blehr
