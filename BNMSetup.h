#pragma once
// ============================================================
//  BNMSetup.h  –  ByNameModding bootstrap & helper macros
//  Wraps the BNM public API so the rest of the mod only
//  needs to #include this one header.
//
//  BNM source:  https://github.com/ByNameModding/BNM-Android
//  Drop the entire BNM/include folder next to this file and
//  point CMakeLists at it.  The actual BNM implementation is
//  header-only after BNM_INIT() runs.
// ============================================================

#include <jni.h>
#include <android/log.h>
#include <string>
#include <cstdint>
#include <cstring>
#include <dlfcn.h>

// ── Logging helpers ─────────────────────────────────────────
#define LOG_TAG  "ACModMenu"
#define LOGI(...)  __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGW(...)  __android_log_print(ANDROID_LOG_WARN,  LOG_TAG, __VA_ARGS__)
#define LOGE(...)  __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define LOGD(...)  __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)

// ── BNM forward declarations ─────────────────────────────────
// BNM lives in the BNM/ subdirectory.  If BNM headers are not
// present the compile will fail with a clear error rather than
// silently producing a broken binary.
#if __has_include("BNM/BNM.hpp")
    #include "BNM/BNM.hpp"
    #include "BNM/Class.hpp"
    #include "BNM/Method.hpp"
    #include "BNM/Field.hpp"
    #include "BNM/Delegates.hpp"
    #include "BNM/BasicMonoStructures.hpp"
    #define BNM_AVAILABLE 1
    LOGI_HELPER("BNM headers found — using ByNameModding for IL2CPP reflection.");
#else
    #warning "BNM headers not found – falling back to raw dlsym/offset stubs."
    #define BNM_AVAILABLE 0
#endif

// ── Dobby fallback (raw function hooking) ───────────────────
//  Used when BNM is unavailable OR for hooking non-IL2CPP
//  native functions.
#if __has_include("dobby.h")
    #include "dobby.h"
    #define DOBBY_AVAILABLE 1
#else
    #define DOBBY_AVAILABLE 0
#endif

// ── IL2CPP basic types (always needed) ──────────────────────
//  Mirrors il2cpp-api-types.h without requiring the full SDK.
typedef struct Il2CppObject Il2CppObject;
typedef struct Il2CppString Il2CppString;
typedef struct Il2CppClass  Il2CppClass;
typedef struct Il2CppArray  Il2CppArray;
typedef struct Il2CppDomain Il2CppDomain;

struct Il2CppObject {
    Il2CppClass* klass;
    void*        monitor;
};

struct Il2CppString {
    Il2CppObject object;
    int32_t      length;
    uint16_t     chars[1]; // UTF-16 inline buffer
};

struct Il2CppArray {
    Il2CppObject object;
    void*        bounds;
    uintptr_t    max_length;
    void*        vector[1];
};

// ── il2cpp runtime API typedefs ─────────────────────────────
//  We resolve these at runtime from libil2cpp.so via dlsym.
typedef Il2CppDomain* (*il2cpp_domain_get_t)();
typedef Il2CppClass*  (*il2cpp_class_from_name_t)(void* img, const char* ns, const char* name);
typedef void*         (*il2cpp_class_get_method_from_name_t)(Il2CppClass* klass, const char* name, int argsCount);
typedef void*         (*il2cpp_method_get_pointer_t)(void* method);
typedef Il2CppObject* (*il2cpp_object_new_t)(Il2CppClass* klass);
typedef Il2CppString* (*il2cpp_string_new_t)(const char* str);
typedef void*         (*il2cpp_resolve_icall_t)(const char* name);
typedef void*         (*il2cpp_class_get_field_from_name_t)(Il2CppClass* klass, const char* name);
typedef void          (*il2cpp_field_get_value_t)(Il2CppObject* obj, void* field, void* value);
typedef void          (*il2cpp_field_set_value_t)(Il2CppObject* obj, void* field, void* value);

// ── Global resolved IL2CPP API pointers ─────────────────────
namespace IL2CPP {
    inline il2cpp_domain_get_t               domain_get              = nullptr;
    inline il2cpp_class_from_name_t          class_from_name         = nullptr;
    inline il2cpp_class_get_method_from_name_t get_method_from_name  = nullptr;
    inline il2cpp_method_get_pointer_t       method_get_pointer      = nullptr;
    inline il2cpp_object_new_t               object_new              = nullptr;
    inline il2cpp_string_new_t               string_new              = nullptr;
    inline il2cpp_resolve_icall_t            resolve_icall           = nullptr;
    inline il2cpp_class_get_field_from_name_t get_field_from_name    = nullptr;
    inline il2cpp_field_get_value_t          field_get_value         = nullptr;
    inline il2cpp_field_set_value_t          field_set_value         = nullptr;
    inline void*                             libil2cpp_handle        = nullptr;

    // Resolve a single symbol from libil2cpp.so with error logging
    inline void* Sym(const char* name) {
        if (!libil2cpp_handle) return nullptr;
        void* sym = dlsym(libil2cpp_handle, name);
        if (!sym) LOGE("dlsym failed for %s: %s", name, dlerror());
        return sym;
    }

    // Call once after libil2cpp.so is loaded in memory.
    // Returns true on full success, false if any symbol is missing.
    inline bool Init() {
        // Try the in-process handle first (library already loaded by the game)
        libil2cpp_handle = dlopen("libil2cpp.so", RTLD_NOW | RTLD_NOLOAD);
        if (!libil2cpp_handle) {
            // Some builds embed il2cpp symbols into the main executable
            libil2cpp_handle = dlopen(nullptr, RTLD_NOW);
        }
        if (!libil2cpp_handle) {
            LOGE("Could not obtain handle to libil2cpp.so: %s", dlerror());
            return false;
        }

#define RESOLVE(fn, type) fn = reinterpret_cast<type>(Sym(#fn)); if(!fn){ LOGE("Missing: " #fn); }
        RESOLVE(il2cpp_domain_get,                  il2cpp_domain_get_t)
        RESOLVE(il2cpp_class_from_name,             il2cpp_class_from_name_t)
        RESOLVE(il2cpp_class_get_method_from_name,  il2cpp_class_get_method_from_name_t)
        RESOLVE(il2cpp_method_get_pointer,          il2cpp_method_get_pointer_t)
        RESOLVE(il2cpp_object_new,                  il2cpp_object_new_t)
        RESOLVE(il2cpp_string_new,                  il2cpp_string_new_t)
        RESOLVE(il2cpp_resolve_icall,               il2cpp_resolve_icall_t)
        RESOLVE(il2cpp_class_get_field_from_name,   il2cpp_class_get_field_from_name_t)
        RESOLVE(il2cpp_field_get_value,             il2cpp_field_get_value_t)
        RESOLVE(il2cpp_field_set_value,             il2cpp_field_set_value_t)
#undef RESOLVE

        bool ok = domain_get && class_from_name && get_method_from_name
               && method_get_pointer && object_new && string_new;
        if (ok) LOGI("IL2CPP API resolved successfully.");
        else    LOGE("IL2CPP API resolution INCOMPLETE – some hooks will be no-ops.");
        return ok;
    }
} // namespace IL2CPP

// ── Unity math structs ───────────────────────────────────────
//  Plain C structs that match Unity's blittable layout in IL2CPP.
struct Vector2 { float x, y; };
struct Vector3 { float x, y, z;
    Vector3 operator+(const Vector3& o) const { return {x+o.x, y+o.y, z+o.z}; }
    Vector3 operator-(const Vector3& o) const { return {x-o.x, y-o.y, z-o.z}; }
    Vector3 operator*(float f)          const { return {x*f,   y*f,   z*f};   }
};
struct Vector4  { float x, y, z, w; };
struct Quaternion { float x, y, z, w; };
struct Color    { float r, g, b, a; };

// ── Safe hook helper ─────────────────────────────────────────
//  Wraps Dobby's DobbyHook with logging; no-ops if Dobby is absent.
inline bool SafeHook(void* target, void* replacement, void** original,
                     const char* tag = "hook") {
#if DOBBY_AVAILABLE
    if (!target) { LOGW("[%s] Hook target is null, skipping.", tag); return false; }
    int rc = DobbyHook(target, replacement, original);
    if (rc != 0) { LOGE("[%s] DobbyHook failed (rc=%d).", tag, rc); return false; }
    LOGI("[%s] Hook installed at %p.", tag, target);
    return true;
#else
    LOGW("[%s] Dobby unavailable – hook skipped.", tag);
    (void)target; (void)replacement; (void)original;
    return false;
#endif
}

// ── BNM wrapper macros ───────────────────────────────────────
//  Thin sugar so mod.cpp reads like documentation.
#if BNM_AVAILABLE
    // Find a BNM class; logs a warning and evaluates to an invalid class on miss.
    #define BNM_CLASS(ns, name) \
        ([&]() -> BNM::Class { \
            auto c = BNM::Class(ns, name); \
            if (!c) LOGW("BNM: class not found – %s::%s", ns, name); \
            return c; \
        }())

    // Find a method by name + arg count; returns null MethodBase on miss.
    #define BNM_METHOD(cls, name, argc) \
        ([&]() -> BNM::MethodBase { \
            auto m = (cls).GetMethod(name, argc); \
            if (!m) LOGW("BNM: method not found – %s (argc=%d)", name, argc); \
            return m; \
        }())
#endif // BNM_AVAILABLE
