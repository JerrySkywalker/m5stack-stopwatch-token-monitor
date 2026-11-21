#include "port.hpp"
#include "token_monitor_http.hpp"

#include <memory>
#include <new>
#include <string>

#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "hg/app.hpp"

#if CONFIG_HG_BOARD_STOPWATCH
namespace {

struct Request {
  std::string endpoint;
  std::string token;
  uint32_t refresh;
};

struct Response {
  std::string body;
  uint32_t refresh;
  int status = 0;
  bool overflow = false;
};

QueueHandle_t request_queue;
QueueHandle_t response_queue;

esp_err_t on_http_event(esp_http_client_event_t* event) {
  auto& response = *static_cast<Response*>(event->user_data);
  if (event->event_id == HTTP_EVENT_ON_DATA) {
    const size_t size = static_cast<size_t>(event->data_len);
    if (size > stopwatch::kMaxSnapshot - response.body.size()) {
      response.overflow = true;
      return ESP_FAIL;
    }
    response.body.append(static_cast<const char*>(event->data), size);
  }
  return ESP_OK;
}

void fetch(const Request& request, Response& response) {
  esp_http_client_config_t config{};
  config.url = request.endpoint.c_str();
  config.timeout_ms = 4000;
  config.disable_auto_redirect = true;
  config.event_handler = on_http_event;
  config.user_data = &response;
  if (request.endpoint.compare(0, 8, "https://") == 0) config.crt_bundle_attach = esp_crt_bundle_attach;
  auto client = esp_http_client_init(&config);
  if (!client) return;
  bool ready = esp_http_client_set_header(client, "Accept", "application/json") == ESP_OK;
  if (ready && !request.token.empty()) {
    const auto auth = "Bearer " + request.token;
    ready = esp_http_client_set_header(client, "Authorization", auth.c_str()) == ESP_OK;
  }
  if (ready && esp_http_client_perform(client) == ESP_OK && !response.overflow)
    response.status = esp_http_client_get_status_code(client);
  esp_http_client_cleanup(client);
}

void run(void*) {
  for (;;) {
    Request* incoming = nullptr;
    if (xQueueReceive(request_queue, &incoming, portMAX_DELAY) != pdTRUE) continue;
    std::unique_ptr<Request> request(incoming);
    auto* response = new (std::nothrow) Response;
    if (!response) {
      // A null response tells the main task to release the in-flight slot.
      xQueueSend(response_queue, &response, portMAX_DELAY);
      continue;
    }
    response->refresh = request->refresh;
    fetch(*request, *response);
    xQueueSend(response_queue, &response, portMAX_DELAY);
  }
}

bool start_worker() {
  request_queue = xQueueCreate(1, sizeof(Request*));
  response_queue = xQueueCreate(1, sizeof(Response*));
  if (request_queue && response_queue &&
      xTaskCreate(run, "watch-http", 6144, nullptr, 2, nullptr) == pdPASS) return true;
  if (request_queue) vQueueDelete(request_queue);
  if (response_queue) vQueueDelete(response_queue);
  request_queue = response_queue = nullptr;
  return false;
}

}  // namespace
#endif

namespace hgp {

void token_monitor_http_tick(hg::App& app) {
#if CONFIG_HG_BOARD_STOPWATCH
  static bool waiting = false;
  static uint32_t next_at = 0;
  static uint32_t sent = 0;
  const uint32_t now = static_cast<uint32_t>(xTaskGetTickCount());
  const uint32_t refresh = app.token_monitor_refresh_id();

  if (waiting) {
    Response* raw_response = nullptr;
    if (xQueueReceive(response_queue, &raw_response, 0) == pdTRUE) {
      std::unique_ptr<Response> response(raw_response);
      waiting = false;
      if (sent == refresh) {
        const bool ok = response ? app.token_monitor_apply(response->body, response->status)
                                 : app.token_monitor_apply({}, 0);
        next_at = now + pdMS_TO_TICKS(ok ? 60000 : 15000);
      }
    }
  }

  if (waiting || !app.token_monitor_network_ready() || app.in_turn() ||
      (refresh == sent && static_cast<int32_t>(now - next_at) < 0)) return;
  if (!request_queue && !start_worker()) {
    app.token_monitor_apply({}, 0);
    sent = refresh;
    next_at = now + pdMS_TO_TICKS(15000);
    return;
  }
  auto* request = new (std::nothrow) Request{app.token_monitor_endpoint(), app.token_monitor_token(), refresh};
  if (!request || xQueueSend(request_queue, &request, 0) != pdTRUE) {
    delete request;
    sent = refresh;
    next_at = now + pdMS_TO_TICKS(15000);
    app.token_monitor_apply({}, 0);
    return;
  }
  waiting = true;
  sent = refresh;
#else
  (void)app;
#endif
}

}  // namespace hgp
