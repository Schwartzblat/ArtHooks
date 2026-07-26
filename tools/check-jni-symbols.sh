#!/usr/bin/env bash
#
# Checks that every native method declared in ArtHooks.java is actually exported by libarthooks.so,
# for every ABI that was built.
#
# JNI binds by mangled symbol name, and the mangling encodes the parameter types. Changing a
# parameter, adding an overload or moving the class renames the symbol, and nothing complains until
# the method is called at runtime. This turns that into a build failure.
#
# Usage: tools/check-jni-symbols.sh [path-to-libarthooks.so ...]
#        With no arguments it checks every unstripped libarthooks.so under arthooks/build.

set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source_file="$root/arthooks/src/main/java/com/arthooks/ArtHooks.java"

nm_bin="$(command -v llvm-nm || true)"
if [ -z "$nm_bin" ]; then
    nm_bin="$(find "${ANDROID_NDK_HOME:-${ANDROID_HOME:-$HOME/Android/Sdk}}" \
        -name llvm-nm -type f 2>/dev/null | sort | tail -1 || true)"
fi
if [ -z "$nm_bin" ]; then
    nm_bin="$(command -v nm || true)"
fi
if [ -z "$nm_bin" ]; then
    echo "no nm/llvm-nm on PATH or in the NDK" >&2
    exit 1
fi

# Generate the authoritative symbol names with javac -h rather than mangling by hand. ArtHooks only
# touches android.os.Build, so a stub is enough to compile it standalone.
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

mkdir -p "$work/stub/android/os" "$work/headers" "$work/classes"
cat > "$work/stub/android/os/Build.java" <<'JAVA'
package android.os;

public class Build {
    public static class VERSION {
        public static final int SDK_INT = 0;
    }
}
JAVA

javac -nowarn -h "$work/headers" -d "$work/classes" \
    "$work/stub/android/os/Build.java" "$source_file" 2>/dev/null

expected="$(grep -oh 'Java_com_arthooks_[A-Za-z0-9_]*' "$work/headers"/*.h | sort -u)"
if [ -z "$expected" ]; then
    echo "javac -h produced no JNI symbols for $source_file" >&2
    exit 1
fi

libraries=("$@")
if [ ${#libraries[@]} -eq 0 ]; then
    mapfile -t libraries < <(find "$root/arthooks/build/intermediates/cxx" -name libarthooks.so 2>/dev/null | sort)
fi
if [ ${#libraries[@]} -eq 0 ]; then
    echo "no libarthooks.so found -- build the library first (./gradlew :arthooks:assembleDebug)" >&2
    exit 1
fi

echo "expecting $(echo "$expected" | wc -l) JNI symbols:"
echo "$expected" | sed 's/^/  /'
echo

status=0
for library in "${libraries[@]}"; do
    abi="$(basename "$(dirname "$library")")"
    exported="$("$nm_bin" --defined-only --extern-only "$library" | grep -o 'Java_[A-Za-z0-9_]*' | sort -u || true)"

    missing=""
    while read -r symbol; do
        [ -z "$symbol" ] && continue
        if ! grep -qx "$symbol" <<<"$exported"; then
            missing="$missing $symbol"
        fi
    done <<<"$expected"

    if [ -n "$missing" ]; then
        echo "FAIL $abi: missing$missing"
        status=1
    else
        echo "ok   $abi"
    fi
done

exit $status
