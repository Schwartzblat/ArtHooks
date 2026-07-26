//
// Created by alon on 6/30/26.
//

#ifndef ARTHOOKS_ART_METHOD_HPP
#define ARTHOOKS_ART_METHOD_HPP


#include <jni.h>

#include <cstddef>
#include <cstdint>


/**
 * Mirror of art::ArtMethod.
 *
 * This is documentation of the layout we expect, not something the hook indexes into: the fields
 * that matter (ptr_sized_fields_) are reached through offsets measured at runtime, so a layout
 * change in a future release costs us a startup warning instead of memory corruption. Everything
 * ahead of ptr_sized_fields_ is left alone entirely, which is what keeps a hooked method's identity
 * (declaring class, access flags, dex/vtable indices) intact for reflection and virtual dispatch.
 */
typedef struct {
    uint32_t declaring_class_;

    uint32_t access_flags_;

    uint32_t dex_method_index_;

    uint16_t method_index_;

    union {
        uint16_t hotness_count_;
        uint16_t imt_index_;
    };

    // Always last in art::ArtMethod, and data_ always precedes the entry point. That ordering is
    // the one property the runtime measurement relies on.
    struct PtrSizedFields {
        void *data_;
        void *entry_point_from_quick_compiled_code_;
    } ptr_sized_fields_;
} ArtMethod;

/**
 * Caches what it takes to reach an ArtMethod on this platform, and must succeed before anything
 * else here is called.
 *
 * That means the hidden Executable.artMethod field, plus sizeof(art::ArtMethod), which is measured
 * rather than assumed: ART lays a class's methods out as one contiguous array, so the distance
 * between the two adjacent ArtHooks.layout_probe_* methods is the element size. The trailing
 * pointer-sized fields are then derived from that size, because they are always last.
 */
bool init_art_method_access(JNIEnv *env, jclass arthooks_class, jint sdk_version);

/** sizeof(art::ArtMethod) on this platform. Valid once init_art_method_access() has succeeded. */
size_t art_method_size();

/** Offset of entry_point_from_quick_compiled_code_ within an ArtMethod. */
size_t entry_point_offset();

/** Resolves a java.lang.reflect.Method or Constructor to the ArtMethod behind it. */
ArtMethod *get_art_method(JNIEnv *env, jobject executable);

void *get_entry_point(const ArtMethod *art_method);

void set_entry_point(ArtMethod *art_method, void *entry_point);


#endif //ARTHOOKS_ART_METHOD_HPP
