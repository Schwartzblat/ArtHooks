#!/usr/bin/env bash
#
# Installs the demo on the connected device or emulator, runs the self-test, and exits non-zero
# unless every check passed.
#
# The suite reports to logcat rather than through an instrumentation runner, so this polls for the
# verdict. It takes roughly six seconds, most of it deliberately waiting for the JIT.

set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
package="com.arthooks"
activity="com.example.arthooks.MainActivity"
timeout_seconds="${SELFTEST_TIMEOUT:-180}"

adb wait-for-device
"$root/gradlew" -p "$root" :app:installDebug

adb logcat -c
adb shell am force-stop "$package" || true
adb shell am start -n "$package/$activity" > /dev/null

echo "waiting up to ${timeout_seconds}s for the verdict..."
deadline=$(( $(date +%s) + timeout_seconds ))
verdict=""
while [ "$(date +%s)" -lt "$deadline" ]; do
    log="$(adb logcat -d -s HookSelfTest:V)"
    if grep -q "all checks passed" <<<"$log"; then
        verdict="pass"
        break
    fi
    if grep -q "FAIL:" <<<"$log"; then
        verdict="fail"
        break
    fi
    sleep 2
done

echo
echo "--- HookSelfTest ---"
adb logcat -d -s HookSelfTest:V | sed 's/^/  /'

case "$verdict" in
    pass)
        echo
        echo "self-test passed ($(adb logcat -d -s HookSelfTest:V | grep -c 'PASS:') PASS lines)"
        ;;
    fail)
        echo
        echo "self-test FAILED" >&2
        echo "--- ArtHooks (native) ---" >&2
        adb logcat -d -s ArtHooks:V | sed 's/^/  /' >&2
        exit 1
        ;;
    *)
        echo
        echo "self-test did not report a verdict within ${timeout_seconds}s" >&2
        echo "--- crash log, if any ---" >&2
        adb logcat -d -s AndroidRuntime:E DEBUG:V libc:F | sed 's/^/  /' >&2
        exit 1
        ;;
esac
