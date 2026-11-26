#include "offsets.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>

namespace cfg {
namespace {

uintptr_t ParseHex(const std::string& s) {
    return static_cast<uintptr_t>(std::strtoull(s.c_str(), nullptr, 0));
}

std::string Trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return {};
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

}  // namespace

bool LoadOffsets(const std::string& path, Offsets& out) {
    std::ifstream f(path);
    if (!f.is_open()) return false;

    std::string line;
    while (std::getline(f, line)) {
        line = Trim(line);
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = Trim(line.substr(0, eq));
        std::string val = Trim(line.substr(eq + 1));
        if (key.empty() || val.empty()) continue;

        // BattleModel singleton
        if (key == "battleModelTypeInfo") { out.battleModelTypeInfo = ParseHex(val); continue; }

        // BattleModel → BattleEntitiesModel
        if (key == "bmEntities") { out.bmEntities = ParseHex(val); continue; }

        // BattleEntitiesModel fields
        if (key == "bePlayer") { out.bePlayer = ParseHex(val); continue; }
        if (key == "beModels") { out.beModels = ParseHex(val); continue; }

        // Entity → ServerModel
        if (key == "localServerModel")  { out.localServerModel  = ParseHex(val); continue; }
        if (key == "entityServerModel") { out.entityServerModel = ParseHex(val); continue; }

        // BattleCharacterModel fields
        if (key == "cmTransform") { out.cmTransform = ParseHex(val); continue; }
        if (key == "cmIsDead")    { out.cmIsDead    = ParseHex(val); continue; }
        if (key == "cmRole")      { out.cmRole      = ParseHex(val); continue; }
        if (key == "cmHealth")    { out.cmHealth    = ParseHex(val); continue; }

        // Transform position
        if (key == "txX") { out.txX = ParseHex(val); continue; }
        if (key == "txY") { out.txY = ParseHex(val); continue; }
        if (key == "txZ") { out.txZ = ParseHex(val); continue; }

        // IPrimitiveModel value
        if (key == "primitiveValue") { out.primitiveValue = ParseHex(val); continue; }

        // Health component
        if (key == "healthValue")   { out.healthValue   = ParseHex(val); continue; }
        if (key == "healthMax")     { out.healthMax      = ParseHex(val); continue; }
        if (key == "healthIsFloat") { out.healthIsFloat  = (val == "true" || val == "1"); continue; }

        // Name slots
        if (key == "beSlots")      { out.beSlots      = ParseHex(val); continue; }
        if (key == "slotsDict")    { out.slotsDict    = ParseHex(val); continue; }
        if (key == "slotNickname") { out.slotNickname = ParseHex(val); continue; }

        // Dictionary internals
        if (key == "dictEntries")    { out.dictEntries    = ParseHex(val); continue; }
        if (key == "dictCount")      { out.dictCount      = ParseHex(val); continue; }
        if (key == "dictEntrySize")  { out.dictEntrySize  = ParseHex(val); continue; }
        if (key == "dictEntryValue") { out.dictEntryValue = ParseHex(val); continue; }
        if (key == "arrayData")      { out.arrayData      = ParseHex(val); continue; }

        // Tuning
        if (key == "metersPerUnit") { out.metersPerUnit = std::strtof(val.c_str(), nullptr); continue; }
        if (key == "headHeight")    { out.headHeight    = std::strtof(val.c_str(), nullptr); continue; }
        if (key == "playerHeight")  { out.playerHeight  = std::strtof(val.c_str(), nullptr); continue; }

        // Role config
        if (key == "copRole")    { out.copRole    = static_cast<int>(std::strtol(val.c_str(), nullptr, 0)); continue; }
        if (key == "robberRole") { out.robberRole = static_cast<int>(std::strtol(val.c_str(), nullptr, 0)); continue; }
    }
    return true;
}

bool SaveDefaultOffsets(const std::string& path) {
    std::ofstream f(path);
    if (!f.is_open()) return false;

    f << "# ROB RUN offsets config\n";
    f << "# Data chain: BattleModel -> BattleEntitiesModel -> entity models\n";
    f << "#   -> BattleCharacterModel -> Transform/IsDead/Role\n";
    f << "# All hex values use 0x prefix.  F5 to hot-reload.\n";
    f << "#\n";
    f << "# IMPORTANT: offsets below 0x0 are PLACEHOLDERS.\n";
    f << "# Run tools/calc_offsets.ps1 on Cpp2IL dummy DLLs to compute them.\n";
    f << "# If positions look wrong, nudge suspect offset by +/-0x08 and F5.\n";
    f << "\n";

    f << "# --- Step 1: BattleModel TypeInfo (0 = auto-detect by class name) ---\n";
    f << "battleModelTypeInfo = 0x0\n";
    f << "\n";

    f << "# --- Step 2: BattleModel -> BattleEntitiesModel ---\n";
    f << "bmEntities = 0x0\n";
    f << "\n";

    f << "# --- Step 3: BattleEntitiesModel ---\n";
    f << "bePlayer = 0x0\n";
    f << "beModels = 0x0\n";
    f << "\n";

    f << "# --- Step 4: entity model -> BattleCharacterModel ---\n";
    f << "# PlayerEntityModel.ServerModel offset (for local player)\n";
    f << "localServerModel  = 0x0\n";
    f << "# BattleCharacterEntityModel.ServerModel offset (for all chars)\n";
    f << "entityServerModel = 0x0\n";
    f << "\n";

    f << "# --- Step 5: BattleCharacterModel fields ---\n";
    f << "# All are pointers to sub-objects; read the pointer, then the value\n";
    f << "cmTransform = 0x0\n";
    f << "cmIsDead    = 0x0\n";
    f << "cmRole      = 0x0\n";
    f << "cmHealth    = 0x0\n";
    f << "\n";

    f << "# --- Step 6: CharacterTransformModel -> XYZ ---\n";
    f << "txX = 0x10\n";
    f << "txY = 0x14\n";
    f << "txZ = 0x18\n";
    f << "\n";

    f << "# --- Step 7: IPrimitiveModel<T> -> value ---\n";
    f << "# IsDead and Role are wrapped in IPrimitiveModel, need extra deref\n";
    f << "primitiveValue = 0x10\n";
    f << "\n";

    f << "# --- Health (optional, 0 = skip health bars) ---\n";
    f << "healthValue   = 0x10\n";
    f << "healthMax     = 0x14\n";
    f << "healthIsFloat = false\n";
    f << "\n";

    f << "# --- Name slots (optional, all 0 = skip names) ---\n";
    f << "beSlots      = 0x0\n";
    f << "slotsDict    = 0x0\n";
    f << "slotNickname = 0x0\n";
    f << "\n";

    f << "# --- C# Dictionary internals (usually don't change) ---\n";
    f << "dictEntries    = 0x18\n";
    f << "dictCount      = 0x20\n";
    f << "dictEntrySize  = 0x18\n";
    f << "dictEntryValue = 0x10\n";
    f << "arrayData      = 0x20\n";
    f << "\n";

    f << "# --- Gameplay tuning ---\n";
    f << "metersPerUnit = 1.0\n";
    f << "headHeight    = 1.75\n";
    f << "playerHeight  = 1.8\n";
    f << "\n";

    f << "# --- PlayerRole enum ---\n";
    f << "# None=0, Citizen=1, Prisoner=2, Robber=3, Cop=4, Settler=5\n";
    f << "copRole    = 4\n";
    f << "robberRole = 3\n";

    return true;
}

}  // namespace cfg
