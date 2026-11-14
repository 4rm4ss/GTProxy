#pragma once
#include <string>

namespace core::browser {
// Opens a page of the proxy in its own window. On Windows that is a Microsoft
// Edge app window (no tabs, no address bar), or the default browser if Edge
// is missing; elsewhere the default browser. Returns false if nothing opened.
bool open_window(const std::string& url);
}
