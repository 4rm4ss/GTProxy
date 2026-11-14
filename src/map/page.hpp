#pragma once
#include <string>

namespace map {
// The map window: one HTML page (canvas + script) built into the proxy, so
// it works without any files next to Proxy.exe.
[[nodiscard]] const std::string& page_html();
}
