#pragma once

#include <algorithm>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

#include "ue_math.h"

// ---------------------------------------------------------------------------
// RenderEntity
// ---------------------------------------------------------------------------
struct RenderEntity {
    enum Flags : uint8_t {
        None         = 0,
        StickyTarget = 1 << 0,
        // room for more without changing the struct: Friendly, Boss, Visible, ...
    };

    FVector2D Feet;
    FVector2D Head;
    float     BoxWidth  = 0.0f;
    float     Health    = 0.0f;
    float     MaxHealth = 0.0f;
    float     Distance  = 0.0f;
    uint8_t   EntityFlags = None;

    // Derived box geometry
    float BoxHeight() const { return Feet.Y - Head.Y; }
    float BoxLeft()   const { return Head.X - BoxWidth * 0.5f; }
    float BoxRight()  const { return Head.X + BoxWidth * 0.5f; }
    float BoxTop()    const { return Head.Y; }
    float BoxBottom() const { return Feet.Y; }

    // 0..1, safe against MaxHealth == 0 and overheal
    float HealthPct() const {
        return (MaxHealth > 0.0f) ? std::clamp(Health / MaxHealth, 0.0f, 1.0f) : 0.0f;
    }
    bool IsAlive() const { return Health > 0.0f; }

    bool HasFlag(Flags f) const { return (EntityFlags & f) != 0; }
    void SetFlag(Flags f, bool on = true) {
        EntityFlags = on ? (EntityFlags | f) : (EntityFlags & ~f);
    }

    // Compatibility with the old bool field
    bool IsStickyTarget() const { return HasFlag(StickyTarget); }
};

// ---------------------------------------------------------------------------
// FrameSnapshot — immutable once published
// ---------------------------------------------------------------------------
struct FrameSnapshot {
    MinimalViewInfo           View;
    std::vector<RenderEntity> Entities;
    int                       ScreenWidth  = 1920;
    int                       ScreenHeight = 1080;
    uint64_t                  FrameId      = 0;

    void Clear() { Entities.clear(); }
    bool Empty() const { return Entities.empty(); }

    const RenderEntity* FindStickyTarget() const {
        for (const auto& e : Entities)
            if (e.IsStickyTarget()) return &e;
        return nullptr;
    }

    // Far-to-near so closer boxes draw on top
    void SortByDistanceDesc() {
        std::sort(Entities.begin(), Entities.end(),
                  [](const RenderEntity& a, const RenderEntity& b) { return a.Distance > b.Distance; });
    }
};

// ---------------------------------------------------------------------------
// SnapshotChannel
//
// The old `FrameSnapshot g_CurrentSnapshot;` is a data race if the game/update
// thread writes it while the render thread (Present hook, overlay) reads it —
// a std::vector reallocating mid-read is a crash, not just a glitch.
//
// Writer builds a fresh snapshot and publishes it; readers grab a shared_ptr
// to the latest one and keep it alive for as long as they're drawing.
// The lock is only held for a pointer swap, never during build or draw.
// ---------------------------------------------------------------------------
class SnapshotChannel {
public:
    using Ptr = std::shared_ptr<const FrameSnapshot>;

    // Writer side: start a new snapshot, pre-reserved to last frame's size.
    FrameSnapshot BeginFrame() const {
        FrameSnapshot s;
        s.Entities.reserve(m_LastEntityCount);
        return s;
    }

    void Publish(FrameSnapshot&& snapshot) {
        snapshot.FrameId = ++m_FrameCounter;
        m_LastEntityCount = snapshot.Entities.size();
        auto ptr = std::make_shared<const FrameSnapshot>(std::move(snapshot));

        std::lock_guard<std::mutex> lock(m_Mutex);
        m_Latest = std::move(ptr);   // old snapshot freed when last reader drops it
    }

    // Reader side: may return nullptr before the first publish.
    Ptr Acquire() const {
        std::lock_guard<std::mutex> lock(m_Mutex);
        return m_Latest;
    }

    void Reset() {
        std::lock_guard<std::mutex> lock(m_Mutex);
        m_Latest.reset();
    }

private:
    mutable std::mutex m_Mutex;
    Ptr                m_Latest;
    uint64_t           m_FrameCounter    = 0;   // writer-thread only
    size_t             m_LastEntityCount = 0;   // writer-thread only
};

inline SnapshotChannel g_Snapshots;

/*
Usage:

    // update thread
    FrameSnapshot snap = g_Snapshots.BeginFrame();
    snap.View = pov;
    snap.ScreenWidth = w; snap.ScreenHeight = h;
    ... snap.Entities.push_back(entity) ...
    snap.SortByDistanceDesc();
    g_Snapshots.Publish(std::move(snap));

    // render thread
    if (auto snap = g_Snapshots.Acquire()) {
        for (const auto& e : snap->Entities) { ... }
    }
*/
