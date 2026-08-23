#!/usr/bin/env bash
# Registers parfait with Omarchy's theme-set hook so a running instance
# retints live when the theme changes. Safe to re-run.
set -euo pipefail
hook_dir="$HOME/.config/omarchy/hooks"
hook="$hook_dir/theme-set-parfait"
mkdir -p "$hook_dir"
cat > "$hook" <<'EOF'
#!/usr/bin/env bash
# Omarchy theme-set hook: ask a running parfait to reload colors.toml.
command -v parfait >/dev/null && parfait --retint || true
EOF
chmod +x "$hook"
echo "Installed $hook"
