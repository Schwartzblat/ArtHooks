#!/usr/bin/env bash
#
# Runs the self-test against an ahead-of-time compiled build, which is the only configuration that
# can catch a hook being inlined away.
#
# tools/run-selftest.sh installs the *debug* APK, and a debuggable app is never inlined: AOSP's
# HInliner::Run() bails out on `graph_->IsDebuggable()`, and a debuggable APK makes both dex2oat and
# the JIT compile debuggable graphs. So that script structurally cannot see this class of failure.
# This one builds the release APK (non-debuggable), signs it so it can be installed, forces a
# compiler filter, and reads the same verdict.
#
# Usage: tools/run-aot-selftest.sh [compiler-filter]     # default: speed
#
# `speed` compiles everything and is the harsher test. `speed-profile` is what background dexopt
# actually applies in the field, but it only compiles what a profile says is hot, so a fresh install
# compiles almost nothing and the run proves little. `verify` produces no compiled code at all and
# is the control: a failure under `speed` that passes under `verify` is an inlining failure.

set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
package="com.arthooks"
activity="com.example.arthooks.MainActivity"
filter="${1:-speed}"
timeout_seconds="${SELFTEST_TIMEOUT:-600}"

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

sdk_dir="${ANDROID_HOME:-${ANDROID_SDK_ROOT:-$HOME/Android/Sdk}}"
build_tools="$(find "$sdk_dir/build-tools" -maxdepth 1 -mindepth 1 -type d 2>/dev/null \
    | sort -V | tail -1)"
if [ -z "$build_tools" ]; then
    echo "no build-tools under $sdk_dir; set ANDROID_HOME" >&2
    exit 1
fi

# The release variant has no signing config -- it is the library's demo, not a shipped app -- so the
# APK comes out unsigned and `adb install` rejects it. Sign it with the standard debug key, creating
# that keystore the way the Android tooling would if this is a machine that has never built one.
keystore="$HOME/.android/debug.keystore"
if [ ! -f "$keystore" ]; then
    echo "creating a debug keystore at $keystore"
    mkdir -p "$(dirname "$keystore")"
    keytool -genkeypair -keystore "$keystore" -storepass android -keypass android \
        -alias androiddebugkey -keyalg RSA -keysize 2048 -validity 10950 \
        -dname "CN=Android Debug,O=Android,C=US" > /dev/null
fi

echo "building the release APK..."
"$root/gradlew" -p "$root" :app:assembleRelease -q

unsigned="$root/app/build/outputs/apk/release/app-release-unsigned.apk"
signed="$work/app-release.apk"
"$build_tools/zipalign" -f -p 4 "$unsigned" "$signed" > /dev/null
"$build_tools/apksigner" sign --ks "$keystore" --ks-pass pass:android --key-pass pass:android \
    --ks-key-alias androiddebugkey "$signed"

adb wait-for-device
# A prior run-selftest.sh install may be signed with whatever debug key Gradle's own signing config
# used, which is not necessarily this script's $HOME/.android/debug.keystore -- on a fresh machine
# the two can differ, and installing a release APK signed with a different key over that fails with
# INSTALL_FAILED_UPDATE_INCOMPATIBLE. Drop whatever is there first; "not installed" is fine.
adb uninstall "$package" > /dev/null 2>&1 || true
# -d allows the downgrade from whatever versionCode a debug install left behind.
adb install -r -d "$signed" > /dev/null

echo "compiling $package with -m $filter..."
adb shell cmd package compile -m "$filter" -f "$package" > /dev/null
status="$(adb shell dumpsys package dexopt | grep -A2 "\[$package\]" | grep -o 'status=[a-z-]*' \
    | head -1 || true)"
echo "  dexopt reports $status"

adb logcat -c 2>/dev/null || echo "note: could not clear logcat; filtering by timestamp instead"
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
echo "--- HookSelfTest ($filter) ---"
self_test_log | sed 's/^/  /'

if [ "$verdict" = "pass" ]; then
    echo
    echo "AOT self-test passed under -m $filter"
    exit 0
fi

echo
echo "AOT self-test did not pass under -m $filter" >&2
echo "--- ArtHooks (native) ---" >&2
adb logcat -t "$started_at" -s ArtHooks:V 2>/dev/null | tail -40 | sed 's/^/  /' >&2 || true
exit 1
