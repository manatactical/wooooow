#ifndef WOW_SOURCE_UI_TALENTUI_H
#define WOW_SOURCE_UI_TALENTUI_H

#include <cstdint>

// The talent panel's engine half.
//
// The panel itself is Interface\AddOns\WXLSpeedflying\Talents.lua: it lays out three branches of four
// tiers from a table the module publishes and reads back a set of globals every frame. None of those
// globals existed in this client, so the panel never built.
//
// The server module owns the tree -- the gates, the refusals, the point tally and the ranks are all
// authoritative there and this file only RENDERS them. That is deliberate: predicting the gates client-side
// would let the panel show a talent as buyable that the server then refuses, and the reference implementation
// does not predict either.
namespace TalentUI
{
    // Publishes the talent table once, and keeps the panel's globals in sync with the last server tally.
    void Update(uint32_t timeMs);

    // The three numbers the panel needs from a "WXLTT\tT\t..." body. Called by SpeedFlying once it has
    // parsed one.
    void OnTally(uint32_t points, uint32_t spent, uint32_t unspent);

    // Every confirmed rank, from a tally body whose counters have already been skipped.
    void OnRanks(const char* tallyBody);

    // The partial-point progress bar, from a bar body "P:<permille>,<yards>".
    void OnPartial(uint32_t permille, uint32_t needYards);

    // A refusal reason from the server, shown as the panel's status line.
    void OnStatus(const char* reason);
}

#endif
