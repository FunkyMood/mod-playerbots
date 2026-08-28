/*
 * Copyright (C) 2026
 *
 * This file is part of mod-playerbots and is released under the AGPLv3.
 */

#include "CityBotMgr.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <ctime>
#include <limits>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "AreaDefines.h"
#include "CreatureData.h"
#include "EmoteAction.h"
#include "Log.h"
#include "MapMgr.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "PlayerbotAIConfig.h"
#include "Random.h"
#include "RandomPlayerbotMgr.h"
#include "Object.h"

namespace
{
struct CityDefinition
{
    uint32 zoneId;
    TeamId team;
    char const* name;
};

constexpr std::array<CityDefinition, 10> Cities = {{
    { AREA_STORMWIND_CITY, TEAM_ALLIANCE, "Stormwind" },
    { AREA_IRONFORGE, TEAM_ALLIANCE, "Ironforge" },
    { AREA_DARNASSUS, TEAM_ALLIANCE, "Darnassus" },
    { AREA_THE_EXODAR, TEAM_ALLIANCE, "Exodar" },
    { AREA_ORGRIMMAR, TEAM_HORDE, "Orgrimmar" },
    { AREA_UNDERCITY, TEAM_HORDE, "Undercity" },
    { AREA_THUNDER_BLUFF, TEAM_HORDE, "Thunder Bluff" },
    { AREA_SILVERMOON_CITY, TEAM_HORDE, "Silvermoon" },
    { AREA_SHATTRATH_CITY, TEAM_NEUTRAL, "Shattrath" },
    { AREA_DALARAN, TEAM_NEUTRAL, "Dalaran" }
}};

struct CityHub
{
    CityDefinition const* city;
    WorldLocation location;
};

struct Assignment
{
    uint32 hubIndex;
    bool wasGrouped;
    time_t returnAt;
    bool walking;
};

std::vector<CityHub> CityHubs;
std::unordered_map<ObjectGuid::LowType, Assignment> Assignments;

CityDefinition const* GetCity(uint32 zoneId)
{
    auto const itr = std::find_if(Cities.begin(), Cities.end(), [zoneId](CityDefinition const& city) {
        return city.zoneId == zoneId;
    });

    return itr != Cities.end() ? &*itr : nullptr;
}

int GetCityWeight(uint32 zoneId)
{
    switch (zoneId)
    {
        case AREA_STORMWIND_CITY: return sPlayerbotAIConfig.weightTeleToStormwind;
        case AREA_IRONFORGE: return sPlayerbotAIConfig.weightTeleToIronforge;
        case AREA_DARNASSUS: return sPlayerbotAIConfig.weightTeleToDarnassus;
        case AREA_THE_EXODAR: return sPlayerbotAIConfig.weightTeleToExodar;
        case AREA_ORGRIMMAR: return sPlayerbotAIConfig.weightTeleToOrgrimmar;
        case AREA_UNDERCITY: return sPlayerbotAIConfig.weightTeleToUndercity;
        case AREA_THUNDER_BLUFF: return sPlayerbotAIConfig.weightTeleToThunderBluff;
        case AREA_SILVERMOON_CITY: return sPlayerbotAIConfig.weightTeleToSilvermoonCity;
        case AREA_SHATTRATH_CITY: return sPlayerbotAIConfig.weightTeleToShattrathCity;
        case AREA_DALARAN: return sPlayerbotAIConfig.weightTeleToDalaran;
        default: return 0;
    }
}

bool IsEligible(Player const* bot, CityDefinition const& city)
{
    if (GetCityWeight(city.zoneId) <= 0)
        return false;

    if (city.team != TEAM_NEUTRAL && city.team != bot->GetTeamId())
        return false;

    if (city.zoneId == AREA_SHATTRATH_CITY)
        return bot->GetLevel() >= sPlayerbotAIConfig.cityLifeShattrathMinLevel &&
               bot->GetLevel() <= sPlayerbotAIConfig.cityLifeShattrathMaxLevel;

    if (city.zoneId == AREA_DALARAN)
        return bot->GetLevel() >= sPlayerbotAIConfig.cityLifeDalaranMinLevel;

    // Every other capital is a low-level hub. Without this cap a bot past
    // Shattrath's own level range - or one whose faction's neutral hub is
    // disabled - is still eligible for its home capital, which is what let
    // high-level bots sit in Stormwind instead of being routed onward.
    return bot->GetLevel() <= sPlayerbotAIConfig.cityLifeCapitalMaxLevel;
}

bool IsServiceNpc(CreatureTemplate const* creatureTemplate)
{
    if (!creatureTemplate)
        return false;

    NPCFlags const flags = NPCFlags(creatureTemplate->npcflag);
    return flags & (UNIT_NPC_FLAG_INNKEEPER | UNIT_NPC_FLAG_GOSSIP | UNIT_NPC_FLAG_QUESTGIVER |
                    UNIT_NPC_FLAG_FLIGHTMASTER | UNIT_NPC_FLAG_BANKER | UNIT_NPC_FLAG_GUILD_BANKER |
                    UNIT_NPC_FLAG_TRAINER_CLASS | UNIT_NPC_FLAG_TRAINER_PROFESSION | UNIT_NPC_FLAG_VENDOR_AMMO |
                    UNIT_NPC_FLAG_VENDOR_FOOD | UNIT_NPC_FLAG_VENDOR_POISON | UNIT_NPC_FLAG_VENDOR_REAGENT |
                    UNIT_NPC_FLAG_AUCTIONEER | UNIT_NPC_FLAG_STABLEMASTER | UNIT_NPC_FLAG_PETITIONER |
                    UNIT_NPC_FLAG_TABARDDESIGNER | UNIT_NPC_FLAG_BATTLEMASTER | UNIT_NPC_FLAG_TRAINER |
                    UNIT_NPC_FLAG_VENDOR | UNIT_NPC_FLAG_REPAIR);
}

uint32 CountAssignments(uint32 hubIndex)
{
    return uint32(std::count_if(Assignments.begin(), Assignments.end(), [hubIndex](auto const& pair) {
        return pair.second.hubIndex == hubIndex;
    }));
}

uint32 CountAssignments(CityDefinition const* city)
{
    return uint32(std::count_if(Assignments.begin(), Assignments.end(), [city](auto const& pair) {
        return pair.second.hubIndex < CityHubs.size() && CityHubs[pair.second.hubIndex].city == city;
    }));
}
} // namespace

void CityBotMgr::BuildHubs()
{
    if (_hubsBuilt)
        return;

    _hubsBuilt = true;
    CityHubs.clear();

    for (auto const& pair : sObjectMgr->GetAllCreatureData())
    {
        CreatureData const& data = pair.second;
        CreatureTemplate const* creatureTemplate = sObjectMgr->GetCreatureTemplate(data.id);
        if (!IsServiceNpc(creatureTemplate))
            continue;

        uint32 const zoneId = sMapMgr->GetZoneId(data.phaseMask, data.mapid, data.posX, data.posY, data.posZ);
        CityDefinition const* city = GetCity(zoneId);
        if (!city)
            continue;

        CityHubs.push_back({ city, WorldLocation(data.mapid, data.posX, data.posY, data.posZ, data.orientation) });
    }

    LOG_INFO("playerbots.citylife", "Loaded {} city-life hubs across {} capitals", CityHubs.size(), Cities.size());
}

bool CityBotMgr::SelectHub(Player* bot, WorldLocation& location)
{
    if (!sPlayerbotAIConfig.enableCityLife || !bot || bot->GetGroup())
        return false;

    BuildHubs();
    if (CityHubs.empty())
        return false;

    std::vector<CityDefinition const*> eligibleCities;
    for (CityDefinition const& city : Cities)
    {
        bool const hasHubs = std::any_of(CityHubs.begin(), CityHubs.end(), [&city](CityHub const& hub) {
            return hub.city == &city;
        });
        if (IsEligible(bot, city) && hasHubs)
            eligibleCities.push_back(&city);
    }

    if (eligibleCities.empty())
        return false;

    uint32 lowestCityPopulation = std::numeric_limits<uint32>::max();
    for (CityDefinition const* city : eligibleCities)
        lowestCityPopulation = std::min(lowestCityPopulation, CountAssignments(city));

    auto const isNotLeastPopulated = [lowestCityPopulation](CityDefinition const* city) {
        return CountAssignments(city) != lowestCityPopulation;
    };
    eligibleCities.erase(
        std::remove_if(eligibleCities.begin(), eligibleCities.end(), isNotLeastPopulated),
        eligibleCities.end());

    CityDefinition const* city = eligibleCities[urand(0, eligibleCities.size() - 1)];
    std::vector<uint32> candidateHubs;
    uint32 lowestHubPopulation = std::numeric_limits<uint32>::max();
    for (uint32 index = 0; index < CityHubs.size(); ++index)
    {
        if (CityHubs[index].city != city)
            continue;

        uint32 const population = CountAssignments(index);
        if (population < sPlayerbotAIConfig.cityLifeHubCapacity)
            lowestHubPopulation = std::min(lowestHubPopulation, population);
    }

    for (uint32 index = 0; index < CityHubs.size(); ++index)
    {
        if (CityHubs[index].city == city && CountAssignments(index) == lowestHubPopulation)
            candidateHubs.push_back(index);
    }

    if (candidateHubs.empty())
        return false;

    uint32 const hubIndex = candidateHubs[urand(0, candidateHubs.size() - 1)];
    Assignments[bot->GetGUID().GetCounter()] = { hubIndex, false, 0, urand(1, 100) <= sPlayerbotAIConfig.cityLifeWalkChance };
    location = CityHubs[hubIndex].location;
    return true;
}

bool CityBotMgr::IsTargetAllowed(Player* bot, WorldObject const* target)
{
    if (!sPlayerbotAIConfig.enableCityLife || !bot || !target || bot->GetGroup())
        return true;

    auto const assignment = Assignments.find(bot->GetGUID().GetCounter());
    if (assignment == Assignments.end() || assignment->second.hubIndex >= CityHubs.size())
        return true;

    CityHub const& hub = CityHubs[assignment->second.hubIndex];
    if (target->GetMapId() != hub.location.GetMapId())
        return false;

    float const dx = target->GetPositionX() - hub.location.GetPositionX();
    float const dy = target->GetPositionY() - hub.location.GetPositionY();
    return std::sqrt(dx * dx + dy * dy) <= sPlayerbotAIConfig.cityLifeHubRadius;
}

bool CityBotMgr::IsInCity(Player const* bot) const
{
    return bot && GetCity(bot->GetZoneId());
}

void CityBotMgr::AssignHub(Player* bot)
{
    WorldLocation location;
    if (!SelectHub(bot, location))
        return;

    if (bot->IsMounted())
        bot->Dismount();

    bot->SetWalk(Assignments[bot->GetGUID().GetCounter()].walking);
    std::vector<WorldLocation> locations = { location };
    sRandomPlayerbotMgr.RandomTeleport(bot, locations, true);
}

void CityBotMgr::ReturnToCity(Player* bot)
{
    AssignHub(bot);
}

void CityBotMgr::Update(uint32 diff)
{
    if (!sPlayerbotAIConfig.enableCityLife)
        return;

    _updateTimer += diff;
    if (_updateTimer < sPlayerbotAIConfig.cityLifeUpdateInterval * IN_MILLISECONDS)
        return;

    _updateTimer = 0;
    BuildHubs();

    PlayerBotMap const bots = sRandomPlayerbotMgr.GetAllBots();
    std::unordered_set<ObjectGuid::LowType> activeBots;
    for (auto const& pair : bots)
    {
        Player* bot = pair.second;
        if (!bot || !sRandomPlayerbotMgr.IsRandomBot(bot))
            continue;

        ObjectGuid::LowType const guid = bot->GetGUID().GetCounter();
        activeBots.insert(guid);
        auto [assignmentItr, inserted] = Assignments.try_emplace(guid, Assignment{ uint32(CityHubs.size()), false, 0, false });
        Assignment& assignment = assignmentItr->second;

        if (IsInCity(bot) && bot->IsMounted())
            bot->Dismount();

        if (bot->GetGroup())
        {
            bot->SetWalk(false);
            assignment.wasGrouped = true;
            assignment.returnAt = 0;
            continue;
        }

        if (assignment.wasGrouped)
        {
            assignment.wasGrouped = false;
            assignment.returnAt = time(nullptr) + sPlayerbotAIConfig.cityLifeReturnDelay;
            continue;
        }

        bot->SetWalk(Assignments[bot->GetGUID().GetCounter()].walking);

        if (inserted || assignment.hubIndex >= CityHubs.size())
        {
            ReturnToCity(bot);
            continue;
        }

        if (assignment.returnAt && time(nullptr) >= assignment.returnAt)
        {
            ReturnToCity(bot);
            continue;
        }

        if (urand(1, 100) <= sPlayerbotAIConfig.cityLifeEmoteChance &&
            IsTargetAllowed(bot, bot))
            bot->HandleEmoteCommand(TalkAction::GetRandomEmote(bot));
    }

    std::erase_if(Assignments, [&activeBots](auto const& pair) { return !activeBots.contains(pair.first); });
}
