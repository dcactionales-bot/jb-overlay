// esp.cpp - Esp bodies. C++20, MSVC /W4 /permissive-.

#include "esp.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace esp {
namespace {

constexpr float kMetersPerUnit = 0.01905f;  // world unit -> meters
constexpr float kBoxAspect     = 0.5f;      // box width / box height
constexpr float kBarWidth      = 3.0f;
constexpr float kBarGap        = 3.0f;
constexpr float kScreenMargin  = 0.0f;      // box must touch the screen rect to be drawn
constexpr ImU32 kShadow        = IM_COL32(0, 0, 0, 200);

float Dist(const Vec3& a, const Vec3& b) noexcept {
    const float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

float HalfWidth(Vec2 top, Vec2 bottom) noexcept {
    return (bottom.y - top.y) * kBoxAspect * 0.5f;
}

void ShadowText(ImDrawList* dl, ImVec2 pos, ImU32 col, const char* text) {
    dl->AddText(ImVec2(pos.x + 1.f, pos.y + 1.f), kShadow, text);
    dl->AddText(pos, col, text);
}

// Offsets: dump.cs / script.json unless tagged. FILL = not in the dump; Update() fails closed while it is 0.
namespace off {
constexpr uintptr_t kBattleModelTypeInfo  = 0x82496E8;  // script.json Game.Battle.BattleModel_TypeInfo (GameAssembly RVA)
constexpr uintptr_t kPlayerEntityTypeInfo = 0x816DCE8;  // script.json ...PlayerEntityModel_TypeInfo (GameAssembly RVA)
constexpr uintptr_t kBattleModelInstance  = 0;          // FILL: dump.cs has no static BattleModel field and no Instance/Current static reaching it (only BattleDebugModel._instance and GlobalDebugUpdater._instance, neither holds one); needs a live-instance root
constexpr uintptr_t kBattleEntities       = 0x90;       // BattleModel.<BattleEntitiesModel>
constexpr uintptr_t kModelsDict           = 0x50;       // BattleEntitiesModel._models (Dictionary<int, IEntityModel>)
constexpr uintptr_t kLocalPlayer          = 0xD0;       // BattleEntitiesModel.<Player>
constexpr uintptr_t kServerModel          = 0x20;       // PlayerEntityModel.<ServerModel>
constexpr uintptr_t kCharTransform        = 0x58;       // BattleCharacterModel.<Transform>
constexpr uintptr_t kCharPrivate          = 0x10;       // BattleCharacterModel.<PrivateModel>
constexpr uintptr_t kPrivHealth           = 0x1B0;      // BattleCharacterPrivateModel.<Health> (HealthModel)
constexpr uintptr_t kXYZ                  = 0x10;       // CharacterTransformModel X,Y,Z floats
constexpr uintptr_t kHealthData           = 0x10;       // HealthModel._health (IPrimitiveData<int>)
constexpr uintptr_t kMaxAmount            = 0x20;       // HealthModel._maxAmount
constexpr uintptr_t kPrimValue            = 0x18;       // BasePrimitiveData<int>._value: dump shows 0x0 for generics; inferred 0x10 header + _serializer(8), verify on a live object
constexpr uintptr_t kDictEntries          = 0x18;       // il2cpp Dictionary<int, ref> x64 layout
constexpr uintptr_t kDictCount            = 0x20;
constexpr uintptr_t kArrData              = 0x20;
constexpr uintptr_t kEntrySize            = 0x18;
constexpr uintptr_t kEntryValue           = 0x10;
}  // namespace off

constexpr int      kMaxEntities = 4096;
constexpr size_t   kScanChunk   = 1u << 18;  // 256 KB per ReadBuf: smaller working set, running_ checked more often
constexpr uint64_t kRescanGapMs = 2000;      // a failed scan walks the whole heap, so retry no faster than this
constexpr uint64_t kScanGapMs   = 500;       // breather between the two scans in a pass so the walk doesn't peg a core
constexpr uint32_t kRevalidateMask = 31;     // re-read+re-validate the matrix every 32nd frame, trust the cache between
constexpr float    kMatMax      = 1.0e5f;    // |element| bound for a plausible matrix
constexpr float    kMatEps      = 1.0e-4f;
constexpr float    kCenterTol   = 0.35f;     // local player must project within this fraction of screen height from center
constexpr float    kHeadHeight  = 1.75f;     // shortcut: fixed head offset above feet, swap for a bone read if needed

// Walks MEM_PRIVATE | MEM_COMMIT | PAGE_READWRITE regions in `chunk`-byte reads overlapping by `overlap` bytes.
// visit(addr, data, n) returns true to stop. Chunk bases stay 8-byte aligned while overlap is a multiple of 8.
template <class Visit>
bool ScanHeap(const mem::Process& proc, size_t chunk, size_t overlap, Visit&& visit) {
    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    const uintptr_t lo = reinterpret_cast<uintptr_t>(si.lpMinimumApplicationAddress);
    const uintptr_t hi = reinterpret_cast<uintptr_t>(si.lpMaximumApplicationAddress);

    std::vector<uint8_t> buf(chunk);
    for (uintptr_t a = lo; a < hi;) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQueryEx(proc.Handle(), reinterpret_cast<LPCVOID>(a), &mbi, sizeof mbi) || !mbi.RegionSize) return false;
        const uintptr_t base = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
        const uintptr_t end  = base + mbi.RegionSize;
        if (end <= a) return false;  // wrapped / no forward progress

        const bool scan = mbi.State == MEM_COMMIT && mbi.Type == MEM_PRIVATE &&
                          (mbi.Protect & PAGE_READWRITE) && !(mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS));
        for (uintptr_t p = scan ? base : end; p < end; p += chunk - overlap) {
            const size_t n = static_cast<size_t>(std::min<uintptr_t>(chunk, end - p));
            if (proc.ReadBuf(p, buf.data(), n) && visit(p, buf.data(), n)) return true;
            if (p + n >= end) break;
        }
        a = end;
    }
    return false;
}

// Every element finite and bounded, enough non-zero entries to rule out blank memory.
bool PlausibleMatrix(const Mat4& m) noexcept {
    int nonZero = 0;
    for (const float v : m.m) {
        if (!std::isfinite(v) || std::fabs(v) > kMatMax) return false;
        if (std::fabs(v) > kMatEps) ++nonZero;
    }
    return nonZero >= 6;
}

// Candidate passes only if the local player's body center lands near the screen center, read row-major or
// column-major; `transposed` reports which layout passed (row-major tried first).
bool ProjectsNearCenter(const Mat4& m, const Vec3& local, Vec2 screen, bool& transposed) noexcept {
    if (!PlausibleMatrix(m)) return false;
    const Vec3 mid{local.x, local.y + kHeadHeight * 0.5f, local.z};
    const float tol = kCenterTol * screen.y;
    for (const bool t : {false, true}) {
        Vec2 s;
        if (!Esp::WorldToScreen(m, mid, screen, s, t)) continue;
        const float dx = s.x - screen.x * 0.5f, dy = s.y - screen.y * 0.5f;
        if (dx * dx + dy * dy <= tol * tol) {
            transposed = t;
            return true;
        }
    }
    return false;
}

}  // namespace

Esp::Esp(const mem::Process& proc) : proc_(proc) {
    static_assert(std::atomic<uintptr_t>::is_always_lock_free, "scan results are shared without a lock");
    screen_ = {static_cast<float>(GetSystemMetrics(SM_CXSCREEN)), static_cast<float>(GetSystemMetrics(SM_CYSCREEN))};
    running_.store(true);
    scanThread_ = std::thread(&Esp::ScanLoop, this);
}

Esp::~Esp() {
    running_.store(false);  // stop the producer, then join before proc_ (declared after esp in main) dies
    if (scanThread_.joinable()) scanThread_.join();
}

// Spins both full-heap scans off the main loop. Each runs only while its published address is 0, so a
// main-thread invalidation (store 0) is what asks for a rescan. Once both are resolved the thread returns
// so it costs nothing; EnsureScanThread re-arms it if the main thread later zeroes an address.
void Esp::ScanLoop() {
    while (running_.load()) {
        bool walked = false;

        if (battleModelAddr_.load() == 0) {
            if (const uintptr_t bm = ScanBattleModel()) battleModelAddr_.store(bm);
            walked = true;
        }

        if (walked) std::this_thread::sleep_for(std::chrono::milliseconds(kScanGapMs));  // breather between the two scans

        if (running_.load() && viewAddr_.load() == 0 && haveLocal_.load()) {
            const Vec3 local{localX_.load(), localY_.load(), localZ_.load()};
            bool t = false;
            if (const uintptr_t va = ScanViewMatrix(local, t)) {
                viewTFound_.store(t);
                viewAddr_.store(va);  // published last: a nonzero viewAddr_ implies viewTFound_ is already set
            }
            walked = true;
        }

        if (battleModelAddr_.load() && viewAddr_.load()) {  // both found -> stop the worker completely
            scanDone_.store(true);
            return;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(walked ? kRescanGapMs : 50));
    }
}

// Called from Update() after it invalidates an address: if the worker had already finished, join the dead
// thread and start a fresh one so the zeroed address gets rescanned. No-op while the worker is still running.
void Esp::EnsureScanThread() {
    if (scanDone_.load() && scanThread_.joinable()) {
        scanThread_.join();
        scanDone_.store(false);
        scanThread_ = std::thread(&Esp::ScanLoop, this);
    }
}

uintptr_t Esp::ScanBattleModel() const {
    const auto ga = proc_.ModuleBase(L"GameAssembly.dll");
    if (!ga) return 0;
    const uintptr_t klass = proc_.Read<uintptr_t>(*ga + off::kBattleModelTypeInfo).value_or(0);
    if (!klass) return 0;

    // Candidate must still carry the class pointer and reach a non-null _models dictionary.
    const auto valid = [&](uintptr_t obj) {
        if (proc_.Read<uintptr_t>(obj).value_or(0) != klass) return false;
        const uintptr_t world = proc_.Read<uintptr_t>(obj + off::kBattleEntities).value_or(0);
        return world && proc_.Read<uintptr_t>(world + off::kModelsDict).value_or(0);
    };

    uintptr_t found = 0;
    ScanHeap(proc_, kScanChunk, 0, [&](uintptr_t addr, const uint8_t* d, size_t n) {
        if (!running_.load()) return true;  // teardown: stop the walk so the join doesn't wait it out (found stays 0)
        for (size_t i = 0; i + sizeof(uintptr_t) <= n; i += sizeof(uintptr_t)) {
            uintptr_t q;
            std::memcpy(&q, d + i, sizeof q);
            if (q == klass && valid(addr + i)) {
                found = addr + i;
                return true;
            }
        }
        return false;
    });
    return found;
}

uintptr_t Esp::ScanViewMatrix(const Vec3& local, bool& transposed) const {
    const Vec2 screen = screen_;

    uintptr_t found  = 0;
    bool      foundT = false;
    // Chunks overlap by one matrix so a candidate straddling a boundary is not lost.
    ScanHeap(proc_, kScanChunk, sizeof(Mat4), [&](uintptr_t addr, const uint8_t* d, size_t n) {
        if (!running_.load()) return true;  // teardown: stop the walk so the join doesn't wait it out (found stays 0)
        for (size_t i = 0; i + sizeof(Mat4) <= n; i += sizeof(float)) {
            Mat4 m;
            std::memcpy(&m, d + i, sizeof m);
            bool t = false;
            if (!ProjectsNearCenter(m, local, screen, t)) continue;
            found  = addr + i;
            foundT = t;
            return true;
        }
        return false;
    });
    transposed = foundT;
    return found;
}

bool Esp::Update() {
    entities_.clear();
    localOrigin_ = {};

    const auto ga = proc_.ModuleBase(L"GameAssembly.dll");
    if (!ga) return false;

    const auto rd = [this](uintptr_t a) -> uintptr_t { return a ? proc_.Read<uintptr_t>(a).value_or(0) : 0; };

    const uintptr_t battle = battleModelAddr_.load();
    if (!battle) return false;  // worker still hunting the model

    const uintptr_t world   = rd(battle + off::kBattleEntities);
    const uintptr_t dict    = rd(world + off::kModelsDict);
    const uintptr_t playerK = rd(*ga + off::kPlayerEntityTypeInfo);
    const uintptr_t local   = rd(world + off::kLocalPlayer);
    const uintptr_t entries = rd(dict + off::kDictEntries);
    if (!world || !dict || !playerK || !entries) {
        battleModelAddr_.store(0);  // stale -> worker rescans
        EnsureScanThread();
        return false;
    }

    const auto count = proc_.Read<int32_t>(dict + off::kDictCount);
    if (!count || *count <= 0 || *count > kMaxEntities) return false;

    bool haveLocal = false;
    entities_.reserve(static_cast<size_t>(*count));
    for (int i = 0; i < *count; ++i) {
        const uintptr_t slot = entries + off::kArrData + static_cast<uintptr_t>(i) * off::kEntrySize;
        const uintptr_t ent  = rd(slot + off::kEntryValue);
        if (!ent || rd(ent) != playerK) continue;  // Il2CppObject::klass filters non-player entities

        const uintptr_t chr  = rd(ent + off::kServerModel);
        const uintptr_t xf   = rd(chr + off::kCharTransform);
        const uintptr_t priv = rd(chr + off::kCharPrivate);
        const uintptr_t hp   = priv ? rd(priv + off::kPrivHealth) : 0;  // null private -> bail, don't read a fixed low addr
        const uintptr_t hd   = hp ? rd(hp + off::kHealthData) : 0;
        if (!chr || !xf || !hp || !hd) continue;

        const auto pos = proc_.Read<Vec3>(xf + off::kXYZ);
        const auto cur = proc_.Read<int32_t>(hd + off::kPrimValue);
        const auto max = proc_.Read<int32_t>(hp + off::kMaxAmount);
        if (!pos || !cur || !max) continue;

        Entity e;
        e.address   = ent;
        e.origin    = *pos;
        e.head      = {pos->x, pos->y + kHeadHeight, pos->z};
        e.health    = *cur;
        e.maxHealth = *max;
        e.alive     = *cur > 0;
        e.role      = (ent == local) ? Role::Local : Role::Enemy;  // shortcut: no team read, upgrade once a team field is mapped
        if (e.role == Role::Local) {
            localOrigin_ = e.origin;
            haveLocal = true;
        }
        entities_.push_back(e);
    }

    // The matrix is validated against the local player, so it can only be resolved once the local entity is read.
    if (!haveLocal) {
        entities_.clear();
        return false;
    }

    // Hand the worker the origin it validates matrix candidates against (a slightly torn read only nudges the search tolerance).
    localX_.store(localOrigin_.x);
    localY_.store(localOrigin_.y);
    localZ_.store(localOrigin_.z);
    haveLocal_.store(true);

    const uintptr_t va = viewAddr_.load();
    if (!va) {
        entities_.clear();
        return false;  // worker still hunting the matrix
    }

    // Re-validate the matrix address only every 32nd frame; it rarely drifts and the read is the hot cost.
    // shortcut: a drift goes uncaught for up to 32 frames (brief stale boxes), re-read sooner if that shows.
    if ((frameCount_++ & kRevalidateMask) == 0 || view_.m[15] == 0.f) {
        const auto vp = proc_.Read<Mat4>(va);
        bool t = viewTFound_.load();
        if (!vp || !ProjectsNearCenter(*vp, localOrigin_, screen_, t)) {
            viewAddr_.store(0);  // drifted -> worker rescans
            EnsureScanThread();
            entities_.clear();
            return false;
        }
        view_  = *vp;
        viewT_ = t;
    }
    return true;
}

bool Esp::WorldToScreen(const Mat4& vp, const Vec3& w, Vec2 screen, Vec2& out, bool transposed) noexcept {
    const float* m = vp.m;
    const auto at = [m, transposed](int r, int c) { return transposed ? m[c * 4 + r] : m[r * 4 + c]; };

    const float clipW = at(3, 0) * w.x + at(3, 1) * w.y + at(3, 2) * w.z + at(3, 3);
    if (!(clipW > 0.001f)) return false;  // behind the camera (also rejects NaN)

    const float nx = (at(0, 0) * w.x + at(0, 1) * w.y + at(0, 2) * w.z + at(0, 3)) / clipW;
    const float ny = (at(1, 0) * w.x + at(1, 1) * w.y + at(1, 2) * w.z + at(1, 3)) / clipW;
    if (!std::isfinite(nx) || !std::isfinite(ny)) return false;

    out.x = screen.x * 0.5f * (1.f + nx);
    out.y = screen.y * 0.5f * (1.f - ny);
    return true;
}

bool Esp::IsVisibleRole(Role r) const noexcept {
    switch (r) {
        case Role::Local:    return !cfg_.skipLocal;
        case Role::Teammate: return !cfg_.skipTeammate;
        default:             return true;
    }
}

ImU32 Esp::RoleColor(Role r) const noexcept {
    const size_t i = static_cast<size_t>(r);
    return ImGui::ColorConvertFloat4ToU32(cfg_.roleColor[i < kRoleCount ? i : static_cast<size_t>(Role::Neutral)]);
}

void Esp::Render(ImDrawList* dl, Vec2 screen) const {
    if (!dl || !cfg_.enabled || screen.x <= 0.f || screen.y <= 0.f) return;

    if (cfg_.fovCircle)
        dl->AddCircle(ImVec2(screen.x * 0.5f, screen.y * 0.5f), cfg_.fovRadius, IM_COL32(255, 255, 255, 160), 64, 1.f);

    for (const Entity& e : entities_) {
        if (!e.alive || e.health <= 0) continue;
        if (!IsVisibleRole(e.role)) continue;

        const float meters = Dist(localOrigin_, e.origin) * kMetersPerUnit;
        if (cfg_.maxDistance > 0.f && meters > cfg_.maxDistance) continue;

        Vec2 top, bottom;
        if (!WorldToScreen(view_, e.head, screen, top, viewT_)) continue;
        if (!WorldToScreen(view_, e.origin, screen, bottom, viewT_)) continue;

        const float h = bottom.y - top.y;
        if (!(h > 1.f)) continue;  // degenerate / upside-down

        const float hw = HalfWidth(top, bottom);
        const float cx = (top.x + bottom.x) * 0.5f;
        if (cx + hw < -kScreenMargin || cx - hw > screen.x + kScreenMargin ||
            bottom.y < -kScreenMargin || top.y > screen.y + kScreenMargin)
            continue;  // fully off-screen

        const ImU32 col = RoleColor(e.role);
        top.x = bottom.x = cx;

        if (cfg_.tracer)    DrawTracer(dl, screen, bottom, col);
        if (cfg_.box)       DrawBox(dl, top, bottom, col);
        if (cfg_.healthBar) DrawHealthBar(dl, e, top, bottom);
        if (cfg_.name)      DrawName(dl, e, top, bottom, col);
        if (cfg_.distance)  DrawDistance(dl, meters, top, bottom, col);
    }
}

void Esp::DrawBox(ImDrawList* dl, Vec2 top, Vec2 bottom, ImU32 col) const {
    const float hw = HalfWidth(top, bottom);
    const ImVec2 a(top.x - hw, top.y), b(top.x + hw, bottom.y);
    dl->AddRect(ImVec2(a.x - 1.f, a.y - 1.f), ImVec2(b.x + 1.f, b.y + 1.f), kShadow, 0.f, 0, cfg_.boxThickness + 1.f);
    dl->AddRect(a, b, col, 0.f, 0, cfg_.boxThickness);
}

void Esp::DrawName(ImDrawList* dl, const Entity& e, Vec2 top, Vec2 /*bottom*/, ImU32 col) const {
    if (e.name[0] == '\0') return;
    const ImVec2 sz = ImGui::CalcTextSize(e.name);
    ShadowText(dl, ImVec2(top.x - sz.x * 0.5f, top.y - sz.y - 2.f), col, e.name);
}

void Esp::DrawHealthBar(ImDrawList* dl, const Entity& e, Vec2 top, Vec2 bottom) const {
    const float hw = HalfWidth(top, bottom);
    const float maxHp = static_cast<float>(e.maxHealth > 0 ? e.maxHealth : 100);
    const float frac = std::clamp(static_cast<float>(e.health) / maxHp, 0.f, 1.f);

    const float x1 = top.x - hw - kBarGap;
    const float x0 = x1 - kBarWidth;
    const float h = bottom.y - top.y;
    const float fillTop = bottom.y - h * frac;

    const ImU32 fill = IM_COL32(static_cast<int>((1.f - frac) * 255.f), static_cast<int>(frac * 255.f), 40, 255);
    dl->AddRectFilled(ImVec2(x0 - 1.f, top.y - 1.f), ImVec2(x1 + 1.f, bottom.y + 1.f), kShadow);
    dl->AddRectFilled(ImVec2(x0, fillTop), ImVec2(x1, bottom.y), fill);
}

void Esp::DrawDistance(ImDrawList* dl, float meters, Vec2 /*top*/, Vec2 bottom, ImU32 col) const {
    char buf[16];
    std::snprintf(buf, sizeof buf, "%.0fm", meters);
    const ImVec2 sz = ImGui::CalcTextSize(buf);
    ShadowText(dl, ImVec2(bottom.x - sz.x * 0.5f, bottom.y + 2.f), col, buf);
}

void Esp::DrawTracer(ImDrawList* dl, Vec2 screen, Vec2 bottom, ImU32 col) const {
    dl->AddLine(ImVec2(screen.x * 0.5f, screen.y), ImVec2(bottom.x, bottom.y), col, cfg_.tracerThickness);
}

}  // namespace esp
