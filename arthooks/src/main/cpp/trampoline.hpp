//
// Created by alon on 7/27/26.
//

#ifndef ARTHOOKS_TRAMPOLINE_HPP
#define ARTHOOKS_TRAMPOLINE_HPP

#include "art_method.hpp"

/**
 * Builds a native thunk that loads `art_method` into the register ART's quick calling convention
 * reserves for the ArtMethod*, then tail-jumps to that method's current entry point.
 *
 * This is what a hook installs in place of an entry point. Copying the replacement's entry point
 * directly does not work: compiled code, nterp and the interpreter bridge all read the method they
 * are running -- its declaring class, dex cache and code item -- out of that register, so entering
 * the replacement's code with the original's ArtMethod in it resolves the replacement's constants
 * against the wrong class. Swapping the register first makes the callee see itself.
 *
 * The entry point is loaded from `art_method` on every call rather than baked in, so the thunk
 * keeps working when ART later replaces it (class initialisation, JIT compilation, deoptimisation).
 *
 * Returns nullptr on failure. Trampolines live for the lifetime of the process.
 */
void *make_trampoline(ArtMethod *art_method);

/**
 * Builds the same thunk, except that `entry_point` is baked in instead of read from `art_method`.
 *
 * This is what a backup installs. A backup has to reach the original body, and the original's own
 * entry point is about to be overwritten with the hook -- so the address has to be captured before
 * the hook goes in, and reading it back from the method at call time would land in the hook.
 *
 * Baking the address in is what lets the thunk still name the *real* ArtMethod, and that matters
 * more than it looks: ArtMethod::declaring_class_ is a GcRoot, and the runtime rewrites it in every
 * real ArtMethod when the compacting GC relocates the class. A detached copy of an ArtMethod is
 * invisible to that fixup, so its declaring class silently goes stale -- and nterp dereferences
 * that field, two levels deep, on every single invocation. Pass the method ART knows about.
 *
 * The trade-off of a baked-in address is that ART replacing the original's code later (JIT,
 * deoptimisation) leaves this thunk pointing at the old body, which is what discourage_compilation()
 * exists to prevent.
 *
 * Returns nullptr on failure, including a null `entry_point`.
 */
void *make_direct_trampoline(ArtMethod *art_method, void *entry_point);

#endif //ARTHOOKS_TRAMPOLINE_HPP
