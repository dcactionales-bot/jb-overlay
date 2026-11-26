#pragma once
#include <cstdint>
#include <string>

namespace cfg {

struct Offsets {
    // === Step 1: BattleModel singleton (TypeInfo ptr in GameAssembly.dll) ===
    uintptr_t battleModelTypeInfo = 0;

    // === Step 2: BattleModel → BattleEntitiesModel ===
    uintptr_t bmEntities = 0;

    // === Step 3: BattleEntitiesModel fields ===
    uintptr_t bePlayer = 0;     // → PlayerEntityModel (local player)
    uintptr_t beModels = 0;     // → Dictionary<int, BattleCharacterEntityModel>

    // === Step 4: entity model → BattleCharacterModel (ServerModel) ===
    uintptr_t localServerModel  = 0;  // PlayerEntityModel → ServerModel
    uintptr_t entityServerModel = 0;  // BattleCharacterEntityModel → ServerModel

    // === Step 5: BattleCharacterModel fields (all pointer indirection) ===
    uintptr_t cmTransform = 0;  // → CharacterTransformModel*
    uintptr_t cmIsDead    = 0;  // → IPrimitiveModel<bool>*
    uintptr_t cmRole      = 0;  // → IPrimitiveModel<PlayerRole>*
    uintptr_t cmHealth    = 0;  // → health component* (0 = skip)

    // === Step 6: CharacterTransformModel fields ===
    uintptr_t txX = 0x10;
    uintptr_t txY = 0x14;
    uintptr_t txZ = 0x18;

    // === Step 7: IPrimitiveModel<T> → value offset ===
    uintptr_t primitiveValue = 0x10;

    // === Health component fields (used only if cmHealth != 0) ===
    uintptr_t healthValue   = 0x10;
    uintptr_t healthMax     = 0x14;
    bool      healthIsFloat = false;

    // === Name reading from slots (all 0 = skip) ===
    uintptr_t beSlots      = 0;  // BattleEntitiesModel → BattleCharacterSlotsModel
    uintptr_t slotsDict    = 0;  // → _slots Dictionary<int, BattleCharacterSlotModel>
    uintptr_t slotNickname = 0;  // BattleCharacterSlotModel → Nickname (C# string)

    // === C# Dictionary<K,V> internals (rarely change) ===
    uintptr_t dictEntries    = 0x18;
    uintptr_t dictCount      = 0x20;
    uintptr_t dictEntrySize  = 0x18;
    uintptr_t dictEntryValue = 0x10;
    uintptr_t arrayData      = 0x20;

    // === Gameplay tuning ===
    float metersPerUnit = 1.0f;
    float headHeight    = 1.75f;
    float playerHeight  = 1.8f;

    // === PlayerRole enum: None=0 Citizen=1 Prisoner=2 Robber=3 Cop=4 Settler=5 ===
    int copRole    = 4;
    int robberRole = 3;
};

bool LoadOffsets(const std::string& path, Offsets& out);
bool SaveDefaultOffsets(const std::string& path);

}  // namespace cfg
