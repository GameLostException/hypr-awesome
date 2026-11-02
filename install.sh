#!/usr/bin/env bash
# install.sh — build and wire hypr-awesome into Hyprland config
#
# What this does:
#   1. Builds the plugin .so
#   2. Makes hawesome-ctl.py executable and symlinks it into ~/.config/hypr/
#   3. Prints the hyprland.conf snippets to add (does NOT modify the file automatically)

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
echo "==> Done. Add the following to ~/.config/hypr/hyprland.conf:"
echo ""
echo "# ─── hypr-awesome ────────────────────────────────────────────────────────────"
echo "exec-once = hyprctl plugin load $REPO_DIR/plugin/build/hawesome.so"
echo ""
echo "# Layout mode cycle"
echo "bind = \$mod,       m, exec, python3 $REPO_DIR/hawesome-ctl.py cycle-mode"
echo "# Layout variant cycle"
echo "bind = \$mod SHIFT, m, exec, python3 $REPO_DIR/hawesome-ctl.py cycle-variant"
echo ""
echo "==> After a Hyprland upgrade, rebuild before the next boot:"
echo "    cd $REPO_DIR/plugin/src && make clean && make -j\$(nproc)"
