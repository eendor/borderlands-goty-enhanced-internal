#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#include <cstdint>
#include <cstring>
#include <type_traits>

namespace SafeMem {

// ---------------------------------------------------------------------------
// Address-range bounds (queried once, correct for both x86 and x64 builds)
// ---------------------------------------------------------------------------
namespace detail {

struct AddressBounds {
    uintptr_t Min;
    uintptr_t Max;
};

inline const AddressBounds& Bounds() {
    static const AddressBounds b = [] {
        SYSTEM_INFO si{};
        GetSystemInfo(&si);
        return AddressBounds{
            reinterpret_cast<uintptr_t>(si.lpMinimumApplicationAddress),
            reinterpret_cast<uintptr_t>(si.lpMaximumApplicationAddress)
        };
    }();
    return b;
}

constexpr DWORD kReadableMask =
    PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
    PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;

constexpr DWORD kWritableMask =
    PAGE_READWRITE | PAGE_WRITECOPY |
    PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;

// Walks every region the range [addr, addr + size) touches, so a read that
// straddles a page/region boundary is checked on both sides.
inline bool CheckRange(uintptr_t addr, size_t size, DWORD requiredMask) {
    if (size == 0) return false;

    const uintptr_t end = addr + size;              // one past the last byte
    if (end < addr) return false;                   // overflow

    const AddressBounds& b = Bounds();
    if (addr < b.Min || end - 1 > b.Max) return false;

    uintptr_t cur = addr;
    while (cur < end) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(reinterpret_cast<LPCVOID>(cur), &mbi, sizeof(mbi)) == 0)
            return false;
        if (mbi.State != MEM_COMMIT)
            return false;
        if (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS))
            return false;
        if ((mbi.Protect & requiredMask) == 0)
            return false;

        cur = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
    }
    return true;
}

// SEH-guarded copies on MSVC; Win32 ReadProcessMemory/WriteProcessMemory on GCC/MinGW.
// Plain non-template functions with no C++ objects that need unwinding.
inline bool GuardedCopyFrom(void* dst, uintptr_t src, size_t size) {
#if defined(_MSC_VER)
    __try {
        std::memcpy(dst, reinterpret_cast<const void*>(src), size);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
#else
    SIZE_T read = 0;
    return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(src), dst, size, &read) && (read == size);
#endif
}

inline bool GuardedCopyTo(uintptr_t dst, const void* src, size_t size) {
#if defined(_MSC_VER)
    __try {
        std::memcpy(reinterpret_cast<void*>(dst), src, size);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
#else
    SIZE_T written = 0;
    return WriteProcessMemory(GetCurrentProcess(), reinterpret_cast<LPVOID>(dst), src, size, &written) && (written == size);
#endif
}

} // namespace detail

// ---------------------------------------------------------------------------
// Pointer checks
// ---------------------------------------------------------------------------
inline bool IsValidPtr(uintptr_t u) {
    const auto& b = detail::Bounds();
    return u >= b.Min && u <= b.Max;
}
inline bool IsValidPtr(const void* p) { return IsValidPtr(reinterpret_cast<uintptr_t>(p)); }

inline bool IsReadable(uintptr_t addr, size_t size) { return detail::CheckRange(addr, size, detail::kReadableMask); }
inline bool IsWritable(uintptr_t addr, size_t size) { return detail::CheckRange(addr, size, detail::kWritableMask); }

inline bool IsReadable(const void* p, size_t size) { return IsReadable(reinterpret_cast<uintptr_t>(p), size); }
inline bool IsWritable(const void* p, size_t size) { return IsWritable(reinterpret_cast<uintptr_t>(p), size); }

// ---------------------------------------------------------------------------
// Raw byte copies
//
// Two layers of protection:
//   1. VirtualQuery pre-check — rejects obviously bad addresses cheaply-ish.
//   2. SEH around the copy — catches the race where the page is freed or
//      reprotected between the check and the access (TOCTOU).
// ---------------------------------------------------------------------------
inline bool ReadBytes(uintptr_t addr, void* out, size_t size) {
    if (!out || !IsReadable(addr, size)) return false;
    return detail::GuardedCopyFrom(out, addr, size);
}

inline bool WriteBytes(uintptr_t addr, const void* in, size_t size) {
    if (!in || !IsWritable(addr, size)) return false;
    return detail::GuardedCopyTo(addr, in, size);
}

// Fast path: SEH only, no VirtualQuery syscall. Use in hot loops
// (per-frame entity iteration) where the pre-check cost adds up.
inline bool ReadBytesFast(uintptr_t addr, void* out, size_t size) {
    if (!out || size == 0 || !IsValidPtr(addr)) return false;
    return detail::GuardedCopyFrom(out, addr, size);
}

// ---------------------------------------------------------------------------
// Typed helpers
// ---------------------------------------------------------------------------
template <typename T>
bool TryRead(uintptr_t addr, T& out) {
    static_assert(std::is_trivially_copyable_v<T>, "SafeMem::TryRead requires a trivially copyable type");
    return ReadBytes(addr, &out, sizeof(T));
}

template <typename T>
T Read(uintptr_t addr, T defaultVal = T{}) {
    static_assert(std::is_trivially_copyable_v<T>, "SafeMem::Read requires a trivially copyable type");
    T out;
    return ReadBytes(addr, &out, sizeof(T)) ? out : defaultVal;
}

template <typename T>
T ReadFast(uintptr_t addr, T defaultVal = T{}) {
    static_assert(std::is_trivially_copyable_v<T>, "SafeMem::ReadFast requires a trivially copyable type");
    T out;
    return ReadBytesFast(addr, &out, sizeof(T)) ? out : defaultVal;
}

template <typename T>
bool Write(uintptr_t addr, const T& val) {
    static_assert(std::is_trivially_copyable_v<T>, "SafeMem::Write requires a trivially copyable type");
    return WriteBytes(addr, &val, sizeof(T));
}

// Follow a pointer chain: base -> [base + o0] -> [.. + o1] -> ... -> final address.
// Returns 0 if any hop is unreadable or null.
template <size_t N>
uintptr_t ResolveChain(uintptr_t base, const uintptr_t (&offsets)[N]) {
    uintptr_t cur = base;
    for (size_t i = 0; i < N; ++i) {
        if (i + 1 == N) return cur + offsets[i];   // last offset: address, not deref
        cur = Read<uintptr_t>(cur + offsets[i]);
        if (cur == 0) return 0;
    }
    return cur;
}

} // namespace SafeMem
