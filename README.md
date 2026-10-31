# OBS BLE Sensors

A native OBS Studio plugin for Windows that shows **live heart rate from a standard Bluetooth LE
heart-rate sensor** as a normal OBS source, with no phone, browser, server, helper app or internet
connection.

**Add BLE Heart Rate source → Scan → Select sensor → Save → Done.** After that it reconnects by
itself every time OBS starts.

> **Strict visibility rule:** NO FRESH VALID HEART-RATE DATA = NOTHING RENDERED.
> When the sensor is off, out of range, not worn, silent, unsupported, or Bluetooth is off, the
> source draws fully transparent pixels. It never shows "Disconnected", "0 BPM", "--", old values or
> a background. The source stays in your scene with its position, transform and filters intact.

![example](docs/example.png)

## Requirements

- Windows 10 1903+ / Windows 11, 64-bit, with a Bluetooth LE adapter
- OBS Studio **31 or newer** (built against the OBS 31.1.1 SDK; tested with OBS 32.2.2)
- A sensor that implements the standard Bluetooth **Heart Rate Service (0x180D)** with the
  **Heart Rate Measurement characteristic (0x2A37)**, for example most chest straps and arm bands
  (Polar H9/H10/Verity Sense, Wahoo TICKR, Garmin HRM-Dual, Coospo, Magene...).
  Smartwatches usually only work when their own "broadcast heart rate" mode is switched on.
  Devices with proprietary protocols are reported as unsupported, never faked.

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
5. Close the properties. The BPM appears as soon as the first valid reading arrives.

Properties:

| Setting | Meaning |
| --- | --- |
| Selected sensor | Which saved sensor this source uses. Each source can use a different sensor. |
| Scan / Manage Sensors... | Scan, save (with a name), rename or forget sensors. |
| Connection status | Plain-language status for troubleshooting. Only shown here, never on stream. |
| Refresh status / Reconnect now | Update the status line / retry immediately. Normally not needed. |
| Reconnect automatically | On by default. Retries with backoff whenever the sensor is missing. |
| Hide when no new data for | 2-15 s (default 5 s). The source hides if no valid reading arrives in this time. |
| Display style | `82`, `82 BPM`, `♥ 82`, `♥ 82 BPM` (default) |
| Font, Text color, Outline | Basic styling. The background is always transparent. |

Tips:
- Two sources can use the same sensor; they share one Bluetooth connection.
- If the selected sensor is unavailable the source stays invisible. It never switches to another
  sensor on its own.
- If a sensor says *"Windows denied access"*, pair it once in **Windows Settings → Bluetooth & devices**.

## How it works

```
 BLE sensor ──GATT notify 0x2A37──► WinRT backend ──► BleManager (one link per sensor, worker thread,
                                                         reconnect + backoff, shared by sources)
                                                              │ mutex-protected snapshot
 OBS graphics thread: HeartRateSource::video_tick/render ◄────┘
     └── renders a private "Text (GDI+)" child source ONLY when shouldRender() is true
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
  measurement is required, and the child text has to have been updated, before anything is drawn.
  While invisible, the child holds a sample text (`♥ 888 BPM`) that is never drawn, only to keep
  the source's size stable for transforms.
- **Threads.** Bluetooth runs on per-sensor worker threads (MTA). WinRT async operations are
  awaited with a 50 ms cancellation granularity. Event handlers go through a `SessionSink` gate
  that is detached before OS objects are released, so no callback can reach a destroyed object.
  Removing a source never blocks the video thread. OBS shutdown cancels and joins every worker.
- **Reconnect.** Exponential backoff (1 s → 30 s, ±20 % jitter). While waiting, a passive
  advertisement scan wakes the link as soon as the sensor reappears. A link that receives no
  notifications for 20 s is torn down and rebuilt, which covers sleep/wake and silent link loss.
- **Packets.** UINT8/UINT16 BPM, contact status, energy expended, RR intervals and variable lengths
  are all supported. Malformed packets are ignored (they never refresh freshness). A BPM of 0 or
  outside 20-300, or an explicit "contact lost", hides the value immediately.

## Build from source

Requirements: Visual Studio 2022 (or Build Tools) with *Desktop development with C++*, Windows SDK
10.0.20348+, CMake 3.28+. The OBS SDK, Qt 6 and obs-deps are downloaded automatically at the versions
pinned in `buildspec.json`.

```powershell
cmake --preset windows-x64
cmake --build --preset windows-x64-release          # build_x64\Release\obs-ble-sensors.dll
powershell -ExecutionPolicy Bypass -File scripts\package.ps1   # release\obs-ble-sensors-1.0.0-windows-x64.zip
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

See [docs/TEST_RESULTS.md](docs/TEST_RESULTS.md) for what was verified automatically, in real OBS,
and on real Bluetooth hardware, and [docs/MANUAL_TEST_CHECKLIST.md](docs/MANUAL_TEST_CHECKLIST.md)
for the repeatable hardware checklist.

## Known limitations

- Windows only (uses Windows Bluetooth LE APIs).
- Only the standard Heart Rate Service is supported. Proprietary watch protocols are not.
- Windows can take several seconds to notice a lost connection. The stale-data timeout (default 5 s)
  hides the value in the meantime.
- Sensors that rotate resolvable private addresses without being paired may need pairing in Windows
  once, so that their identity address can be resolved.
- Text is drawn with OBS's built-in *Text (GDI+)* renderer, so only outline styling is available
  (no drop shadow).
- English UI only (strings live in `data/locale/en-US.ini`).

## License

GPL-2.0-or-later, like OBS Studio. See [LICENSE](LICENSE) and [NOTICE.md](NOTICE.md).
