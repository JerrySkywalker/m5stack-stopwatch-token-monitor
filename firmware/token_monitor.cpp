#include "hg/token_monitor.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <string>

#include "hg/canvas.hpp"
#include "hg/font.hpp"
#include "hg/font_cjk.hpp"

namespace stopwatch {
namespace {

using hg::Canvas;
using hg::json::Value;

constexpr int kRows = 4;
constexpr const char* kPages[] = {"Overview", "Quotas", "Trend", "Devices"};
constexpr const char* kPagesZh[] = {"总览", "额度", "趋势", "设备"};
constexpr std::array<uint32_t, 5> kIntervals{30, kDefaultRefreshSeconds, 120, 300, 900};
constexpr int kSettingsTop = 132;
constexpr int kSettingsStep = 66;
constexpr int kSettingsHeight = 51;
constexpr uint16_t kBackground = hg::rgb565(0, 0, 0);
constexpr uint16_t kPanel = hg::rgb565(12, 23, 30);
constexpr uint16_t kTrack = hg::rgb565(18, 47, 53);
constexpr uint16_t kBright = hg::rgb565(232, 247, 249);
constexpr uint16_t kMuted = hg::rgb565(155, 176, 186);
constexpr uint16_t kTeal = hg::rgb565(68, 230, 203);
constexpr uint16_t kOrange = hg::rgb565(255, 182, 96);

#include "token_monitor_glyphs.inc"

const uint8_t* dashboard_glyph(char32_t character) {
  if (character < 128) return nullptr;
  const auto* point = std::lower_bound(std::begin(kDashboardCodepoints), std::end(kDashboardCodepoints), character);
  if (point == std::end(kDashboardCodepoints) || *point != character) return nullptr;
  return kDashboardBitmaps[point - kDashboardCodepoints];
}

struct TextMeasure {
  size_t bytes;
  size_t columns;
};

TextMeasure measure_text(std::string_view text, size_t limit) {
  TextMeasure measured{0, 0};
  for (size_t next = 0; next < text.size();) {
    const auto character = hg::font::next(text, next);
    const size_t width = dashboard_glyph(character) ? 2 : hg::font::columns(character);
    if (measured.columns + width > limit) break;
    measured = {next, measured.columns + width};
  }
  return measured;
}

int text_width(std::string_view text, int scale) {
  if (text.empty()) return 0;
  return static_cast<int>(measure_text(text, text.size() * 2).columns) * hg::font::kCellWidth * scale - scale;
}

std::string compact(const Value& value) {
  const double amount = value.as_number(-1);
  if (amount < 0 || !std::isfinite(amount)) return "--";
  char text[32];
  if (amount >= 1e12) std::snprintf(text, sizeof(text), "%.2fT", amount / 1e12);
  else if (amount >= 1e9) std::snprintf(text, sizeof(text), "%.2fB", amount / 1e9);
  else if (amount >= 1e6) std::snprintf(text, sizeof(text), "%.2fM", amount / 1e6);
  else if (amount >= 1e3) std::snprintf(text, sizeof(text), "%.1fK", amount / 1e3);
  else std::snprintf(text, sizeof(text), "%.0f", amount);
  return text;
}

std::string usd(const Value& value) {
  const double cost = value.as_number(-1);
  if (cost < 0 || !std::isfinite(cost)) return "--";
  char text[32];
  if (cost < 1000) std::snprintf(text, sizeof(text), "$%.2f", cost);
  else return "$" + compact(value);
  return text;
}

const char* tr(bool zh, const char* english, const char* chinese) {
  return zh ? chinese : english;
}

std::string age(int64_t seconds, bool zh) {
  if (seconds < 0) return "--";
  const char* mins = tr(zh, "m", "分");
  const char* hours = tr(zh, "h", "时");
  if (seconds < 60) return tr(zh, "<1m", "<1分");
  if (seconds < 3600) return std::to_string(seconds / 60) + mins;
  if (seconds < 86400) return std::to_string(seconds / 3600) + hours + " " +
                               std::to_string((seconds / 60) % 60) + mins;
  return std::to_string(seconds / 86400) + tr(zh, "d", "天") + " " +
         std::to_string((seconds / 3600) % 24) + hours;
}

void text_at(Canvas& c, int x, int y, std::string_view text, int scale, uint16_t color) {
  constexpr int size = hg::font::kCjkSize;
  for (size_t i = 0; i < text.size();) {
    const size_t start = i;
    const auto character = hg::font::next(text, i);
    if (const uint8_t* glyph = dashboard_glyph(character)) {
      for (int bit = 0; bit < size * size; ++bit) {
        if ((glyph[bit >> 3] >> (7 - (bit & 7))) & 1)
          c.fill_rect(x + (bit % size) * scale, y + (bit / size - 3) * scale, scale, scale, color);
      }
      x += size * scale;
    } else {
      x = c.text(x, y - hg::font::kAsciiYOffset * scale, text.substr(start, i - start), scale, color);
    }
  }
}

void centered(Canvas& c, int y, std::string_view text, int scale, uint16_t color) {
  text_at(c, (c.width() - text_width(text, scale)) / 2, y, text, scale, color);
}

void right(Canvas& c, int x, int y, std::string_view text, int scale, uint16_t color) {
  text_at(c, x - text_width(text, scale), y, text, scale, color);
}

void name_at(Canvas& c, int x, int y, std::string_view title, size_t columns, uint16_t color = kBright) {
  if (measure_text(title, columns).bytes < title.size()) {
    const auto cropped = std::string(title.substr(0, measure_text(title, columns - 1).bytes)) + "~";
    text_at(c, x, y, cropped, 2, color);
    return;
  }
  text_at(c, x, y, title, 2, color);
}

void card(Canvas& c, int y, int height) {
  c.fill_round_rect(76, y, 314, height, 9, kPanel);
}

void settings_icon(Canvas& c) {
  constexpr int x = 350, y = 91;
  c.fill_rect(x - 2, y - 11, 5, 23, kMuted);
  c.fill_rect(x - 11, y - 2, 23, 5, kMuted);
  c.fill_circle(x, y, 8, kMuted);
  c.fill_circle(x, y, 4, kBackground);
}

void overview(Canvas& c, const Value& data, bool models, bool zh) {
  centered(c, 123, tr(zh, "TODAY TOKENS", "今日用量"), 2, kMuted);
  centered(c, 145, compact(data["today"]["tokens"]), 5, kBright);
  centered(c, 190, usd(data["today"]["cost"]) + tr(zh, " est.", " 预估"), 2, kTeal);
  card(c, 217, 32);
  text_at(c, 91, 226, tr(zh, "MONTH", "本月"), 2, kMuted);
  right(c, 376, 226, compact(data["month"]["tokens"]) + " / " + usd(data["month"]["cost"]), 2, kBright);
  text_at(c, 82, 264, models ? tr(zh, "TOP MODELS", "模型排行") : tr(zh, "TOP TOOLS", "工具排行"), 2, kMuted);
  const auto& rows = data[models ? "models" : "tools"].elements();
  if (rows.empty()) centered(c, 298, tr(zh, "No breakdown", "暂无明细"), 2, kMuted);
  for (size_t index = 0; index < std::min<size_t>(3, rows.size()); ++index) {
    const int y = 290 + static_cast<int>(index) * 24;
    name_at(c, 87, y, rows[index]["label"].as_string(), 18);
    right(c, 378, y, compact(rows[index]["tokens"]), 2, kTeal);
  }
  centered(c, 365, models ? tr(zh, "Tap to see tools", "点按切换到工具")
                          : tr(zh, "Tap to see models", "点按切换到模型"), 2, kMuted);
}

void quotas(Canvas& c, const Value& rows, size_t block, int64_t now, bool zh) {
  if (!rows.size()) centered(c, 213, tr(zh, "No quota data", "暂无额度数据"), 2, kMuted);
  const size_t start = block * kRows;
  for (size_t index = start; index < std::min(rows.size(), start + kRows); ++index) {
    const auto& row = rows[index];
    const int y = 125 + static_cast<int>(index - start) * 58;
    const int64_t reset = row["reset"].as_int();
    const bool stale = row["stale"].as_bool() || (reset && reset <= now);
    const double pct = row["pct"].as_number(-1);
    const uint16_t accent = stale || (pct >= 0 && pct < 25) ? kOrange : kTeal;
    card(c, y, 52);
    name_at(c, 87, y + (zh ? 5 : 7), row["label"].as_string(), 25);
    const int status_y = y + (zh ? 25 : 28);
    const std::string amount = pct >= 0 && pct <= 100
        ? tr(zh, "", "剩余 ") + std::to_string(static_cast<int>(std::round(pct))) + tr(zh, "% left", "%")
        : "--";
    right(c, 378, status_y, amount, 2, accent);
    name_at(c, 87, status_y, stale ? tr(zh, "Stale", "已过期")
                                : reset ? tr(zh, "Reset ", "重置 ") + age(reset - now, zh) : "", 15, kMuted);
    c.fill_rect(87, y + 46, 291, 2, kTrack);
    if (pct >= 0 && pct <= 100) c.fill_rect(87, y + 46, static_cast<int>(291 * pct / 100), 2, accent);
  }
  if (rows.size() > kRows) centered(c, 365, tr(zh, "Swipe up for more", "上滑查看更多"), 2, kMuted);
}

void trend(Canvas& c, const Value& rows, size_t detail, bool zh) {
  const size_t count = detail % 2 ? 30 : 7;
  const std::string_view field = detail >= 2 ? "cost" : "tokens";
  centered(c, 126, std::to_string(count) + tr(zh, " DAYS / ", " 天 / ") +
           (detail >= 2 ? tr(zh, "EST. COST", "预估费用") : tr(zh, "TOKENS", "用量")), 2, kMuted);
  if (rows.size() == 0) {
    centered(c, 217, tr(zh, "No history data", "暂无历史数据"), 2, kMuted);
    return;
  }
  const size_t start = rows.size() > count ? rows.size() - count : 0;
  double largest = 0, total = 0;
  int known = 0;
  for (size_t i = start; i < rows.size(); ++i) {
    const double amount = rows[i][field].as_number(-1);
    if (amount >= 0) {
      largest = std::max(largest, amount);
      total += amount;
      ++known;
    }
  }
  centered(c, 155, !known ? "--" : detail >= 2 ? usd(Value(total)) : compact(Value(total)), 4, kBright);
  constexpr int bottom = 310;
  c.fill_rect(86, bottom, 294, 1, kTrack);
  const int step = 294 / static_cast<int>(count);
  for (size_t i = start; i < rows.size(); ++i) {
    const double amount = rows[i][field].as_number(-1);
    const int x = 86 + static_cast<int>(i - start) * step;
    const int width = std::max(2, step - 3);
    if (amount < 0) {
      c.fill_rect(x, bottom + 3, width, 2, kOrange);
    } else {
      const int h = largest > 0 ? std::max(1, static_cast<int>(100 * amount / largest)) : 1;
      c.fill_rect(x, bottom - h, width, h, kTeal);
    }
  }
  name_at(c, 86, 329, rows[start]["day"].as_string(), 5, kMuted);
  right(c, 380, 329, rows[rows.size() - 1]["day"].as_string(), 2, kMuted);
  centered(c, 365, known < static_cast<int>(count) ? tr(zh, "Gaps = no reading", "短线表示缺少数据")
             : tr(zh, "Tap for range / cost", "点按切换时段和费用"), 2, kMuted);
}

void devices(Canvas& c, const Value& rows, size_t block, int64_t now, bool zh) {
  if (!rows.size()) centered(c, 213, tr(zh, "No device data", "暂无设备数据"), 2, kMuted);
  const size_t start = block * kRows;
  for (size_t index = start; index < std::min(rows.size(), start + kRows); ++index) {
    const auto& row = rows[index];
    const int y = 125 + static_cast<int>(index - start) * 58;
    const int64_t seen = row["updated"].as_int();
    card(c, y, 52);
    const auto label = row["label"].as_string();
    const int title_y = y + (zh ? 6 : 7);
    const int status_y = y + (zh ? 32 : 29);
    name_at(c, 88, title_y, zh && label.substr(0, 7) == "Device " ? "设备 " + label.substr(7) : label, 18);
    right(c, 377, y + 7, compact(row["tokens"]), 2, kBright);
    const bool stale = row["stale"].as_bool();
    c.fill_circle(92, status_y + 6, 4, stale ? kOrange : kTeal);
    text_at(c, 105, status_y, stale ? tr(zh, "Stale", "已过期") : tr(zh, "Reporting", "上报正常"), 2, stale ? kOrange : kTeal);
    right(c, 377, status_y, !seen ? "--" : age(std::max<int64_t>(0, now - seen), zh) + tr(zh, " ago", "前"), 2, kMuted);
  }
  if (rows.size() > kRows) centered(c, 365, tr(zh, "Swipe up for more", "上滑查看更多"), 2, kMuted);
}

void settings(Canvas& c, bool zh, uint32_t seconds, bool network) {
  const auto row_y = [](int row) { return kSettingsTop + row * kSettingsStep; };
  const std::string interval = seconds < 60 ? std::to_string(seconds) + tr(zh, " sec", " 秒")
                                            : std::to_string(seconds / 60) + tr(zh, " min", " 分钟");
  for (int row = 0; row < 3; ++row) card(c, row_y(row), kSettingsHeight);
  text_at(c, 94, row_y(0) + 18, tr(zh, "Language", "语言"), 2, kBright);
  right(c, 371, row_y(0) + 18, tr(zh, "English", "简体中文"), 2, kTeal);
  text_at(c, 94, row_y(1) + 18, tr(zh, "Refresh interval", "拉取间隔"), 2, kBright);
  right(c, 371, row_y(1) + 18, interval, 2, kTeal);
  text_at(c, 94, row_y(2) + 18, tr(zh, "Refresh now", "立即刷新"), 2, kBright);
  right(c, 371, row_y(2) + 18, ">", 2, kTeal);
  centered(c, 362, tr(zh, "< Back", "< 返回"), 2, kBright);
  centered(c, 406, network ? tr(zh, "Network online", "网络已连接")
                            : tr(zh, "Offline", "网络未连接"), 2, network ? kMuted : kOrange);
}

}  // namespace

bool valid_refresh_seconds(uint32_t seconds) {
  return std::find(kIntervals.begin(), kIntervals.end(), seconds) != kIntervals.end();
}

uint32_t next_refresh_seconds(uint32_t seconds) {
  const auto current = std::find(kIntervals.begin(), kIntervals.end(), seconds);
  if (current == kIntervals.end() || current + 1 == kIntervals.end()) return kIntervals.front();
  return *(current + 1);
}

void TokenMonitor::enable(bool enabled, bool combined) {
  enabled_ = enabled;
  combined_ = combined;
  hidden_ = false;
  settings_open_ = false;
  dirty_ = true;
}

void TokenMonitor::hide(bool hidden) {
  hidden_ = hidden;
  dirty_ = true;
}

bool TokenMonitor::ingest(std::string_view body, uint32_t now) {
  Value next;
  if (body.size() > kMaxSnapshot || !hg::json::parse(body, next, nullptr, 8) ||
      next["version"].as_number() != 1.0 || next["asOf"].as_int() <= 0 ||
      !next["today"].is_object() || !next["month"].is_object() ||
      !next["tools"].is_array() || next["tools"].size() > 4 ||
      !next["models"].is_array() || next["models"].size() > 4 ||
      !next["limits"].is_array() || next["limits"].size() > 12 ||
      !next["trend"].is_array() || next["trend"].size() > 30 ||
      !next["devices"].is_array() || next["devices"].size() > 12) {
    failure();
    return false;
  }
  data_ = std::move(next);
  received_at_ = now;
  failed_ = false;
  if (page_ == 1 || page_ == 3) {
    const size_t count = data_[page_ == 1 ? "limits" : "devices"].size();
    detail_ = std::min(detail_, count ? (count - 1) / kRows : 0);
  }
  dirty_ = true;
  return true;
}

void TokenMonitor::failure() {
  if (!failed_) dirty_ = true;
  failed_ = true;
}

void TokenMonitor::reset() {
  data_ = Value();
  detail_ = 0;
  failed_ = false;
  dirty_ = true;
  refresh();
}

const char* TokenMonitor::page_name() const { return settings_open_ ? "Settings" : kPages[page_]; }

void TokenMonitor::open_settings(bool open) {
  settings_open_ = open;
  dirty_ = true;
}

void TokenMonitor::set_language(Language language) {
  language_ = language;
  dirty_ = true;
}

void TokenMonitor::set_refresh_seconds(uint32_t seconds) {
  if (seconds == refresh_seconds_) return;
  refresh_seconds_ = seconds;
  refresh();
  dirty_ = true;
}

void TokenMonitor::next(int step) {
  if (settings_open_) {
    open_settings(false);
    return;
  }
  page_ = (page_ + step + 4) % 4;
  detail_ = 0;
  dirty_ = true;
}

void TokenMonitor::detail() {
  if (settings_open_) return;
  size_t pages = page_ == 2 ? 4 : 2;
  if (page_ == 1 || page_ == 3) {
    const size_t count = data_[page_ == 1 ? "limits" : "devices"].size();
    pages = std::max<size_t>(1, (count + kRows - 1) / kRows);
  }
  detail_ = (detail_ + 1) % pages;
  dirty_ = true;
}

TouchAction TokenMonitor::touch(bool down, int x, int y, uint32_t now) {
  if (down) {
    if (!touching_) {
      first_x_ = x;
      first_y_ = y;
      touch_at_ = now;
    }
    touching_ = true;
    last_x_ = x;
    last_y_ = y;
    return TouchAction::None;
  }
  if (!touching_) return TouchAction::None;
  touching_ = false;
  const int dx = last_x_ - first_x_;
  const int dy = last_y_ - first_y_;
  const bool tap = std::abs(dx) < 18 && std::abs(dy) < 18;
  const bool header = first_y_ < 118 && first_x_ >= 96 && first_x_ <= 370;
  const uint32_t held = now - touch_at_;
  const bool short_tap = tap && held < 550;
  if (tap && first_y_ < 118 && held >= 650) return TouchAction::HermesSettings;
  if (settings_open_) {
    if ((dx >= 42 && dx > std::abs(dy)) || (short_tap && (header || (first_y_ >= 331 && first_y_ < 398)))) {
      open_settings(false);
    } else if (short_tap && first_x_ >= 76 && first_x_ <= 390 && first_y_ >= kSettingsTop) {
      const int offset = first_y_ - kSettingsTop;
      if (offset % kSettingsStep < kSettingsHeight) {
        switch (offset / kSettingsStep) {
          case 0: return TouchAction::Language;
          case 1: return TouchAction::Interval;
          case 2:
            refresh();
            open_settings(false);
            break;
        }
      }
    }
  } else if (std::abs(dx) >= 42 && std::abs(dx) > std::abs(dy)) {
    next(dx < 0 ? 1 : -1);
  } else if (dy < -38 && std::abs(dy) > std::abs(dx)) {
    detail();
  } else if (tap) {
    if (header && short_tap) open_settings(true);
    else if (first_y_ >= 391) refresh();
    else if (held < 550 && first_y_ >= 118) detail();
  }
  return TouchAction::None;
}

void TokenMonitor::draw(hg::Display& display, uint32_t now, bool network, bool force) {
  const auto info = display.info();
  if (info.width != 466 || info.height != 466 || display.framebuffer_rows() != 0) return;
  const bool redraw = display.take_redraw_request();
  if (!dirty_ && network == network_ && !force && !redraw && now - last_frame_ < 15000) return;
  dirty_ = false;
  network_ = network;
  last_frame_ = now;
  Canvas c(display.framebuffer(), info.width, info.height, info.swap_bytes);
  c.fill_rect(0, 0, info.width, info.height, kBackground);
  c.ring(233, 233, 224, 2, kTrack);
  c.arc(233, 233, 224, 3, page_ % 2 ? -1 : 1, kTeal);
  const bool zh = language_ == Language::Chinese;
  centered(c, zh ? 47 : 57, tr(zh, "TOKEN MONITOR", "用量监控"), 2, kMuted);
  centered(c, 82, settings_open_ ? tr(zh, "Settings", "设置") : (zh ? kPagesZh : kPages)[page_], 3, kBright);
  if (settings_open_) {
    settings(c, zh, refresh_seconds_, network);
    display.flush(0, info.height);
    return;
  }
  settings_icon(c);

  const bool present = data_.is_object();
  const int64_t elapsed = static_cast<uint32_t>(now - received_at_) / 1000;
  const int64_t server_now = data_["asOf"].as_int() + elapsed;
  if (present) {
    switch (page_) {
      case 0: overview(c, data_, detail_ != 0, zh); break;
      case 1: quotas(c, data_["limits"], detail_, server_now, zh); break;
      case 2: trend(c, data_["trend"], detail_, zh); break;
      case 3: devices(c, data_["devices"], detail_, server_now, zh); break;
    }
  } else {
    centered(c, 190, failed_ ? tr(zh, "Relay unavailable", "中继不可用")
                             : tr(zh, "Waiting for data", "正在等待数据"), 2, kBright);
    centered(c, 224, network ? tr(zh, "Set relay via USB", "通过 USB 配置中继")
                              : tr(zh, "Connect Wi-Fi", "请连接 Wi-Fi"), 2, kMuted);
    centered(c, 252, tr(zh, "Tap title for settings", "点按标题进入设置"), 2, kMuted);
  }
  for (int i = 0; i < 4; ++i) c.fill_circle(209 + 16 * i, 392, i == page_ ? 4 : 2, i == page_ ? kTeal : kMuted);
  const bool stale = failed_ || (present && elapsed >= 3 * refresh_seconds_) || !network;
  centered(c, 406, !present ? (network ? tr(zh, "Tap to refresh", "点按刷新")
                                       : tr(zh, "Offline", "离线"))
                            : (stale ? tr(zh, "Cached ", "缓存 ") : tr(zh, "Updated ", "更新 ")) +
                                  age(elapsed, zh) + tr(zh, " ago", "前"),
           2, stale ? kOrange : kMuted);
  display.flush(0, info.height);
}

}  // namespace stopwatch
