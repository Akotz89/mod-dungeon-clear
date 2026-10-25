/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#ifndef _PLAYERBOT_DCLABTAP_H
#define _PLAYERBOT_DCLABTAP_H

#include <atomic>
#include <cstdint>

// Pull Lab trace tap (src/Lab). The few pull-pipeline facts a sampled trace
// cannot see exactly — a phase write that reverts inside one sample, the
// governor's verdict, the moment a follower is put passive — are reported here.
//
// The dependency points one way: DC code calls these, and only the Lab installs
// a Hooks table. With no table installed (every normal run) each call is one
// relaxed atomic load and a branch, and nothing about the pull FSM changes
// whether a table is installed or not — the hooks only observe.
//
// Engine-free (stdlib only) so DcPullContext.h can include it and the gtest
// target links it unchanged.
namespace DcLabTap
{
    struct Hooks
    {
        // A DcPullContext phase write. `ctx` identifies the context (the Lab maps
        // context addresses to bots when it arms); from/to are DcPullPhase values.
        void (*phase)(void const* ctx, std::uint32_t from, std::uint32_t to, std::uint32_t nowMs) = nullptr;
        // A governor (DecidePull) verdict for the leader `guid`. Called every
        // governor tick; the recorder keeps only changes.
        void (*verdict)(std::uint64_t guid, char const* verdict, std::uint32_t predicted,
                        std::uint32_t ceiling) = nullptr;
        // DcFollowerLifecycle put `guid` passive (on) or released it (off).
        void (*passive)(std::uint64_t guid, bool on) = nullptr;
    };

    inline std::atomic<Hooks const*> g_hooks{nullptr};

    inline void Install(Hooks const* h) { g_hooks.store(h, std::memory_order_release); }

    inline void Phase(void const* ctx, std::uint32_t from, std::uint32_t to, std::uint32_t nowMs)
    {
        if (Hooks const* h = g_hooks.load(std::memory_order_acquire))
            if (h->phase && from != to)
                h->phase(ctx, from, to, nowMs);
    }

    inline void Verdict(std::uint64_t guid, char const* verdict, std::uint32_t predicted,
                        std::uint32_t ceiling)
    {
        if (Hooks const* h = g_hooks.load(std::memory_order_acquire))
            if (h->verdict)
                h->verdict(guid, verdict, predicted, ceiling);
    }

    inline void Passive(std::uint64_t guid, bool on)
    {
        if (Hooks const* h = g_hooks.load(std::memory_order_acquire))
            if (h->passive)
                h->passive(guid, on);
    }
}

#endif  // _PLAYERBOT_DCLABTAP_H
