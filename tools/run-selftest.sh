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
# Generous because the first launch after an install is far slower than later ones -- on a cold
# API 33 emulator it can produce nothing for well over a minute while the app is optimised. The
# script exits as soon as it sees a verdict, so a high ceiling costs nothing when things go well.
timeout_seconds="${SELFTEST_TIMEOUT:-600}"

adb wait-for-device
"$root/gradlew" -p "$root" :app:installDebug

# Emulators fail this often enough to matter ("failed to clear the 'main' log"), and under `set -e`
# an unguarded failure here kills the run before the app is even started. Nothing below depends on
# it succeeding -- the verdict is matched by timestamp instead -- so a failure is only cosmetic.
adb logcat -c 2>/dev/null || echo "note: could not clear logcat; filtering by timestamp instead"

# Device clock, not host clock: logcat stamps lines with the device's time, and an emulator's can
# be minutes off the runner's. Everything matched below is at or after this instant, so output
# from an earlier run cannot be mistaken for this one's verdict.
# Quoted for the *device* shell, not the host one: adb re-splits its arguments, so an unquoted
# format string arrives as two arguments and toybox date rejects it.
started_at="$(adb shell "date '+%m-%d %H:%M:%S.000'" | tr -d '\r')"

adb shell am force-stop "$package" || true
adb shell am start -n "$package/$activity" > /dev/null

self_test_log() {
    adb logcat -t "$started_at" -s HookSelfTest:V 2>/dev/null || true
}

echo "waiting up to ${timeout_seconds}s for the verdict..."
deadline=$(( $(date +%s) + timeout_seconds ))
verdict=""
while [ "$(date +%s)" -lt "$deadline" ]; do
    log="$(self_test_log)"
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
self_test_log | sed 's/^/  /'

case "$verdict" in
    pass)
        echo
        echo "self-test passed"
        ;;
    fail)
        echo
        echo "self-test FAILED" >&2
        echo "--- ArtHooks (native) ---" >&2
        adb logcat -t "$started_at" -s ArtHooks:V 2>/dev/null | sed 's/^/  /' >&2 || true
        exit 1
        ;;
    *)
        # No verdict at all means the app never got far enough to report one -- it crashed, it never
        # started, or it is still running. Each looks identical from the outside, so dump enough to
        # tell them apart rather than leaving the next reader to guess.
        echo
        echo "self-test did not report a verdict within ${timeout_seconds}s" >&2
        echo "  (raise it with SELFTEST_TIMEOUT=<seconds>)" >&2

        echo "--- is the process alive? ---" >&2
        if adb shell pidof "$package" > /dev/null 2>&1; then
            echo "  yes -- still running, so it is slow rather than dead" >&2
        else
            echo "  no -- it exited or never started" >&2
        fi

        echo "--- crash log, if any ---" >&2
        adb logcat -t "$started_at" -s AndroidRuntime:E DEBUG:V libc:F 2>/dev/null \
            | sed 's/^/  /' >&2 || true

        echo "--- ArtHooks (native) ---" >&2
        adb logcat -t "$started_at" -s ArtHooks:V 2>/dev/null | tail -40 | sed 's/^/  /' >&2 || true

        echo "--- last 60 lines from the app, whatever the tag ---" >&2
        pid="$(adb shell pidof "$package" 2>/dev/null | tr -d '\r' || true)"
        if [ -n "$pid" ]; then
            adb logcat -t "$started_at" --pid="$pid" 2>/dev/null | tail -60 | sed 's/^/  /' >&2 || true
        else
            adb logcat -t "$started_at" 2>/dev/null | grep -i arthooks | tail -60 \
                | sed 's/^/  /' >&2 || true
        fi
        exit 1
        ;;
esac
