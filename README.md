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

The outer ring starts off. In **Settings → Outer ring**, choose a provider
from the current quota snapshot or turn it off. The watch remembers the
selection. When bound, the ring uses that provider's color and shows its
lowest fresh remaining percentage within **Short-term** or **Cycle**.
If the selected provider has no fresh quota for that view, the ring stays
gray and the header shows `--`.

Swipe horizontally to change views. Tap the body or swipe up to cycle the
current view's details. Tap the footer to refresh. Hold the title and
release to open the Hermes Gadget hardware settings. The standalone blue button refreshes and
the yellow button advances a page. In the personal profile the blue button
retains its Hermes push-to-talk behavior.

## Dashboard settings / 看板设置

Tap the title or gear to open the dashboard settings. **Language** switches
both the dashboard and the Hermes Gadget menus between English and Simplified
Chinese. The Language row in Hermes settings applies the same preference.
An upgrade carries over the watch's previous dashboard language. Both the
StopWatch firmware and the round simulator include the CJK font for Chinese
replies. Tap **Refresh interval**
to cycle through 30 seconds, 1 minute (default), 2 minutes, 5 minutes and
15 minutes. The watch remembers both settings after reboot. **Refresh now**
requests a fresh snapshot and returns to the current dashboard view.
The USB console's `factory-reset` also clears the saved dashboard settings
and relay provisioning on the next boot.

点按标题或齿轮进入设置。「语言」同时切换看板和 Hermes 的简体中文与英文，
也可以在 Hermes 设置里切换。升级会沿用原来的看板语言。点按「拉取间隔」
可在 30 秒、1 分钟（默认）、2 分钟、5 分钟和 15 分钟之间切换，重启后仍保留。
「立即刷新」会请求新的快照，并返回原来的页面。

在「设置 → 外圈」选择要绑定的服务商，也可以关闭外圈。外圈跟随「短时／周期」
切换，显示所选服务商的最低剩余额度。选择会在重启后保留。

## Simulator and local verification

Run verification and firmware builds on your development machine. The local
checks below cover the native core tests and the round-screen integration test.

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
loopback, exercises all four pages and both languages with simulated touch,
checks settings persistence, failure and recovery, and verifies a local Gadget voice-state round trip from the
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
notices and a SHA-256 receipt. The `--profile personal` switch builds the same
dashboard plus the upstream Hermes transport and audio behavior. On Windows,
WSL can run the firmware build in a Linux-native directory and write the
completed artifacts to a Windows path via `/mnt/c`.

The CJK builds use two 2304 KiB application slots. Use a fresh build directory
when moving from the old SDK configuration. An upgrade from the old partition
table needs a USB partition-table update in addition to the app. The address of
the NVS settings partition stays at `0x9000`.

Back up the full original flash using the
[StopWatch upstream instructions](https://github.com/Adolanium/hermes-gadget-sdk/blob/v0.6.0/docs/hardware.md#m5stack-stopwatch)
before the first USB flash. Use the merged image for the first installation;
the app-only file is for compatible OTA tooling. Use your own built image
when updating a customized device.

## Connect a Watch relay

`tools/watch_relay.py` reads the existing Javis603/token-monitor Node Hub.
The watch polls only `/watch/v1/snapshot`. The adapter projects a bounded,
read-only response. It aliases devices by default and excludes prompts,
email addresses and credentials.
It recognizes the Token Monitor v0.92.0 Hub contract. See
[the snapshot format](docs/snapshot.md).

Start with `python tools/watch_relay.py --fixture` for synthetic data.
To use a real Hub, provide `STOPWATCH_HUB_URL` and `TOKEN_MONITOR_SECRET`
in the adapter process environment, plus `STOPWATCH_DEVICE_TOKEN` for the
watch. Run the adapter behind your HTTPS reverse proxy on a protected
loopback or service network. The Hub secret is used only by the adapter.

Pass `--show-device-names` to show the Hub hostnames in the authenticated
watch snapshot. A device with no hostname keeps its numbered label. Long names
may be shortened on the watch.

Use the upstream Wi-Fi setup and a **USB console** at 115200 baud to provision
the watch. The `tm status` command reports configuration and HTTP state without
revealing the relay token.

~~~text
tm endpoint https://relay.example/watch/v1/snapshot
tm token YOUR_WATCH_ONLY_TOKEN
tm refresh
tm status
tm settings
tm language zh
tm interval 120
~~~

For a local Wi-Fi demo with synthetic data, explicitly bind the fixture server
to your LAN address and use `tm lab http://192.168.1.23:17322/watch/v1/snapshot`.
Lab mode accepts private IPv4 addresses only and sends no device credential.
Use a trusted USB connection for provisioning. Keep tokens out of build flags,
source control and screenshots.

The firmware and simulator share the dashboard renderer. Physical touch,
Wi-Fi/TLS on the board, audio and power still need hardware acceptance.
Upstream code and graphics retain their respective license and attribution.
