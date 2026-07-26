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
    return measure_layout(env, arthooks_class);
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
