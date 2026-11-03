#pragma once

#include <cmath>
#include <cstdint>

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------
namespace UEMath {

inline constexpr float Pi                 = 3.14159265358979323846f;
inline constexpr float SmallNumber        = 1.e-8f;
inline constexpr float KindaSmallNumber   = 1.e-4f;

inline constexpr float URotationToRadians = Pi / 32768.0f;
inline constexpr float RadiansToURotation = 32768.0f / Pi;
inline constexpr float URotationToDegrees = 360.0f / 65536.0f;
inline constexpr float DegreesToURotation = 65536.0f / 360.0f;
inline constexpr float RadiansToDegrees   = 180.0f / Pi;
inline constexpr float DegreesToRadians   = Pi / 180.0f;

// Wrap a UE3 rotation component into [-32768, 32767].
constexpr int32_t NormalizeAxis(int32_t a) {
    a &= 0xFFFF;
    return (a > 32767) ? a - 65536 : a;
}

} // namespace UEMath

// ---------------------------------------------------------------------------
// FVector
// ---------------------------------------------------------------------------
struct FVector {
    float X = 0.0f, Y = 0.0f, Z = 0.0f;

    constexpr FVector() = default;
    constexpr FVector(float x, float y, float z) : X(x), Y(y), Z(z) {}
    constexpr explicit FVector(float s) : X(s), Y(s), Z(s) {}

    // Arithmetic
    constexpr FVector operator+(const FVector& v) const { return { X + v.X, Y + v.Y, Z + v.Z }; }
    constexpr FVector operator-(const FVector& v) const { return { X - v.X, Y - v.Y, Z - v.Z }; }
    constexpr FVector operator*(const FVector& v) const { return { X * v.X, Y * v.Y, Z * v.Z }; }
    constexpr FVector operator*(float s)          const { return { X * s, Y * s, Z * s }; }
    constexpr FVector operator/(float s)          const { return (s != 0.0f) ? FVector(X / s, Y / s, Z / s) : FVector(); }
    constexpr FVector operator-()                 const { return { -X, -Y, -Z }; }

    FVector& operator+=(const FVector& v) { X += v.X; Y += v.Y; Z += v.Z; return *this; }
    FVector& operator-=(const FVector& v) { X -= v.X; Y -= v.Y; Z -= v.Z; return *this; }
    FVector& operator*=(float s)          { X *= s; Y *= s; Z *= s; return *this; }
    FVector& operator/=(float s)          { return *this = *this / s; }

    constexpr bool operator==(const FVector& v) const { return X == v.X && Y == v.Y && Z == v.Z; }
    constexpr bool operator!=(const FVector& v) const { return !(*this == v); }

    // Products
    constexpr float   Dot(const FVector& v)   const { return X * v.X + Y * v.Y + Z * v.Z; }
    constexpr FVector Cross(const FVector& v) const {
        return { Y * v.Z - Z * v.Y, Z * v.X - X * v.Z, X * v.Y - Y * v.X };
    }

    // Length / distance
    constexpr float SizeSquared()   const { return Dot(*this); }
    constexpr float SizeSquared2D() const { return X * X + Y * Y; }
    float Size()   const { return std::sqrt(SizeSquared()); }
    float Size2D() const { return std::sqrt(SizeSquared2D()); }

    constexpr float DistSquared(const FVector& v) const { return (*this - v).SizeSquared(); }
    float Distance(const FVector& v)   const { return (*this - v).Size(); }
    float Distance2D(const FVector& v) const { return (*this - v).Size2D(); }

    // Direction
    bool IsZero()       const { return X == 0.0f && Y == 0.0f && Z == 0.0f; }
    bool IsNearlyZero(float tol = UEMath::KindaSmallNumber) const {
        return std::fabs(X) <= tol && std::fabs(Y) <= tol && std::fabs(Z) <= tol;
    }

    FVector GetSafeNormal(float tol = UEMath::SmallNumber) const {
        const float sq = SizeSquared();
        return (sq > tol) ? *this * (1.0f / std::sqrt(sq)) : FVector();
    }

    bool Normalize(float tol = UEMath::SmallNumber) {
        const float sq = SizeSquared();
        if (sq <= tol) return false;
        *this *= 1.0f / std::sqrt(sq);
        return true;
    }

    static constexpr FVector Zero()    { return {}; }
    static constexpr FVector One()     { return { 1.0f, 1.0f, 1.0f }; }
    static constexpr FVector Forward() { return { 1.0f, 0.0f, 0.0f }; }
    static constexpr FVector Right()   { return { 0.0f, 1.0f, 0.0f }; }
    static constexpr FVector Up()      { return { 0.0f, 0.0f, 1.0f }; }

    struct FRotator ToRotator() const; // defined below FRotator
};

constexpr FVector operator*(float s, const FVector& v) { return v * s; }

// ---------------------------------------------------------------------------
// FRotator (UE3 integer units: 65536 = full turn)
// ---------------------------------------------------------------------------
struct FRotator {
    int32_t Pitch = 0, Yaw = 0, Roll = 0;

    constexpr FRotator() = default;
    constexpr FRotator(int32_t p, int32_t y, int32_t r) : Pitch(p), Yaw(y), Roll(r) {}

    static FRotator FromDegrees(float p, float y, float r) {
        using namespace UEMath;
        return { static_cast<int32_t>(p * DegreesToURotation),
                 static_cast<int32_t>(y * DegreesToURotation),
                 static_cast<int32_t>(r * DegreesToURotation) };
    }

    constexpr FRotator operator+(const FRotator& r) const { return { Pitch + r.Pitch, Yaw + r.Yaw, Roll + r.Roll }; }
    constexpr FRotator operator-(const FRotator& r) const { return { Pitch - r.Pitch, Yaw - r.Yaw, Roll - r.Roll }; }
    constexpr bool operator==(const FRotator& r) const { return Pitch == r.Pitch && Yaw == r.Yaw && Roll == r.Roll; }
    constexpr bool operator!=(const FRotator& r) const { return !(*this == r); }

    // Wrap every axis into [-32768, 32767].
    constexpr FRotator GetNormalized() const {
        using UEMath::NormalizeAxis;
        return { NormalizeAxis(Pitch), NormalizeAxis(Yaw), NormalizeAxis(Roll) };
    }
    void Normalize() { *this = GetNormalized(); }

    // Per-axis conversions
    float PitchDeg() const { return static_cast<float>(UEMath::NormalizeAxis(Pitch)) * UEMath::URotationToDegrees; }
    float YawDeg()   const { return static_cast<float>(UEMath::NormalizeAxis(Yaw))   * UEMath::URotationToDegrees; }
    float RollDeg()  const { return static_cast<float>(UEMath::NormalizeAxis(Roll))  * UEMath::URotationToDegrees; }

    struct Axes { FVector X, Y, Z; };

    Axes GetAxes() const {
        using UEMath::URotationToRadians;
        const float radPitch = static_cast<float>(Pitch) * URotationToRadians;
        const float radYaw   = static_cast<float>(Yaw)   * URotationToRadians;
        const float radRoll  = static_cast<float>(Roll)  * URotationToRadians;

        const float SP = std::sin(radPitch), CP = std::cos(radPitch);
        const float SY = std::sin(radYaw),   CY = std::cos(radYaw);
        const float SR = std::sin(radRoll),  CR = std::cos(radRoll);

        return {
            { CP * CY, CP * SY, SP },
            { SR * SP * CY - CR * SY,  SR * SP * SY + CR * CY, -SR * CP },
            { -(CR * SP * CY) - SR * SY, -(CR * SP * SY) + SR * CY, CR * CP }
        };
    }

    // Original out-param signature kept for drop-in compatibility.
    void GetAxes(FVector& x, FVector& y, FVector& z) const {
        const Axes a = GetAxes();
        x = a.X; y = a.Y; z = a.Z;
    }

    // Forward direction only (cheaper than full axes).
    FVector ToVector() const {
        using UEMath::URotationToRadians;
        const float radPitch = static_cast<float>(Pitch) * URotationToRadians;
        const float radYaw   = static_cast<float>(Yaw)   * URotationToRadians;
        const float CP = std::cos(radPitch);
        return { CP * std::cos(radYaw), CP * std::sin(radYaw), std::sin(radPitch) };
    }
};

inline FRotator FVector::ToRotator() const {
    using UEMath::RadiansToURotation;
    return { static_cast<int32_t>(std::atan2(Z, Size2D()) * RadiansToURotation),
             static_cast<int32_t>(std::atan2(Y, X)        * RadiansToURotation),
             0 };
}

// ---------------------------------------------------------------------------
// FVector2D
// ---------------------------------------------------------------------------
struct FVector2D {
    float X = 0.0f, Y = 0.0f;

    constexpr FVector2D() = default;
    constexpr FVector2D(float x, float y) : X(x), Y(y) {}

    constexpr FVector2D operator+(const FVector2D& v) const { return { X + v.X, Y + v.Y }; }
    constexpr FVector2D operator-(const FVector2D& v) const { return { X - v.X, Y - v.Y }; }
    constexpr FVector2D operator*(float s)            const { return { X * s, Y * s }; }
    constexpr FVector2D operator/(float s)            const { return (s != 0.0f) ? FVector2D(X / s, Y / s) : FVector2D(); }

    FVector2D& operator+=(const FVector2D& v) { X += v.X; Y += v.Y; return *this; }
    FVector2D& operator-=(const FVector2D& v) { X -= v.X; Y -= v.Y; return *this; }

    constexpr bool operator==(const FVector2D& v) const { return X == v.X && Y == v.Y; }
    constexpr bool operator!=(const FVector2D& v) const { return !(*this == v); }

    constexpr float Dot(const FVector2D& v) const { return X * v.X + Y * v.Y; }
    constexpr float SizeSquared() const { return Dot(*this); }
    float Size() const { return std::sqrt(SizeSquared()); }
    float Distance(const FVector2D& v) const { return (*this - v).Size(); }
};

// ---------------------------------------------------------------------------
// TArray64 — raw remote array header (Data is an address, not a local pointer)
// ---------------------------------------------------------------------------
struct TArray64 {
    uintptr_t Data  = 0;
    int32_t   Count = 0;
    int32_t   Max   = 0;

    constexpr bool    IsValid() const { return Data != 0 && Count > 0 && Count <= Max; }
    constexpr bool    IsValidIndex(int32_t i) const { return i >= 0 && i < Count; }
    constexpr int32_t Num() const { return Count; }

    // Address of element i, given the element stride in bytes.
    constexpr uintptr_t ElementAddress(int32_t i, size_t stride) const {
        return Data + static_cast<uintptr_t>(i) * stride;
    }
    template <typename T>
    constexpr uintptr_t ElementAddress(int32_t i) const { return ElementAddress(i, sizeof(T)); }
};

// ---------------------------------------------------------------------------
// MinimalViewInfo
// ---------------------------------------------------------------------------
struct MinimalViewInfo {
    FVector  Location;
    FRotator Rotation;
    float    FOV = 90.0f;
};

// ---------------------------------------------------------------------------
// Layout guards — these structs mirror external memory, so sizes must not drift.
// ---------------------------------------------------------------------------
static_assert(sizeof(FVector)         == 12, "FVector layout changed");
static_assert(sizeof(FRotator)        == 12, "FRotator layout changed");
static_assert(sizeof(FVector2D)       ==  8, "FVector2D layout changed");
static_assert(sizeof(TArray64)        == (sizeof(uintptr_t) == 8 ? 16 : 12), "TArray64 layout changed");
static_assert(sizeof(MinimalViewInfo) == 28, "MinimalViewInfo layout changed");
