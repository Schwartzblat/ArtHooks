//
// Created by alon on 9/26/26.
//

#include "deoptimize.hpp"

#include "art_symbols.hpp"
#include "log.hpp"

#include <sys/mman.h>
#include <unistd.h>

#include <atomic>
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

// art::ClassLinker::VisitClasses(art::ClassVisitor*). Never called for its own sake: libart's PLT
// slot for it is where the Instrumentation* that DeoptimizeBootImage() builds its visitor around is
// read.
constexpr const char *kVisitClasses = "_ZN3art11ClassLinker12VisitClassesEPNS_12ClassVisitorE";

// art::instrumentation::Instrumentation::UpdateEntrypointsForDebuggable(), Android 14 onwards. It is
// what ART itself runs to move every class it has *already* linked off its AOT code when a started
// runtime turns debuggable -- openjdkjvmti's DeoptManager::FinishSetup() is the in-tree caller, right
// after SetRuntimeDebugState(kJavaDebuggable), exactly as here.
constexpr const char *kUpdateEntrypointsForDebuggable =
        "_ZN3art15instrumentation15Instrumentation30UpdateEntrypointsForDebuggableEv";

// art::Runtime::RuntimeDebugState::kJavaDebuggable. The enum is
// { kNonJavaDebuggable, kJavaDebuggable, kJavaDebuggableAtInit }, and IsJavaDebuggable() is true
// for the latter two. kJavaDebuggableAtInit means "started debuggable" and is deliberately not used
// here -- ART treats it as a state it must never be moved out of.
constexpr int kJavaDebuggable = 1;

// How far into art::Runtime to look for the Instrumentation* read out of the visitor. Runtime owns
// it through a std::unique_ptr member -- +0x328 on API 36 -- so it is one of Runtime's leading words.
constexpr size_t kRuntimeScanBytes = 4096;

using SetRuntimeDebugStateFn = void (*)(void *runtime, int state);
using SetJavaDebuggableFn = void (*)(void *runtime, bool value);
using DeoptimizeBootImageFn = void (*)(void *runtime);
using SuspendAllCtorFn = void (*)(void *scope, const char *cause, bool long_suspend);
using SuspendAllDtorFn = void (*)(void *scope);
using VisitClassesFn = void (*)(void *class_linker, void *visitor);
using UpdateEntrypointsForDebuggableFn = void (*)(void *instrumentation);

void **g_runtime_instance = nullptr;
SetRuntimeDebugStateFn g_set_runtime_debug_state = nullptr;
SetJavaDebuggableFn g_set_java_debuggable = nullptr;
DeoptimizeBootImageFn g_deoptimize_boot_image = nullptr;
SuspendAllCtorFn g_suspend_all_ctor = nullptr;
SuspendAllDtorFn g_suspend_all_dtor = nullptr;
UpdateEntrypointsForDebuggableFn g_update_entrypoints_for_debuggable = nullptr;
void **g_visit_classes_slot = nullptr;
// What that slot held before capture_visit_classes() took it over, and so what it forwards to and
// what is put back -- not the symbol's own address, in case something else had redirected it first.
VisitClassesFn g_visit_classes_original = nullptr;

bool g_resolved = false;
bool g_disabled = false;
bool g_linked_code_dropped = false;

// The one thread capture_visit_classes() reads arguments for; every other caller is forwarded
// untouched. Atomic because those other callers read it too.
std::atomic<pid_t> g_capturing_thread{0};
void *g_captured_class_linker = nullptr;
void *g_captured_instrumentation = nullptr;

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
    g_update_entrypoints_for_debuggable = reinterpret_cast<UpdateEntrypointsForDebuggableFn>(
            find_libart_symbol(kUpdateEntrypointsForDebuggable));
    g_visit_classes_slot = find_libart_plt_slot(kVisitClasses);
}

/** One line of /proc/self/maps: where a mapping is, how it is protected, and what backs it. */
struct Mapping {
    uintptr_t start;
    uintptr_t end;
    int protection;  // PROT_* bits
    char path[512];  // empty for an anonymous mapping
};

/**
 * Strips the tag arm64 heap pointers carry in their top byte. The hardware ignores it on a load
 * (Top Byte Ignore), but /proc/self/maps lists untagged addresses, so a heap object such as
 * art::Runtime is in no mapping until it is removed.
 */
uintptr_t untagged(const void *address) {
#if defined(__aarch64__)
    return reinterpret_cast<uintptr_t>(address) & ((uintptr_t{1} << 56) - 1);
#else
    return reinterpret_cast<uintptr_t>(address);
#endif
}

/**
 * Finds the mapping that contains `address`, tagged or not.
 *
 * Returns false when the address is in no mapping at all. The single owner of the /proc/self/maps
 * walk, shared by is_aot_code(), is_in_libart() and the PLT-slot write. It allocates (fopen), so it
 * must not run while every other thread is suspended -- one of them may hold the allocator's lock.
 */
bool find_mapping(const void *address, Mapping *out) {
    out->path[0] = '\0';
    FILE *maps = fopen("/proc/self/maps", "re");
    if (maps == nullptr) {
        return false;
    }

    const size_t target = untagged(address);
    bool found = false;
    char line[512];
    while (fgets(line, sizeof(line), maps) != nullptr) {
        size_t start = 0;
        size_t end = 0;
        char permissions[5] = {};
        int path_offset = 0;
        // The path is optional, so it is captured by offset rather than by conversion: %n records
        // where parsing stopped whether or not a name follows.
        if (sscanf(line, "%zx-%zx %4s %*x %*s %*u %n", &start, &end, permissions, &path_offset) < 3) {
            continue;
        }
        if (target < start || target >= end) {
            continue;
        }

        out->start = start;
        out->end = end;
        out->protection = (permissions[0] == 'r' ? PROT_READ : 0) |
                          (permissions[1] == 'w' ? PROT_WRITE : 0) |
                          (permissions[2] == 'x' ? PROT_EXEC : 0);

        const char *path = (path_offset > 0) ? line + path_offset : "";
        while (*path == ' ') {
            path++;
        }
        // fgets keeps the trailing newline, which would otherwise end up inside the copied path.
        size_t length = strlen(path);
        while (length > 0 && (path[length - 1] == '\n' || path[length - 1] == '\r')) {
            length--;
        }
        if (length >= sizeof(out->path)) {
            length = sizeof(out->path) - 1;
        }
        memcpy(out->path, path, length);
        out->path[length] = '\0';
        found = true;
        break;
    }

    fclose(maps);
    return found;
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
 * Stands in for ClassLinker::VisitClasses while DeoptimizeBootImage() runs: reads the
 * Instrumentation* that call's visitor carries, then does exactly what the real function would.
 *
 * Runtime::GetInstrumentation() is an inline accessor over a struct offset nothing here can
 * measure, and UpdateEntrypointsForDebuggable() needs its result as `this`. DeoptimizeBootImage()
 * hands it over anyway, as the one member of the visitor it passes to VisitClasses --
 * DeoptimizeBootImageClassVisitor, UpdateEntryPointsClassVisitor before Android 15, both declared as
 * a vtable pointer followed by `Instrumentation* const instrumentation_`. That one-member layout is
 * the only assumption made about an ART type here, and validate_instrumentation() checks the value
 * before anything is called with it.
 *
 * Only the capturing thread is read from. Any other thread reaching VisitClasses through the slot
 * meanwhile -- one in native code is not stopped by suspending all threads -- is forwarded as is.
 */
void capture_visit_classes(void *class_linker, void *visitor) {
    if (g_capturing_thread.load(std::memory_order_relaxed) == gettid() &&
        g_captured_instrumentation == nullptr && visitor != nullptr) {
        g_captured_class_linker = class_linker;
        g_captured_instrumentation = static_cast<void **>(visitor)[1];
    }
    g_visit_classes_original(class_linker, visitor);
}

/**
 * Stores `value` into a GOT slot. The slot lives in libart's RELRO, which is read-only once the
 * linker is done with it, so the page is made writable for the one store and put back after.
 *
 * A pointer-aligned slot cannot straddle a page boundary, so one page covers it. If restoring the
 * protection fails the page is merely left writable; the store itself has already landed.
 */
bool write_slot(void **slot, void *value, int protection) {
    const auto page_size = static_cast<uintptr_t>(sysconf(_SC_PAGESIZE));
    void *page = reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(slot) & ~(page_size - 1));
    if (mprotect(page, page_size, protection | PROT_WRITE) != 0) {
        return false;
    }
    __atomic_store_n(slot, value, __ATOMIC_RELEASE);
    mprotect(page, page_size, protection);
    return true;
}

/**
 * Whether `candidate` is plausibly art::Runtime's Instrumentation*.
 *
 * It was read out of a visitor whose layout comes from AOSP's source rather than from anything
 * measured, and calling UpdateEntrypointsForDebuggable() on a wrong `this` crashes the app at
 * startup, so it has to earn trust. Runtime owns its Instrumentation, so the pointer must be one of
 * Runtime's own words; and it must not be either of the other two pointers in play, the Runtime or
 * the ClassLinker VisitClasses was called on. A visitor whose member turned out to be anything else
 * either fails the first test or is caught by the second.
 */
bool validate_instrumentation(const void *candidate, const void *runtime, const void *class_linker,
                              size_t runtime_scan_bytes) {
    if (candidate == nullptr || candidate == runtime || candidate == class_linker) {
        return false;
    }
    const auto *words = static_cast<const void *const *>(runtime);
    for (size_t i = 0; i < runtime_scan_bytes / sizeof(void *); i++) {
        if (words[i] == candidate) {
            return true;
        }
    }
    return false;
}

/** What dropping already-linked AOT code needs from /proc/self/maps, read before suspending. */
struct LinkedCodePlan {
    int slot_protection;
    size_t runtime_scan_bytes;
};

/**
 * Checks that classes ART already linked can be moved off their AOT code, and reads what that needs
 * out of /proc/self/maps while doing so is still safe. Returns the reason it cannot, or nullptr.
 */
const char *plan_linked_code_drop(void *runtime, LinkedCodePlan *plan) {
    if (g_update_entrypoints_for_debuggable == nullptr) {
        return "libart has no Instrumentation::UpdateEntrypointsForDebuggable(), which Android 14 "
               "added";
    }
    if (g_visit_classes_slot == nullptr) {
        return "libart does not call ClassLinker::VisitClasses through its PLT, so the "
               "Instrumentation* cannot be read";
    }

    Mapping mapping = {};
    if (!find_mapping(g_visit_classes_slot, &mapping)) {
        return "libart's PLT slot for ClassLinker::VisitClasses is in no mapping";
    }
    plan->slot_protection = mapping.protection;

    if (!find_mapping(runtime, &mapping)) {
        return "art::Runtime is in no mapping";
    }
    const size_t mapped = mapping.end - untagged(runtime);
    plan->runtime_scan_bytes = mapped < kRuntimeScanBytes ? mapped : kRuntimeScanBytes;
    return nullptr;
}

/**
 * Moves every method ART has linked so far off its AOT code, with every other thread stopped.
 *
 * Runs Runtime::DeoptimizeBootImage(), which handles the boot image and hands the JIT's
 * non-debuggable code over, and -- when `plan` is non-null -- reads the Instrumentation* out of it
 * on the way and then runs Instrumentation::UpdateEntrypointsForDebuggable() for everything else:
 * the app's own classes, including all of an app image's, which ART links when the class loader
 * opens and so before any of this library can run. Both walk every loaded class rewriting entry
 * points, and their visitors require the mutator lock held *exclusively* -- which is what
 * suspending all threads grants, and what ART's own call sites do. Being inside a JNI method is what
 * makes it legal to ask for: the thread is in the native state, so it holds no mutator lock to
 * deadlock against itself with.
 *
 * Returns whether the boot image was deoptimized; `*not_dropped_because` is left null only if the
 * app's already-linked classes were moved off their AOT code too.
 */
bool drop_linked_aot_code(void *runtime, const LinkedCodePlan *plan,
                          const char **not_dropped_because) {
    if (g_deoptimize_boot_image == nullptr || g_suspend_all_ctor == nullptr ||
        g_suspend_all_dtor == nullptr) {
        *not_dropped_because = "libart does not export DeoptimizeBootImage or ScopedSuspendAll";
        return false;
    }

    // ScopedSuspendAll is a ValueObject with no members; it exists only for its constructor and
    // destructor. Give it room anyway rather than passing a one-byte object to a foreign ABI.
    alignas(16) unsigned char scope[32] = {};
    g_suspend_all_ctor(scope, "ArtHooks: dropping AOT code", /*long_suspend=*/false);

    bool captured = false;
    if (plan != nullptr) {
        g_capturing_thread.store(gettid(), std::memory_order_relaxed);
        g_visit_classes_original = reinterpret_cast<VisitClassesFn>(*g_visit_classes_slot);
        captured = write_slot(g_visit_classes_slot,
                              reinterpret_cast<void *>(&capture_visit_classes),
                              plan->slot_protection);
    }

    g_deoptimize_boot_image(runtime);

    if (captured) {
        write_slot(g_visit_classes_slot, reinterpret_cast<void *>(g_visit_classes_original),
                   plan->slot_protection);
    }
    g_capturing_thread.store(0, std::memory_order_relaxed);

    if (plan == nullptr) {
        // *not_dropped_because already says why.
    } else if (!captured) {
        *not_dropped_because = "libart's PLT slot for ClassLinker::VisitClasses could not be "
                               "made writable";
    } else if (!validate_instrumentation(g_captured_instrumentation, runtime,
                                         g_captured_class_linker, plan->runtime_scan_bytes)) {
        *not_dropped_because = "DeoptimizeBootImage's visitor did not carry an Instrumentation* "
                               "that art::Runtime holds";
    } else {
        g_update_entrypoints_for_debuggable(g_captured_instrumentation);
        g_linked_code_dropped = true;
    }

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
    // Instrumentation::CanUseAotCode(), which now says no. What it linked *earlier* still holds its
    // AOT bodies, so those are rewritten next, the way ART does it when a debugger makes a running
    // app debuggable.
    LinkedCodePlan plan = {};
    const char *not_dropped_because = plan_linked_code_drop(runtime, &plan);
    const bool boot_image =
            drop_linked_aot_code(runtime, not_dropped_because == nullptr ? &plan : nullptr,
                                 &not_dropped_because);

    if (g_linked_code_dropped) {
        LOGI("ART will no longer run AOT code, and every class it had already linked was moved off "
             "its AOT code; hooks now survive a speed/speed-profile build");
        return true;
    }

    LOGI("ART will no longer run AOT code%s",
         boot_image ? ", and the boot image was deoptimized" : " for classes loaded from here on");
    LOGW("classes ART linked before ArtHooks loaded keep their AOT code (%s) -- a caller among them "
         "that dex2oat inlined a target into will keep running the target's original body. A "
         "speed-profile install's app image links many classes that early.",
         not_dropped_because);
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

bool is_aot_code(const void *entry_point) {
    if (entry_point == nullptr) {
        return false;
    }

    Mapping mapping = {};
    if (!find_mapping(entry_point, &mapping)) {
        return false;
    }

    // An anonymous mapping is the JIT's code cache, and every ART stub -- nterp, the interpreter
    // bridge, the resolution trampoline -- is inside libart.so. Anything else backed by a file is an
    // oat file, which is to say compiled code.
    const bool is_aot = mapping.path[0] == '/' && strstr(mapping.path, ".so") == nullptr;
    if (is_aot) {
        LOGD("entry point %p is AOT code from %s", entry_point, mapping.path);
    }
    return is_aot;
}

bool is_in_libart(const void *address) {
    if (address == nullptr) {
        return false;
    }

    Mapping mapping = {};
    if (!find_mapping(address, &mapping)) {
        return false;
    }

    const char *last_slash = strrchr(mapping.path, '/');
    const char *base = (last_slash != nullptr) ? last_slash + 1 : mapping.path;
    return strcmp(base, "libart.so") == 0;
}
