/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#ifndef _DC_TEST_BLACKROCKSPIREROOKERYEGGS_H
#define _DC_TEST_BLACKROCKSPIREROOKERYEGGS_H

// The Rookery Eggs (GO 175124, map 229) on the Rookery floor's NORTH half (z < 101,
// y > -285): every egg the north lane passes, and the five clusters
// DcNavPenaltyRegistry boxes. From data/sql/base/db_world/gameobject.sql; the
// south half (Father Flame's side) is not routed through and is left out.
namespace DcRookeryEggs
{
    struct Egg { float x, y, z; };

    constexpr Egg NORTH_FLOOR[] = {
        { 54.27f, -255.44f, 96.86f },
        { 55.35f, -249.98f, 97.05f },
        { 55.44f, -254.35f, 97.20f },
        { 56.85f, -253.34f, 96.96f },
        { 56.88f, -255.38f, 96.97f },
        { 57.86f, -256.66f, 96.74f },
        { 58.12f, -253.95f, 96.88f },
        { 59.03f, -271.14f, 93.64f },
        { 59.17f, -255.10f, 96.82f },
        { 60.19f, -270.20f, 93.68f },
        { 60.66f, -268.14f, 93.60f },
        { 61.32f, -272.01f, 93.60f },
        { 64.20f, -268.47f, 93.25f },
        { 65.30f, -267.93f, 93.08f },
        { 71.79f, -278.17f, 92.46f },
        { 72.07f, -276.63f, 92.30f },
        { 74.21f, -273.59f, 91.64f },
        { 74.75f, -275.85f, 91.65f },
        { 76.24f, -277.38f, 91.47f },
        { 76.65f, -263.57f, 91.48f },
        { 76.86f, -275.63f, 91.48f },
        { 77.28f, -262.14f, 91.48f },
        { 78.59f, -266.50f, 91.47f },
        { 79.21f, -262.50f, 91.48f },
        { 80.79f, -253.59f, 91.48f },
        { 81.01f, -264.96f, 91.47f },
        { 82.08f, -251.81f, 91.47f },
        { 82.76f, -267.75f, 91.47f },
        { 82.96f, -249.99f, 91.47f },
        { 83.65f, -252.41f, 91.47f },
        { 84.11f, -268.80f, 91.46f },
        { 84.51f, -248.38f, 91.47f },
        { 85.98f, -250.81f, 91.46f },
        { 86.37f, -246.97f, 91.46f },
        { 87.23f, -252.47f, 91.46f },
        { 87.28f, -254.39f, 91.46f },
        { 87.41f, -278.12f, 91.45f },
        { 87.92f, -280.00f, 91.45f },
        { 88.34f, -248.34f, 91.46f },
        { 89.00f, -251.21f, 91.46f },
        { 92.13f, -279.85f, 91.45f },
        { 94.56f, -281.09f, 91.45f },
        { 96.34f, -280.52f, 91.45f },
        { 97.13f, -281.95f, 91.45f },
        { 106.42f, -281.43f, 91.51f },
        { 106.60f, -283.78f, 91.51f },
        { 107.42f, -282.51f, 91.52f },
    };
}

#endif
