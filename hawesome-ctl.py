#!/usr/bin/env python3
"""
hawesome-ctl — query the hawesome plugin's control socket.

Layout changes go through the plugin dispatchers, not this socket:
  hyprctl dispatch hawesome:cycle-mode
  hyprctl dispatch hawesome:cycle-variant

Usage:
  hawesome-ctl status               # print current (focused) WS×mon layout as JSON
  hawesome-ctl status:<monitor>     # print layout for named monitor's active WS
  hawesome-ctl dump                 # print full state dict as JSON
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
    valid = {"status", "dump"}
    is_valid = cmd in valid or cmd.startswith("status:")
    if not is_valid:
        print(f"hawesome-ctl: unknown command '{cmd}'", file=sys.stderr)
        print(f"Valid commands: {', '.join(sorted(valid))} | status:<monitor>",
              file=sys.stderr)
        sys.exit(1)

    reply = send(cmd)

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
