# hypr-awesome

Per-workspace × per-monitor tiling layout plugin for Hyprland.  
Inspired by AwesomeWM's per-tag layout model — each workspace on each monitor independently remembers its own layout mode and variant.

```
WS 1  → dwindle/h   WS 2  → master/left   WS 3  → monocle
WS 1  → master/top  WS 4  → dwindle/v     ← different monitor, different state
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
| `dwindle` | `h` — split horizontally ↔ `v` — split vertically |
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
| [Hyprland](https://hyprland.org/) | Any recent release |
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

[split-monitor-workspaces](https://github.com/Duckonaut/split-monitor-workspaces) is a Hyprland community plugin that gives each monitor its own independent set of workspace slots (1–8, 1–8, 1–8…) instead of sharing one global pool.

**Single-monitor**: you don't need it. Hyprland's default workspace model already gives you slots 1–N on your one monitor.

**Multi-monitor**: it is a hard requirement. Without it, workspaces are shared globally across monitors and hypr-awesome's slot-based state persistence will restore the wrong layout on the wrong workspace.

Install via hyprpm:
```bash
hyprpm add https://github.com/Duckonaut/split-monitor-workspaces
hyprpm enable split-monitor-workspaces
```

Configure in `~/.config/hypr/workspaces.conf` (create this file if it doesn't exist):
```ini
plugin {
    split-monitor-workspaces {
        count = 8
        keep_focused = 0
        enable_notifications = 0
        enable_persistent_workspaces = 1
    }
}
```

Then source it from `hyprland.conf`:
```ini
source = ~/.config/hypr/workspaces.conf
```

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

Add the following to `~/.config/hypr/hyprland.conf`:

### 1. Load plugins (order matters)

`split-monitor-workspaces` must load **before** `hawesome` so workspace ID ranges are established before hawesome seeds its initial state.

```ini
# split-monitor-workspaces first — establishes per-monitor workspace ID ranges
exec-once = hyprctl plugin load /var/cache/hyprpm/boris/split-monitor-workspaces/split-monitor-workspaces.so

# hawesome second — seeds layout state after workspace ranges are set
exec-once = hyprctl plugin load /home/YOUR_USER/Lab/hypr-awesome/plugin/build/hawesome.so
```

> The exact path to `split-monitor-workspaces.so` depends on how you installed it.  
> Check `hyprpm list` or find it with: `find /var/cache/hyprpm -name "split-monitor-workspaces.so"`

Single-monitor users (no split-monitor-workspaces):
```ini
exec-once = hyprctl plugin load /home/YOUR_USER/Lab/hypr-awesome/plugin/build/hawesome.so
```

### 2. Keybinds

```ini
$mod = SUPER

# Cycle layout mode for the current workspace × monitor
bind = $mod,       m, exec, hyprctl dispatch hawesome:cycle-mode

# Cycle layout variant within the current mode (noop on monocle)
bind = $mod SHIFT, m, exec, hyprctl dispatch hawesome:cycle-variant
```

> **Why `exec, hyprctl dispatch` instead of just `bind = …, hawesome:cycle-mode`?**  
> Plugins load via `exec-once` *after* the config is parsed. Using `exec, hyprctl dispatch`
> resolves the dispatcher at keypress time (when the plugin is already loaded), not at
> config-parse time (when it isn't yet). Using the direct bind syntax causes
> "Invalid dispatcher" errors on startup.

### 3. Full example snippet

```ini
# ~/.config/hypr/hyprland.conf (relevant sections)

source = ~/.config/hypr/workspaces.conf   # split-monitor-workspaces config

exec-once = hyprctl plugin load /var/cache/hyprpm/boris/split-monitor-workspaces/split-monitor-workspaces.so
exec-once = hyprctl plugin load /home/YOUR_USER/Lab/hypr-awesome/plugin/build/hawesome.so

$mod = SUPER
bind = $mod,       m, exec, hyprctl dispatch hawesome:cycle-mode
bind = $mod SHIFT, m, exec, hyprctl dispatch hawesome:cycle-variant
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
python3 ~/.config/hypr/hawesome-ctl.py cycle-mode       # same as SUPER+M
python3 ~/.config/hypr/hawesome-ctl.py cycle-variant    # same as SUPER+SHIFT+M
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

- `wsPerMonitor()` reads `~/.config/hypr/workspaces.conf` at first call and caches the result for the plugin's lifetime. If you change the workspace count, reload the plugin (`make reload`) to pick up the new value.
- Workspace IDs ≥ 800 are reserved for internal use by Hyprland operations and are never saved to state.
- The plugin registers four dispatchers: `hawesome:cycle-mode`, `hawesome:cycle-variant`, `hawesome:status`, `hawesome:focus-window`.
