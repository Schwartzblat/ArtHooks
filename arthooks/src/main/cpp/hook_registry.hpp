//
// Created by alon on 9/26/26.
//

#ifndef ARTHOOKS_HOOK_REGISTRY_HPP
#define ARTHOOKS_HOOK_REGISTRY_HPP

#include "art_method.hpp"

/**
 * Remembers what each hooked method's entry point was, and performs the entry-point write itself --
 * for both installing and removing a hook -- so that a lock can cover the read/check, the write and
 * the bookkeeping as one step.
 *
 * That atomicity is what makes this safe against two hazards a split "read/write, then update the
 * registry" sequence cannot avoid on its own:
 *
 * - A hook_function() and an unhook_function() aimed at the *same* target racing each other. If each
 *   only took the lock around its own bookkeeping, one thread could capture "the current entry point"
 *   between the other's write and its bookkeeping update, or two unhook calls could pop the registry
 *   in one order and land their writes in the other order -- leaving a live trampoline with no record
 *   naming it. Locking the whole read-check-write-update sequence rules that out: install_hook() and
 *   remove_hook() on one target run strictly one at a time, end to end.
 * - A hook that ART itself has since overwritten (class initialisation, JIT compilation,
 *   deoptimisation). remove_hook() re-checks that the live entry point still matches the record before
 *   trusting the "previous" address in it -- because if ART already replaced this hook with something
 *   of its own, that address is not a displaced hook's original body any more, it is just stale.
 *
 * Entries are removed only by a *verified* remove_hook(), and the trampolines they name are never
 * freed, so a stored address stays valid for the process's lifetime.
 */

/**
 * Installs a hook on `target`: reads its current entry point, writes `trampoline` in its place, and
 * -- only if that write is verified -- records the displaced entry point so a later remove_hook() can
 * restore it.
 *
 * Returns false, having recorded nothing, if the write did not take.
 */
bool install_hook(ArtMethod *target, void *trampoline);

/**
 * Whether this method's entry point still holds the trampoline that was installed for it.
 *
 * False for a method that was never hooked, and false for one whose hook ART has since overwritten
 * -- which is the interesting answer, because nothing else reports that.
 */
bool hook_is_installed(ArtMethod *target);

/**
 * Removes the most recent hook on `target` and restores the entry point it displaced, handing that
 * address back through `*restored_entry_out` on success (the caller has no other way to see it, since
 * the write happens here, under the registry's lock).
 *
 * Three outcomes:
 * - No hook is on record for this target: returns false, nothing touched.
 * - A hook is on record, but the live entry point no longer matches the trampoline it says is
 *   installed -- ART has overwritten it by some mechanism of its own, so the recorded "previous"
 *   address is not trustworthy either. Nothing is written; the stale record is dropped anyway, since
 *   it no longer describes reality, and this returns false.
 * - The live entry point still matches: the previous entry point is written back. If that write is
 *   verified, the record is dropped and this returns true. If the write did not take, the record is
 *   *kept* -- the hook is presumably still standing (a no-op write leaves the old trampoline live), so
 *   hook_is_installed() has to keep saying so, and a later call must not lose this address -- and this
 *   returns false.
 *
 * A method hooked twice has two entries, so removing twice walks back through them in order.
 */
bool remove_hook(ArtMethod *target, void **restored_entry_out);

#endif //ARTHOOKS_HOOK_REGISTRY_HPP
