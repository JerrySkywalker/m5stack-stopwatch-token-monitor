# StopWatch Token Monitor

A standalone, four-page Token Monitor display for the **M5Stack StopWatch C152**.
It uses the Hermes Gadget SDK's display, touch, Wi-Fi, power and simulator code
and can also be built with Hermes voice support. The two profiles share the
same dashboard and data format. This is a community project.

| View | Contents |
| --- | --- |
| Overview | Today and month tokens and estimated USD cost, top tools or models |
| Quotas | Reported account windows, remaining percentages and reset countdowns |
| Trend | 7 or 30 days of tokens or estimated cost. Gaps indicate missing data |
| Devices | Per-device totals and freshness without exposing hostnames by default |

Swipe horizontally to change views. Tap the body or swipe up to cycle the
current view's details. Tap the footer to refresh, or hold the title and
release to open hardware settings. The standalone blue button refreshes and
the yellow button advances a page. In the personal profile the blue button
retains its Hermes push-to-talk behavior.

## Simulator and local verification

Install Python 3.12, CMake and a C++17 toolchain (Visual Studio C++ on Windows).
Tkinter is needed for the interactive window. Clone with
`git clone --recurse-submodules` or run `git submodule update --init --recursive`.

~~~sh
python tools/prepare.py
python -m pip install -e ./build/watch
hermes-gadget build-sim --test
python tools/check_local.py --output dist/screens
python tools/show_simulator.py
~~~

The headless test runs a synthetic authenticated Hub and the real adapter over
loopback, exercises all four pages with actual simulated touch, checks failure
and recovery, and verifies a local Gadget voice-state round trip from the
dashboard. It saves PNGs suitable for human or automated review. It uses
synthetic data and a local echo gateway.

## Build firmware

~~~sh
python -m pip install platformio==6.1.19
python tools/build_firmware.py
~~~

The build locks the Gadget SDK, ESP32 board environment and PlatformIO platform.
Use a short build path on Windows, for example
`python tools/build_firmware.py --build-dir C:/Dev/build/stopwatch/watch`.
The output contains a merged image (flash at `0x0`), an app-only image, license
notices and a SHA-256 receipt. The matching GitHub Actions job also packages
the standalone profile. The `--profile personal` switch builds the same
dashboard plus the upstream Hermes transport and audio behavior.

Back up the full original flash using the
[StopWatch upstream instructions](https://github.com/Adolanium/hermes-gadget-sdk/blob/v0.6.0/docs/hardware.md#m5stack-stopwatch)
before the first USB flash. Use the merged image for the first installation;
the app-only file is for compatible OTA tooling. Use your own built image
when updating a customized device.

## Connect a Watch relay

`tools/watch_relay.py` reads the existing Javis603/token-monitor Node Hub.
The watch polls only `/watch/v1/snapshot`. The adapter projects a bounded,
read-only response with no prompts, hostnames, email addresses or credentials.
It recognizes the Token Monitor v0.92.0 Hub contract. See
[the snapshot format](docs/snapshot.md).

Start with `python tools/watch_relay.py --fixture` for synthetic data.
To use a real Hub, provide `STOPWATCH_HUB_URL` and `TOKEN_MONITOR_SECRET`
in the adapter process environment, plus `STOPWATCH_DEVICE_TOKEN` for the
watch. Run the adapter behind your HTTPS reverse proxy on a protected
loopback or service network. The Hub secret is used only by the adapter.

Use the upstream Wi-Fi setup and a **USB console** at 115200 baud to provision
the watch. The `tm status` command reports configuration and HTTP state only.

~~~text
tm endpoint https://relay.example/watch/v1/snapshot
tm token YOUR_WATCH_ONLY_TOKEN
tm refresh
tm status
~~~

For a local Wi-Fi demo with synthetic data, explicitly bind the fixture server
to your LAN address and use `tm lab http://192.168.1.23:17322/watch/v1/snapshot`.
Lab mode accepts private IPv4 addresses only and sends no device credential.
Use a trusted USB connection for provisioning. Keep tokens out of build flags,
source control and screenshots.

The firmware and simulator share the dashboard renderer. Physical touch,
Wi-Fi/TLS on the board, audio and power still need hardware acceptance.
Upstream code and graphics retain their respective license and attribution.
