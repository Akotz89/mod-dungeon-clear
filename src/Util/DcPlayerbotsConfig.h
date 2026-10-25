/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#ifndef _PLAYERBOT_DCPLAYERBOTSCONFIG_H
#define _PLAYERBOT_DCPLAYERBOTSCONFIG_H

#include "PlayerbotAIConfig.h"

// mod-playerbots #2854 renamed every PlayerbotAIConfig member to UpperCamelCase
// (`reactDelay` -> `ReactDelay`). test-staging has it, master does not yet, and
// the module must build against both, so every config read goes through
// DC_PB_CONFIG(NewName, oldName): an lvalue reference to whichever member the
// playerbots being compiled against declares. The discarded branch is never
// instantiated, so the missing name is not an error. The sPlayerbotAIConfig
// macro already hides the instance() -> Instance() rename.
//
// Once master carries the rename, replace DC_PB_CONFIG(X, x) with
// sPlayerbotAIConfig.X and delete this header.
#define DC_PB_CONFIG(NewName, oldName)                \
    ([]<typename C>(C& c) -> auto& {                  \
        if constexpr (requires { c.NewName; })        \
            return c.NewName;                         \
        else                                          \
            return c.oldName;                         \
    }(sPlayerbotAIConfig))

#endif
