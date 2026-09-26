//
// Created by alon on 7/27/26.
//

#ifndef ARTHOOKS_CLASS_INIT_HPP
#define ARTHOOKS_CLASS_INIT_HPP

#include <jni.h>

#include "art_method.hpp"

/**
 * Caches the reflection entry points ensure_class_initialized() needs, and measures the quick
 * resolution stub's address (see ensure_class_visibly_initialized).
 */
bool init_class_initializer(JNIEnv *env);

/**
 * Runs the declaring class's initialiser if it has not run yet.
 *
 * Getting a Method or Constructor by reflection does not initialise its class, and ART rewrites the
 * entry point of every method of a class when it finally does run the initialiser -- which would
 * silently drop a hook installed before that point.
 */
bool ensure_class_initialized(JNIEnv *env, jobject executable);

/**
 * Waits for ART to make a static method's declaring class *visibly* initialized, which is a
 * different thing from initialized and is the state its entry point depends on.
 *
 * Only static, non-constructor methods care. Instrumentation::GetInitialEntrypoint() gives such a
 * method the quick resolution stub while it is AOT-compiled or native, and leaves it there until
 * ClassLinker::FixupStaticTrampolines() runs -- which waits for the class to become visibly
 * initialized, a transition arm64 batches behind a VisiblyInitializedCallback. install_backup()
 * capturing the stub is what makes a static backup recurse: calling it re-reads the entry point,
 * finds the hook, and lands back in the replacement until the stack overflows.
 *
 * The stub is detected by measuring its address at init from a probe method that sits on it by
 * construction (ArtHooks.ResolutionStubProbe.stub_probe). A target whose entry point is not that
 * stub is already past the fixup and returns true at once. A target on the stub is nudged with
 * Class.forName(name, true, loader) -- which trips ClassLinker::EnsureInitialized's per-thread
 * counter and makes ART request the transition -- until its entry point leaves the stub, bounded.
 *
 * Returns true when the class is believed visibly initialized (including when it never needed to
 * be), false when a static target was still on the stub after the budget or a <clinit> threw. The
 * caller must refuse a hook when this returns false: the stub means FixupStaticTrampolines has not
 * run, and it rewrites the entry point unconditionally when it does, so the hook could not survive.
 */
bool ensure_class_visibly_initialized(JNIEnv *env, jobject executable, ArtMethod *method);

#endif //ARTHOOKS_CLASS_INIT_HPP
