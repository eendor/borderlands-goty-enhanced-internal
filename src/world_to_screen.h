#pragma once

#include <cmath>

#include "ue_math.h"

// ---------------------------------------------------------------------------
// ViewProjector
//
// Build once per frame from the camera, then call Project() for every point.
// The old WorldToScreen redid GetAxes (6 sin/cos) and tan() on every call;
// with dozens of entities × several bones each, that adds up fast.
// ---------------------------------------------------------------------------
class ViewProjector {
public:
    static constexpr float kNearPlane     = 1.0f;
    static constexpr float kDefaultMargin = 500.0f;

    ViewProjector() = default;
    ViewProjector(const MinimalViewInfo& pov, int screenWidth, int screenHeight) {
        Update(pov, screenWidth, screenHeight);
    }

    // Returns false if the camera data is unusable (bad FOV / zero-size screen).
    bool Update(const MinimalViewInfo& pov, int screenWidth, int screenHeight) {
        m_Valid = false;
        if (screenWidth <= 0 || screenHeight <= 0) return false;
        if (!(pov.FOV > 0.0f && pov.FOV < 180.0f)) return false;   // also rejects NaN

        const FRotator::Axes axes = pov.Rotation.GetAxes();
        m_Forward = axes.X;
        m_Right   = axes.Y;
        m_Up      = axes.Z;
        m_Origin  = pov.Location;

        m_Width      = static_cast<float>(screenWidth);
        m_Height     = static_cast<float>(screenHeight);
        m_HalfWidth  = m_Width  * 0.5f;
        m_HalfHeight = m_Height * 0.5f;

        // UE uses horizontal FOV, so both axes scale by halfWidth.
        const float tanHalfFov = std::tan(pov.FOV * 0.5f * UEMath::DegreesToRadians);
        if (tanHalfFov <= 0.0f) return false;
        m_Scale = m_HalfWidth / tanHalfFov;

        m_Valid = true;
        return true;
    }

    bool IsValid() const { return m_Valid; }

    // Projects worldLoc. Returns false only if the point is behind the camera
    // (or the projector is invalid) — outScreen is filled for any point in front,
    // even if it's off-screen, so you can still clamp/draw edge indicators.
    bool Project(const FVector& worldLoc, FVector2D& outScreen, float* outDepth = nullptr) const {
        if (!m_Valid) return false;

        const FVector delta = worldLoc - m_Origin;
        const float depth = delta.Dot(m_Forward);
        if (depth < kNearPlane) return false;

        const float invDepth = m_Scale / depth;
        outScreen.X = m_HalfWidth  + delta.Dot(m_Right) * invDepth;
        outScreen.Y = m_HalfHeight - delta.Dot(m_Up)    * invDepth;

        if (outDepth) *outDepth = depth;
        return true;
    }

    bool IsOnScreen(const FVector2D& p, float margin = 0.0f) const {
        return p.X >= -margin && p.X <= m_Width  + margin &&
               p.Y >= -margin && p.Y <= m_Height + margin;
    }

    // Project + on-screen check in one call (matches the old behaviour).
    bool ProjectVisible(const FVector& worldLoc, FVector2D& outScreen, float margin = kDefaultMargin) const {
        return Project(worldLoc, outScreen) && IsOnScreen(outScreen, margin);
    }

    // Distance from screen centre, handy for "closest to crosshair" sorting.
    float DistanceFromCenter(const FVector2D& p) const {
        return p.Distance({ m_HalfWidth, m_HalfHeight });
    }

    const FVector& Origin()  const { return m_Origin; }
    const FVector& Forward() const { return m_Forward; }
    float Width()  const { return m_Width; }
    float Height() const { return m_Height; }

private:
    FVector m_Origin, m_Forward, m_Right, m_Up;
    float   m_Width = 0.0f, m_Height = 0.0f;
    float   m_HalfWidth = 0.0f, m_HalfHeight = 0.0f;
    float   m_Scale = 0.0f;
    bool    m_Valid = false;
};

// ---------------------------------------------------------------------------
// Drop-in wrapper with the original signature (fine for one-off calls;
// use ViewProjector directly inside per-frame loops).
// ---------------------------------------------------------------------------
inline bool WorldToScreen(const FVector& worldLoc, const MinimalViewInfo& pov,
                          int screenWidth, int screenHeight, FVector2D& outScreen) {
    const ViewProjector proj(pov, screenWidth, screenHeight);
    return proj.ProjectVisible(worldLoc, outScreen);
}
