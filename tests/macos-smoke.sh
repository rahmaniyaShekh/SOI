#!/bin/bash
#
# The macOS counterpart of tests/packaging-smoke.ps1, run against the shipped
# binary in a throwaway HOME so nothing touches the real user:
#
#   * it runs from the folder it sits in, with no install
#   * `start` detaches: the launching shell exits, the share keeps running with
#     no controlling terminal, and `status` / `stop` reach it from elsewhere
#   * install puts it in ~/.soi-share/bin and on PATH via ~/.zshrc; uninstall
#     undoes both and keeps the share code unless --purge
#
#   tests/macos-smoke.sh <path-to-soi-share> <expected-version>
#
set -euo pipefail

src="${1:?usage: macos-smoke.sh <binary> <version>}"
want="${2:?usage: macos-smoke.sh <binary> <version>}"
src="$(cd "$(dirname "$src")" && pwd)/$(basename "$src")"

pass=0
fail=0
ok()  { echo "  PASS  $*"; pass=$((pass + 1)); }
bad() { echo "  FAIL  $*"; fail=$((fail + 1)); }
check() { local what="$1"; shift; if "$@"; then ok "$what"; else bad "$what"; fi; }

home=$(mktemp -d)
folder="$home/Downloads/soi-share"
mkdir -p "$folder"
cp "$src" "$folder/soi-share"
export HOME="$home"
export SHELL=/bin/zsh
unset SOI_SHARE_GITHUB_TOKEN GH_TOKEN GITHUB_TOKEN || true
# A hosted runner may not grant Screen Recording; the permission gate is
# tested by hand, the process lifecycle here.
export SOI_SHARE_SKIP_PERMISSION_CHECK=1
cd "$folder"
bin=./soi-share
state="$home/Library/Application Support/soi-share"

cleanup() {
    "$bin" stop --force >/dev/null 2>&1 || true
    rm -rf "$home"
}
trap cleanup EXIT

echo "== runs from its own folder =="
check "version is $want" [ "$("$bin" version --short)" = "$want" ]
check "help says to type ./soi-share here" sh -c "\"$bin\" help | grep -q './soi-share'"
check "nothing was installed by running it" [ ! -e "$home/.soi-share" ]

echo "== detached start / status / stop =="
# Started from a shell that exits straight away, like a terminal window being
# closed: the share must outlive it.
sh -c "\"$bin\" start --no-code --no-stun --port 0 > \"$home/start.log\" 2>&1; echo \$? > \"$home/start.rc\""
cat "$home/start.log"
check "start returns success" [ "$(cat "$home/start.rc")" = 0 ]
if [ "$(cat "$home/start.rc")" != 0 ]; then
    # Diagnose: the same instance, attached, so its own output is visible.
    echo "  -- the background copy failed; running it in the foreground for 8 s:"
    "$bin" start --foreground --no-code --no-stun --port 0 > "$home/fg.log" 2>&1 &
    fg=$!
    sleep 8
    kill "$fg" 2>/dev/null || true
    rc=0; wait "$fg" 2>/dev/null || rc=$?
    echo "  -- exit status $rc"
    sed 's/^/     | /' "$home/fg.log"
fi
sleep 2
status=$("$bin" status || true)
echo "$status"
check "status reaches the running share" sh -c "printf '%s' \"\$1\" | grep -q 'waiting for the viewer'" _ "$status"
pid=$(printf '%s\n' "$status" | awk '/^  pid/{print $2}')
if [ -n "$pid" ]; then
    info=$(ps -o ppid=,tty=,sess= -p "$pid" || true)
    echo "  (ppid tty sess: $info)"
    check "the share has no controlling terminal" sh -c "ps -o tty= -p $pid | grep -q '?'"
    check "the share was re-parented away from the shell that started it" \
        sh -c "[ \"\$(ps -o ppid= -p $pid | tr -d ' ')\" = 1 ]"
    check "its log is in the state folder" [ -s "$state/soi-share.log" ]
else
    bad "status reported no pid"
fi
check "a second start does not start a second share" sh -c "\"$bin\" start --no-code | grep -q 'already running'"
check "stop stops it" "$bin" stop
sleep 1
check "status then says not running" sh -c "\"$bin\" status | grep -q 'not running'"
[ -n "${pid:-}" ] && check "the process is gone" sh -c "! kill -0 $pid 2>/dev/null"

echo "== install / uninstall =="
"$bin" install
check "installed to ~/.soi-share/bin" [ -x "$home/.soi-share/bin/soi-share" ]
check "notices installed next to it" [ -s "$home/.soi-share/bin/THIRD_PARTY_NOTICES.md" ]
check "~/.zshrc puts it on PATH" grep -q 'export PATH="$PATH:$HOME/.soi-share/bin"' "$home/.zshrc"
check "a new zsh finds soi-share on PATH" \
    zsh -c "source \"$home/.zshrc\" && [ \"\$(command -v soi-share)\" = \"$home/.soi-share/bin/soi-share\" ]"
"$bin" install >/dev/null
check "installing twice leaves one PATH block" [ "$(grep -c '>>> soi-share >>>' "$home/.zshrc")" = 1 ]
check "the installed copy says it is installed" \
    sh -c "\"$home/.soi-share/bin/soi-share\" version | grep -q 'the installed copy'"

mkdir -p "$state"
echo "machine.code-test" > "$state/machine.code"
"$home/.soi-share/bin/soi-share" uninstall
check "uninstall removes the program" [ ! -e "$home/.soi-share" ]
check "uninstall removes the PATH block" sh -c "! grep -q 'soi-share' \"$home/.zshrc\""
check "uninstall keeps the share code" [ -f "$state/machine.code" ]
"$bin" uninstall --purge >/dev/null || true
check "uninstall --purge removes the share code" [ ! -f "$state/machine.code" ]

echo
echo "$pass passed, $fail failed"
[ "$fail" = 0 ]
