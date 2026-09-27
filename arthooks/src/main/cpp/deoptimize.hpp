//
// Created by alon on 9/26/26.
//

#ifndef ARTHOOKS_DEOPTIMIZE_HPP
#define ARTHOOKS_DEOPTIMIZE_HPP

/**
 * Stops ART running ahead-of-time compiled code, so that calls reach the entry point a hook lives
 * in instead of bypassing it.
 *
 * This is the answer to the one failure mode entry-point hooking cannot otherwise survive. dex2oat
 * *inlines*: under a `speed` or `speed-profile` filter it copies a small method's body into every
 * caller it compiles, and a caller holding a copy never loads the callee's entry point at all. The
 * hook installs, reports success, and silently never fires. That happened at install time, before
 * any of this library ran, so nothing here can undo it after the fact. (AOSP's HInliner refuses to
 * inline a method that is not compilable, which is why kAccCompileDontBother on the target already
 * covers the *JIT*. It cannot cover what dex2oat emitted before the process started.)
 *
 * What can be done is to make that compiled code unreachable. ART asks
 * `Instrumentation::CanUseAotCode()` before it hands a freshly linked method its AOT body, and that
 * returns false for a Java-debuggable runtime -- "for simplicity, we never use AOT code for
 * debuggable", in its own words. Telling the runtime it is Java-debuggable therefore makes
 * `ClassLinker::LinkCode` give every method ART links from then on an interpreter entry point
 * instead, upgraded to nterp once its class verifies. Calls go through the ArtMethod again, so
 * hooks fire. Deoptimizing the boot image does the same for framework code that was already linked.
 *
 * Three consequences worth knowing before calling it:
 *
 * - **It only helps classes ART has not linked yet.** A class whose methods already hold AOT entry
 *   points keeps them. Call this before the code you intend to hook, and its callers, are first
 *   touched -- which init() does, by running it at library load.
 * - **It costs performance, process-wide.** The app runs nterp plus JIT rather than AOT code, and
 *   while debuggable the JIT compiles without inlining.
 * - **It rewrites entry points**, so it has to happen before any hook is installed, or the hook is
 *   overwritten.
 *
 * Every address it uses is a symbol libart.so exports by name; nothing here indexes a runtime
 * struct. A platform that renames or stops exporting one makes this return false and change
 * nothing -- the same "measure, don't assume" bargain as the ArtMethod layout.
 */
bool disable_aot_code();

/** Whether libart exports what disable_aot_code() needs, and so whether calling it can do anything. */
bool can_disable_aot_code();

/** Whether disable_aot_code() has already run successfully in this process. */
bool aot_code_disabled();

/**
 * Whether `entry_point` is ahead-of-time compiled code rather than one of ART's stubs.
 *
 * Told apart by what backs the mapping it lands in: AOT code lives in an oat file, while nterp, the
 * interpreter bridge and the resolution stub are all inside libart.so, and JIT output is anonymous.
 * A target still on AOT code is one whose class ART linked before disable_aot_code() ran, so its
 * callers may hold an inlined copy of it and the hook may never be reached -- which is worth saying
 * out loud, because nothing else about the hook will look wrong.
 */
bool is_aot_code(const void *entry_point);

/**
 * Whether `address` falls inside the mapping backed by libart.so.
 *
 * Used to validate the measured resolution-stub address: every ART stub -- the resolution
 * trampoline included -- lives in libart.so, so a measured "stub" that is not inside libart is not
 * the stub. Shares the /proc/self/maps walk with is_aot_code().
 */
bool is_in_libart(const void *address);

#endif //ARTHOOKS_DEOPTIMIZE_HPP
