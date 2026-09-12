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

constexpr size_t align_up(size_t size) {
    return (size + 7u) & ~static_cast<size_t>(7u);
}

#if defined(__aarch64__)

// 0:  ldr  x0, #16            x0 = art_method, from the literal below
// 4:  ldr  x16, [x0, #entry]  x16 = art_method->entry_point_from_quick_compiled_code_
// 8:  br   x16
// 12: nop                     padding, so the literal lands 8-byte aligned
// 16: .quad art_method
constexpr size_t kIndirectSize = 24;

void emit_indirect(uint8_t *out, ArtMethod *art_method) {
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

// 0:  ldr  x0, #16     x0 = art_method, from the first literal
// 4:  ldr  x16, #20    x16 = entry_point, from the second literal
// 8:  br   x16
// 12: nop              padding, so the literals land 8-byte aligned
// 16: .quad art_method
// 24: .quad entry_point
constexpr size_t kDirectSize = 32;

void emit_direct(uint8_t *out, ArtMethod *art_method, void *entry_point) {
    const uint32_t code[] = {
            0x58000000u | (4u << 5),                        // ldr x0, #16
            0x58000000u | (5u << 5) | 16u,                  // ldr x16, #20
            0xD61F0000u | (16u << 5),                       // br x16
            0xD503201Fu,                                    // nop
    };
    memcpy(out, code, sizeof(code));
    memcpy(out + 16, &art_method, sizeof(art_method));
    memcpy(out + 24, &entry_point, sizeof(entry_point));
}

#elif defined(__x86_64__)

// movabs rdi, art_method
// jmp    [rdi + entry]
constexpr size_t kIndirectSize = 16;

void emit_indirect(uint8_t *out, ArtMethod *art_method) {
    const uint32_t entry = static_cast<uint32_t>(entry_point_offset());
    out[0] = 0x48;
    out[1] = 0xBF;
    memcpy(out + 2, &art_method, sizeof(art_method));
    out[10] = 0xFF;
    out[11] = 0xA7;
    memcpy(out + 12, &entry, sizeof(entry));
}

// 0:  movabs rdi, art_method
// 10: jmp    [rip + 0]        the literal sits immediately after the instruction
// 16: .quad  entry_point
//
// Jumping through memory rather than a register keeps every argument register intact, so no
// scratch register has to be borrowed from the calling convention.
constexpr size_t kDirectSize = 24;

void emit_direct(uint8_t *out, ArtMethod *art_method, void *entry_point) {
    out[0] = 0x48;
    out[1] = 0xBF;
    memcpy(out + 2, &art_method, sizeof(art_method));
    out[10] = 0xFF;
    out[11] = 0x25;
    const uint32_t displacement = 0;  // rip-relative, measured from the end of the instruction
    memcpy(out + 12, &displacement, sizeof(displacement));
    memcpy(out + 16, &entry_point, sizeof(entry_point));
}

#elif defined(__arm__)

// 0:  ldr r0, [pc, #4]     r0 = art_method, from the literal below
// 4:  ldr pc, [r0, #entry]  loading pc interworks, so a thumb entry point is fine
// 8:  nop                   never reached; padding for the literal
// 12: .word art_method
constexpr size_t kIndirectSize = 16;

void emit_indirect(uint8_t *out, ArtMethod *art_method) {
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

// 0:  ldr r0, [pc, #4]     r0 = art_method   (pc reads 8 ahead, so this is the word at 12)
// 4:  ldr pc, [pc, #4]     pc = entry_point  (the word at 16); loading pc interworks
// 8:  nop                  never reached; padding for the literals
// 12: .word art_method
// 16: .word entry_point
constexpr size_t kDirectSize = 24;

void emit_direct(uint8_t *out, ArtMethod *art_method, void *entry_point) {
    const uint32_t code[] = {
            0xE59F0004u,                // ldr r0, [pc, #4]
            0xE59FF004u,                // ldr pc, [pc, #4]
            0xE320F000u,                // nop
    };
    memcpy(out, code, sizeof(code));
    const uint32_t method_address = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(art_method));
    const uint32_t entry_address = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(entry_point));
    memcpy(out + 12, &method_address, sizeof(method_address));
    memcpy(out + 16, &entry_address, sizeof(entry_address));
}

#elif defined(__i386__)

// mov eax, art_method
// jmp [eax + entry]
constexpr size_t kIndirectSize = 11;

void emit_indirect(uint8_t *out, ArtMethod *art_method) {
    const uint32_t address = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(art_method));
    const uint32_t entry = static_cast<uint32_t>(entry_point_offset());
    out[0] = 0xB8;
    memcpy(out + 1, &address, sizeof(address));
    out[5] = 0xFF;
    out[6] = 0xA0;
    memcpy(out + 7, &entry, sizeof(entry));
}

// 0:  mov eax, art_method
// 5:  jmp [literal]        absolute indirect, so no argument register is borrowed
// 11: .long entry_point
constexpr size_t kDirectSize = 16;

void emit_direct(uint8_t *out, ArtMethod *art_method, void *entry_point) {
    const uint32_t method_address = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(art_method));
    const uint32_t entry_address = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(entry_point));
    const uint32_t literal_address = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(out + 11));
    out[0] = 0xB8;
    memcpy(out + 1, &method_address, sizeof(method_address));
    out[5] = 0xFF;
    out[6] = 0x25;
    memcpy(out + 7, &literal_address, sizeof(literal_address));
    memcpy(out + 11, &entry_address, sizeof(entry_address));
}

#else
#error "ArtHooks has no trampoline for this architecture"
#endif

// Trampolines are tiny and never freed, so they are bump-allocated out of one executable page.
uint8_t *g_page = nullptr;
size_t g_page_size = 0;
size_t g_page_used = 0;

/** Hands back `size` writable bytes in the trampoline page, moving to a fresh page if needed. */
uint8_t *reserve(size_t size) {
    const size_t stride = align_up(size);

    if (g_page_size == 0) {
        const long page_size = sysconf(_SC_PAGESIZE);
        g_page_size = (page_size > 0) ? static_cast<size_t>(page_size) : 4096u;
    }

    if (g_page == nullptr || g_page_used + stride > g_page_size) {
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
    g_page_used += stride;
    return trampoline;
}

/** Makes the page executable again and flushes the freshly written bytes out of the i-cache. */
bool seal(uint8_t *trampoline, size_t size) {
    if (mprotect(g_page, g_page_size, PROT_READ | PROT_EXEC) != 0) {
        LOGE("could not make the trampoline page executable: %s", strerror(errno));
        return false;
    }
    __builtin___clear_cache(reinterpret_cast<char *>(trampoline),
                            reinterpret_cast<char *>(trampoline + align_up(size)));
    return true;
}

}  // namespace

void *make_trampoline(ArtMethod *art_method) {
    uint8_t *trampoline = reserve(kIndirectSize);
    if (trampoline == nullptr) {
        return nullptr;
    }
    emit_indirect(trampoline, art_method);
    if (!seal(trampoline, kIndirectSize)) {
        return nullptr;
    }
    LOGD("trampoline %p -> ArtMethod %p", trampoline, art_method);
    return trampoline;
}

void *make_direct_trampoline(ArtMethod *art_method, void *entry_point) {
    if (entry_point == nullptr) {
        LOGE("refusing to build a trampoline to a null entry point");
        return nullptr;
    }
    uint8_t *trampoline = reserve(kDirectSize);
    if (trampoline == nullptr) {
        return nullptr;
    }
    emit_direct(trampoline, art_method, entry_point);
    if (!seal(trampoline, kDirectSize)) {
        return nullptr;
    }
    LOGD("trampoline %p -> ArtMethod %p, entry %p", trampoline, art_method, entry_point);
    return trampoline;
}
