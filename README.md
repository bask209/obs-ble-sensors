# OBS BLE Sensors

A native OBS Studio plugin for Windows that shows **live heart rate from Bluetooth LE devices** (heart-rate
straps and bands, watches in broadcast mode, smart bikes, treadmills, rowers, cross-trainers, pulse
oximeters) as a normal OBS source. It needs no phone, browser, server, helper app or internet connection.

**Add BLE Heart Rate source → Scan → Select sensor → Save → Done.** After that it reconnects by
itself every time OBS starts.

> **Strict visibility rule:** NO FRESH VALID HEART-RATE DATA = NOTHING RENDERED.
> When the device is off, out of range, not worn, silent, unsupported, or Bluetooth is off, the
> source draws fully transparent pixels. It never shows "Disconnected", "0 BPM", "--", old values or
> a background. The source stays in your scene with its position, transform and filters intact.

![example](docs/example.png)

*Animated heart, digits that invert whatever is behind them, contrast halo. Captured in OBS 32.2.2.*

The **[review report](#review-report-v100)** below documents the efficiency, security and compatibility
work and what was measured.

## Compatibility

**Computer:** any Windows 10 (version 1903 or newer) or Windows 11 PC, 64-bit (x64), with a Bluetooth
**4.0 or newer** adapter. That covers built-in Bluetooth on practically every laptop since about 2013
and any USB Bluetooth dongle. No drivers or extra software are needed. If the adapter lacks Bluetooth
LE support, or Bluetooth is off or missing, the plugin says so exactly.
**OBS:** Studio **31 or newer**, 64-bit (built against the OBS 31.1.1 SDK; tested on 32.2.2).

**Heart-rate hardware:** every device that uses one of the three open Bluetooth SIG standards for
heart rate:

| Standard | Typical devices | Notes |
| --- | --- | --- |
| **Heart Rate Service 0x180D** | Chest straps and arm bands (Polar H9/H10/OH1/Verity Sense, Wahoo TICKR, Garmin HRM-Dual/Pro, Coospo, Magene, Decathlon, Suunto...). Watches in heart-rate broadcast mode (Galaxy Watch, Garmin, Coros, Suunto, Polar, Amazfit...). | Tested with a Galaxy Watch5 Pro. Contact detection is used when the device reports it. |
| **Fitness Machine Service 0x1826** | Smart bikes and trainers, treadmills, rowers, cross-trainers (Indoor Bike, Treadmill, Rower and Cross Trainer data) | Heart rate comes from a strap paired to the machine or from hand grips. |
| **Pulse Oximeter Service 0x1822** | Bluetooth fingertip pulse oximeters (continuous or spot-check pulse rate) | Shows the pulse rate. |

**Not possible:** devices that never send heart rate over an open standard.
- **Apple Watch:** it has no broadcast mode. A third-party "heart-rate broadcast" app on the watch
  that exposes 0x180D should work (untested).
- **Fitbit, Oura, Whoop app data, most phones**
- **ANT+-only and Bluetooth Classic-only straps**

These are identified as unsupported. Nothing is ever faked or guessed.

**Watches and phones with changing ("private") addresses:** many watches change their Bluetooth
address every few minutes for privacy. The plugin never guesses a device by name, so pair such a
device once in **Windows Settings → Bluetooth & devices → Add device** and select its
**"Paired with Windows"** entry in the Scan dialog. Windows then resolves its real identity and
reconnection keeps working. The dialog shows this tip automatically for such devices.

## Install

1. Download **`obs-ble-sensors-<version>-windows-x64-setup.exe`** from
   [Releases](https://github.com/bask209/obs-ble-sensors/releases).
2. Close OBS and run it. Windows asks for administrator permission because plugins go into
   `C:\ProgramData`. The installer is not code-signed, so SmartScreen may show
   "Windows protected your PC": click **More info → Run anyway**.
3. Start OBS. You can enable or disable the plugin in OBS 32's **Tools → Plugin Manager**.

Prefer no installer? Download the `.zip` instead, extract it and run **Install.cmd**, or copy the
`obs-ble-sensors` folder into `C:\ProgramData\obs-studio\plugins\`.

Resulting layout (standard OBS plugin layout):

```
C:\ProgramData\obs-studio\plugins\obs-ble-sensors\
    bin\64bit\obs-ble-sensors.dll
    data\effects\hr_overlay.effect
    data\locale\en-US.ini
```

### Uninstall

Windows Settings → Apps → **OBS BLE Sensors** → Uninstall (if you used the Setup.exe), or run **Uninstall.cmd** / delete
`C:\ProgramData\obs-studio\plugins\obs-ble-sensors`. Saved sensors are stored in
`%APPDATA%\obs-studio\plugin_config\obs-ble-sensors\devices.json` (Uninstall.cmd offers to delete it).

## User guide

1. **Sources → + → BLE Heart Rate**, name it, OK. Nothing appears on the canvas yet; that is expected.
2. Put the sensor on. Most chest straps only wake up when they touch skin.
3. Click **Scan / Manage Sensors...**. Saved sensors appear immediately; nearby heart-rate sensors are
   added as they are found.
4. Select your sensor and click **Use selected sensor**, then give it a friendly name.
5. Close the properties. The BPM appears once readings are confirmed live (the second valid reading,
   normally within about 2 s).

Properties:

| Setting | Meaning |
| --- | --- |
| Selected sensor | Which saved sensor this source uses. Each source can use a different sensor. |
| Scan / Manage Sensors... | Scan, save (with a name), rename or forget sensors. |
| Connection status | Plain-language status for troubleshooting. Only shown here, never on stream. |
| Refresh status / Reconnect now | Update the status line / retry immediately. Normally not needed. |
| Reconnect automatically | On by default. Retries with backoff whenever the sensor is missing. |
| Disconnect while not visible | On by default. If the source has not been visible anywhere (program, preview, projectors) for 30 s, its Bluetooth link is released, and it reconnects when the source shows again. This saves sensor battery and radio time. |
| Hide when no new data for | 2-15 s (default 5 s). The source hides if no valid reading arrives in this time. |
| **Appearance** → Display style, Font | `82`, `82 BPM`, `♥ 82`, `♥ 82 BPM` (default); any installed font |
| **Heart** → color, *Beat with my heart rate*, beat strength | The heart pulses ("lub-dub") at your live BPM. Default: red, on, 18 % |
| **Numbers** → *Invert what's behind (negative)* or *Fixed color* | Default: invert. The digits become a negative of whatever is behind them: black on white, white on black, cyan on red |
| **Contrast halo** (on/off), color, size, opacity | A ring around the heart and digits so they stay readable on any background (default: black, 3 px, 60 %) |

The background is always transparent.

Tips:
- Two sources can use the same sensor; they share one Bluetooth connection.
- If the selected sensor is unavailable the source stays invisible. It never switches to another
  sensor on its own.
- If a sensor says *"Windows denied access"*, pair it once in **Windows Settings → Bluetooth & devices**.
- *Invert what's behind* needs the source to draw straight onto the scene. If you add filters to
  this source, OBS draws it on its own layer first and the digits show plain white instead. Use
  *Fixed color* in that case.


## Bluetooth safety

The plugin is designed to be harmless to every Bluetooth device, yours and other people's:

- **Read-only.** The only thing it ever writes to a device is the standard "send me updates" switch
  (CCCD) of the one measurement characteristic it uses, on the device *you* selected. It never
  writes any other setting, never touches firmware, and never reads personal data.
- **Never pairs, unpairs or changes Windows Bluetooth settings.** It doesn't turn the radio on or
  off and doesn't install drivers. It only lists devices *you* paired. Your other Bluetooth devices
  (headphones, mouse, keyboard, controllers) are not connected to or disturbed. Closing its
  connection never disconnects a device that Windows or another app is using.
- **Connects only to the device you chose.** It matches by Bluetooth address, never by name, and
  never switches to another device by itself.
- **Leaves non-heart-rate devices alone.** If a selected device turns out to offer no standard
  heart-rate data, it disconnects right away and never retries on its own. The dialog also warns
  before you select a device that doesn't advertise heart rate.
- **Gentle on the sensor.** Missing devices are retried at most every 15 s. A device that connects
  but sends nothing (e.g. a watch with broadcasting off) is retried progressively less often, down
  to every 2 minutes. Invisible sources release the connection after 30 s.
- **Listens without transmitting.** There is no background scanning. An active scan runs only
  while the Scan dialog is open.
- Like any heart-rate app, a device connected to OBS uses one of its connection slots. Straps that
  allow only one connection can't talk to a phone app at the same time, and Windows may refuse a
  second app while OBS holds the device.

## Review report (v1.0.0)

This section records the in-depth review of resource use, security and compatibility, every change
it led to, and **what was measured versus what could not be verified**.

### 1. Resource efficiency

The plugin does almost nothing between heart beats. Every remaining cost was measured or removed:

| Area | Before the review | After | Evidence |
| --- | --- | --- | --- |
| CPU while waiting for an absent sensor | ≈1.7 % of one core (a continuous background scan made Windows hand every nearby advertisement to OBS) | **≈0.16 %** (background scan removed; periodic attempts only) | Measured with the real WinRT backend over 20 s, 18 BLE devices nearby |
| Connection thread while streaming | Woke 4× per second | **Event-driven**: sleeps until a packet arrives or the 20 s no-data deadline | Code + tests |
| Per-frame rendering | Contrast halo computed with 32 texture samples per pixel every frame | **Baked into a texture** when the number changes (≈1×/s). Each frame draws 3-4 small textured quads | OBS 32.2.2, 1080p60: **0 skipped frames of 7,567**, average frame render 0.22 ms for a 6-source scene |
| Text rendering | | Glyphs rasterized by GDI+ only when the displayed value or font changes. No string is built per frame | Code review |
| Invisible sources | Kept the device connected | Link **released after 30 s invisible** and re-established when shown (option, default on) | Real OBS: "connection closed" logged exactly 30 s after the scene was hidden |
| Silent devices (watch with broadcast off) | Reconnected every ≈22 s forever (observed in your OBS log) | Progressive back-off **15 s → 2 min** | Unit test + log analysis |
| Every reconnect | Full GATT discovery over the radio | **Cached GATT lookups first**, device only on a cache miss | Real hardware: cached discovery found the watch's service |
| Radio link | Windows default connection parameters | **Power-optimized** connection requested (Windows 11 22H2+; ignored on older versions) | API verified; radio-level savings not measurable here |
| Memory / threads | | One sleeping thread per sensor *in use*, none otherwise. A few MB in total | Code + process measurement |
| Shared sensors | | Two sources on one device share **one** connection | Unit test + real OBS log |

Why this is about as low as it can go: heart-rate data is roughly one 2-6 byte packet per second. The
plugin processes each packet once (a few hundred CPU instructions), redraws text at most once per
second, and otherwise only sleeps. The remaining costs are Windows' own Bluetooth stack and OBS
compositing a tiny transparent texture. The only further saving would be to stop reconnecting
entirely, which would defeat automatic reconnection.

### 2. Security

Threat model: everything received over the air (measurement packets, advertised device names) is
**attacker-controlled**, because anyone nearby can broadcast. Secondary concerns: local file
tampering and the integrity of the download.

| Measure | Details | Verified by |
| --- | --- | --- |
| Bounds-checked parsers | Every field read is length-checked; truncated or garbage packets are rejected; packets over 512 bytes are dropped before parsing | **2.1 million fuzzed packets** across all 7 measurement formats plus 200,000 random packets through the state machine |
| Memory safety | The whole test suite, including the multithreaded connection manager, runs under **AddressSanitizer** (also UBSan on Linux in CI) | 0 errors locally; enforced in CI |
| Device names | Capped at 64 bytes, invalid UTF-8 and control characters removed. Message boxes are forced to plain text, so a name can never be interpreted as HTML | Unit tests |
| Flood resistance | The scan list is capped (300 devices) and stale entries are pruned, so fake advertisements can't grow memory without bound | Code review |
| Thread and lifetime safety | Bluetooth callbacks go through gates that are closed before objects are destroyed. Shutdown cancels and joins every thread | "0 memory leaks" on every OBS shutdown tested; shutdown-while-connecting tests |
| Binary hardening | **Control Flow Guard**, **CET shadow-stack compatible**, SDL checks, ASLR (high entropy), DEP | Confirmed in the shipped DLL's headers (`dumpbin`) |
| Installed files | The installer removes write access for non-administrators on the plugin folder, so other users or malware without admin rights can't swap the DLL that OBS loads | Installer script |
| No network, no telemetry | No sockets, no internet, no data collection. The only file written is the saved-device list in your OBS config folder (atomic write with backup) | Code review; DLL imports |
| Supply chain | Releases are built by GitHub Actions from public source, with **SHA-256 checksums** and **GitHub build-provenance attestations** (`gh attestation verify <file> -R bask209/obs-ble-sensors`). Workflows use least-privilege tokens | Release workflow |
| Not done | **Code signing** (needs a paid certificate; SmartScreen will warn). **Spectre mitigations** were skipped because the plugin handles no secrets | |

### 3. Compatibility work

- **More hardware:** in addition to the Heart Rate Service, the plugin now reads the open SIG
  **Fitness Machine** (bikes, treadmills, rowers, cross-trainers) and **Pulse Oximeter** standards.
  Each parser is unit-tested against the specification's field layouts.
- **Paired devices** are listed in the Scan dialog, and devices with changing private addresses are
  detected and explained, with the pairing tip. This affects watches like the Galaxy Watch.
- **Precise adapter diagnostics:** Bluetooth off, no adapter, and adapter without Bluetooth LE are
  each reported clearly.
- **Stale-value protection:** a reading is shown only after a second reading in the same connection
  confirms the device is really streaming. Some watches replay their last stored value when an app
  subscribes, even after they stopped measuring.

### 4. Verification status (honest)

| Verified | How |
| --- | --- |
| 69 automated tests (parsers, fuzzing, state machine, connection manager with simulated devices, security) | Every build; 5+ consecutive runs; AddressSanitizer clean |
| Rendering, hiding, reconnect, restart persistence, shared connections, pause-when-hidden, clean shutdown | Real OBS 32.2.2 (portable copy) with the simulated sensor, driven through obs-websocket |
| Real Windows Bluetooth: scanning, paired-device listing, unsupported-device handling, absent device | `ble-probe` on this PC (Qualcomm FastConnect 7800) |
| Real heart-rate device | Galaxy Watch5 Pro: 19 minutes of continuous live data with v1.0.0-beta.1 in the user's OBS. The new connection code found its heart-rate service; live streaming through the final code still needs the watch broadcasting. |

**Not verified:** chest straps, fitness machines and pulse oximeters on real hardware (the parsers
are tested against the specifications only); Windows 10; other Bluetooth adapters; ARM64 Windows
(not built: only x64 is supported); radio-level energy savings. Reports are welcome in Issues.

## How it works

```
 BLE sensor ──GATT notify 0x2A37──► WinRT backend ──► BleManager (one link per sensor, worker thread,
                                                         reconnect + backoff, shared by sources)
                                                              │ mutex-protected snapshot
 OBS graphics thread: HeartRateSource::video_tick/render ◄────┘
     └── draws heart + digit masks (halo, pulse, invert shader) ONLY when shouldRender() is true
```

| Component | File | Responsibility |
| --- | --- | --- |
| PluginMain | `src/plugin-main.cpp` | Module load/unload, early BLE shutdown on OBS exit |
| HeartRateSource | `src/obs/heart_rate_source.cpp` | `obs_source_info`, settings, properties, rendering, transparent fallback |
| BleManager | `src/core/ble_manager.*` | Shared links, connect/subscribe/reconnect, scan demand, shutdown safety |
| WinRT backend | `src/windows/winrt_backend.*` | `BluetoothLEAdvertisementWatcher`, `BluetoothLEDevice`, GATT, CCCD |
| DeviceRegistry | `src/core/device_registry.*`, `src/obs/registry_store.*` | Saved sensors, crash-safe JSON (`obs_data_save_json_pretty_safe`, `.bak`) |
| SensorDialog | `src/ui/sensor_dialog*` | Qt scan/manage dialog |
| HeartRateParser | `src/core/heart_rate_parser.*` | Pure Bluetooth SIG 0x2A37 parser |
| State machine | `src/core/sensor_state.*` | `NotConfigured → Searching → Connecting → DiscoveringServices → Subscribing → Live / Stale / Disconnected / Unsupported / BluetoothOff / Error` |

Key design decisions:

- **Identity.** A sensor is keyed by its Bluetooth identity address and type
  (`ble:public:A0:9E:1A:12:34:56`). Windows derives its own device id from this address for
  unpaired devices and reports the resolved identity address for bonded devices that use private
  addresses. The Windows `DeviceId` is stored as extra metadata and used as a fallback. Devices are
  never matched by name.
- **Rendering.** Visibility is decided on every frame from the time of the last valid measurement,
  not from the connection state. The renderer checks freshness again at draw time, so a disconnect
  or timeout hides the source on the very next frame. After a device switch or reconnect, a new
  measurement is required, and its mask has to be re-rendered, before anything is drawn.
  Glyphs come from private *Text (GDI+)* children captured into offscreen masks. A shader
  (`data/effects/hr_overlay.effect`) tints them, draws the halo ring, and inverts the background with
  an `INVDSTCOLOR`/`INVSRCCOLOR` blend. While invisible, the number child holds a sample text
  (`888 BPM`) that is never drawn, only to keep the source's size stable for transforms.
- **Threads.** Bluetooth runs on per-sensor worker threads (MTA). WinRT async operations are
  awaited with a 50 ms cancellation granularity. Event handlers go through a `SessionSink` gate
  that is detached before OS objects are released, so no callback can reach a destroyed object.
  Removing a source never blocks the video thread. OBS shutdown cancels and joins every worker.
- **Reconnect.** Exponential backoff (1 s → 15 s, ±20 % jitter). The backoff resets only after real
  data was received. An optional passive scan to wake early (`ManagerConfig::backgroundScan`) is off
  by default to save CPU. A link that receives no
  notifications for 20 s is torn down and rebuilt, which covers sleep/wake and silent link loss.
- **Packets.** Heart Rate Service: UINT8/UINT16 BPM, contact status, energy expended, RR intervals and variable lengths
  are all supported. Malformed packets are ignored (they never refresh freshness). A BPM of 0 or
  outside 20-300, or an explicit "contact lost", hides the value immediately.
  Fitness-machine data is parsed field by field per the FTMS specification; pulse-oximeter pulse
  rate is an IEEE-11073 SFLOAT, and its "not available" codes are never shown.

## Build from source

Requirements: Visual Studio 2022 (or Build Tools) with *Desktop development with C++*, Windows SDK
10.0.20348+, CMake 3.28+. The OBS SDK, Qt 6 and obs-deps are downloaded automatically at the versions
pinned in `buildspec.json`.

```powershell
cmake --preset windows-x64
cmake --build --preset windows-x64-release          # build_x64\Release\obs-ble-sensors.dll
powershell -ExecutionPolicy Bypass -File scripts\package.ps1   # release\*-setup.exe + .zip (needs Inno Setup 6)
```

Unit tests (no OBS, Qt or Bluetooth needed; also run on Linux in CI):

```powershell
cmake -S tests -B build_tests
cmake --build build_tests --config Release
ctest --test-dir build_tests -C Release --output-on-failure
```

`build_tests\Release\ble-probe.exe` is a console diagnostic for real hardware. Run it without
arguments to scan, or with `ble-probe ble:public:AA:BB:CC:DD:EE:FF 60` to connect and print live state.

### Development mode (simulated sensor)

The `windows-dev-x64` preset compiles a simulated backend into the plugin. It is active only when
OBS is started with `OBS_BLE_SENSORS_MOCK=1`, and the log and dialog say so clearly. Release presets
and CI never include it, and CI checks that the release DLL does not contain it.

```powershell
cmake --preset windows-dev-x64; cmake --build --preset windows-dev-x64
```

`tests/integration/obs_itest.mjs` drives a portable OBS through obs-websocket to check rendering and
visibility with the simulated sensor (see `docs/TEST_RESULTS.md`).


## Testing status

See [docs/TEST_RESULTS.md](docs/TEST_RESULTS.md) for the detailed test log, and
[docs/MANUAL_TEST_CHECKLIST.md](docs/MANUAL_TEST_CHECKLIST.md) for the repeatable hardware checklist.

## Known limitations

- Windows x64 only (uses Windows Bluetooth LE APIs; ARM64 builds are not provided).
- Only open Bluetooth SIG standards (Heart Rate, Fitness Machine, Pulse Oximeter). Proprietary
  protocols are not supported.
- Devices with changing private addresses must be paired with Windows once for reliable
  reconnection (see Compatibility).
- Windows can take several seconds to notice a lost connection. The stale-data timeout (default 5 s)
  hides the value in the meantime.
- The first reading after each (re)connect is held until a second one confirms live data (about 1 s).
- A switched-on device is picked up within the retry interval (up to 15 s), and a hidden source
  reconnects when shown again (a few seconds).
- Inversion is a difference blend: on mid-grey backgrounds the digits stay light grey, and the halo
  keeps them readable. It is not available when the source has filters (see Tips).
- The installer is not code-signed, so Windows SmartScreen shows a warning.
- English UI only (strings live in `data/locale/en-US.ini`).

## License

GPL-2.0-or-later, like OBS Studio. See [LICENSE](LICENSE) and [NOTICE.md](NOTICE.md).
