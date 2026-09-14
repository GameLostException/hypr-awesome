#!/usr/bin/env python3
"""
hawesome — per-workspace × per-monitor tiling layout daemon for Hyprland.

Inspired by AwesomeWM's per-tag layout model.
Each (workspace_id, monitor_name) combination independently tracks:
  - mode:     dwindle | monocle | master
  - variants: per-mode sub-state (dwindle split direction, master orientation)

IPC:
  Listens on Hyprland's .socket2.sock for workspace/monitor focus events.
  Exposes a control socket at /tmp/hawesome-<UID>.sock for hawesome-ctl.

Commands accepted on control socket (newline-terminated):
  cycle-mode            → advance mode for current WS×mon
  cycle-variant         → advance variant for current mode on current WS×mon
  status                → return JSON of current (focused) WS×mon state
  status:<monitor>      → return JSON of active WS state on named monitor
  dump                  → return JSON of full state dict
"""

import asyncio
import json
import logging
import os
import signal
import subprocess
from pathlib import Path

# ─── Logging ─────────────────────────────────────────────────────────────────

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s [hawesome] %(levelname)s %(message)s",
    datefmt="%H:%M:%S",
)
log = logging.getLogger("hawesome")

# ─── Constants ───────────────────────────────────────────────────────────────

# Ordered cycle for SUP+M
MODE_CYCLE = ["dwindle", "monocle", "master"]

# Ordered variant cycles per mode for SUP+SHIFT+M
VARIANT_CYCLE = {
    "dwindle": ["h", "v"],
    "monocle": [],          # no variants
    "master":  ["left", "top", "right", "bottom"],
}

# Default state for an unseen WS×mon combo
DEFAULT_MODE     = "dwindle"
DEFAULT_VARIANTS = {
    "dwindle": "h",
    "master":  "left",
}

# Control socket path (unique per user)
UID      = os.getuid()
CTL_SOCK = f"/tmp/hawesome-{UID}.sock"

# Persistence file
STATE_FILE = Path.home() / ".config" / "hypr-awesome" / "state.json"

# ─── Hyprland IPC helpers ─────────────────────────────────────────────────────

def _hypr_sig() -> str:
    """Return the active Hyprland instance signature."""
    sig = os.environ.get("HYPRLAND_INSTANCE_SIGNATURE", "")
    if not sig:
        base = Path(f"/run/user/{UID}/hypr")
        dirs = sorted(base.iterdir(), key=lambda p: p.stat().st_mtime, reverse=True)
        sig = dirs[0].name if dirs else ""
    return sig


def _hypr_dir() -> Path:
    return Path(f"/run/user/{UID}/hypr/{_hypr_sig()}")


def hyprctl(*args: str) -> str:
    """Run hyprctl and return stdout."""
    result = subprocess.run(["hyprctl", *args], capture_output=True, text=True)
    return result.stdout.strip()


def hyprctl_json(*args: str):
    """Run hyprctl and parse JSON output."""
    try:
        return json.loads(hyprctl(*args))
    except (json.JSONDecodeError, ValueError):
        return None


def get_active_ws_mon() -> tuple[int, str]:
    """Return (workspace_id, monitor_name) of the currently focused workspace."""
    ws  = hyprctl_json("activeworkspace", "-j") or {}
    mon = hyprctl_json("monitors", "-j") or []
    ws_id   = ws.get("id", 1)
    focused = next((m["name"] for m in mon if m.get("focused")), "")
    return ws_id, focused


def get_ws_mon_for_monitor(monitor_name: str) -> tuple[int, str]:
    """Return (active_workspace_id, monitor_name) for a specific monitor."""
    monitors = hyprctl_json("monitors", "-j") or []
    for m in monitors:
        if m["name"] == monitor_name:
            return m["activeWorkspace"]["id"], monitor_name
    # Monitor not found — fall back to focused
    return get_active_ws_mon()


def ws_slot_name(ws_id: int) -> str:
    """
    Resolve a Hyprland workspace id to its human-readable slot name ("1"–"8").
    Falls back to str(ws_id) if the workspace isn't found.
    """
    workspaces = hyprctl_json("workspaces", "-j") or []
    ws = next((w for w in workspaces if w["id"] == ws_id), None)
    return ws["name"] if ws else str(ws_id)

# ─── Layout application ───────────────────────────────────────────────────────

def apply_dwindle_togglesplit() -> None:
    """Issue a dwindle togglesplit dispatch."""
    hyprctl("dispatch", "layoutmsg", "togglesplit")


def apply_master_orientation(orientation: str) -> None:
    """Set master orientation."""
    hyprctl("keyword", "master:orientation", orientation)

# ─── State management ─────────────────────────────────────────────────────────

class LayoutState:
    def __init__(self):
        # Runtime dict: key (ws_id: int, monitor: str)
        self._state: dict[tuple[int, str], dict] = {}
        # Persisted dict: key "slot@monitor" — loaded at startup, merged lazily
        self._saved: dict[str, dict] = {}

    # ── Persistence ──────────────────────────────────────────────────────────

    def load(self) -> None:
        """Load state from disk into _saved. Called once at startup."""
        try:
            text = STATE_FILE.read_text()
            self._saved = json.loads(text)
            log.info("Loaded %d saved layout(s) from %s", len(self._saved), STATE_FILE)
        except FileNotFoundError:
            log.info("No saved state file — starting fresh")
        except (json.JSONDecodeError, OSError) as e:
            log.warning("Could not load state file: %s", e)

    def save(self) -> None:
        """
        Persist current state to disk.
        Keys are 'slot@monitor' for human readability and restart stability.
        """
        try:
            STATE_FILE.parent.mkdir(parents=True, exist_ok=True)

            # Resolve all ws_ids to slot names in one hyprctl call
            workspaces = hyprctl_json("workspaces", "-j") or []
            id_to_slot = {w["id"]: w["name"] for w in workspaces}

            out: dict[str, dict] = {}
            for (ws_id, mon), s in self._state.items():
                # Skip temp workspaces used by _switch_ws_engine (IDs ≥ 800)
                if ws_id >= 800:
                    continue
                slot = id_to_slot.get(ws_id, str(ws_id))
                out[f"{slot}@{mon}"] = {
                    "mode":     s["mode"],
                    "variants": dict(s["variants"]),
                }
            # Merge with _saved so entries for currently-unseen combos are kept
            merged = {**self._saved, **out}
            STATE_FILE.write_text(json.dumps(merged, indent=2))
        except OSError as e:
            log.warning("Could not save state: %s", e)

    # ── State access ─────────────────────────────────────────────────────────

    def _default(self) -> dict:
        return {
            "mode":     DEFAULT_MODE,
            "variants": dict(DEFAULT_VARIANTS),
        }

    def _saved_for(self, ws_id: int, mon: str) -> dict | None:
        """Look up saved state for a WS×mon by resolving its slot name."""
        slot = ws_slot_name(ws_id)
        key  = f"{slot}@{mon}"
        raw  = self._saved.get(key)
        if raw is None:
            return None
        # Validate and fill missing variant keys
        entry = self._default()
        entry["mode"] = raw.get("mode", DEFAULT_MODE)
        entry["variants"].update(raw.get("variants", {}))
        return entry

    def get(self, ws: int, mon: str) -> dict:
        """Return state for (ws, mon), initialising from saved data if first seen."""
        if ws >= 800:
            # Temp workspace used by _switch_ws_engine — always return default, never persist
            return self._default()
        key = (ws, mon)
        if key not in self._state:
            saved = self._saved_for(ws, mon)
            self._state[key] = saved if saved is not None else self._default()
        return self._state[key]

    # ── Mutations ────────────────────────────────────────────────────────────

    def cycle_mode(self, ws: int, mon: str) -> dict:
        s   = self.get(ws, mon)
        idx = MODE_CYCLE.index(s["mode"])
        s["mode"] = MODE_CYCLE[(idx + 1) % len(MODE_CYCLE)]
        return s

    def cycle_variant(self, ws: int, mon: str) -> tuple[dict, bool]:
        """Returns (state, changed) — changed=False if mode has no variants."""
        s     = self.get(ws, mon)
        mode  = s["mode"]
        cycle = VARIANT_CYCLE.get(mode, [])
        if not cycle:
            return s, False
        cur = s["variants"].get(mode, cycle[0])
        idx = cycle.index(cur) if cur in cycle else 0
        s["variants"][mode] = cycle[(idx + 1) % len(cycle)]
        return s, True

    def current_variant(self, ws: int, mon: str) -> str | None:
        s = self.get(ws, mon)
        return s["variants"].get(s["mode"])

    # ── Serialisation ────────────────────────────────────────────────────────

    def to_json(self, ws: int, mon: str) -> str:
        s = self.get(ws, mon)
        return json.dumps({
            "ws":      ws,
            "mon":     mon,
            "mode":    s["mode"],
            "variant": self.current_variant(ws, mon),
            "variants": s["variants"],
        })

    def dump_json(self) -> str:
        out = {}
        for (ws, mon), s in self._state.items():
            out[f"{ws}@{mon}"] = s
        return json.dumps(out, indent=2)


# ─── Daemon ───────────────────────────────────────────────────────────────────

class HawesomeDaemon:
    def __init__(self):
        self.state            = LayoutState()
        self._last_ws:  int   = -1
        self._last_mon: str   = ""
        self._current_layout: str  = ""        # last layout keyword sent to Hyprland
        self._monocle_ws: set[int] = set()     # workspace IDs with monocle active
        self._switching: bool = False           # True while _switch_ws_engine runs

    # ── Hyprland event listener ──────────────────────────────────────────────

    async def listen_hyprland(self) -> None:
        """Connect to .socket2.sock and handle workspace/monitor focus events."""
        sock_path = _hypr_dir() / ".socket2.sock"
        log.info("Connecting to Hyprland event socket: %s", sock_path)

        while True:
            try:
                reader, _ = await asyncio.open_unix_connection(str(sock_path))
                log.info("Connected to Hyprland IPC")
                async for line in reader:
                    await self._handle_event(line.decode().strip())
            except (ConnectionRefusedError, FileNotFoundError, OSError) as e:
                log.warning("Hyprland socket lost (%s), retrying in 2s…", e)
                await asyncio.sleep(2)

    async def _handle_event(self, line: str) -> None:
        """Parse a Hyprland event line and react to workspace/monitor changes."""
        if ">>" not in line:
            return
        event, _, data = line.partition(">>")

        if event == "workspace":
            try:
                ws_id = int(data)
            except ValueError:
                return
            _, mon = get_active_ws_mon()
            await self._on_focus_change(ws_id, mon)

        elif event == "focusedmon":
            parts = data.split(",")
            if len(parts) < 2:
                return
            mon = parts[0]
            try:
                ws_id = int(parts[1])
            except ValueError:
                return
            await self._on_focus_change(ws_id, mon)

        elif event == "closewindow":
            # If a window closes while monocle is active on its workspace,
            # pull the next stashed window into view.
            if self._monocle_ws and not self._switching:
                self._monocle_restore_after_close()

    async def _on_focus_change(self, ws: int, mon: str) -> None:
        """Apply saved layout when WS×mon focus changes."""
        # Suppress during _switch_ws_engine — it fires temp workspace events
        # that would clobber the layout we're in the middle of setting.
        if self._switching:
            return
        if ws == self._last_ws and mon == self._last_mon:
            return
        self._last_ws  = ws
        self._last_mon = mon

        s       = self.state.get(ws, mon)
        mode    = s["mode"]
        variant = self.state.current_variant(ws, mon)
        log.info("Focus → ws=%d mon=%s → apply %s/%s", ws, mon, mode, variant or "—")
        self._apply(mode, variant)

    # ── Control socket ───────────────────────────────────────────────────────

    async def serve_ctl(self) -> None:
        """Serve the control socket for hawesome-ctl commands."""
        try:
            os.unlink(CTL_SOCK)
        except FileNotFoundError:
            pass

        server = await asyncio.start_unix_server(self._handle_ctl, CTL_SOCK)
        log.info("Control socket: %s", CTL_SOCK)
        async with server:
            await server.serve_forever()

    async def _handle_ctl(
        self, reader: asyncio.StreamReader, writer: asyncio.StreamWriter
    ) -> None:
        try:
            raw   = await reader.readline()
            cmd   = raw.decode().strip()
            reply = await self._dispatch_cmd(cmd)
            writer.write((reply + "\n").encode())
            await writer.drain()
        finally:
            writer.close()
            await writer.wait_closed()

    async def _dispatch_cmd(self, cmd: str) -> str:
        ws, mon = get_active_ws_mon()

        if cmd == "cycle-mode":
            s       = self.state.cycle_mode(ws, mon)
            mode    = s["mode"]
            variant = self.state.current_variant(ws, mon)
            log.info("cycle-mode → ws=%d mon=%s → %s/%s", ws, mon, mode, variant or "—")
            self._apply(mode, variant, force_retile=True)
            # Clear _switching after yielding to the event loop so queued
            # workspace events from _switch_ws_engine get ignored, not applied.
            await asyncio.sleep(0.3)
            self._switching = False
            self._notify_wayapps()
            asyncio.get_running_loop().call_soon(self.state.save)
            return self.state.to_json(ws, mon)

        elif cmd == "cycle-variant":
            s, changed = self.state.cycle_variant(ws, mon)
            mode    = s["mode"]
            variant = self.state.current_variant(ws, mon)
            if changed:
                log.info("cycle-variant → ws=%d mon=%s → %s/%s", ws, mon, mode, variant)
                self._apply_variant(mode, variant)
                self._notify_wayapps()
                asyncio.get_running_loop().call_soon(self.state.save)
            else:
                log.info("cycle-variant → no variants for mode=%s", mode)
            return self.state.to_json(ws, mon)

        elif cmd == "status":
            # Status for currently focused WS×mon
            return self.state.to_json(ws, mon)

        elif cmd.startswith("status:"):
            # Status for a specific monitor's active WS: "status:DP-5"
            target_mon = cmd[7:]
            ws_for_mon, _ = get_ws_mon_for_monitor(target_mon)
            return self.state.to_json(ws_for_mon, target_mon)

        elif cmd == "dump":
            return self.state.dump_json()

        else:
            return json.dumps({"error": f"unknown command: {cmd}"})

    # ── Layout application helpers ───────────────────────────────────────────

    def _apply(self, mode: str, variant: str | None, force_retile: bool = False) -> None:
        """
        Apply layout for the currently focused WS×mon.

        Hyprland assigns a layout engine (dwindle or master) to each workspace
        permanently at creation time. general:layout only affects future
        workspaces. To change an existing workspace's engine we must:
          1. Set general:layout to the new engine
          2. Move all tiled windows out (workspace is destroyed when empty)
          3. Move them back (workspace is recreated fresh with the new engine)

        Monocle is simulated via a special stash workspace (special:monocleN)
        that hides all non-active tiled windows.

        force_retile=True: apply the engine switch / monocle simulation.
                           Used on explicit cycle-mode (SUP+M).
        force_retile=False: only update general:layout for new workspaces.
                            Used on focus change (no visual disruption).
        """
        if mode == "monocle":
            if force_retile:
                self._apply_monocle()
            # general:layout stays as whatever it was (dwindle/master)
            return

        # Exiting monocle — undo fullscreen if active on this workspace
        if force_retile:
            self._exit_monocle_if_active()

        layout = "dwindle" if mode == "dwindle" else "master"
        old_layout = self._current_layout

        # Always keep general:layout in sync for new workspaces
        if old_layout != layout:
            hyprctl("keyword", "general:layout", layout)
            self._current_layout = layout

        if mode == "master":
            hyprctl("keyword", "master:orientation", variant or "left")

        # On explicit cycle: always run the engine switch.
        # Do NOT gate on _current_layout — monocle mode leaves _current_layout
        # unchanged, so cycling monocle→master with _current_layout=="master"
        # would otherwise skip the switch even though the workspace still has
        # the old dwindle engine from before the monocle.
        if force_retile:
            self._switch_ws_engine(layout)

    def _apply_monocle(self) -> None:
        """
        Enter monocle mode: hide all non-active tiled windows by moving them
        to a special stash workspace (special:monocleN). The active window
        naturally expands to fill the workspace. Cycling focuses the next
        stashed window by bringing it back and re-stashing the current one.
        """
        ws = hyprctl_json("activeworkspace", "-j") or {}
        ws_id = ws.get("id")
        if not ws_id:
            return

        aw = hyprctl_json("activewindow", "-j") or {}
        active_addr = aw.get("address", "")

        clients = hyprctl_json("clients", "-j") or []
        tiled = [
            c for c in clients
            if c.get("workspace", {}).get("id") == ws_id
            and not c.get("hidden")
            and not c.get("floating")
        ]

        if ws_id in self._monocle_ws:
            # Already in monocle — cycle: bring next stashed window back,
            # stash the current active one.
            stash = f"special:monocle{ws_id}"
            stashed = [
                c for c in clients
                if c.get("workspace", {}).get("name") == stash
            ]
            if stashed:
                next_win = stashed[0]
                batch = (
                    f"dispatch movetoworkspacesilent {ws_id},address:{next_win['address']}"
                )
                if active_addr:
                    batch += f" ; dispatch movetoworkspacesilent {stash},address:{active_addr}"
                hyprctl("--batch", batch)
                log.info("monocle cycle: ws=%d showed %s", ws_id, next_win.get("class"))
            return

        # If active_addr is empty (e.g. after a _switch_ws_engine temp workspace
        # hop), focus the first tiled window before stashing others.
        if not active_addr and tiled:
            active_addr = tiled[0]["address"]
            hyprctl("dispatch", "focuswindow", f"address:{active_addr}")

        # Entering monocle: stash all non-active tiled windows
        others = [c["address"] for c in tiled if c["address"] != active_addr]
        if not others and len(tiled) <= 1:
            # Only one window — nothing to hide, just mark monocle active
            self._monocle_ws.add(ws_id)
            log.info("monocle enter: ws=%d single window", ws_id)
            return

        stash = f"special:monocle{ws_id}"
        batch = " ; ".join(
            f"dispatch movetoworkspacesilent {stash},address:{a}" for a in others
        )
        hyprctl("--batch", batch)
        self._monocle_ws.add(ws_id)
        log.info("monocle enter: ws=%d stashed %d windows", ws_id, len(others))

    def _exit_monocle_if_active(self) -> None:
        """
        Exit monocle: restore all stashed windows back to their workspace.
        Undo any fullscreen state on the active window.
        """
        ws = hyprctl_json("activeworkspace", "-j") or {}
        ws_id = ws.get("id")
        if not ws_id or ws_id not in self._monocle_ws:
            return

        stash = f"special:monocle{ws_id}"
        clients = hyprctl_json("clients", "-j") or []
        stashed = [
            c for c in clients
            if c.get("workspace", {}).get("name") == stash
        ]

        if stashed:
            batch = " ; ".join(
                f"dispatch movetoworkspacesilent {ws_id},address:{c['address']}"
                for c in stashed
            )
            hyprctl("--batch", batch)
            log.info("monocle exit: ws=%d restored %d windows", ws_id, len(stashed))

        # Undo any fullscreen on the current window
        aw = hyprctl_json("activewindow", "-j") or {}
        if aw.get("fullscreen", 0):
            hyprctl("dispatch", "fullscreen", "1")

        self._monocle_ws.discard(ws_id)

    def _switch_ws_engine(self, layout: str) -> None:
        """
        Switch the active workspace's tiling engine by destroying and recreating it.

        Hyprland assigns a layout engine to a workspace permanently at creation.
        The only way to change it is to destroy the workspace (by emptying it)
        and recreate it under the new general:layout.

        Steps:
        1. Switch the monitor to an adjacent workspace (so the target ws can be destroyed)
        2. Move all tiled windows to a temp workspace (target ws is now empty → destroyed)
        3. Switch back to the target workspace (recreated fresh under new engine)
        4. Move all windows back (they tile under the new engine)
        """
        ws = hyprctl_json("activeworkspace", "-j") or {}
        ws_id  = ws.get("id")
        current_engine = ws.get("tiledLayout", "")

        if not ws_id or current_engine == layout:
            log.info("switch_ws_engine: ws=%d already %s, skip", ws_id, layout)
            return

        clients = hyprctl_json("clients", "-j") or []
        tiled = [
            c["address"] for c in clients
            if c.get("workspace", {}).get("id") == ws_id
            and not c.get("hidden")
            and not c.get("floating")
        ]
        if not tiled:
            log.info("switch_ws_engine: ws=%d no tiled windows, skip", ws_id)
            return

        log.info("switch_ws_engine: ws=%d %s→%s (%d windows)",
                 ws_id, current_engine, layout, len(tiled))

        mon_name = ws.get("monitor", "")
        tmp_ws   = 800 + ws_id  # scratch workspace, outside any monitor's 1–24 range

        batch_out = " ; ".join(
            f"dispatch movetoworkspacesilent {tmp_ws},address:{a}" for a in tiled
        )
        batch_in = " ; ".join(
            f"dispatch movetoworkspacesilent {ws_id},address:{a}" for a in tiled
        )

        # Suppress focus-change event handling while we move workspaces around.
        # The temp workspace switches fire focusedmon/workspace events that would
        # call _on_focus_change and clobber general:layout mid-operation.
        # NOTE: hyprctl calls below are blocking subprocess.run() inside asyncio.
        # The event loop is suspended during these calls, so queued socket events
        # accumulate in the buffer and are processed AFTER this function returns.
        # We keep _switching=True until after the loop has had a chance to drain
        # those queued events by yielding control with asyncio.sleep(0).
        self._switching = True
        try:
            # 1. Move THIS monitor to tmp_ws so ws_id becomes non-active → destroyable.
            hyprctl("--batch",
                    f"dispatch focusmonitor {mon_name} ; dispatch workspace {tmp_ws}")

            # 2. Move all windows out → ws_id empty → Hyprland destroys it
            hyprctl("--batch", batch_out)

            # 3. Switch this monitor back to ws_id → Hyprland creates it fresh under
            #    the current general:layout
            hyprctl("--batch",
                    f"dispatch focusmonitor {mon_name} ; dispatch workspace {ws_id}")

            # 4. Move windows back → tiled under new engine
            hyprctl("--batch", batch_in)
            # tmp_ws is empty and no longer active → Hyprland auto-destroys it
        finally:
            # Re-sync _last_ws/_last_mon so the next real focus change is detected
            self._last_ws  = ws_id
            self._last_mon = mon_name
            # _switching stays True — cleared by _clear_switching() scheduled below

    def _force_retile_active_ws(self) -> None:
        """Unused legacy wrapper — kept so external tooling isn't broken."""
        pass

    def _monocle_restore_after_close(self) -> None:
        """
        After a window closes, if the active workspace is in monocle and has
        stashed windows, bring the next one into view automatically.
        """
        ws = hyprctl_json("activeworkspace", "-j") or {}
        ws_id = ws.get("id")
        if not ws_id or ws_id not in self._monocle_ws:
            return
        # Check if workspace is now empty (the closed window was the last visible one)
        clients = hyprctl_json("clients", "-j") or []
        visible = [c for c in clients
                   if c.get("workspace", {}).get("id") == ws_id
                   and not c.get("hidden") and not c.get("floating")]
        stash = f"special:monocle{ws_id}"
        stashed = [c for c in clients
                   if c.get("workspace", {}).get("name") == stash]
        if not visible and stashed:
            # Bring next stashed window back
            hyprctl("dispatch",
                    f"movetoworkspacesilent {ws_id},address:{stashed[0]['address']}")
            log.info("monocle close recovery: ws=%d restored %s",
                     ws_id, stashed[0].get("class"))
        elif not visible and not stashed:
            # No more windows — exit monocle state cleanly
            self._monocle_ws.discard(ws_id)

    def _apply_variant(self, mode: str, variant: str | None) -> None:
        """Apply only the variant change for the current mode."""
        if mode == "dwindle":
            apply_dwindle_togglesplit()
        elif mode == "master":
            apply_master_orientation(variant or "left")
        # monocle: noop
        # _current_layout unchanged — variant changes don't switch the layout

    def _notify_wayapps(self) -> None:
        """Signal all wayapps instances to refresh their layout icon (SIGUSR1)."""
        subprocess.run(["pkill", "-SIGUSR1", "-f", "wayapps.py"], capture_output=True)
        # Also signal waybar for any custom layout modules
        subprocess.run(["pkill", "-RTMIN+8", "waybar"], capture_output=True)

    # ── Entry point ──────────────────────────────────────────────────────────

    async def seed(self) -> None:
        """Load persisted state and seed current WS×mon focus tracking."""
        self.state.load()
        ws, mon = get_active_ws_mon()
        self._last_ws  = ws
        self._last_mon = mon
        # Seed current layout from compositor so first focus change doesn't
        # needlessly re-issue the keyword if it already matches.
        try:
            cfg = hyprctl_json("getoption", "general:layout")
            self._current_layout = (cfg or {}).get("str", "")
        except Exception:
            self._current_layout = ""
        log.info("Initial focus ws=%d mon=%s layout=%r", ws, mon, self._current_layout)

        # Restore any monocle stash workspaces left over from a previous session.
        # special:monocleN workspaces persist across daemon restarts — move all
        # stashed windows back to their original workspace so the slate is clean.
        clients = hyprctl_json("clients", "-j") or []
        for c in clients:
            ws_name = c.get("workspace", {}).get("name", "")
            if ws_name.startswith("special:monocle"):
                try:
                    orig_ws = int(ws_name[len("special:monocle"):])
                    hyprctl("dispatch",
                            f"movetoworkspacesilent {orig_ws},address:{c['address']}")
                    log.info("seed: restored stashed window %s to ws=%d",
                             c.get("class"), orig_ws)
                except (ValueError, KeyError):
                    pass


# ─── Main ─────────────────────────────────────────────────────────────────────

def main() -> None:
    daemon = HawesomeDaemon()

    async def _run() -> None:
        loop       = asyncio.get_running_loop()
        stop_event = asyncio.Event()

        def _shutdown(sig_num: int) -> None:
            log.info("Received signal %d, shutting down", sig_num)
            stop_event.set()

        loop.add_signal_handler(signal.SIGTERM, _shutdown, signal.SIGTERM)
        loop.add_signal_handler(signal.SIGINT,  _shutdown, signal.SIGINT)

        await daemon.seed()

        tasks = [
            asyncio.create_task(daemon.listen_hyprland()),
            asyncio.create_task(daemon.serve_ctl()),
        ]

        await stop_event.wait()

        for t in tasks:
            t.cancel()
        await asyncio.gather(*tasks, return_exceptions=True)

    try:
        asyncio.run(_run())
    finally:
        try:
            os.unlink(CTL_SOCK)
        except FileNotFoundError:
            pass
        log.info("hawesome stopped")


if __name__ == "__main__":
    main()
