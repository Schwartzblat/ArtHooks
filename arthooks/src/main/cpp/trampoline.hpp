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

#endif //ARTHOOKS_TRAMPOLINE_HPP
