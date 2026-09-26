//
// Created by alon on 9/26/26.
//

#include "deoptimize.hpp"

#include "art_symbols.hpp"
#include "log.hpp"

#include <cstdio>
#include <cstring>

namespace {

// art::Runtime::instance_, the process-wide runtime singleton. A data symbol, so what the lookup
// finds is the address *of the pointer*, not the pointer.
constexpr const char *kRuntimeInstance = "_ZN3art7Runtime9instance_E";

// art::Runtime::SetRuntimeDebugState(art::Runtime::RuntimeDebugState). Android 13 renamed this from
// SetJavaDebuggable(bool); minSdk is 33, so the old name should never be the one that matches, but
// it costs one lookup to not care.
constexpr const char *kSetRuntimeDebugState =
        "_ZN3art7Runtime20SetRuntimeDebugStateENS0_17RuntimeDebugStateE";
constexpr const char *kSetJavaDebuggable = "_ZN3art7Runtime17SetJavaDebuggableEb";

// art::Runtime::DeoptimizeBootImage().
constexpr const char *kDeoptimizeBootImage = "_ZN3art7Runtime19DeoptimizeBootImageEv";

// art::ScopedSuspendAll::{ScopedSuspendAll(char const*, bool), ~ScopedSuspendAll()}.
constexpr const char *kSuspendAllCtor = "_ZN3art16ScopedSuspendAllC1EPKcb";
constexpr const char *kSuspendAllDtor = "_ZN3art16ScopedSuspendAllD1Ev";

// art::Runtime::RuntimeDebugState::kJavaDebuggable. The enum is
// { kNonJavaDebuggable, kJavaDebuggable, kJavaDebuggableAtInit }, and IsJavaDebuggable() is true
// for the latter two. kJavaDebuggableAtInit means "started debuggable" and is deliberately not used
// here -- ART treats it as a state it must never be moved out of.
constexpr int kJavaDebuggable = 1;

using SetRuntimeDebugStateFn = void (*)(void *runtime, int state);
using SetJavaDebuggableFn = void (*)(void *runtime, bool value);
using DeoptimizeBootImageFn = void (*)(void *runtime);
using SuspendAllCtorFn = void (*)(void *scope, const char *cause, bool long_suspend);
using SuspendAllDtorFn = void (*)(void *scope);

void **g_runtime_instance = nullptr;
SetRuntimeDebugStateFn g_set_runtime_debug_state = nullptr;
SetJavaDebuggableFn g_set_java_debuggable = nullptr;
DeoptimizeBootImageFn g_deoptimize_boot_image = nullptr;
SuspendAllCtorFn g_suspend_all_ctor = nullptr;
SuspendAllDtorFn g_suspend_all_dtor = nullptr;

bool g_resolved = false;
bool g_disabled = false;

void resolve_symbols() {
    if (g_resolved) {
        return;
    }
    g_resolved = true;

    g_runtime_instance = static_cast<void **>(find_libart_symbol(kRuntimeInstance));
    g_set_runtime_debug_state =
            reinterpret_cast<SetRuntimeDebugStateFn>(find_libart_symbol(kSetRuntimeDebugState));
    g_set_java_debuggable =
            reinterpret_cast<SetJavaDebuggableFn>(find_libart_symbol(kSetJavaDebuggable));
    g_deoptimize_boot_image =
            reinterpret_cast<DeoptimizeBootImageFn>(find_libart_symbol(kDeoptimizeBootImage));
    g_suspend_all_ctor = reinterpret_cast<SuspendAllCtorFn>(find_libart_symbol(kSuspendAllCtor));
    g_suspend_all_dtor = reinterpret_cast<SuspendAllDtorFn>(find_libart_symbol(kSuspendAllDtor));
}

/** Flips the runtime's own view of whether it is debuggable, which is what gates AOT code. */
bool mark_java_debuggable(void *runtime) {
    if (g_set_runtime_debug_state != nullptr) {
        g_set_runtime_debug_state(runtime, kJavaDebuggable);
        return true;
    }
    if (g_set_java_debuggable != nullptr) {
        g_set_java_debuggable(runtime, true);
        return true;
    }
    return false;
}

/**
 * Runs Runtime::DeoptimizeBootImage() with every other thread stopped.
 *
 * It walks every loaded class rewriting entry points, and its visitor asserts the mutator lock is
 * held *exclusively* -- which is what suspending all threads grants. ART's own call site does
 * exactly this. Being inside a JNI method is what makes it legal to ask for: the thread is in the
 * native state, so it holds no mutator lock to deadlock against itself with.
 */
bool deoptimize_boot_image(void *runtime) {
    if (g_deoptimize_boot_image == nullptr || g_suspend_all_ctor == nullptr ||
        g_suspend_all_dtor == nullptr) {
        return false;
    }

    // ScopedSuspendAll is a ValueObject with no members; it exists only for its constructor and
    // destructor. Give it room anyway rather than passing a one-byte object to a foreign ABI.
    alignas(16) unsigned char scope[32] = {};
    g_suspend_all_ctor(scope, "ArtHooks: dropping AOT code", /*long_suspend=*/false);
    g_deoptimize_boot_image(runtime);
    g_suspend_all_dtor(scope);
    return true;
}

}  // namespace

bool can_disable_aot_code() {
    resolve_symbols();
    return g_runtime_instance != nullptr &&
           (g_set_runtime_debug_state != nullptr || g_set_java_debuggable != nullptr);
}

bool disable_aot_code() {
    if (g_disabled) {
        return true;
    }
    if (!can_disable_aot_code()) {
        LOGW("libart does not export what it takes to stop ART using AOT code; a target that "
             "dex2oat inlined into its callers cannot be hooked on this platform");
        return false;
    }

    void *runtime = *g_runtime_instance;
    if (runtime == nullptr) {
        LOGW("art::Runtime::instance_ is null; leaving AOT code alone");
        return false;
    }

    if (!mark_java_debuggable(runtime)) {
        return false;
    }
    g_disabled = true;

    // Everything ART links from here on ignores its AOT body -- ClassLinker::LinkCode asks
    // Instrumentation::CanUseAotCode(), which now says no. Classes it linked *earlier* keep theirs,
    // which is why this wants to run before the code being hooked is first touched.
    const bool boot_image = deoptimize_boot_image(runtime);

    LOGI("ART will no longer run AOT code%s; hooks now survive a speed/speed-profile build",
         boot_image ? ", and the boot image was deoptimized" : " for classes loaded from here on");
    if (!boot_image) {
        LOGW("could not deoptimize the boot image, so framework code that was already linked keeps "
             "its AOT bodies -- hooking a boot-classpath method may still miss calls made inside "
             "the framework");
    }
    return true;
}

bool aot_code_disabled() {
    return g_disabled;
}

namespace {

/**
 * Copies the file path backing the mapping that contains `address` into `path_out`.
 *
 * Returns false when the address is in no mapping at all; an anonymous mapping yields an empty
 * string. The single owner of the /proc/self/maps walk, shared by is_aot_code() and is_in_libart().
 */
bool mapping_path_for(const void *address, char *path_out, size_t path_cap) {
    path_out[0] = '\0';
    FILE *maps = fopen("/proc/self/maps", "re");
    if (maps == nullptr) {
        return false;
    }

    const size_t target = reinterpret_cast<size_t>(address);
    bool found = false;
    char line[512];
    while (fgets(line, sizeof(line), maps) != nullptr) {
        size_t start = 0;
        size_t end = 0;
        int path_offset = 0;
        // The path is optional, so it is captured by offset rather than by conversion: %n records
        // where parsing stopped whether or not a name follows.
        if (sscanf(line, "%zx-%zx %*4s %*x %*s %*u %n", &start, &end, &path_offset) < 2) {
            continue;
        }
        if (target < start || target >= end) {
            continue;
        }

        const char *path = (path_offset > 0) ? line + path_offset : "";
        while (*path == ' ') {
            path++;
        }
        // fgets keeps the trailing newline, which would otherwise end up inside the copied path.
        size_t length = strlen(path);
        while (length > 0 && (path[length - 1] == '\n' || path[length - 1] == '\r')) {
            length--;
        }
        if (length >= path_cap) {
            length = path_cap - 1;
        }
        memcpy(path_out, path, length);
        path_out[length] = '\0';
        found = true;
        break;
    }

    fclose(maps);
    return found;
}

}  // namespace

bool is_aot_code(const void *entry_point) {
    if (entry_point == nullptr) {
        return false;
    }

    char path[512];
    if (!mapping_path_for(entry_point, path, sizeof(path))) {
        return false;
    }

    // An anonymous mapping is the JIT's code cache, and every ART stub -- nterp, the interpreter
    // bridge, the resolution trampoline -- is inside libart.so. Anything else backed by a file is an
    // oat file, which is to say compiled code.
    const bool is_aot = path[0] == '/' && strstr(path, ".so") == nullptr;
    if (is_aot) {
        LOGD("entry point %p is AOT code from %s", entry_point, path);
    }
    return is_aot;
}

bool is_in_libart(const void *address) {
    if (address == nullptr) {
        return false;
    }

    char path[512];
    if (!mapping_path_for(address, path, sizeof(path))) {
        return false;
    }

    const char *last_slash = strrchr(path, '/');
    const char *base = (last_slash != nullptr) ? last_slash + 1 : path;
    return strcmp(base, "libart.so") == 0;
}
