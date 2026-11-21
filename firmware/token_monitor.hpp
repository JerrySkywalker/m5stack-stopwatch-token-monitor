// StopWatch Token Monitor dashboard. Runs in the same task as the Hermes UI.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "hg/hal.hpp"
#include "hg/json.hpp"

namespace stopwatch {

constexpr size_t kMaxSnapshot = 8192;

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
  void refresh() { ++refresh_id_; }
  uint32_t refresh_id() const { return refresh_id_; }
  bool has_data() const { return data_.is_object(); }
  const char* page_name() const;
  void draw(hg::Display& display, uint32_t now, bool network, bool force);
  void touch(bool down, int x, int y, uint32_t now, bool& settings);
  void cancel_touch() { touching_ = false; }
  bool touching() const { return touching_; }

 private:
  hg::json::Value data_;
  uint32_t received_at_ = 0;
  uint32_t refresh_id_ = 0;
  uint32_t last_frame_ = 0;
  int page_ = 0;
  size_t detail_ = 0;
  bool enabled_ = false;
  bool combined_ = true;
  bool hidden_ = false;
  bool dirty_ = true;
  bool failed_ = false;
  bool network_ = false;
  bool touching_ = false;
  uint32_t touch_at_ = 0;
  int first_x_ = 0, first_y_ = 0, last_x_ = 0, last_y_ = 0;
};

}  // namespace stopwatch
