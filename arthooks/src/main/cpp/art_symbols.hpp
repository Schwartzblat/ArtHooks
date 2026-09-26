//
// Created by alon on 9/26/26.
//

#ifndef ARTHOOKS_ART_SYMBOLS_HPP
#define ARTHOOKS_ART_SYMBOLS_HPP

/**
 * Resolves exported symbols out of the libart.so already mapped into this process.
 *
 * dlopen() cannot be used for this: libart.so is not in public.libraries.txt, so an app's
 * classloader namespace refuses to link against it. But the library is mapped and its .dynsym is
 * part of the read-only segment, so the symbols it *exports* can be looked up by walking that table
 * directly. Only exported symbols are reachable -- anything inlined or hidden is not, which is the
 * property that keeps this honest: every address used here is one libart publishes by name.
 *
 * Returns nullptr when the symbol is absent, which is the expected answer on a platform that
 * renamed or stopped exporting it. Callers are responsible for degrading instead of crashing.
 */
void *find_libart_symbol(const char *name);

#endif //ARTHOOKS_ART_SYMBOLS_HPP
