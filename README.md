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
hawesome-ctl.py dump            # JSON: full state dict
```

## Planned (phase 2)

- **Persistence** — save/restore state dict to `~/.config/hypr-awesome/state.json`
- **Window arrangement snapshots** — save window positions per WS×mon, restore on switch

## Notes

- Requires Hyprland with `split-monitor-workspaces` plugin (already in your setup)
- `monocle` uses Hyprland's built-in `monocle` layout keyword
- Dwindle split direction is toggled via `layoutmsg togglesplit` (Hyprland doesn't expose a set-direction API)
- Master orientation is set via `keyword master:orientation`
- Waybar is signalled (RTMIN+8) after every mode change for custom module refresh
