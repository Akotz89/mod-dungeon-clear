/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#ifndef _PLAYERBOT_DCENGINEACCESS_H
#define _PLAYERBOT_DCENGINEACCESS_H

#include "PlayerbotAI.h"

class Engine;

// Read-only access to PlayerbotAI::engines[] for the Pull Lab's executed-action
// trace (pull-lab plan §4.2). The array is protected with no public getter, and
// mod-playerbots is not edited (plan decision 2), so this names the member
// through a derived class: `&DcEngineAccess::engines` is a pointer-to-member of
// PlayerbotAI, and applying it to any PlayerbotAI is ordinary C++ — no cast, no
// layout assumption. DcEngineAccess is never instantiated.
//
// The cost is compile-time coupling to the member's NAME: an upstream rename
// breaks this file's build, loudly, instead of silently reading the wrong
// thing. That is the sanctioned exception to "public playerbots API only"
// (plan §8 decision 5); nothing else in the module may grow one of these.
//
// Lifetime (verified): the engines are created once in the PlayerbotAI
// constructor and deleted only by its destructor, so an Engine* read here is
// valid exactly as long as the PlayerbotAI it came from.
class DcEngineAccess : public PlayerbotAI
{
public:
    DcEngineAccess() = delete;

    static Engine* Get(PlayerbotAI* ai, BotState state)
    {
        if (!ai || state >= BOT_STATE_MAX)
            return nullptr;
        return (ai->*(&DcEngineAccess::engines))[state];
    }
};

#endif  // _PLAYERBOT_DCENGINEACCESS_H
