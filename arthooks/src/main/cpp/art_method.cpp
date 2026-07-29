//
// Created by alon on 6/30/26.
//

#include "art_method.hpp"
#include "log.hpp"

#include <sys/mman.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

namespace {

jint g_sdk_version = 0;
jfieldID g_art_method_field = nullptr;
size_t g_art_method_size = 0;
size_t g_entry_point_offset = 0;

// Located at startup rather than assumed; see find_access_flags_offset(). Zero means "not found",
// which is a real possibility and only disables the compilation mitigation.
size_t g_access_flags_offset = 0;
bool g_access_flags_found = false;

// The method modifiers that appear verbatim in both access_flags_ and getModifiers(): public,
// private, protected, static, final, bridge, varargs, native, abstract, strict, synthetic.
//
// SYNCHRONIZED (0x20) is deliberately *not* here. A dex method never carries ACC_SYNCHRONIZED --
// it carries ACC_DECLARED_SYNCHRONIZED (0x20000), which is what ART keeps, and getModifiers()
// translates back to Java's 0x20 on the way out. Comparing that bit would make a synchronized probe
// mismatch its own flags, which is exactly what it did: 0x2a expected against 0x103a000a stored.
constexpr uint32_t kJavaMethodModifiers = 0x1dff & ~0x20u;

// art::kAccCompileDontBother, from art/runtime/modifiers.h. Unlike the layout, this cannot be
// measured -- there is no method known to already carry it -- so it is the one constant here taken
// on faith. It has held this value from Android 8 through 16; if a future release moves it, the
// effect is that the write lands on some other runtime flag, which is why it is only applied after
// the offset has been confirmed against two probes and is verified by reading it back.
constexpr uint32_t kAccCompileDontBother = 0x02000000;

/** Caches the field ID for the hidden java.lang.reflect.Executable.artMethod. */
bool find_art_method_field(JNIEnv *env) {
    jclass executable_class = env->FindClass("java/lang/reflect/Executable");
    if (executable_class == nullptr) {
        env->ExceptionClear();
        LOGE("could not find java.lang.reflect.Executable");
        return false;
    }

    g_art_method_field = env->GetFieldID(executable_class, "artMethod", "J");
    env->DeleteLocalRef(executable_class);

    if (g_art_method_field == nullptr) {
        // artMethod is a non-SDK field; this is what hidden-API enforcement looks like here.
        env->ExceptionClear();
        LOGE("could not find Executable.artMethod -- check hidden-API enforcement");
        return false;
    }
    return true;
}

/** Resolves one of the layout probes to its ArtMethod. */
ArtMethod *probe(JNIEnv *env, jclass arthooks_class, const char *name) {
    jmethodID id = env->GetStaticMethodID(arthooks_class, name, "()V");
    if (id == nullptr) {
        env->ExceptionClear();
        LOGE("layout probe %s is missing from com.arthooks.ArtHooks", name);
        return nullptr;
    }

    // Go through java.lang.reflect.Method rather than treating the jmethodID as an ArtMethod*, so
    // the measurement uses the same accessor as every other lookup in the library.
    jobject reflected = env->ToReflectedMethod(arthooks_class, id, JNI_TRUE);
    if (reflected == nullptr) {
        env->ExceptionClear();
        LOGE("could not reflect layout probe %s", name);
        return nullptr;
    }

    ArtMethod *art_method = get_art_method(env, reflected);
    env->DeleteLocalRef(reflected);
    return art_method;
}

/** Measures sizeof(art::ArtMethod) from the gap between two adjacent methods. */
bool measure_layout(JNIEnv *env, jclass arthooks_class) {
    ArtMethod *a = probe(env, arthooks_class, "layout_probe_a");
    ArtMethod *b = probe(env, arthooks_class, "layout_probe_b");
    if (a == nullptr || b == nullptr) {
        return false;
    }

    uintptr_t low = reinterpret_cast<uintptr_t>(a);
    uintptr_t high = reinterpret_cast<uintptr_t>(b);
    size_t size = (high > low) ? (high - low) : (low - high);

    // Two pointer-sized fields plus a handful of 32-bit ones; anything outside this means the
    // probes were not adjacent and the number is meaningless.
    const size_t min_size = 2 * sizeof(void *) + 16;
    if (size < min_size || size > 128 || size % sizeof(void *) != 0) {
        LOGE("measured implausible sizeof(ArtMethod) = %zu, refusing to hook", size);
        return false;
    }

    g_art_method_size = size;
    g_entry_point_offset = size - sizeof(void *);

    if (size != sizeof(ArtMethod)) {
        // Harmless by itself -- nothing indexes the mirror struct -- but it means art_method.hpp no
        // longer describes this platform, so fix it before trusting anything written against it.
        LOGW("sizeof(ArtMethod) is %zu here but the mirror in art_method.hpp is %zu; "
             "diff it against this release's art/runtime/art_method.h",
             size, sizeof(ArtMethod));
    }

    LOGI("ArtMethod: size=%zu data_ at +%zu, entry point at +%zu",
         size, size - 2 * sizeof(void *), g_entry_point_offset);
    return true;
}

/** Resolves a probe by name and shape, returning both its ArtMethod and its Java modifiers. */
bool flag_probe(JNIEnv *env, jclass arthooks_class, const char *name, bool is_static,
                ArtMethod **art_method_out, uint32_t *modifiers_out) {
    jmethodID id = is_static ? env->GetStaticMethodID(arthooks_class, name, "()V")
                             : env->GetMethodID(arthooks_class, name, "()V");
    if (id == nullptr) {
        env->ExceptionClear();
        LOGW("flag probe %s is missing from com.arthooks.ArtHooks", name);
        return false;
    }

    jobject reflected = env->ToReflectedMethod(arthooks_class, id, is_static ? JNI_TRUE : JNI_FALSE);
    if (reflected == nullptr) {
        env->ExceptionClear();
        LOGW("could not reflect flag probe %s", name);
        return false;
    }

    // Read the modifiers through reflection instead of hard-coding what the source says, so that a
    // compiler adding a synthetic bit cannot turn the comparison below into a silent miss.
    jclass executable = env->FindClass("java/lang/reflect/Executable");
    jmethodID get_modifiers =
            (executable == nullptr) ? nullptr : env->GetMethodID(executable, "getModifiers", "()I");
    if (executable != nullptr) {
        env->DeleteLocalRef(executable);
    }
    if (get_modifiers == nullptr) {
        env->ExceptionClear();
        env->DeleteLocalRef(reflected);
        LOGW("could not resolve Executable.getModifiers()");
        return false;
    }

    jint modifiers = env->CallIntMethod(reflected, get_modifiers);
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        env->DeleteLocalRef(reflected);
        return false;
    }

    *art_method_out = get_art_method(env, reflected);
    *modifiers_out = static_cast<uint32_t>(modifiers) & kJavaMethodModifiers;
    env->DeleteLocalRef(reflected);
    return *art_method_out != nullptr;
}

/**
 * Locates art::ArtMethod::access_flags_ by scanning for the one offset that holds each probe's own
 * modifiers.
 *
 * Same principle as the size measurement: derive it from the running platform rather than trust a
 * struct offset that is not ABI-stable. Two probes with deliberately disjoint modifiers (0x0a and
 * 0x11) are read at every aligned offset, and the offset is accepted only if exactly one matches
 * both -- so a dex index or method index that happens to equal one of them cannot be mistaken for
 * the field. An ambiguous or absent result leaves the mitigation off rather than guessing.
 */
void find_access_flags_offset(JNIEnv *env, jclass arthooks_class) {
    ArtMethod *a = nullptr;
    ArtMethod *b = nullptr;
    uint32_t modifiers_a = 0;
    uint32_t modifiers_b = 0;

    if (!flag_probe(env, arthooks_class, "flag_probe_a", true, &a, &modifiers_a)
        || !flag_probe(env, arthooks_class, "flag_probe_b", false, &b, &modifiers_b)) {
        LOGW("could not resolve the flag probes; hooking a hot method stays racy");
        return;
    }
    if (modifiers_a == modifiers_b) {
        // Would make every candidate offset ambiguous. Means the probes were edited to have the
        // same shape, which defeats the whole method.
        LOGW("flag probes have identical modifiers (0x%x); cannot locate access_flags_", modifiers_a);
        return;
    }

    size_t found = 0;
    size_t offset = 0;
    for (size_t candidate = 0; candidate + sizeof(uint32_t) <= g_art_method_size;
         candidate += sizeof(uint32_t)) {
        uint32_t word_a;
        uint32_t word_b;
        memcpy(&word_a, reinterpret_cast<const uint8_t *>(a) + candidate, sizeof(word_a));
        memcpy(&word_b, reinterpret_cast<const uint8_t *>(b) + candidate, sizeof(word_b));

        if ((word_a & kJavaMethodModifiers) == modifiers_a
            && (word_b & kJavaMethodModifiers) == modifiers_b) {
            found++;
            offset = candidate;
        }
    }

    if (found != 1) {
        LOGW("found %zu candidates for access_flags_ (wanted exactly 1); "
             "hooking a hot method stays racy", found);
        // Dump what was actually there, so a platform where this stops working can be diagnosed
        // from a logcat instead of a debugger.
        LOGW("expected modifiers 0x%x and 0x%x, masked with 0x%x", modifiers_a, modifiers_b,
             kJavaMethodModifiers);
        for (size_t candidate = 0; candidate + sizeof(uint32_t) <= g_art_method_size;
             candidate += sizeof(uint32_t)) {
            uint32_t word_a;
            uint32_t word_b;
            memcpy(&word_a, reinterpret_cast<const uint8_t *>(a) + candidate, sizeof(word_a));
            memcpy(&word_b, reinterpret_cast<const uint8_t *>(b) + candidate, sizeof(word_b));
            LOGW("  +%2zu: 0x%08x (masked 0x%04x) / 0x%08x (masked 0x%04x)", candidate,
                 word_a, word_a & kJavaMethodModifiers, word_b, word_b & kJavaMethodModifiers);
        }
        return;
    }

    g_access_flags_offset = offset;
    g_access_flags_found = true;

    if (offset != offsetof(ArtMethod, access_flags_)) {
        LOGW("access_flags_ is at +%zu here but the mirror in art_method.hpp says +%zu",
             offset, offsetof(ArtMethod, access_flags_));
    }
    LOGI("ArtMethod: access_flags_ at +%zu (probes 0x%x / 0x%x)", offset, modifiers_a, modifiers_b);
}

/**
 * Makes the page holding `address` writable.
 *
 * Defensive, not known to be required: on the platform this was tested against (Android 16, arm64)
 * boot-image ArtMethod pages are already writable and RuntimeCases' boot-classpath hook passes with
 * this call removed. It is kept because that mapping is not guaranteed, and a read-only one would
 * otherwise fault. The mapping is private, so a page that did need it just becomes copy-on-write
 * for this process; it is left writable afterwards because that is already the normal state for
 * every method ART installs entry points into.
 */
void make_page_writable(void *address) {
    static size_t page_size = 0;
    if (page_size == 0) {
        const long queried = sysconf(_SC_PAGESIZE);
        page_size = (queried > 0) ? static_cast<size_t>(queried) : 4096u;
    }

    uintptr_t start = reinterpret_cast<uintptr_t>(address) & ~static_cast<uintptr_t>(page_size - 1);
    // The slot is pointer-aligned but may still be the last field on the page.
    size_t length = (reinterpret_cast<uintptr_t>(address) + sizeof(void *)) - start;

    if (mprotect(reinterpret_cast<void *>(start), length, PROT_READ | PROT_WRITE) != 0) {
        // Not fatal: the page is usually already writable, and mprotect can fail on mappings that
        // never needed it. The store is what actually decides.
        LOGD("mprotect(%p) failed: %s", reinterpret_cast<void *>(start), strerror(errno));
    }
}

/** Address of the entry point slot inside an ArtMethod. */
void **entry_point_slot(const ArtMethod *art_method) {
    uint8_t *base = const_cast<uint8_t *>(reinterpret_cast<const uint8_t *>(art_method));
    return reinterpret_cast<void **>(base + g_entry_point_offset);
}

}  // namespace

bool init_art_method_access(JNIEnv *env, jclass arthooks_class, jint sdk_version) {
    g_sdk_version = sdk_version;

    if (g_sdk_version >= __ANDROID_API_R__ && !find_art_method_field(env)) {
        return false;
    }
    if (!measure_layout(env, arthooks_class)) {
        return false;
    }

    // Needs the measured size to bound the scan, so it runs last. Advisory: a failure here costs the
    // hot-method mitigation, not the library.
    find_access_flags_offset(env, arthooks_class);
    return true;
}

size_t art_method_size() {
    return g_art_method_size;
}

size_t entry_point_offset() {
    return g_entry_point_offset;
}

ArtMethod *get_art_method(JNIEnv *env, jobject executable) {
    if (executable == nullptr) {
        return nullptr;
    }

    ArtMethod *art_method;
    if (g_sdk_version >= __ANDROID_API_R__) {
        art_method = reinterpret_cast<ArtMethod *>(
                env->GetLongField(executable, g_art_method_field));
    } else {
        art_method = reinterpret_cast<ArtMethod *>(env->FromReflectedMethod(executable));
    }

    LOGD("ArtMethod: %p", art_method);
    return art_method;
}

void *get_entry_point(const ArtMethod *art_method) {
    return __atomic_load_n(entry_point_slot(art_method), __ATOMIC_ACQUIRE);
}

void set_entry_point(ArtMethod *art_method, void *entry_point) {
    void **slot = entry_point_slot(art_method);
    make_page_writable(slot);
    // Another thread can be dispatching through this method right now, and the trampoline it may
    // pick up has to be fully written before the pointer to it becomes visible.
    __atomic_store_n(slot, entry_point, __ATOMIC_RELEASE);
}

bool can_discourage_compilation() {
    return g_access_flags_found;
}

bool discourage_compilation(ArtMethod *art_method) {
    if (!g_access_flags_found || art_method == nullptr) {
        return false;
    }

    uint32_t *slot = reinterpret_cast<uint32_t *>(
            reinterpret_cast<uint8_t *>(art_method) + g_access_flags_offset);
    make_page_writable(slot);

    // access_flags_ is a std::atomic<uint32_t> in ART and the JIT reads it from its own thread, so
    // this is a read-modify-write rather than a plain store -- a load/or/store would race with the
    // runtime setting an unrelated bit.
    uint32_t before = __atomic_fetch_or(slot, kAccCompileDontBother, __ATOMIC_SEQ_CST);

    uint32_t after = __atomic_load_n(slot, __ATOMIC_SEQ_CST);
    if ((after & kAccCompileDontBother) == 0) {
        // A read-only mapping is the likely cause; the hook still works, just racily.
        LOGW("could not set kAccCompileDontBother on %p (flags 0x%x)", art_method, after);
        return false;
    }

    if ((before & kAccCompileDontBother) == 0) {
        LOGD("kAccCompileDontBother set on %p (flags 0x%x -> 0x%x)", art_method, before, after);
    }
    return true;
}
