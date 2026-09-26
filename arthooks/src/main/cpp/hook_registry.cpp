//
// Created by alon on 9/26/26.
//

#include "hook_registry.hpp"

#include <iterator>
#include <mutex>
#include <vector>

#include "log.hpp"

namespace {

struct Entry {
    ArtMethod *target;
    void *previous_entry;
    void *installed_entry;
};

// A vector rather than a map: hooks are counted in tens, lookups happen at install time and at an
// explicit is_hooked() call, and a stack is exactly the shape unwinding a chained hook needs.
std::vector<Entry> g_hooks;
std::mutex g_lock;

}  // namespace

bool install_hook(ArtMethod *target, void *trampoline) {
    std::lock_guard<std::mutex> guard(g_lock);
    void *previous_entry = get_entry_point(target);
    if (!set_entry_point(target, trampoline)) {
        return false;
    }
    g_hooks.push_back(Entry{target, previous_entry, trampoline});
    return true;
}

bool hook_is_installed(ArtMethod *target) {
    std::lock_guard<std::mutex> guard(g_lock);
    for (auto entry = g_hooks.rbegin(); entry != g_hooks.rend(); ++entry) {
        if (entry->target == target) {
            return get_entry_point(target) == entry->installed_entry;
        }
    }
    return false;
}

bool remove_hook(ArtMethod *target, void **restored_entry_out) {
    std::lock_guard<std::mutex> guard(g_lock);
    for (auto entry = g_hooks.rbegin(); entry != g_hooks.rend(); ++entry) {
        if (entry->target != target) {
            continue;
        }
        if (get_entry_point(target) != entry->installed_entry) {
            // Something other than this registry moved the entry point since this hook went in --
            // ART's own doing (class init, JIT, deoptimization). The "previous" address recorded
            // alongside it describes a hook that is no longer the live one, so it is not safe to
            // write back. The record is stale either way, so it is dropped rather than kept around
            // to be misread the same way again.
            LOGW("ArtMethod %p's hook is no longer installed -- something else already changed its "
                 "entry point; dropping the stale record instead of restoring it", target);
            g_hooks.erase(std::next(entry).base());
            return false;
        }
        void *previous_entry = entry->previous_entry;
        if (!set_entry_point(target, previous_entry)) {
            // The write did not take, so the trampoline this record names is presumably still live.
            // Losing the record now would make is_hooked() start lying and would hand a *second*
            // unhook attempt the wrong "previous" address -- possibly overwriting a still-live outer
            // hook in a chain. Leave it exactly as it was and let the caller retry.
            return false;
        }
        g_hooks.erase(std::next(entry).base());
        *restored_entry_out = previous_entry;
        return true;
    }
    LOGW("ArtMethod %p is not hooked", target);
    return false;
}
