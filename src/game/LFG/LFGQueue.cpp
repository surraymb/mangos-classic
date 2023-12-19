/*
 * This file is part of the CMaNGOS Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "LFG/LFGQueue.h"
#include "World/World.h"
#include "LFG/LFGMgr.h"
#include "Server/WorldPacket.h"
#include "Entities/Player.h"
#include "Globals/ObjectMgr.h"
#include "Groups/Group.h"

std::array<LfgRoles, 3> PotentialRoles =
{
    PLAYER_ROLE_TANK,
    PLAYER_ROLE_HEALER,
    PLAYER_ROLE_DAMAGE
};

void LFGPlayerQueueInfo::CalculateRoles(Classes plrClass)
{
    roleMask = LFGMgr::CalculateRoles(plrClass);

    // Determine role priority
    for (LfgRoles role : PotentialRoles)
        rolePriority.emplace_back(role, LFGMgr::GetPriority(plrClass, role));
}

void LFGPlayerQueueInfo::CalculateTalentRoles(Player* player)
{
    roleMask = LFGMgr::CalculateTalentRoles(player);

    // Determine role priority
    for (LfgRoles role : PotentialRoles)
        rolePriority.emplace_back(role, LFGMgr::GetPriority(Classes(player->getClass()), role));
}

LfgRolePriority LFGPlayerQueueInfo::GetRolePriority(LfgRoles role)
{
    for (const auto& iter : rolePriority)
    {
        if (iter.first == role)
            return iter.second;
    }

    return LFG_PRIORITY_NONE;
}

void LFGQueue::Update()
{
    TimePoint previously = sWorld.GetCurrentClockTime();
    while (!World::IsStopped())
    {
        GetMessager().Execute(this);

        TimePoint now = sWorld.GetCurrentClockTime();
        uint32 diff = (now - previously).count();
        previously = now;

        if (m_queuedGroups.empty() && m_queuedPlayers.empty())
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            continue;
        }

        // Iterate over QueuedPlayersMap to update players timers and remove offline/disconnected players.
        for (auto itr = m_queuedPlayers.begin(); itr != m_queuedPlayers.end();)
        {
            itr->second.timeInLFG += diff;

            // Update player timer and give him queue priority.
            if (itr->second.timeInLFG >= (30 * MINUTE * IN_MILLISECONDS))
                itr->second.hasQueuePriority = true;

            // if matchmaking enabled, ignore talents after some time (default 5 min)
            if (sWorld.getConfig(CONFIG_BOOL_LFG_MATCHMAKING) && itr->second.timeInLFG >= (sWorld.getConfig(CONFIG_UINT32_LFG_MATCHMAKING_TIMER) * IN_MILLISECONDS))
            {
                itr->second.rolePriority.clear();
                itr->second.CalculateRoles((Classes)itr->second.playerClass);
            }

            ++itr;
        }

        if (!m_queuedGroups.empty())
        {
            // Iterate over QueuedGroupsMap to fill groups with roles they're missing.
            for (QueuedGroupsMap::iterator qGroup = m_queuedGroups.begin(); qGroup != m_queuedGroups.end();)
            {
                // Remove group from queue if it has been filled by players manually inviting.
                // We fill 1 group per update since removing it from the queue invalidates
                // the iterator and we can save on performance a little.
                // Iterate over QueuedPlayersMap to find suitable player to join group
                QueuedPlayersMap::iterator next = m_queuedPlayers.begin();
                for (QueuedPlayersMap::iterator qPlayer = next; next != m_queuedPlayers.end(); qPlayer = next)
                {
                    // Pre-increment iterator here since FindRoleToGroup() may remove qPlayer
                    // from the map
                    ++next;

                    // Check here that players team and areaId they're in queue are same
                    if (qPlayer->second.team == qGroup->second.team &&
                        qPlayer->second.areaId == qGroup->second.areaId)
                    {
                        bool groupFound = false;
                        // Find any role that this player matches and that the group requires. If none,
                        // then continue onto the next group.
                        for (LfgRoles role : PotentialRoles)
                        {
                            if (!(qPlayer->second.roleMask & role))
                                continue;

                            if ((role & qGroup->second.availableRoles) == role && FindRoleToGroup(qPlayer->first, qGroup->first, role))
                            {
                                groupFound = true;
                                break;
                            }
                        }

                        // If we found a group and it's now full, break. If it's not full,
                        // go onto the next player and maybe they can fill it.
                        if (groupFound && qGroup->second.playerCount == 5)
                        {
                            break;
                        }
                    }
                }

                if (qGroup->second.playerCount == 5)
                {
                    if (sWorld.getConfig(CONFIG_BOOL_LFG_TELEPORT))
                        TeleportGroupToStone(qGroup->first, qGroup->second.areaId);

                    RemoveGroupFromQueue(qGroup->first, GROUP_SYSTEM_LEAVE);
                    break;
                }

                // Update group timer. After each 5 minutes group will be broadcasted they're still waiting more members.
                if (qGroup->second.groupTimer <= diff)
                {
                    qGroup->second.groupTimer = 5 * MINUTE * IN_MILLISECONDS;

                    sWorld.GetMessager().AddMessage([groupId = qGroup->first](World* world)
                    {
                        Group* group = sObjectMgr.GetGroupById(groupId);

                        WorldPacket data;
                        LFGMgr::BuildInProgressPacket(data);

                        group->BroadcastPacket(data, true);
                    });
                }
                else
                {
                    qGroup->second.groupTimer -= diff;
                }

                ++qGroup;
            }
        }

        // Pick first 2 players and form group out of them also inserting them into queue as group.
        if (m_queuedPlayers.size() >= m_groupSize)
        {
            // Pick Leader as first target.
            QueuedPlayersMap::iterator leader = m_queuedPlayers.begin();

            std::list<ObjectGuid> playersInArea;
            FindInArea(playersInArea, leader->second.areaId, leader->second.team, leader->first);

            // 4 players + the leader
            if (playersInArea.size() >= m_groupSize - 1)
            {
                ObjectGuid leaderGuid = leader->first;
                ObjectGuid memberGuid = playersInArea.front();
                uint32 areaId = leader->second.areaId;

                RemovePlayerFromQueue(leaderGuid, PLAYER_SYSTEM_LEAVE);
                RemovePlayerFromQueue(memberGuid, PLAYER_SYSTEM_LEAVE);

                sWorld.GetMessager().AddMessage([leaderGuid, memberGuid, areaId](World* world)
                {
                    Player* leader = sObjectMgr.GetPlayer(leaderGuid);
                    Player* member = sObjectMgr.GetPlayer(memberGuid);

                    WorldPacket data;
                    LFGMgr::BuildMemberAddedPacket(data, member->GetObjectGuid());

                    leader->GetSession()->SendPacket(data);

                    Group* newQueueGroup = new Group;
                    if (!newQueueGroup->IsCreated())
                    {
                        if (newQueueGroup->Create(leader->GetObjectGuid(), leader->GetName()))
                            sObjectMgr.AddGroup(newQueueGroup);
                        else
                        {
                            // should never be reached for a newly created group
                            MANGOS_ASSERT(false);
                        }
                    }

                    // Add member to the group. Leader is already added upon creation of group.
                    newQueueGroup->AddMember(member->GetObjectGuid(), member->GetName(), GROUP_LFG);

                    // Add this new group to GroupQueue now and remove players from PlayerQueue - there will be a moment in time when the group isnt in queue
                    sLFGMgr.AddToQueue(leader, areaId);
                });
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
}

bool LFGQueue::IsPlayerInQueue(ObjectGuid const& plrGuid) const
{
    return m_queuedPlayers.find(plrGuid) != m_queuedPlayers.end();
}

bool LFGQueue::IsGroupInQueue(uint32 groupId) const
{
    return m_queuedGroups.find(groupId) != m_queuedGroups.end();
}

void LFGQueue::UpdateGroup(LFGGroupQueueInfo const& groupInfo, bool join, ObjectGuid playerGuid, uint32 groupId)
{
    auto itr = m_queuedGroups.find(groupId);
    if (itr == m_queuedGroups.end())
        return; // already removed
    auto& existingGroupInfo = itr->second;
    if (existingGroupInfo.playerCount == groupInfo.playerCount)
        return;

    existingGroupInfo = groupInfo;

    if (groupInfo.playerCount == 5)
    {
        RemoveGroupFromQueue(groupId, GROUP_SYSTEM_LEAVE);
    }
}

void LFGQueue::AddGroup(LFGGroupQueueInfo const& groupInfo, uint32 groupId)
{
    m_queuedGroups[groupId] = groupInfo;

    sWorld.GetMessager().AddMessage([groupId = groupId, areaId = groupInfo.areaId](World* world)
    {
        Group* group = sObjectMgr.GetGroupById(groupId);

        WorldPacket data;
        LFGMgr::BuildSetQueuePacket(data, areaId, MEETINGSTONE_STATUS_JOINED_QUEUE);

        group->BroadcastPacket(data, true);
    });
}

void LFGQueue::AddPlayer(LFGPlayerQueueInfo const& playerInfo, ObjectGuid playerGuid)
{
    m_queuedPlayers[playerGuid] = playerInfo;

    sWorld.GetMessager().AddMessage([playerGuid, areaId = playerInfo.areaId](World* world)
    {
        Player* player = sObjectMgr.GetPlayer(playerGuid);

        player->GetSession()->SendMeetingstoneSetqueue(areaId, MEETINGSTONE_STATUS_JOINED_QUEUE);
    });
}

void LFGQueue::FindInArea(std::list<ObjectGuid>& players, uint32 area, uint32 team, ObjectGuid const& exclude)
{
    for (const auto& itr : m_queuedPlayers)
    {
        if (itr.first == exclude)
            continue;

        if (itr.second.areaId == area && itr.second.team == team)
            players.push_back(itr.first);
    }
}

// Don't pass playerGuid by ref since we may destroy it in RemovePlayerFromQueue
bool LFGQueue::FindRoleToGroup(ObjectGuid playerGuid, uint32 groupId, LfgRoles role)
{
    QueuedGroupsMap::iterator qGroup = m_queuedGroups.find(groupId);
    QueuedPlayersMap::iterator qPlayer = m_queuedPlayers.find(playerGuid);

    if (qGroup != m_queuedGroups.end() && qPlayer != m_queuedPlayers.end())
    {
        bool queueTimePriority = qPlayer->second.hasQueuePriority;
        bool classPriority = qPlayer->second.GetRolePriority(role);
        // Iterate over QueuedPlayersMap to find if players have been longer in Queue.
        for (auto& itr : m_queuedPlayers)
        {
            if (qPlayer->first == itr.first)
                continue;

            // Ignore players queuing for a different dungeon or from opposite factions
            if (qPlayer->second.areaId != itr.second.areaId || qPlayer->second.team != itr.second.team)
                continue;

            // Compare priority/queue time to players that can fill the same role
            if ((itr.second.roleMask & role) == role)
            {
                bool otherTimePriority = itr.second.hasQueuePriority;
                bool otherClassPriority = itr.second.GetRolePriority(role);
                bool otherLongerInQueue = itr.second.timeInLFG > qPlayer->second.timeInLFG;

                // Another player is more valuable in this role, they have priority
                if (otherClassPriority > classPriority)
                    return false;

                // If the other player has time priority and has spent longer in the queue,
                // they are ahead of us
                if (otherTimePriority && otherLongerInQueue)
                    return false;

                // We do not have priority in the queue and they have spent longer
                // in the queue, that means they are ahead
                if (!queueTimePriority && otherLongerInQueue)
                    return false;
            }
        }

        switch (role)
        {
            case PLAYER_ROLE_TANK:
            {
                // Remove tank flag if player can perform tank role.
                qGroup->second.availableRoles &= ~PLAYER_ROLE_TANK;
                break;
            }
            case PLAYER_ROLE_HEALER:
            {
                // Remove healer flag if player can perform healer role.
                qGroup->second.availableRoles &= ~PLAYER_ROLE_HEALER;
                break;
            }
            case PLAYER_ROLE_DAMAGE:
            {
                if (qGroup->second.dpsCount < LFGMgr::GetMaximumDPSSlots())
                {
                    // Update dps count first.
                    ++qGroup->second.dpsCount;

                    // Remove dps flag if there is enough dps in group.
                    if (qGroup->second.dpsCount >= LFGMgr::GetMaximumDPSSlots())
                        qGroup->second.availableRoles &= ~PLAYER_ROLE_DAMAGE;
                }
                break;
            }
            default:
            {
                return false;
            }
        }

        // Remove player from queue.
        RemovePlayerFromQueue(qPlayer->first, PLAYER_SYSTEM_LEAVE);

        ++qGroup->second.playerCount;

        sWorld.GetMessager().AddMessage([playerGuid, groupId](World* world)
        {
            Group* group = sObjectMgr.GetGroupById(groupId);
            Player* player = sObjectMgr.GetPlayer(playerGuid);

            WorldPacket data;
            LFGMgr::BuildMemberAddedPacket(data, playerGuid);
            group->BroadcastPacket(data, true);

            // Add member to the group.
            group->AddMember(playerGuid, player->GetName(), GROUP_LFG);
        });

        // Found player return true.
        return true;
    }

    return false;
}

void LFGQueue::RemovePlayerFromQueue(ObjectGuid playerGuid, PlayerLeaveMethod leaveMethod)
{
    auto itr = m_queuedPlayers.find(playerGuid);
    if (itr != m_queuedPlayers.end())
    {
        if (leaveMethod == PLAYER_CLIENT_LEAVE)
        {
            sWorld.GetMessager().AddMessage([playerGuid](World* world)
            {
                Player* player = sObjectMgr.GetPlayer(playerGuid);

                if (player)
                {
                    if (player->GetSession())
                    {
                        WorldPacket data;
                        LFGMgr::BuildSetQueuePacket(data, 0, MEETINGSTONE_STATUS_LEAVE_QUEUE);
                        player->GetSession()->SendPacket(data);
                    }
                    player->SetLFGAreaId(0);
                }
            });
        }

        m_queuedPlayers.erase(itr);
    }
}

void LFGQueue::RemoveGroupFromQueue(uint32 groupId, GroupLeaveMethod leaveMethod)
{
    QueuedGroupsMap::iterator iter = m_queuedGroups.find(groupId);

    if (iter != m_queuedGroups.end())
    {
        sWorld.GetMessager().AddMessage([groupId, leaveMethod](World* world)
        {
            if (Group* grp = sObjectMgr.GetGroupById(groupId))
            {
                if (leaveMethod == GROUP_CLIENT_LEAVE)
                {
                    WorldPacket data;
                    LFGMgr::BuildSetQueuePacket(data, 0, MEETINGSTONE_STATUS_LEAVE_QUEUE);
                    grp->BroadcastPacket(data, true);
                }
                else
                {
                    // Send complete information to party
                    WorldPacket data;
                    LFGMgr::BuildCompletePacket(data);
                    grp->BroadcastPacket(data, true);

                    // Reset UI for party
                    LFGMgr::BuildSetQueuePacket(data, 0, MEETINGSTONE_STATUS_NONE);
                    grp->BroadcastPacket(data, true);
                }

                grp->SetLFGAreaId(0);
            }
        });

        m_queuedGroups.erase(iter);
    }
}

void LFGQueue::RestoreOfflinePlayer(ObjectGuid playerGuid)
{
    auto itr = m_offlinePlayers.find(playerGuid);

    if (itr != m_offlinePlayers.end())
    {
        m_queuedPlayers[playerGuid] = itr->second;
        m_offlinePlayers.erase(itr);
        sWorld.GetMessager().AddMessage([playerGuid, areaId = itr->second.areaId](World* world)
        {
            if (Player* player = sObjectMgr.GetPlayer(playerGuid))
                player->GetSession()->SendMeetingstoneSetqueue(areaId, MEETINGSTONE_STATUS_JOINED_QUEUE);
        });
    }
    else
    {
        sWorld.GetMessager().AddMessage([playerGuid](World* world)
        {
            if (Player* player = sObjectMgr.GetPlayer(playerGuid))
                player->GetSession()->SendMeetingstoneSetqueue(0, MEETINGSTONE_STATUS_NONE);
        });
    }
}

void LFGQueue::GetPlayerQueueInfo(LFGPlayerQueueInfo* info, ObjectGuid const& plrGuid)
{
    QueuedPlayersMap::iterator iter = m_queuedPlayers.find(plrGuid);
    if (iter != m_queuedPlayers.end())
        *info = iter->second;

    return;
}

void LFGQueue::GetGroupQueueInfo(LFGGroupQueueInfo* info, uint32 groupId)
{
    QueuedGroupsMap::iterator iter = m_queuedGroups.find(groupId);
    if (iter != m_queuedGroups.end())
        *info = iter->second;

    return;
}

void LFGQueue::LoadMeetingStones()
{
    m_MeetingStonesMap.clear();
    uint32 count = 0;
    for (uint32 i = 0; i < sGOStorage.GetMaxEntry(); ++i)
    {
        auto data = sGOStorage.LookupEntry<GameObjectInfo>(i);
        if (data && data->type == GAMEOBJECT_TYPE_MEETINGSTONE)
        {
            AreaTableEntry const* area = GetAreaEntryByAreaID(data->meetingstone.areaID);
            if (area)
            {
                MeetingStoneInfo info;
                info.area = data->meetingstone.areaID;
                info.minlevel = data->meetingstone.minLevel;
                info.maxlevel = data->meetingstone.maxLevel;
                info.name = area->area_name[0];
                // get stone position
                Position stonePosition = Position();
                uint32 mapId = 0;
                FindGOData worker(i, nullptr);
                sObjectMgr.DoGOData(worker);
                GameObjectDataPair const* dataPair = worker.GetResult();
                if (dataPair)
                {
                    GameObjectData const* objData = &dataPair->second;
                    stonePosition = Position(objData->posX, objData->posY, objData->posZ, 0.f);
                    mapId = objData->mapid;
                }
                info.position = stonePosition;
                info.mapId = mapId;
                switch (info.area)
                {
                case 1977:
                case 2159:
                    info.dungeonType = MAP_RAID;
                    break;
                default:
                    info.dungeonType = MAP_INSTANCE;
                }
                /*if (MapEntry const* mEntry = sMapStore.LookupEntry(area->mapid))
                {
                    info.dungeonType = mEntry->map_type;
                }
                else
                    info.dungeonType = MAP_COMMON;*/

                m_MeetingStonesMap.insert(std::make_pair(data->id, info));
                sLog.outBasic(">> Loaded Meeting Stone Entry:%d, Area:%d, Level:%d - %d, Name:%s, Dungeon Type:%u", i, info.area, info.minlevel, info.maxlevel, info.name, info.dungeonType);
                count++;
            }
        }
    }

    sLog.outString(">> Loaded %u Meeting Stones", count);
    sLog.outString();
}

MeetingStoneSet LFGQueue::GetDungeonsForPlayer(Player* player)
{
    MeetingStoneSet list;
    uint32 level = player->GetLevel();
    for (MeetingStonesMap::iterator it = m_MeetingStonesMap.begin(); it != m_MeetingStonesMap.end(); ++it)
    {
        MeetingStoneInfo data = it->second;

        if (data.maxlevel && data.maxlevel < level)
            continue;

        if (data.minlevel && data.minlevel > level)
            continue;

        list.insert(list.end(), data);
    }
    return list;
}

void LFGQueue::TeleportGroupToStone(uint32 groupId, uint32 areaId)
{
    Group* grp = sObjectMgr.GetGroupById(groupId);
    if (!grp)
        return;

    // custom teleport to dungeon
    for (auto & it : m_MeetingStonesMap)
    {
        MeetingStoneInfo data = it.second;
        if (data.area != areaId)
            continue;

        if (data.position.IsEmpty())
            continue;

        // get map of stone
        Map* stoneMap = sMapMgr.FindMap(data.mapId);
        if (!stoneMap)
            continue;

        AreaTableEntry const* entry = GetAreaEntryByAreaID(data.area);
        if (!entry)
            continue;


        // calculate random teleport position near stone
        float x = data.position.x;
        float y = data.position.y;
        float z = data.position.z;
        //stoneMap->GetReachableRandomPointOnGround(x, y, z, 20.f);

        // send teleport offer
        for (GroupReference* ref = grp->GetFirstMember(); ref != nullptr; ref = ref->next())
        {
            if (Player* member = ref->getSource())
            {
                // dont summon if already has summon request
                if (member->HasSummonOffer())
                    continue;

                // dont summon if close
                if (member->GetMapId() == data.mapId && member->IsWithinDist2d(data.position.x, data.position.y, 100.f))
                    continue;

                bool foundPoint = false;
                uint32 attempts = 0;
                do
                {
                    // Generate a random range and direction for the new point
                    const float angle = rand_norm_f() * (M_PI_F * 2.0f);
                    const float range = 20.0f;

                    float i_x = x + range * cos(angle);
                    float i_y = y + range * sin(angle);
                    float i_z = z + 1.0f;

                    member->UpdateAllowedPositionZ(i_x, i_y, i_z, stoneMap);

                    // project vector to get only positive value
                    float ab = fabs(x - i_x);
                    float ac = fabs(z - i_z);

                    // slope represented by c angle (in radian)
                    const float MAX_SLOPE_IN_RADIAN = 50.0f / 180.0f * M_PI_F;  // 50(degree) max seem best value for walkable slope

                    // check ab vector to avoid divide by 0
                    if (ab > 0.0f)
                    {
                        // compute c angle and convert it from radian to degree
                        float slope = atan(ac / ab);
                        if (slope < MAX_SLOPE_IN_RADIAN)
                        {
                            x = i_x;
                            y = i_y;
                            z = i_z;
                            foundPoint = true;
                        }
                    }
                    attempts++;
                } while (!foundPoint && attempts < 10);

                member->SetSummonPoint(data.mapId, x, y, z, grp->GetLeaderGuid());
                WorldPacket summon(SMSG_SUMMON_REQUEST, 8 + 4 + 4);
                summon << grp->GetLeaderGuid();                               // summoner guid
                summon << uint32(entry->zone != 0 ? entry->zone : entry->ID); // summoner zone
                summon << uint32(MAX_PLAYER_SUMMON_DELAY * IN_MILLISECONDS);  // auto decline after msecs
                member->GetSession()->SendPacket(summon);
            }
        }
    }
}
