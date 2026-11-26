#pragma once
// esp.h - overlay drawing state and entity feed. Declarations only; bodies in esp.cpp.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <thread>
#include <vector>

#include "imgui.h"
#include "memory.h"

namespace esp {

struct Vec2 { float x = 0.f, y = 0.f; };
struct Vec3 { float x = 0.f, y = 0.f, z = 0.f; };
struct Mat4 { float m[16] = {}; };  // row-major view-projection

enum class Role : uint8_t { Local, Teammate, Enemy, Neutral, Count };
constexpr size_t kRoleCount = static_cast<size_t>(Role::Count);

struct Entity {
    uintptr_t address = 0;
    Vec3      origin;      // feet
    Vec3      head;
    int       health    = 0;
    int       maxHealth = 100;
    Role      role      = Role::Neutral;
    bool      alive     = false;
    char      name[32]  = {};
};

struct Config {
    bool enabled      = true;
    bool box          = true;
    bool name         = true;
    bool healthBar    = true;
    bool distance     = true;
    bool tracer       = true;
    bool skipLocal    = true;
    bool skipTeammate = false;

    float boxThickness    = 1.5f;
    float tracerThickness = 1.0f;
    float maxDistance     = 0.f;  // 0 = unlimited

    bool  fovCircle = false;
    float fovRadius = 150.f;      // pixels, centered on screen

    ImVec4 roleColor[kRoleCount] = {
        ImVec4(0.30f, 0.80f, 1.00f, 1.00f),  // Local
        ImVec4(0.30f, 1.00f, 0.40f, 1.00f),  // Teammate
        ImVec4(1.00f, 0.25f, 0.25f, 1.00f),  // Enemy
        ImVec4(0.85f, 0.85f, 0.85f, 1.00f),  // Neutral
    };
};

class Esp {
public:
    explicit Esp(const mem::Process& proc);
    ~Esp();
    Esp(const Esp&) = delete;
    Esp& operator=(const Esp&) = delete;

    Config&       Cfg() noexcept { return cfg_; }
    const Config& Cfg() const noexcept { return cfg_; }

    const std::vector<Entity>& Entities() const noexcept { return entities_; }

    // Refreshes entities_ and view_ from the target; false = read failed, entities_ left empty.
    bool Update();

    // Draws one frame into the given list; screen is the overlay size in pixels.
    void Render(ImDrawList* dl, Vec2 screen) const;

    // transposed = read the matrix column-major (w row at m[3], m[7], m[11], m[15]).
    static bool WorldToScreen(const Mat4& vp, const Vec3& world, Vec2 screen, Vec2& out, bool transposed = false) noexcept;

private:
    // Background worker: owns both full-heap scans so Update() never walks the heap on the main thread.
    void ScanLoop();

    // Heap scan for the live BattleModel (first qword == its Il2CppClass*). Returns 0 if not found.
    uintptr_t ScanBattleModel() const;

    // Heap scan for a 4x4 matrix that projects `local` near screen center. Returns 0, or the address and sets transposed.
    uintptr_t ScanViewMatrix(const Vec3& local, bool& transposed) const;

    bool IsVisibleRole(Role r) const noexcept;
    ImU32 RoleColor(Role r) const noexcept;

    void DrawBox(ImDrawList* dl, Vec2 top, Vec2 bottom, ImU32 col) const;
    void DrawName(ImDrawList* dl, const Entity& e, Vec2 top, Vec2 bottom, ImU32 col) const;
    void DrawHealthBar(ImDrawList* dl, const Entity& e, Vec2 top, Vec2 bottom) const;
    void DrawDistance(ImDrawList* dl, float meters, Vec2 top, Vec2 bottom, ImU32 col) const;
    void DrawTracer(ImDrawList* dl, Vec2 screen, Vec2 bottom, ImU32 col) const;

    const mem::Process&  proc_;
    Config               cfg_;
    Mat4                 view_;
    Vec3                 localOrigin_;
    bool                 viewT_ = false;  // layout the validated matrix was found in
    std::vector<Entity>  entities_;

    // Shared with ScanLoop: the worker publishes the found addresses, the main thread consumes them and
    // invalidates (store 0) to request a rescan. uintptr_t atomics are lock-free, so no mutex on the hot path.
    std::atomic<uintptr_t> battleModelAddr_{0};
    std::atomic<uintptr_t> viewAddr_{0};
    std::atomic<bool>      viewTFound_{false};
    std::atomic<bool>      haveLocal_{false};
    std::atomic<float>     localX_{0.f}, localY_{0.f}, localZ_{0.f};
    std::atomic<bool>      running_{false};
    std::thread            scanThread_;
};

}  // namespace esp
