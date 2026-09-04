// panicast CLI service subcommands (N09/S1, unit renamed in N10): status / start /
//   stop / restart / enable / disable / log [-f]. Dispatched from main() before the TUI
//   path. The daemon is `panicast -d` (systemd unit panicast.service, ExecStart=panicast
//   -d); start/stop/restart go through systemctl directly (the S1-3 polkit rule allows
//   the owning user passwordless), while enable/disable deliberately require the user's
//   sudo (explicit opt-in to autostart).
#pragma once

namespace panicast
{

// S1-4 session handover: if the panicast systemd service is running, stop it (the TUI
//   takes over the single-instance playback session; the daemon's clean exit persists
//   player state, which the TUI then restores). Returns true when a service was stopped
//   (i.e. the caller should restore it on the way out). Passwordless via the polkit rule.
bool service_handover_takeover();

// N10.1 (user-final semantics): `panicast` implies the background service — before the
//   TUI comes up, make sure the daemon is running: start the unit when it is not and
//   wait (bounded) for the pidfile to name a live process. The handover then takes it
//   over for the TUI session and restores it on exit, so the service is running before
//   AND after any TUI session. Returns false only when the unit can't be started (not
//   installed / no polkit) — the TUI still runs standalone in that case.
bool service_ensure_running();
// Restart the daemon after the TUI exits (only meaningful when takeover stopped it).
void service_handover_restore();

// Returns 0 when the command ran (and a process exit code is appropriate), -1 when
//   argv does not name a service subcommand (caller continues normal startup).
int run_cli_command(int argc, char *argv[]);

} // namespace panicast
