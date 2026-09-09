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
        Persist current state to disk asynchronously.
        Keys are 'slot@monitor' for human readability and restart stability.
        """
        try:
            STATE_FILE.parent.mkdir(parents=True, exist_ok=True)
            out: dict[str, dict] = {}
            for (ws_id, mon), s in self._state.items():
                slot = ws_slot_name(ws_id)
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
        self.state     = LayoutState()
        self._last_ws:  int = -1
        self._last_mon: str = ""

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

    async def _on_focus_change(self, ws: int, mon: str) -> None:
        """Apply saved layout when WS×mon focus changes."""
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
            self._apply(mode, variant)
            self._notify_waybar()
            asyncio.get_event_loop().call_soon(self.state.save)
            return self.state.to_json(ws, mon)

        elif cmd == "cycle-variant":
            s, changed = self.state.cycle_variant(ws, mon)
            mode    = s["mode"]
            variant = self.state.current_variant(ws, mon)
            if changed:
                log.info("cycle-variant → ws=%d mon=%s → %s/%s", ws, mon, mode, variant)
                self._apply_variant(mode, variant)
                self._notify_waybar()
                asyncio.get_event_loop().call_soon(self.state.save)
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

    def _apply(self, mode: str, variant: str | None) -> None:
        """Full apply: set layout keyword + variant."""
        if mode == "monocle":
            hyprctl("keyword", "general:layout", "monocle")

        elif mode == "dwindle":
            hyprctl("keyword", "general:layout", "dwindle")
            # Don't togglesplit on WS switch — dwindle remembers its own
            # per-node split state. We only togglesplit on explicit cycle-variant.

        elif mode == "master":
            hyprctl("keyword", "general:layout", "master")
            hyprctl("keyword", "master:orientation", variant or "left")

    def _apply_variant(self, mode: str, variant: str | None) -> None:
        """Apply only the variant change for the current mode."""
        if mode == "dwindle":
            apply_dwindle_togglesplit()
        elif mode == "master":
            apply_master_orientation(variant or "left")
        # monocle: noop

    def _notify_waybar(self) -> None:
        """Signal waybar to refresh custom modules (RTMIN+8)."""
        subprocess.run(["pkill", "-RTMIN+8", "waybar"], capture_output=True)

    # ── Entry point ──────────────────────────────────────────────────────────

    async def seed(self) -> None:
        """Load persisted state and seed current WS×mon focus tracking."""
        self.state.load()
        ws, mon = get_active_ws_mon()
        self._last_ws  = ws
        self._last_mon = mon
        log.info("Initial focus ws=%d mon=%s", ws, mon)


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
