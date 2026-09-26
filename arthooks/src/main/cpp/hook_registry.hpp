//
// Created by alon on 9/26/26.
//

#ifndef ARTHOOKS_HOOK_REGISTRY_HPP
#define ARTHOOKS_HOOK_REGISTRY_HPP

#include "art_method.hpp"

/**
 * Remembers what each hooked method's entry point was, and what was put there.
 *
 * Two things need this. A caller asking "is this still hooked?" needs something to compare the
 * current entry point against, because the field is the only evidence and ART rewrites it for
 * reasons of its own -- class initialisation, JIT compilation, deoptimisation. And unhooking needs
 * the address that was displaced.
 *
 * Entries are removed only by forget_hook(), and the trampolines they name are never freed, so a
 * stored address stays valid for the process's lifetime.
 */
void remember_hook(ArtMethod *target, void *previous_entry, void *installed_entry);

/**
 * Whether this method's entry point still holds the trampoline that was installed for it.
 *
 * False for a method that was never hooked, and false for one whose hook ART has since overwritten
 * -- which is the interesting answer, because nothing else reports that.
 */
bool hook_is_installed(ArtMethod *target);

/**
 * Drops the most recent hook on this method and hands back the entry point it displaced.
 *
 * Returns false when the method is not in the registry. A method hooked twice has two entries, so
 * unhooking twice walks back through them in order.
 */
bool forget_hook(ArtMethod *target, void **previous_entry_out);

#endif //ARTHOOKS_HOOK_REGISTRY_HPP
