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
    uintptr_t cmTransform    = 0;  // → CharacterTransformModel*
    uintptr_t cmIsDead       = 0;  // → IPrimitiveModel<bool>*
    uintptr_t cmRole         = 0;  // → IPrimitiveModel<PlayerRole>*
    uintptr_t cmPrivateModel = 0;  // → BattleCharacterPrivateModel* (0 = skip health)

    // === Step 6: CharacterTransformModel fields ===
    uintptr_t txX = 0x10;
    uintptr_t txY = 0x14;
    uintptr_t txZ = 0x18;

    // === Step 7: IPrimitiveModel<T> double dereference ===
    uintptr_t primitiveData  = 0x10;  // PrimitiveModel → _data (PrimitiveData*)
    uintptr_t primitiveValue = 0x18;  // PrimitiveData → value

    // === Health (via PrivateModel, used only if cmPrivateModel != 0) ===
    uintptr_t pmHealth      = 0;     // BattleCharacterPrivateModel → HealthModel*
    uintptr_t healthValue   = 0x10;
    uintptr_t healthMax     = 0x20;
    bool      healthIsFloat = true;

    // === Name reading from slots (all 0 = skip) ===
    uintptr_t bmSlots      = 0;  // BattleModel → BattleCharacterSlotsModel
    uintptr_t slotsArray   = 0;  // BattleCharacterSlotsModel → Slots (list)
    uintptr_t slotNickname = 0;  // BattleCharacterSlotData → Nickname (C# string)
    uintptr_t slotData     = 0;  // BattleCharacterSlotModel → _data
    uintptr_t slotEntityId = 0;  // BattleCharacterSlotData → EntityId
    uintptr_t cmId         = 0;  // BattleCharacterModel → Id

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

    // === Target process name (overrides built-in default) ===
    wchar_t targetProcess[64]{};
};

bool LoadOffsets(const std::string& path, Offsets& out);
bool SaveDefaultOffsets(const std::string& path);

}  // namespace cfg
