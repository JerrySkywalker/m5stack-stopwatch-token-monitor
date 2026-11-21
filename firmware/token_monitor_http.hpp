#pragma once

namespace hg { class App; }

namespace hgp {

// Called from the application task. Network I/O runs on a lower-priority worker.
void token_monitor_http_tick(hg::App& app);

}  // namespace hgp
