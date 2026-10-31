# Notices and attribution

OBS BLE Sensors is licensed under the GNU General Public License v2.0 or later (see `LICENSE`).

It is built from and links against the following projects:

| Component | Use | License |
| --- | --- | --- |
| [obs-plugintemplate](https://github.com/obsproject/obs-plugintemplate) | CMake build system, CI helper scripts, `plugin-support` sources | GPL-2.0 |
| [OBS Studio](https://github.com/obsproject/obs-studio) (`libobs`, `obs-frontend-api`) | Plugin API, linked at runtime from the OBS installation | GPL-2.0-or-later |
| [Qt 6](https://www.qt.io/) (Core, Widgets) | Settings dialog, linked at runtime from the OBS installation | LGPL-3.0 |
| [C++/WinRT](https://github.com/microsoft/cppwinrt) | Windows Bluetooth LE API projection (headers from the Windows SDK) | MIT |
| Windows SDK (`WindowsApp.lib`) | Windows Runtime import library | Microsoft SDK license |

"Bluetooth" is a registered trademark of Bluetooth SIG, Inc. The Heart Rate Service
(0x180D) and Heart Rate Measurement characteristic (0x2A37) are implemented according to
the publicly available Bluetooth SIG Heart Rate Service specification.

This project is not affiliated with or endorsed by the OBS Project, Bluetooth SIG or any
sensor manufacturer.
