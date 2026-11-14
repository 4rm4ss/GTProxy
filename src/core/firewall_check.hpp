#pragma once

namespace core::firewall {
// Windows: logs whether Windows Firewall lets the game server's UDP replies
// reach this Proxy.exe (current network profile, block-all mode, and every
// rule for this exe). Read-only, needs no administrator rights. Elsewhere a
// no-op.
void log_status();
}
