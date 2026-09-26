#!/usr/bin/env bash
#
# Measures what giving up AOT code costs, across the four configurations that matter.
#
# Row by row: an ordinary AOT build with ArtHooks leaving it alone (the baseline you cannot actually
# have if you want hooks to fire), the same with ArtHooks dropping AOT code, and both of those with
# the app compiled `verify` -- which is what android:vmSafeMode="true" forces the package manager to
# do. `arthooks.keep_aot` is what turns ArtHooks' half on and off, and it is read from a system
# property so that one APK can produce every row.
#
# Usage: tools/run-benchmark.sh [repeats]        # default 3
#
# A phone is a noisy place to benchmark: DVFS, big.LITTLE placement and thermal drift move a number
# further than the thing being measured does. Two things keep it honest. The whole four-config
# matrix is repeated, so a config is never the only one running while the device is hot. And the
# figure reported is the *minimum* across every sample, because noise can only ever add time -- a
# median would mostly be measuring the governor.
#
# Even so, treat single-digit-percent differences as nothing.

set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
package="com.arthooks"
activity="com.example.arthooks.MainActivity"
repeats="${1:-3}"

sdk_dir="${ANDROID_HOME:-${ANDROID_SDK_ROOT:-$HOME/Android/Sdk}}"
build_tools="$(find "$sdk_dir/build-tools" -maxdepth 1 -mindepth 1 -type d 2>/dev/null \
    | sort -V | tail -1)"
if [ -z "$build_tools" ]; then
    echo "no build-tools under $sdk_dir; set ANDROID_HOME" >&2
    exit 1
fi

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
samples="$work/samples.txt"
: > "$samples"

keystore="$HOME/.android/debug.keystore"
if [ ! -f "$keystore" ]; then
    mkdir -p "$(dirname "$keystore")"
    keytool -genkeypair -keystore "$keystore" -storepass android -keypass android \
        -alias androiddebugkey -keyalg RSA -keysize 2048 -validity 10950 \
        -dname "CN=Android Debug,O=Android,C=US" > /dev/null
fi

echo "building and installing the release APK..."
"$root/gradlew" -p "$root" :app:assembleRelease -q
"$build_tools/zipalign" -f -p 4 \
    "$root/app/build/outputs/apk/release/app-release-unsigned.apk" "$work/bench.apk" > /dev/null
"$build_tools/apksigner" sign --ks "$keystore" --ks-pass pass:android --key-pass pass:android \
    --ks-key-alias androiddebugkey "$work/bench.apk" > /dev/null
adb wait-for-device
adb install -r -d "$work/bench.apk" > /dev/null

# One config, one pass: apply the compiler filter, launch in benchmark mode, wait for the END line.
measure() {
    local filter="$1" keep_aot="$2" label="$3"

    adb shell cmd package compile -m "$filter" -f "$package" > /dev/null
    adb shell am force-stop "$package"
    local started_at
    started_at="$(adb shell "date '+%m-%d %H:%M:%S.000'" | tr -d '\r')"

    adb shell am start -n "$package/$activity" --ez benchmark true \
        --es arthooks_keep_aot "$keep_aot" > /dev/null

    local deadline=$(( $(date +%s) + 900 ))
    while [ "$(date +%s)" -lt "$deadline" ]; do
        if adb logcat -t "$started_at" -s HookBenchmark:V 2>/dev/null | grep -q "END sink="; then
            break
        fi
        sleep 3
    done

    adb logcat -t "$started_at" -s HookBenchmark:V 2>/dev/null \
        | sed -n "s/.*RESULT \([a-z_]*\) *first= *\([0-9]*\)us *steady= *[0-9]*us *best= *\([0-9]*\)us.*/$label \1 \2 \3/p" \
        >> "$samples"
}

for pass in $(seq 1 "$repeats"); do
    echo "pass $pass of $repeats..."
    measure speed  true  aot_baseline
    measure speed  false aot_plus_disable
    measure verify true  vmsafemode
    measure verify false vmsafemode_plus_disable
done

python3 - "$samples" <<'PY'
import collections, sys

labels = [
    ("aot_baseline",            "AOT baseline (no ArtHooks)"),
    ("aot_plus_disable",        "AOT + disable_aot"),
    ("vmsafemode",              "vmSafeMode only"),
    ("vmsafemode_plus_disable", "vmSafeMode + disable_aot"),
]

first = collections.defaultdict(list)
best = collections.defaultdict(list)
order = []
for line in open(sys.argv[1]):
    config, workload, f, b = line.split()
    if workload not in order:
        order.append(workload)
    first[(config, workload)].append(int(f))
    best[(config, workload)].append(int(b))

def table(title, data, note):
    print()
    print(title)
    print(note)
    header = f"{'workload':<14}" + "".join(f"{name:>26}" for _, name in labels)
    print(header)
    print("-" * len(header))
    for workload in order:
        row = f"{workload:<14}"
        baseline = min(data.get((labels[0][0], workload), [0])) or 1
        for config, _ in labels:
            values = data.get((config, workload))
            if not values:
                row += f"{'-':>26}"
                continue
            value = min(values)
            row += f"{value/1000.0:>18.1f}ms {value/baseline:>6.1f}x"
        print(row)

table("FIRST ROUND -- code that has not been JIT-compiled yet (min across repeats)",
      first, "the number a user feels the first time a code path runs")
table("STEADY STATE -- after the JIT has caught up (min across repeats)",
      best, "throughput once the code is hot")
PY
