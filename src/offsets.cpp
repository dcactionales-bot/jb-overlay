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
        if (key == "cmTransform")    { out.cmTransform    = ParseHex(val); continue; }
        if (key == "cmIsDead")       { out.cmIsDead       = ParseHex(val); continue; }
        if (key == "cmRole")         { out.cmRole         = ParseHex(val); continue; }
        if (key == "cmPrivateModel") { out.cmPrivateModel = ParseHex(val); continue; }

        // Transform position
        if (key == "txX") { out.txX = ParseHex(val); continue; }
        if (key == "txY") { out.txY = ParseHex(val); continue; }
        if (key == "txZ") { out.txZ = ParseHex(val); continue; }

        // IPrimitiveModel double deref
        if (key == "primitiveData")  { out.primitiveData  = ParseHex(val); continue; }
        if (key == "primitiveValue") { out.primitiveValue = ParseHex(val); continue; }

        // Health (via PrivateModel)
        if (key == "pmHealth")      { out.pmHealth      = ParseHex(val); continue; }
        if (key == "healthValue")   { out.healthValue   = ParseHex(val); continue; }
        if (key == "healthMax")     { out.healthMax     = ParseHex(val); continue; }
        if (key == "healthIsFloat") { out.healthIsFloat = (val == "true" || val == "1"); continue; }

        // Name slots
        if (key == "bmSlots")      { out.bmSlots      = ParseHex(val); continue; }
        if (key == "slotsArray")   { out.slotsArray   = ParseHex(val); continue; }
        if (key == "slotNickname") { out.slotNickname = ParseHex(val); continue; }
        if (key == "slotData")     { out.slotData     = ParseHex(val); continue; }
        if (key == "slotEntityId") { out.slotEntityId = ParseHex(val); continue; }
        if (key == "cmId")         { out.cmId         = ParseHex(val); continue; }

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

    f << "# ROB RUN offsets — Build 26509119\n";
    f << "# Chain: BattleModel -> BattleEntitiesModel -> entity -> BattleCharacterModel\n";
    f << "# F5 to hot-reload.\n\n";

    f << "# --- BattleModel TypeInfo RVA in GameAssembly.dll ---\n";
    f << "battleModelTypeInfo = 0x82496E8\n\n";

    f << "# --- BattleModel -> BattleEntitiesModel ---\n";
    f << "bmEntities = 0x90\n\n";

    f << "# --- BattleEntitiesModel ---\n";
    f << "bePlayer = 0xD0\n";
    f << "beModels = 0xE0\n\n";

    f << "# --- entity -> BattleCharacterModel (ServerModel) ---\n";
    f << "localServerModel  = 0x20\n";
    f << "entityServerModel = 0x28\n\n";

    f << "# --- BattleCharacterModel fields ---\n";
    f << "cmTransform    = 0x58\n";
    f << "cmIsDead       = 0xF0\n";
    f << "cmRole         = 0x208\n";
    f << "cmPrivateModel = 0x10\n";
    f << "cmId           = 0x38\n\n";

    f << "# --- CharacterTransformModel -> XYZ ---\n";
    f << "txX = 0x10\n";
    f << "txY = 0x14\n";
    f << "txZ = 0x18\n\n";

    f << "# --- IPrimitiveModel<T> double deref ---\n";
    f << "# PrimitiveModel._data -> PrimitiveData.value\n";
    f << "primitiveData  = 0x10\n";
    f << "primitiveValue = 0x18\n\n";

    f << "# --- Health (PrivateModel -> HealthModel) ---\n";
    f << "pmHealth      = 0x1B0\n";
    f << "healthValue   = 0x10\n";
    f << "healthMax     = 0x20\n";
    f << "healthIsFloat = true\n\n";

    f << "# --- Name slots ---\n";
    f << "bmSlots      = 0x4D0\n";
    f << "slotsArray   = 0x18\n";
    f << "slotData     = 0x10\n";
    f << "slotEntityId = 0x20\n";
    f << "slotNickname = 0x30\n\n";

    f << "# --- C# internals ---\n";
    f << "dictEntries    = 0x18\n";
    f << "dictCount      = 0x20\n";
    f << "dictEntrySize  = 0x18\n";
    f << "dictEntryValue = 0x10\n";
    f << "arrayData      = 0x20\n\n";

    f << "# --- Gameplay tuning ---\n";
    f << "metersPerUnit = 1.0\n";
    f << "headHeight    = 1.75\n";
    f << "playerHeight  = 1.8\n\n";

    f << "# --- PlayerRole enum ---\n";
    f << "copRole    = 4\n";
    f << "robberRole = 3\n";

    return true;
}

}  // namespace cfg
