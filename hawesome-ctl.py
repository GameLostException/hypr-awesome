#!/usr/bin/env python3
"""
hawesome-ctl — query and control the hawesome plugin via its control socket.

Layout changes (Hyprland v0.57+):
  Hyprland's lua-based dispatch system cannot parse plugin dispatcher names
  that contain colons (e.g. 'hawesome:cycle-mode') via hyprctl dispatch.
  Instead, hawesome exposes these as commands on its own control socket.

Usage:
  hawesome-ctl status               # print current (focused) WS×mon layout as JSON
  hawesome-ctl status:<monitor>     # print layout for named monitor's active WS
  hawesome-ctl dump                 # print full state dict as JSON
  hawesome-ctl cycle-mode           # cycle layout mode on focused monitor's workspace
  hawesome-ctl cycle-variant        # cycle layout variant on focused monitor's workspace
"""

import json
import os
import socket
import sys

UID      = os.getuid()
CTL_SOCK = f"/tmp/hawesome-{UID}.sock"


def send(cmd: str) -> str:
    try:
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as s:
            s.settimeout(2.0)
            s.connect(CTL_SOCK)
            s.sendall((cmd + "\n").encode())
            chunks = []
            while True:
                chunk = s.recv(4096)
                if not chunk:
                    break
                chunks.append(chunk)
            return b"".join(chunks).decode().strip()
    except FileNotFoundError:
        print("hawesome: plugin not loaded (socket not found)", file=sys.stderr)
        sys.exit(1)
    except ConnectionRefusedError:
        print("hawesome: plugin not responding", file=sys.stderr)
        sys.exit(1)
    except TimeoutError:
        print("hawesome: plugin timed out", file=sys.stderr)
        sys.exit(1)


def main() -> None:
    if len(sys.argv) < 2:
        print(__doc__, file=sys.stderr)
        sys.exit(1)

    cmd = sys.argv[1]

    # Validate command
    dispatch_cmds = {"cycle-mode", "cycle-variant"}
    query_cmds    = {"status", "dump"}
    is_valid = cmd in dispatch_cmds or cmd in query_cmds or cmd.startswith("status:")

    if not is_valid:
        print(f"hawesome-ctl: unknown command '{cmd}'", file=sys.stderr)
        print(f"Valid commands: {', '.join(sorted(dispatch_cmds | query_cmds))} | status:<monitor>",
              file=sys.stderr)
        sys.exit(1)

    reply = send(cmd)

    if cmd in dispatch_cmds:
        # Dispatch commands return "ok" or a JSON error
        if reply == "ok":
            return  # success, no output needed
        # Try to parse as JSON error
        try:
            parsed = json.loads(reply)
            if isinstance(parsed, dict) and "error" in parsed:
                print(f"hawesome-ctl: {parsed['error']}", file=sys.stderr)
                sys.exit(1)
        except json.JSONDecodeError:
            pass
        print(reply)
        return

    # Query commands: pretty-print JSON
    try:
        parsed = json.loads(reply)
    except json.JSONDecodeError:
        print(reply)
        return

    print(json.dumps(parsed, indent=2))
    if isinstance(parsed, dict) and "error" in parsed:
        sys.exit(1)


if __name__ == "__main__":
    main()
