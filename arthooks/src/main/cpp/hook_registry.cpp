//
// Created by alon on 9/26/26.
//

#include "hook_registry.hpp"

#include <iterator>
#include <mutex>
#include <vector>

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

void remember_hook(ArtMethod *target, void *previous_entry, void *installed_entry) {
    std::lock_guard<std::mutex> guard(g_lock);
    g_hooks.push_back(Entry{target, previous_entry, installed_entry});
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

bool forget_hook(ArtMethod *target, void **previous_entry_out) {
    std::lock_guard<std::mutex> guard(g_lock);
    for (auto entry = g_hooks.rbegin(); entry != g_hooks.rend(); ++entry) {
        if (entry->target == target) {
            *previous_entry_out = entry->previous_entry;
            g_hooks.erase(std::next(entry).base());
            return true;
        }
    }
    return false;
}
