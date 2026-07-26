//
// Created by alon on 7/27/26.
//

#include "trampoline.hpp"

#include "log.hpp"

#include <sys/mman.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

namespace {

#if defined(__aarch64__)

// 0:  ldr  x0, #16            x0 = art_method, from the literal below
// 4:  ldr  x16, [x0, #entry]  x16 = art_method->entry_point_from_quick_compiled_code_
// 8:  br   x16
// 12: nop                     padding, so the literal lands 8-byte aligned
// 16: .quad art_method
constexpr size_t kTrampolineSize = 24;

void emit_trampoline(uint8_t *out, ArtMethod *art_method) {
    const uint32_t entry_slot = static_cast<uint32_t>(entry_point_offset() / sizeof(void *));
    const uint32_t code[] = {
            0x58000000u | (4u << 5),                        // ldr x0, #16
            0xF9400000u | (entry_slot << 10) | 16u,         // ldr x16, [x0, #entry]
            0xD61F0000u | (16u << 5),                       // br x16
            0xD503201Fu,                                    // nop
    };
    memcpy(out, code, sizeof(code));
    memcpy(out + 16, &art_method, sizeof(art_method));
}

#elif defined(__x86_64__)

// movabs rdi, art_method
// jmp    [rdi + entry]
constexpr size_t kTrampolineSize = 16;

void emit_trampoline(uint8_t *out, ArtMethod *art_method) {
    const uint32_t entry = static_cast<uint32_t>(entry_point_offset());
    out[0] = 0x48;
    out[1] = 0xBF;
    memcpy(out + 2, &art_method, sizeof(art_method));
    out[10] = 0xFF;
    out[11] = 0xA7;
    memcpy(out + 12, &entry, sizeof(entry));
}

#elif defined(__arm__)

// 0:  ldr r0, [pc, #4]     r0 = art_method, from the literal below
// 4:  ldr pc, [r0, #entry]  loading pc interworks, so a thumb entry point is fine
// 8:  nop                   never reached; padding for the literal
// 12: .word art_method
constexpr size_t kTrampolineSize = 16;

void emit_trampoline(uint8_t *out, ArtMethod *art_method) {
    const uint32_t entry = static_cast<uint32_t>(entry_point_offset());
    const uint32_t code[] = {
            0xE59F0004u,                // ldr r0, [pc, #4]
            0xE590F000u | entry,        // ldr pc, [r0, #entry]
            0xE320F000u,                // nop
    };
    memcpy(out, code, sizeof(code));
    const uint32_t address = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(art_method));
    memcpy(out + 12, &address, sizeof(address));
}

#elif defined(__i386__)

// mov eax, art_method
// jmp [eax + entry]
constexpr size_t kTrampolineSize = 11;

void emit_trampoline(uint8_t *out, ArtMethod *art_method) {
    const uint32_t address = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(art_method));
    const uint32_t entry = static_cast<uint32_t>(entry_point_offset());
    out[0] = 0xB8;
    memcpy(out + 1, &address, sizeof(address));
    out[5] = 0xFF;
    out[6] = 0xA0;
    memcpy(out + 7, &entry, sizeof(entry));
}

#else
#error "ArtHooks has no trampoline for this architecture"
#endif

// Trampolines are tiny and never freed, so they are bump-allocated out of one executable page.
uint8_t *g_page = nullptr;
size_t g_page_size = 0;
size_t g_page_used = 0;

constexpr size_t kTrampolineStride = (kTrampolineSize + 7u) & ~static_cast<size_t>(7u);

}  // namespace

void *make_trampoline(ArtMethod *art_method) {
    if (g_page_size == 0) {
        const long page_size = sysconf(_SC_PAGESIZE);
        g_page_size = (page_size > 0) ? static_cast<size_t>(page_size) : 4096u;
    }

    if (g_page == nullptr || g_page_used + kTrampolineStride > g_page_size) {
        void *page = mmap(nullptr, g_page_size, PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (page == MAP_FAILED) {
            LOGE("could not map a trampoline page: %s", strerror(errno));
            return nullptr;
        }
        g_page = static_cast<uint8_t *>(page);
        g_page_used = 0;
    } else if (mprotect(g_page, g_page_size, PROT_READ | PROT_WRITE) != 0) {
        LOGE("could not make the trampoline page writable: %s", strerror(errno));
        return nullptr;
    }

    uint8_t *trampoline = g_page + g_page_used;
    emit_trampoline(trampoline, art_method);
    g_page_used += kTrampolineStride;

    if (mprotect(g_page, g_page_size, PROT_READ | PROT_EXEC) != 0) {
        LOGE("could not make the trampoline page executable: %s", strerror(errno));
        return nullptr;
    }
    __builtin___clear_cache(reinterpret_cast<char *>(trampoline),
                            reinterpret_cast<char *>(trampoline + kTrampolineStride));

    LOGD("trampoline %p -> ArtMethod %p", trampoline, art_method);
    return trampoline;
}
