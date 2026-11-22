// StopWatch Token Monitor dashboard. Runs in the same task as the Hermes UI.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "hg/hal.hpp"
#include "hg/json.hpp"

namespace stopwatch {

constexpr size_t kMaxSnapshot = 8192;
constexpr uint32_t kDefaultRefreshSeconds = 60;

enum class Language { English, Chinese };
enum class TouchAction { None, HermesSettings, Language, Interval, RingProvider };

bool valid_refresh_seconds(uint32_t seconds);
uint32_t next_refresh_seconds(uint32_t seconds);

class TokenMonitor {
 public:
  void enable(bool enabled, bool combined);
  bool enabled() const { return enabled_; }
  bool combined() const { return combined_; }
  bool hidden() const { return hidden_; }
  void hide(bool hidden);
  bool ingest(std::string_view body, uint32_t now);
  void failure();
  void reset();
  void next(int step);
  void detail();
  bool cycle_quotas() const { return cycle_quotas_; }
  size_t quota_count() const;
  void open_settings(bool open);
  void set_language(Language language);
  Language language() const { return language_; }
  void set_refresh_seconds(uint32_t seconds);
  uint32_t refresh_seconds() const { return refresh_seconds_; }
  void set_ring_provider(std::string_view provider);
  const std::string& ring_provider() const { return ring_provider_; }
  void refresh() { ++refresh_id_; }
  uint32_t refresh_id() const { return refresh_id_; }
  bool has_data() const { return data_.is_object(); }
  const char* page_name() const;
  void draw(hg::Display& display, uint32_t now, bool network, bool force);
  TouchAction touch(bool down, int x, int y, uint32_t now, std::string_view& provider);
  void cancel_touch() { touching_ = false; }
  bool touching() const { return touching_; }

 private:
  enum class SettingsPage { Closed, General, Ring };

  hg::json::Value data_;
  std::string ring_provider_;
  uint32_t received_at_ = 0;
  uint32_t refresh_id_ = 0;
  uint32_t last_frame_ = 0;
  uint32_t refresh_seconds_ = kDefaultRefreshSeconds;
  Language language_ = Language::English;
  int page_ = 0;
  size_t detail_ = 0;
  bool enabled_ = false;
  bool combined_ = true;
  bool hidden_ = false;
  SettingsPage settings_page_ = SettingsPage::Closed;
  size_t provider_page_ = 0;
  bool cycle_quotas_ = false;
  bool dirty_ = true;
  bool failed_ = false;
  bool network_ = false;
  bool touching_ = false;
  uint32_t touch_at_ = 0;
  int first_x_ = 0, first_y_ = 0, last_x_ = 0, last_y_ = 0;
};

}  // namespace stopwatch
