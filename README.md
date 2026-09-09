# hypr-awesome

Per-workspace × per-monitor tiling layout daemon for Hyprland.  
Inspired by AwesomeWM's per-tag layout model.

## Concept

Each **(workspace, monitor)** combination independently remembers its own:
- **layout mode** — `dwindle`, `monocle`, or `master`
- **variant** — sub-state within the mode (e.g. dwindle split direction, master orientation)

Switching workspace or monitor focus automatically restores the saved layout for that combo.  
No combo ever clobbers another.

## Layout modes

| Mode | Description | Variants (SUP+SHIFT+M) |
|------|-------------|------------------------|
| `dwindle` | Binary space partition | `h` (horizontal) ↔ `v` (vertical) |
| `monocle` | All windows full-size, stacked | — (no variants) |
| `master` | One master + slave stack | `left` → `top` → `right` → `bottom` |

## Keybinds

| Bind | Action |
|------|--------|
| `SUP+M` | Cycle layout mode for current WS×mon: dwindle → monocle → master → … |
| `SUP+SHIFT+M` | Cycle variant for current mode (noop on monocle) |

## Architecture

```
hawesome.py        daemon — IPC listener + control socket server
hawesome-ctl.py    CLI — sends commands to daemon (called by keybinds)
install.sh         setup helper
```

Communication between daemon and ctl via Unix socket at `/tmp/hawesome-<UID>.sock`.

## Install

```bash
cd ~/Lab/hypr-awesome
bash install.sh
```

Then add the printed snippets to `~/.config/hypr/hyprland.conf` and remove the old
`toggle-monocle.sh` / `toggle-split.sh` binds.

Restart Hyprland (or `exec-once` the daemon manually for a live session):
```bash
python3 ~/Lab/hypr-awesome/hawesome.py &
```

## Control socket commands

```bash
hawesome-ctl.py cycle-mode      # advance mode for current WS×mon
hawesome-ctl.py cycle-variant   # advance variant for current mode
hawesome-ctl.py status          # JSON: current WS×mon state
hawesome-ctl.py status:eDP-1    # JSON: state for named monitor's active WS
hawesome-ctl.py dump            # JSON: full state dict
```

## Planned (phase 2)

- **Persistence** — save/restore layout state dict to `~/.config/hypr-awesome/state.json` on every change, keyed by `(ws_slot, monitor_name)` for stability across restarts

## Planned (phase 3)

- **Session save/restore** — on Hyprland exit, record all open windows (class, WS, monitor)
  via `hyprctl clients -j`. On next launch, re-launch each app and move its window back to
  the saved WS×screen. If a screen is gone, fall back to current active screen.
  
  Implementation notes:
  - Save trigger: `hyprland:shutdown` IPC event or systemd `ExecStopPost`
  - Restore: map `window class → launch command` via `.desktop` file `Exec=` field (~80%
    coverage), with a hardcoded exceptions table for the rest
  - Window placement is async: launch app, wait for `openwindow` IPC event matching the
    class, then dispatch `movetoworkspacesilent`
  - Separate script: `hawesome-session.py` (not part of the main daemon)
  
  Alternatives to evaluate first:
  - [`wayland-session-manager`](https://github.com/tw4452852/wayland-session-manager)
  - KDE's ksmserver approach (systemd session units per app)
  - `systemd --user` service units with `PartOf=graphical-session.target`

## Notes

- Requires Hyprland with `split-monitor-workspaces` plugin (already in your setup)
- `monocle` uses Hyprland's built-in `monocle` layout keyword
- Dwindle split direction is toggled via `layoutmsg togglesplit` (Hyprland doesn't expose a set-direction API)
- Master orientation is set via `keyword master:orientation`
- Waybar is signalled (RTMIN+8) after every mode change for custom module refresh
