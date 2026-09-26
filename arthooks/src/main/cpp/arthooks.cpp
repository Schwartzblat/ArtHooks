#include "arthooks.hpp"

#include "art_method.hpp"
#include "class_init.hpp"
#include "deoptimize.hpp"
#include "log.hpp"
#include "trampoline.hpp"

namespace {

bool g_initialized = false;
jmethodID g_get_modifiers = nullptr;

// java.lang.reflect.Modifier.SYNCHRONIZED. On a method this is ACC_SYNCHRONIZED.
constexpr jint kAccSynchronized = 0x0020;

// java.lang.reflect.Modifier.NATIVE and .STATIC.
constexpr jint kAccNative = 0x0100;
constexpr jint kAccStatic = 0x0008;

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

/** Reads an Executable's Java modifiers. Returns false when they could not be read. */
bool modifiers_of(JNIEnv *env, jobject executable, jint *modifiers_out) {
    if (g_get_modifiers == nullptr) {
        return false;
    }
    jint modifiers = env->CallIntMethod(executable, g_get_modifiers);
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        return false;
    }
    *modifiers_out = modifiers;
    return true;
}

/**
 * Refuses a backup whose shape does not match its target, because the mismatch fails silently.
 *
 * Installing a backup only rewrites its entry point, which assumes every call to it goes through
 * that field. Whether that holds depends on the *target*, and the two cases want opposite things.
 *
 * For an instance method or a constructor the backup has to be `native`. A backup with a Java body
 * is small and returns nothing interesting, so the compiler inlines it into the replacement and the
 * call site ends up holding a copy of the backup's own body instead of a call. dex2oat does that at
 * install time -- before any of this code has run, so nothing here can undo it -- and the JIT does
 * it again later. The replacement then gets the backup's own do-nothing answer, the original body
 * never runs, and nothing reports an error: the hook says it succeeded and quietly returns the
 * wrong value from then on. A native method has no body to copy, so the call has to go through the
 * entry point. Measured on Android 16 / API 36: the same hook passes under `compile -m verify` and
 * fails under `compile -m speed`, which is why an app can work when it is installed and start
 * misbehaving hours later once background dexopt has compiled it.
 *
 * A static target is a separate, unfixed defect, and the backup's shape has nothing to do with it:
 * backing one up recurses into the replacement until the stack runs out, with or without `native`.
 * The cause is ART's. Instrumentation::GetInitialEntrypoint() reads, for a method where
 * NeedsClinitCheckBeforeCall() holds -- which is exactly IsStatic() && !IsConstructor():
 *
 *     return (aot_code != nullptr || IsNative(flags)) ? GetQuickResolutionStub()
 *                                                     : GetQuickToInterpreterBridge();
 *
 * so a static method whose oat file carries code for it starts on the *resolution stub*, and keeps
 * it until ClassLinker::FixupStaticTrampolines() runs -- which waits for the declaring class to
 * become *visibly* initialized, a transition arm64 batches behind a VisiblyInitializedCallback.
 * install_backup() captures that stub, and calling the backup re-enters it, where it re-reads
 * entry_point_from_quick_compiled_code_ -- by then the hook -- and lands in the replacement.
 *
 * Note the condition is `aot_code != nullptr`, not CanUseAotCode(): disable_aot_code() does not
 * affect this branch. What decides it is whether dex2oat compiled the method at all, which is why
 * this reproduces under `compile -m speed` and not under `-m verify`, and why it looked like a
 * debug-versus-release difference -- a debuggable APK is compiled `verify`.
 *
 * Repairing it needs ClassLinker::MakeInitializedClassesVisiblyInitialized(), and reaching that
 * means finding Runtime::GetClassLinker()'s result, an inline accessor over a struct offset this
 * library has no way to measure. So: warn, install anyway, and let the caller decide. See
 * ArityCases in the self-test.
 */
bool reject_mismatched_backup(JNIEnv *env, jobject original, jobject backup) {
    jint target_modifiers = 0;
    jint backup_modifiers = 0;
    if (!modifiers_of(env, original, &target_modifiers)
        || !modifiers_of(env, backup, &backup_modifiers)) {
        // Advisory lookup. Refusing every backup because the shape could not be read would be worse
        // than the risk of installing a mismatched one.
        LOGW("cannot read the target's or backup's modifiers; the backup shape is unchecked");
        return false;
    }

    const bool target_is_static = (target_modifiers & kAccStatic) != 0;
    const bool backup_is_native = (backup_modifiers & kAccNative) != 0;

    if (!target_is_static && !backup_is_native) {
        LOGE("the backup for an instance method or constructor must be declared native -- one with "
             "a Java body gets inlined into the replacement by dex2oat, which makes the entry point "
             "swap invisible and leaves the backup silently returning its own answer. Declare it "
             "`static native` with no body, keeping the replacement's signature.");
        return true;
    }
    if (target_is_static) {
        LOGW("backing up a static target is unreliable, whether or not the backup is native: while "
             "the target's class is not yet *visibly* initialized ART leaves an AOT-compiled static "
             "method on the resolution stub, and a backup built on that stub re-dispatches through "
             "the hook and recurses until the stack overflows. It is safe when dex2oat did not "
             "compile the target (a `verify` build). Installing it anyway -- nothing here can force "
             "that class transition.");
    }
    return false;
}

/**
 * Points `backup` at `target`'s original body, by capturing its entry point before the hook
 * overwrites it and baking that address into the backup's own trampoline.
 *
 * Must be called before the target is redirected, or the captured address is the hook itself and
 * the backup becomes an infinite loop.
 *
 * The trampoline names the real `target`, not a copy of it. Copying is the obvious implementation
 * -- a private ArtMethod whose entry point still refers to the original code -- but an ArtMethod
 * the runtime does not own is a liability: declaring_class_ is a GcRoot that ART rewrites in every
 * real ArtMethod when the compacting GC relocates the class, and a detached copy never gets that
 * fixup. nterp walks declaring_class_ -> dex cache on every invocation, so the first call after a
 * relocating GC reads a dead class and crashes. Measured on Android 16 / API 36: five backup calls
 * succeeded, a 76MB concurrent mark-compact GC ran, and the next call took SIGSEGV.
 */
bool install_backup(ArtMethod *backup, ArtMethod *target) {
    void *original_entry = get_entry_point(target);
    if (original_entry == nullptr) {
        LOGE("ArtMethod %p has no entry point to back up", target);
        return false;
    }

    void *trampoline = make_direct_trampoline(target, original_entry);
    if (trampoline == nullptr) {
        return false;
    }

    set_entry_point(backup, trampoline);
    LOGD("backup ArtMethod %p now runs the body of %p (entry %p)", backup, target, original_entry);
    return true;
}

/**
 * Warns when the target is running ahead-of-time compiled code.
 *
 * That means the class kept the body dex2oat produced for it -- and dex2oat inlines. Any caller it
 * compiled may hold a copy of this method rather than a call through the entry point, and no call
 * site like that can be redirected by anything written here. The hook still installs and still
 * fires for every caller that does load the entry point, so this is a warning rather than a
 * refusal, but it is the one case where a hook reports success and does nothing.
 *
 * Two ways to be here: disable_aot_code() never ran (libart did not export what it needs, or
 * ArtHooks.KEEP_AOT_PROPERTY asked it not to), or it ran too late -- ART had already linked this
 * class, and it only governs classes linked afterwards.
 */
void warn_if_aot_compiled(ArtMethod *target) {
    if (!is_aot_code(get_entry_point(target))) {
        return;
    }
    if (aot_code_disabled()) {
        LOGW("the target is on AOT code because ART linked its class before ArtHooks loaded -- a "
             "caller dex2oat inlined it into will keep running the original body. Touch "
             "com.arthooks.ArtHooks earlier (Application.attachBaseContext is the usual place).");
    } else {
        LOGW("the target is on AOT code and ArtHooks did not stop ART using it -- a caller dex2oat "
             "inlined it into will keep running the original body. See ArtHooks.disable_aot().");
    }
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

    // Before any state is touched: a backup that can be inlined cannot be installed at all, and
    // failing here leaves the target running its own body rather than half-hooked.
    if (backup != nullptr && reject_mismatched_backup(env, original, backup)) {
        return false;
    }

    ArtMethod *target = get_art_method(env, original);
    ArtMethod *hook = get_art_method(env, replacement);
    ArtMethod *backup_method = get_art_method(env, backup);
    if (target == nullptr || hook == nullptr || (backup != nullptr && backup_method == nullptr)) {
        LOGE("could not resolve an ArtMethod");
        return false;
    }

    warn_if_aot_compiled(target);

    // Before anything is written: if the target is currently hot, the JIT may already have queued it
    // for compilation, and a compile that lands after the trampoline is installed would overwrite the
    // entry point and silently drop the hook. Telling ART not to compile it closes that off. Ordering
    // matters -- doing this after the entry point write would leave the window open in between.
    //
    // It does one more thing, which matters more than the race: AOSP's HInliner refuses to inline a
    // method that is not compilable, so this is also what stops the JIT inlining the target's body
    // into a hot caller later and quietly stepping around the hook.
    if (!discourage_compilation(target) && can_discourage_compilation()) {
        LOGW("could not stop ART compiling %p; hooking it while it is hot may lose the hook", target);
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
JNIEXPORT jboolean JNICALL
Java_com_arthooks_ArtHooks_disable_1aot(JNIEnv *env, jclass clazz) {
    return disable_aot_code() ? JNI_TRUE : JNI_FALSE;
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
