#!/bin/bash
#
# The macOS counterpart of tools/check-dlls.ps1. Fails unless the binary
#
#   * contains both an arm64 and an x86_64 slice (one download for every Mac),
#   * links nothing but macOS itself (/usr/lib, /System/Library) -- no
#     Homebrew, no vcpkg dylib, nothing a clean Mac would be missing,
#   * weak-links ScreenCaptureKit, which macOS before 12.3 does not have,
#   * declares a minimum macOS of 10.15 (Intel) / 11.0 (Apple silicon), so a
#     newer SDK on the build machine cannot silently raise it.
#
#   tools/check-macos-binary.sh <path-to-soi-share>
#
set -euo pipefail

bin="${1:?usage: check-macos-binary.sh <binary>}"
fail=0
bad() { echo "FAIL  $*"; fail=1; }
ok()  { echo "ok    $*"; }

archs=$(lipo -archs "$bin")
for a in arm64 x86_64; do
    if [[ " $archs " == *" $a "* ]]; then ok "has an $a slice"; else bad "no $a slice (has: $archs)"; fi
done

for a in arm64 x86_64; do
    # Every load command naming a library, for this slice.
    while read -r lib; do
        case "$lib" in
            /usr/lib/*|/System/Library/*) ;;
            *) bad "$a links $lib, which is not part of macOS" ;;
        esac
    done < <(otool -arch "$a" -L "$bin" | tail -n +2 | awk '{print $1}')

    if otool -arch "$a" -l "$bin" | grep -B1 -A3 LC_LOAD_WEAK_DYLIB | grep -q ScreenCaptureKit; then
        ok "$a weak-links ScreenCaptureKit"
    else
        bad "$a does not weak-link ScreenCaptureKit (it would not start before macOS 12.3)"
    fi

    minos=$(otool -arch "$a" -l "$bin" | awk '/LC_BUILD_VERSION/{f=1} f&&/minos/{print $2; exit}')
    [ -n "$minos" ] || minos=$(otool -arch "$a" -l "$bin" | awk '/LC_VERSION_MIN_MACOSX/{f=1} f&&/version/{print $2; exit}')
    want=$([ "$a" = arm64 ] && echo 11.0 || echo 10.15)
    if [ "$(printf '%s\n%s\n' "$minos" "$want" | sort -V | tail -1)" = "$want" ]; then
        ok "$a runs on macOS $minos and later"
    else
        bad "$a needs macOS $minos, newer than the promised $want"
    fi
done

if codesign --verify "$bin" 2>/dev/null; then ok "signature verifies"; else bad "signature does not verify"; fi

echo
echo "libraries:"
otool -arch arm64 -L "$bin" | tail -n +2
exit $fail
