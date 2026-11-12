# hypr-awesome

Per-workspace × per-monitor tiling layout plugin for Hyprland.  
Inspired by AwesomeWM's per-tag layout model — each workspace on each monitor independently remembers its own layout mode and variant.

```
WS 1  → dwindle     WS 2  → master/left   WS 3  → monocle
WS 1  → master/top  WS 4  → dwindle       ← different monitor, different state
```

Switching workspace or monitor focus instantly restores the saved layout — no manual re-applying.

---

## What it does

### Layout modes

| Mode | Description |
|------|-------------|
| `dwindle` | Binary space partition — new windows split the focused tile |
| `monocle` | One window fills the workspace; others stack behind it |
| `master` | One master window on one side; remaining windows in a slave stack |

### Variants (sub-state within a mode)

| Mode | Variants (cycle with `SUPER+SHIFT+M`) |
|------|---------------------------------------|
| `dwindle` | none saved — `SUPER+SHIFT+M` toggles the focused split (`togglesplit`) |
| `monocle` | none |
| `master` | `left` → `top` → `right` → `bottom` (master window position) |

### Keybinds

| Bind | Action |
|------|--------|
| `SUPER+M` | Cycle mode: dwindle → monocle → master → dwindle… |
| `SUPER+SHIFT+M` | Cycle variant within current mode (noop on monocle) |

State is saved to `~/.config/hypr-awesome/state.json` and restored on restart.

---

## Requirements

### System

| Requirement | Notes |
|-------------|-------|
| [Hyprland](https://hyprland.org/) | 0.57 or later (the plugin is built against its internal headers) |
| C++23 compiler (`g++` or `clang++`) | For building the plugin |
| `pkg-config` | For build flags resolution |
| Hyprland development headers | See below |

### Hyprland headers

The plugin compiles against Hyprland's own headers. You need the development package for your distro:

```bash
# Arch Linux (AUR)
yay -S hyprland-devel
# or:
paru -S hyprland-devel
```

Verify the headers are found:
```bash
pkg-config --cflags hyprland
# should print something like: -I/usr/include/hyprland ...
```

If `pkg-config` can't find the `hyprland` package, the headers aren't installed — the build will fail with "package hyprland was not found".

### split-monitor-workspaces (required for multi-monitor; optional for single-monitor)

[split-monitor-workspaces](https://github.com/Duckonaut/split-monitor-workspaces) v1.3.0+ is a **pure Lua package** — no `.so` needed. It gives each monitor its own independent set of workspace slots (1–8, 9–16, 17–24…).

**Single-monitor**: you don't need it.

**Multi-monitor**: it is a hard requirement. Without it, workspaces are shared globally and hypr-awesome's slot-based state will restore the wrong layout on the wrong workspace.

```bash
git clone https://github.com/Duckonaut/split-monitor-workspaces ~/Lab/split-monitor-workspaces
```

Configure in `hyprland.lua` (see §Hyprland configuration below).

---

## Install

```bash
git clone https://github.com/GameLostException/hypr-awesome.git ~/Lab/hypr-awesome
cd ~/Lab/hypr-awesome
bash install.sh
```

`install.sh` builds the plugin `.so` and symlinks `hawesome-ctl.py` into `~/.config/hypr/`.

Or build manually:
```bash
cd ~/Lab/hypr-awesome/plugin/src
make -j$(nproc)
```

The compiled plugin ends up at `plugin/build/hawesome.so`.

---

## Hyprland configuration

> **Hyprland v0.57+** uses a Lua config (`hyprland.lua`). All examples use the Lua API.

### 1. Load plugins

`hawesome.so` is loaded via `hl.plugin.load()`. `split-monitor-workspaces` is pure Lua — no `.so` needed.

```lua
-- split-monitor-workspaces: pure Lua package
package.path = package.path .. ";/home/YOUR_USER/Lab/split-monitor-workspaces/lua/?.lua"
for _, m in ipairs({"split-monitor-workspaces","monitors","globals","helpers","dispatchers"}) do
    package.loaded[m] = nil  -- clear cache so config.reloaded gets a fresh module
end
local smw = require("split-monitor-workspaces")
local WS_COUNT = 8
smw.setup({
    workspace_count              = WS_COUNT,
    monitor_priority             = { "DP-6", "DP-5", "eDP-1" },
    keep_focused                 = true,
    enable_persistent_workspaces = true,
})

-- hawesome: requires a compiled .so
hl.plugin.load("/home/YOUR_USER/Lab/hypr-awesome/plugin/build/hawesome.so")
```

### 2. Keybinds

In Hyprland v0.57+, `hyprctl dispatch hawesome:cycle-mode` fails because the lua dispatch
router cannot parse dispatcher names containing colons. Use `hawesome-ctl.py` instead,
which sends commands directly to the plugin's Unix socket:

```lua
local mod = "SUPER"

-- Cycle layout mode via hawesome control socket (v0.57 compat)
hl.bind(mod .. " + m",         hl.dsp.exec_cmd("python3 ~/.config/hypr/hawesome-ctl.py cycle-mode"))
hl.bind(mod .. " + SHIFT + m", hl.dsp.exec_cmd("python3 ~/.config/hypr/hawesome-ctl.py cycle-variant"))
```

### 3. Full example snippet

```lua
-- ~/.config/hypr/hyprland.lua (relevant sections)

package.path = package.path .. ";/home/YOUR_USER/Lab/split-monitor-workspaces/lua/?.lua"
for _, m in ipairs({"split-monitor-workspaces","monitors","globals","helpers","dispatchers"}) do package.loaded[m] = nil end
local smw = require("split-monitor-workspaces")
local WS_COUNT = 8
smw.setup({ workspace_count = WS_COUNT, monitor_priority = {"DP-6","DP-5","eDP-1"},
            keep_focused = true, enable_persistent_workspaces = true })

hl.plugin.load("/home/YOUR_USER/Lab/hypr-awesome/plugin/build/hawesome.so")

local mod = "SUPER"
hl.bind(mod .. " + m",         hl.dsp.exec_cmd("python3 ~/.config/hypr/hawesome-ctl.py cycle-mode"))
hl.bind(mod .. " + SHIFT + m", hl.dsp.exec_cmd("python3 ~/.config/hypr/hawesome-ctl.py cycle-variant"))
```

---

## After a Hyprland upgrade

The plugin compiles against Hyprland's internal headers. After any Hyprland update, rebuild before the next session or the plugin will refuse to load with a "Version mismatch" notification:

```bash
cd ~/Lab/hypr-awesome/plugin/src && make clean && make -j$(nproc)
```

Or from the repo root:
```bash
make reload   # build + unload old + load new into the running session
```

---

## Monocle mode

Monocle uses Hyprland's native `CMonocleAlgorithm`. All windows remain on the workspace — Hyprland stacks them with the focused window filling the space. There are no hidden special workspaces.

- **Entering**: press `SUPER+M` until you reach monocle. The focused window fills the workspace; others are behind it.
- **Cycling through windows**: click or hover a button in the [wayapps](../wayapps/) taskbar. The clicked window comes to the front.
- **Exiting**: press `SUPER+M` to advance to master or back to dwindle. The algorithm switches back live.

Monocle works without wayapps — you can still use `ALT+TAB` or `SUPER+TAB` to cycle windows. wayapps just makes it more visual.

---

## Control socket (for scripting / status bars)

`hawesome-ctl.py` queries the plugin's Unix socket at `/tmp/hawesome-{uid}.sock`:

```bash
python3 ~/.config/hypr/hawesome-ctl.py status           # JSON: current WS×mon state
python3 ~/.config/hypr/hawesome-ctl.py status:eDP-1     # JSON: named monitor's active WS
python3 ~/.config/hypr/hawesome-ctl.py dump             # JSON: full persisted state

# Dispatch commands (v0.57 workaround — hyprctl dispatch can't parse colon names)
python3 ~/.config/hypr/hawesome-ctl.py cycle-mode       # cycle layout mode
python3 ~/.config/hypr/hawesome-ctl.py cycle-variant    # cycle layout variant
```

Example status output:
```json
{
  "ws": 3,
  "mon": "DP-5",
  "mode": "master",
  "variant": "left"
}
```

---

## Live reload (during development)

```bash
make reload   # from repo root: build → unload old .so → load new .so
```

---

## Architecture

```
plugin/src/main.cpp         Plugin entry point — dispatchers, event hooks, control socket
plugin/src/LayoutState.cpp  Per-WS×monitor state — slot-keyed persistence, monitor lifecycle
plugin/src/LayoutState.hpp  State types (SHAState, CLayoutState)
hawesome-ctl.py             CLI client for the plugin's control socket
install.sh                  Build + symlink installer
Makefile                    build / reload / clean targets
```

State is saved as `slot@monitor` keys (e.g. `"3@DP-5"`). "Slot" is the 1-based position within a monitor's workspace range, independent of monitor index — so state survives monitor reordering (e.g. unplugging DP-5 and re-plugging it as a different index).

---

## Notes

- `wsPerMonitor()` reads `workspace_count` from `~/.config/hypr/hyprland.lua` at first call and caches the result. If you change the workspace count, reload Hyprland (`hyprctl reload`) to pick up the new value.
- Workspace IDs ≥ 800 are reserved for internal use by Hyprland and are never saved to state.
- The plugin registers three dispatchers: `hawesome:cycle-mode`, `hawesome:cycle-variant`, `hawesome:status`. In Hyprland v0.57+, call these via `hawesome-ctl.py` rather than `hyprctl dispatch` (which cannot parse colon-containing names through its Lua router).
