#pragma once

#include <cstdint>
#include <cstring>
#include <string>

#include "config.h"
#include "safe_memory.h"
#include "ue_math.h"   // TArray64

// ---------------------------------------------------------------------------
// Offsets — one place to update after a game patch
// ---------------------------------------------------------------------------
namespace Offsets {
    constexpr uintptr_t UObject_Name      = 0x48;  // FName { int32 Index; int32 Number; }
    constexpr uintptr_t UObject_Class     = 0x50;
    constexpr uintptr_t FNameEntry_String = 0x14;  // inline ANSI string
}

constexpr size_t   kMaxNameLen        = 128;
constexpr int32_t  kInvalidNameIndex  = -1;
constexpr uint64_t kRescanCooldownMs  = 250;   // don't full-scan GObjects every frame

// ---------------------------------------------------------------------------
// Global arrays (GNames / GObjects are both TArray<T*>)
// ---------------------------------------------------------------------------
inline bool ReadGlobalArray(uintptr_t moduleBase, uintptr_t rva, TArray64& out) {
    if (!SafeMem::TryRead(moduleBase + rva, out)) return false;
    return out.IsValid() && SafeMem::IsValidPtr(out.Data);
}

inline uintptr_t ReadArrayPtr(const TArray64& arr, int32_t i) {
    if (!arr.IsValidIndex(i)) return 0;
    return SafeMem::ReadFast<uintptr_t>(arr.ElementAddress<uintptr_t>(i));
}

// ---------------------------------------------------------------------------
// Names
// ---------------------------------------------------------------------------

// Copies the string for a name index into buf (always NUL-terminated).
// Reads in small chunks so we never touch bytes far past the real string end.
inline bool GetNameByIndex(const TArray64& gNames, int32_t idx, char* buf, size_t bufSize) {
    if (!buf || bufSize == 0) return false;
    buf[0] = '\0';

    const uintptr_t entry = ReadArrayPtr(gNames, idx);
    if (!SafeMem::IsValidPtr(entry)) return false;

    constexpr size_t kChunk = 16;
    const uintptr_t src = entry + Offsets::FNameEntry_String;
    size_t written = 0;

    while (written + 1 < bufSize) {
        char chunk[kChunk];
        const size_t want = (bufSize - 1 - written < kChunk) ? bufSize - 1 - written : kChunk;
        if (!SafeMem::ReadBytesFast(src + written, chunk, want)) break;

        for (size_t i = 0; i < want; ++i) {
            buf[written] = chunk[i];
            if (chunk[i] == '\0') return written > 0;
            ++written;
        }
    }
    buf[written] = '\0';
    return written > 0;
}

inline int32_t GetObjectNameIndex(uintptr_t obj) {
    if (!SafeMem::IsValidPtr(obj)) return kInvalidNameIndex;
    return SafeMem::ReadFast<int32_t>(obj + Offsets::UObject_Name, kInvalidNameIndex);
}

inline uintptr_t GetObjectClass(uintptr_t obj) {
    if (!SafeMem::IsValidPtr(obj)) return 0;
    const uintptr_t cls = SafeMem::ReadFast<uintptr_t>(obj + Offsets::UObject_Class);
    return SafeMem::IsValidPtr(cls) ? cls : 0;
}

// Safe replacement for the old const char* version: returns an owned copy,
// so the caller never holds a pointer into game memory that can be freed.
inline std::string GetObjectName(uintptr_t moduleBase, uintptr_t obj) {
    TArray64 gNames;
    if (!ReadGlobalArray(moduleBase, g_Config.RVA_GNames, gNames)) return {};

    char buf[kMaxNameLen];
    return GetNameByIndex(gNames, GetObjectNameIndex(obj), buf, sizeof(buf)) ? std::string(buf) : std::string();
}

// Linear search of GNames for an exact string. Slow — call once and cache.
inline int32_t FindNameIndex(uintptr_t moduleBase, const char* name) {
    TArray64 gNames;
    if (!name || !ReadGlobalArray(moduleBase, g_Config.RVA_GNames, gNames)) return kInvalidNameIndex;

    char buf[kMaxNameLen];
    for (int32_t i = 0; i < gNames.Num(); ++i) {
        if (GetNameByIndex(gNames, i, buf, sizeof(buf)) && std::strcmp(buf, name) == 0)
            return i;
    }
    return kInvalidNameIndex;
}

// ---------------------------------------------------------------------------
// Player controller
// ---------------------------------------------------------------------------
namespace detail {

struct PCNameCache {
    uintptr_t moduleBase = 0;
    int32_t   classIdx   = kInvalidNameIndex;   // "WillowPlayerController"
    int32_t   cdoIdx     = kInvalidNameIndex;   // "Default__WillowPlayerController"

    bool Ready(uintptr_t base) {
        if (moduleBase != base || classIdx == kInvalidNameIndex) {
            moduleBase = base;
            classIdx   = FindNameIndex(base, "WillowPlayerController");
            cdoIdx     = FindNameIndex(base, "Default__WillowPlayerController");
        }
        return classIdx != kInvalidNameIndex;
    }
};

inline PCNameCache g_PCNames;
inline uint64_t    g_LastPCScanMs = 0;

// Pure integer compares — no string reads in the hot path.
inline bool IsLivePlayerController(uintptr_t obj, const PCNameCache& names) {
    const uintptr_t cls = GetObjectClass(obj);
    if (!cls || GetObjectNameIndex(cls) != names.classIdx) return false;
    return GetObjectNameIndex(obj) != names.cdoIdx;   // skip the class default object
}

} // namespace detail

inline uintptr_t ResolvePlayerController(uintptr_t moduleBase) {
    auto& names = detail::g_PCNames;
    if (!names.Ready(moduleBase)) return 0;

    // 1. Cached pointer still valid?
    if (g_CachedPlayerController && detail::IsLivePlayerController(g_CachedPlayerController, names))
        return g_CachedPlayerController;
    g_CachedPlayerController = 0;

    // 2. Rate-limit full scans (during loading screens there is no controller,
    //    and scanning 100k+ objects every frame is wasted work).
    const uint64_t now = GetTickCount64();
    if (now - detail::g_LastPCScanMs < kRescanCooldownMs) return 0;
    detail::g_LastPCScanMs = now;

    // 3. Scan GObjects.
    TArray64 gObjects;
    if (!ReadGlobalArray(moduleBase, g_Config.RVA_GObjects, gObjects)) return 0;

    for (int32_t i = 0; i < gObjects.Num(); ++i) {
        const uintptr_t obj = ReadArrayPtr(gObjects, i);
        if (SafeMem::IsValidPtr(obj) && detail::IsLivePlayerController(obj, names)) {
            g_CachedPlayerController = obj;
            return obj;
        }
    }
    return 0;
}

// Call on level change / module reload if you want to force everything to refresh.
inline void ResetObjectResolverCache() {
    g_CachedPlayerController = 0;
    detail::g_PCNames       = {};
    detail::g_LastPCScanMs  = 0;
}
