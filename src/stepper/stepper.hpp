#pragma once
// The sub-tick stepper (ported from the Frame Window Counter, v1.121–v1.150) and the presses between frames it needs.
//
// Two parts:
//   - subtick.cpp: a click with sub > 0 is "armed" when its frame comes (by xdBot's playback, or the stepper), and pressed
//     from inside the next physics update of player 1, which is split there the way Click Between Frames splits it. So a
//     macro with clicks between frames plays back in xdBot itself, stepper on or off.
//   - stepper.cpp: the stepper. The level only moves when a step runs a frame. The timeline it edits is xdBot's own macro
//     (Global::macro.inputs): clicks you place go in there at the stepper's spot (whole frames or parts of one), the panel
//     moves / deletes / undoes them, and the macro is saved the usual way (or as .cml).

#include "../includes.hpp"
#include "core.hpp"

namespace xdstep {

// ---- presses between frames ----

struct SubState {
    std::vector<input> armed;   // pressed in the next physics of player 1, in order of sub
    bool mid = false;           // inside the split of an update (the pieces' own update calls go straight through)
    bool p2Done = false;        // player 2's update was done inside player 1's
    bool p1Split = false, p2Split = false; // moved in pieces this step
    float rotDt = 0.f;          // the last piece's length (rotation still to do at the end of the step)
    cocos2d::CCPoint p1Pos, p2Pos;
    int pressing = 0;           // inside one of these presses (xdBot's handleButton hook treats it as the macro's own)
    // counts, for the readout / a check
    int splitUpdates = 0, bufferedUpdates = 0;
};
SubState& sub();

// Arms a click for the next physics (kept in order of sub).
void arm(input const& e);
// Presses one click now, through GD's handleButton, with xdBot's Flip 2-player controls applied as its playback does.
void pressNow(GJBaseGameLayer* gl, input const& e);
// Presses whatever is still armed now (the physics never came) and clears it.
void flushArmed(GJBaseGameLayer* gl);
void clearArmed();
// Click Between Frames is loaded and not switched off: it splits the update itself.
bool cbfActive();
// why the stepper can't run with the input mods loaded now ("" = it can): CBF on, its Physics Bypass, CBF Lite, SIP
std::string cbfConflictText();

// ---- the stepper ----

bool stepperOn();
// inside the stepper's own run of a step (its presses and the deaths it handles)
bool stepperStepping();
void stepperToggle();
void stepperStart(PlayLayer* pl);
void stepperStop(PlayLayer* pl, bool playOn);

// From GJBaseGameLayer::processCommands while the stepper is on (after GD's own): the timeline's clicks of the frame
// that just came are pressed (whole ones now, the others armed).
void stepperAfterCommands(GJBaseGameLayer* gl);
// GJBaseGameLayer::update while the stepper is on: the stepper's frame instead of the game's.
void stepperFrame(GJBaseGameLayer* gl, float dt);
// GJBaseGameLayer::handleButton while the stepper is on: true if the click was taken (put on the timeline).
bool stepperTakeClick(bool down, int button, bool isPlayer1);
// PlayLayer::destroyPlayer while the stepper is on: true if the death is the stepper's (the player isn't killed).
bool stepperTakeDeath(PlayLayer* pl, PlayerObject* player);

} // namespace xdstep
