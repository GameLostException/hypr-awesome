#!/usr/bin/env python3
"""
hawesome-ctl — send commands to the hawesome daemon.

Usage:
  hawesome-ctl cycle-mode      # SUP+M  — advance layout mode for current WS×mon
  hawesome-ctl cycle-variant   # SUP+SHIFT+M — advance variant for current mode
  hawesome-ctl status          # print current WS×mon layout as JSON
  hawesome-ctl dump            # print full state dict as JSON
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
            # Read until connection closes
            chunks = []
            while True:
                chunk = s.recv(4096)
                if not chunk:
                    break
                chunks.append(chunk)
            return b"".join(chunks).decode().strip()
    except FileNotFoundError:
        print("hawesome: daemon not running (socket not found)", file=sys.stderr)
        sys.exit(1)
    except ConnectionRefusedError:
        print("hawesome: daemon not responding", file=sys.stderr)
        sys.exit(1)
    except TimeoutError:
        print("hawesome: daemon timed out", file=sys.stderr)
        sys.exit(1)


VALID_COMMANDS = {"cycle-mode", "cycle-variant", "status", "dump"}


def main() -> None:
    if len(sys.argv) < 2 or sys.argv[1] not in VALID_COMMANDS:
        print(f"Usage: hawesome-ctl <{'|'.join(sorted(VALID_COMMANDS))}>", file=sys.stderr)
        sys.exit(1)

    cmd    = sys.argv[1]
    reply  = send(cmd)

    # Pretty-print JSON replies
    try:
        parsed = json.loads(reply)
        print(json.dumps(parsed, indent=2))
    except json.JSONDecodeError:
        print(reply)


if __name__ == "__main__":
    main()
