#include "arthooks.hpp"

#include <cstdlib>
#include <cstring>

#include "art_method.hpp"
#include "class_init.hpp"
#include "log.hpp"
#include "trampoline.hpp"

namespace {

bool g_initialized = false;
jmethodID g_get_modifiers = nullptr;

// java.lang.reflect.Modifier.SYNCHRONIZED. On a method this is ACC_SYNCHRONIZED.
constexpr jint kAccSynchronized = 0x0020;

bool find_get_modifiers(JNIEnv *env) {
    jclass executable_class = env->FindClass("java/lang/reflect/Executable");
    if (executable_class == nullptr) {
        env->ExceptionClear();
        return false;
    }
    g_get_modifiers = env->GetMethodID(executable_class, "getModifiers", "()I");
    env->DeleteLocalRef(executable_class);
    if (g_get_modifiers == nullptr) {
        env->ExceptionClear();
        return false;
    }
    return true;
}

/**
 * Warns when the target is synchronized, because the hook cannot preserve that.
 *
 * A synchronized method has no monitor-enter in its body -- the lock is taken by the callee's own
 * entry sequence, driven by ACC_SYNCHRONIZED on the method being entered. The hook redirects before
 * any of that runs and lands in a replacement that does not carry the flag, so the monitor is never
 * acquired and the caller cannot tell. Nothing here can fix it: the caller does not participate in
 * the locking, so there is no argument or register to fix up.
 */
void warn_if_synchronized(JNIEnv *env, jobject original) {
    if (g_get_modifiers == nullptr) {
        return;
    }
    jint modifiers = env->CallIntMethod(original, g_get_modifiers);
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        return;
    }
    if ((modifiers & kAccSynchronized) != 0) {
        LOGW("the target is synchronized, but the replacement will NOT hold its monitor -- "
             "synchronize the replacement yourself on the receiver, or on the declaring class if "
             "the target is static");
    }
}

/**
 * Points `backup` at a snapshot of `target` taken before it is hooked, so calling the backup runs
 * the original body.
 *
 * The snapshot is a private copy of the ArtMethod rather than the real one: the trampoline has to
 * name a method whose entry point still points at the original code, and `target`'s is about to
 * stop doing that. Reading the entry point from the copy also stops ART from ever routing the
 * backup back through the hook.
 */
bool install_backup(ArtMethod *backup, const ArtMethod *target) {
    ArtMethod *snapshot = static_cast<ArtMethod *>(malloc(art_method_size()));
    if (snapshot == nullptr) {
        LOGE("out of memory while backing up ArtMethod %p", target);
        return false;
    }
    memcpy(snapshot, target, art_method_size());

    void *trampoline = make_trampoline(snapshot);
    if (trampoline == nullptr) {
        free(snapshot);
        return false;
    }

    set_entry_point(backup, trampoline);
    LOGD("backup ArtMethod %p now runs the body of %p", backup, target);
    return true;
}

/**
 * Resolves a method by JNI descriptor to the Method or Constructor object that names it.
 *
 * The descriptor alone does not say whether the caller meant an instance or a static method, so
 * both lookups are tried. GetMethodID also covers <init>, which ToReflectedMethod hands back as a
 * java.lang.reflect.Constructor.
 */
jobject find_executable(JNIEnv *env, jclass owner, const char *name, const char *signature) {
    jboolean is_static = JNI_FALSE;
    jmethodID id = env->GetMethodID(owner, name, signature);
    if (id == nullptr) {
        env->ExceptionClear();
        id = env->GetStaticMethodID(owner, name, signature);
        is_static = JNI_TRUE;
    }
    if (id == nullptr) {
        env->ExceptionClear();
        LOGE("no method matching %s%s", name, signature);
        return nullptr;
    }

    jobject found = env->ToReflectedMethod(owner, id, is_static);
    if (found == nullptr) {
        env->ExceptionClear();
        LOGE("could not reflect %s%s", name, signature);
    }
    return found;
}

}  // namespace

bool hook_function(JNIEnv *env, jobject original, jobject replacement, jobject backup) {
    if (!g_initialized) {
        LOGE("ArtHooks failed to initialise; refusing to hook");
        return false;
    }
    if (original == nullptr || replacement == nullptr) {
        LOGE("hook_function() needs a non-null original and replacement");
        return false;
    }
    if (!ensure_class_initialized(env, original) || !ensure_class_initialized(env, replacement)) {
        return false;
    }
    warn_if_synchronized(env, original);

    ArtMethod *target = get_art_method(env, original);
    ArtMethod *hook = get_art_method(env, replacement);
    ArtMethod *backup_method = get_art_method(env, backup);
    if (target == nullptr || hook == nullptr || (backup != nullptr && backup_method == nullptr)) {
        LOGE("could not resolve an ArtMethod");
        return false;
    }

    // The backup goes in first: once the target is redirected, the replacement can be entered on
    // another thread and call through immediately.
    if (backup_method != nullptr && !install_backup(backup_method, target)) {
        return false;
    }

    void *trampoline = make_trampoline(hook);
    if (trampoline == nullptr) {
        return false;
    }
    set_entry_point(target, trampoline);

    LOGI("hooked ArtMethod %p with %p", target, hook);
    return true;
}

extern "C"
JNIEXPORT jboolean JNICALL
Java_com_arthooks_ArtHooks_init(JNIEnv *env, jclass clazz, jint sdk_version) {
    g_initialized = init_art_method_access(env, clazz, sdk_version) && init_class_initializer(env);
    // Advisory only, so a failure here does not stop the library coming up.
    find_get_modifiers(env);
    if (g_initialized) {
        LOGI("ArtHooks initialised on API %d", sdk_version);
    }
    return g_initialized ? JNI_TRUE : JNI_FALSE;
}

extern "C"
JNIEXPORT jobject JNICALL
Java_com_arthooks_ArtHooks_find_1function(JNIEnv *env, jclass clazz, jclass owner, jstring name,
                                          jstring signature) {
    if (owner == nullptr || name == nullptr || signature == nullptr) {
        LOGE("find_function() needs a non-null owner, name and signature");
        return nullptr;
    }

    const char *name_chars = env->GetStringUTFChars(name, nullptr);
    const char *signature_chars = env->GetStringUTFChars(signature, nullptr);

    jobject found = nullptr;
    if (name_chars != nullptr && signature_chars != nullptr) {
        found = find_executable(env, owner, name_chars, signature_chars);
    }

    if (name_chars != nullptr) {
        env->ReleaseStringUTFChars(name, name_chars);
    }
    if (signature_chars != nullptr) {
        env->ReleaseStringUTFChars(signature, signature_chars);
    }
    return found;
}

extern "C"
JNIEXPORT jboolean JNICALL
Java_com_arthooks_ArtHooks_hook_1function__Ljava_lang_reflect_Executable_2Ljava_lang_reflect_Executable_2(
        JNIEnv *env, jclass clazz, jobject original, jobject replacement) {
    return hook_function(env, original, replacement, nullptr) ? JNI_TRUE : JNI_FALSE;
}

extern "C"
JNIEXPORT jboolean JNICALL
Java_com_arthooks_ArtHooks_hook_1function__Ljava_lang_reflect_Executable_2Ljava_lang_reflect_Executable_2Ljava_lang_reflect_Executable_2(
        JNIEnv *env, jclass clazz, jobject original, jobject replacement, jobject backup) {
    return hook_function(env, original, replacement, backup) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jint JNICALL
JNI_OnLoad(JavaVM *vm, void *reserved) {
    LOGD("ArtHooks loaded!");
    return JNI_VERSION_1_6;
}
