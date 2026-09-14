# hypr-awesome

Per-workspace × per-monitor tiling layout daemon for Hyprland.  
Inspired by AwesomeWM's per-tag layout model.

## Concept

Each **(workspace, monitor)** combination independently remembers its own:
- **layout mode** — `dwindle`, `monocle`, or `master`
- **variant** — sub-state within the mode (dwindle split direction, master orientation)

Switching workspace or monitor focus automatically restores the saved layout for that combo.

## Layout modes

| Mode | Description | Variants (`SUP+SHIFT+M`) |
|------|-------------|--------------------------|
| `dwindle` | Binary space partition | `h` (horizontal) ↔ `v` (vertical) |
| `monocle` | One window at a time; hover taskbar to cycle | — (no variants) |
| `master` | One master + slave stack | `left` → `top` → `right` → `bottom` |

## Keybinds

| Bind | Action |
|------|--------|
| `SUP+M` | Cycle layout mode for current WS×mon: dwindle → monocle → master → … |
| `SUP+SHIFT+M` | Cycle variant for current mode (noop on monocle) |

## Monocle implementation

Monocle is **not** a Hyprland layout engine. It is simulated by hawesome:

- **Enter**: all non-active tiled windows are moved to `special:monocle{ws_id}` (a hidden special workspace). The active window naturally expands to fill the space.
- **Cycle** (`SUP+M` again while in monocle): the next stashed window is brought forward, the current one is stashed.
- **Taskbar integration**: [wayapps](../wayapps/) includes stashed windows in the taskbar so you can hover or click to switch between them directly.
- **Exit** (cycle to dwindle/master): all stashed windows are restored before the engine switch.
- **Daemon restart**: any `special:monocleN` workspaces left from a previous session are automatically cleaned up on startup.

## Layout engine switching (dwindle ↔ master)

Hyprland assigns a tiling engine to each workspace permanently at creation. `general:layout` only affects newly created workspaces. To change an existing workspace's engine, hawesome:

1. Sets `general:layout` to the target engine
2. Moves the monitor to a temporary workspace `800+ws_id` (so the target workspace can be destroyed)
3. Empties the target workspace → Hyprland destroys it
4. Switches back → Hyprland recreates it under the new engine
5. Moves all windows back → they tile under the new engine

Focus events fired during this operation are suppressed via `_switching` flag + `asyncio.sleep(0.3)` drain.

## Hyprland layout constraint

`general:layout` is a single global keyword — no per-monitor or per-workspace layout API exists. hawesome works around this:

- On focus change, `general:layout` is set to the incoming WS×mon's mode. This only matters for newly-opened windows; existing window positions are preserved per-workspace by Hyprland natively.
- **Real limitation**: two monitors cannot simultaneously use different engines for new windows. If you open a new window while focused on a monitor with a different mode than its neighbor, the new window uses the focused monitor's engine. This is a Hyprland constraint, not a hawesome bug.

## Architecture

```
hawesome.py        daemon — asyncio IPC listener + control socket server
hawesome-ctl.py    CLI — sends commands to daemon (called by keybinds)
```

State is persisted to `~/.config/hypr-awesome/state.json` as `slot@monitor` keys.  
Control socket: `/tmp/hawesome-{uid}.sock`

## Install

```bash
cd ~/Lab/hypr-awesome
bash install.sh
```

Add to `hyprland.conf`:
```ini
exec-once = python3 ~/Lab/hypr-awesome/hawesome.py
```

## Control socket commands

```bash
hawesome-ctl.py cycle-mode        # advance layout mode for current WS×mon
hawesome-ctl.py cycle-variant     # advance variant for current mode
hawesome-ctl.py status            # JSON: current WS×mon state
hawesome-ctl.py status:eDP-1      # JSON: named monitor's active WS state
hawesome-ctl.py dump              # JSON: full in-memory state
```

## Notes

- Requires Hyprland with `split-monitor-workspaces` plugin
- State file uses `slot@monitor` keys (e.g. `"3@DP-5"`) for stability across restarts
- Temp workspace IDs ≥ 800 are used during engine switches and never saved to state
- `general:layout` is only issued when it differs from the last known value, to avoid disturbing other monitors on focus change
