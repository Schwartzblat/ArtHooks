//
// Created by alon on 6/29/26.
//

#ifndef ARTHOOKS_ARTHOOKS_HPP
#define ARTHOOKS_ARTHOOKS_HPP

#include <jni.h>

/**
 * Redirects `original` to `replacement`, both java.lang.reflect.Executable -- so either may be a
 * Method or a Constructor.
 *
 * `backup` is optional. When given, it is wired up to run `original`'s pre-hook body so the
 * replacement can call through; it keeps its own Java identity, only its entry point changes.
 *
 * `replacement` (and `backup`) must be static and take the receiver as an explicit leading
 * parameter, because the hook redirects the call without touching the arguments already in place.
 */
bool hook_function(JNIEnv *env, jobject original, jobject replacement, jobject backup);

#endif //ARTHOOKS_ARTHOOKS_HPP
