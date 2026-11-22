#include "hg/app.hpp"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cstdlib>
#include <utility>

namespace hg {
namespace {

constexpr std::string_view kPath = "/watch/v1/snapshot";
constexpr size_t kMaxEndpoint = 190;
constexpr size_t kMaxToken = 192;

uint32_t refresh_seconds(std::string_view value) {
  uint32_t seconds = 0;
  const auto parsed = std::from_chars(value.data(), value.data() + value.size(), seconds);
  if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() ||
      !stopwatch::valid_refresh_seconds(seconds)) return 0;
  return seconds;
}

bool clean_token(std::string_view token) {
  return token.size() <= kMaxToken &&
         std::all_of(token.begin(), token.end(), [](unsigned char ch) { return ch >= 33 && ch <= 126; });
}

bool private_ipv4(std::string_view host) {
  int octet[4]{};
  for (int i = 0; i < 4; ++i) {
    const auto pos = host.find('.');
    const auto text = host.substr(0, pos);
    if (text.empty() || text.size() > 3 ||
        !std::all_of(text.begin(), text.end(), [](unsigned char ch) { return std::isdigit(ch); }))
      return false;
    octet[i] = std::atoi(std::string(text).c_str());
    if (octet[i] > 255 || (i < 3) != (pos != std::string_view::npos)) return false;
    if (i < 3) host.remove_prefix(pos + 1);
  }
  return octet[0] == 10 || octet[0] == 127 || (octet[0] == 192 && octet[1] == 168) ||
         (octet[0] == 172 && octet[1] >= 16 && octet[1] <= 31);
}

bool endpoint_valid(std::string_view endpoint, bool lab) {
  if (endpoint.size() > kMaxEndpoint || endpoint.size() <= kPath.size() ||
      endpoint.substr(endpoint.size() - kPath.size()) != kPath ||
      endpoint.find_first_of("?#@\\") != std::string_view::npos ||
      !clean_token(endpoint)) return false;
  if (endpoint.substr(0, 8) == "https://")
    return !lab && endpoint.size() > 8 + kPath.size() && endpoint[8] != '/';
  if (!lab || endpoint.substr(0, 7) != "http://") return false;
  const auto authority = endpoint.substr(7, endpoint.find('/', 7) - 7);
  return private_ipv4(authority.substr(0, authority.find(':')));
}

}  // namespace

void App::token_monitor_enable(bool enabled, bool combined) {
  const bool supports_dashboard = hal_.display && hal_.display->info().width == 466 &&
      hal_.display->info().height == 466 && hal_.display->framebuffer_rows() == 0;
  token_monitor_.enable(enabled && supports_dashboard, combined);
}

void App::token_monitor_load() {
  if (!token_monitor_.enabled()) return;
  auto endpoint = setting("tm_endpoint");
  auto token = setting("tm_token");
  token_monitor_.set_language(setting("tm_lang") == "zh" ? stopwatch::Language::Chinese
                                                        : stopwatch::Language::English);
  if (const auto seconds = refresh_seconds(setting("tm_interval")))
    token_monitor_.set_refresh_seconds(seconds);
  const auto provider = setting("tm_ring_src");
  if (provider.size() <= 26) token_monitor_.set_ring_provider(provider);
  const bool lab = endpoint.substr(0, 7) == "http://";
  if (endpoint_valid(endpoint, lab)) tm_endpoint_ = std::move(endpoint);
  if (!lab && clean_token(token)) tm_token_ = std::move(token);
}

bool App::token_monitor_set_language(stopwatch::Language language) {
  if (language == token_monitor_.language()) return true;
  if (!hal_.storage || !hal_.storage->set("tm_lang", language == stopwatch::Language::Chinese ? "zh" : "en"))
    return false;
  token_monitor_.set_language(language);
  return true;
}

bool App::token_monitor_set_interval(uint32_t seconds) {
  if (seconds == token_monitor_.refresh_seconds()) return true;
  if (!hal_.storage || !hal_.storage->set("tm_interval", std::to_string(seconds))) return false;
  token_monitor_.set_refresh_seconds(seconds);
  return true;
}

bool App::token_monitor_screen(Screen screen) const {
  if (!token_monitor_.enabled() || token_monitor_.hidden()) return false;
  return screen == Screen::Ready || screen == Screen::Offline || screen == Screen::Clock ||
         screen == Screen::Connecting || (screen == Screen::Error && server_url_.empty());
}

bool App::token_monitor_touch(bool down, int x, int y, uint32_t when, bool allow_start) {
  if (tm_touch_wake_) {
    if (!down) tm_touch_wake_ = false;
    return true;
  }
  if (!token_monitor_.touching()) {
    if (!down || !allow_start || !token_monitor_rendered_) return false;
    if (touch_ignored()) return false;
    if (wake_display()) {
      tm_touch_wake_ = true;
      return true;
    }
  }
  if (!token_monitor_rendered_ && token_monitor_.touching()) {
    if (!down) token_monitor_.cancel_touch();
    return true;
  }
  activity_at_ = now();
  clock_showing_ = false;
  std::string_view provider;
  switch (token_monitor_.touch(down, x, y, when, provider)) {
    case stopwatch::TouchAction::HermesSettings:
      open_settings();
      break;
    case stopwatch::TouchAction::Language:
      token_monitor_set_language(token_monitor_.language() == stopwatch::Language::English
                                     ? stopwatch::Language::Chinese : stopwatch::Language::English);
      break;
    case stopwatch::TouchAction::Interval:
      token_monitor_set_interval(stopwatch::next_refresh_seconds(token_monitor_.refresh_seconds()));
      break;
    case stopwatch::TouchAction::RingProvider:
      if (provider == token_monitor_.ring_provider() ||
          (hal_.storage && hal_.storage->set("tm_ring_src", provider))) {
        token_monitor_.set_ring_provider(provider);
        token_monitor_.open_settings(true);
      }
      break;
    case stopwatch::TouchAction::None:
      break;
  }
  if (!down) update_model();
  return true;
}

bool App::token_monitor_apply(std::string_view body, int status) {
  tm_http_status_ = status;
  const bool ok = status == 200 && token_monitor_.ingest(body, now());
  if (!ok) token_monitor_.failure();
  update_model();
  return ok;
}

std::string App::token_monitor_command(std::string_view args) {
#if !defined(ESP_PLATFORM)
  if (args == "mode combined" || args == "mode watch" || args == "mode off") {
    token_monitor_enable(args != "mode off", args != "mode watch");
    token_monitor_load();
    update_model();
    return "@ok simulator mode";
  }
#endif
  if (!token_monitor_.enabled()) return "@error Token Monitor is unavailable";
  if (args.empty() || args == "status") {
    auto result = json::Value::object();
    result.set("page", token_monitor_.page_name())
        .set("visible", token_monitor_rendered_)
        .set("data", token_monitor_.has_data())
        .set("relay_configured", !tm_endpoint_.empty())
        .set("authenticated", !tm_token_.empty())
        .set("language", token_monitor_.language() == stopwatch::Language::Chinese ? "zh" : "en")
        .set("quota_view", token_monitor_.cycle_quotas() ? "cycle" : "short")
        .set("quota_count", static_cast<int>(token_monitor_.quota_count()))
        .set("ring_provider", token_monitor_.ring_provider())
        .set("interval_seconds", token_monitor_.refresh_seconds())
        .set("status", tm_http_status_)
        .set("refresh_requested", token_monitor_.refresh_id());
    return "@tm " + result.dump();
  }
  if (args == "refresh") token_monitor_.refresh();
  else if (args == "settings" || args == "back") token_monitor_.open_settings(args == "settings");
  else if (args == "language en" || args == "language zh") {
    if (!token_monitor_set_language(args == "language zh" ? stopwatch::Language::Chinese
                                                          : stopwatch::Language::English))
      return "@error unable to store language";
  } else if (args.substr(0, 9) == "interval ") {
    const auto seconds = refresh_seconds(args.substr(9));
    if (!seconds) return "@error refresh interval must be 30, 60, 120, 300 or 900 seconds";
    if (!token_monitor_set_interval(seconds)) return "@error unable to store refresh interval";
  }
  else if (args == "next") token_monitor_.next(1);
  else if (args == "previous") token_monitor_.next(-1);
  else if (args == "detail") token_monitor_.detail();
  else if (args == "show" || args == "hide") token_monitor_.hide(args == "hide");
#if !defined(ESP_PLATFORM)
  else if (args.substr(0, 7) == "inject ")
    return token_monitor_apply(args.substr(7), 200) ? "@ok snapshot" : "@error invalid snapshot";
  else if (args == "inject-error") {
    token_monitor_apply({}, 503);
    return "@ok simulated relay failure";
  }
#endif
  else if (args.substr(0, 9) == "endpoint " || args.substr(0, 4) == "lab " || args == "clear-endpoint") {
    const bool lab = args.substr(0, 4) == "lab ";
    const auto endpoint = args == "clear-endpoint" ? std::string_view() : args.substr(lab ? 4 : 9);
    if (!tm_token_.empty() && endpoint != tm_endpoint_)
      return "@error clear the device token before changing endpoints";
    if (!endpoint.empty() && !endpoint_valid(endpoint, lab))
      return "@error HTTPS endpoint or private lab HTTP without a token is required";
    if (!hal_.storage || !hal_.storage->set("tm_endpoint", endpoint))
      return "@error unable to store endpoint";
    tm_endpoint_ = endpoint;
    token_monitor_.reset();
  } else if (args.substr(0, 6) == "token " || args == "clear-token") {
    const auto token = args == "clear-token" ? std::string_view() : args.substr(6);
    if (!clean_token(token) || (!token.empty() && tm_endpoint_.substr(0, 8) != "https://"))
      return "@error a valid token and HTTPS endpoint are required";
    if (!hal_.storage || !hal_.storage->set("tm_token", token))
      return "@error unable to store token";
    tm_token_ = token;
    token_monitor_.reset();
  } else {
    return "@error tm status | endpoint <HTTPS URL> | lab <private HTTP URL> | token <value> | "
           "clear-token | clear-endpoint | settings | back | language en/zh | "
           "interval <seconds> | refresh | next | previous | detail | show | hide";
  }
  update_model();
  return "@ok Token Monitor";
}

}  // namespace hg
