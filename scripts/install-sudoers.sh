#!/bin/sh
#
# Install (or refresh) the passwordless sudoers rule used by `yabai --load-sa`.
#
# Run this once after installing yabai, and again after every upgrade, because
# the rule pins the sha256 of the installed binary:
#
#     yabai-sudoers
#
# Pass the path to yabai as the first argument if it is not on your PATH.
# Pass --no-load to only write the rule without reloading the
# scripting-addition.
#
set -eu

if [ "$(id -u)" -ne 0 ]; then
    self=$(cd "$(dirname "$0")" && pwd)/$(basename "$0")
    if [ -n "${SUDO_ASKPASS:-}" ]; then
        exec sudo -A -- "$self" "$@"
    fi
    exec sudo -- "$self" "$@"
fi

load_sa=1
if [ "${1:-}" = "--no-load" ]; then
    load_sa=
    shift
fi

target="${1:-}"
if [ -z "$target" ] && [ -n "${SUDO_USER:-}" ]; then
    target=$(sudo -u "$SUDO_USER" sh -lc 'command -v yabai' 2>/dev/null || true)
fi
if [ -z "$target" ]; then
    target=$(command -v yabai 2>/dev/null || true)
fi
if [ -z "$target" ]; then
    echo "yabai-sudoers: could not find yabai; pass its full path as the first argument" >&2
    exit 1
fi

digest=$(shasum -a 256 "$target" | cut -d' ' -f1)
user="${SUDO_USER:-$(id -un)}"
rule="$user ALL=(root) NOPASSWD: sha256:$digest $target --load-sa"

tmp=$(mktemp /tmp/yabai-sudoers.XXXXXX)
trap 'rm -f "$tmp"' EXIT

printf '%s\n' "$rule" > "$tmp"
visudo -cf "$tmp" >/dev/null
install -m 440 -o root -g wheel "$tmp" /private/etc/sudoers.d/yabai

echo "wrote /private/etc/sudoers.d/yabai"
echo "  $rule"

if [ -n "$load_sa" ]; then
    "$target" --load-sa
    echo "scripting-addition loaded"
fi
