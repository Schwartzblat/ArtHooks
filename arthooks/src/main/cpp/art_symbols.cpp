//
// Created by alon on 9/26/26.
//

#include "art_symbols.hpp"
#include "log.hpp"

#include <elf.h>
#include <link.h>

#include <cstring>

namespace {

struct Library {
    ElfW(Addr) bias;
    const ElfW(Sym) *symbols;
    const char *strings;
    const uint32_t *gnu_hash;
};

Library g_libart = {0, nullptr, nullptr, nullptr};
bool g_looked_for_libart = false;

/**
 * Turns a d_un.d_ptr out of PT_DYNAMIC into an address in this process.
 *
 * The ELF spec lets these hold either link-time virtual addresses or already-relocated ones, and
 * which you get depends on the linker that produced the file. A shared library is mapped far above
 * its own link-time addresses, so "smaller than the load bias" separates the two cases.
 */
const void *resolve(ElfW(Addr) bias, ElfW(Addr) value) {
    const uintptr_t address = (value < bias) ? (bias + value) : value;
    return reinterpret_cast<const void *>(address);
}

/** The djb2-with-33 hash that DT_GNU_HASH buckets are keyed by. */
uint32_t gnu_hash_of(const char *name) {
    uint32_t hash = 5381;
    for (const uint8_t *c = reinterpret_cast<const uint8_t *>(name); *c != '\0'; c++) {
        hash = hash * 33 + *c;
    }
    return hash;
}

/** Picks the symbol, string and hash tables out of one loaded object's PT_DYNAMIC. */
bool describe(const dl_phdr_info *info, Library *out) {
    const ElfW(Dyn) *dynamic = nullptr;
    for (ElfW(Half) i = 0; i < info->dlpi_phnum; i++) {
        if (info->dlpi_phdr[i].p_type == PT_DYNAMIC) {
            dynamic = static_cast<const ElfW(Dyn) *>(
                    resolve(info->dlpi_addr, info->dlpi_phdr[i].p_vaddr));
            break;
        }
    }
    if (dynamic == nullptr) {
        return false;
    }

    Library found = {info->dlpi_addr, nullptr, nullptr, nullptr};
    for (const ElfW(Dyn) *entry = dynamic; entry->d_tag != DT_NULL; entry++) {
        switch (entry->d_tag) {
            case DT_SYMTAB:
                found.symbols = static_cast<const ElfW(Sym) *>(
                        resolve(info->dlpi_addr, entry->d_un.d_ptr));
                break;
            case DT_STRTAB:
                found.strings = static_cast<const char *>(
                        resolve(info->dlpi_addr, entry->d_un.d_ptr));
                break;
            case DT_GNU_HASH:
                found.gnu_hash = static_cast<const uint32_t *>(
                        resolve(info->dlpi_addr, entry->d_un.d_ptr));
                break;
            default:
                break;
        }
    }

    // All three are needed. The hash table in particular is not optional: no DT_ tag records how
    // many entries DT_SYMTAB has, so its chain is the only thing that bounds the search. libart.so
    // is built --hash-style=gnu, so DT_HASH -- which would carry the count directly -- is absent.
    if (found.symbols == nullptr || found.strings == nullptr || found.gnu_hash == nullptr) {
        return false;
    }
    *out = found;
    return true;
}

/** Whether this object's path names libart.so itself rather than something with a similar name. */
bool is_libart(const char *name) {
    if (name == nullptr) {
        return false;
    }
    const char *last_slash = strrchr(name, '/');
    const char *base = (last_slash != nullptr) ? last_slash + 1 : name;
    return strcmp(base, "libart.so") == 0;
}

/** Finds libart.so among the objects mapped into this process. Cached, including the failure. */
const Library *libart() {
    if (g_looked_for_libart) {
        return (g_libart.symbols != nullptr) ? &g_libart : nullptr;
    }
    g_looked_for_libart = true;

    dl_iterate_phdr(
            [](dl_phdr_info *info, size_t, void *data) -> int {
                if (!is_libart(info->dlpi_name)) {
                    return 0;  // keep looking
                }
                if (!describe(info, static_cast<Library *>(data))) {
                    LOGW("found %s but could not read its symbol table", info->dlpi_name);
                }
                return 1;  // stop either way: there is only one libart
            },
            &g_libart);

    if (g_libart.symbols == nullptr) {
        LOGW("libart.so is not visible to dl_iterate_phdr");
        return nullptr;
    }
    return &g_libart;
}

}  // namespace

void *find_libart_symbol(const char *name) {
    const Library *library = libart();
    if (library == nullptr) {
        return nullptr;
    }

    // DT_GNU_HASH is a header, then a Bloom filter, then the buckets and the chain. The filter is
    // only a fast reject, so it is skipped -- but its size still has to be stepped over.
    const uint32_t bucket_count = library->gnu_hash[0];
    const uint32_t first_hashed = library->gnu_hash[1];
    const uint32_t bloom_words = library->gnu_hash[2];
    if (bucket_count == 0 || bloom_words == 0) {
        return nullptr;
    }

    const uint32_t *buckets =
            library->gnu_hash + 4 + bloom_words * (sizeof(ElfW(Addr)) / sizeof(uint32_t));
    const uint32_t *chain = buckets + bucket_count;

    const uint32_t hash = gnu_hash_of(name);
    uint32_t index = buckets[hash % bucket_count];
    // Zero is an empty bucket, and nothing below the first hashed symbol has a chain entry.
    if (index < first_hashed) {
        return nullptr;
    }

    for (;;) {
        const uint32_t entry = chain[index - first_hashed];
        // The low bit marks the end of the chain rather than being part of the hash, so both sides
        // drop it before comparing.
        if (((entry ^ hash) >> 1) == 0) {
            const ElfW(Sym) *symbol = &library->symbols[index];
            if (symbol->st_shndx != SHN_UNDEF && symbol->st_value != 0 &&
                strcmp(library->strings + symbol->st_name, name) == 0) {
                return reinterpret_cast<void *>(library->bias + symbol->st_value);
            }
        }
        if ((entry & 1) != 0) {
            return nullptr;
        }
        index++;
    }
}
