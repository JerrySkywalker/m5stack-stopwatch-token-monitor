#include "hg/token_monitor.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <string>
#include <vector>

#include "hg/canvas.hpp"
#include "hg/font.hpp"
#include "hg/font_cjk.hpp"

namespace stopwatch {
namespace {

using hg::Canvas;
using hg::json::Value;

constexpr size_t kQuotaRows = 4;
constexpr size_t kLegacyQuotaRows = 12;
constexpr int kQuotaTabsY = 118;
constexpr int kQuotaTabsHeight = 30;
constexpr size_t kDeviceRows = 4;
constexpr const char* kPages[] = {"Overview", "Quotas", "Trend", "Devices"};
constexpr const char* kPagesZh[] = {"总览", "剩余额度", "趋势", "设备"};
constexpr std::array<uint32_t, 5> kIntervals{30, kDefaultRefreshSeconds, 120, 300, 900};
constexpr int kSettingsTop = 126;
constexpr int kSettingsStep = 54;
constexpr int kSettingsHeight = 45;
constexpr int kPickerTop = 124;
constexpr int kPickerStep = 39;
constexpr int kPickerHeight = 33;
constexpr size_t kProviderRows = 6;
constexpr uint16_t kBackground = hg::rgb565(0, 0, 0);
constexpr uint16_t kPanel = hg::rgb565(16, 21, 31);
constexpr uint16_t kTrack = hg::rgb565(39, 47, 63);
constexpr uint16_t kBright = hg::rgb565(240, 244, 252);
constexpr uint16_t kMuted = hg::rgb565(155, 170, 192);
constexpr uint16_t kAccent = hg::rgb565(169, 191, 221);
constexpr uint16_t kNeutral = hg::rgb565(121, 137, 161);
constexpr uint16_t kHealthy = hg::rgb565(102, 217, 158);
constexpr uint16_t kWarning = hg::rgb565(255, 126, 112);

struct SourceStyle {
  uint16_t color;
  const char* label;
};

SourceStyle source_style(std::string_view source) {
  source = source.substr(0, source.find(" #"));
  std::string key;
  for (char ch : source) {
    if (ch >= 'A' && ch <= 'Z') ch += 'a' - 'A';
    if ((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9')) key += ch;
  }
  if (key == "openai") return {hg::rgb565(88, 217, 234), "OpenAI"};
  if (key == "codex" || key == "codexcli" || key == "openaicodex")
    return {hg::rgb565(88, 217, 234), "Codex"};
  if (key == "cursor" || key == "cursorcli") return {hg::rgb565(196, 155, 255), "Cursor"};
  if (key == "gemini" || key == "geminicli") return {hg::rgb565(113, 156, 255), "Gemini"};
  if (key == "opencode" || key == "opencodecli") return {hg::rgb565(255, 130, 183), "OpenCode"};
  if (key == "githubcopilot" || key == "copilot" || key == "githubcopilotcli" || key == "copilotcli")
    return {hg::rgb565(244, 209, 110), "GitHub Copilot"};
  if (key == "antigravity") return {hg::rgb565(145, 215, 137), "Antigravity"};
  return {kNeutral, nullptr};
}

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

void usage_band(Canvas& c, const Value& data, const Value& rows) {
  constexpr int left = 90, y = 210, width = 286;
  const double total = data["today"]["tokens"].as_number();
  if (total <= 0 || !std::isfinite(total) || !rows.size()) return;
  c.fill_rect(left, y, width, 5, kTrack);
  double used = 0;
  int start = 0;
  for (const auto& row : rows.elements()) {
    const double amount = row["tokens"].as_number();
    if (amount <= 0 || !std::isfinite(amount)) continue;
    used = std::min(total, used + amount);
    const int end = static_cast<int>(std::round(width * (used / total)));
    if (end > start) {
      c.fill_rect(left + start, y, std::max(1, end - start - 2), 5,
                  source_style(row["label"].as_string()).color);
      start = end;
    }
  }
}

void overview(Canvas& c, const Value& data, bool models, bool zh) {
  centered(c, 120, tr(zh, "TODAY TOKENS", "今日用量"), 2, kMuted);
  centered(c, 145, compact(data["today"]["tokens"]), 5, kBright);
  centered(c, 185, usd(data["today"]["cost"]) + tr(zh, " est.", " 预估"), 2, kAccent);
  if (!models) usage_band(c, data, data["tools"]);
  card(c, 225, 31);
  text_at(c, 91, 233, tr(zh, "MONTH", "本月"), 2, kMuted);
  right(c, 376, 233, compact(data["month"]["tokens"]) + " / " + usd(data["month"]["cost"]), 2, kBright);
  text_at(c, 87, 270, models ? tr(zh, "TOP MODELS", "模型排行") : tr(zh, "TOP TOOLS", "工具排行"), 2, kMuted);
  const auto& rows = data[models ? "models" : "tools"];
  if (rows.size() == 0) centered(c, 298, tr(zh, "No breakdown", "暂无明细"), 2, kMuted);
  for (size_t index = 0; index < rows.size(); ++index) {
    const int y = 292 + static_cast<int>(index) * 18;
    const auto label = rows[index]["label"].as_string();
    const auto style = models ? SourceStyle{kNeutral, nullptr} : source_style(label);
    c.fill_circle(92, y + 6, 4, style.color);
    name_at(c, 104, y, style.label ? std::string_view(style.label) : std::string_view(label), 15);
    right(c, 378, y, compact(rows[index]["tokens"]), 2, kBright);
  }
  centered(c, 369, models ? tr(zh, "Tap to see tools", "点按切换到工具")
                          : tr(zh, "Tap to see models", "点按切换到模型"), 2, kMuted);
}

std::string_view quota_period(const Value& row) {
  const auto& period = row["period"].as_string();
  if (!period.empty()) return period;

  // Older relays only sent the combined title. Match explicit period words,
  // and leave generic windows unclassified.
  const std::string_view label = row["label"].as_string();
  const auto account = label.find(" #");
  const auto window = account == label.npos ? label.npos : label.find(' ', account + 2);
  if (window == label.npos) return "unknown";
  std::string suffix(label.substr(window + 1));
  for (auto& ch : suffix) {
    if (ch >= 'A' && ch <= 'Z') ch += 'a' - 'A';
  }
  if (suffix == "session" || suffix == "5h" || suffix.find("5-hour") != suffix.npos) return "session";
  if (suffix == "day" || suffix == "daily") return "day";
  if (suffix == "week" || suffix.find("weekly") != suffix.npos) return "week";
  if (suffix == "month" || suffix.find("monthly") != suffix.npos) return "month";
  if (suffix == "billing") return "billing";
  return "unknown";
}

struct QuotaRows {
  std::array<const Value*, 2 * kLegacyQuotaRows> rows{};
  size_t count = 0;
};

QuotaRows quota_rows(const Value& data, bool cycle) {
  QuotaRows result;
  for (auto field : {"limits", "limitsMore"}) {
    for (const auto& row : data[field].elements()) {
      const auto period = quota_period(row);
      const bool matches = cycle ? (period == "week" || period == "month" || period == "billing" || period == "cycle")
                                 : (period == "session" || period == "day");
      if (matches && result.count < result.rows.size()) result.rows[result.count++] = &row;
    }
  }
  return result;
}

bool quota_stale(const Value& row, int64_t now) {
  const int64_t reset = row["reset"].as_int();
  return row["stale"].as_bool() || (reset && reset <= now);
}

std::string_view quota_provider(const Value& row) {
  const std::string_view label = row["label"].as_string();
  const auto style = source_style(label);
  return style.label ? std::string_view(style.label) : label.substr(0, label.find(" #"));
}

std::vector<std::string_view> ring_providers(const Value& data) {
  std::vector<std::string_view> providers{std::string_view{}};
  for (const bool cycle : {false, true}) {
    const auto rows = quota_rows(data, cycle);
    for (size_t index = 0; index < rows.count; ++index) {
      const auto provider = quota_provider(*rows.rows[index]);
      if (std::find(providers.begin(), providers.end(), provider) == providers.end())
        providers.push_back(provider);
    }
  }
  std::sort(providers.begin() + 1, providers.end());
  return providers;
}

const Value* lowest_quota(const Value& data, std::string_view provider, bool cycle, int64_t now) {
  const Value* lowest = nullptr;
  double remaining = 101;
  const auto quotas = quota_rows(data, cycle);
  for (size_t index = 0; index < quotas.count; ++index) {
    const auto& row = *quotas.rows[index];
    const double percent = row["pct"].as_number(-1);
    if (percent >= 0 && percent <= 100 && percent < remaining &&
        !quota_stale(row, now) && quota_provider(row) == provider) {
      lowest = &row;
      remaining = percent;
    }
  }
  return lowest;
}

void quota_ring(Canvas& c, std::string_view provider, const Value* quota, bool cycle, bool zh) {
  constexpr int radius = 222;
  constexpr int start = 120;
  constexpr int sweep = 300;
  constexpr float radians_per_degree = 0.0174532925f;
  const double remaining = quota ? (*quota)["pct"].as_number() : 0;
  const int filled = static_cast<int>(std::round(sweep * remaining / 100));
  const auto style = source_style(provider);
  const auto paint = [&](int through, uint16_t color, int thickness) {
    for (int degree = 0; degree <= through; ++degree) {
      const float angle = (start + degree) * radians_per_degree;
      const int x = 233 + static_cast<int>(std::round(radius * std::cos(angle)));
      const int y = 233 + static_cast<int>(std::round(radius * std::sin(angle)));
      c.fill_circle(x, y, thickness, color);
    }
  };
  paint(sweep, kTrack, 3);
  if (filled > 0) paint(filled, style.color, 4);
  const auto* window = cycle ? tr(zh, "CYCLE", "周期") : tr(zh, "SHORT", "短时");
  const int summary_y = zh ? 53 : 57;
  std::string summary(window);
  if (provider == "GitHub Copilot") provider = "Copilot";
  summary += ' ';
  constexpr size_t provider_columns = 11;
  if (measure_text(provider, provider_columns).bytes < provider.size()) {
    summary += provider.substr(0, measure_text(provider, provider_columns - 1).bytes);
    summary += '~';
  } else {
    summary += provider;
  }
  summary += quota ? " " + std::to_string(static_cast<int>(std::round(remaining))) + "%" : " --";
  centered(c, summary_y, summary, 2, !quota ? kMuted : remaining <= 10 ? kWarning : style.color);
}

std::string quota_caption(const Value& row, bool zh) {
  const auto period = quota_period(row);
  const auto minutes = row["minutes"].as_int();
  if (period == "day") return tr(zh, "Daily", "每日");
  if (period == "week") return tr(zh, "Weekly", "每周");
  if (period == "month") return tr(zh, "Monthly", "月度");
  if (period == "billing") return tr(zh, "Billing", "账期");
  if (period == "cycle")
    return minutes >= 1440 ? std::to_string(minutes / 1440) + "d" : tr(zh, "Cycle", "周期");
  if (minutes <= 0) return tr(zh, "Session", "会话");
  if (minutes < 60) return std::to_string(minutes) + "m";
  const auto rest = minutes % 60;
  return std::to_string(minutes / 60) + "h" + (rest ? std::to_string(rest) + "m" : "");
}

void quota_tabs(Canvas& c, bool cycle, bool zh) {
  c.fill_round_rect(91, kQuotaTabsY, 284, kQuotaTabsHeight, 8, kTrack);
  c.fill_round_rect(cycle ? 233 : 93, kQuotaTabsY + 2, 140, kQuotaTabsHeight - 4, 6,
                    hg::rgb565(58, 74, 98));
  const auto short_label = tr(zh, "Short-term", "短时");
  const auto cycle_label = tr(zh, "Cycle", "周期");
  text_at(c, 163 - text_width(short_label, 2) / 2, kQuotaTabsY + 8, short_label, 2, cycle ? kMuted : kBright);
  text_at(c, 303 - text_width(cycle_label, 2) / 2, kQuotaTabsY + 8, cycle_label, 2, cycle ? kBright : kMuted);
}

void quotas(Canvas& c, const Value& data, size_t block, int64_t now, bool cycle, bool zh) {
  quota_tabs(c, cycle, zh);
  auto rows = quota_rows(data, cycle);
  if (!rows.count) {
    centered(c, 218, cycle ? tr(zh, "No cycle windows", "暂无周期额度")
                           : tr(zh, "No short-term windows", "暂无短时额度"), 2, kMuted);
    return;
  }
  std::stable_sort(rows.rows.begin(), rows.rows.begin() + rows.count, [now](const Value* left, const Value* right) {
    const bool left_stale = quota_stale(*left, now), right_stale = quota_stale(*right, now);
    if (left_stale != right_stale) return !left_stale;
    return (*left)["pct"].as_number(101) < (*right)["pct"].as_number(101);
  });
  const size_t start = block * kQuotaRows;
  for (size_t index = start; index < std::min(rows.count, start + kQuotaRows); ++index) {
    const auto& row = *rows.rows[index];
    const int y = 154 + static_cast<int>(index - start) * 53;
    const int64_t reset = row["reset"].as_int();
    const bool stale = quota_stale(row, now);
    const double pct = row["pct"].as_number(-1);
    const bool known = pct >= 0 && pct <= 100;
    const bool low = known && pct < 25;
    const auto& label = row["label"].as_string();
    const auto style = source_style(label);
    const auto account = label.find(" #");
    const auto title = style.label && account != label.npos ? style.label + label.substr(account) : label;
    card(c, y, 47);
    c.fill_round_rect(82, y + 7, 4, 29, 2, style.color);
    name_at(c, 92, y + 4, title, 25);
    const int status_y = y + 25;
    const std::string amount = known
        ? std::string(low && !stale ? "! " : "") + std::to_string(static_cast<int>(std::round(pct))) + "%"
        : "--";
    right(c, 378, status_y, amount, 2, stale || low ? kWarning : kBright);
    auto status = quota_caption(row, zh);
    if (stale) status += tr(zh, " / Stale", " / 已过期");
    else if (reset) status += " / " + age(reset - now, zh);
    name_at(c, 92, status_y, status, 17, stale ? kWarning : kMuted);
    c.fill_rect(92, y + 43, 286, 2, kTrack);
    if (known && !stale) {
      const int filled = static_cast<int>(286 * pct / 100);
      c.fill_rect(92, y + 43, filled, 2, low ? kWarning : style.color);
    }
  }
  if (rows.count > kQuotaRows) {
    const auto page = std::to_string(block + 1) + "/" + std::to_string((rows.count + kQuotaRows - 1) / kQuotaRows);
    centered(c, 374, page + tr(zh, "  Swipe up", "  上滑翻页"), 2, kMuted);
  }
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
      c.fill_rect(x, bottom + 3, width, 2, kWarning);
    } else {
      const int h = largest > 0 ? std::max(1, static_cast<int>(100 * amount / largest)) : 1;
      c.fill_rect(x, bottom - h, width, h, kAccent);
    }
  }
  name_at(c, 86, 329, rows[start]["day"].as_string(), 5, kMuted);
  right(c, 380, 329, rows[rows.size() - 1]["day"].as_string(), 2, kMuted);
  centered(c, 365, known < static_cast<int>(count) ? tr(zh, "Gaps = no reading", "短线表示缺少数据")
             : tr(zh, "Tap for range / cost", "点按切换时段和费用"), 2, kMuted);
}

void devices(Canvas& c, const Value& rows, size_t block, int64_t now, bool zh) {
  if (!rows.size()) centered(c, 213, tr(zh, "No device data", "暂无设备数据"), 2, kMuted);
  const size_t start = block * kDeviceRows;
  for (size_t index = start; index < std::min(rows.size(), start + kDeviceRows); ++index) {
    const auto& row = rows[index];
    const int y = 125 + static_cast<int>(index - start) * 58;
    const int64_t seen = row["updated"].as_int();
    card(c, y, 52);
    const auto label = row["label"].as_string();
    const int title_y = y + (zh ? 6 : 7);
    const int status_y = y + (zh ? 32 : 29);
    const auto tokens = compact(row["tokens"]);
    const bool alias = !row["named"].as_bool() && label.substr(0, 7) == "Device ";
    const int available = 377 - 88 - 12 - text_width(tokens, 2);
    name_at(c, 88, title_y, zh && alias ? "设备 " + label.substr(7) : label,
            static_cast<size_t>(available / (hg::font::kCellWidth * 2)));
    right(c, 377, y + 7, tokens, 2, kBright);
    const bool stale = row["stale"].as_bool();
    c.fill_circle(92, status_y + 6, 4, stale ? kWarning : kHealthy);
    text_at(c, 105, status_y, stale ? tr(zh, "Stale", "已过期") : tr(zh, "Reporting", "上报正常"), 2,
            stale ? kWarning : kHealthy);
    right(c, 377, status_y, !seen ? "--" : age(std::max<int64_t>(0, now - seen), zh) + tr(zh, " ago", "前"), 2, kMuted);
  }
  if (rows.size() > kDeviceRows) centered(c, 365, tr(zh, "Swipe up for more", "上滑查看更多"), 2, kMuted);
}

void settings(Canvas& c, bool zh, uint32_t seconds, bool network, std::string_view provider) {
  const auto row_y = [](int row) { return kSettingsTop + row * kSettingsStep; };
  const std::string interval = seconds < 60 ? std::to_string(seconds) + tr(zh, " sec", " 秒")
                                            : std::to_string(seconds / 60) + tr(zh, " min", " 分钟");
  for (int row = 0; row < 4; ++row) card(c, row_y(row), kSettingsHeight);
  text_at(c, 94, row_y(0) + 14, tr(zh, "Language", "语言"), 2, kBright);
  right(c, 371, row_y(0) + 14, tr(zh, "English", "简体中文"), 2, kAccent);
  text_at(c, 94, row_y(1) + 14, tr(zh, "Refresh interval", "拉取间隔"), 2, kBright);
  right(c, 371, row_y(1) + 14, interval, 2, kAccent);
  text_at(c, 94, row_y(2) + 14, tr(zh, "Refresh now", "立即刷新"), 2, kBright);
  right(c, 371, row_y(2) + 14, ">", 2, kAccent);
  text_at(c, 94, row_y(3) + 14, tr(zh, "Outer ring", "外圈"), 2, kBright);
  if (provider.empty()) right(c, 347, row_y(3) + 14, tr(zh, "Off", "关闭"), 2, kMuted);
  else name_at(c, 217, row_y(3) + 14, provider == "GitHub Copilot" ? "Copilot" : provider,
               11, source_style(provider).color);
  right(c, 371, row_y(3) + 14, ">", 2, kAccent);
  centered(c, 367, tr(zh, "< Back", "< 返回"), 2, kBright);
  centered(c, 406, network ? tr(zh, "Network online", "网络已连接")
                            : tr(zh, "Offline", "网络未连接"), 2, network ? kMuted : kWarning);
}

void ring_picker(Canvas& c, const std::vector<std::string_view>& options, size_t page,
                 std::string_view selected, bool zh) {
  const size_t start = page * kProviderRows;
  for (size_t i = start; i < std::min(start + kProviderRows, options.size()); ++i) {
    const int y = kPickerTop + static_cast<int>(i - start) * kPickerStep;
    const auto provider = options[i];
    card(c, y, kPickerHeight);
    if (!provider.empty()) c.fill_circle(98, y + 16, 4, source_style(provider).color);
    name_at(c, 113, y + 10, provider.empty() ? tr(zh, "Off", "关闭") : provider, 18);
    if (provider == selected) right(c, 372, y + 10, tr(zh, "Set", "已选"), 2, kHealthy);
  }
  if (options.size() == 1) centered(c, 225, tr(zh, "No quota data", "暂无额度"), 2, kMuted);
  const size_t pages = (options.size() + kProviderRows - 1) / kProviderRows;
  if (pages > 1) {
    const auto label = std::to_string(page + 1) + "/" + std::to_string(pages);
    centered(c, 367, label + tr(zh, "  Swipe up", "  上滑翻页"), 2, kMuted);
  }
  centered(c, 407, tr(zh, "< Back", "< 返回"), 2, kBright);
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
  settings_page_ = SettingsPage::Closed;
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
      !next["limits"].is_array() || next["limits"].size() > kLegacyQuotaRows ||
      (!next["limitsMore"].is_null() &&
       (!next["limitsMore"].is_array() || next["limitsMore"].size() > kLegacyQuotaRows)) ||
      !next["trend"].is_array() || next["trend"].size() > 30 ||
      !next["devices"].is_array() || next["devices"].size() > 12) {
    failure();
    return false;
  }
  data_ = std::move(next);
  received_at_ = now;
  failed_ = false;
  if (page_ == 1 || page_ == 3) {
    const size_t count = page_ == 1 ? quota_count() : data_["devices"].size();
    const size_t per_page = page_ == 1 ? kQuotaRows : kDeviceRows;
    detail_ = std::min(detail_, count ? (count - 1) / per_page : 0);
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

const char* TokenMonitor::page_name() const {
  if (settings_page_ == SettingsPage::Ring) return "Ring";
  return settings_page_ == SettingsPage::General ? "Settings" : kPages[page_];
}

size_t TokenMonitor::quota_count() const { return quota_rows(data_, cycle_quotas_).count; }

void TokenMonitor::open_settings(bool open) {
  settings_page_ = open ? SettingsPage::General : SettingsPage::Closed;
  provider_page_ = 0;
  dirty_ = true;
}

void TokenMonitor::set_language(Language language) {
  if (language == language_) return;
  language_ = language;
  dirty_ = true;
}

void TokenMonitor::set_refresh_seconds(uint32_t seconds) {
  if (seconds == refresh_seconds_) return;
  refresh_seconds_ = seconds;
  refresh();
  dirty_ = true;
}

void TokenMonitor::set_ring_provider(std::string_view provider) {
  ring_provider_ = provider;
  dirty_ = true;
}

void TokenMonitor::next(int step) {
  if (settings_page_ != SettingsPage::Closed) {
    open_settings(false);
    return;
  }
  page_ = (page_ + step + 4) % 4;
  detail_ = 0;
  dirty_ = true;
}

void TokenMonitor::detail() {
  if (settings_page_ != SettingsPage::Closed) return;
  size_t pages = page_ == 2 ? 4 : 2;
  if (page_ == 1 || page_ == 3) {
    const size_t count = page_ == 1 ? quota_count() : data_["devices"].size();
    const size_t per_page = page_ == 1 ? kQuotaRows : kDeviceRows;
    pages = std::max<size_t>(1, (count + per_page - 1) / per_page);
  }
  detail_ = (detail_ + 1) % pages;
  dirty_ = true;
}

TouchAction TokenMonitor::touch(bool down, int x, int y, uint32_t now, std::string_view& provider) {
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
  const bool swipe_back = dx >= 42 && dx > std::abs(dy);
  const bool in_columns = first_x_ >= 76 && first_x_ <= 390;
  if (settings_page_ == SettingsPage::Ring) {
    if (swipe_back || (short_tap && (header || first_y_ >= 395))) {
      open_settings(true);
    } else {
      const auto options = ring_providers(data_);
      const size_t pages = (options.size() + kProviderRows - 1) / kProviderRows;
      provider_page_ = std::min(provider_page_, pages - 1);
      const int offset = first_y_ - kPickerTop;
      if ((std::abs(dy) > 38 && std::abs(dy) > std::abs(dx)) ||
          (short_tap && first_y_ >= 350 && first_y_ < 395)) {
        provider_page_ = (provider_page_ + (dy > 38 ? pages - 1 : 1)) % pages;
        dirty_ = true;
      } else if (short_tap && in_columns && offset >= 0 && offset / kPickerStep < static_cast<int>(kProviderRows) &&
                 offset % kPickerStep < kPickerHeight) {
        const size_t row = provider_page_ * kProviderRows + static_cast<size_t>(offset / kPickerStep);
        if (row < options.size()) {
          provider = options[row];
          return TouchAction::RingProvider;
        }
      }
    }
  } else if (settings_page_ == SettingsPage::General) {
    if (swipe_back || (short_tap && (header || (first_y_ >= 351 && first_y_ < 399)))) {
      open_settings(false);
    } else if (short_tap && in_columns && first_y_ >= kSettingsTop) {
      const int offset = first_y_ - kSettingsTop;
      if (offset % kSettingsStep < kSettingsHeight) {
        switch (offset / kSettingsStep) {
          case 0: return TouchAction::Language;
          case 1: return TouchAction::Interval;
          case 2:
            refresh();
            open_settings(false);
            break;
          case 3:
            provider_page_ = 0;
            settings_page_ = SettingsPage::Ring;
            dirty_ = true;
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
    else if (page_ == 1 && short_tap && first_x_ >= 91 && first_x_ < 375 &&
             first_y_ >= kQuotaTabsY && first_y_ < kQuotaTabsY + kQuotaTabsHeight) {
      cycle_quotas_ = first_x_ >= 233;
      detail_ = 0;
      dirty_ = true;
    }
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
  const bool zh = language_ == Language::Chinese;
  const bool present = data_.is_object();
  const int64_t elapsed = static_cast<uint32_t>(now - received_at_) / 1000;
  const int64_t server_now = data_["asOf"].as_int() + elapsed;
  const bool stale = failed_ || (present && elapsed >= 3 * refresh_seconds_) || !network;
  const char* title = zh ? kPagesZh[page_] : page_ == 1 ? "Quota left" : kPages[page_];
  if (settings_page_ != SettingsPage::Closed) {
    if (settings_page_ == SettingsPage::Ring) {
      centered(c, 82, tr(zh, "Outer ring", "外圈"), 3, kBright);
      const auto providers = ring_providers(data_);
      provider_page_ = std::min(provider_page_, (providers.size() - 1) / kProviderRows);
      ring_picker(c, providers, provider_page_, ring_provider_, zh);
    } else {
      centered(c, 82, tr(zh, "Settings", "设置"), 3, kBright);
      settings(c, zh, refresh_seconds_, network, ring_provider_);
    }
    display.flush(0, info.height);
    return;
  }
  if (!ring_provider_.empty()) {
    const auto* quota = present && !stale ? lowest_quota(data_, ring_provider_, cycle_quotas_, server_now) : nullptr;
    quota_ring(c, ring_provider_, quota, cycle_quotas_, zh);
  }
  centered(c, 82, title, 3, kBright);
  settings_icon(c);

  if (present) {
    switch (page_) {
      case 0: overview(c, data_, detail_ != 0, zh); break;
      case 1: quotas(c, data_, detail_, server_now, cycle_quotas_, zh); break;
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
  for (int i = 0; i < 4; ++i) c.fill_circle(209 + 16 * i, 392, i == page_ ? 4 : 2, i == page_ ? kAccent : kMuted);
  centered(c, 406, !present ? (network ? tr(zh, "Tap to refresh", "点按刷新")
                                       : tr(zh, "Offline", "离线"))
                            : (stale ? tr(zh, "Cached ", "缓存 ") : tr(zh, "Updated ", "更新 ")) +
                                  age(elapsed, zh) + tr(zh, " ago", "前"),
           2, stale ? kWarning : kMuted);
  display.flush(0, info.height);
}

}  // namespace stopwatch
