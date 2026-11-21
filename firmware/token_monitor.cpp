#include "hg/token_monitor.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "hg/canvas.hpp"

namespace stopwatch {
namespace {

using hg::Canvas;
using hg::json::Value;

constexpr int kRows = 4;
constexpr const char* kPages[] = {"Overview", "Quotas", "Trend", "Devices"};
constexpr uint16_t kBackground = hg::rgb565(0, 0, 0);
constexpr uint16_t kPanel = hg::rgb565(12, 23, 30);
constexpr uint16_t kTrack = hg::rgb565(18, 47, 53);
constexpr uint16_t kBright = hg::rgb565(232, 247, 249);
constexpr uint16_t kMuted = hg::rgb565(155, 176, 186);
constexpr uint16_t kTeal = hg::rgb565(68, 230, 203);
constexpr uint16_t kOrange = hg::rgb565(255, 182, 96);

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

std::string age(int64_t seconds) {
  if (seconds < 0) return "--";
  if (seconds < 60) return "<1m";
  if (seconds < 3600) return std::to_string(seconds / 60) + "m";
  if (seconds < 86400) return std::to_string(seconds / 3600) + "h " +
                               std::to_string((seconds / 60) % 60) + "m";
  return std::to_string(seconds / 86400) + "d " + std::to_string((seconds / 3600) % 24) + "h";
}

void centered(Canvas& c, int y, std::string_view text, int scale, uint16_t color) {
  c.text((c.width() - Canvas::text_width(text, scale)) / 2, y, text, scale, color);
}

void right(Canvas& c, int x, int y, std::string_view text, int scale, uint16_t color) {
  c.text(x - Canvas::text_width(text, scale), y, text, scale, color);
}

void name_at(Canvas& c, int x, int y, std::string_view title, size_t max_chars, uint16_t color = kBright) {
  std::string cropped(title.substr(0, max_chars));
  for (char& chr : cropped) {
    if (chr < ' ' || chr > '~') chr = '?';
  }
  if (title.size() > max_chars && cropped.size() > 1) cropped.back() = '~';
  c.text(x, y, cropped, 2, color);
}

void card(Canvas& c, int y, int height) {
  c.fill_round_rect(76, y, 314, height, 9, kPanel);
}

void overview(Canvas& c, const Value& data, bool models) {
  centered(c, 123, "TODAY TOKENS", 2, kMuted);
  centered(c, 145, compact(data["today"]["tokens"]), 5, kBright);
  centered(c, 190, usd(data["today"]["cost"]) + " est.", 2, kTeal);
  card(c, 217, 32);
  c.text(91, 226, "MONTH", 2, kMuted);
  right(c, 376, 226, compact(data["month"]["tokens"]) + " / " + usd(data["month"]["cost"]), 2, kBright);
  c.text(82, 264, models ? "TOP MODELS" : "TOP TOOLS", 2, kMuted);
  const auto& rows = data[models ? "models" : "tools"].elements();
  if (rows.empty()) centered(c, 298, "No breakdown", 2, kMuted);
  for (size_t index = 0; index < std::min<size_t>(3, rows.size()); ++index) {
    const int y = 290 + static_cast<int>(index) * 24;
    name_at(c, 87, y, rows[index]["label"].as_string(), 18);
    right(c, 378, y, compact(rows[index]["tokens"]), 2, kTeal);
  }
  centered(c, 368, models ? "Tap to see tools" : "Tap to see models", 2, kMuted);
}

void quotas(Canvas& c, const Value& rows, size_t block, int64_t now) {
  if (!rows.size()) centered(c, 213, "No quota data", 2, kMuted);
  const size_t start = block * kRows;
  for (size_t index = start; index < std::min(rows.size(), start + kRows); ++index) {
    const auto& row = rows[index];
    const int y = 125 + static_cast<int>(index - start) * 58;
    const int64_t reset = row["reset"].as_int();
    const bool stale = row["stale"].as_bool() || (reset && reset <= now);
    const double pct = row["pct"].as_number(-1);
    const uint16_t accent = stale || (pct >= 0 && pct < 25) ? kOrange : kTeal;
    card(c, y, 52);
    name_at(c, 87, y + 7, row["label"].as_string(), 25);
    const std::string amount = pct >= 0 && pct <= 100
        ? std::to_string(static_cast<int>(std::round(pct))) + "% left"
        : "--";
    right(c, 378, y + 28, amount, 2, accent);
    name_at(c, 87, y + 28, stale ? "Stale" : reset ? "Reset " + age(reset - now) : "", 15, kMuted);
    c.fill_rect(87, y + 46, 291, 2, kTrack);
    if (pct >= 0 && pct <= 100) c.fill_rect(87, y + 46, static_cast<int>(291 * pct / 100), 2, accent);
  }
  if (rows.size() > kRows) centered(c, 370, "Swipe up for more", 2, kMuted);
}

void trend(Canvas& c, const Value& rows, size_t detail) {
  const size_t count = detail % 2 ? 30 : 7;
  const std::string_view field = detail >= 2 ? "cost" : "tokens";
  centered(c, 126, std::to_string(count) + " DAYS / " + (detail >= 2 ? "EST. COST" : "TOKENS"), 2, kMuted);
  if (rows.size() == 0) {
    centered(c, 217, "No history data", 2, kMuted);
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
  centered(c, 368, known < static_cast<int>(count) ? "Gaps = no reading" : "Tap for range / cost", 2, kMuted);
}

void devices(Canvas& c, const Value& rows, size_t block, int64_t now) {
  if (!rows.size()) centered(c, 213, "No device data", 2, kMuted);
  const size_t start = block * kRows;
  for (size_t index = start; index < std::min(rows.size(), start + kRows); ++index) {
    const auto& row = rows[index];
    const int y = 125 + static_cast<int>(index - start) * 58;
    const int64_t seen = row["updated"].as_int();
    card(c, y, 52);
    name_at(c, 88, y + 7, row["label"].as_string(), 18);
    right(c, 377, y + 7, compact(row["tokens"]), 2, kBright);
    const bool stale = row["stale"].as_bool();
    c.fill_circle(92, y + 35, 4, stale ? kOrange : kTeal);
    c.text(105, y + 29, stale ? "Stale" : "Reporting", 2, stale ? kOrange : kTeal);
    right(c, 377, y + 29, !seen ? "--" : age(std::max<int64_t>(0, now - seen)) + " ago", 2, kMuted);
  }
  if (rows.size() > kRows) centered(c, 370, "Swipe up for more", 2, kMuted);
}

}  // namespace

void TokenMonitor::enable(bool enabled, bool combined) {
  enabled_ = enabled;
  combined_ = combined;
  hidden_ = false;
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

const char* TokenMonitor::page_name() const { return kPages[page_]; }

void TokenMonitor::next(int step) {
  page_ = (page_ + step + 4) % 4;
  detail_ = 0;
  dirty_ = true;
}

void TokenMonitor::detail() {
  size_t pages = page_ == 2 ? 4 : 2;
  if (page_ == 1 || page_ == 3) {
    const size_t count = data_[page_ == 1 ? "limits" : "devices"].size();
    pages = std::max<size_t>(1, (count + kRows - 1) / kRows);
  }
  detail_ = (detail_ + 1) % pages;
  dirty_ = true;
}

void TokenMonitor::touch(bool down, int x, int y, uint32_t now, bool& settings) {
  if (down) {
    if (!touching_) {
      first_x_ = x;
      first_y_ = y;
      touch_at_ = now;
    }
    touching_ = true;
    last_x_ = x;
    last_y_ = y;
    return;
  }
  if (!touching_) return;
  touching_ = false;
  const int dx = last_x_ - first_x_;
  const int dy = last_y_ - first_y_;
  if (std::abs(dx) >= 42 && std::abs(dx) > std::abs(dy)) {
    next(dx < 0 ? 1 : -1);
  } else if (dy < -38 && std::abs(dy) > std::abs(dx)) {
    detail();
  } else if (std::abs(dx) < 18 && std::abs(dy) < 18) {
    if (first_y_ < 118 && now - touch_at_ >= 650) settings = true;
    else if (first_y_ >= 391) refresh();
    else if (now - touch_at_ < 550 && first_y_ >= 118) detail();
  }
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
  centered(c, 59, "TOKEN MONITOR", 2, kMuted);
  centered(c, 82, page_name(), 3, kBright);

  const bool present = data_.is_object();
  const int64_t elapsed = static_cast<uint32_t>(now - received_at_) / 1000;
  const int64_t server_now = data_["asOf"].as_int() + elapsed;
  if (present) {
    switch (page_) {
      case 0: overview(c, data_, detail_ != 0); break;
      case 1: quotas(c, data_["limits"], detail_, server_now); break;
      case 2: trend(c, data_["trend"], detail_); break;
      case 3: devices(c, data_["devices"], detail_, server_now); break;
    }
  } else {
    centered(c, 190, failed_ ? "Relay unavailable" : "Waiting for data", 2, kBright);
    centered(c, 224, network ? "Set relay via USB" : "Connect Wi-Fi", 2, kMuted);
    centered(c, 252, "Hold title for settings", 2, kMuted);
  }
  for (int i = 0; i < 4; ++i) c.fill_circle(209 + 16 * i, 392, i == page_ ? 4 : 2, i == page_ ? kTeal : kMuted);
  const bool stale = failed_ || (present && elapsed >= 180) || !network;
  centered(c, 406, !present ? (network ? "Tap to refresh" : "Offline")
                            : (stale ? "Cached " : "Updated ") + age(elapsed) + " ago",
           2, stale ? kOrange : kMuted);
  display.flush(0, info.height);
}

}  // namespace stopwatch
