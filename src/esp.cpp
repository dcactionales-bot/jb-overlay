#include "esp.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <random>
#include <sstream>
#include <unordered_set>
#include <vector>

namespace esp {
namespace {

// ============================================================
// XOR string encryption
// ============================================================

constexpr wchar_t xkey = 0x4B2F;
template <size_t N>
struct XW {
    wchar_t d[N];
    constexpr XW(const wchar_t (&s)[N]) { for (size_t i = 0; i < N; i++) d[i] = s[i] ^ xkey; }
    struct Buf { wchar_t s[N]; };
    Buf dec() const { Buf b; for (size_t i = 0; i < N; i++) b.s[i] = d[i] ^ xkey; return b; }
};
static constexpr XW xGA{L"GameAssembly.dll"};

constexpr char ckey = 0x5A;
template <size_t N>
struct XA {
    char d[N];
    constexpr XA(const char (&s)[N]) { for (size_t i = 0; i < N; i++) d[i] = s[i] ^ ckey; }
    struct Buf { char s[N]; };
    Buf dec() const { Buf b; for (size_t i = 0; i < N; i++) b.s[i] = d[i] ^ ckey; return b; }
};
static constexpr XA xWatermark{"jb overlay"};
static constexpr XA xClassName{"BattleModel"};

// ============================================================
// Constants
// ============================================================

constexpr float kBoxAspect     = 0.5f;
constexpr float kBarWidth      = 3.0f;
constexpr float kBarGap        = 3.0f;
constexpr ImU32 kShadow        = IM_COL32(0, 0, 0, 200);

constexpr int      kMaxEntities    = 4096;
constexpr size_t   kScanChunk      = 1u << 18;
constexpr uint64_t kRescanGapMs    = 2000;
constexpr uint64_t kScanGapMs      = 500;
constexpr uint64_t kJitterMs       = 300;
constexpr uint32_t kRevalidateMask = 31;
constexpr float    kMatMax         = 1.0e5f;
constexpr float    kMatEps         = 1.0e-4f;
constexpr float    kCenterTol      = 0.35f;

// ============================================================
// Helpers
// ============================================================

float Dist(const Vec3& a, const Vec3& b) noexcept {
    float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

float HalfWidth(Vec2 top, Vec2 bottom) noexcept {
    return (bottom.y - top.y) * kBoxAspect * 0.5f;
}

void ShadowText(ImDrawList* dl, ImVec2 pos, ImU32 col, const char* text) {
    dl->AddText(ImVec2(pos.x + 1.f, pos.y + 1.f), kShadow, text);
    dl->AddText(pos, col, text);
}

template <class Visit>
bool ScanHeap(const mem::Process& proc, size_t chunk, size_t overlap, Visit&& visit) {
    SYSTEM_INFO si{};
    GetNativeSystemInfo(&si);
    const uintptr_t lo = reinterpret_cast<uintptr_t>(si.lpMinimumApplicationAddress);
    const uintptr_t hi = reinterpret_cast<uintptr_t>(si.lpMaximumApplicationAddress);

    std::vector<uint8_t> buf(chunk);
    for (uintptr_t a = lo; a < hi;) {
        auto mbiOpt = proc.QueryMemory(a);
        if (!mbiOpt || !mbiOpt->RegionSize) return false;
        const auto& mbi = *mbiOpt;
        const uintptr_t base = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
        const uintptr_t end  = base + mbi.RegionSize;
        if (end <= a) return false;

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

bool PlausibleMatrix(const Mat4& m) noexcept {
    int nonZero = 0;
    for (float v : m.m) {
        if (!std::isfinite(v) || std::fabs(v) > kMatMax) return false;
        if (std::fabs(v) > kMatEps) ++nonZero;
    }
    return nonZero >= 6;
}

bool ProjectsNearCenter(const Mat4& m, const Vec3& local, Vec2 screen, float headH, bool& transposed) noexcept {
    if (!PlausibleMatrix(m)) return false;
    const Vec3 mid{local.x, local.y + headH * 0.5f, local.z};
    const float tol = kCenterTol * screen.y;
    for (bool t : {false, true}) {
        Vec2 s;
        if (!Esp::WorldToScreen(m, mid, screen, s, t)) continue;
        float dx = s.x - screen.x * 0.5f, dy = s.y - screen.y * 0.5f;
        if (dx * dx + dy * dy <= tol * tol) { transposed = t; return true; }
    }
    return false;
}

// Menu styling
void PushMenuStyle() {
    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowRounding = 8.f;
    s.FrameRounding = 4.f;
    s.GrabRounding = 4.f;
    s.TabRounding = 4.f;
    s.WindowPadding = ImVec2(12, 10);
    s.FramePadding = ImVec2(8, 4);
    s.ItemSpacing = ImVec2(8, 5);
    s.ScrollbarSize = 10.f;
    s.GrabMinSize = 8.f;
    s.WindowBorderSize = 1.f;
    s.TabBorderSize = 0.f;

    ImVec4* c = s.Colors;
    c[ImGuiCol_WindowBg]        = ImVec4(0.06f, 0.06f, 0.09f, 0.96f);
    c[ImGuiCol_Border]          = ImVec4(0.18f, 0.20f, 0.30f, 0.60f);
    c[ImGuiCol_FrameBg]         = ImVec4(0.10f, 0.10f, 0.14f, 1.f);
    c[ImGuiCol_FrameBgHovered]  = ImVec4(0.16f, 0.16f, 0.22f, 1.f);
    c[ImGuiCol_FrameBgActive]   = ImVec4(0.22f, 0.22f, 0.30f, 1.f);
    c[ImGuiCol_SliderGrab]      = ImVec4(0.35f, 0.45f, 0.85f, 1.f);
    c[ImGuiCol_SliderGrabActive]= ImVec4(0.45f, 0.55f, 0.95f, 1.f);
    c[ImGuiCol_CheckMark]       = ImVec4(0.35f, 0.55f, 1.f, 1.f);
    c[ImGuiCol_Tab]             = ImVec4(0.10f, 0.10f, 0.15f, 1.f);
    c[ImGuiCol_TabHovered]      = ImVec4(0.22f, 0.26f, 0.42f, 1.f);
    c[ImGuiCol_TabSelected]     = ImVec4(0.18f, 0.22f, 0.38f, 1.f);
    c[ImGuiCol_Header]          = ImVec4(0.14f, 0.14f, 0.20f, 1.f);
    c[ImGuiCol_HeaderHovered]   = ImVec4(0.20f, 0.20f, 0.28f, 1.f);
    c[ImGuiCol_HeaderActive]    = ImVec4(0.24f, 0.24f, 0.34f, 1.f);
    c[ImGuiCol_Button]          = ImVec4(0.14f, 0.16f, 0.24f, 1.f);
    c[ImGuiCol_ButtonHovered]   = ImVec4(0.22f, 0.26f, 0.38f, 1.f);
    c[ImGuiCol_ButtonActive]    = ImVec4(0.28f, 0.32f, 0.45f, 1.f);
    c[ImGuiCol_Separator]       = ImVec4(0.20f, 0.20f, 0.28f, 0.50f);
    c[ImGuiCol_Text]            = ImVec4(0.90f, 0.90f, 0.93f, 1.f);
    c[ImGuiCol_TextDisabled]    = ImVec4(0.45f, 0.45f, 0.52f, 1.f);
    c[ImGuiCol_PopupBg]         = ImVec4(0.08f, 0.08f, 0.12f, 0.96f);
    c[ImGuiCol_TitleBg]         = ImVec4(0.06f, 0.06f, 0.09f, 1.f);
    c[ImGuiCol_TitleBgActive]   = ImVec4(0.10f, 0.12f, 0.20f, 1.f);
}

auto Section = [](const char* label) {
    ImGui::Spacing();
    ImGui::TextColored(ImVec4(0.40f, 0.55f, 1.f, 1.f), "%s", label);
    ImGui::Separator();
};

}  // namespace

// ============================================================
// Lifecycle + Scan
// ============================================================

Esp::Esp(const mem::Process& proc, cfg::Offsets& off) : proc_(proc), off_(off) {
    screen_ = {static_cast<float>(GetSystemMetrics(SM_CXSCREEN)),
               static_cast<float>(GetSystemMetrics(SM_CYSCREEN))};
    lastUpdate_ = std::chrono::steady_clock::now();
    running_.store(true);
}

Esp::~Esp() {
    running_.store(false);
    if (scanThread_.joinable()) scanThread_.join();
}

void Esp::ScanLoop() {
    try {
        std::mt19937 rng(static_cast<unsigned>(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::uniform_int_distribution<uint64_t> jitter(0, kJitterMs);

        while (running_.load()) {
            bool walked = false;
            if (gmAddr_.load() == 0) {
                uintptr_t gm = (off_.battleModelTypeInfo != 0)
                    ? ScanGameManager()
                    : ScanGameManagerByName();
                if (gm) gmAddr_.store(gm);
                walked = true;
            }
            if (walked) std::this_thread::sleep_for(std::chrono::milliseconds(kScanGapMs + jitter(rng)));
            if (running_.load() && viewAddr_.load() == 0 && haveLocal_.load()) {
                Vec3 local{localX_.load(), localY_.load(), localZ_.load()};
                bool t = false;
                if (uintptr_t va = ScanViewMatrix(local, t)) {
                    viewTFound_.store(t);
                    viewAddr_.store(va);
                }
                walked = true;
            }
            if (gmAddr_.load() && viewAddr_.load()) { scanDone_.store(true); return; }
            std::this_thread::sleep_for(std::chrono::milliseconds((walked ? kRescanGapMs : 50) + jitter(rng)));
        }
    } catch (...) {
        scanDone_.store(true);
    }
}

void Esp::ResetScan() {
    gmAddr_.store(0);
    viewAddr_.store(0);
    haveLocal_.store(false);
    entities_.clear();
    prevPos_.clear();
    aimTargetAddr_ = 0;
    aimProgress_ = 0.f;
    EnsureScanThread();
}

void Esp::EnsureScanThread() {
    if (scanDone_.load() && scanThread_.joinable()) {
        scanThread_.join();
        scanDone_.store(false);
        running_.store(true);
        scanThread_ = std::thread(&Esp::ScanLoop, this);
    }
}

uintptr_t Esp::ScanGameManager() const {
    if (off_.battleModelTypeInfo == 0) return 0;

    auto gaName = xGA.dec();
    const auto ga = proc_.ModuleBase(gaName.s);
    if (!ga) return 0;
    const uintptr_t klass = proc_.Read<uintptr_t>(*ga + off_.battleModelTypeInfo).value_or(0);
    if (!klass) return 0;

    const auto valid = [&](uintptr_t obj) {
        if (proc_.Read<uintptr_t>(obj).value_or(0) != klass) return false;
        uintptr_t entities = proc_.Read<uintptr_t>(obj + off_.bmEntities).value_or(0);
        return entities != 0;
    };

    uintptr_t found = 0;
    ScanHeap(proc_, kScanChunk, 0, [&](uintptr_t addr, const uint8_t* d, size_t n) {
        if (!running_.load()) return true;
        for (size_t i = 0; i + sizeof(uintptr_t) <= n; i += sizeof(uintptr_t)) {
            uintptr_t q;
            std::memcpy(&q, d + i, sizeof q);
            if (q == klass && valid(addr + i)) { found = addr + i; return true; }
        }
        return false;
    });
    return found;
}

uintptr_t Esp::ScanGameManagerByName() const {
    if (off_.bmEntities == 0) return 0;

    auto cn = xClassName.dec();
    const size_t nameLen = std::strlen(cn.s);
    const size_t needBytes = nameLen + 1;

    std::vector<uintptr_t> nameAddrs;
    ScanHeap(proc_, kScanChunk, needBytes, [&](uintptr_t addr, const uint8_t* d, size_t n) {
        if (!running_.load()) return true;
        for (size_t i = 0; i + needBytes <= n; ++i) {
            if (std::memcmp(d + i, cn.s, needBytes) != 0) continue;
            if (i > 0 && d[i - 1] >= 'A' && d[i - 1] <= 'z') continue;
            nameAddrs.push_back(addr + i);
            if (nameAddrs.size() >= 64) return true;
        }
        return false;
    });
    if (nameAddrs.empty()) return 0;

    std::unordered_set<uintptr_t> nameSet(nameAddrs.begin(), nameAddrs.end());
    uintptr_t klass = 0;
    ScanHeap(proc_, kScanChunk, 0, [&](uintptr_t addr, const uint8_t* d, size_t n) {
        if (!running_.load() || klass) return true;
        for (size_t i = 0; i + 8 <= n; i += 8) {
            uintptr_t val;
            std::memcpy(&val, d + i, 8);
            if (!nameSet.count(val)) continue;
            uintptr_t candidate = addr + i - 0x10;
            uintptr_t nsPtr = proc_.Read<uintptr_t>(candidate + 0x18).value_or(0);
            if (!nsPtr) continue;
            char ns[64]{};
            if (!proc_.ReadBuf(nsPtr, ns, 63)) continue;
            bool valid = true;
            for (int j = 0; ns[j]; ++j) {
                if (ns[j] < 0x20 || ns[j] > 0x7E) { valid = false; break; }
            }
            if (valid) { klass = candidate; return true; }
        }
        return false;
    });
    if (!klass) return 0;

    uintptr_t found = 0;
    ScanHeap(proc_, kScanChunk, 0, [&](uintptr_t addr, const uint8_t* d, size_t n) {
        if (!running_.load()) return true;
        for (size_t i = 0; i + 8 <= n; i += 8) {
            uintptr_t q;
            std::memcpy(&q, d + i, 8);
            if (q != klass) continue;
            uintptr_t obj = addr + i;
            uintptr_t entities = proc_.Read<uintptr_t>(obj + off_.bmEntities).value_or(0);
            if (entities) { found = obj; return true; }
        }
        return false;
    });
    return found;
}

uintptr_t Esp::ScanViewMatrix(const Vec3& local, bool& transposed) const {
    Vec2 screen = screen_;
    float headH = off_.headHeight;
    uintptr_t found = 0;
    bool foundT = false;
    ScanHeap(proc_, kScanChunk, sizeof(Mat4), [&](uintptr_t addr, const uint8_t* d, size_t n) {
        if (!running_.load()) return true;
        for (size_t i = 0; i + sizeof(Mat4) <= n; i += sizeof(float)) {
            Mat4 m;
            std::memcpy(&m, d + i, sizeof m);
            bool t = false;
            if (!ProjectsNearCenter(m, local, screen, headH, t)) continue;
            found = addr + i; foundT = t;
            return true;
        }
        return false;
    });
    transposed = foundT;
    return found;
}

// ============================================================
// Update — traverses ROB RUN object hierarchy
// ============================================================

bool Esp::Update() {
    if (!scanThread_.joinable() && running_.load()) {
        try { scanThread_ = std::thread(&Esp::ScanLoop, this); } catch (...) {}
    }
    EnsureScanThread();
    entities_.clear();
    localOrigin_ = {};

    const auto rd = [this](uintptr_t a) -> uintptr_t {
        return a ? proc_.Read<uintptr_t>(a).value_or(0) : 0;
    };

    uintptr_t bm = gmAddr_.load();
    if (!bm) return false;

    uintptr_t be = rd(bm + off_.bmEntities);
    if (!be) { gmAddr_.store(0); EnsureScanThread(); return false; }

    uintptr_t localPlayer = rd(be + off_.bePlayer);
    uintptr_t localServer = localPlayer ? rd(localPlayer + off_.localServerModel) : 0;

    int localRole = -1;
    if (localServer && off_.cmRole) {
        uintptr_t rp = rd(localServer + off_.cmRole);
        uintptr_t pd = rp ? rd(rp + off_.primitiveData) : 0;
        if (pd) localRole = proc_.Read<int32_t>(pd + off_.primitiveValue).value_or(-1);
    }

    if (localServer && off_.cmTransform) {
        uintptr_t ltx = rd(localServer + off_.cmTransform);
        if (ltx) {
            localOrigin_.x = proc_.Read<float>(ltx + off_.txX).value_or(0);
            localOrigin_.y = proc_.Read<float>(ltx + off_.txY).value_or(0);
            localOrigin_.z = proc_.Read<float>(ltx + off_.txZ).value_or(0);
            localX_.store(localOrigin_.x);
            localY_.store(localOrigin_.y);
            localZ_.store(localOrigin_.z);
            haveLocal_.store(true);
        }
    }

    uintptr_t models = rd(be + off_.beModels);
    if (!models) return false;

    uintptr_t entries = rd(models + off_.dictEntries);
    int count = proc_.Read<int32_t>(models + off_.dictCount).value_or(0);
    if (!entries || count <= 0 || count > kMaxEntities) return false;

    bool haveLocal = false;
    entities_.reserve(static_cast<size_t>(count));

    for (int i = 0; i < count; ++i) {
        uintptr_t entry = entries + off_.arrayData +
                          static_cast<uintptr_t>(i) * off_.dictEntrySize;
        uintptr_t em = rd(entry + off_.dictEntryValue);
        if (!em) continue;

        uintptr_t server = rd(em + off_.entityServerModel);
        if (!server) continue;

        uintptr_t tx = off_.cmTransform ? rd(server + off_.cmTransform) : 0;
        if (!tx) continue;

        Entity e;
        e.address = server;
        if (off_.cmId) e.id = proc_.Read<int32_t>(server + off_.cmId).value_or(-1);
        e.origin.x = proc_.Read<float>(tx + off_.txX).value_or(0);
        e.origin.y = proc_.Read<float>(tx + off_.txY).value_or(0);
        e.origin.z = proc_.Read<float>(tx + off_.txZ).value_or(0);
        e.head = {e.origin.x, e.origin.y + off_.headHeight, e.origin.z};

        if (off_.cmIsDead) {
            uintptr_t dp = rd(server + off_.cmIsDead);
            uintptr_t pd = dp ? rd(dp + off_.primitiveData) : 0;
            if (pd) e.alive = proc_.Read<uint8_t>(pd + off_.primitiveValue).value_or(0) == 0;
        }

        bool isLocal = (localServer != 0 && server == localServer);
        if (isLocal) {
            e.role = Role::Local;
            localOrigin_ = e.origin;
            haveLocal = true;
        } else {
            int role = -1;
            if (off_.cmRole) {
                uintptr_t rp = rd(server + off_.cmRole);
                uintptr_t pd = rp ? rd(rp + off_.primitiveData) : 0;
                if (pd) role = proc_.Read<int32_t>(pd + off_.primitiveValue).value_or(-1);
            }
            if (localRole >= 0 && role == localRole)
                e.role = Role::Teammate;
            else if (role == off_.copRole || role == off_.robberRole)
                e.role = Role::Enemy;
            else
                e.role = Role::Neutral;
        }

        if (off_.cmPrivateModel && off_.pmHealth) {
            uintptr_t priv = rd(server + off_.cmPrivateModel);
            uintptr_t hp = priv ? rd(priv + off_.pmHealth) : 0;
            if (hp) {
                auto readHpVal = [&](uintptr_t base) -> float {
                    if (off_.healthIsPrimitive) {
                        uintptr_t pd = rd(base);
                        uintptr_t pv = pd ? rd(pd + off_.primitiveData) : 0;
                        if (!pv) return 100.f;
                        if (off_.healthIsFloat)
                            return proc_.Read<float>(pv + off_.primitiveValue).value_or(100.f);
                        return static_cast<float>(proc_.Read<int32_t>(pv + off_.primitiveValue).value_or(100));
                    }
                    if (off_.healthIsFloat)
                        return proc_.Read<float>(base).value_or(100.f);
                    return static_cast<float>(proc_.Read<int32_t>(base).value_or(100));
                };
                e.health    = static_cast<int>(readHpVal(hp + off_.healthValue));
                e.maxHealth = static_cast<int>(readHpVal(hp + off_.healthMax));
            }
        }
        if (!e.alive) e.health = 0;

        entities_.push_back(e);
    }

    if (!haveLocal && localServer) {
        Entity e;
        e.address = localServer;
        e.origin = localOrigin_;
        e.head = {localOrigin_.x, localOrigin_.y + off_.headHeight, localOrigin_.z};
        e.role = Role::Local;
        entities_.push_back(e);
        haveLocal = true;
    }

    if (!haveLocal) { entities_.clear(); return false; }

    // name reading from slot table
    if (off_.bmSlots && off_.slotsArray && off_.slotNickname && off_.slotData && off_.slotEntityId) {
        uintptr_t slotsModel = rd(bm + off_.bmSlots);
        uintptr_t slotsList = slotsModel ? rd(slotsModel + off_.slotsArray) : 0;
        if (slotsList) {
            uintptr_t items = rd(slotsList + 0x10);
            int sc = proc_.Read<int32_t>(slotsList + 0x18).value_or(0);
            if (items && sc > 0 && sc < 512) {
                for (int s = 0; s < sc; ++s) {
                    uintptr_t slot = proc_.Read<uintptr_t>(items + off_.arrayData + static_cast<uintptr_t>(s) * 8).value_or(0);
                    if (!slot) continue;
                    uintptr_t data = rd(slot + off_.slotData);
                    if (!data) continue;
                    int eid = proc_.Read<int32_t>(data + off_.slotEntityId).value_or(-1);
                    if (eid < 0) continue;
                    uintptr_t nick = rd(data + off_.slotNickname);
                    if (!nick) continue;
                    int len = proc_.Read<int32_t>(nick + 0x10).value_or(0);
                    if (len <= 0 || len > 15) continue;
                    wchar_t wb[16]{};
                    if (!proc_.ReadBuf(nick + 0x14, wb, static_cast<size_t>(len) * 2)) continue;
                    for (auto& e : entities_) {
                        if (e.id == eid && e.name[0] == '\0') {
                            for (int c = 0; c < len && c < 31; ++c)
                                e.name[c] = static_cast<char>(wb[c] < 128 ? wb[c] : '?');
                            break;
                        }
                    }
                }
            }
        }
    }

    // velocity estimation
    auto now = std::chrono::steady_clock::now();
    float dt = std::chrono::duration<float>(now - lastUpdate_).count();
    lastUpdate_ = now;
    if (dt > 0.001f && dt < 1.f) {
        for (auto& e : entities_) {
            auto it = prevPos_.find(e.address);
            if (it != prevPos_.end()) {
                e.velocity.x = (e.origin.x - it->second.x) / dt;
                e.velocity.y = (e.origin.y - it->second.y) / dt;
                e.velocity.z = (e.origin.z - it->second.z) / dt;
            }
        }
    }
    prevPos_.clear();
    for (const auto& e : entities_) prevPos_[e.address] = e.origin;

    // view matrix
    uintptr_t va = viewAddr_.load();
    if (!va) { entities_.clear(); return false; }

    if ((frameCount_++ & kRevalidateMask) == 0 || view_.m[15] == 0.f) {
        auto vp = proc_.Read<Mat4>(va);
        bool t = viewTFound_.load();
        if (!vp || !ProjectsNearCenter(*vp, localOrigin_, screen_, off_.headHeight, t)) {
            viewAddr_.store(0);
            EnsureScanThread();
            entities_.clear();
            return false;
        }
        view_ = *vp; viewT_ = t;
    }
    return true;
}

// ============================================================
// W2S
// ============================================================

bool Esp::WorldToScreen(const Mat4& vp, const Vec3& w, Vec2 screen, Vec2& out, bool transposed) noexcept {
    const float* m = vp.m;
    const auto at = [m, transposed](int r, int c) { return transposed ? m[c * 4 + r] : m[r * 4 + c]; };
    float clipW = at(3, 0) * w.x + at(3, 1) * w.y + at(3, 2) * w.z + at(3, 3);
    if (!(clipW > 0.001f)) return false;
    float nx = (at(0, 0) * w.x + at(0, 1) * w.y + at(0, 2) * w.z + at(0, 3)) / clipW;
    float ny = (at(1, 0) * w.x + at(1, 1) * w.y + at(1, 2) * w.z + at(1, 3)) / clipW;
    if (!std::isfinite(nx) || !std::isfinite(ny)) return false;
    out.x = screen.x * 0.5f * (1.f + nx);
    out.y = screen.y * 0.5f * (1.f - ny);
    return true;
}

// ============================================================
// ESP rendering
// ============================================================

bool Esp::IsVisibleRole(Role r) const noexcept {
    if (r == Role::Local) return !cfg_.skipLocal;
    if (r == Role::Teammate) return !cfg_.skipTeammate;
    return true;
}

ImU32 Esp::RoleColor(Role r) const noexcept {
    size_t i = static_cast<size_t>(r);
    return ImGui::ColorConvertFloat4ToU32(cfg_.roleColor[i < kRoleCount ? i : static_cast<size_t>(Role::Neutral)]);
}

void Esp::Render(ImDrawList* dl, Vec2 screen) const {
    if (!dl || !cfg_.enabled || screen.x <= 0.f || screen.y <= 0.f) return;

    if (cfg_.fovCircle)
        dl->AddCircle(ImVec2(screen.x * 0.5f, screen.y * 0.5f), cfg_.fovRadius,
                      IM_COL32(255, 255, 255, 50), 64, 1.f);

    for (const Entity& e : entities_) {
        if (cfg_.skipDead && (!e.alive || e.health <= 0)) continue;
        if (!IsVisibleRole(e.role)) continue;
        float meters = Dist(localOrigin_, e.origin) * off_.metersPerUnit;
        if (cfg_.maxDistance > 0.f && meters > cfg_.maxDistance) continue;

        Vec2 top, bottom;
        if (!WorldToScreen(view_, e.head, screen, top, viewT_)) continue;
        if (!WorldToScreen(view_, e.origin, screen, bottom, viewT_)) continue;
        float h = bottom.y - top.y;
        if (!(h > 1.f)) continue;
        float hw = HalfWidth(top, bottom);
        float cx = (top.x + bottom.x) * 0.5f;
        if (cx + hw < 0 || cx - hw > screen.x || bottom.y < 0 || top.y > screen.y) continue;

        ImU32 col = RoleColor(e.role);
        top.x = bottom.x = cx;

        if (cfg_.tracer)   DrawTracer(dl, screen, bottom, col);
        if (cfg_.skeleton) DrawSkeleton(dl, top, bottom, col);

        switch (cfg_.boxStyle) {
        case 1: DrawBox(dl, top, bottom, col); break;
        case 2: DrawCornerBox(dl, top, bottom, col); break;
        case 3: DrawFilledBox(dl, top, bottom, col); break;
        default: break;
        }

        if (cfg_.headDot)   DrawHeadDot(dl, top, col);
        if (cfg_.healthBar) DrawHealthBar(dl, e, top, bottom);
        if (cfg_.name)      DrawName(dl, e, top, bottom, col);
        if (cfg_.distance)  DrawDistance(dl, meters, top, bottom, col);
    }
}

void Esp::DrawBox(ImDrawList* dl, Vec2 top, Vec2 bottom, ImU32 col) const {
    float hw = HalfWidth(top, bottom);
    ImVec2 a(top.x - hw, top.y), b(top.x + hw, bottom.y);
    dl->AddRect(ImVec2(a.x - 1.f, a.y - 1.f), ImVec2(b.x + 1.f, b.y + 1.f), kShadow, 0.f, 0, cfg_.boxThickness + 1.f);
    dl->AddRect(a, b, col, 0.f, 0, cfg_.boxThickness);
}

void Esp::DrawCornerBox(ImDrawList* dl, Vec2 top, Vec2 bottom, ImU32 col) const {
    float hw = HalfWidth(top, bottom);
    float h = bottom.y - top.y;
    float w = hw * 2.f;
    float cl = std::min(w, h) * 0.25f;
    float x1 = top.x - hw, y1 = top.y, x2 = top.x + hw, y2 = bottom.y;
    float th = cfg_.boxThickness;

    auto seg = [&](float ax, float ay, float bx, float by) {
        dl->AddLine(ImVec2(ax, ay), ImVec2(bx, by), kShadow, th + 1.5f);
        dl->AddLine(ImVec2(ax, ay), ImVec2(bx, by), col, th);
    };
    seg(x1, y1, x1 + cl, y1); seg(x1, y1, x1, y1 + cl);
    seg(x2, y1, x2 - cl, y1); seg(x2, y1, x2, y1 + cl);
    seg(x1, y2, x1 + cl, y2); seg(x1, y2, x1, y2 - cl);
    seg(x2, y2, x2 - cl, y2); seg(x2, y2, x2, y2 - cl);
}

void Esp::DrawFilledBox(ImDrawList* dl, Vec2 top, Vec2 bottom, ImU32 col) const {
    float hw = HalfWidth(top, bottom);
    ImVec2 a(top.x - hw, top.y), b(top.x + hw, bottom.y);
    ImU32 fill = (col & 0x00FFFFFFu) | 0x28000000u;
    dl->AddRectFilled(a, b, fill);
    dl->AddRect(a, b, col, 0.f, 0, cfg_.boxThickness);
}

void Esp::DrawName(ImDrawList* dl, const Entity& e, Vec2 top, Vec2, ImU32 col) const {
    if (e.name[0] == '\0') return;
    ImVec2 sz = ImGui::CalcTextSize(e.name);
    ShadowText(dl, ImVec2(top.x - sz.x * 0.5f, top.y - sz.y - 2.f), col, e.name);
}

void Esp::DrawHealthBar(ImDrawList* dl, const Entity& e, Vec2 top, Vec2 bottom) const {
    float hw = HalfWidth(top, bottom);
    float maxHp = static_cast<float>(e.maxHealth > 0 ? e.maxHealth : 100);
    float frac = std::clamp(static_cast<float>(e.health) / maxHp, 0.f, 1.f);
    float x1 = top.x - hw - kBarGap, x0 = x1 - kBarWidth;
    float h = bottom.y - top.y, fillTop = bottom.y - h * frac;
    ImU32 fill = IM_COL32(static_cast<int>((1.f - frac) * 255.f),
                           static_cast<int>(frac * 255.f), 40, 255);
    dl->AddRectFilled(ImVec2(x0 - 1.f, top.y - 1.f), ImVec2(x1 + 1.f, bottom.y + 1.f), kShadow);
    dl->AddRectFilled(ImVec2(x0, fillTop), ImVec2(x1, bottom.y), fill);

    if (cfg_.healthText) {
        char buf[8];
        std::snprintf(buf, sizeof buf, "%d", e.health);
        ImVec2 sz = ImGui::CalcTextSize(buf);
        ShadowText(dl, ImVec2(x0 - sz.x - 2.f, fillTop), IM_COL32(255, 255, 255, 200), buf);
    }
}

void Esp::DrawDistance(ImDrawList* dl, float meters, Vec2, Vec2 bottom, ImU32 col) const {
    char buf[16];
    std::snprintf(buf, sizeof buf, "%.0fm", static_cast<double>(meters));
    ImVec2 sz = ImGui::CalcTextSize(buf);
    ShadowText(dl, ImVec2(bottom.x - sz.x * 0.5f, bottom.y + 2.f), col, buf);
}

void Esp::DrawTracer(ImDrawList* dl, Vec2 screen, Vec2 bottom, ImU32 col) const {
    ImVec2 from;
    switch (cfg_.tracerPos) {
    case 0: from = ImVec2(screen.x * 0.5f, 0.f); break;
    case 1: from = ImVec2(screen.x * 0.5f, screen.y * 0.5f); break;
    default: from = ImVec2(screen.x * 0.5f, screen.y); break;
    }
    dl->AddLine(from, ImVec2(bottom.x, bottom.y), col, cfg_.tracerThickness);
}

void Esp::DrawHeadDot(ImDrawList* dl, Vec2 head, ImU32 col) const {
    dl->AddCircleFilled(ImVec2(head.x, head.y), 3.f, kShadow);
    dl->AddCircleFilled(ImVec2(head.x, head.y), 2.f, col);
}

void Esp::DrawSkeleton(ImDrawList* dl, Vec2 head, Vec2 bottom, ImU32 col) const {
    float h = bottom.y - head.y;
    if (h < 10.f) return;
    float hw = h * 0.12f;
    ImVec2 nk(head.x, head.y + h * 0.12f);
    ImVec2 hip(head.x, head.y + h * 0.55f);
    ImVec2 lsh(head.x - hw, nk.y + 2.f), rsh(head.x + hw, nk.y + 2.f);
    ImVec2 lf(head.x - hw * 0.6f, bottom.y), rf(head.x + hw * 0.6f, bottom.y);
    float th = 1.5f;
    dl->AddLine(ImVec2(head.x, head.y), nk, col, th);
    dl->AddLine(nk, hip, col, th);
    dl->AddLine(nk, lsh, col, th);
    dl->AddLine(nk, rsh, col, th);
    dl->AddLine(hip, lf, col, th);
    dl->AddLine(hip, rf, col, th);
}

// ============================================================
// Silent Aim
// ============================================================

int Esp::SelectTarget(Vec2 center) const {
    int best = -1;
    float bestScore = 1e9f;

    for (int i = 0; i < static_cast<int>(entities_.size()); ++i) {
        const Entity& e = entities_[static_cast<size_t>(i)];
        if (!e.alive || e.health <= 0 || e.role == Role::Local || e.role == Role::Teammate) continue;

        Vec3 aim = e.head;
        switch (aimCfg_.bone) {
        case 1: aim.y = (e.origin.y + e.head.y) * 0.5f; break;
        case 2: aim.y = e.origin.y + (e.head.y - e.origin.y) * 0.6f; break;
        default: break;
        }
        if (aimCfg_.prediction) {
            aim.x += e.velocity.x * aimCfg_.predScale;
            aim.y += e.velocity.y * aimCfg_.predScale;
            aim.z += e.velocity.z * aimCfg_.predScale;
        }

        Vec2 s;
        if (!WorldToScreen(view_, aim, screen_, s, viewT_)) continue;
        float dx = s.x - center.x, dy = s.y - center.y;
        float screenDist = std::sqrt(dx * dx + dy * dy);
        if (screenDist > aimCfg_.fov) continue;

        float score;
        switch (aimCfg_.priority) {
        case 1: score = Dist(localOrigin_, e.origin); break;
        case 2: score = static_cast<float>(e.health); break;
        default: score = screenDist; break;
        }

        if (score < bestScore) { bestScore = score; best = i; }
    }
    return best;
}

void Esp::RenderAim(Vec2 screen) {
    if (!aimCfg_.enabled) return;

    Vec2 center = {screen.x * 0.5f, screen.y * 0.5f};

    if (aimCfg_.drawFov) {
        ImGui::GetBackgroundDrawList()->AddCircle(
            ImVec2(center.x, center.y), aimCfg_.fov,
            IM_COL32(255, 255, 255, 35), 64, 1.f);
    }

    // triggerbot (independent of aim key)
    if (aimCfg_.triggerbot) {
        bool onTarget = false;
        for (const Entity& e : entities_) {
            if (!e.alive || e.health <= 0 || e.role == Role::Local || e.role == Role::Teammate) continue;
            Vec2 s;
            if (!WorldToScreen(view_, e.head, screen, s, viewT_)) continue;
            float d = std::sqrt((s.x - center.x) * (s.x - center.x) +
                                (s.y - center.y) * (s.y - center.y));
            if (d < aimCfg_.triggerRadius) { onTarget = true; break; }
        }
        auto now = std::chrono::steady_clock::now();
        if (onTarget) {
            if (!triggerArmed_) { triggerArmed_ = true; triggerStart_ = now; }
            float elapsed = std::chrono::duration<float>(now - triggerStart_).count();
            if (elapsed >= aimCfg_.triggerDelay) {
                INPUT inp[2]{};
                inp[0].type = INPUT_MOUSE;
                inp[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
                inp[1].type = INPUT_MOUSE;
                inp[1].mi.dwFlags = MOUSEEVENTF_LEFTUP;
                SendInput(2, inp, sizeof(INPUT));
                triggerArmed_ = false;
            }
        } else {
            triggerArmed_ = false;
        }
    }

    // aimbot requires key
    if (!(GetAsyncKeyState(aimCfg_.key) & 0x8000)) {
        aimTargetAddr_ = 0;
        aimProgress_ = 0.f;
        return;
    }

    // sticky aim — keep locked target if still valid
    int targetIdx = -1;
    if (aimCfg_.stickyAim && aimTargetAddr_ != 0) {
        for (int i = 0; i < static_cast<int>(entities_.size()); ++i) {
            const auto& e = entities_[static_cast<size_t>(i)];
            if (e.address == aimTargetAddr_ && e.alive && e.health > 0) {
                targetIdx = i; break;
            }
        }
    }
    if (targetIdx < 0) {
        targetIdx = SelectTarget(center);
        if (targetIdx >= 0) {
            aimTargetAddr_ = entities_[static_cast<size_t>(targetIdx)].address;
            aimProgress_ = 0.f;
            aimLockTime_ = std::chrono::steady_clock::now();
        }
    }
    if (targetIdx < 0) return;

    // aim delay
    float since = std::chrono::duration<float>(std::chrono::steady_clock::now() - aimLockTime_).count();
    if (since < aimCfg_.aimDelay) return;

    const Entity& target = entities_[static_cast<size_t>(targetIdx)];
    Vec3 aimPoint = target.head;
    switch (aimCfg_.bone) {
    case 1: aimPoint.y = (target.origin.y + target.head.y) * 0.5f; break;
    case 2: aimPoint.y = target.origin.y + (target.head.y - target.origin.y) * 0.6f; break;
    default: break;
    }
    if (aimCfg_.prediction) {
        aimPoint.x += target.velocity.x * aimCfg_.predScale;
        aimPoint.y += target.velocity.y * aimCfg_.predScale;
        aimPoint.z += target.velocity.z * aimCfg_.predScale;
    }

    Vec2 targetScreen;
    if (!WorldToScreen(view_, aimPoint, screen, targetScreen, viewT_)) return;

    float totalDx = targetScreen.x - center.x;
    float totalDy = targetScreen.y - center.y;
    float totalDist = std::sqrt(totalDx * totalDx + totalDy * totalDy);

    // ease-out: fast far, slow close
    float factor = 1.f;
    if (aimCfg_.bezier) {
        float norm = std::min(totalDist / aimCfg_.fov, 1.f);
        factor = norm * norm;
        factor = std::max(factor, 0.04f);
    }

    // variable smooth: more smooth when close to target
    float smooth = aimCfg_.smooth;
    if (totalDist < aimCfg_.fov * 0.3f) {
        float blend = totalDist / (aimCfg_.fov * 0.3f);
        smooth = aimCfg_.smoothNear * (1.f - blend) + aimCfg_.smooth * blend;
    }

    float dx = totalDx * factor / smooth;
    float dy = totalDy * factor / smooth;

    // overshoot on initial approach
    aimProgress_ = std::min(aimProgress_ + 0.025f, 1.f);
    if (aimProgress_ < 0.3f && aimCfg_.overshoot > 0.f) {
        float os = aimCfg_.overshoot * (1.f - aimProgress_ / 0.3f);
        dx *= (1.f + os);
        dy *= (1.f + os);
    }

    // clamp speed
    float speed = std::sqrt(dx * dx + dy * dy);
    if (speed > aimCfg_.maxSpeed) {
        dx = dx / speed * aimCfg_.maxSpeed;
        dy = dy / speed * aimCfg_.maxSpeed;
    }

    if (std::fabs(dx) < 0.3f && std::fabs(dy) < 0.3f) return;

    // human jitter
    static std::mt19937 rng(static_cast<unsigned>(
        std::chrono::steady_clock::now().time_since_epoch().count() ^ GetCurrentProcessId()));
    std::uniform_real_distribution<float> noise(-aimCfg_.jitter, aimCfg_.jitter);
    dx += noise(rng);
    dy += noise(rng);

    INPUT input{};
    input.type = INPUT_MOUSE;
    input.mi.dx = static_cast<LONG>(dx);
    input.mi.dy = static_cast<LONG>(dy);
    input.mi.dwFlags = MOUSEEVENTF_MOVE;
    SendInput(1, &input, sizeof(INPUT));
}

// ============================================================
// Misc rendering
// ============================================================

void Esp::RenderMisc(ImDrawList* dl, Vec2 screen, float fps) const {
    if (!dl) return;

    if (miscCfg_.crosshair) {
        ImVec2 c(screen.x * 0.5f, screen.y * 0.5f);
        ImU32 col = ImGui::ColorConvertFloat4ToU32(miscCfg_.crosshairColor);
        float sz = miscCfg_.crosshairSize;
        float th = miscCfg_.crosshairThick;
        int style = miscCfg_.crosshairStyle;
        if (style == 0 || style == 3) dl->AddCircleFilled(c, 2.f, col);
        if (style == 1 || style == 3) {
            dl->AddLine(ImVec2(c.x - sz, c.y), ImVec2(c.x - 2.f, c.y), col, th);
            dl->AddLine(ImVec2(c.x + 2.f, c.y), ImVec2(c.x + sz, c.y), col, th);
            dl->AddLine(ImVec2(c.x, c.y - sz), ImVec2(c.x, c.y - 2.f), col, th);
            dl->AddLine(ImVec2(c.x, c.y + 2.f), ImVec2(c.x, c.y + sz), col, th);
        }
        if (style == 2 || style == 3) dl->AddCircle(c, sz, col, 32, th);
    }

    if (miscCfg_.watermark) {
        auto wm = xWatermark.dec();
        ImVec2 sz = ImGui::CalcTextSize(wm.s);
        float pad = 6.f;
        dl->AddRectFilled(ImVec2(5, 5), ImVec2(sz.x + pad * 2.f + 5, sz.y + pad + 7), IM_COL32(10, 10, 16, 180), 6.f);
        dl->AddRect(ImVec2(5, 5), ImVec2(sz.x + pad * 2.f + 5, sz.y + pad + 7), IM_COL32(60, 80, 160, 100), 6.f);
        ShadowText(dl, ImVec2(5 + pad, 5 + pad * 0.5f), IM_COL32(130, 170, 255, 255), wm.s);
    }

    if (miscCfg_.fpsCounter) {
        char buf[16];
        std::snprintf(buf, sizeof buf, "%.0f fps", static_cast<double>(fps));
        float y = miscCfg_.watermark ? 30.f : 7.f;
        ShadowText(dl, ImVec2(10, y), IM_COL32(180, 180, 180, 180), buf);
    }
}

// ============================================================
// World rendering (radar + proximity)
// ============================================================

void Esp::RenderWorld(ImDrawList* dl, Vec2 screen) const {
    if (!dl) return;

    if (worldCfg_.proximity && !entities_.empty()) {
        for (const Entity& e : entities_) {
            if (!e.alive || e.role == Role::Local || e.role == Role::Teammate) continue;
            float d = Dist(localOrigin_, e.origin) * off_.metersPerUnit;
            if (d < worldCfg_.proximityDist) {
                float alpha = (1.f - d / worldCfg_.proximityDist) * worldCfg_.proximityColor.w;
                ImU32 col = IM_COL32(static_cast<int>(worldCfg_.proximityColor.x * 255),
                                     static_cast<int>(worldCfg_.proximityColor.y * 255),
                                     static_cast<int>(worldCfg_.proximityColor.z * 255),
                                     static_cast<int>(alpha * 255));
                dl->AddRectFilled(ImVec2(0, 0), ImVec2(screen.x, screen.y), col);
                break;
            }
        }
    }

    if (!worldCfg_.radar || entities_.empty()) return;

    float rs = worldCfg_.radarSize;
    float rx = worldCfg_.radarX, ry = worldCfg_.radarY;
    if (miscCfg_.watermark) ry = std::max(ry, 48.f);

    ImVec2 rMin(rx, ry), rMax(rx + rs, ry + rs);
    ImVec2 rCenter(rx + rs * 0.5f, ry + rs * 0.5f);

    dl->AddRectFilled(rMin, rMax, IM_COL32(10, 10, 16, 160), 6.f);
    dl->AddRect(rMin, rMax, IM_COL32(60, 70, 100, 150), 6.f, 0, 1.f);
    dl->AddLine(ImVec2(rCenter.x - 3, rCenter.y), ImVec2(rCenter.x + 3, rCenter.y), IM_COL32(255, 255, 255, 80));
    dl->AddLine(ImVec2(rCenter.x, rCenter.y - 3), ImVec2(rCenter.x, rCenter.y + 3), IM_COL32(255, 255, 255, 80));

    float scale = (rs * 0.5f) / worldCfg_.radarRange;

    for (const Entity& e : entities_) {
        if (!e.alive) continue;
        float dx = (e.origin.x - localOrigin_.x) * off_.metersPerUnit * scale;
        float dz = (e.origin.z - localOrigin_.z) * off_.metersPerUnit * scale;
        float halfR = rs * 0.5f - 3.f;
        if (dx * dx + dz * dz > halfR * halfR) {
            float len = std::sqrt(dx * dx + dz * dz);
            dx = dx / len * halfR;
            dz = dz / len * halfR;
        }

        ImU32 col;
        float dotSize;
        if (e.role == Role::Local) { col = IM_COL32(0, 255, 0, 255); dotSize = 3.f; }
        else if (e.role == Role::Teammate) { col = IM_COL32(80, 130, 255, 255); dotSize = 2.5f; }
        else { col = IM_COL32(255, 60, 60, 255); dotSize = 2.5f; }
        dl->AddCircleFilled(ImVec2(rCenter.x + dx, rCenter.y + dz), dotSize, col);
    }
}

// ============================================================
// Status HUD
// ============================================================

void Esp::RenderStatus(ImDrawList* dl, Vec2 screen, bool attached, bool active) const {
    if (!dl) return;

    // dormant banner when ESP not activated
    if (!active) {
        const char* hint = attached ? "F1: activate  |  INS: menu  |  END: exit"
                                    : "waiting for game...  |  END: exit";
        ImVec2 hsz = ImGui::CalcTextSize(hint);
        float hx = (screen.x - hsz.x) * 0.5f;
        float hy = screen.y * 0.42f;
        ShadowText(dl, ImVec2(hx, hy), IM_COL32(180, 200, 255, 160), hint);

        const char* tag = "jb overlay";
        ImVec2 tsz = ImGui::CalcTextSize(tag);
        ShadowText(dl, ImVec2((screen.x - tsz.x) * 0.5f, hy - tsz.y - 6.f),
                   IM_COL32(90, 110, 160, 100), tag);
        return;
    }

    char buf[128];
    const char* bmStatus = gmAddr_.load() ? "found"
                         : (off_.battleModelTypeInfo == 0 ? "name-scan..." : "scan...");
    std::snprintf(buf, sizeof buf, "%s | BM:%s | VM:%s | %d ents",
                  attached ? "attached" : "waiting",
                  bmStatus,
                  viewAddr_.load() ? "found" : "scan...",
                  static_cast<int>(entities_.size()));
    ImVec2 sz = ImGui::CalcTextSize(buf);
    float pad = 5.f;
    float x = screen.x - sz.x - pad * 2.f;
    float y = screen.y - sz.y - pad * 2.f;
    dl->AddRectFilled(ImVec2(x, y), ImVec2(screen.x, screen.y), IM_COL32(10, 10, 16, 150), 4.f);
    ShadowText(dl, ImVec2(x + pad, y + pad), IM_COL32(140, 150, 170, 180), buf);
}

// ============================================================
// Menu
// ============================================================

void Esp::RenderMenu() {
    PushMenuStyle();

    if (ImGui::BeginTabBar("##tabs", ImGuiTabBarFlags_NoTooltip)) {

        // ---- ESP TAB ----
        if (ImGui::BeginTabItem("ESP")) {
            ImGui::Checkbox("Enable ESP", &cfg_.enabled);

            Section("Box Style");
            ImGui::RadioButton("Off##box", &cfg_.boxStyle, 0); ImGui::SameLine();
            ImGui::RadioButton("Full", &cfg_.boxStyle, 1); ImGui::SameLine();
            ImGui::RadioButton("Corner", &cfg_.boxStyle, 2); ImGui::SameLine();
            ImGui::RadioButton("Filled", &cfg_.boxStyle, 3);
            ImGui::SliderFloat("Thickness##box", &cfg_.boxThickness, 1.f, 4.f, "%.1f");

            Section("Elements");
            ImGui::Checkbox("Health Bar", &cfg_.healthBar); ImGui::SameLine(160.f);
            ImGui::Checkbox("HP Text", &cfg_.healthText);
            ImGui::Checkbox("Distance", &cfg_.distance); ImGui::SameLine(160.f);
            ImGui::Checkbox("Names", &cfg_.name);
            ImGui::Checkbox("Head Dot", &cfg_.headDot); ImGui::SameLine(160.f);
            ImGui::Checkbox("Skeleton", &cfg_.skeleton);
            ImGui::Checkbox("FOV Circle", &cfg_.fovCircle);
            if (cfg_.fovCircle)
                ImGui::SliderFloat("FOV Radius", &cfg_.fovRadius, 20.f, 600.f, "%.0f px");

            Section("Tracers");
            ImGui::Checkbox("Enable##tracer", &cfg_.tracer);
            if (cfg_.tracer) {
                ImGui::RadioButton("Top", &cfg_.tracerPos, 0); ImGui::SameLine();
                ImGui::RadioButton("Center", &cfg_.tracerPos, 1); ImGui::SameLine();
                ImGui::RadioButton("Bottom", &cfg_.tracerPos, 2);
                ImGui::SliderFloat("Thickness##tr", &cfg_.tracerThickness, 1.f, 4.f, "%.1f");
            }

            Section("Filters");
            ImGui::Checkbox("Skip Self", &cfg_.skipLocal); ImGui::SameLine(160.f);
            ImGui::Checkbox("Skip Team", &cfg_.skipTeammate);
            ImGui::Checkbox("Skip Dead", &cfg_.skipDead);
            ImGui::SliderFloat("Max Dist", &cfg_.maxDistance, 0.f, 500.f, "%.0f m (0=inf)");

            Section("Colors");
            ImGui::ColorEdit4("Enemy##c", &cfg_.roleColor[1].x, ImGuiColorEditFlags_NoInputs);
            ImGui::SameLine();
            ImGui::ColorEdit4("Team##c", &cfg_.roleColor[2].x, ImGuiColorEditFlags_NoInputs);
            ImGui::SameLine();
            ImGui::ColorEdit4("Neutral##c", &cfg_.roleColor[3].x, ImGuiColorEditFlags_NoInputs);
            ImGui::EndTabItem();
        }

        // ---- AIM TAB ----
        if (ImGui::BeginTabItem("Aim")) {
            ImGui::Checkbox("Enable Aimbot", &aimCfg_.enabled);

            Section("Targeting");
            ImGui::SliderFloat("FOV", &aimCfg_.fov, 20.f, 400.f, "%.0f px");
            const char* bones[] = {"Head", "Neck", "Chest"};
            ImGui::Combo("Bone", &aimCfg_.bone, bones, 3);
            const char* prio[] = {"Crosshair", "Distance", "Lowest HP"};
            ImGui::Combo("Priority", &aimCfg_.priority, prio, 3);
            ImGui::Checkbox("Draw FOV", &aimCfg_.drawFov);

            Section("Silent Aim");
            ImGui::Checkbox("Bezier Smoothing", &aimCfg_.bezier);
            if (aimCfg_.bezier)
                ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.f), "  ease-out curve, natural acceleration");
            ImGui::Checkbox("Sticky Aim", &aimCfg_.stickyAim);
            ImGui::Checkbox("Target Prediction", &aimCfg_.prediction);
            if (aimCfg_.prediction)
                ImGui::SliderFloat("Prediction##str", &aimCfg_.predScale, 0.1f, 2.f, "%.2f");

            Section("Smoothing");
            ImGui::SliderFloat("Base Smooth", &aimCfg_.smooth, 1.f, 25.f, "%.1f");
            ImGui::SliderFloat("Near Smooth", &aimCfg_.smoothNear, 1.f, 30.f, "%.1f");
            ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.f), "  higher = slower near target");
            ImGui::SliderFloat("Max Speed", &aimCfg_.maxSpeed, 5.f, 100.f, "%.0f px/f");

            Section("Humanization");
            ImGui::SliderFloat("Aim Delay", &aimCfg_.aimDelay, 0.f, 0.3f, "%.3f s");
            ImGui::SliderFloat("Overshoot", &aimCfg_.overshoot, 0.f, 0.5f, "%.2f");
            ImGui::SliderFloat("Jitter", &aimCfg_.jitter, 0.f, 5.f, "%.1f px");

            Section("Triggerbot");
            ImGui::Checkbox("Enable##trig", &aimCfg_.triggerbot);
            if (aimCfg_.triggerbot) {
                ImGui::SliderFloat("Delay##trig", &aimCfg_.triggerDelay, 0.02f, 0.3f, "%.3f s");
                ImGui::SliderFloat("Radius##trig", &aimCfg_.triggerRadius, 2.f, 30.f, "%.0f px");
            }

            ImGui::Spacing();
            ImGui::TextColored(ImVec4(0.5f, 0.6f, 0.8f, 1.f), "Hold Right Click to aim");
            ImGui::EndTabItem();
        }

        // ---- VISUALS TAB ----
        if (ImGui::BeginTabItem("Visuals")) {
            Section("Crosshair");
            ImGui::Checkbox("Enable##xh", &miscCfg_.crosshair);
            if (miscCfg_.crosshair) {
                const char* styles[] = {"Dot", "Cross", "Circle", "Cross+Circle"};
                ImGui::Combo("Style##xh", &miscCfg_.crosshairStyle, styles, 4);
                ImGui::SliderFloat("Size##xh", &miscCfg_.crosshairSize, 2.f, 20.f, "%.0f");
                ImGui::SliderFloat("Thick##xh", &miscCfg_.crosshairThick, 1.f, 4.f, "%.1f");
                ImGui::ColorEdit4("Color##xh", &miscCfg_.crosshairColor.x, ImGuiColorEditFlags_NoInputs);
            }

            Section("HUD");
            ImGui::Checkbox("Watermark", &miscCfg_.watermark);
            ImGui::Checkbox("FPS Counter", &miscCfg_.fpsCounter);

            ImGui::EndTabItem();
        }

        // ---- WORLD TAB ----
        if (ImGui::BeginTabItem("World")) {
            Section("Radar");
            ImGui::Checkbox("Enable##radar", &worldCfg_.radar);
            if (worldCfg_.radar) {
                ImGui::SliderFloat("Size##r", &worldCfg_.radarSize, 60.f, 300.f, "%.0f px");
                ImGui::SliderFloat("Range##r", &worldCfg_.radarRange, 20.f, 200.f, "%.0f m");
            }

            Section("Proximity Alert");
            ImGui::Checkbox("Enable##prox", &worldCfg_.proximity);
            if (worldCfg_.proximity) {
                ImGui::SliderFloat("Dist##prox", &worldCfg_.proximityDist, 5.f, 50.f, "%.0f m");
                ImGui::ColorEdit4("Flash##prox", &worldCfg_.proximityColor.x,
                    ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar);
            }

            ImGui::EndTabItem();
        }

        // ---- CONFIG TAB ----
        if (ImGui::BeginTabItem("Config")) {
            Section("Game Offsets (offsets.cfg)");
            char hex[32];
            auto row = [&](const char* label, uintptr_t val) {
                std::snprintf(hex, sizeof hex, "0x%llX", static_cast<unsigned long long>(val));
                ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.7f, 1.f), "%-22s", label);
                ImGui::SameLine(180.f);
                ImGui::Text("%s", hex);
            };

            if (off_.battleModelTypeInfo == 0)
                ImGui::TextColored(ImVec4(0.4f, 0.8f, 0.4f, 1.f), "%-22s auto-detect", "battleModelTypeInfo");
            else
                row("battleModelTypeInfo", off_.battleModelTypeInfo);

            row("bmEntities", off_.bmEntities);
            row("bePlayer", off_.bePlayer);
            row("beModels", off_.beModels);
            row("entityServerModel", off_.entityServerModel);
            row("cmTransform", off_.cmTransform);
            row("cmIsDead", off_.cmIsDead);
            row("cmRole", off_.cmRole);

            Section("Scan State");
            ImGui::Text("BM: 0x%llX", static_cast<unsigned long long>(gmAddr_.load()));
            ImGui::Text("VM: 0x%llX", static_cast<unsigned long long>(viewAddr_.load()));
            ImGui::Text("Local: %s  |  Entities: %d",
                        haveLocal_.load() ? "yes" : "no",
                        static_cast<int>(entities_.size()));

            Section("Hotkeys");
            ImGui::BulletText("F5  Reload offsets.cfg");
            ImGui::BulletText("F6  Save settings");
            ImGui::BulletText("Insert  Toggle menu");
            ImGui::BulletText("End  Exit overlay");

            ImGui::EndTabItem();
        }

        ImGui::EndTabBar();
    }
}

// ============================================================
// Settings save/load
// ============================================================

bool Esp::SaveSettings(const std::string& path) const {
    std::ofstream f(path);
    if (!f.is_open()) return false;

    f << "# jb overlay settings (auto-generated)\n\n";

    auto wb = [&](const char* k, bool v)  { f << k << " = " << (v ? 1 : 0) << "\n"; };
    auto wi = [&](const char* k, int v)   { f << k << " = " << v << "\n"; };
    auto wf = [&](const char* k, float v) { char b[32]; std::snprintf(b, sizeof b, "%.4f", static_cast<double>(v)); f << k << " = " << b << "\n"; };
    auto wc = [&](const char* k, const ImVec4& v) {
        char b[64]; std::snprintf(b, sizeof b, "%.3f %.3f %.3f %.3f",
            static_cast<double>(v.x), static_cast<double>(v.y), static_cast<double>(v.z), static_cast<double>(v.w));
        f << k << " = " << b << "\n";
    };

    f << "# ESP\n";
    wb("esp.enabled", cfg_.enabled);
    wi("esp.boxStyle", cfg_.boxStyle);
    wb("esp.healthBar", cfg_.healthBar);
    wb("esp.healthText", cfg_.healthText);
    wb("esp.distance", cfg_.distance);
    wb("esp.tracer", cfg_.tracer);
    wi("esp.tracerPos", cfg_.tracerPos);
    wb("esp.name", cfg_.name);
    wb("esp.headDot", cfg_.headDot);
    wb("esp.skeleton", cfg_.skeleton);
    wb("esp.fovCircle", cfg_.fovCircle);
    wb("esp.skipLocal", cfg_.skipLocal);
    wb("esp.skipTeammate", cfg_.skipTeammate);
    wb("esp.skipDead", cfg_.skipDead);
    wf("esp.maxDistance", cfg_.maxDistance);
    wf("esp.fovRadius", cfg_.fovRadius);
    wf("esp.boxThickness", cfg_.boxThickness);
    wf("esp.tracerThickness", cfg_.tracerThickness);
    wc("esp.color.enemy", cfg_.roleColor[1]);
    wc("esp.color.team", cfg_.roleColor[2]);
    wc("esp.color.neutral", cfg_.roleColor[3]);

    f << "\n# Aim\n";
    wb("aim.enabled", aimCfg_.enabled);
    wf("aim.fov", aimCfg_.fov);
    wf("aim.smooth", aimCfg_.smooth);
    wf("aim.smoothNear", aimCfg_.smoothNear);
    wi("aim.bone", aimCfg_.bone);
    wi("aim.key", aimCfg_.key);
    wb("aim.drawFov", aimCfg_.drawFov);
    wb("aim.bezier", aimCfg_.bezier);
    wb("aim.stickyAim", aimCfg_.stickyAim);
    wb("aim.prediction", aimCfg_.prediction);
    wf("aim.predScale", aimCfg_.predScale);
    wi("aim.priority", aimCfg_.priority);
    wf("aim.aimDelay", aimCfg_.aimDelay);
    wf("aim.overshoot", aimCfg_.overshoot);
    wf("aim.jitter", aimCfg_.jitter);
    wf("aim.maxSpeed", aimCfg_.maxSpeed);
    wb("aim.triggerbot", aimCfg_.triggerbot);
    wf("aim.triggerDelay", aimCfg_.triggerDelay);
    wf("aim.triggerRadius", aimCfg_.triggerRadius);

    f << "\n# Visuals\n";
    wb("vis.crosshair", miscCfg_.crosshair);
    wi("vis.crosshairStyle", miscCfg_.crosshairStyle);
    wf("vis.crosshairSize", miscCfg_.crosshairSize);
    wf("vis.crosshairThick", miscCfg_.crosshairThick);
    wc("vis.crosshairColor", miscCfg_.crosshairColor);
    wb("vis.watermark", miscCfg_.watermark);
    wb("vis.fpsCounter", miscCfg_.fpsCounter);

    f << "\n# World\n";
    wb("world.radar", worldCfg_.radar);
    wf("world.radarSize", worldCfg_.radarSize);
    wf("world.radarRange", worldCfg_.radarRange);
    wb("world.proximity", worldCfg_.proximity);
    wf("world.proximityDist", worldCfg_.proximityDist);
    wc("world.proximityColor", worldCfg_.proximityColor);

    return true;
}

bool Esp::LoadSettings(const std::string& path) {
    std::ifstream f(path);
    if (!f.is_open()) return false;

    auto parseBool = [](const std::string& s) { return s == "1" || s == "true"; };
    auto parseColor = [](const std::string& s) -> ImVec4 {
        ImVec4 c{};
        std::sscanf(s.c_str(), "%f %f %f %f", &c.x, &c.y, &c.z, &c.w);
        return c;
    };

    std::string line;
    while (std::getline(f, line)) {
        if (line.empty() || line[0] == '#') continue;
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = line.substr(0, eq);
        std::string v = line.substr(eq + 1);
        while (!k.empty() && (k.back() == ' ' || k.back() == '\t')) k.pop_back();
        while (!v.empty() && (v.front() == ' ' || v.front() == '\t')) v.erase(v.begin());
        if (k.empty() || v.empty()) continue;

        // ESP
        if (k == "esp.enabled") cfg_.enabled = parseBool(v);
        else if (k == "esp.boxStyle") cfg_.boxStyle = std::atoi(v.c_str());
        else if (k == "esp.healthBar") cfg_.healthBar = parseBool(v);
        else if (k == "esp.healthText") cfg_.healthText = parseBool(v);
        else if (k == "esp.distance") cfg_.distance = parseBool(v);
        else if (k == "esp.tracer") cfg_.tracer = parseBool(v);
        else if (k == "esp.tracerPos") cfg_.tracerPos = std::atoi(v.c_str());
        else if (k == "esp.name") cfg_.name = parseBool(v);
        else if (k == "esp.headDot") cfg_.headDot = parseBool(v);
        else if (k == "esp.skeleton") cfg_.skeleton = parseBool(v);
        else if (k == "esp.fovCircle") cfg_.fovCircle = parseBool(v);
        else if (k == "esp.skipLocal") cfg_.skipLocal = parseBool(v);
        else if (k == "esp.skipTeammate") cfg_.skipTeammate = parseBool(v);
        else if (k == "esp.skipDead") cfg_.skipDead = parseBool(v);
        else if (k == "esp.maxDistance") cfg_.maxDistance = std::strtof(v.c_str(), nullptr);
        else if (k == "esp.fovRadius") cfg_.fovRadius = std::strtof(v.c_str(), nullptr);
        else if (k == "esp.boxThickness") cfg_.boxThickness = std::strtof(v.c_str(), nullptr);
        else if (k == "esp.tracerThickness") cfg_.tracerThickness = std::strtof(v.c_str(), nullptr);
        else if (k == "esp.color.enemy") cfg_.roleColor[1] = parseColor(v);
        else if (k == "esp.color.team") cfg_.roleColor[2] = parseColor(v);
        else if (k == "esp.color.neutral") cfg_.roleColor[3] = parseColor(v);
        // Aim
        else if (k == "aim.enabled") aimCfg_.enabled = parseBool(v);
        else if (k == "aim.fov") aimCfg_.fov = std::strtof(v.c_str(), nullptr);
        else if (k == "aim.smooth") aimCfg_.smooth = std::strtof(v.c_str(), nullptr);
        else if (k == "aim.smoothNear") aimCfg_.smoothNear = std::strtof(v.c_str(), nullptr);
        else if (k == "aim.bone") aimCfg_.bone = std::atoi(v.c_str());
        else if (k == "aim.key") aimCfg_.key = std::atoi(v.c_str());
        else if (k == "aim.drawFov") aimCfg_.drawFov = parseBool(v);
        else if (k == "aim.bezier") aimCfg_.bezier = parseBool(v);
        else if (k == "aim.stickyAim") aimCfg_.stickyAim = parseBool(v);
        else if (k == "aim.prediction") aimCfg_.prediction = parseBool(v);
        else if (k == "aim.predScale") aimCfg_.predScale = std::strtof(v.c_str(), nullptr);
        else if (k == "aim.priority") aimCfg_.priority = std::atoi(v.c_str());
        else if (k == "aim.aimDelay") aimCfg_.aimDelay = std::strtof(v.c_str(), nullptr);
        else if (k == "aim.overshoot") aimCfg_.overshoot = std::strtof(v.c_str(), nullptr);
        else if (k == "aim.jitter") aimCfg_.jitter = std::strtof(v.c_str(), nullptr);
        else if (k == "aim.maxSpeed") aimCfg_.maxSpeed = std::strtof(v.c_str(), nullptr);
        else if (k == "aim.triggerbot") aimCfg_.triggerbot = parseBool(v);
        else if (k == "aim.triggerDelay") aimCfg_.triggerDelay = std::strtof(v.c_str(), nullptr);
        else if (k == "aim.triggerRadius") aimCfg_.triggerRadius = std::strtof(v.c_str(), nullptr);
        // Visuals
        else if (k == "vis.crosshair") miscCfg_.crosshair = parseBool(v);
        else if (k == "vis.crosshairStyle") miscCfg_.crosshairStyle = std::atoi(v.c_str());
        else if (k == "vis.crosshairSize") miscCfg_.crosshairSize = std::strtof(v.c_str(), nullptr);
        else if (k == "vis.crosshairThick") miscCfg_.crosshairThick = std::strtof(v.c_str(), nullptr);
        else if (k == "vis.crosshairColor") miscCfg_.crosshairColor = parseColor(v);
        else if (k == "vis.watermark") miscCfg_.watermark = parseBool(v);
        else if (k == "vis.fpsCounter") miscCfg_.fpsCounter = parseBool(v);
        // World
        else if (k == "world.radar") worldCfg_.radar = parseBool(v);
        else if (k == "world.radarSize") worldCfg_.radarSize = std::strtof(v.c_str(), nullptr);
        else if (k == "world.radarRange") worldCfg_.radarRange = std::strtof(v.c_str(), nullptr);
        else if (k == "world.proximity") worldCfg_.proximity = parseBool(v);
        else if (k == "world.proximityDist") worldCfg_.proximityDist = std::strtof(v.c_str(), nullptr);
        else if (k == "world.proximityColor") worldCfg_.proximityColor = parseColor(v);
    }
    return true;
}

}  // namespace esp
