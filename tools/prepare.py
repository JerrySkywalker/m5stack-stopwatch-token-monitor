"""Materialize a build tree from the pinned Hermes Gadget SDK and watch sources."""

import argparse
import io
import pathlib
import re
import subprocess
import tarfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
UPSTREAM = ROOT / "upstream" / "hermes-gadget-sdk"
PIN = "5160f509b285821a8b999ec365fa49f158d4a5e9"


def insert(text, old, new, filename):
    if text.count(old) != 1:
        raise ValueError(f"{filename}: expected exactly one integration point: {old!r}")
    return text.replace(old, new, 1)


def update(destination, data):
    if not destination.is_file() or destination.read_bytes() != data:
        destination.parent.mkdir(parents=True, exist_ok=True)
        destination.write_bytes(data)


def dashboard_glyphs(upstream_font, dashboard):
    def hex_values(name):
        table = re.search(rf"{name}\[\]\s*=\s*\{{([^}}]+)\}};", upstream_font)
        if not table:
            raise ValueError(f"Missing {name} in SDK font")
        return [int(value, 16) for value in re.findall(r"0x[\dA-Fa-f]+", table[1])]

    codepoints = hex_values("kCjkCodepoints")
    bitmaps = hex_values("kCjkBitmaps")
    literals = re.findall(r'"(?:[^"\\]|\\.)*"', dashboard)
    needed = sorted({ord(character) for literal in literals for character in literal if ord(character) > 127})
    if len(bitmaps) != 18 * len(codepoints):
        raise ValueError("SDK font does not contain 18 bytes per glyph")
    index = {cp: i for i, cp in enumerate(codepoints)}
    missing = set(needed) - index.keys()
    if not needed or missing:
        raise ValueError(f"Dashboard glyphs missing from the SDK font: {sorted(missing)}")

    points = ", ".join(f"0x{cp:04X}" for cp in needed)
    rows = []
    for cp in needed:
        start = index[cp] * 18
        pixels = ", ".join(f"0x{byte:02X}" for byte in bitmaps[start:start + 18])
        rows.append(f"    {{{pixels}}},")
    return (
        "// Subset of the SDK's Fusion Pixel Font bitmaps (SIL OFL 1.1).\n"
        f"constexpr char32_t kDashboardCodepoints[] = {{{points}}};\n"
        "constexpr uint8_t kDashboardBitmaps[][18] = {\n"
        + "\n".join(rows)
        + "\n};\n"
    ).encode("ascii")


def prepare(output, profile):
    output = output.resolve()
    if output == ROOT or output in ROOT.parents or ROOT / "upstream" in (output, *output.parents):
        raise ValueError("Use a dedicated build output directory outside the upstream checkout")
    if UPSTREAM not in (output, *output.parents) and UPSTREAM.is_dir() and any(UPSTREAM.iterdir()):
        head = subprocess.check_output(
            ["git", "-C", str(UPSTREAM), "rev-parse", "HEAD"], text=True
        ).strip()
    else:
        raise ValueError("Initialize the Hermes Gadget submodule: git submodule update --init")
    if head != PIN:
        raise ValueError(f"The upstream submodule must be checked out at {PIN}")
    marker = output / ".stopwatch-build"
    identity = f"{PIN}\n{profile}\n".encode()
    if output.exists() and not marker.is_file() and any(output.iterdir()):
        raise ValueError("Build output exists and is not a StopWatch build tree")
    if marker.is_file() and marker.read_bytes() != identity:
        raise ValueError("Use separate build directories for distinct profiles and upstream pins")

    patches = {
        "firmware/core/CMakeLists.txt": [
            ("    src/app.cpp\n", "    src/app.cpp\n    src/token_monitor.cpp\n    src/token_monitor_app.cpp\n"),
            ("    src/i18n_es.cpp\n", "    src/i18n_es.cpp\n    src/i18n_zh.cpp\n"),
        ],
        "firmware/CMakeLists.txt": [
            ("add_subdirectory(core)\n", "add_subdirectory(core)\nhg_core_library(hg_core_cjk ON)\n"),
            ("target_link_libraries(hgsim PRIVATE hg_core)", "target_link_libraries(hgsim PRIVATE hg_core_cjk)"),
            ("  hg_core_library(hg_core_cjk ON)\n", ""),
            ("add_executable(hg_cjk_tests tests/main.cpp tests/test_cjk.cpp)",
             "add_executable(hg_cjk_tests tests/main.cpp tests/test_cjk.cpp tests/test_i18n.cpp)"),
        ],
        "firmware/tests/test_i18n.cpp": [
            ("constexpr Panel kPanels[] = {\n",
             "constexpr Panel kPanels[] = {\n#if HG_CJK_FONT\n"
             "    {466, 466, true},  // This firmware enables the taller CJK lines on the StopWatch.\n"
             "#else\n"),
            ("// ESP32-S3-Touch-LCD-1.47 landscape (lcd.font_scale = 2 in board.cpp)\n};",
             "// ESP32-S3-Touch-LCD-1.47 landscape (lcd.font_scale = 2 in board.cpp)\n#endif\n};"),
        ],
        "firmware/core/include/hg/i18n.hpp": [
            ("{ En, Es, Count }", "{ En, Es, Zh, Count }"),
            ("constexpr size_t kLangCount = static_cast<size_t>(Lang::Count);",
             """#if HG_CJK_FONT
constexpr size_t kLangCount = static_cast<size_t>(Lang::Count);
#else
constexpr size_t kLangCount = static_cast<size_t>(Lang::Zh);
#endif"""),
        ],
        "firmware/core/src/i18n.cpp": [
            ("extern const size_t kSpanishCount;", "extern const size_t kSpanishCount;\nextern const Entry kChinese[];\nextern const size_t kChineseCount;"),
            ('    {"es", "Español"},', '    {"es", "Español"},\n#if HG_CJK_FONT\n    {"zh", "简体中文"},\n#endif'),
            ("    case Lang::Es: return {kSpanish, kSpanishCount};",
             "    case Lang::Es: return {kSpanish, kSpanishCount};\n#if HG_CJK_FONT\n"
             "    case Lang::Zh: return {kChinese, kChineseCount};\n#endif"),
        ],
        "firmware/core/include/hg/app.hpp": [
            ('#include "hg/ui.hpp"', '#include "hg/ui.hpp"\n#include "hg/token_monitor.hpp"'),
            ("  void on_button(Button button, bool pressed, bool touch = false);",
             """  void on_button(Button button, bool pressed, bool touch = false);
  void token_monitor_enable(bool enabled, bool combined);
  bool token_monitor_network_ready() const {
    return token_monitor_.enabled() && network_up_ && !tm_endpoint_.empty();
  }
  const std::string& token_monitor_endpoint() const { return tm_endpoint_; }
  const std::string& token_monitor_token() const { return tm_token_; }
  uint32_t token_monitor_refresh_id() const { return token_monitor_.refresh_id(); }
  uint32_t token_monitor_refresh_seconds() const { return token_monitor_.refresh_seconds(); }
  bool token_monitor_apply(std::string_view body, int status);
  bool token_monitor_touch(bool down, int x, int y, uint32_t when, bool allow_start);"""),
            ("  std::string server_url_;", """  std::string server_url_;
  stopwatch::TokenMonitor token_monitor_;
  std::string tm_endpoint_, tm_token_;
  bool token_monitor_rendered_ = false;
  bool tm_touch_wake_ = false;
  int tm_http_status_ = 0;
  void token_monitor_load();
  void token_monitor_sync_language();
  bool token_monitor_set_language(stopwatch::Language language);
  bool token_monitor_set_interval(uint32_t seconds);
  bool token_monitor_screen(Screen screen) const;
  std::string token_monitor_command(std::string_view args);"""),
        ],
        "firmware/core/src/app.cpp": [
            ("  load_settings();\n  agent_profile_ =", "  token_monitor_load();\n  load_settings();\n  agent_profile_ ="),
            ("  lang_ = lang_auto_ ? hermes_lang_.value_or(i18n::Lang::En) : i18n::parse(language).value_or(i18n::Lang::En);",
             "  lang_ = lang_auto_ ? hermes_lang_.value_or(i18n::Lang::En) : i18n::parse(language).value_or(i18n::Lang::En);\n"
             "  token_monitor_sync_language();"),
            ("  if (lang_auto_) lang_ = hermes_lang_.value_or(i18n::Lang::En);",
             "  if (lang_auto_) {\n    lang_ = hermes_lang_.value_or(i18n::Lang::En);\n    token_monitor_sync_language();\n  }"),
            ("void App::connect_now() {\n  reconnect_pending_ = false;",
             """void App::connect_now() {
  reconnect_pending_ = false;
  if (token_monitor_.enabled() && !token_monitor_.combined()) {
    phase_ = Phase::NoNetwork;
    update_model();
    return;
  }"""),
            ("  if (ui_ && bench_ == Bench::None) ui_->mark_input();\n  if (!wifi_setup_text_.empty())",
             """  if (ui_ && bench_ == Bench::None) ui_->mark_input();
  if (token_monitor_rendered_ && !touch &&
      (button == Button::Up || button == Button::Down ||
       (!token_monitor_.combined() && (button == Button::Talk || button == Button::Cancel)))) {
    if (pressed) {
      const bool consumed_as_wake = wake_display();
      activity_at_ = now();
      clock_showing_ = false;
      if (!consumed_as_wake) {
        if (button == Button::Talk) token_monitor_.refresh();
        else token_monitor_.next(button == Button::Up ? -1 : 1);
      }
      update_model();
    }
    return;
  }
  if (!wifi_setup_text_.empty())"""),
            ("void App::tick() {\n  const uint32_t t = now();",
             """void App::tick() {
  const uint32_t t = now();
  if (token_monitor_rendered_ && !display_sleeping_)
    token_monitor_.draw(*hal_.display, t, network_up_, false);"""),
            ('  if (cmd == "status") return "@status " + status_json();',
             '  if (cmd == "tm") return token_monitor_command(rest);\n  if (cmd == "status") return "@status " + status_json();'),
            ('    if (cmd == "factory-reset") {\n      for (const char* k : kSettingKeys)',
             '''    if (cmd == "factory-reset") {
      for (const char* key : {"tm_lang", "tm_interval", "tm_endpoint", "tm_token", "tm_ring_src"})
        hal_.storage->erase(key);
      for (const char* k : kSettingKeys)'''),
            ('@help commands: status |', '@help commands: status | tm |'),
        ],
        "firmware/core/src/app_settings.cpp": [
            ("    case Menu::Language: {\n      // English,",
             """    case Menu::Language: {
      if (token_monitor_.enabled()) {
        token_monitor_set_language(lang_ == i18n::Lang::Zh ? stopwatch::Language::English
                                                          : stopwatch::Language::Chinese);
        break;
      }
      // English,"""),
        ],
        "firmware/core/src/app_perf.cpp": [
            ("  if (ui_ && bench_ == Bench::None) ui_->render(m);",
             """  if (!ui_ || bench_ != Bench::None) return;
  const bool watch = token_monitor_screen(m.screen);
  if (watch) {
    token_monitor_.draw(*hal_.display, now(), network_up_, !token_monitor_rendered_);
  } else {
    if (token_monitor_rendered_) ui_->invalidate();
    ui_->render(m);
  }
  token_monitor_rendered_ = watch;"""),
        ],
        "firmware/core/src/touch.cpp": [
            ("void TouchGestures::update(bool touching, int x, int y, uint32_t now_ms) {",
             """void TouchGestures::update(bool touching, int x, int y, uint32_t now_ms) {
  if (app_.token_monitor_touch(touching, x, y, now_ms, state_ == State::Idle)) return;"""),
        ],
        "firmware/esp32/main/CMakeLists.txt": [
            ('        "main.cpp"\n', '        "main.cpp"\n        "token_monitor_http.cpp"\n'),
            ('        esp_http_server\n', '        esp_http_server\n        esp_http_client\n'),
        ],
        "firmware/esp32/main/main.cpp": [
            ('#include "port.hpp"', '#include "port.hpp"\n#include "token_monitor_http.hpp"\n#include "stopwatch_profile.hpp"'),
            ("  app.begin();\n", """#if CONFIG_HG_BOARD_STOPWATCH
  app.token_monitor_enable(true, STOPWATCH_COMBINED);
#endif
  app.begin();
"""),
            ("  g_time.set_wanted(app.clock_enabled());  // SNTP only while the clock face is on",
             "  g_time.set_wanted(app.clock_enabled() || STOPWATCH_TLS_TIME);  // Set the clock for TLS."),
            ('    if (key == "clock") g_time.set_wanted(app.clock_enabled());',
             '    if (key == "clock") g_time.set_wanted(app.clock_enabled() || STOPWATCH_TLS_TIME);'),
            ("    app.tick();\n", "    hgp::token_monitor_http_tick(app);\n    app.tick();\n"),
        ],
        "firmware/esp32/main/port_console.cpp": [
            ('"log", "face", "choose"};', '"log", "face", "choose", "tm"};'),
        ],
        "firmware/esp32/platformio.ini": [
            ("platform = espressif32\n", "platform = platformio/espressif32@8.0.0\n"),
            ("[env:m5stack-stopwatch]\nboard = esp32-s3-devkitc-1\n",
             "[env:m5stack-stopwatch]\nboard = esp32-s3-devkitc-1\nboard_build.partitions = partitions-stopwatch.csv\n"),
            ('    -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;boards/m5stack-stopwatch/sdkconfig.defaults"\n',
             '    -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;boards/m5stack-stopwatch/sdkconfig.defaults"\n'
             '    -DHG_CJK_FONT=ON\n'),
        ],
        "firmware/esp32/boards/m5stack-stopwatch/sdkconfig.defaults": [
            ('CONFIG_IDF_TARGET="esp32s3"\n',
             'CONFIG_IDF_TARGET="esp32s3"\nCONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions-stopwatch.csv"\n'),
        ],
    }
    overlays = {
        "firmware/core/include/hg/token_monitor.hpp": "token_monitor.hpp",
        "firmware/core/src/token_monitor.cpp": "token_monitor.cpp",
        "firmware/core/src/token_monitor_app.cpp": "token_monitor_app.cpp",
        "firmware/core/src/i18n_zh.cpp": "i18n_zh.cpp",
        "firmware/esp32/main/token_monitor_http.cpp": "token_monitor_http.cpp",
        "firmware/esp32/main/token_monitor_http.hpp": "token_monitor_http.hpp",
        "firmware/esp32/partitions-stopwatch.csv": "partitions-stopwatch.csv",
    }

    archive = subprocess.check_output(["git", "-C", str(UPSTREAM), "archive", "--format=tar", PIN])
    update(marker, identity)
    with tarfile.open(fileobj=io.BytesIO(archive)) as repo:
        for member in repo:
            if not member.isfile():
                continue
            rel = pathlib.PurePosixPath(member.name)
            if rel.is_absolute() or ".." in rel.parts:
                raise ValueError(f"Invalid upstream archive member: {rel}")
            data = repo.extractfile(member).read()
            if member.name in patches:
                text = data.decode("utf-8")
                for old, new in patches.pop(member.name):
                    text = insert(text, old, new, member.name)
                data = text.encode("utf-8")
            update(output / rel, data)
    if patches:
        raise ValueError(f"Missing upstream files for {', '.join(patches)}")
    for destination, source in overlays.items():
        update(output / destination, (ROOT / "firmware" / source).read_bytes())
    glyphs = dashboard_glyphs(
        (output / "firmware/core/src/font_cjk_data.cpp").read_text(encoding="utf-8"),
        (ROOT / "firmware/token_monitor.cpp").read_text(encoding="utf-8"),
    )
    update(output / "firmware/core/src/token_monitor_glyphs.inc", glyphs)
    definition = f"#pragma once\n#define STOPWATCH_COMBINED {int(profile == 'personal')}\n#if CONFIG_HG_BOARD_STOPWATCH\n#define STOPWATCH_TLS_TIME 1\n#else\n#define STOPWATCH_TLS_TIME 0\n#endif\n"
    update(output / "firmware/esp32/main/stopwatch_profile.hpp", definition.encode())
    print(output)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--profile", choices=("watch", "personal"), default="watch")
    parser.add_argument("--output", type=pathlib.Path)
    args = parser.parse_args()
    prepare(args.output or ROOT / "build" / args.profile, args.profile)


if __name__ == "__main__":
    main()
