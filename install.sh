#!/usr/bin/env bash
# install.sh — build and wire hypr-awesome into Hyprland config
#
# What this does:
#   1. Builds the plugin .so
#   2. Makes hawesome-ctl.py executable and symlinks it into ~/.config/hypr/
#   3. Prints the hyprland.lua snippets to add (does NOT modify the file automatically)
#
# Hyprland v0.57+: plugins load via hl.plugin.load() in hyprland.lua.
# Layout dispatchers are called via hawesome-ctl.py (control socket) since
# hyprctl dispatch cannot parse 'hawesome:cycle-mode' through the lua router.

set -euo pipefail

REPO_DIR="$(cd "$(dirname "$0")" && pwd)"
HYPR_CFG="$HOME/.config/hypr"

echo "==> Building plugin"
make -C "$REPO_DIR/plugin/src" -j"$(nproc)"

echo "==> Making hawesome-ctl.py executable"
chmod +x "$REPO_DIR/hawesome-ctl.py"

echo "==> Creating symlink in $HYPR_CFG"
ln -sf "$REPO_DIR/hawesome-ctl.py" "$HYPR_CFG/hawesome-ctl.py"

echo ""
echo "==> Done. Add the following to ~/.config/hypr/hyprland.lua:"
echo ""
echo "-- ─── hypr-awesome ──────────────────────────────────────────────────────────"
echo "hl.plugin.load(\"$REPO_DIR/plugin/build/hawesome.so\")"
echo ""
echo "-- Layout mode cycle (uses hawesome-ctl.py control socket — hyprctl dispatch"
echo "-- cannot parse 'hawesome:cycle-mode' in Hyprland v0.57+ lua router)"
echo "hl.bind(mod .. \" + m\",         hl.dsp.exec_cmd(\"python3 $REPO_DIR/hawesome-ctl.py cycle-mode\"))"
echo "hl.bind(mod .. \" + SHIFT + m\", hl.dsp.exec_cmd(\"python3 $REPO_DIR/hawesome-ctl.py cycle-variant\"))"
echo ""
echo "==> After a Hyprland upgrade, rebuild before the next boot:"
echo "    cd $REPO_DIR/plugin/src && make clean && make -j\$(nproc)"
