/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "Creature.h"
#include "ScriptMgr.h"
#include "Unit.h"

#include "Lab/DcLabJob.h"
#include "Lab/DcLabRecorder.h"

// Pull Lab trace hooks: first damage per (attacker, victim), healing totals and
// the exact creature-engages-party moment. Each is one atomic load and a return
// unless a Lab recorder is armed (DcLabHub::Active), and none of them changes
// the value it is handed.
class DungeonClearLabTraceScript : public UnitScript
{
public:
    DungeonClearLabTraceScript()
        : UnitScript("DungeonClearLabTraceScript", true, {
            UNITHOOK_ON_DAMAGE,
            UNITHOOK_ON_HEAL,
            UNITHOOK_ON_UNIT_ENTER_COMBAT
        }) {}

    void OnDamage(Unit* attacker, Unit* victim, uint32& damage) override
    {
        if (DcLabHub::Active())
            DcLabHub::Damage(attacker, victim, damage);
    }

    void OnHeal(Unit* healer, Unit* reciever, uint32& gain) override
    {
        if (DcLabHub::Active())
            DcLabHub::Heal(healer, reciever, gain);
    }

    void OnUnitEnterCombat(Unit* unit, Unit* victim) override
    {
        if (DcLabHub::Active())
            DcLabHub::Engage(unit, victim);
    }
};

// A creature entering the world in a Lab instance after staging (its grid just
// loaded as the party moved): hidden like the rest unless the scenario keeps it.
// One map lookup under a mutex, and only while a Lab instance exists at all.
class DungeonClearLabStageScript : public AllCreatureScript
{
public:
    DungeonClearLabStageScript() : AllCreatureScript("DungeonClearLabStageScript") {}

    void OnCreatureAddWorld(Creature* creature) override
    {
        DcLabJob::HideOnAdd(creature);
    }
};

void AddSC_dungeon_clear_lab()
{
    new DungeonClearLabTraceScript();
    new DungeonClearLabStageScript();
}
