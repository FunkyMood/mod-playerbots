/*
 * Copyright (C) 2026
 *
 * This file is part of mod-playerbots and is released under the AGPLv3.
 */

#ifndef PLAYERBOTS_CITYBOTMGR_H
#define PLAYERBOTS_CITYBOTMGR_H

#include "Position.h"

class Player;
class WorldObject;

class CityBotMgr
{
public:
    static CityBotMgr& instance()
    {
        static CityBotMgr instance;
        return instance;
    }

    bool SelectHub(Player* bot, WorldLocation& location);
    bool IsInCity(Player const* bot) const;
    bool IsTargetAllowed(Player* bot, WorldObject const* target);
    void Update(uint32 diff);

private:
    CityBotMgr() = default;

    void BuildHubs();
    void AssignHub(Player* bot);
    void ReturnToCity(Player* bot);

    bool _hubsBuilt = false;
    uint32 _updateTimer = 0;
};

#define sCityBotMgr CityBotMgr::instance()

#endif
