#include "WoW/Source/Ui/TalentUI.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "Engine/Source/FrameScript/FrameScript.h"
#include "Engine/Source/lua-5.1/src/lua.h"

// The 17 talents, in the server's own order and with its own numbers, from mod-mana-speedflying's
// kTalents[]. The ids are the wire contract: they ride WXLTT as integers and key the stored ranks, so this
// table must be append-only and never renumbered.
//
// Columns are exactly what Talents.lua indexes:
//   [1] id  [2] branch(1 Dive,2 Glide,3 Wings)  [3] tier  [4] maxRank  [5] effect
//   [6] name  [7] description  [8] step  [9] step unit  [10] branch this opens (or 0)
namespace
{
    struct TalentDef
    {
        uint16_t    m_id;
        uint8_t     m_branch;
        uint8_t     m_tier;
        uint8_t     m_maxRank;
        uint8_t     m_effect;
        const char* m_name;
        const char* m_desc;
        float       m_step;
        const char* m_unit;
        uint8_t     m_opens;
    };

    // effect 0 is "no server effect" (E_Count): the bridges and Smoothed Plumage are client-side.
    const TalentDef kTalents[] =
    {
        {  1, 1, 1, 3, 1, "Steep Descent",     "Accelerate into a dive 8% faster per rank.",                     -8.0f, "%", 0 },
        {  3, 1, 2, 3, 1, "Terminal Velocity", "Reach 6% more dive speed per rank.",                              6.0f, "%", 0 },
        {  2, 1, 2, 3, 1, "Held Breath",       "Hold dive speed 15% longer pulling up into a climb.",            15.0f, "%", 0 },
        {  4, 1, 3, 3, 1, "Shallow Pull",      "A pull-out 15 degrees shallower per rank keeps its glide.",       15.0f, "%", 0 },
        {  5, 1, 4, 1, 1, "Aftershock",        "Releasing the boost key into a dive commits the burst at once.",   0.0f, "",  0 },
        {  6, 2, 1, 3, 1, "Long Glide",        "Glide 12% longer per rank out of a dive.",                        12.0f, "%", 0 },
        {  7, 2, 2, 3, 1, "Slipstream",        "Shorten the step to the next point by 12% per rank.",            -12.0f, "%", 0 },
        {  8, 2, 2, 3, 1, "Banked Momentum",   "Fold 10 more points of a boost into the glide per rank.",         10.0f, "%", 0 },
        { 10, 2, 3, 3, 1, "Skyward Path",      "Glide 10% further above the level bar per rank.",                 10.0f, "%", 0 },
        {  9, 2, 4, 1, 1, "Twin Vault",        "Hold a fourth boost stack.",                                       1.0f, "",  0 },
        { 14, 3, 1, 3, 0, "Smoothed Plumage",  "Dive shake 25% quieter per rank, speed motes 30% brighter.",     -25.0f, "%", 0 },
        { 12, 3, 2, 3, 1, "Steady Wings",      "Boost stacks recharge 8% faster per rank.",                       -8.0f, "%", 0 },
        { 11, 3, 2, 3, 1, "Diving Wings",      "Diving boost stacks recharge 10% faster per rank.",              -10.0f, "%", 0 },
        { 13, 3, 3, 3, 1, "Iron Wings",        "Each boost adds 8% more speed per rank.",                          8.0f, "%", 0 },
        { 15, 3, 4, 1, 1, "Rising Current",    "A climb keeps 40% more of its entered speed.",                    40.0f, "%", 0 },
        { 16, 1, 3, 1, 0, "Still Air",         "Glide 10 degrees steeper before it reads as a climb.",             0.0f, "",  2 },
        { 17, 2, 3, 1, 0, "Tucked Wings",      "Shallow the dive threshold 10 degrees.",                           0.0f, "",  3 },
    };

    constexpr uint32_t kTalentCount = sizeof(kTalents) / sizeof(kTalents[0]);
    constexpr uint32_t kMaxPoints = 13;

    struct State
    {
        bool     m_tablePushed = false;
        void*    m_pushedInto = nullptr;
        uint32_t m_points = 0;
        uint32_t m_spent = 0;
        uint32_t m_unspent = 0;

        // Confirmed rank per talent id, from the last tally.
        uint32_t m_ranks[32] = {};

        // Staged ranks the player has clicked but not confirmed. The panel draws confirmed+staged.
        int32_t  m_staged[32] = {};

        // Progress toward the next point, from the WXLTT "P" body.
        uint32_t m_partialPermille = 0;
        uint32_t m_needYards = 0;

        uint32_t m_lastSync = 0;
        char     m_status[96] = {};
    };

    State s_state;

    lua_State* L()
    {
        return FrameScript_GetContext();
    }

    void SetGlobalNumber(const char* name, double value)
    {
        lua_State* lua = L();

        if (!lua)
        {
            return;
        }

        lua_pushnumber(lua, value);
        lua_setglobal(lua, name);
    }

    void SetGlobalString(const char* name, const char* value)
    {
        lua_State* lua = L();

        if (!lua)
        {
            return;
        }

        lua_pushstring(lua, value ? value : "");
        lua_setglobal(lua, name);
    }

    int RankOf(int id)
    {
        const int confirmed = (id >= 0 && id < 32) ? static_cast<int>(s_state.m_ranks[id]) : 0;
        const int staged = (id >= 0 && id < 32) ? s_state.m_staged[id] : 0;

        const int total = confirmed + staged;

        return total < 0 ? 0 : total;
    }

    // Publishes WXL_TalentD once. Talents.lua builds its whole frame from this and bails if it is absent.
    void PushTable()
    {
        lua_State* lua = L();

        if (!lua)
        {
            return;
        }

        // A UI reload creates a fresh lua_State with no WXL_TalentD; the flag must not outlive it.
        if (lua != s_state.m_pushedInto)
        {
            s_state.m_pushedInto = lua;
            s_state.m_tablePushed = false;
        }

        if (s_state.m_tablePushed)
        {
            return;
        }

        // Build an array of 10-element arrays, exactly the columns the panel indexes.
        lua_newtable(lua);

        for (uint32_t i = 0; i < kTalentCount; i++)
        {
            const TalentDef& t = kTalents[i];

            lua_newtable(lua);

            lua_pushnumber(lua, t.m_id);        lua_rawseti(lua, -2, 1);
            lua_pushnumber(lua, t.m_branch);    lua_rawseti(lua, -2, 2);
            lua_pushnumber(lua, t.m_tier);      lua_rawseti(lua, -2, 3);
            lua_pushnumber(lua, t.m_maxRank);   lua_rawseti(lua, -2, 4);
            lua_pushnumber(lua, t.m_effect);    lua_rawseti(lua, -2, 5);
            lua_pushstring(lua, t.m_name);      lua_rawseti(lua, -2, 6);
            lua_pushstring(lua, t.m_desc);      lua_rawseti(lua, -2, 7);
            lua_pushnumber(lua, t.m_step);      lua_rawseti(lua, -2, 8);
            lua_pushstring(lua, t.m_unit);      lua_rawseti(lua, -2, 9);
            lua_pushnumber(lua, t.m_opens);     lua_rawseti(lua, -2, 10);

            lua_rawseti(lua, -2, static_cast<int>(i + 1));
        }

        lua_setglobal(lua, "WXL_TalentD");

        s_state.m_tablePushed = true;

        SetGlobalNumber("WXL_TalentMax", static_cast<double>(kMaxPoints));
    }

    // The panel raises these; SpeedFlying polls and sends them. Kept here so the two halves stay together.
    void PublishState()
    {
        SetGlobalNumber("WXL_TalentPoints", static_cast<double>(s_state.m_points));
        SetGlobalNumber("WXL_TalentSpent", static_cast<double>(s_state.m_spent));
        SetGlobalNumber("WXL_TalentUnspent", static_cast<double>(s_state.m_unspent));
        SetGlobalNumber("WXL_TalentPartial", static_cast<double>(s_state.m_partialPermille));
        SetGlobalNumber("WXL_TalentNeed", static_cast<double>(s_state.m_needYards));
        SetGlobalString("WXL_TalentStatus", s_state.m_status);

        lua_State* lua = L();

        if (!lua)
        {
            return;
        }

        // WXL_TalentRanks is indexed by the talent's position in WXL_TalentD, not by id.
        lua_newtable(lua);

        for (uint32_t i = 0; i < kTalentCount; i++)
        {
            lua_pushnumber(lua, RankOf(kTalents[i].m_id));
            lua_rawseti(lua, -2, static_cast<int>(i + 1));
        }

        lua_setglobal(lua, "WXL_TalentRanks");

        // WXL_TalentCan: the server is authoritative, so nothing is pre-greyed. A click is always allowed and
        // the server answers with a tally or an F refusal.
        lua_newtable(lua);

        for (uint32_t i = 0; i < kTalentCount; i++)
        {
            lua_pushboolean(lua, 1);
            lua_rawseti(lua, -2, static_cast<int>(i + 1));
        }

        lua_setglobal(lua, "WXL_TalentCan");
    }

    // Reads the panel's raised intents and hands them to SpeedFlying to put on the wire.
    void PollPanelInput()
    {
        lua_State* lua = L();

        if (!lua)
        {
            return;
        }

        // WXL_TalentPick = { <table index>, +1 | -1 }
        lua_getglobal(lua, "WXL_TalentPick");

        if (lua_istable(lua, -1))
        {
            lua_rawgeti(lua, -1, 1);
            const int index = static_cast<int>(lua_tonumber(lua, -1));
            lua_pop(lua, 1);

            lua_rawgeti(lua, -1, 2);
            const int delta = static_cast<int>(lua_tonumber(lua, -1));
            lua_pop(lua, 1);

            if (index >= 1 && index <= static_cast<int>(kTalentCount) && delta != 0)
            {
                const int id = kTalents[index - 1].m_id;
                const int maxRank = kTalents[index - 1].m_maxRank;

                int staged = s_state.m_staged[id] + delta;

                // Clamp only against the rank cap; the tier and pick gates belong to the server.
                const int ceiling = maxRank - static_cast<int>(s_state.m_ranks[id]);

                if (staged > ceiling)
                {
                    staged = ceiling;
                }

                if (staged < -static_cast<int>(s_state.m_ranks[id]))
                {
                    staged = -static_cast<int>(s_state.m_ranks[id]);
                }

                s_state.m_staged[id] = staged;

                lua_pushnil(lua);
                lua_setglobal(lua, "WXL_TalentPick");
            }
        }

        lua_pop(lua, 1);

        // WXL_TalentConfirm: send the whole staged set as one batch, absolute targets, which is what makes a
        // resend idempotent on the server.
        lua_getglobal(lua, "WXL_TalentConfirm");

        if (lua_isnumber(lua, -1) && lua_tonumber(lua, -1) != 0.0)
        {
            lua_pop(lua, 1);
            lua_pushnil(lua);
            lua_setglobal(lua, "WXL_TalentConfirm");

            // Every pair is 8 bytes at most ("16:3,"), and there are at most 17 of them, so 256 is ample and
            // the buffer is sized above that with the framing prefix included.
            char body[512];
            size_t at = 0;
            body[0] = '\0';

            bool any = false;
            bool truncated = false;

            for (uint32_t i = 0; i < kTalentCount; i++)
            {
                const int id = kTalents[i].m_id;
                const int target = static_cast<int>(s_state.m_ranks[id]) + s_state.m_staged[id];

                if (s_state.m_staged[id] == 0)
                {
                    continue;
                }

                // Skip a pair that would not fit rather than writing half of one: a cut entry becomes an
                // unreadable "<id>:" and the server refuses the whole batch, while the staged intents were
                // then cleared anyway and the player's clicks vanished.
                if (at + 16 >= sizeof(body))
                {
                    truncated = true;
                    continue;
                }

                const int written = std::snprintf(body + at, sizeof(body) - at, "%s%d:%d",
                                                  any ? "," : "", id, target);

                if (written <= 0)
                {
                    continue;
                }

                at += static_cast<size_t>(written);
                any = true;
            }

            if (any)
            {
                // "B\t<id>:<target>,..." -- the poller in SpeedFlying puts the WXLTT prefix on.
                char framed[540];
                std::snprintf(framed, sizeof(framed), "B\t%s", body);
                SetGlobalString("WXL_TalentSend", framed);

                // Only the intents that actually made it into the message are cleared, so anything dropped by
                // truncation stays staged for the next Confirm instead of being silently lost.
                if (!truncated)
                {
                    for (uint32_t i = 0; i < kTalentCount; i++)
                    {
                        s_state.m_staged[kTalents[i].m_id] = 0;
                    }
                }
            }
        }
        else
        {
            lua_pop(lua, 1);
        }
    }
}

namespace TalentUI
{
    void OnTally(uint32_t points, uint32_t spent, uint32_t unspent)
    {
        s_state.m_points = points;
        s_state.m_spent = spent;
        s_state.m_unspent = unspent;

        // A tally replaces the confirmed ranks wholesale: a refund removes pairs, so merging would leave a
        // stale rank behind and the panel would over-report the tree.
        for (uint32_t i = 0; i < 32; i++)
        {
            s_state.m_ranks[i] = 0;
        }
    }

    // <id>:<rank>,<id>:<rank>,... with an optional ",P<permille>,N<yards>" tail. The tail is parsed by
    // OnPartial from the separate bar body; a client that cannot read a pair treats it as the end of the
    // list, which is why the tail is appended last.
    void OnRanks(const char* tallyBody)
    {
        if (!tallyBody)
        {
            return;
        }

        const char* cursor = tallyBody;

        while (cursor && *cursor)
        {
            if (*cursor == 'P' || *cursor == 'N')
            {
                break;
            }

            const int id = atoi(cursor);
            const char* colon = strchr(cursor, ':');

            if (!colon)
            {
                break;
            }

            const uint32_t rank = static_cast<uint32_t>(atoi(colon + 1));

            if (id > 0 && id < 32)
            {
                s_state.m_ranks[id] = rank;
            }

            const char* comma = strchr(colon, ',');

            if (!comma)
            {
                break;
            }

            cursor = comma + 1;
        }
    }

    void OnPartial(uint32_t permille, uint32_t needYards)
    {
        s_state.m_partialPermille = permille;
        s_state.m_needYards = needYards;
    }

    void OnStatus(const char* reason)
    {
        SStrCopy(s_state.m_status, reason ? reason : "", sizeof(s_state.m_status));
    }

    void Update(uint32_t timeMs)
    {
        if (!L())
        {
            return;
        }

        PushTable();
        PollPanelInput();

        // The panel repaints on its own OnUpdate from these globals; pushing them every frame would be Lua
        // churn for no gain, so they are refreshed when something has actually moved.
        if (timeMs - s_state.m_lastSync >= 250)
        {
            s_state.m_lastSync = timeMs;
            PublishState();
        }
    }
}
