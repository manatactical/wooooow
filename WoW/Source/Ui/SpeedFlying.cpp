#include "WoW/Source/Ui/SpeedFlying.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "Engine/Source/BlizzardCore/SFile2-Core.h"
#include "Engine/Source/FrameScript/FrameScript.h"
#include "Engine/Source/Os/TimeManager.h"
#include "Engine/Source/Os/Input.h"
#include "Engine/Source/lua-5.1/src/lua.h"
#include "Engine/Source/Model2/M2Model.h"
#include "Engine/Source/Model2/M2Data.h"
#include "WoW/Source/Object/ObjectClient/Player_C.h"
#include "WoW/Source/Object/ObjectClient/Unit_C.h"
#include "WoW/Source/ObjectMgrClient/ObjectMgrClient.h"
#include "WoW/Source/Ui/GameUI.h"
#include "Engine/Source/ModernAssets/ModernAssetsLog.h"
#include "WoW/Source/Ui/TalentUI.h"

// The engine builds CMSG_MESSAGECHAT for addon chat inside Script_SendAddonMessage, and exposes no C++
// equivalent, so sending goes through a Lua helper the same addon provides. See SpeedFlying.lua.
#define SF_LUA_SEND "WXL_SendAddon"

namespace
{
    // Link state letters, from the server module's README and its `default:` branch. An unknown letter is
    // treated by the server as plain level flight, so a wrong one degrades rather than breaks.
    constexpr char kStateLevel    = 'L';
    constexpr char kStateClimb    = 'C';
    constexpr char kStateDive     = 'D';
    constexpr char kStateHover    = 'H';
    constexpr char kStateBoost    = 'B';

    // The server throttles nothing but drops floods; it asks for roughly one message per 200 ms plus a 1 s
    // heartbeat. Sending faster than that is pure spam.
    constexpr uint32_t kSendIntervalMs = 200;
    constexpr uint32_t kHeartbeatMs    = 1000;

    // How long a freshly seen flight state must persist before it is applied. Every change restarts the
    // mount's clip and the server's speed blend, so following single jittery frames is what made both
    // stutter. The reference uses the same 60 ms for the same reason.
    constexpr uint32_t kHoldMs = 60;

    // The reference's DebugHUD setting: when on, the HUD adds its debug readout (currently the live speed
    // percentage). Off by default, exactly like the reference's INI. Flip this to 1 to show it.
    constexpr bool g_debugHud = false;

    // What the server announced about itself. Until the first S: arrives these are the reference client's
    // own documented fallbacks, so the bar and shake have sane ends even with no server module.
    struct ServerSpeeds
    {
        float    m_levelPct   = 280.0f;
        float    m_divePct    = 600.0f;
        float    m_boostPct   = 1200.0f;
        uint32_t m_maxStacks  = 3;
        uint32_t m_stacks     = 3;
        uint32_t m_stackMs      = 9000;
        uint32_t m_stackDiveMs  = 3000;
        uint32_t m_stackAccumMs = 0;
        uint32_t m_stackIntervalMs = 9000;
        uint32_t m_boostFadeMs = 4000;
        bool     m_received   = false;
    };

    struct State
    {
        ServerSpeeds m_speeds;

        // The server announces a stack count on a timer, so between announces it is stale. These model
        // the recharge locally from the announce anchor, which is what stops a press that SHOULD work
        // from being refused by the client's own stale count.
        int32_t  m_stacksBase = 0;
        uint32_t m_stacksBaseMs = 0;
        uint32_t m_stacksAtLastAnnounce = 0;
        uint32_t m_stacksAnnounceMs = 0;

        // The last link message sent, so a steady state does not repeat itself.
        bool     m_sentAdvFly   = false;
        char     m_sentState    = 0;
        uint32_t m_lastSendMs   = 0;
        uint32_t m_lastAnyMs    = 0;
        uint32_t m_lastBoostMs  = 0;

        // What we last pushed into the addon globals, so a per-frame tick is not also per-frame Lua churn.
        char     m_uiState[16] = {};
        char     m_uiSpeedPct[16] = {};
        float    m_uiFill = -1.0f;
        float    m_uiDiveFrac = -1.0f;
        int32_t  m_uiStacks = -1;
        int32_t  m_uiMaxStacks = -1;
        bool     m_uiFrameOn = false;
        bool     m_uiDiving = false;
        bool     m_uiGlideOn = false;
        bool     m_uiSpaceBound = false;

        // Client-side view of the talent ranks that affect the CLIENT: 14 Smoothed Plumage (dive shake and
        // mote density), 16 Still Air and 17 Tucked Wings (the climb/dive angle thresholds). Every other
        // talent is applied entirely server-side and never needs parsing here.
        uint32_t m_rankFeel = 0;
        uint32_t m_rankStillAir = 0;
        uint32_t m_rankTuckedWings = 0;

        // Parsed tally bookkeeping, only used to feed the addon's own panel globals.
        char     m_talentBody[512] = {};
        char     m_talentBar[64]   = {};
        char     m_refusal[64]     = {};

        // Re-scan the inbound Lua globals on a timer rather than every frame: they are strings the Listener
        // writes, and reading them means crossing into Lua.
        uint32_t m_lastPollMs = 0;
        uint32_t m_lastTalentQueryMs = 0;
        char     m_seenServerMsg[192] = {};
        char     m_seenTalentBody[512] = {};
        char     m_seenTalentBar[64] = {};

        // The orb recharge anchor came with a fresh server announce and has not been published yet.
        bool     m_anchorDirty = false;

        bool     m_initialised = false;
    };

    State s_state;

    // Defined with the boost send, but PublishHud needs it (the orb row shows the same estimate a press
    // is judged on, so the two cannot disagree).
    int32_t EffectiveStacks(uint32_t nowMs);

    lua_State* L()
    {
        return FrameScript_GetContext();
    }

    // A UI reload (/reload, or a reload of the frame script context) makes a brand-new lua_State with none of
    // the globals this module publishes. Every "already pushed" cache is therefore keyed on the state it was
    // pushed into: when that pointer changes the caches are invalid and everything is published again.
    lua_State* s_pushedInto = nullptr;

    bool NeedsRepublish(lua_State* lua)
    {
        if (lua != s_pushedInto)
        {
            s_pushedInto = lua;
            return true;
        }

        return false;
    }

    // Reads a global as a string. Returns false when absent or not a string.
    bool ReadGlobalString(const char* name, char* out, size_t outLen)
    {
        lua_State* lua = L();

        if (!lua || !out || outLen < 2)
        {
            return false;
        }

        lua_getglobal(lua, name);

        bool ok = false;

        if (lua_isstring(lua, -1))
        {
            const char* value = lua_tolstring(lua, -1, nullptr);

            if (value)
            {
                SStrCopy(out, value, outLen);
                ok = true;
            }
        }

        lua_pop(lua, 1);
        return ok;
    }

    bool ReadGlobalNumber(const char* name, double* out)
    {
        lua_State* lua = L();

        if (!lua || !out)
        {
            return false;
        }

        lua_getglobal(lua, name);

        bool ok = false;

        if (lua_isnumber(lua, -1))
        {
            *out = lua_tonumber(lua, -1);
            ok = true;
        }

        lua_pop(lua, 1);
        return ok;
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

    // Sends a WXLMS/WXLTT body through the addon's Lua helper. The helper prefixes nothing: the caller
    // passes prefix and body separately, matching SendAddonMessage's signature.
    void SendLink(const char* prefix, const char* body)
    {
        lua_State* lua = L();

        if (!lua || !prefix || !body)
        {
            return;
        }

        lua_getglobal(lua, SF_LUA_SEND);

        if (!lua_isfunction(lua, -1))
        {
            lua_pop(lua, 1);
            return;
        }

        lua_pushstring(lua, prefix);
        lua_pushstring(lua, body);

        if (lua_pcall(lua, 2, 0, 0) != 0)
        {
            lua_pop(lua, 1);
            MODERN_ASSETS_LOG_INFO("speedflying: send %s\t%s FAILED", prefix, body);
            return;
        }

        MODERN_ASSETS_LOG_INFO("speedflying: send %s\t%s", prefix, body);
    }

    // --- protocol parsing -------------------------------------------------------------------------

    // WXLMS\tS:<levelPct>,<divePct>,<boostPct>,<cooldownMs>,<maxStacks>,<boostFadeMs>,<stacks>,
    //         <stackMs>,<stackDiveMs>[,<accumMs>,<intervalMs>]
    void ParseServerSpeeds(const char* body)
    {
        if (!body || body[0] != 'S' || body[1] != ':')
        {
            return;
        }

        float values[11] = {};
        uint32_t count = 0;

        const char* cursor = body + 2;

        while (cursor && *cursor && count < 11)
        {
            values[count++] = static_cast<float>(atof(cursor));

            const char* comma = strchr(cursor, ',');

            if (!comma)
            {
                break;
            }

            cursor = comma + 1;
        }

        // Nine fields are mandatory: stacks, stackMs and stackDiveMs live at 6..8, and leaving them at 0
        // disabled boost entirely and handed the addon a 0 ms recharge interval to divide by.
        if (count < 9)
        {
            return;
        }

        s_state.m_speeds.m_levelPct  = values[0];
        s_state.m_speeds.m_divePct   = values[1];
        s_state.m_speeds.m_boostPct  = values[2];
        s_state.m_speeds.m_maxStacks = static_cast<uint32_t>(values[4]);
        s_state.m_speeds.m_boostFadeMs = static_cast<uint32_t>(values[5]);
        s_state.m_speeds.m_stacks    = static_cast<uint32_t>(values[6]);
        // The anchor moves with every announce, so a value only becomes "the server's truth" once it has
        // actually changed; the local clock is then re-synced to it. Anything else would rewind the clock
        // on every 5 s heartbeat.
        const uint32_t announceMs = static_cast<uint32_t>(OsGetAsyncTimeMs());
        const bool stackCountMoved = s_state.m_speeds.m_received
            && static_cast<uint32_t>(values[6]) != s_state.m_stacksAtLastAnnounce;

        s_state.m_stacksAtLastAnnounce = static_cast<uint32_t>(values[6]);
        s_state.m_stacksAnnounceMs = announceMs;

        if (!s_state.m_speeds.m_received || stackCountMoved)
        {
            s_state.m_stacksBase = static_cast<int32_t>(values[6]);
            s_state.m_stacksBaseMs = announceMs;
        }

        s_state.m_speeds.m_stackMs     = static_cast<uint32_t>(values[7]);
        s_state.m_speeds.m_stackDiveMs = static_cast<uint32_t>(values[8]);
        s_state.m_speeds.m_received  = true;
        s_state.m_anchorDirty = true;

        // The two trailing fields are the server's accrual anchor; an older server omits them and the addon
        // then runs its own clock.
        if (count >= 11)
        {
            s_state.m_speeds.m_stackAccumMs = static_cast<uint32_t>(values[9]);
            s_state.m_speeds.m_stackIntervalMs = static_cast<uint32_t>(values[10]);
        }
    }

    // WXLTT\tT\t<points>,<spent>,<unspent>[,<id>:<rank>,...][,P<permille>,N<yards>]
    //
    // Only the three client-side talent ranks are mined out of it; everything else is handed to the addon
    // verbatim so the panel renders exactly what the server said.
    void ParseTally(const char* body)
    {
        if (!body || body[0] != 'T' || body[1] != ':')
        {
            return;
        }

        SStrCopy(s_state.m_talentBody, body, sizeof(s_state.m_talentBody));

        s_state.m_rankFeel = 0;
        s_state.m_rankStillAir = 0;
        s_state.m_rankTuckedWings = 0;

        const char* cursor = body + 2;

        // The three leading counters: points, spent, unspent.
        const uint32_t points  = static_cast<uint32_t>(atoi(cursor));

        const char* comma1 = strchr(cursor, ',');

        if (!comma1)
        {
            return;
        }

        const uint32_t spent = static_cast<uint32_t>(atoi(comma1 + 1));

        const char* comma2 = strchr(comma1 + 1, ',');

        if (!comma2)
        {
            return;
        }

        const uint32_t unspent = static_cast<uint32_t>(atoi(comma2 + 1));

        TalentUI::OnTally(points, spent, unspent);

        cursor = comma2 + 1;

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

            switch (id)
            {
                case 14: s_state.m_rankFeel = rank; break;
                case 16: s_state.m_rankStillAir = rank; break;
                case 17: s_state.m_rankTuckedWings = rank; break;
                default: break;
            }

            const char* comma = strchr(colon, ',');

            if (!comma)
            {
                break;
            }

            cursor = comma + 1;
        }

        // Every rank pair is handed to the panel as well -- the panel renders the whole tree, and these are
        // the confirmed ranks it draws before any staging.
        //
        // It must start AFTER the three counters: OnRanks treats its argument as "<id>:<rank>,...", so passing
        // the counters as well made it read the points total as a talent id and bind it to the first rank it
        // found, corrupting the whole list.
        TalentUI::OnRanks(comma2 + 1);
    }

    // Polls what Listener.lua stashed and reacts to anything new.
    void PollInbound(uint32_t nowMs)
    {
        if (nowMs - s_state.m_lastPollMs < 100)
        {
            return;
        }

        s_state.m_lastPollMs = nowMs;

        char buffer[512];

        if (ReadGlobalString("WXL_MountAnimServerMsg", buffer, sizeof(buffer))
            && strcmp(buffer, s_state.m_seenServerMsg) != 0)
        {
            SStrCopy(s_state.m_seenServerMsg, buffer, sizeof(s_state.m_seenServerMsg));

            // Listener stores the whole body including the leading "S:".
            ParseServerSpeeds(buffer);
            MODERN_ASSETS_LOG_INFO("speedflying: server %s", buffer);
        }

        if (ReadGlobalString("WXL_TalentBody", buffer, sizeof(buffer))
            && strcmp(buffer, s_state.m_seenTalentBody) != 0)
        {
            SStrCopy(s_state.m_seenTalentBody, buffer, sizeof(s_state.m_seenTalentBody));

            if (buffer[0] == 'T' && buffer[1] == ':')
            {
                ParseTally(buffer);
                MODERN_ASSETS_LOG_INFO("talent: tally %s", buffer);
            }
            else if (buffer[0] == 'F' && buffer[1] == ':')
            {
                // A refusal is the only feedback a rejected click gets, so it is handed to the panel as its
                // status line. Without this the player saw nothing at all.
                SStrCopy(s_state.m_refusal, buffer + 2, sizeof(s_state.m_refusal));
                TalentUI::OnStatus(s_state.m_refusal);
                MODERN_ASSETS_LOG_INFO("talent: server refused: %s", s_state.m_refusal);
            }
        }

        if (ReadGlobalString("WXL_TalentBar", buffer, sizeof(buffer))
            && strcmp(buffer, s_state.m_seenTalentBar) != 0)
        {
            SStrCopy(s_state.m_seenTalentBar, buffer, sizeof(s_state.m_seenTalentBar));
            SStrCopy(s_state.m_talentBar, buffer, sizeof(s_state.m_talentBar));

            // "P	<permille>,<yards>" -- the partial point, on its own so the tally is not resent for it.
            if (buffer[0] == 'P' && buffer[1] == ':')
            {
                TalentUI::OnPartial(static_cast<uint32_t>(atoi(buffer + 2)),
                                    static_cast<uint32_t>(atoi(strchr(buffer + 2, ',') ? strchr(buffer + 2, ',') + 1 : "0")));
            }
        }
    }

    // --- the flight state -------------------------------------------------------------------------

    // Picks the link letter from the mount's real flight path, exactly as the animation chooser does. Kept
    // in step with CGUnit_C::ChooseMountFlightAnim deliberately: the server's speed and the mount's clip
    // must never disagree about which state the player is in.
    char ChooseState(const CGUnit_C* unit, bool* diving, bool* glideOn, uint32_t nowMs)
    {
        *diving = false;
        *glideOn = false;

        const uint32_t flags = unit->m_movementData.m_flags;

        if (!(flags & MOVEMENTFLAG_FLYING))
        {
            return kStateLevel;
        }

        if (fabsf(unit->m_movementData.m_currentSpeed) < 0.5f)
        {
            return kStateHover;
        }

        // Strafe and reverse are level travel, and they must be decided BEFORE the angle -- exactly as
        // CGUnit_C::ChooseMountFlightAnim does, or the mount would play a climb or dive clip while the server
        // was told 'L' and the two would disagree about what the player is doing.
        //
        // The link letter has no lateral value: the server only distinguishes climb / dive / hover / level,
        // and a sidestep is level flight with no vertical component worth chasing.
        if (flags & (MOVEMENTFLAG_STRAFE_LEFT | MOVEMENTFLAG_STRAFE_RIGHT | MOVEMENTFLAG_BACKWARD))
        {
            *glideOn = true;
            return kStateLevel;
        }

        const float pathSin = unit->m_movementData.m_pitchSin;

        // The two bridges widen these angles: Still Air (16) lifts the climb threshold 15 -> 25 degrees,
        // Tucked Wings (17) drops the dive threshold 35 -> 45. Both are client-side effects whose ranks
        // ride the tally.
        float climbSin = 0.2588f;  // sin(15)
        float diveSin  = -0.5736f; // sin(-35)

        if (s_state.m_rankStillAir)
        {
            climbSin = 0.4226f;    // sin(25)
        }

        if (s_state.m_rankTuckedWings)
        {
            diveSin = -0.7071f;    // sin(-45)
        }

        // The flight-path angle decides; the ascend/descend bits only break a tie.
        //
        // Testing them as an OR made the state flap. The server leaves MOVEMENTFLAG_ASCENDING set across long
        // stretches while the mount is actually level or sinking, so C and L alternated on consecutive frames
        // -- the log shows C/L/C/L with the pitch sitting at -0.2 to -0.5 the whole time. Every flip restarts
        // the clip, which is why a climb or a boost never played through. The flags now only decide when the
        // angle is inside the level band, which is the case they exist for: climbing straight up leaves the
        // pitch at zero.
        char raw;

        if (pathSin > climbSin)
        {
            raw = kStateClimb;
        }
        else if (pathSin < diveSin)
        {
            raw = kStateDive;
        }
        else if (flags & MOVEMENTFLAG_ASCENDING)
        {
            raw = kStateClimb;
        }
        else if (flags & MOVEMENTFLAG_DESCENDING)
        {
            raw = kStateDive;
        }
        else
        {
            raw = kStateLevel;
        }

        // Debounce: a freshly seen state must persist for kHoldMs before it is applied. Every change
        // restarts the mount's clip and the server's blend, so reacting to single jittery frames is what
        // made the animation crawl and the speed stutter. Same idea, same 60 ms, as the reference's
        // HoldMs. Strafe/backward and hover never reach here -- they return above -- so a held key or a
        // measured stop is still immediate.
        static char candidate = 0;
        static char applied = kStateLevel;
        static uint32_t candidateSinceMs = 0;

        if (raw == applied)
        {
            candidate = raw;
            candidateSinceMs = nowMs;
        }
        else if (raw != candidate)
        {
            candidate = raw;
            candidateSinceMs = nowMs;
        }
        else if (nowMs - candidateSinceMs >= kHoldMs)
        {
            applied = candidate;
        }

        if (applied == kStateDive)
        {
            *diving = true;
        }

        *glideOn = (applied == kStateLevel);
        return applied;
    }

    // --- the HUD globals --------------------------------------------------------------------------

    void PublishHud(const CGUnit_C* unit, char stateLetter, bool diving, bool glideOn, uint32_t nowMs)
    {
        const ServerSpeeds& speeds = s_state.m_speeds;

        // The mount can fly at all: the HUD frame shows for any flying mount, and the bar/glow only for one
        // the link drives. That mirrors the reference, where a stock mount gets no readout.
        const bool frameOn = (unit->m_movementData.m_flags & MOVEMENTFLAG_FLYING) != 0;

        if (frameOn != s_state.m_uiFrameOn)
        {
            s_state.m_uiFrameOn = frameOn;
            SetGlobalNumber("WXL_MountAnimFrameOn", frameOn ? 1 : 0);
        }

        if (diving != s_state.m_uiDiving)
        {
            s_state.m_uiDiving = diving;
            SetGlobalNumber("WXL_MountAnimDiving", diving ? 1 : 0);
        }

        if (glideOn != s_state.m_uiGlideOn)
        {
            s_state.m_uiGlideOn = glideOn;
            SetGlobalNumber("WXL_MountAnimGlideOn", glideOn ? 1 : 0);
        }

        // The state word. FlightStateText is off by default in the reference, and the addon hides the label
        // when the string is empty, so it is always published and the addon decides.
        const char* word = "";

        switch (stateLetter)
        {
            case kStateClimb: word = "Climbing"; break;
            case kStateDive:  word = "Diving";   break;
            case kStateHover: word = "Hovering"; break;
            default:          word = "Leveling"; break;
        }

        if (strcmp(word, s_state.m_uiState) != 0)
        {
            SStrCopy(s_state.m_uiState, word, sizeof(s_state.m_uiState));
            SetGlobalString("WXL_MountAnimState", word);
        }

        // Live speed as a percentage of unbuffed run speed, the same scale the server's own keys use.
        //
        // m_currentSpeed is yards/second, while the server's keys (280 / 600 / 1200) are percentages of
        // the 7 yd/s unbuffed run speed -- which is exactly what the reference divides by, a hard 7.0.
        // Using m_runSpeed here was wrong: that field carries the character's BASE run rate after
        // mount/aura modifiers, so dividing by it silently re-normalised the answer and a 600% dive could
        // read as 280.
        constexpr float kBaseRunSpeed = 7.0f;

        const float pct = (fabsf(unit->m_movementData.m_currentSpeed) / kBaseRunSpeed) * 100.0f;

        {
            static float lastLogged = -999.0f;

            if (fabsf(pct - lastLogged) > 25.0f)
            {
                lastLogged = pct;
                MODERN_ASSETS_LOG_INFO("speedflying: speed cur=%.2f run=%.2f flight=%.2f pct=%.0f",
                                       unit->m_movementData.m_currentSpeed, unit->m_movementData.m_runSpeed,
                                       unit->m_movementData.m_flightSpeed, pct);
            }
        }

        char pctText[16];
        SStrPrintf(pctText, sizeof(pctText), "%.0f%%", pct);

        if (strcmp(pctText, s_state.m_uiSpeedPct) != 0)
        {
            SStrCopy(s_state.m_uiSpeedPct, pctText, sizeof(s_state.m_uiSpeedPct));
            SetGlobalString("WXL_MountAnimSpeedPct", pctText);
        }

        // Bar fill: the left half spans the plain glide up to the dive ceiling, the right half the dive
        // ceiling to the boost ceiling -- exactly the reference's split, so a normal dive fills half.
        //
        // Three details that decide whether the bar moves at all, all taken from the reference's
        // BarFracForPct rather than guessed:
        //   - The empty end is levelPct PLUS a dead band. Without it the bar sits on zero through the
        //     whole ordinary glide, because the glide IS the level speed.
        //   - Under the dead band the fill is a true 0, and only above it does it climb.
        //   - The split point is the dive ceiling (levelPct is the empty end, not the middle).
        constexpr float kBarDeadbandPct = 25.0f;

        const float level = speeds.m_levelPct > 1.0f ? speeds.m_levelPct : 280.0f;
        const float dive  = speeds.m_divePct  > level ? speeds.m_divePct : level;
        const float boost = speeds.m_boostPct > dive ? speeds.m_boostPct : dive;
        const float zero  = level + kBarDeadbandPct;

        float fill = 0.0f;

        if (dive > zero && boost > dive)
        {
            if (pct > zero)
            {
                fill = (pct <= dive)
                    ? 0.5f * ((pct - zero) / (dive - zero))
                    : 0.5f + 0.5f * ((pct - dive) / (boost - dive));
            }
        }
        else
        {
            fill = pct > zero ? 1.0f : 0.0f;
        }

        if (fill < 0.0f)
        {
            fill = 0.0f;
        }

        if (fill > 1.0f)
        {
            fill = 1.0f;
        }

        if (fabsf(fill - s_state.m_uiFill) > 0.005f)
        {
            s_state.m_uiFill = fill;
            SetGlobalNumber("WXL_MountAnimFill", fill);
            MODERN_ASSETS_LOG_INFO("hud: fill=%.3f pct=%.0f level=%.0f dive=%.0f boost=%.0f",
                                   fill, pct, level, dive, boost);
        }

        // Where the boost tint starts: the dive ceiling's position on the same curve. The dive ceiling
        // sits at exactly half by construction, so the tint begins at the bar's midpoint.
        if (fabsf(0.5f - s_state.m_uiDiveFrac) > 0.005f)
        {
            s_state.m_uiDiveFrac = 0.5f;
            SetGlobalNumber("WXL_MountAnimDiveFrac", 0.5);
        }

        // The orb row shows the LOCAL estimate, not the raw announce value: the announce is only every few
        // seconds, so the raw count sits stale between them and the row disagreed with what SPACE would
        // actually accept. EffectiveStacks is the same function the press uses, so the two cannot disagree.
        const int32_t shownStacks = EffectiveStacks(nowMs);

        if (shownStacks != s_state.m_uiStacks)
        {
            s_state.m_uiStacks = shownStacks;
            SetGlobalNumber("WXL_MountAnimStacks", s_state.m_uiStacks);
        }

        if (static_cast<int32_t>(speeds.m_maxStacks) != s_state.m_uiMaxStacks)
        {
            s_state.m_uiMaxStacks = static_cast<int32_t>(speeds.m_maxStacks);
            SetGlobalNumber("WXL_MountAnimMaxStacks", s_state.m_uiMaxStacks);
        }
    }

    // The HUD's own settings, published once. These are the reference INI's Hud / HudX / HudY / DebugHUD
    // keys; the addon reads them back and hides the frame when Hud is off.
    void PublishHudSettings()
    {
        static bool pushed = false;

        if (pushed && !NeedsRepublish(L()))
        {
            return;
        }

        pushed = true;

        SetGlobalNumber("WXL_MountAnimHudOn", 1);   // Hud=1
        SetGlobalNumber("WXL_MountAnimHudX", 0);    // HudX=0
        SetGlobalNumber("WXL_MountAnimHudY", -200); // HudY=-200, a little below centre
        SetGlobalNumber("WXL_MountAnimDbgHud", g_debugHud ? 1 : 0);
        SetGlobalNumber("WXL_MountAnimText", 0);    // FlightStateText=0
        SetGlobalNumber("WXL_SF", 1);
    }

    // Pushes the server-owned constants the addon needs once, and again whenever they change.
    void PublishConstants()
    {
        static uint32_t pushedMaxStacks = 0;
        static uint32_t pushedStackMs = 0;
        static uint32_t pushedStackDiveMs = 0;

        if (NeedsRepublish(L()))
        {
            pushedMaxStacks = 0;
            pushedStackMs = 0;
            pushedStackDiveMs = 0;
        }

        const ServerSpeeds& speeds = s_state.m_speeds;

        if (speeds.m_maxStacks != pushedMaxStacks)
        {
            pushedMaxStacks = speeds.m_maxStacks;
            SetGlobalNumber("WXL_MountAnimMaxStacks", static_cast<double>(speeds.m_maxStacks));
        }

        if (speeds.m_stackMs != pushedStackMs)
        {
            pushedStackMs = speeds.m_stackMs;
            SetGlobalNumber("WXL_MountAnimStackMs", static_cast<double>(speeds.m_stackMs));
        }

        if (speeds.m_stackDiveMs != pushedStackDiveMs)
        {
            pushedStackDiveMs = speeds.m_stackDiveMs;
            SetGlobalNumber("WXL_MountAnimStackDiveMs", static_cast<double>(speeds.m_stackDiveMs));
        }

        // The accrual anchor belongs to the SERVER's clock and is pushed only when an announce arrives --
        // rewriting it every frame reset the addon's interpolation and the recharge bar never advanced.
        if (s_state.m_anchorDirty)
        {
            s_state.m_anchorDirty = false;
            SetGlobalNumber("WXL_MountAnimOrbAccum", static_cast<double>(speeds.m_stackAccumMs));
            SetGlobalNumber("WXL_MountAnimOrbAnchorAt", static_cast<double>(OsGetAsyncTimeMs()));
        }
    }

    // --- the link ---------------------------------------------------------------------------------

    void SendStateMessage(const CGUnit_C* unit, char stateLetter, bool advFly, uint32_t nowMs)
    {
        char body[16];
        SStrPrintf(body, sizeof(body), "%c%c", advFly ? '1' : '0', stateLetter);

        SendLink("WXLMS", body);

        s_state.m_sentAdvFly = advFly;
        s_state.m_sentState = stateLetter;
        s_state.m_lastSendMs = nowMs;
        s_state.m_lastAnyMs = nowMs;

        (void)unit;
    }

    // A boost is a one-shot: the server spends a stack and decides the speed, so the client only asks.
    void SendBoost(bool advFly, uint32_t nowMs)
    {
        if (!advFly)
        {
            return;
        }

        // No 200 ms floor here. A boost is not a state reiteration -- the server deliberately exempts it
        // from its own flood guard because each press spends a stack and must be answered. Throttling it
        // client-side is what made a held-stack press silently do nothing.
        SendLink("WXLMS", "1B");

        s_state.m_lastSendMs = nowMs;
        s_state.m_lastAnyMs = nowMs;
    }

    // How many stacks the server can have by now. The announce is the truth but it only arrives every few
    // seconds, and it is also the only thing that ever told us the count -- so with the confirmed count alone
    // a stack that has since recharged (or one that just got granted) reads as absent and the press is
    // dropped. This extrapolates from the last announce using the same cadence the server uses.
    int32_t EffectiveStacks(uint32_t nowMs)
    {
        const ServerSpeeds& speeds = s_state.m_speeds;

        if (!speeds.m_received || speeds.m_maxStacks == 0)
        {
            return 0;
        }

        int32_t stacks = s_state.m_stacksBase;

        // Dive recharges faster; use the cadence the current flight state implies.
        const uint32_t interval = s_state.m_sentState == kStateDive
            ? (speeds.m_stackDiveMs ? speeds.m_stackDiveMs : speeds.m_stackMs)
            : speeds.m_stackMs;

        if (!interval)
        {
            return stacks;
        }

        const uint32_t elapsed = nowMs - s_state.m_stacksBaseMs;
        stacks += static_cast<int32_t>(elapsed / interval);

        if (stacks < 0)
        {
            stacks = 0;
        }

        if (stacks > static_cast<int32_t>(speeds.m_maxStacks))
        {
            stacks = static_cast<int32_t>(speeds.m_maxStacks);
        }

        return stacks;
    }

    // Records a local spend so the estimate drops immediately instead of re-claiming the stack until the
    // next announce lands.
    void NoteLocalSpend(uint32_t nowMs)
    {
        const int32_t stacks = EffectiveStacks(nowMs);

        s_state.m_stacksBase = stacks > 0 ? stacks - 1 : 0;
        s_state.m_stacksBaseMs = nowMs;
    }

    // The talent panel's requests, raised by the addon through WXL_TalentToggle / WXL_TalentConfirm / etc.
    // The panel writes the intent into a global; this picks it up so the addon needs no outbound path of
    // its own.
    // Shows or hides the talent panel. Talents.lua builds WXL_TalentFrame on its first Update once
    // WXL_TalentD exists, and wires the close button, but nothing in it ever OPENS the frame -- that was the
    // module's job and it was missing.
    // Calls a zero-argument global function, returning false if it is absent or errors. Used to hand the
    // toggle to the addon so it decides between the talent window's Riding tab and the standalone panel.
    bool CallGlobalFunction(const char* name)
    {        lua_State* lua = L();

        if (!lua || !name)
        {
            return false;
        }

        lua_getglobal(lua, name);

        if (!lua_isfunction(lua, -1))
        {
            lua_pop(lua, 1);
            return false;
        }

        if (lua_pcall(lua, 0, 0, 0) != 0)
        {
            lua_pop(lua, 1);
            return false;
        }

        return true;
    }

    void ToggleTalentPanel(bool forceOpen)
    {
        lua_State* lua = L();

        if (!lua)
        {
            return;
        }

        // The addon exposes WXL_OpenRiding, which opens the talent window on the Riding tab when that tab
        // exists and falls back to the standalone panel otherwise. Doing the decision there keeps the frame
        // handling in Lua, where the frames actually live.
        if (CallGlobalFunction(forceOpen ? "WXL_OpenRiding" : "WXL_ToggleRiding"))
        {
            return;
        }

        lua_getglobal(lua, "WXL_TalentFrame");

        if (!lua_istable(lua, -1) && !lua_isuserdata(lua, -1))
        {
            lua_pop(lua, 1);
            return;
        }

        bool shown = false;

        // lua_pcall, never lua_call: an error inside Show/Hide IsShown longjmps, and there is nothing here to
        // unwind it, so an unprotected call would corrupt the Lua stack and take the client down.
        lua_getfield(lua, -1, "IsShown");

        if (lua_isfunction(lua, -1))
        {
            lua_pushvalue(lua, -2);

            if (lua_pcall(lua, 1, 1, 0) == 0)
            {
                shown = lua_toboolean(lua, -1) != 0;
            }

            lua_pop(lua, 1);
        }
        else
        {
            lua_pop(lua, 1);
        }

        const char* method = (forceOpen || !shown) ? "Show" : "Hide";

        lua_getfield(lua, -1, method);

        if (lua_isfunction(lua, -1))
        {
            lua_pushvalue(lua, -2);

            if (lua_pcall(lua, 1, 0, 0) != 0)
            {
                lua_pop(lua, 1);
            }
        }
        else
        {
            lua_pop(lua, 1);
        }

        lua_pop(lua, 1);
    }

    // Calls a zero-argument global function for its side effect only (AscendStop). An absent global is not
    // an error: an older UI simply has nothing to stop.
    void CallGlobalVoid(const char* name)
    {
        lua_State* lua = L();

        if (!lua || !name)
        {
            return;
        }

        lua_getglobal(lua, name);

        if (!lua_isfunction(lua, -1))
        {
            lua_pop(lua, 1);
            return;
        }

        if (lua_pcall(lua, 0, 0, 0) != 0)
        {
            lua_pop(lua, 1);
        }
    }

    // TEMPORARY probe: logs which talent frames actually exist at runtime so the Riding tab can be attached
    // to the real one instead of guessed at. Remove once the tab is wired.
    void ProbeTalentFrameOnce(lua_State* lua)
    {
        static int ticks = 0;

        if (ticks > 400)
        {
            return;
        }

        ++ticks;

        if (ticks != 400)
        {
            return;
        }

        static const char* kNames[] = {
            "PlayerTalentFrame", "PlayerTalentFrameTab1", "PlayerTalentFrameTab2",
            "PlayerTalentFrameTab3", "PlayerTalentFrameTab4", "PlayerTalentFrameTab5",
            "PlayerTalentFrameTab6", "PlayerTalentFrameTab7", "PlayerTalentFrameTab8",
            "PlayerTalentFrameTab9", "PlayerTalentFrameTab10", "PlayerTalentFrameTab11",
            "TalentFrame", "InspectTalentFrame", "InspectTalentFrameTab1",
            "InspectTalentFrameTab2", "InspectTalentFrameTab3", "InspectTalentFrameTab4",
            "InspectTalentFrameTab5", "InspectTalentFrameTab6", "InspectTalentFrameTab7",
            "InspectTalentFrameTab8", "InspectTalentFrameTab9", "InspectTalentFrameTab10",
            "InspectTalentFrameTab11", "PlayerTalentFrameTab_1", "InspectTalentFramePetTab",
        };

        for (const char* name : kNames)
        {
            lua_getglobal(lua, name);
            const char* kind = lua_isnil(lua, -1) ? "nil" : (lua_istable(lua, -1) ? "table" : "frame");

            if (lua_istable(lua, -1) || lua_isuserdata(lua, -1))
            {
                const char* text = "";

                lua_getfield(lua, -1, "GetText");

                if (lua_isfunction(lua, -1))
                {
                    lua_pushvalue(lua, -2);

                    if (lua_pcall(lua, 1, 1, 0) == 0)
                    {
                        if (lua_isstring(lua, -1))
                        {
                            text = lua_tostring(lua, -1);
                        }
                    }

                    lua_pop(lua, 1);
                }
                else
                {
                    lua_pop(lua, 1);
                }

                MODERN_ASSETS_LOG_INFO("talentprobe: %s = %s text='%s'", name, kind, text);
            }
            else
            {
                MODERN_ASSETS_LOG_INFO("talentprobe: %s = %s", name, kind);
            }

            lua_pop(lua, 1);
        }

        // Does the real frame expose numTabs, and which frame is the tab strip parent?
        lua_getglobal(lua, "PlayerTalentFrame");

        if (lua_istable(lua, -1) || lua_isuserdata(lua, -1))
        {
            lua_getfield(lua, -1, "numTabs");
            MODERN_ASSETS_LOG_INFO("talentprobe: PlayerTalentFrame.numTabs = %s",
                                   lua_isnumber(lua, -1) ? "number" : "absent");
            lua_pop(lua, 1);

            lua_getfield(lua, -1, "selectedTab");
            MODERN_ASSETS_LOG_INFO("talentprobe: PlayerTalentFrame.selectedTab = %s",
                                   lua_isnumber(lua, -1) ? "number" : "absent");
            lua_pop(lua, 1);
        }

        lua_pop(lua, 1);
    }

    void PollTalentRequests(uint32_t nowMs)
    {
        static uint32_t lastToggle = 0;

        double flag = 0.0;

        if (ReadGlobalNumber("WXL_TalentToggle", &flag) && flag != 0.0)
        {
            // Only consumed when it is acted on: clearing it inside the throttle window dropped the click
            // entirely, so the button appeared dead.
            if (nowMs - lastToggle > 250)
            {
                lastToggle = nowMs;
                SetGlobalNumber("WXL_TalentToggle", 0);
                SendLink("WXLTT", "Q");
                ToggleTalentPanel(false);
            }
        }

        // SHIFT+T, the reference's binding. Edge-triggered so holding it does not strobe the panel.
        static bool comboDown = false;
        const bool shift = (OsInputIsKeyDown(KEY_LSHIFT) != 0) || (OsInputIsKeyDown(KEY_RSHIFT) != 0);
        const bool t = OsInputIsKeyDown(KEY_T) != 0;
        const bool combo = shift && t && (OsInputIsAppActive() != 0);

        if (combo && !comboDown && nowMs - lastToggle > 250)
        {
            lastToggle = nowMs;
            SetGlobalNumber("WXL_TalentToggle", 0);
            SendLink("WXLTT", "Q");
            ToggleTalentPanel(false);
        }

        comboDown = combo;

        // WXL_TalentReset: the panel's Reset button. "R\tA" refunds the whole tree.
        double reset = 0.0;

        if (ReadGlobalNumber("WXL_TalentReset", &reset) && reset != 0.0)
        {
            SetGlobalNumber("WXL_TalentReset", 0);
            SendLink("WXLTT", "R\tA");
            MODERN_ASSETS_LOG_INFO("talent: reset requested -> WXLTT R A");
        }

        char buffer[512];

        if (ReadGlobalString("WXL_TalentSend", buffer, sizeof(buffer)) && buffer[0])
        {
            // Rate-limited like everything else, and cleared only once sent: the addon may leave the global
            // set, and without this a non-empty value resent every frame.
            if (nowMs - lastToggle > 200)
            {
                lastToggle = nowMs;
                SendLink("WXLTT", buffer);
                MODERN_ASSETS_LOG_INFO("talent: send %s", buffer);
                SetGlobalString("WXL_TalentSend", "");
            }
        }
    }
}

namespace SpeedFlying
{
    void Initialize()
    {
        if (s_state.m_initialised)
        {
            return;
        }

        s_state.m_initialised = true;

        // The panel's tree is rendered from what the server announced, so ask for it once the world is up.
        // A Q before the player exists is answered with nothing, so this is repeated by Update until a
        // tally arrives.
    }

    void Update(uint32_t timeMs)
    {
        lua_State* lua = L();

        if (!lua)
        {
            return;
        }

        PollInbound(timeMs);
        PollTalentRequests(timeMs);
        TalentUI::Update(timeMs);
        ProbeTalentFrameOnce(lua);

        CGPlayer_C* player = static_cast<CGPlayer_C*>(
            ClntObjMgrObjectPtr(ClntObjMgrGetActivePlayer(), TYPE_PLAYER, __FILE__, __LINE__));

        if (!player)
        {
            return;
        }

        CGUnit_C* unit = static_cast<CGUnit_C*>(player);

        // Ask for the tree until the first tally arrives: the server answers a Q with a whole tree, and a Q
        // sent before the world was ready is simply answered with nothing.
        // A separate clock: SendStateMessage also touches m_lastAnyMs once a second, so sharing it meant this
        // condition could never come true while the flight link was live and a missing tally was never asked
        // for again.
        if (!s_state.m_talentBody[0] && timeMs - s_state.m_lastTalentQueryMs > 2000)
        {
            s_state.m_lastTalentQueryMs = timeMs;
            SendLink("WXLTT", "Q");
        }

        PublishConstants();

        // The link only drives a mount whose model carries the AdvFly block; that is what keeps the speed
        // change off stock mounts and druid flight forms. The animation side already resolved that, so the
        // cheapest honest test is whether the mount's flight chooser picked an AdvFly id.
        const bool onMount = unit->m_mountModel != nullptr;

        bool advFly = false;

        if (onMount)
        {
            const M2Data* data = unit->m_mountModel->m_shared ? unit->m_mountModel->m_shared->m_data : nullptr;

            if (data)
            {
                const uint32_t advFlyIds[] = { 1532, 1722, 1724, 1524, 1520, 1522, 1530, 1704, 1678 };

                for (uint32_t id : advFlyIds)
                {
                    if (unit->m_mountModel->HasSequence(data, id))
                    {
                        advFly = true;
                        break;
                    }
                }
            }
        }

        bool diving = false;
        bool glideOn = false;
        const char stateLetter = ChooseState(unit, &diving, &glideOn, timeMs);

        PublishHudSettings();

        // WXL_MountAnimAdvFly gates every speed effect in the HUD, and DiveNow switches the orb row to the
        // faster dive recharge cadence.
        static bool pushedAdvFly = false;
        static bool pushedDiveNow = false;

        if (NeedsRepublish(L()))
        {
            pushedAdvFly = false;
            pushedDiveNow = false;
        }

        if (advFly != pushedAdvFly)
        {
            pushedAdvFly = advFly;
            SetGlobalNumber("WXL_MountAnimAdvFly", advFly ? 1 : 0);
        }

        if (diving != pushedDiveNow)
        {
            pushedDiveNow = diving;
            SetGlobalNumber("WXL_MountAnimDiveNow", diving ? 1 : 0);
        }

        // One line per state change: proves whether this whole path runs (it is gated on the active player
        // and on MOUNTED), what the mount resolved as, and what the HUD was told.
        {
            static char lastLetter = 0;
            static bool lastAdvFly = false;

            if (stateLetter != lastLetter || advFly != lastAdvFly)
            {
                lastLetter = stateLetter;
                lastAdvFly = advFly;

                MODERN_ASSETS_LOG_INFO("speedflying: mounted=%d advfly=%d state=%c speed=%.2f pitch=%.3f flags=0x%x",
                                       onMount ? 1 : 0, advFly ? 1 : 0, stateLetter,
                                       unit->m_movementData.m_currentSpeed,
                                       unit->m_movementData.m_pitchSin,
                                       unit->m_movementData.m_flags);
            }
        }

        PublishHud(unit, stateLetter, diving, glideOn, timeMs);

        // A '0' link tells the server to restore the mount's own speed and stand down, so it is sent as soon
        // as the player is not on an AdvFly mount rather than being withheld.
        if (!advFly)
        {
            if (s_state.m_sentAdvFly && timeMs - s_state.m_lastSendMs >= kSendIntervalMs)
            {
                SendStateMessage(unit, kStateLevel, false, timeMs);
            }

            return;
        }

        if (stateLetter != s_state.m_sentState || timeMs - s_state.m_lastSendMs >= kHeartbeatMs)
        {
            if (timeMs - s_state.m_lastSendMs >= kSendIntervalMs)
            {
                SendStateMessage(unit, stateLetter, true, timeMs);
            }
        }

        // SPACE is the boost key -- but only while a stack is held and the mount is link-eligible. Jumping
        // is the same key, so the order matters: the reference consumes it while flying, which is what this
        // does.
        // OsInputIsKeyDown wraps GetAsyncKeyState, so it reports a key held with the client unfocused or while
        // an edit box has focus. Both the boost and SHIFT+T are therefore gated on the game actually having
        // input focus and no text entry being active, otherwise a space typed into chat spends a real
        // server-side stack.
        const bool acceptInput = OsInputIsAppActive() != 0;

        static bool spaceDown = false;
        const bool spaceNow = OsInputIsKeyDown(KEY_SPACE) != 0;

        // Tracked before any early return above: if the key is pressed while the mount is not link-eligible,
        // the next eligible frame must not see a fresh press and spend a stack the player never asked for.
        const bool spacePressed = spaceNow && !spaceDown && acceptInput;
        const bool spaceReleased = !spaceNow && spaceDown && acceptInput;
        spaceDown = spaceNow;

        // SPACE must boost, never ascend. The reference swallows the key with an override binding, which
        // is the only thing that stops the base game's own ascend from firing -- polling the key alone
        // cannot, because the stock binding fires on the down-edge before this code runs. The binding may
        // only be swapped while the key is UP: changing it mid-press latches the ascend on permanently.
        const bool flying = (unit->m_movementData.m_flags & MOVEMENTFLAG_FLYING) != 0;
        const int32_t stacks = EffectiveStacks(timeMs);
        const bool wantSpaceBound = advFly && flying && stacks > 0;

        if (!spaceNow && wantSpaceBound != s_state.m_uiSpaceBound)
        {
            s_state.m_uiSpaceBound = wantSpaceBound;
            SetGlobalNumber("WXL_MountAnimSpaceBound", wantSpaceBound ? 1 : 0);
        }

        if (spacePressed && stacks > 0 && flying)
        {
            SendBoost(advFly, timeMs);
            s_state.m_lastBoostMs = timeMs;

            // Force the flap clip from its start. Without this a chained burst plays nothing new, and
            // a burst during a climb or dive never shows at all.
            unit->m_flgtBoostRequestMs = timeMs ? timeMs : 1;

            NoteLocalSpend(timeMs);
        }

        // Every SPACE release stops the ascend, even when this module did not start it. While the
        // override owns the key the base game never sees the key-up that calls AscendStop, and a bind
        // swap under a press can drop that key-up too. AscendStop is idempotent.
        if (spaceReleased)
        {
            CallGlobalVoid("AscendStop");
        }

        // D5 Aftershock: releasing the boost key while the burst is still fading lets a diver commit it at
        // once. The server checks the rank itself, so sending it unranked costs nothing.
        if (spaceReleased && s_state.m_lastBoostMs
            && timeMs - s_state.m_lastBoostMs < s_state.m_speeds.m_boostFadeMs
            && flying)
        {
            SendLink("WXLMS", "1X");
            s_state.m_lastSendMs = timeMs;
            s_state.m_lastAnyMs = timeMs;
        }
    }
}
