//
// Created by alon on 7/27/26.
//

#include "class_init.hpp"
#include "log.hpp"

namespace {

jclass g_class_class = nullptr;
jmethodID g_for_name = nullptr;
jmethodID g_get_name = nullptr;
jmethodID g_get_class_loader = nullptr;
jmethodID g_get_declaring_class = nullptr;

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
    g_get_declaring_class = env->GetMethodID(executable_class, "getDeclaringClass",
                                             "()Ljava/lang/Class;");

    env->DeleteLocalRef(class_class);
    env->DeleteLocalRef(executable_class);

    if (g_class_class == nullptr || g_for_name == nullptr || g_get_name == nullptr ||
        g_get_class_loader == nullptr || g_get_declaring_class == nullptr) {
        env->ExceptionClear();
        LOGE("could not resolve the java.lang.Class members used to initialise classes");
        return false;
    }
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
