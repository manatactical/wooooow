#ifndef WOW_SOURCE_UI_SPEEDFLYING_H
#define WOW_SOURCE_UI_SPEEDFLYING_H

#include <cstdint>

// Client half of the mana-speedflying link.
//
// The addon under Interface\AddOns\WXLSpeedflying is the presentation: it owns the HUD frames, the talent
// panel and the micro-menu button, and it reads ~45 globals back out of _G. This file is the half that was
// missing -- nothing in this client ever wrote those globals, which is why the HUD never appeared.
//
// It talks to the server module (mod-mana-speedflying) over the addon channel, which is the only channel a
// native client module has. The protocol is documented in that module's README and implemented in
// src/mod_mana_speedflying.cpp; both sides must agree exactly:
//
//   client -> server   WXLMS\t<advfly:0|1><state:L|C|D|H|B|X>
//                      WXLTT\tW\t<id>                          learn one rank
//                      WXLTT\tB\t<id>:<target>[,...]           learn to an absolute rank
//                      WXLTT\tR\t<1|2|3|A>                     refund a branch / everything
//                      WXLTT\tQ                                request the tree
//   server -> client   WXLMS\tS:<levelPct>,<divePct>,<boostPct>,<cooldownMs>,<maxStacks>,<boostFadeMs>,
//                             <stacks>,<stackMs>,<stackDiveMs>[,<accumMs>,<intervalMs>]
//                      WXLTT\tT\t<points>,<spent>,<unspent>[,<id>:<rank>,...][,P<permille>,N<yards>]
//                      WXLTT\tP\t<permille>,<yards>
//                      WXLTT\tF\t<reason>
//
// Sending has to go through Lua: the engine builds CMSG_MESSAGECHAT in Script_SendAddonMessage and there is
// no C++ equivalent, so SpeedFlying.lua exposes WXL_SendAddon for this module to call. Receiving needs
// nothing from us -- Listener.lua already stashes every inbound body in a global.
namespace SpeedFlying
{
    // Called once, after the frame script context exists. Safe to call before the addon is loaded.
    void Initialize();

    // Called once per frame from the world frame tick. Cheap when not mounted.
    void Update(uint32_t timeMs);
}

#endif
