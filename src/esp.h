#pragma once
#include "memory.h"
#include "offsets.h"
#include "imgui.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace esp {

struct Vec2 { float x, y; };
struct Vec3 { float x, y, z; };
struct Mat4 { float m[16]; };

enum class Role : int { Local = 0, Enemy, Teammate, Neutral };
constexpr size_t kRoleCount = 4;

struct Entity {
    uintptr_t address = 0;
    int  id = -1;
    Vec3 origin{}, head{};
    Vec3 velocity{};
    int  health = 0, maxHealth = 100;
    bool alive = true;
    Role role = Role::Neutral;
    char name[32]{};
};

// ---- ESP config ----
struct Config {
    bool  enabled       = true;
    int   boxStyle      = 1;     // 0=off, 1=full, 2=corner, 3=filled
    bool  healthBar     = true;
    bool  healthText    = false;
    bool  distance      = true;
    bool  tracer        = false;
    int   tracerPos     = 2;     // 0=top, 1=center, 2=bottom
    bool  name          = false;
    bool  headDot       = true;
    bool  skeleton      = false;
    bool  fovCircle     = false;
    bool  skipLocal     = true;
    bool  skipTeammate  = false;
    bool  skipDead      = true;
    float maxDistance    = 0.f;
    float fovRadius     = 200.f;
    float boxThickness  = 1.5f;
    float tracerThickness = 1.f;
    ImVec4 roleColor[kRoleCount] = {
        {0.f, 1.f, 0.f, 1.f},
        {1.f, 0.3f, 0.3f, 1.f},
        {0.3f, 0.5f, 1.f, 1.f},
        {1.f, 1.f, 1.f, 1.f},
    };
};

// ---- Aimbot config ----
struct AimConfig {
    bool  enabled     = false;
    float fov         = 120.f;
    float smooth      = 6.f;
    float smoothNear  = 12.f;
    int   bone        = 0;       // 0=head, 1=neck, 2=chest
    int   key         = 2;       // VK_RBUTTON
    bool  drawFov     = true;
    bool  bezier      = true;
    bool  stickyAim   = true;
    bool  prediction  = true;
    float predScale   = 0.5f;
    int   priority    = 0;       // 0=crosshair, 1=distance, 2=lowHP
    float aimDelay    = 0.05f;
    float overshoot   = 0.15f;
    float jitter      = 1.2f;
    float maxSpeed    = 40.f;
    bool  triggerbot    = false;
    float triggerDelay  = 0.08f;
    float triggerRadius = 8.f;
};

// ---- Misc config ----
struct MiscConfig {
    bool  crosshair      = true;
    int   crosshairStyle = 1;
    float crosshairSize  = 5.f;
    float crosshairThick = 1.5f;
    ImVec4 crosshairColor = {0.f, 1.f, 0.f, 1.f};
    bool  watermark      = true;
    bool  fpsCounter     = true;
};

// ---- World config ----
struct WorldConfig {
    bool  radar        = true;
    float radarSize    = 120.f;
    float radarRange   = 80.f;
    float radarX       = 10.f;
    float radarY       = 50.f;
    bool  proximity    = true;
    float proximityDist = 15.f;
    ImVec4 proximityColor = {1.f, 0.f, 0.f, 0.15f};
};

class Esp {
public:
    explicit Esp(const mem::Process& proc, cfg::Offsets& off);
    ~Esp();

    bool Update();
    void ResetScan();
    void Render(ImDrawList* dl, Vec2 screen) const;
    void RenderAim(Vec2 screen);
    void RenderMisc(ImDrawList* dl, Vec2 screen, float fps) const;
    void RenderWorld(ImDrawList* dl, Vec2 screen) const;
    void RenderStatus(ImDrawList* dl, Vec2 screen, bool attached, bool active) const;
    void RenderMenu();

    bool SaveSettings(const std::string& path) const;
    bool LoadSettings(const std::string& path);

    Config&      Cfg()      { return cfg_; }
    AimConfig&   AimCfg()   { return aimCfg_; }
    MiscConfig&  MiscCfg()  { return miscCfg_; }
    WorldConfig& WorldCfg() { return worldCfg_; }

    static bool WorldToScreen(const Mat4& vp, const Vec3& w, Vec2 screen, Vec2& out, bool transposed) noexcept;

private:
    void      ScanLoop();
    void      EnsureScanThread();
    uintptr_t ScanGameManager() const;
    uintptr_t ScanGameManagerByName() const;
    uintptr_t ScanViewMatrix(const Vec3& local, bool& transposed) const;

    bool  IsVisibleRole(Role r) const noexcept;
    ImU32 RoleColor(Role r) const noexcept;
    void  DrawBox(ImDrawList* dl, Vec2 top, Vec2 bottom, ImU32 col) const;
    void  DrawCornerBox(ImDrawList* dl, Vec2 top, Vec2 bottom, ImU32 col) const;
    void  DrawFilledBox(ImDrawList* dl, Vec2 top, Vec2 bottom, ImU32 col) const;
    void  DrawName(ImDrawList* dl, const Entity& e, Vec2 top, Vec2 bottom, ImU32 col) const;
    void  DrawHealthBar(ImDrawList* dl, const Entity& e, Vec2 top, Vec2 bottom) const;
    void  DrawDistance(ImDrawList* dl, float meters, Vec2 top, Vec2 bottom, ImU32 col) const;
    void  DrawTracer(ImDrawList* dl, Vec2 screen, Vec2 bottom, ImU32 col) const;
    void  DrawHeadDot(ImDrawList* dl, Vec2 head, ImU32 col) const;
    void  DrawSkeleton(ImDrawList* dl, Vec2 head, Vec2 bottom, ImU32 col) const;

    int  SelectTarget(Vec2 center) const;

    const mem::Process& proc_;
    cfg::Offsets& off_;

    Config      cfg_;
    AimConfig   aimCfg_;
    MiscConfig  miscCfg_;
    WorldConfig worldCfg_;

    Vec2 screen_{};
    Mat4 view_{};
    bool viewT_ = false;

    std::vector<Entity> entities_;
    Vec3 localOrigin_{};
    uint32_t frameCount_ = 0;

    // velocity tracking
    std::unordered_map<uintptr_t, Vec3> prevPos_;
    std::chrono::steady_clock::time_point lastUpdate_{};

    // aim state
    uintptr_t aimTargetAddr_ = 0;
    float     aimProgress_   = 0.f;
    std::chrono::steady_clock::time_point aimLockTime_{};
    std::chrono::steady_clock::time_point triggerStart_{};
    bool      triggerArmed_ = false;

    std::thread       scanThread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> scanDone_{false};
    std::atomic<uintptr_t> gmAddr_{0};
    std::atomic<uintptr_t> viewAddr_{0};
    std::atomic<bool>      viewTFound_{false};
    std::atomic<bool>      haveLocal_{false};
    std::atomic<float>     localX_{0}, localY_{0}, localZ_{0};
};

}  // namespace esp
