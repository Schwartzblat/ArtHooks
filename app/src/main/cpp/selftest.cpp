//
// Created by alon on 7/27/26.
//
// Native side of the demo's self-test. Not part of the hooking library; it only exists to give
// DispatchCases a real JNI method to hook.
//

#include <jni.h>

extern "C"
JNIEXPORT jint JNICALL
Java_com_example_arthooks_DispatchCases_native_1method(JNIEnv *env, jclass clazz, jint value) {
    return value * 3;
}
