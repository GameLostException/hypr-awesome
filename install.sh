#!/usr/bin/env bash
# install.sh — wire hypr-awesome into Hyprland config
#
# What this does:
#   1. Makes hawesome.py and hawesome-ctl.py executable
#   2. Creates symlinks in ~/.config/hypr/ for convenience
#   3. Prints the hyprland.conf snippets to add (does NOT modify the file automatically)

set -euo pipefail

REPO_DIR="$(cd "$(dirname "$0")" && pwd)"
HYPR_CFG="$HOME/.config/hypr"

echo "==> Making scripts executable"
chmod +x "$REPO_DIR/hawesome.py"
chmod +x "$REPO_DIR/hawesome-ctl.py"

echo "==> Creating symlinks in $HYPR_CFG"
ln -sf "$REPO_DIR/hawesome.py"     "$HYPR_CFG/hawesome.py"
ln -sf "$REPO_DIR/hawesome-ctl.py" "$HYPR_CFG/hawesome-ctl.py"

echo ""
echo "==> Done. Add the following to ~/.config/hypr/hyprland.conf:"
echo ""
echo "# ─── hypr-awesome ────────────────────────────────────────────────────────────"
echo "exec-once = python3 $REPO_DIR/hawesome.py"
echo ""
echo "# Layout mode cycle (replaces toggle-monocle.sh)"
echo "bind = \$mod,       m, exec, python3 $REPO_DIR/hawesome-ctl.py cycle-mode"
echo "# Layout variant cycle (replaces toggle-split.sh)"
echo "bind = \$mod SHIFT, m, exec, python3 $REPO_DIR/hawesome-ctl.py cycle-variant"
echo ""
echo "==> Also REMOVE or COMMENT OUT these old binds from hyprland.conf:"
echo "    bind = \$mod,       m,         fullscreen, 1"
echo "    bind = \$mod,       m,         exec, ~/.config/hypr/toggle-monocle.sh"
echo "    bind = \$mod SHIFT, m,         exec, ~/.config/hypr/toggle-split.sh"
