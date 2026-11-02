# hypr-awesome

Per-workspace × per-monitor tiling layout plugin for Hyprland.  
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
| `monocle` | One window at a time; stacked with the focused one on top | — (no variants) |
| `master` | One master + slave stack | `left` → `top` → `right` → `bottom` |

## Keybinds

| Bind | Action |
|------|--------|
| `SUP+M` | Cycle layout mode for current WS×mon: dwindle → monocle → master → … |
| `SUP+SHIFT+M` | Cycle variant for current mode (noop on monocle) |

## Monocle implementation

Monocle uses Hyprland's native `CMonocleAlgorithm` — all windows remain on the
workspace and Hyprland stacks them with the focused one on top.  There are no
stash workspaces.

- **Enter**: the workspace switches to `CMonocleAlgorithm`; the focused window fills the space.
- **Cycle** (`SUP+M` again while in monocle): clicking or hovering a taskbar button focuses that window; `CMonocleAlgorithm` brings it to front via `focusTargetUpdate`.
- **Taskbar integration**: [wayapps](../wayapps/) shows all windows on the workspace (including those behind the front one) so you can click to cycle between them.
- **Exit** (cycle to dwindle/master): the algorithm is swapped back live via `updateTiledAlgo`.

## Architecture

```
plugin/src/main.cpp         Hyprland plugin — registers dispatchers, hooks workspace/window/monitor events
plugin/src/LayoutState.cpp  State management — slot-keyed persistence, monitor lifecycle
plugin/src/LayoutState.hpp  State types and class declaration
plugin/src/Dispatchers.cpp  (reserved — dispatcher implementations live in main.cpp)
plugin/src/Dispatchers.hpp  (reserved)
hawesome-ctl.py             CLI — queries the plugin's Unix socket (status, dump)
```

State is persisted to `~/.config/hypr-awesome/state.json` as `slot@monitor` keys
(e.g. `"1@DP-5"` — stable across monitor index changes).  
Control socket: `/tmp/hawesome-{uid}.sock` (provided by the plugin).

## Install

```bash
cd ~/Lab/hypr-awesome
bash install.sh
```

Or build manually:
```bash
cd ~/Lab/hypr-awesome/plugin/src
make -j$(nproc)
```

Add to `hyprland.conf` (hyprpm is not used — load directly):
```ini
exec-once = hyprctl plugin load ~/Lab/hypr-awesome/plugin/build/hawesome.so
```

**After a Hyprland upgrade**, rebuild before the next boot or the plugin will fail
to load with "Version mismatch":
```bash
cd ~/Lab/hypr-awesome/plugin/src && make clean && make -j$(nproc)
```

## Live reload

```bash
make reload   # build + unload old + load new, from repo root
```

## Control socket commands

```bash
hawesome-ctl.py cycle-mode        # advance layout mode for current WS×mon
hawesome-ctl.py cycle-variant     # advance variant for current mode
hawesome-ctl.py status            # JSON: current WS×mon state
hawesome-ctl.py status:eDP-1      # JSON: named monitor's active WS state
hawesome-ctl.py dump              # JSON: full state.json (from disk)
```

## Notes

- Requires Hyprland with `split-monitor-workspaces` plugin
- State file uses `slot@monitor` keys (e.g. `"3@DP-5"`) for stability across restarts
- `wsPerMonitor()` reads `~/.config/hypr/workspaces.conf` at first call and caches the result; reload the plugin after changing the workspace count
- Temp workspace IDs ≥ 800 are never saved to state (not applicable to the plugin — kept for compat with any tooling that checks)
