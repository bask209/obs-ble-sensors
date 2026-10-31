// "BLE Heart Rate" OBS input source.
//
// Rendering: two private OBS "Text (GDI+)" child sources (never added to a scene) render
// the heart glyph and the number. Each is captured into an offscreen mask only when its
// text or style changes (about once per second at most), and the masks are drawn with a
// small shader (data/effects/hr_overlay.effect):
//   - contrast halo: a ring around (never under) each glyph, one shader pass,
//     so the overlay stays readable on any background;
//   - heart: tinted, scaled with a lub-dub pulse at the measured BPM;
//   - number: either a fixed color, or "invert what's behind" (difference blend:
//     out = mask * (1 - behind) + behind * (1 - mask)).
// Every frame this source first decides whether to draw anything at all:
//
//   NO FRESH VALID HEART-RATE DATA = NOTHING RENDERED (fully transparent output)
//
// Threads: create/destroy/get_properties run on the UI thread; update (deferred by
// libobs for video sources), video_tick and video_render run on the graphics thread.
// BLE work happens on BleManager worker threads; the only shared state is the
// subscription, read through a small mutex-protected snapshot.

#include "heart_rate_source.hpp"

#include "core/display_format.hpp"
#include "core/heart_animation.hpp"
#include "obs/plugin_context.hpp"
#include "ui/sensor_dialog.hpp"

#include <graphics/vec2.h>
#include <graphics/vec4.h>
#include <obs-module.h>
#include <plugin-support.h>

#include <algorithm>
#include <atomic>
#include <cmath>
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
constexpr const char *S_HEART_COLOR = "heart_color";
constexpr const char *S_HEART_ANIMATE = "heart_animate";
constexpr const char *S_HEART_BEAT = "heart_beat_strength";
constexpr const char *S_NUMBER_MODE = "number_mode";
constexpr const char *S_COLOR = "color";
constexpr const char *S_HALO = "halo";
constexpr const char *S_HALO_COLOR = "halo_color";
constexpr const char *S_HALO_SIZE = "halo_size";
constexpr const char *S_HALO_OPACITY = "halo_opacity";

enum NumberMode : int { NumberInvert = 0, NumberFixedColor = 1 };

constexpr int64_t kSecondNs = 1000000000LL;

const char *T(const char *key)
{
	return obs_module_text(key);
}

vec4 colorFromSetting(obs_data_t *settings, const char *name, float opacity = 1.0f)
{
	vec4 v;
	vec4_from_rgba_srgb(&v, uint32_t(obs_data_get_int(settings, name)) | 0xFF000000u);
	v.w = opacity;
	return v;
}

struct Look {
	DisplayStyle style = kDefaultDisplayStyle;
	bool heart = true;
	bool animate = true;
	float beat = 0.18f;
	vec4 heartColor{};
	bool invert = true;
	vec4 numberColor{};
	bool halo = true;
	vec4 haloColor{};
	float haloSize = 3.0f;
};

// One private text child plus the offscreen mask captured from it.
struct GlyphLayer {
	obs_source_t *child = nullptr;
	gs_texrender_t *mask = nullptr;
	std::string requested;      // Text last sent to the child.
	bool requestedLive = false; // Whether that text is a real measurement.
	uint64_t readyTick = 0;     // Child has applied `requested` from this tick on.
	std::string captured;       // Text currently in `mask` ("" = nothing valid).
	bool capturedLive = false;
	uint32_t cx = 0, cy = 0; // Size of the captured mask.

	void setText(const std::string &text, bool live, uint64_t tick)
	{
		obs_data_t *d = obs_data_create();
		obs_data_set_string(d, "text", text.c_str());
		obs_source_update(child, d);
		obs_data_release(d);
		requested = text;
		requestedLive = live;
		// libobs applies the child's update during its next video_tick; by our next
		// tick it has certainly happened.
		readyTick = tick + 1;
		if (!live)
			capturedLive = false;
	}

	void invalidate()
	{
		requested.clear();
		captured.clear();
		capturedLive = false;
	}

	// Graphics context. Re-captures only when the applied text differs from the mask.
	bool ensureCaptured(uint64_t tick)
	{
		if (!child || !mask)
			return false;
		if (captured == requested && !captured.empty())
			return true;
		if (requested.empty() || tick < readyTick)
			return !captured.empty(); // Child not updated yet: keep the previous mask.
		const uint32_t w = obs_source_get_width(child);
		const uint32_t h = obs_source_get_height(child);
		if (!w || !h)
			return false;
		gs_texrender_reset(mask);
		gs_blend_state_push();
		gs_blend_function_separate(GS_BLEND_SRCALPHA, GS_BLEND_INVSRCALPHA, GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
		bool ok = false;
		if (gs_texrender_begin(mask, w, h)) {
			vec4 clear;
			vec4_zero(&clear);
			gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);
			gs_ortho(0.0f, float(w), 0.0f, float(h), -100.0f, 100.0f);
			obs_source_video_render(child);
			gs_texrender_end(mask);
			ok = true;
		}
		gs_blend_state_pop();
		if (!ok)
			return false;
		captured = requested;
		capturedLive = requestedLive;
		cx = w;
		cy = h;
		return true;
	}
};

class HeartRateSource {
public:
	HeartRateSource(obs_data_t *settings, obs_source_t *source) : self_(source)
	{
		heart_.child = createTextChild(settings, kHeartGlyph, "BLE Heart Rate Heart");
		number_.child = createTextChild(settings, numberLayoutSample(displayStyleFromInt(obs_data_get_int(settings, S_STYLE))).c_str(),
						"BLE Heart Rate Number");
		char *path = obs_module_file("effects/hr_overlay.effect");
		obs_enter_graphics();
		heart_.mask = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
		number_.mask = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
		char *errors = nullptr;
		effect_ = path ? gs_effect_create_from_file(path, &errors) : nullptr;
		obs_leave_graphics();
		if (!effect_)
			obs_log(LOG_ERROR, "could not load overlay shader %s: %s", path ? path : "(missing)",
				errors ? errors : "");
		bfree(errors);
		bfree(path);
		if (effect_) {
			paramImage_ = gs_effect_get_param_by_name(effect_, "image");
			paramTint_ = gs_effect_get_param_by_name(effect_, "tint");
			paramMaskSize_ = gs_effect_get_param_by_name(effect_, "mask_size");
			paramHaloRadius_ = gs_effect_get_param_by_name(effect_, "halo_radius");
		}
		applySettings(settings);
	}

	~HeartRateSource()
	{
		{
			std::lock_guard<std::mutex> lock(subMutex_);
			sub_.reset(); // Releases the shared BLE link without blocking.
		}
		obs_enter_graphics();
		gs_texrender_destroy(heart_.mask);
		gs_texrender_destroy(number_.mask);
		gs_effect_destroy(effect_);
		obs_leave_graphics();
		if (heart_.child)
			obs_source_release(heart_.child);
		if (number_.child)
			obs_source_release(number_.child);
	}

	void applySettings(obs_data_t *settings)
	{
		const std::string key = obs_data_get_string(settings, S_DEVICE);
		const bool autoReconnect = obs_data_get_bool(settings, S_AUTO_RECONNECT);
		const int64_t timeout = std::clamp<int64_t>(obs_data_get_int(settings, S_TIMEOUT), 2, 15);
		timeoutNs_.store(timeout * kSecondNs);

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
				deviceChanged_.store(true); // Never show a mask rendered for the old device.
			} else if (sub_) {
				sub_->setAutoReconnect(autoReconnect);
			}
		}

		// Look: read on the graphics thread only (update is deferred there; create
		// happens before any tick/render).
		Look look;
		look.style = displayStyleFromInt(obs_data_get_int(settings, S_STYLE));
		look.heart = styleShowsHeart(look.style);
		look.animate = obs_data_get_bool(settings, S_HEART_ANIMATE);
		look.beat = float(std::clamp<int64_t>(obs_data_get_int(settings, S_HEART_BEAT), 0, 40)) / 100.0f;
		look.heartColor = colorFromSetting(settings, S_HEART_COLOR);
		look.invert = obs_data_get_int(settings, S_NUMBER_MODE) == NumberInvert;
		look.numberColor = colorFromSetting(settings, S_COLOR);
		look.halo = obs_data_get_bool(settings, S_HALO);
		look.haloSize = float(std::clamp<int64_t>(obs_data_get_int(settings, S_HALO_SIZE), 1, 12));
		look.haloColor = colorFromSetting(settings, S_HALO_COLOR,
						  float(std::clamp<int64_t>(obs_data_get_int(settings, S_HALO_OPACITY), 0, 100)) /
							  100.0f);
		look_ = look;

		updateFont(settings);
	}

	void tick(float seconds)
	{
		if (!number_.child)
			return;
		tickCount_++;

		uint16_t bpm = 0;
		uint64_t generation = 0;
		const bool live = liveBpm(bpm, generation);

		const bool fontChanged = fontChanged_.exchange(false);
		if (deviceChanged_.exchange(false) || fontChanged)
			number_.invalidate();
		if (fontChanged) {
			heart_.invalidate();
			remeasureTick_ = tickCount_ + 2; // Children apply the new font on their next tick.
		}
		if (live && generation != shownGeneration_) {
			number_.invalidate(); // New connection: wait for text from this connection.
			shownGeneration_ = generation;
		}

		// Number: real value while live; otherwise a wide sample that is never drawn and
		// only keeps the source's size stable for transforms.
		const int64_t textKey = live ? (int64_t(look_.style) << 16 | bpm) : -1 - int64_t(look_.style);
		if (number_.requested.empty() || textKey != numberKey_) {
			numberKey_ = textKey;
			const std::string text = live ? formatNumber(look_.style, bpm) : numberLayoutSample(look_.style);
			if (text != number_.requested || live != number_.requestedLive)
				number_.setText(text, live, tickCount_);
		}
		if (heart_.requested.empty())
			heart_.setText(kHeartGlyph, true, tickCount_);

		if (live && look_.animate)
			animator_.advance(seconds, bpm);
		else
			animator_.reset();

		updateLayout();
	}

	void render()
	{
		if (!effect_)
			return;
		// 1) The visibility rule, re-checked at draw time so a disconnect or timeout is
		//    honoured on the very next frame.
		uint16_t bpm = 0;
		uint64_t generation = 0;
		if (!liveBpm(bpm, generation) || generation != shownGeneration_)
			return;
		// 2) Only draw masks rendered from live data of this connection. While a new value
		//    is being applied the previous (still fresh, same connection) value stays up,
		//    so BPM changes never flicker. Device/connection changes clear the mask.
		if (!number_.ensureCaptured(tickCount_) || !number_.capturedLive)
			return;
		const bool drawHeart = look_.heart && heart_.ensureCaptured(tickCount_);

		const bool previousSrgb = gs_framebuffer_srgb_enabled();
		gs_enable_framebuffer_srgb(true);
		gs_blend_state_push();

		const float heartScale = look_.animate ? animator_.scale(look_.beat) : 1.0f;
		const float hx = heartCenterX_ - float(heart_.cx) * heartScale * 0.5f;
		const float hy = heartCenterY_ - float(heart_.cy) * heartScale * 0.5f;

		// Contrast halo behind everything.
		gs_blend_function_separate(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA, GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
		if (look_.halo && look_.haloColor.w > 0.0f) {
			if (drawHeart)
				drawHalo(heart_, hx, hy, heartScale);
			drawHalo(number_, numberX_, numberY_, 1.0f);
		}

		if (drawHeart)
			drawMask(heart_, hx, hy, heartScale, look_.heartColor, "Tint");

		if (look_.invert) {
			// out = mask * (1 - behind) + behind * (1 - mask): a negative of the background.
			gs_blend_function_separate(GS_BLEND_INVDSTCOLOR, GS_BLEND_INVSRCCOLOR, GS_BLEND_ONE,
						   GS_BLEND_INVSRCALPHA);
			drawMask(number_, numberX_, numberY_, 1.0f, look_.numberColor, "InvertMask");
		} else {
			drawMask(number_, numberX_, numberY_, 1.0f, look_.numberColor, "Tint");
		}

		gs_blend_state_pop();
		gs_enable_framebuffer_srgb(previousSrgb);
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

	void drawMask(const GlyphLayer &layer, float x, float y, float scale, const vec4 &tint, const char *technique)
	{
		gs_texture_t *tex = gs_texrender_get_texture(layer.mask);
		if (!tex)
			return;
		gs_effect_set_texture(paramImage_, tex);
		gs_effect_set_vec4(paramTint_, &tint);
		gs_matrix_push();
		gs_matrix_translate3f(x, y, 0.0f);
		gs_matrix_scale3f(scale, scale, 1.0f);
		while (gs_effect_loop(effect_, technique))
			gs_draw_sprite(tex, 0, layer.cx, layer.cy);
		gs_matrix_pop();
	}

	// One pass: a ring of `haloSize` output pixels around the glyph, never under it.
	void drawHalo(const GlyphLayer &layer, float x, float y, float scale)
	{
		gs_texture_t *tex = gs_texrender_get_texture(layer.mask);
		if (!tex || scale <= 0.0f)
			return;
		const float r = look_.haloSize / scale; // In mask pixels.
		vec2 size;
		vec2_set(&size, float(layer.cx), float(layer.cy));
		gs_effect_set_texture(paramImage_, tex);
		gs_effect_set_vec4(paramTint_, &look_.haloColor);
		gs_effect_set_vec2(paramMaskSize_, &size);
		gs_effect_set_float(paramHaloRadius_, r);
		gs_matrix_push();
		gs_matrix_translate3f(x - r * scale, y - r * scale, 0.0f);
		gs_matrix_scale3f(scale, scale, 1.0f);
		while (gs_effect_loop(effect_, "Halo"))
			gs_draw_sprite(tex, 0, uint32_t(std::ceil(float(layer.cx) + 2.0f * r)),
				       uint32_t(std::ceil(float(layer.cy) + 2.0f * r)));
		gs_matrix_pop();
	}

	// Stable layout from the children's sizes; never shrinks while the font is unchanged.
	void updateLayout()
	{
		if (remeasureTick_ && tickCount_ >= remeasureTick_) {
			remeasureTick_ = 0;
			maxNumberW_ = maxNumberH_ = 0;
		}
		maxNumberW_ = std::max(maxNumberW_, obs_source_get_width(number_.child));
		maxNumberH_ = std::max(maxNumberH_, obs_source_get_height(number_.child));
		const float heartW = look_.heart && heart_.child ? float(obs_source_get_width(heart_.child)) : 0.0f;
		const float heartH = look_.heart && heart_.child ? float(obs_source_get_height(heart_.child)) : 0.0f;
		const float growth = 1.0f + (look_.animate ? look_.beat : 0.0f);
		const float heartBox = std::max(heartW, heartH) * growth;
		const float pad = look_.halo ? look_.haloSize : 0.0f;
		const float gap = heartBox > 0.0f ? heartH * 0.12f : 0.0f;
		const float numberW = float(maxNumberW_);
		const float numberH = float(maxNumberH_);
		const float contentH = std::max(heartBox, numberH);

		heartCenterX_ = pad + heartBox * 0.5f;
		heartCenterY_ = pad + contentH * 0.5f;
		numberX_ = pad + heartBox + gap;
		numberY_ = pad + (contentH - numberH) * 0.5f;
		width_.store(uint32_t(std::ceil(numberX_ + numberW + pad)));
		height_.store(uint32_t(std::ceil(contentH + 2.0f * pad)));
	}

	static obs_source_t *createTextChild(obs_data_t *settings, const char *text, const char *name)
	{
		const char *id = obs_get_latest_input_type_id("text_gdiplus");
		if (!id) {
			obs_log(LOG_ERROR, "Text (GDI+) source is not available; BLE Heart Rate cannot draw");
			return nullptr;
		}
		obs_data_t *childSettings = obs_data_create();
		fillTextStyle(childSettings, settings);
		obs_data_set_string(childSettings, "text", text);
		obs_source_t *child = obs_source_create_private(id, name, childSettings);
		obs_data_release(childSettings);
		return child;
	}

	// Children always draw plain white glyphs; color, halo and inversion happen in our
	// shader, so only the font matters here.
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
		obs_data_set_int(child, "color", 0xFFFFFFFF);
		obs_data_set_int(child, "opacity", 100);
		obs_data_set_bool(child, "outline", false);
		obs_data_set_int(child, "bk_opacity", 0);
		obs_data_set_bool(child, "extents", false);
		obs_data_set_bool(child, "antialiasing", true);
	}

	void updateFont(obs_data_t *settings)
	{
		obs_data_t *child = obs_data_create();
		fillTextStyle(child, settings);
		const char *json = obs_data_get_json(child);
		std::string signature = json ? json : "";
		// Only touch the children when the font actually changed (avoids re-rendering
		// GDI+ textures and resizing on unrelated setting changes).
		if (signature != fontSignature_) {
			fontSignature_ = std::move(signature);
			if (heart_.child)
				obs_source_update(heart_.child, child);
			if (number_.child)
				obs_source_update(number_.child, child);
			fontChanged_.store(true);
		}
		obs_data_release(child);
	}

	obs_source_t *self_;
	GlyphLayer heart_;
	GlyphLayer number_;
	gs_effect_t *effect_ = nullptr;
	gs_eparam_t *paramImage_ = nullptr;
	gs_eparam_t *paramTint_ = nullptr;
	gs_eparam_t *paramMaskSize_ = nullptr;
	gs_eparam_t *paramHaloRadius_ = nullptr;

	std::mutex subMutex_;
	std::unique_ptr<BleManager::Subscription> sub_;
	std::string deviceKey_;

	std::atomic<int64_t> timeoutNs_{5 * kSecondNs};
	std::atomic<bool> deviceChanged_{false};
	std::atomic<bool> fontChanged_{false};
	std::atomic<uint32_t> width_{0};
	std::atomic<uint32_t> height_{0};

	// Graphics-thread only (create runs before any tick/render).
	Look look_;
	HeartBeatAnimator animator_;
	std::string fontSignature_;
	uint64_t tickCount_ = 0;
	uint64_t shownGeneration_ = 0;
	uint64_t remeasureTick_ = 0;
	int64_t numberKey_ = 0;
	uint32_t maxNumberW_ = 0;
	uint32_t maxNumberH_ = 0;
	float heartCenterX_ = 0, heartCenterY_ = 0, numberX_ = 0, numberY_ = 0;
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

bool onLookModified(obs_properties_t *props, obs_property_t *, obs_data_t *settings)
{
	const bool heart = styleShowsHeart(displayStyleFromInt(obs_data_get_int(settings, S_STYLE)));
	const bool animate = obs_data_get_bool(settings, S_HEART_ANIMATE);
	const bool fixed = obs_data_get_int(settings, S_NUMBER_MODE) == NumberFixedColor;
	const bool halo = obs_data_get_bool(settings, S_HALO);
	obs_property_set_visible(obs_properties_get(props, S_HEART_COLOR), heart);
	obs_property_set_visible(obs_properties_get(props, S_HEART_ANIMATE), heart);
	obs_property_set_visible(obs_properties_get(props, S_HEART_BEAT), heart && animate);
	obs_property_set_visible(obs_properties_get(props, S_COLOR), fixed);
	obs_property_set_visible(obs_properties_get(props, S_HALO_COLOR), halo);
	obs_property_set_visible(obs_properties_get(props, S_HALO_SIZE), halo);
	obs_property_set_visible(obs_properties_get(props, S_HALO_OPACITY), halo);
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

	// Sensor
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

	// Appearance
	obs_properties_t *look = obs_properties_create();
	obs_property_t *style =
		obs_properties_add_list(look, S_STYLE, T("DisplayStyle"), OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(style, T("DisplayStyle.NumberOnly"), int(DisplayStyle::NumberOnly));
	obs_property_list_add_int(style, T("DisplayStyle.NumberUnit"), int(DisplayStyle::NumberUnit));
	obs_property_list_add_int(style, T("DisplayStyle.HeartNumber"), int(DisplayStyle::HeartNumber));
	obs_property_list_add_int(style, T("DisplayStyle.HeartNumberUnit"), int(DisplayStyle::HeartNumberUnit));
	obs_property_set_modified_callback(style, onLookModified);
	obs_properties_add_font(look, S_FONT, T("Font"));
	obs_properties_add_group(props, "group_look", T("Group.Look"), OBS_GROUP_NORMAL, look);

	obs_properties_t *heart = obs_properties_create();
	obs_properties_add_color(heart, S_HEART_COLOR, T("HeartColor"));
	obs_property_t *animate = obs_properties_add_bool(heart, S_HEART_ANIMATE, T("HeartAnimate"));
	obs_property_set_modified_callback(animate, onLookModified);
	obs_property_t *beat = obs_properties_add_int_slider(heart, S_HEART_BEAT, T("HeartBeatStrength"), 0, 40, 1);
	obs_property_int_set_suffix(beat, " %");
	obs_properties_add_group(props, "group_heart", T("Group.Heart"), OBS_GROUP_NORMAL, heart);

	obs_properties_t *numbers = obs_properties_create();
	obs_property_t *mode = obs_properties_add_list(numbers, S_NUMBER_MODE, T("NumberMode"), OBS_COMBO_TYPE_LIST,
						       OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(mode, T("NumberMode.Invert"), NumberInvert);
	obs_property_list_add_int(mode, T("NumberMode.Fixed"), NumberFixedColor);
	obs_property_set_modified_callback(mode, onLookModified);
	obs_properties_add_color(numbers, S_COLOR, T("Color"));
	obs_properties_add_group(props, "group_numbers", T("Group.Numbers"), OBS_GROUP_NORMAL, numbers);

	obs_properties_t *halo = obs_properties_create();
	obs_properties_add_color(halo, S_HALO_COLOR, T("HaloColor"));
	obs_property_t *size = obs_properties_add_int_slider(halo, S_HALO_SIZE, T("HaloSize"), 1, 12, 1);
	obs_property_int_set_suffix(size, " px");
	obs_property_t *opacity = obs_properties_add_int_slider(halo, S_HALO_OPACITY, T("HaloOpacity"), 0, 100, 1);
	obs_property_int_set_suffix(opacity, " %");
	obs_property_t *haloGroup = obs_properties_add_group(props, S_HALO, T("Group.Halo"), OBS_GROUP_CHECKABLE, halo);
	obs_property_set_modified_callback(haloGroup, onLookModified);
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

	// Colors are OBS 0xAABBGGRR values.
	obs_data_set_default_int(settings, S_HEART_COLOR, 0xFF3A30E8); // red
	obs_data_set_default_bool(settings, S_HEART_ANIMATE, true);
	obs_data_set_default_int(settings, S_HEART_BEAT, 18);
	obs_data_set_default_int(settings, S_NUMBER_MODE, NumberInvert);
	obs_data_set_default_int(settings, S_COLOR, 0xFFFFFFFF);
	obs_data_set_default_bool(settings, S_HALO, true);
	obs_data_set_default_int(settings, S_HALO_COLOR, 0xFF000000);
	obs_data_set_default_int(settings, S_HALO_SIZE, 3);
	obs_data_set_default_int(settings, S_HALO_OPACITY, 60);
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
	info.video_tick = [](void *data, float seconds) { static_cast<HeartRateSource *>(data)->tick(seconds); };
	info.video_render = [](void *data, gs_effect_t *) { static_cast<HeartRateSource *>(data)->render(); };
	info.get_width = [](void *data) { return static_cast<HeartRateSource *>(data)->width(); };
	info.get_height = [](void *data) { return static_cast<HeartRateSource *>(data)->height(); };
	obs_register_source(&info);
}

} // namespace blehr
