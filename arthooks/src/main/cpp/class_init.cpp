//
// Created by alon on 7/27/26.
//

#include "class_init.hpp"

#include "deoptimize.hpp"
#include "log.hpp"

namespace {

jclass g_class_class = nullptr;
jmethodID g_for_name = nullptr;
jmethodID g_get_name = nullptr;
jmethodID g_get_class_loader = nullptr;
jmethodID g_get_declaring_class = nullptr;
jmethodID g_get_declared_method = nullptr;
jmethodID g_get_modifiers = nullptr;

// The quick resolution stub's address, measured at init from a probe that sits on it. Only trusted
// when g_stub_valid; otherwise ensure_class_visibly_initialized() falls back to watching the
// target's own entry point move.
void *g_resolution_stub = nullptr;
bool g_stub_valid = false;

// java.lang.reflect.Modifier.STATIC.
constexpr jint kAccStatic = 0x0008;

// How many Class.forName() nudges to spend forcing the visible-initialization transition before
// giving up. Task 1 measured the per-thread flush counter tripping well inside this.
constexpr int kMaxNudges = 4096;

/** Logs and swallows a pending exception, if any. Returns whether one was pending. */
bool clear_exception(JNIEnv *env, const char *what) {
    if (!env->ExceptionCheck()) {
        return false;
    }
    env->ExceptionDescribe();
    env->ExceptionClear();
    LOGE("%s", what);
    return true;
}

/**
 * Measures the quick resolution stub's address, the "measure, don't assume" move already used for
 * sizeof(ArtMethod) and access_flags_.
 *
 * ArtHooks.ResolutionStubProbe.stub_probe is a static native in a class that is loaded but never
 * initialized, so Instrumentation::GetInitialEntrypoint parks it on the resolution stub by
 * construction. The method is reached with getDeclaredMethod (which does not initialize the class),
 * never GetStaticMethodID (which does). The result is validated -- non-null, inside libart.so, and
 * different from a resolved static native's entry -- before it is trusted.
 *
 * The resolved native for that last check is java.lang.System.nanoTime, not one of ArtHooks' own:
 * ArtHooks is still inside its own <clinit> here, so it is not visibly initialized and every one of
 * its static natives (init included) is itself still on the resolution stub. System is initialized
 * at boot, so nanoTime is off the stub -- which is exactly the value the probe must differ from.
 */
void measure_resolution_stub(JNIEnv *env) {
    jclass probe_class = env->FindClass("com/arthooks/ArtHooks$ResolutionStubProbe");
    if (probe_class == nullptr) {
        env->ExceptionClear();
        LOGW("could not find ResolutionStubProbe; static-target settling will use the fallback");
        return;
    }

    jstring method_name = env->NewStringUTF("stub_probe");
    jobjectArray no_args = env->NewObjectArray(0, g_class_class, nullptr);
    jobject probe_method = env->CallObjectMethod(probe_class, g_get_declared_method, method_name,
                                                 no_args);
    env->DeleteLocalRef(method_name);
    env->DeleteLocalRef(no_args);
    env->DeleteLocalRef(probe_class);
    if (clear_exception(env, "could not reflect ResolutionStubProbe.stub_probe")
        || probe_method == nullptr) {
        return;
    }

    ArtMethod *probe = get_art_method(env, probe_method);
    env->DeleteLocalRef(probe_method);
    if (probe == nullptr) {
        LOGW("could not resolve the ArtMethod for the resolution-stub probe");
        return;
    }
    void *stub = get_entry_point(probe);

    // A static native in a *visibly* initialized class has a real JNI entry rather than the stub.
    // System is initialized at boot, so System.nanoTime is off the stub.
    void *resolved_native_entry = nullptr;
    jclass system_class = env->FindClass("java/lang/System");
    if (system_class != nullptr) {
        jmethodID nano_time = env->GetStaticMethodID(system_class, "nanoTime", "()J");
        if (nano_time != nullptr) {
            jobject nano_time_method = env->ToReflectedMethod(system_class, nano_time, JNI_TRUE);
            if (nano_time_method != nullptr) {
                ArtMethod *nano_time_art_method = get_art_method(env, nano_time_method);
                env->DeleteLocalRef(nano_time_method);
                if (nano_time_art_method != nullptr) {
                    resolved_native_entry = get_entry_point(nano_time_art_method);
                }
            }
        }
        env->DeleteLocalRef(system_class);
    }
    env->ExceptionClear();

    if (stub == nullptr) {
        LOGW("resolution-stub probe has a null entry point; static-target settling will use the "
             "fallback");
        return;
    }
    if (!is_in_libart(stub)) {
        LOGW("resolution-stub probe entry %p is not inside libart.so; static-target settling will "
             "use the fallback", stub);
        return;
    }
    if (resolved_native_entry != nullptr && stub == resolved_native_entry) {
        LOGW("resolution-stub probe entry %p equals a resolved native's entry -- the probe class was "
             "initialized; static-target settling will use the fallback", stub);
        return;
    }

    g_resolution_stub = stub;
    g_stub_valid = true;
    LOGI("measured quick resolution stub at %p (resolved native entry %p)", stub,
         resolved_native_entry);
}

/**
 * Nudges a static target off the resolution stub, waiting for FixupStaticTrampolines to run.
 *
 * Class.forName(name, true, loader) reaches ClassLinker::EnsureInitialized, which bumps a per-thread
 * counter for an initialized-but-not-visible class and asks ART to flush the batch once it trips.
 * The flush is asynchronous, so the entry point is watched until it leaves the stub. The forName
 * result is a Class local ref discarded every iteration, or 4096 of them would overflow the table.
 */
bool settle_off_the_stub(JNIEnv *env, ArtMethod *method, jstring name, jobject loader,
                         const char *label) {
    if (get_entry_point(method) != g_resolution_stub) {
        LOGD("static target %s is not on the resolution stub; no settling needed", label);
        return true;
    }

    int nudges = 0;
    bool left = false;
    while (nudges < kMaxNudges && !left) {
        jobject resolved = env->CallStaticObjectMethod(g_class_class, g_for_name, name, JNI_TRUE,
                                                       loader);
        if (clear_exception(env, "class initialisation failed while forcing visible initialisation")) {
            return false;
        }
        if (resolved != nullptr) {
            env->DeleteLocalRef(resolved);
        }
        nudges++;
        left = get_entry_point(method) != g_resolution_stub;
    }

    if (left) {
        LOGD("static target %s was on the resolution stub; left it after %d nudges", label, nudges);
    } else {
        LOGW("static target %s is still on the resolution stub after %d nudges; refusing to hook it "
             "-- FixupStaticTrampolines has not run for its class, so a backup would capture the stub "
             "and recurse, and the hook would sit under a fixup that has not happened yet",
             label, kMaxNudges);
    }
    return left;
}

/**
 * Fallback for when the stub could not be measured: watch the target's own entry point move.
 *
 * Without a stub address there is no way to tell an already-settled class (entry never changes)
 * from one stuck on the stub. The common case by far is the former -- most static targets belong to
 * classes that were visibly initialized long ago -- so "no change" is treated as already settled and
 * the hook proceeds, with a warning, rather than refusing every such hook. The cost is the rare
 * stuck-on-the-stub case: a backup installed on it may recurse. Only a throwing <clinit> returns
 * false. This path is only taken on a platform where the probe failed validation.
 */
bool settle_by_watching_entry(JNIEnv *env, ArtMethod *method, jstring name, jobject loader,
                              const char *label) {
    const void *before = get_entry_point(method);
    int nudges = 0;
    bool changed = false;
    while (nudges < kMaxNudges && !changed) {
        jobject resolved = env->CallStaticObjectMethod(g_class_class, g_for_name, name, JNI_TRUE,
                                                       loader);
        if (clear_exception(env, "class initialisation failed while forcing visible initialisation")) {
            return false;
        }
        if (resolved != nullptr) {
            env->DeleteLocalRef(resolved);
        }
        nudges++;
        changed = get_entry_point(method) != before;
    }

    if (changed) {
        LOGD("(stub probe unavailable) static target %s entry moved after %d nudges", label, nudges);
    } else {
        LOGW("(stub probe unavailable) static target %s entry did not move after %d nudges; assuming "
             "its class is already visibly initialized and hooking it -- if it is in fact still on "
             "the resolution stub, a backup installed on it may recurse", label, kMaxNudges);
    }
    return true;
}

}  // namespace

bool init_class_initializer(JNIEnv *env) {
    jclass class_class = env->FindClass("java/lang/Class");
    // Executable, not Method: getDeclaringClass() has to work for a Constructor too.
    jclass executable_class = env->FindClass("java/lang/reflect/Executable");
    if (class_class == nullptr || executable_class == nullptr) {
        env->ExceptionClear();
        LOGE("could not find java.lang.Class / java.lang.reflect.Executable");
        return false;
    }

    g_class_class = static_cast<jclass>(env->NewGlobalRef(class_class));
    g_for_name = env->GetStaticMethodID(
            class_class, "forName",
            "(Ljava/lang/String;ZLjava/lang/ClassLoader;)Ljava/lang/Class;");
    g_get_name = env->GetMethodID(class_class, "getName", "()Ljava/lang/String;");
    g_get_class_loader = env->GetMethodID(class_class, "getClassLoader",
                                          "()Ljava/lang/ClassLoader;");
    g_get_declared_method = env->GetMethodID(
            class_class, "getDeclaredMethod",
            "(Ljava/lang/String;[Ljava/lang/Class;)Ljava/lang/reflect/Method;");
    g_get_declaring_class = env->GetMethodID(executable_class, "getDeclaringClass",
                                             "()Ljava/lang/Class;");
    g_get_modifiers = env->GetMethodID(executable_class, "getModifiers", "()I");

    env->DeleteLocalRef(class_class);
    env->DeleteLocalRef(executable_class);

    if (g_class_class == nullptr || g_for_name == nullptr || g_get_name == nullptr ||
        g_get_class_loader == nullptr || g_get_declared_method == nullptr ||
        g_get_declaring_class == nullptr || g_get_modifiers == nullptr) {
        env->ExceptionClear();
        LOGE("could not resolve the java.lang.Class members used to initialise classes");
        return false;
    }

    // Advisory: a failure here only forces ensure_class_visibly_initialized()'s fallback path.
    measure_resolution_stub(env);
    return true;
}

bool ensure_class_initialized(JNIEnv *env, jobject executable) {
    jobject declaring_class = env->CallObjectMethod(executable, g_get_declaring_class);
    if (clear_exception(env, "could not get a method's declaring class") ||
        declaring_class == nullptr) {
        return false;
    }

    jstring name = static_cast<jstring>(env->CallObjectMethod(declaring_class, g_get_name));
    jobject loader = env->CallObjectMethod(declaring_class, g_get_class_loader);
    env->DeleteLocalRef(declaring_class);

    bool initialized = !clear_exception(env, "could not describe a method's declaring class");
    if (initialized) {
        // Class.forName(name, true, loader) is the only way to force <clinit> from here; FindClass
        // and GetStaticMethodID deliberately do not.
        env->CallStaticObjectMethod(g_class_class, g_for_name, name, JNI_TRUE, loader);
        initialized = !clear_exception(env, "class initialisation failed for a method being hooked");
    }

    env->DeleteLocalRef(name);
    env->DeleteLocalRef(loader);
    return initialized;
}

bool ensure_class_visibly_initialized(JNIEnv *env, jobject executable, ArtMethod *method) {
    if (method == nullptr) {
        return false;
    }

    jint modifiers = env->CallIntMethod(executable, g_get_modifiers);
    if (clear_exception(env, "could not read a method's modifiers")) {
        return false;
    }
    if ((modifiers & kAccStatic) == 0) {
        // NeedsClinitCheckBeforeCall() is IsStatic() && !IsConstructor(); an instance method or a
        // constructor is never parked on the resolution stub, so there is nothing to settle.
        return true;
    }

    jobject declaring_class = env->CallObjectMethod(executable, g_get_declaring_class);
    if (clear_exception(env, "could not get a static method's declaring class")
        || declaring_class == nullptr) {
        return false;
    }
    jstring name = static_cast<jstring>(env->CallObjectMethod(declaring_class, g_get_name));
    jobject loader = env->CallObjectMethod(declaring_class, g_get_class_loader);
    env->DeleteLocalRef(declaring_class);
    if (clear_exception(env, "could not describe a static method's declaring class")) {
        return false;
    }

    const char *class_name = env->GetStringUTFChars(name, nullptr);
    if (class_name == nullptr) {
        // GetStringUTFChars only returns null after throwing OutOfMemoryError. Leaving that
        // pending and nudging anyway would just throw again on the first Class.forName call, so
        // give up now instead.
        clear_exception(env, "could not read a static method's declaring class name");
        env->DeleteLocalRef(name);
        env->DeleteLocalRef(loader);
        return false;
    }

    bool settled = g_stub_valid ? settle_off_the_stub(env, method, name, loader, class_name)
                                : settle_by_watching_entry(env, method, name, loader, class_name);

    env->ReleaseStringUTFChars(name, class_name);
    env->DeleteLocalRef(name);
    env->DeleteLocalRef(loader);
    return settled;
}
