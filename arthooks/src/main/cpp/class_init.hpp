//
// Created by alon on 7/27/26.
//

#ifndef ARTHOOKS_CLASS_INIT_HPP
#define ARTHOOKS_CLASS_INIT_HPP

#include <jni.h>

/** Caches the reflection entry points ensure_class_initialized() needs. */
bool init_class_initializer(JNIEnv *env);

/**
 * Runs the declaring class's initialiser if it has not run yet.
 *
 * Getting a Method or Constructor by reflection does not initialise its class, and ART rewrites the
 * entry point of every method of a class when it finally does run the initialiser -- which would
 * silently drop a hook installed before that point.
 */
bool ensure_class_initialized(JNIEnv *env, jobject executable);

#endif //ARTHOOKS_CLASS_INIT_HPP
