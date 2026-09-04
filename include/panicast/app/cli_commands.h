// panicast CLI service subcommands (N09/S1): status / start / stop / restart / enable /
//   disable / log [-f]. Dispatched from main() before the TUI path. `panicastd` is the
//   headless daemon (systemd unit panicastd.service); start/stop/restart go through
//   systemctl directly (the S1-3 polkit rule allows the owning user passwordless), while
//   enable/disable deliberately require the user's sudo (explicit opt-in to autostart).
#pragma once

namespace panicast
{

// Returns 0 when the command ran (and a process exit code is appropriate), -1 when
//   argv does not name a service subcommand (caller continues normal startup).
int run_cli_command(int argc, char *argv[]);

} // namespace panicast
