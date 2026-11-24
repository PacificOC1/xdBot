// The sub-tick stepper, ported from the Frame Window Counter (v1.121 stepper, v1.122 timeline, v1.123 keys and hold-to-step,
// v1.124 stop before deaths, v1.137 the panel, 2.4 billion Hz) to step xdBot's own macro.
//
// Where things are in time: the stepper sits between two of GD's steps. The coming step's processCommands brings xdBot's
// frame to `frame` (the level clock × TPS + 1, the way xdBot counts it), and that's where xdBot presses the frame's
// inputs (at the end of processCommands, before the players' physics, which GD runs after it). So a click at frame F
// part p means: pressed p of the way into the physics right after frame F's press point. Part 0 is a plain whole-frame
// click, the same as xdBot records. The panel, the readout and the saved macro all use those frame numbers.
//
// Going back: a checkpoint is kept before every step (Stepper: frames you can go back), loaded the way practice mode
// loads them, with xdBot's own player data (practice fixes) put back on top. Jump (and left / right) are then set the way
// the timeline has them there.
#include "stepper.hpp"

#include "../practice_fixes/practice_fixes.hpp"
#include "../hacks/show_trajectory.hpp"
#include "../ui/record_layer.hpp"
#include "../ui/game_ui.hpp"

#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PauseLayer.hpp>

#include <deque>
#include <functional>

#ifdef GEODE_IS_WINDOWS
#include <Windows.h>
#endif

namespace xdstep {

namespace {

enum Act {
    kTickBack = 0, kStepBack, kStep, kTick, // the step buttons (repeat while held)
    kPlay, kReturn, kHold, kAdd, kPull, kDeleteSel, kDeleteNext, kUndo, kSelPrev, kSelNext, kMoveEarlier, kMoveLater,
    kGraphBack, kGraphFwd, kGraphIn, kGraphOut, kPath, kMark, kFiner, kCoarser, kOff
};

struct StepBack {
    CheckpointObject* cp = nullptr;
    int frame = 0;  // the coming frame then
    int diedAt = -1;
};

struct State {
    bool on = false;
    int parts = 10;         // parts a frame is split into (stepper_hz / TPS)
    int frame = 0;          // the coming frame: its clicks are pressed when its step runs
    int part = 0;           // how far into it the stepper is
    long long want = 0;     // parts to move (negative = back)
    int pressedUpTo = -1;   // the frames up to this one had their clicks pressed
    bool stepping = false;  // inside the stepper's own run of a step
    bool loading = false;   // inside the stepper's own checkpoint load
    int commands = 0;       // processCommands calls in this step (0: the step didn't run)
    int seenFrame = -1;     // the frame xdBot counted in this step
    bool playing = false;
    double playAcc = 0;
    std::optional<Pos> playFrom;
    std::deque<StepBack> back; // oldest first
    int diedAt = -1;
    bool diedNow = false;
    bool stoppedByDeath = false;
    state prevState = state::none;
    bool usedThisAttempt = false; // no progress or completion saved for this attempt
    double songBaseMs = -1;
    std::string note;
    Marks marks;
    std::optional<long long> lastPress;
    // a step key held: after a moment it keeps stepping (the stepper's own repeat, not the keyboard's)
    std::string holdKey;
    int holdDir = 0;
    bool holdWhole = false;
    double holdT = 0, holdAcc = 0;
    bool holdBlocked = false;
    bool keyStepped[4] = {};
    bool keyDownPrev[4] = {};
    double polledHeld[4] = {};
    // the panel's buttons (step / move buttons repeat while held)
    int toolHeld = -1;
    double toolHoldT = 0, toolHoldAcc = 0;
    bool toolTouched = false, toolDownPrev = false;
    // a button you're physically holding ([button][player 2]): a queued release with no press before it is GD's own
    bool physDown[4][2] = {};
    // the panel
    int selected = -1;
    double graphOffset = 0;
    int graphSpan = 50;
    std::vector<std::vector<input>> undo;
    long long editEarliest = LLONG_MAX;
    std::optional<Pos> replayTo;
    // nodes (children of the PlayLayer / its object layer)
    Ref<CCLabelBMFont> label;
    Ref<CCDrawNode> ghost;
    Ref<CCDrawNode> boxes;
    Ref<CCMenu> panel;
    Ref<CCDrawNode> graph;
    Ref<CCLabelBMFont> status;
    std::vector<Ref<CCLabelBMFont>> nums;
    std::vector<std::pair<Ref<CCMenuItemSpriteExtra>, int>> buttons;
    CCRect panelRect, graphRect;
    PlayLayer* builtFor = nullptr;
    long long boxesKey = LLONG_MIN;
    int fixesDropped = 0;   // xdBot frame fixes taken off after edits
};

State& st() {
    static State s;
    return s;
}

std::vector<input>& tl() { return Global::get().macro.inputs; }

double tps() { return std::max(1.0, static_cast<double>(Global::getTPS())); }

// one physics step: the largest float not over 1 / TPS (a bypass that adds up the time never finds two steps in one call)
float stepDt() {
    double const t = tps();
    float dt = static_cast<float>(1.0 / t);
    if (static_cast<double>(dt) > 1.0 / t) dt = std::nextafter(dt, 0.f);
    return dt;
}

bool setting(char const* key) { return Mod::get()->getSettingValue<bool>(key); }
int64_t settingInt(char const* key) { return Mod::get()->getSettingValue<int64_t>(key); }

long long posKey() { return keyOf(Pos{st().frame, st().part}, st().parts); }

std::string keyName(char const* setting) {
    auto binds = Mod::get()->getSettingValue<std::vector<geode::Keybind>>(setting);
    if (binds.empty()) return "(none)";
    return binds.front().toString();
}

bool keybindHeldNow(char const* setting) {
#ifdef GEODE_IS_WINDOWS
    auto binds = Mod::get()->getSettingValue<std::vector<geode::Keybind>>(setting);
    auto down = [](int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; };
    if (GetForegroundWindow() != GetActiveWindow()) return false; // (GD not in front)
    unsigned held = (down(VK_SHIFT) ? 1u : 0u) | (down(VK_CONTROL) ? 2u : 0u) | (down(VK_MENU) ? 4u : 0u);
    for (auto const& b : binds) {
        int const vk = static_cast<int>(b.key);
        if (vk <= 0 || vk > 0xFE) continue;
        if (down(vk) && (static_cast<unsigned>(b.modifiers.value) & 7u) == held) return true;
    }
#else
    (void)setting;
#endif
    return false;
}

bool twoPlayer(PlayLayer* pl) { return pl && pl->m_levelSettings && pl->m_levelSettings->m_twoPlayerMode; }

// xdBot's "2p" flag for a click on GD's player `isPlayer1`, as its recording stores it
bool flagFor(PlayLayer* pl, bool isPlayer1) {
    bool flag = twoPlayer(pl) ? isPlayer1 : false;
    if (Macro::flipControls()) flag = !flag;
    return flag;
}
// and the player a flag presses (GD's isPlayer1 argument after the flip)
bool isPlayer1Of(bool flag) { return Macro::flipControls() ? !flag : flag; }

std::string buttonName(int b) { return b == 2 ? "left" : b == 3 ? "right" : "jump"; }

std::string clickText(PlayLayer* pl, input const& e) {
    std::string who;
    if (twoPlayer(pl)) who = isPlayer1Of(e.player2) ? " (P1)" : " (P2)";
    return fmt::format("{} {}{}", e.down ? "press" : "release", buttonName(e.button), who);
}

void pauseSong(bool paused) {
    if (auto ch = FMODAudioEngine::sharedEngine()->m_backgroundMusicChannel) ch->setPaused(paused);
}

void releaseBack() {
    auto& s = st();
    auto& g = Global::get();
    for (auto& b : s.back) {
        if (!b.cp) continue;
        g.checkpoints.erase(b.cp);
        b.cp->release();
    }
    s.back.clear();
}

void popBack() {
    auto& s = st();
    if (s.back.empty()) return;
    auto b = s.back.back();
    s.back.pop_back();
    if (b.cp) {
        Global::get().checkpoints.erase(b.cp);
        b.cp->release();
    }
}

// ---- the game: one step, a load, jump as the timeline has it ----

// Jump (and left / right on platformer levels) held or let go the way the timeline has them just before the coming
// frame's press point. A checkpoint load or reset lets go of buttons by itself.
void syncHold(PlayLayer* pl) {
    if (!pl || !pl->m_player1) return;
    auto& s = st();
    bool const platformer = pl->m_levelSettings && pl->m_levelSettings->m_platformerMode;
    long long const key = keyOf(Pos{s.frame, 0}, s.parts) - 1;
    for (int f = 0; f < 2; ++f) {
        bool const flag = f == 1;
        for (int button = 1; button <= (platformer ? 3 : 1); ++button) {
            auto want = heldAt(tl(), key, button, flag);
            if (!want) continue;
            PlayerObject* p = (!twoPlayer(pl) || isPlayer1Of(flag)) ? pl->m_player1 : pl->m_player2;
            if (!p) continue;
            auto it = p->m_holdingButtons.find(button);
            bool const held = it != p->m_holdingButtons.end() && it->second;
            if (held == *want) continue;
            pressNow(pl, input(s.frame, button, flag, *want));
        }
    }
}

// Back to the game just before the step whose coming frame was b.frame.
void load(PlayLayer* pl, StepBack const& b) {
    auto& s = st();
    auto& g = Global::get();
    auto* cp = b.cp;
    if (!pl || !cp) return;
    s.loading = true;
    clearArmed();
    // (the reset below counts an attempt and the stepper's own jumps: those are put back after it, as FWC does)
    auto gsm = GameStatsManager::sharedState();
    int const statJumps = gsm->getStat("1"), statAttempts = gsm->getStat("2");
    int const levelAttempts = pl->m_level->m_attempts.value(), levelJumps = pl->m_level->m_jumps.value();
    int const layerAttempts = pl->m_attempts, layerJumps = pl->m_jumps;
    if (pl->m_checkpointArray) {
        pl->m_checkpointArray->addObject(cp);
        pl->resetLevel();
        bool found = false;
        if (auto arr = pl->m_checkpointArray) {
            for (unsigned i = arr->count(); i-- > 0;) {
                if (arr->objectAtIndex(i) == cp) {
                    arr->removeObjectAtIndex(i);
                    found = true;
                    break;
                }
            }
        }
        if (!found) pl->loadFromCheckpoint(cp);
    }
    else pl->loadFromCheckpoint(cp);
    pl->m_queuedButtons.clear(); // (resetLevel queues a jump release for the next step)
    gsm->setStat("1", statJumps);
    gsm->setStat("2", statAttempts);
    pl->m_level->m_attempts = levelAttempts;
    pl->m_level->m_jumps = levelJumps;
    pl->m_attempts = layerAttempts;
    pl->m_jumps = layerJumps;
    // xdBot's practice fixes: the players as they were when the checkpoint was made
    if (auto it = g.checkpoints.find(cp); it != g.checkpoints.end()) {
        PlayerPracticeFixes::applyData(pl->m_player1, it->second.p1, false);
        PlayerPracticeFixes::applyData(pl->m_player2, it->second.p2, true);
    }
    s.loading = false;
    s.frame = b.frame;
    s.pressedUpTo = b.frame - 1;
    s.diedAt = b.diedAt;
    syncHold(pl);
    g.safeMode = true; // (xdBot's resetLevel cleared it)
    if (g.showTrajectory) ShowTrajectory::updateTrajectory(pl);
}

// Runs the coming step with its clicks, after keeping a checkpoint to come back to. False if it didn't run (or killed the
// player and *Stepper: deaths* is to stop before deaths: then it's undone).
bool stepFrame(PlayLayer* pl) {
    auto& s = st();
    if (!pl || pl->m_levelEndAnimationStarted) return false;
    syncHold(pl);
    if (auto cp = pl->createCheckpoint()) {
        cp->retain();
        s.back.push_back({cp, s.frame, s.diedAt});
        auto const cap = static_cast<std::size_t>(std::max<int64_t>(1, settingInt("stepper_back")));
        while (s.back.size() > cap) {
            auto& f = s.back.front();
            if (f.cp) {
                Global::get().checkpoints.erase(f.cp);
                f.cp->release();
            }
            s.back.pop_front();
        }
    }
    int const frameBefore = s.frame;
    bool const kept = !s.back.empty() && s.back.back().frame == frameBefore;
    clearArmed();
    s.diedNow = false;
    s.commands = 0;
    s.seenFrame = -1;
    s.stepping = true;
    Global::get().leftOver = 0.f; // (xdBot's TPS bypass: one step a call)
    for (int i = 0; i < 16 && s.commands == 0; ++i) {
        pl->m_extraDelta = 0.0;
        pl->update(stepDt());
    }
    if (s.commands == 0) { // (a time just short of GD's step: let it add up this time)
        for (int i = 0; i < 16 && s.commands == 0; ++i) pl->update(stepDt());
        pl->m_extraDelta = 0.0;
    }
    s.stepping = false;
    flushArmed(pl); // (clicks whose physics never came)
    if (s.commands == 0) {
        if (kept) popBack();
        return false;
    }
    s.frame = (s.seenFrame >= 0 ? s.seenFrame : Global::getCurrentFrame()) + 1;
    if (s.diedNow && Mod::get()->getSettingValue<std::string>("stepper_deaths") != "Go through (noclip)" && kept) {
        auto b = s.back.back();
        s.back.pop_back();
        load(pl, b);
        Global::get().checkpoints.erase(b.cp);
        b.cp->release();
        s.stoppedByDeath = true;
        s.diedNow = false;
        s.playing = false;
        return false;
    }
    s.stoppedByDeath = false;
    if (s.diedNow && s.diedAt < 0) s.diedAt = frameBefore;
    return true;
}

// ---- moves and edits ----

void stepOnce(int dir, bool whole) {
    auto& s = st();
    s.playing = false;
    long long const by = whole ? 1LL * dir * s.parts : dir;
    long long const cap = 240LL * s.parts;
    s.want = std::clamp<long long>(s.want + by, -cap, cap);
    s.replayTo.reset();
}

int selectedIndex() {
    auto& s = st();
    if (s.selected >= 0 && s.selected < static_cast<int>(tl().size())) return s.selected;
    s.selected = -1;
    auto const i = firstAtOrAfter(tl(), posKey());
    return i < tl().size() ? static_cast<int>(i) : -1;
}

void pushUndo() {
    auto& s = st();
    s.undo.push_back(tl());
    if (s.undo.size() > 200) s.undo.erase(s.undo.begin());
}

// The timeline changed from `key` on: if that frame already ran, go back before it and come here again (next frame).
void edited(long long key) {
    auto& s = st();
    s.editEarliest = std::min(s.editEarliest, key);
    s.boxesKey = LLONG_MIN;
    // xdBot's frame fixes (positions it recorded, Accuracy: Frame Fixes / Input Fixes) from the edit on describe the old run:
    // played back, they'd put the player back on it. They go.
    auto& fixes = Global::get().macro.frameFixes;
    long long const frame = std::max<long long>(0, key / kUnits);
    std::size_t const before = fixes.size();
    std::erase_if(fixes, [frame](gdr::FrameFix const& f) { return static_cast<long long>(f.frame) >= frame; });
    s.fixesDropped += static_cast<int>(before - fixes.size());
}

void placeClick(PlayLayer* pl, bool down, int button, bool isPlayer1) {
    auto& s = st();
    long long const sub = subOf(s.part, s.parts);
    input e(s.frame, button, flagFor(pl, isPlayer1), down, sub);
    insertClickAt(tl(), e);
    if (down) s.lastPress = keyOf(e);
    Global::get().safeMode = true;
}

std::optional<bool> jumpHeldHere(PlayLayer* pl) {
    return heldAt(tl(), posKey(), 1, flagFor(pl, true));
}

void moveSelected(PlayLayer* pl, long long by) {
    auto& s = st();
    int const i = selectedIndex();
    if (i < 0) {
        s.note = "No click to move: select one on the graph (or step before one)";
        return;
    }
    s.playing = false;
    pushUndo();
    auto const before = tl()[static_cast<std::size_t>(i)];
    long long const step = std::max<long long>(1, kUnits / std::max(1, s.parts));
    auto moved = movedBy(before, by * step);
    tl().erase(tl().begin() + i);
    s.selected = static_cast<int>(insertClickAt(tl(), moved));
    edited(std::min(keyOf(before), keyOf(moved)));
    double const total = movedFrames(moved);
    int const dec = std::max(3, decimalsFor(s.parts));
    s.note = fmt::format("Moved {} to {} ({:+.{}f} frames)", clickText(pl, moved), unitsText(keyOf(moved), s.parts), total, dec);
}

void updateLabel();

void doAction(int id) {
    auto& s = st();
    auto pl = PlayLayer::get();
    if (!pl || !s.on) return;
    switch (id) {
        case kTickBack: stepOnce(-1, true); break;
        case kStepBack: stepOnce(-1, false); break;
        case kStep: stepOnce(1, false); break;
        case kTick: stepOnce(1, true); break;
        case kPlay:
            s.playing = !s.playing;
            s.playAcc = 0;
            s.want = 0;
            s.note.clear();
            if (s.playing) {
                s.playFrom = Pos{s.frame, s.part};
                s.graphOffset = 0;
                s.replayTo.reset();
            }
            break;
        case kReturn:
            s.playing = false;
            s.replayTo.reset();
            if (!s.playFrom) s.note = "Nothing played yet";
            else s.want = (1LL * s.playFrom->frame * s.parts + s.playFrom->part) - (1LL * s.frame * s.parts + s.part);
            break;
        case kAdd:
        case kHold: {
            s.playing = false;
            pushUndo();
            bool const held = jumpHeldHere(pl).value_or(false);
            placeClick(pl, !held, 1, true);
            edited(posKey());
            s.note = fmt::format("Jump {} at {}", held ? "released" : "pressed", posText({s.frame, s.part}, s.parts));
            if (id == kAdd) {
                long long const here = posKey();
                for (auto i = firstAtOrAfter(tl(), here); i < tl().size() && keyOf(tl()[i]) == here; ++i)
                    s.selected = static_cast<int>(i);
            }
            break;
        }
        case kPull: {
            s.playing = false;
            auto i = firstAtOrAfter(tl(), posKey());
            if (i >= tl().size()) s.note = "No click after here to move";
            else {
                pushUndo();
                auto before = tl()[i];
                moveNextHere(tl(), posKey());
                s.note = fmt::format("Moved {} from {} to {}", clickText(pl, before), unitsText(keyOf(before), s.parts),
                    posText({s.frame, s.part}, s.parts));
                if (before.down) s.lastPress = posKey();
                s.selected = static_cast<int>(i);
                edited(posKey());
            }
            break;
        }
        case kDeleteNext: {
            s.playing = false;
            auto i = firstAtOrAfter(tl(), posKey());
            if (i >= tl().size()) s.note = "No click after here to delete";
            else {
                pushUndo();
                auto gone = *eraseNext(tl(), posKey());
                s.note = fmt::format("Deleted {} at {}", clickText(pl, gone), unitsText(keyOf(gone), s.parts));
                s.selected = -1;
                edited(keyOf(gone));
            }
            break;
        }
        case kDeleteSel: {
            int const i = selectedIndex();
            if (i < 0) s.note = "No click to delete";
            else {
                s.playing = false;
                pushUndo();
                auto const gone = tl()[static_cast<std::size_t>(i)];
                tl().erase(tl().begin() + i);
                s.selected = tl().empty() ? -1 : std::min(i, static_cast<int>(tl().size()) - 1);
                edited(keyOf(gone));
                s.note = fmt::format("Deleted {} at {}", clickText(pl, gone), unitsText(keyOf(gone), s.parts));
            }
            break;
        }
        case kUndo: {
            if (s.undo.empty()) s.note = "Nothing to undo";
            else {
                s.playing = false;
                auto backTl = std::move(s.undo.back());
                s.undo.pop_back();
                long long const k = firstDifference(tl(), backTl);
                tl() = std::move(backTl);
                if (s.selected >= static_cast<int>(tl().size())) s.selected = -1;
                if (k != LLONG_MAX) edited(k);
                s.note = fmt::format("Undone ({} more)", s.undo.size());
            }
            break;
        }
        case kSelPrev:
        case kSelNext: {
            int const i = selectedIndex();
            if (tl().empty()) s.note = "No clicks on the timeline yet";
            else if (i < 0) s.selected = static_cast<int>(tl().size()) - 1;
            else s.selected = std::clamp(i + (id == kSelNext ? 1 : -1), 0, static_cast<int>(tl().size()) - 1);
            if (s.selected >= 0) {
                double const at = timeOf(tl()[static_cast<std::size_t>(s.selected)]);
                double const here = s.frame + s.part / static_cast<double>(std::max(1, s.parts));
                if (std::fabs(at - (here + s.graphOffset)) > s.graphSpan * 0.4) s.graphOffset = at - here;
            }
            break;
        }
        case kMoveEarlier: moveSelected(pl, -1); break;
        case kMoveLater: moveSelected(pl, 1); break;
        case kGraphBack: s.graphOffset -= s.graphSpan / 5.0; break;
        case kGraphFwd: s.graphOffset += s.graphSpan / 5.0; break;
        case kGraphIn:
        case kGraphOut: {
            static constexpr int kSpans[] = {5, 10, 20, 50, 100, 200, 500, 1000};
            int k = 0;
            while (k + 1 < static_cast<int>(std::size(kSpans)) && kSpans[k] < s.graphSpan) ++k;
            k = std::clamp(k + (id == kGraphOut ? 1 : -1), 0, static_cast<int>(std::size(kSpans)) - 1);
            s.graphSpan = kSpans[k];
            Mod::get()->setSavedValue<int64_t>("stepper_graph_span", s.graphSpan);
            break;
        }
        case kPath: {
            auto& g = Global::get();
            g.showTrajectory = !g.showTrajectory;
            g.mod->setSavedValue("macro_show_trajectory", g.showTrajectory);
            if (g.layer) {
                if (auto t = static_cast<RecordLayer*>(g.layer)->trajectoryToggle) t->toggle(g.showTrajectory);
            }
            if (g.showTrajectory) ShowTrajectory::updateTrajectory(pl);
            else ShowTrajectory::trajectoryOff();
            s.boxesKey = LLONG_MIN;
            s.note = g.showTrajectory ? "Trajectory on (xdBot's Show Trajectory)" : "Trajectory off";
            break;
        }
        case kMark: {
            long long const here = s.lastPress.value_or(posKey());
            s.marks.add(here);
            s.note = "Mark: " + s.marks.text(s.parts);
            break;
        }
        case kFiner:
        case kCoarser: {
            long long const hz = nextHz(hzOf(s.parts, tps()), id == kFiner ? 1 : -1, tps());
            int const parts = partsFor(hz, tps());
            // (the spot kept as near as the new split allows)
            s.part = static_cast<int>(std::llround(static_cast<double>(s.part) * parts / std::max(1, s.parts)));
            if (s.part >= parts) s.part = parts - 1;
            s.parts = parts;
            Mod::get()->setSettingValue<int64_t>("stepper_hz", hz);
            s.note = fmt::format("Steps of {} frame ({} Hz)", unitsText(kUnits / parts, parts), hzText(hzOf(parts, tps())));
            break;
        }
        case kOff: stepperStop(pl, true); return;
        default: break;
    }
    updateLabel();
}

// ---- drawing: the readout, the ghost, the trajectory's hitboxes, the panel ----

void updateLabel() {
    auto& s = st();
    auto pl = PlayLayer::get();
    if (!s.label || !pl) return;
    s.label->setVisible(s.on);
    if (!s.on) return;
    int const dec = decimalsFor(s.parts);
    std::string text = fmt::format("SUB-TICK STEPPER{}  steps of {} frame ({} Hz)\n", s.playing ? " [PLAYING]" : "",
        s.parts == 1 ? std::string("1") : unitsText(kUnits / s.parts, s.parts), hzText(hzOf(s.parts, tps())));
    std::string bar;
    if (s.parts > 1 && s.parts <= 20) {
        for (int k = 0; k < s.parts; ++k) bar += k < s.part ? '#' : '-';
        bar = " [" + bar + "]";
    }
    text += fmt::format("Frame {}{} (the next frame to run)", posText({s.frame, s.part}, s.parts), bar);
    if (auto held = jumpHeldHere(pl)) text += *held ? "   jump held" : "   jump let go";
    text += "\n";
    auto const i = firstAtOrAfter(tl(), posKey());
    if (i < tl().size()) {
        auto const& e = tl()[i];
        long long const d = keyOf(e) - posKey();
        long long const frames = d / kUnits;
        std::string when = d == 0 ? "here" : frames == 0 ? "in this frame" : fmt::format("in {} frame{}", frames, frames == 1 ? "" : "s");
        text += fmt::format("Next: {} at {} ({})   {} clicks to come\n", clickText(pl, e), unitsText(keyOf(e), s.parts), when,
            tl().size() - i);
    }
    else text += fmt::format("No clicks after here ({} in the macro)\n", tl().size());
    if (s.stoppedByDeath) text += fmt::format("Frame {} kills the player: stopped in front of it\n", s.frame);
    else if (s.diedAt >= 0) text += fmt::format("Died in frame {} (went through it)\n", s.diedAt);
    if (s.marks.lo) text += "Marks: " + s.marks.text(s.parts) + "\n";
    if (s.fixesDropped > 0)
        text += fmt::format("{} of xdBot's frame fixes after your edits taken off (they'd put the player back on the old run)\n", s.fixesDropped);
    if (!s.note.empty()) text += s.note + "\n";
    text += fmt::format("Can go back {} frame{}\n", s.back.size(), s.back.size() == 1 ? "" : "s");
    text += fmt::format("{} / {} frame, {} / {} small step, {} jump, {} play, {} next here, {} delete next, {} / {} finer / coarser",
        keyName("key_stepper_tick"), keyName("key_stepper_tick_back"), keyName("key_stepper_fine"), keyName("key_stepper_fine_back"),
        keyName("key_stepper_hold"), keyName("key_stepper_play"), keyName("key_stepper_pull"), keyName("key_stepper_delete"),
        keyName("key_stepper_finer"), keyName("key_stepper_coarser"));
    (void)dec;
    s.label->setString(text.c_str());
    auto win = CCDirector::get()->getWinSize();
    bool const panelShown = s.panel && s.panel->isVisible();
    s.label->setPosition({6.f, panelShown ? s.panelRect.getMaxY() + 4.f : 6.f});
    s.label->limitLabelWidth(win.width * 0.75f, 0.5f, 0.2f);
}

// where the player will be at the stepper's spot inside the coming frame (along the last frame's movement)
void drawGhost(PlayLayer* pl) {
    auto& s = st();
    if (!s.ghost) return;
    s.ghost->clear();
    if (!s.on || s.part == 0 || !setting("stepper_ghost")) return;
    double const frac = static_cast<double>(s.part) / std::max(1, s.parts);
    PlayerObject* players[2] = {pl->m_player1, pl->m_gameState.m_isDualMode ? pl->m_player2 : nullptr};
    for (auto* p : players) {
        if (!p) continue;
        auto pos = p->getPosition();
        auto last = p->m_lastPosition;
        cocos2d::CCPoint at{pos.x + static_cast<float>((pos.x - last.x) * frac), pos.y + static_cast<float>((pos.y - last.y) * frac)};
        auto r = p->getObjectRect();
        float const hw = r.size.width / 2.f, hh = r.size.height / 2.f;
        cocos2d::CCPoint q[4] = {{at.x - hw, at.y - hh}, {at.x + hw, at.y - hh}, {at.x + hw, at.y + hh}, {at.x - hw, at.y + hh}};
        s.ghost->drawPolygon(q, 4, {0.f, 0.9f, 1.f, 0.12f}, 0.6f, {0.f, 0.9f, 1.f, 0.9f});
    }
}

// the player's hitbox along xdBot's trajectory (the hold path)
void drawBoxes(PlayLayer* pl) {
    auto& s = st();
    if (!s.boxes) return;
    auto& g = Global::get();
    bool const want = s.on && g.showTrajectory && setting("stepper_trajectory_boxes");
    if (!want) {
        if (s.boxesKey != LLONG_MIN) s.boxes->clear();
        s.boxesKey = LLONG_MIN;
        return;
    }
    long long const key = (static_cast<long long>(s.frame) * 1000003LL) ^ static_cast<long long>(tl().size() * 131) ^ (s.back.size() << 40);
    if (key == s.boxesKey) return;
    s.boxesKey = key;
    s.boxes->clear();
    auto& t = ShowTrajectory::get();
    auto r = pl->m_player1->getObjectRect();
    float const hw = r.size.width / 2.f, hh = r.size.height / 2.f;
    int const n = std::clamp(t.length, 0, 480);
    for (int i = 1; i < n; ++i) {
        auto at = t.player1Trajectory[i];
        if (at.x == 0.f && at.y == 0.f) continue;
        cocos2d::CCPoint q[4] = {{at.x - hw, at.y - hh}, {at.x + hw, at.y - hh}, {at.x + hw, at.y + hh}, {at.x - hw, at.y + hh}};
        s.boxes->drawPolygon(q, 4, {1.f, 0.2f, 0.2f, 0.05f}, 0.3f, {0.3f, 1.f, 0.4f, 0.35f});
    }
}

class StepperPanel;

struct GraphView {
    double left = 0, right = 1;
    float x0 = 0, x1 = 1;
    double px = 1;
    float xOf(double t) const { return x0 + static_cast<float>((t - left) * px); }
    double frameAt(float x) const { return left + (x - x0) / px; }
};
GraphView graphView() {
    auto& s = st();
    GraphView v;
    double const here = s.frame + s.part / static_cast<double>(std::max(1, s.parts));
    double const centre = here + s.graphOffset;
    v.left = centre - s.graphSpan / 2.0;
    v.right = centre + s.graphSpan / 2.0;
    v.x0 = s.graphRect.getMinX() + 6.f;
    v.x1 = s.graphRect.getMaxX() - 6.f;
    v.px = (v.x1 - v.x0) / std::max(1e-9, v.right - v.left);
    return v;
}
// the graph's lane: P1 jump on the axis, the others under it
int laneOf(PlayLayer* pl, input const& e) {
    int const b = std::clamp(e.button, 1, 3);
    bool const p1 = !twoPlayer(pl) || isPlayer1Of(e.player2);
    return (b - 1) + (p1 ? 0 : 3);
}

void graphTap(cocos2d::CCPoint p) {
    auto& s = st();
    auto pl = PlayLayer::get();
    auto const v = graphView();
    float const axis = s.graphRect.getMinY() + s.graphRect.size.height * 0.62f;
    int best = -1;
    float bestD = 9.f;
    long long const leftKey = static_cast<long long>(std::floor(std::max(0.0, v.left))) * kUnits;
    for (auto i = firstAtOrAfter(tl(), leftKey); i < tl().size(); ++i) {
        auto const& e = tl()[i];
        double const t = timeOf(e);
        if (t > v.right) break;
        float const x = v.xOf(t), y = axis - 9.f * static_cast<float>(laneOf(pl, e));
        float const d = std::max(std::fabs(x - p.x), std::fabs(y - p.y) * 0.5f);
        if (d < bestD) {
            bestD = d;
            best = static_cast<int>(i);
        }
    }
    if (best >= 0) {
        s.selected = best;
        s.note.clear();
        updateLabel();
        return;
    }
    double const t = std::max(0.0, v.frameAt(p.x));
    long long const target = std::llround(t * s.parts);
    long long const now = 1LL * s.frame * s.parts + s.part;
    if (target == now) return;
    s.playing = false;
    s.replayTo.reset();
    s.want = std::clamp<long long>(target - now, -240LL * s.parts, 240LL * s.parts);
    s.graphOffset -= static_cast<double>(s.want) / s.parts;
}

class StepperPanel : public CCMenu {
public:
    static StepperPanel* create() {
        auto ret = new StepperPanel();
        if (ret->init()) {
            ret->autorelease();
            return ret;
        }
        delete ret;
        return nullptr;
    }
    // buttons that repeat while held (steps, move selected) act from the frame's polling; the others when let go
    void onButton(CCObject* sender) {
        auto& s = st();
        s.toolTouched = true;
        int const id = sender ? sender->getTag() : -1;
        if ((id >= kTickBack && id <= kTick) || id == kMoveEarlier || id == kMoveLater) return;
        doAction(id);
    }
    bool ccTouchBegan(CCTouch* touch, CCEvent* event) override {
        m_own = false;
        if (!this->isVisible()) return false;
        if (CCMenu::ccTouchBegan(touch, event)) return true;
        auto& s = st();
        auto p = touch->getLocation();
        if (!s.panelRect.containsPoint(p)) return false;
        m_own = true;
        m_onGraph = s.graphRect.containsPoint(p);
        m_start = p;
        m_dragged = false;
        s.toolTouched = true;
        return true;
    }
    void ccTouchMoved(CCTouch* touch, CCEvent* event) override {
        if (!m_own) return CCMenu::ccTouchMoved(touch, event);
        auto& s = st();
        auto p = touch->getLocation();
        if (m_onGraph && (m_dragged || std::fabs(p.x - m_start.x) > 5.f)) {
            if (!m_dragged) m_last = m_start;
            m_dragged = true;
            auto const v = graphView();
            s.graphOffset -= (p.x - m_last.x) / v.px;
            m_last = p;
        }
    }
    void ccTouchEnded(CCTouch* touch, CCEvent* event) override {
        if (!m_own) return CCMenu::ccTouchEnded(touch, event);
        m_own = false;
        st().toolTouched = true;
        if (m_onGraph && !m_dragged) graphTap(touch->getLocation());
    }
    void ccTouchCancelled(CCTouch* touch, CCEvent* event) override {
        if (!m_own) return CCMenu::ccTouchCancelled(touch, event);
        m_own = false;
    }

private:
    bool m_own = false, m_onGraph = false, m_dragged = false;
    cocos2d::CCPoint m_start, m_last;
};

void buildNodes(PlayLayer* pl) {
    auto& s = st();
    if (s.builtFor == pl && s.label && s.label->getParent() == pl) return;
    s.builtFor = pl;
    s.buttons.clear();
    s.nums.clear();
    auto win = CCDirector::get()->getWinSize();

    auto label = CCLabelBMFont::create("", "chatFont.fnt");
    label->setAnchorPoint({0.f, 0.f});
    label->setScale(0.5f);
    label->setVisible(false);
    label->setID("stepper-readout"_spr);
    pl->addChild(label, 1001);
    s.label = label;

    if (pl->m_objectLayer) {
        auto ghost = CCDrawNode::create();
        ghost->setID("stepper-ghost"_spr);
        pl->m_objectLayer->addChild(ghost, 9999);
        s.ghost = ghost;
        auto boxes = CCDrawNode::create();
        boxes->setID("stepper-hitboxes"_spr);
        pl->m_objectLayer->addChild(boxes, 9998);
        s.boxes = boxes;
    }

    float const h = 112.f, pad = 4.f;
    s.panelRect = cocos2d::CCRect(pad, 2.f, win.width - 2 * pad, h);
    s.graphRect = cocos2d::CCRect(pad + 6.f, 8.f, win.width - 2 * pad - 12.f, 52.f);
    auto panel = StepperPanel::create();
    panel->setID("stepper-panel"_spr);
    panel->setPosition({0.f, 0.f});
    panel->setVisible(false);
    auto bg = CCScale9Sprite::create("square02b_001.png", {0, 0, 80, 80});
    bg->setContentSize(s.panelRect.size);
    bg->setAnchorPoint({0.f, 0.f});
    bg->setPosition(s.panelRect.origin);
    bg->setColor({0, 0, 0});
    bg->setOpacity(170);
    panel->addChild(bg, -2);
    auto gbg = CCScale9Sprite::create("square02b_001.png", {0, 0, 80, 80});
    gbg->setContentSize(s.graphRect.size);
    gbg->setAnchorPoint({0.f, 0.f});
    gbg->setPosition(s.graphRect.origin);
    gbg->setColor({10, 14, 30});
    gbg->setOpacity(200);
    panel->addChild(gbg, -1);
    auto graph = CCDrawNode::create();
    panel->addChild(graph, 1);
    s.graph = graph;
    auto status = CCLabelBMFont::create("", "chatFont.fnt");
    status->setAnchorPoint({0.f, 0.5f});
    status->setScale(0.6f);
    status->setPosition({s.panelRect.getMinX() + 8.f, s.panelRect.getMaxY() - 10.f});
    panel->addChild(status, 2);
    s.status = status;
    for (int i = 0; i < 160; ++i) {
        auto n = CCLabelBMFont::create("", "chatFont.fnt");
        n->setScale(0.38f);
        n->setVisible(false);
        panel->addChild(n, 3);
        s.nums.push_back(n);
    }
    struct B { char const* text; int id; char const* bg; };
    struct G { char const* caption; std::vector<B> buttons; };
    std::vector<G> const groups = {
        {"Step", {{"<<", kTickBack, "GJ_button_04.png"}, {"<", kStepBack, "GJ_button_04.png"},
                  {">", kStep, "GJ_button_04.png"}, {">>", kTick, "GJ_button_04.png"}}},
        {"Play", {{"Play", kPlay, "GJ_button_01.png"}, {"Back", kReturn, "GJ_button_01.png"}}},
        {"Clicks", {{"+", kAdd, "GJ_button_02.png"}, {"Here", kPull, "GJ_button_02.png"},
                    {"X", kDeleteSel, "GJ_button_06.png"}, {"Undo", kUndo, "GJ_button_03.png"}}},
        {"Select", {{"<", kSelPrev, "GJ_button_05.png"}, {">", kSelNext, "GJ_button_05.png"}}},
        {"Move selected", {{"<", kMoveEarlier, "GJ_button_02.png"}, {">", kMoveLater, "GJ_button_02.png"}}},
        {"Move graph", {{"<", kGraphBack, "GJ_button_05.png"}, {">", kGraphFwd, "GJ_button_05.png"}}},
        {"Graph", {{"-", kGraphOut, "GJ_button_04.png"}, {"+", kGraphIn, "GJ_button_04.png"}}},
        {"Hz", {{"-", kCoarser, "GJ_button_04.png"}, {"+", kFiner, "GJ_button_04.png"}}},
        {"Level", {{"Path", kPath, "GJ_button_03.png"}, {"Mark", kMark, "GJ_button_03.png"}}},
        {"", {{"Off", kOff, "GJ_button_06.png"}}},
    };
    float const scale = 0.4f;
    auto widthOf = [](B const& b) { return std::strlen(b.text) <= 2 ? 34.f : 64.f; };
    float const gapIn = 2.f, gapGroup = 9.f;
    float total = 0;
    for (auto const& g : groups) {
        for (auto const& b : g.buttons) total += widthOf(b) * scale + gapIn;
        total += gapGroup - gapIn;
    }
    total -= gapGroup;
    float x = win.width / 2.f - total / 2.f;
    float const y = s.panelRect.getMaxY() - 33.f;
    for (auto const& g : groups) {
        float const gx0 = x;
        for (auto const& b : g.buttons) {
            float const w = widthOf(b);
            auto spr = ButtonSprite::create(b.text, static_cast<int>(w), true, "bigFont.fnt", b.bg, 30.f, 0.6f);
            spr->setScale(scale);
            auto item = CCMenuItemSpriteExtra::create(spr, panel, menu_selector(StepperPanel::onButton));
            item->setTag(b.id);
            item->setPosition({x + w * scale / 2.f, y});
            panel->addChild(item);
            s.buttons.push_back({item, b.id});
            x += w * scale + gapIn;
        }
        if (*g.caption) {
            auto cap = CCLabelBMFont::create(g.caption, "chatFont.fnt");
            cap->setScale(0.38f);
            cap->setOpacity(170);
            cap->setPosition({(gx0 + x - gapIn) / 2.f, y - 13.f});
            panel->addChild(cap, 2);
        }
        x += gapGroup - gapIn;
    }
    pl->addChild(panel, 1002);
    s.panel = panel;
}

void drawPanel(PlayLayer* pl) {
    auto& s = st();
    if (!s.panel) return;
    bool const show = s.on && setting("stepper_panel");
    s.panel->setVisible(show);
    if (!show || !s.graph || !s.status) return;
    auto const& T = tl();
    auto const nums = inputNumbers(T);
    int const sel = selectedIndex();
    int const dec = std::max(1, decimalsFor(s.parts));
    std::string text = fmt::format("{} at frame {}", s.playing ? "Playing" : "Stepping", posText({s.frame, s.part}, s.parts));
    if (sel >= 0) {
        auto const& e = T[static_cast<std::size_t>(sel)];
        int const n = nums[static_cast<std::size_t>(sel)];
        std::string who = n > 0 ? fmt::format("input {}{}", n, e.down ? "" : " release") : clickText(pl, e);
        if (e.origin != kNoOrigin && keyOf(e) != e.origin)
            text += fmt::format("  -  {} moved {:+.{}f} frames", who, movedFrames(e), std::max(3, dec));
        else text += fmt::format("  -  {} at {}{}", who, unitsText(keyOf(e), s.parts), s.selected < 0 ? " (next)" : "");
    }
    else text += "  -  no clicks ahead";
    if (int const moved = movedCount(T); moved > 0) text += fmt::format("  -  {} moved in total", moved);
    if (s.stoppedByDeath) text += fmt::format("  -  frame {} kills", s.frame);
    else if (s.diedAt >= 0) text += fmt::format("  -  died at {}", s.diedAt);
    if (s.replayTo) text += "  -  replaying...";
    s.status->setString(text.c_str());
    s.status->limitLabelWidth(s.panelRect.size.width - 16.f, 0.6f, 0.3f);

    auto* g = s.graph.data();
    g->clear();
    auto const v = graphView();
    auto const& R = s.graphRect;
    float const top = R.getMaxY() - 3.f, bottom = R.getMinY() + 3.f;
    float const axis = R.getMinY() + R.size.height * 0.62f;
    std::size_t used = 0;
    auto label = [&](std::string const& t, cocos2d::CCPoint at, ccColor3B c, GLubyte o = 230) {
        if (used >= s.nums.size()) return;
        auto* l = s.nums[used++].data();
        l->setString(t.c_str());
        l->setPosition(at);
        l->setColor(c);
        l->setOpacity(o);
        l->setVisible(true);
    };
    int const major = s.graphSpan <= 100 ? 10 : s.graphSpan <= 500 ? 50 : 100;
    bool const every = v.px >= 3.0;
    for (long long t = static_cast<long long>(std::ceil(v.left)); t <= static_cast<long long>(std::floor(v.right)); ++t) {
        if (t < 0) continue;
        float const x = v.xOf(static_cast<double>(t));
        if (t % major == 0) {
            g->drawSegment({x, bottom + 6.f}, {x, top}, 0.5f, {1.f, 0.82f, 0.25f, 0.8f});
            label(std::to_string(t), {x, bottom + 1.f}, {255, 210, 70});
        }
        else if (every) g->drawSegment({x, axis - 4.f}, {x, axis + 4.f}, 0.35f, {1.f, 1.f, 1.f, 0.45f});
    }
    if (s.parts > 1 && v.px / s.parts >= 4.0) {
        for (int k = 1; k < s.parts; ++k) {
            float const x = v.xOf(s.frame + k / static_cast<double>(s.parts));
            g->drawSegment({x, axis - 2.f}, {x, axis + 2.f}, 0.25f, {0.6f, 0.9f, 1.f, 0.4f});
        }
    }
    g->drawSegment({v.x0, axis}, {v.x1, axis}, 0.35f, {1.f, 1.f, 1.f, 0.35f});
    int const deathFrame = s.stoppedByDeath ? s.frame : s.diedAt;
    if (deathFrame >= 0 && deathFrame + 1 >= v.left && deathFrame <= v.right) {
        float const a = std::max(v.x0, v.xOf(deathFrame)), b = std::min(v.x1, v.xOf(deathFrame + 1.0));
        cocos2d::CCPoint q[4] = {{a, bottom}, {std::max(a + 1.f, b), bottom}, {std::max(a + 1.f, b), top}, {a, top}};
        g->drawPolygon(q, 4, {1.f, 0.1f, 0.1f, 0.3f}, 0.f, {0, 0, 0, 0});
    }
    long long const leftKey = static_cast<long long>(std::floor(std::max(0.0, v.left))) * kUnits;
    std::size_t const from = firstAtOrAfter(T, leftKey);
    bool laneUsed[6] = {};
    for (auto const& e : T) laneUsed[laneOf(pl, e)] = true;
    for (int lane = 0; lane < 6; ++lane) {
        if (!laneUsed[lane]) continue;
        int const button = lane % 3 + 1;
        bool const p1 = lane < 3;
        bool const flag = flagFor(pl, p1);
        float const y = axis - 9.f * static_cast<float>(lane);
        auto held = heldAt(T, static_cast<long long>(std::max(0.0, v.left) * kUnits), button, flag);
        double t0 = v.left;
        auto seg = [&](double a, double b, std::optional<bool> h) {
            if (!h || b <= a) return;
            float const xa = std::max(v.x0, v.xOf(a)), xb = std::min(v.x1, v.xOf(b));
            if (xb <= xa) return;
            g->drawSegment({xa, y}, {xb, y}, *h ? 1.3f : 0.8f, *h ? ccColor4F{0.3f, 1.f, 0.4f, 0.75f} : ccColor4F{1.f, 0.3f, 0.3f, 0.6f});
        };
        for (auto i = from; i < T.size(); ++i) {
            auto const& e = T[i];
            if (e.button != button || laneOf(pl, e) != lane) continue;
            double const t = timeOf(e);
            if (t > v.right) break;
            seg(t0, t, held);
            t0 = t;
            held = e.down;
        }
        seg(t0, v.right, held);
        if (lane > 0) label(fmt::format("{}{}", button == 1 ? "J" : button == 2 ? "L" : "R", p1 ? "" : "2"), {R.getMinX() + 4.f, y},
            {170, 170, 170}, 160);
    }
    float lastNumX = -1e9f;
    for (auto i = from; i < T.size(); ++i) {
        auto const& e = T[i];
        double const t = timeOf(e);
        if (t > v.right) break;
        float const x = v.xOf(t), y = axis - 9.f * static_cast<float>(laneOf(pl, e));
        if (e.origin != kNoOrigin && e.origin != keyOf(e)) {
            double const o = e.origin / static_cast<double>(kUnits);
            if (o >= v.left && o <= v.right) {
                float const xo = v.xOf(o);
                cocos2d::CCPoint q[4] = {{xo - 2.5f, y - 2.5f}, {xo + 2.5f, y - 2.5f}, {xo + 2.5f, y + 2.5f}, {xo - 2.5f, y + 2.5f}};
                g->drawPolygon(q, 4, {0.6f, 0.6f, 0.6f, 0.35f}, 0.4f, {0.8f, 0.8f, 0.8f, 0.6f});
            }
            float const xa = std::clamp(v.xOf(std::clamp(o, v.left, v.right)), v.x0, v.x1);
            g->drawSegment({xa, y + 5.f}, {x, y + 5.f}, 0.3f, {0.8f, 0.8f, 0.8f, 0.5f});
        }
        bool const isSel = static_cast<int>(i) == sel;
        float const r = isSel ? 4.5f : e.down ? 3.f : 2.5f;
        ccColor4F const fill = e.down ? ccColor4F{0.3f, 1.f, 0.4f, 1.f} : ccColor4F{1.f, 0.3f, 0.3f, 1.f};
        cocos2d::CCPoint q[4] = {{x - r, y - r}, {x + r, y - r}, {x + r, y + r}, {x - r, y + r}};
        if (isSel) g->drawSegment({x, bottom}, {x, top}, 0.4f, {1.f, 1.f, 1.f, 0.5f});
        g->drawPolygon(q, 4, fill, isSel ? 0.9f : 0.4f, isSel ? ccColor4F{1.f, 1.f, 1.f, 1.f} : ccColor4F{0.f, 0.f, 0.f, 0.8f});
        int const n = nums[i];
        if (e.down && n > 0 && (x - lastNumX > 16.f || isSel)) {
            label(std::to_string(n), {x, y + 10.f}, isSel ? ccColor3B{255, 255, 255} : ccColor3B{200, 255, 200});
            lastNumX = x;
        }
    }
    float const xp = v.xOf(s.frame + s.part / static_cast<double>(std::max(1, s.parts)));
    if (xp >= v.x0 - 1.f && xp <= v.x1 + 1.f) {
        g->drawSegment({xp, bottom}, {xp, top + 2.f}, 0.7f, {0.f, 0.9f, 1.f, 1.f});
        cocos2d::CCPoint tri[3] = {{xp - 4.f, top + 5.f}, {xp + 4.f, top + 5.f}, {xp, top}};
        g->drawPolygon(tri, 3, {0.f, 0.9f, 1.f, 1.f}, 0.f, {0, 0, 0, 0});
    }
    else {
        float const xe = xp < v.x0 ? v.x0 + 2.f : v.x1 - 2.f, d = xp < v.x0 ? -1.f : 1.f;
        cocos2d::CCPoint tri[3] = {{xe, axis - 5.f}, {xe, axis + 5.f}, {xe + 6.f * d, axis}};
        g->drawPolygon(tri, 3, {0.f, 0.9f, 1.f, 0.9f}, 0.f, {0, 0, 0, 0});
    }
    for (; used < s.nums.size(); ++used) s.nums[used]->setVisible(false);
}

} // namespace

// ---- on / off ----

bool stepperOn() { return st().on; }
bool stepperStepping() { return st().stepping; }

void stepperStart(PlayLayer* pl) {
    auto& s = st();
    auto& g = Global::get();
    if (!pl || s.on) return;
    if (pl->m_levelEndAnimationStarted) return;
    if (auto why = cbfConflictText(); !why.empty()) {
        Notification::create(why + ": turn it off for the stepper (it puts clicks between frames itself)", NotificationIcon::Error, 3.f)->show();
        return;
    }
    if (g.renderer.recording || g.renderer.recordingAudio) {
        Notification::create("The stepper can't run while rendering", NotificationIcon::Error)->show();
        return;
    }
    if (g.frameStepper) Global::frameStepperOff(); // (this replaces it while it's on)
    releaseBack();
    buildNodes(pl);
    s.on = true;
    s.usedThisAttempt = true;
    s.want = 0;
    s.playing = false;
    s.playFrom.reset();
    s.diedAt = -1;
    s.diedNow = s.stoppedByDeath = false;
    s.note.clear();
    s.marks.clear();
    s.lastPress.reset();
    s.holdKey.clear();
    s.toolHeld = -1;
    s.selected = -1;
    s.graphOffset = 0;
    s.graphSpan = static_cast<int>(std::clamp<int64_t>(Mod::get()->getSavedValue<int64_t>("stepper_graph_span", 50), 5, 1000));
    s.undo.clear();
    s.editEarliest = LLONG_MAX;
    s.replayTo.reset();
    s.boxesKey = LLONG_MIN;
    std::memset(s.physDown, 0, sizeof s.physDown);
    s.parts = partsFor(settingInt("stepper_hz"), tps());
    s.part = 0;
    int const now = Global::getCurrentFrame();
    s.frame = now + 1;
    // (from the level's start nothing's been pressed yet; otherwise the frames up to now have been)
    s.pressedUpTo = pl->m_gameState.m_levelTime <= 0.0 ? -1 : now;
    s.prevState = g.state;
    g.state = state::none;
    g.safeMode = true;
    s.fixesDropped = 0;
    // the macro as a timeline: in time order
    if (!isSorted(tl())) {
        sortTimeline(tl());
        s.note = "The macro's clicks were put in time order";
    }
    if (!g.macro.xdBotMacro && !tl().empty())
        s.note = "This macro is from another bot (its frames are counted its own way): the stepper counts them xdBot's way";
    unsigned ms = FMODAudioEngine::sharedEngine()->getMusicTimeMS(0);
    s.songBaseMs = ms > 0 ? ms - pl->m_gameState.m_levelTime * 1000.0 : -1.0;
    pauseSong(true);
    clearArmed();
    pl->m_queuedButtons.clear();
    Interface::updateLabels();
    Interface::updateButtons();
    updateLabel();
    log::info("stepper on at frame {} ({} Hz, {} clicks in the macro)", s.frame, hzOf(s.parts, tps()), tl().size());
}

void stepperStop(PlayLayer* pl, bool playOn) {
    auto& s = st();
    auto& g = Global::get();
    if (!s.on) return;
    releaseBack();
    s.on = false;
    s.want = 0;
    s.playing = false;
    s.holdKey.clear();
    s.toolHeld = -1;
    s.undo.clear();
    s.replayTo.reset();
    s.editEarliest = LLONG_MAX;
    clearArmed();
    if (s.ghost) s.ghost->clear();
    if (s.boxes) s.boxes->clear();
    s.boxesKey = LLONG_MIN;
    if (s.panel) s.panel->setVisible(false);
    if (s.label) s.label->setVisible(false);
    for (auto& e : tl()) e.origin = kNoOrigin;
    if (pl) {
        if (s.songBaseMs >= 0) {
            double ms = s.songBaseMs + pl->m_gameState.m_levelTime * 1000.0;
            if (ms >= 0) FMODAudioEngine::sharedEngine()->setMusicTimeMS(static_cast<unsigned>(ms), true, 0);
        }
        if (playOn) pauseSong(false);
    }
    // xdBot goes on as it was: playing from here, or recording from here (the clicks after here are taken off then: you're
    // recording them now)
    state next = g.state != state::none ? g.state : s.prevState;
    s.prevState = state::none;
    if (next == state::playing && pl) {
        g.currentAction = firstAtOrAfter(tl(), static_cast<long long>(s.pressedUpTo + 1) * kUnits);
        g.currentFrameFix = 0;
        while (g.currentFrameFix < g.macro.frameFixes.size() && g.macro.frameFixes[g.currentFrameFix].frame <= s.pressedUpTo)
            ++g.currentFrameFix;
        g.previousFrame = Global::getCurrentFrame();
        g.state = state::playing;
    }
    else if (next == state::recording && pl) {
        auto cut = firstAtOrAfter(tl(), static_cast<long long>(s.pressedUpTo + 1) * kUnits);
        std::size_t const dropped = tl().size() - cut;
        tl().resize(cut);
        while (!g.macro.frameFixes.empty() && static_cast<int>(g.macro.frameFixes.back().frame) > s.pressedUpTo)
            g.macro.frameFixes.pop_back();
        g.previousFrame = Global::getCurrentFrame();
        g.state = state::recording;
        if (dropped > 0)
            Notification::create(fmt::format("Recording from here: {} click{} after here taken off", dropped, dropped == 1 ? "" : "s"),
                NotificationIcon::Info, 2.f)->show();
    }
    else g.state = next;
    Interface::updateLabels();
    Interface::updateButtons();
    log::info("stepper off at frame {}", s.frame);
}

void stepperToggle() {
    auto pl = PlayLayer::get();
    if (!pl) return;
    if (st().on) stepperStop(pl, !pl->m_isPaused);
    else stepperStart(pl);
}

// ---- hooks' work ----

void stepperAfterCommands(GJBaseGameLayer* gl) {
    auto& s = st();
    if (!s.on || !s.stepping) return;
    int const f = Global::getCurrentFrame();
    ++s.commands;
    s.seenFrame = f;
    if (f <= s.pressedUpTo) return;
    for (auto i : dueIn(tl(), s.pressedUpTo, f)) {
        auto const e = tl()[i];
        if (e.sub > 0) arm(e);
        else pressNow(gl, e);
    }
    s.pressedUpTo = f;
}

bool stepperTakeClick(bool down, int button, bool isPlayer1) {
    auto& s = st();
    if (!s.on) return false;
    if (s.loading || s.stepping) return true; // (GD's own presses / lets go from a load or its queue: the timeline decides)
    auto pl = PlayLayer::get();
    if (!pl) return false;
    placeClick(pl, down, button, isPlayer1);
    edited(posKey());
    return true;
}

bool stepperTakeDeath(PlayLayer* pl, PlayerObject* player) {
    auto& s = st();
    if (!s.on || !pl) return false;
    if (player != pl->m_player1 && player != pl->m_player2) return false;
    s.diedNow = true;
    Global::get().safeMode = true;
    return true;
}

void stepperFrame(GJBaseGameLayer* gl, float dt) {
    auto& s = st();
    auto pl = PlayLayer::get();
    if (!pl || gl != pl) return;
    pauseSong(true); // (GD starts it again after its pause menu)
    if (pl->m_levelEndAnimationStarted) {
        stepperStop(pl, false);
        return;
    }
    // the panel's buttons: a step / move button acts when pressed and keeps going while held
    bool toolDown = false;
    if (s.panel && s.panel->isVisible()) {
        for (auto const& [item, id] : s.buttons) {
            if (!item || !item->isSelected()) continue;
            toolDown = true;
            bool const isMove = id == kMoveEarlier || id == kMoveLater;
            bool const isStep = id >= kTickBack && id <= kTick;
            if (!isMove && !isStep) continue;
            if (isStep && s.toolHeld == -2) continue; // (stopped before a death: let go first)
            int const dir = id == kMoveEarlier || id == kTickBack || id == kStepBack ? -1 : 1;
            bool const whole = id == kTickBack || id == kTick;
            if (s.toolHeld != id) {
                s.toolHeld = id;
                s.toolHoldT = 0;
                s.toolHoldAcc = 0;
                if (isMove) moveSelected(pl, dir);
                else stepOnce(dir, whole);
            }
            else {
                s.toolHoldT += dt;
                if (s.toolHoldT > 0.3) {
                    double const rate = isMove ? 30.0 : whole ? 60.0 : std::max(30.0, static_cast<double>(s.parts));
                    s.toolHoldAcc += static_cast<double>(dt) * rate;
                    int const n = std::min(isMove || whole ? 16 : std::max(16, s.parts / 10), static_cast<int>(s.toolHoldAcc));
                    s.toolHoldAcc -= n;
                    if (n > 0) {
                        if (isMove) moveSelected(pl, 1LL * n * dir);
                        else s.want += 1LL * n * dir * (whole ? s.parts : 1);
                    }
                }
            }
        }
    }
    if (!toolDown) s.toolHeld = -1;
    // clicks while stopped: GD queued them for its next step (a touch on the panel isn't one)
    if (!pl->m_queuedButtons.empty()) {
        auto cmds = pl->m_queuedButtons;
        pl->m_queuedButtons.clear();
        bool const fromButtons = (toolDown && !s.toolDownPrev) || s.toolTouched;
        for (auto const& c : cmds) {
            if (!pl->buttonIsRelevant(c)) continue;
            int const button = static_cast<int>(c.m_button);
            if (fromButtons && button == 1) continue;
            if (button < 1 || button > 3) continue;
            bool& down = s.physDown[button][c.m_isPlayer2 ? 1 : 0];
            if (!c.m_isPush && !down) continue; // (GD's own release when it resumes or resets)
            down = c.m_isPush;
            placeClick(pl, c.m_isPush, button, !c.m_isPlayer2);
            edited(posKey());
        }
    }
    s.toolTouched = false;
    s.toolDownPrev = toolDown;
    if (s.playing) {
        s.want = 0;
        s.playAcc = std::min(s.playAcc + static_cast<double>(dt) * tps(), 16.0);
        while (s.playAcc >= 1.0 && s.playing) {
            s.playAcc -= 1.0;
            if (!stepFrame(pl)) {
                s.playing = false;
                if (!s.stoppedByDeath) s.note = "The frame didn't run (the level ended?)";
                break;
            }
            s.part = 0;
            if (s.diedNow) {
                s.playing = false;
                s.note = fmt::format("Stopped: died in frame {}", s.frame - 1);
            }
        }
    }
    // the step keys polled: a key let go ends the hold even if its release never came, and a held key starts one
    {
        static constexpr char const* kKeys[4] = {"key_stepper_tick", "key_stepper_tick_back", "key_stepper_fine", "key_stepper_fine_back"};
        static constexpr int kDir[4] = {1, -1, 1, -1};
        static constexpr bool kWhole[4] = {true, true, false, false};
        for (int k = 0; k < 4; ++k) {
            bool const held = keybindHeldNow(kKeys[k]);
            s.polledHeld[k] = held ? s.polledHeld[k] + dt : 0.0;
            if (held && !s.keyDownPrev[k] && !s.keyStepped[k] && !s.playing) {
                stepOnce(kDir[k], kWhole[k]);
                s.keyStepped[k] = true;
            }
            if (!held) s.keyStepped[k] = false;
            s.keyDownPrev[k] = held;
#ifdef GEODE_IS_WINDOWS
            if (!held && s.holdKey == kKeys[k]) {
                s.holdKey.clear();
                s.holdDir = 0;
            }
#endif
            if (held && s.holdKey.empty() && !s.holdBlocked && s.polledHeld[k] > 0.3 && !s.playing) {
                s.holdKey = kKeys[k];
                s.holdDir = kDir[k];
                s.holdWhole = kWhole[k];
                s.holdT = s.polledHeld[k];
                s.holdAcc = 0;
            }
        }
        if (s.polledHeld[0] == 0 && s.polledHeld[1] == 0 && s.polledHeld[2] == 0 && s.polledHeld[3] == 0) s.holdBlocked = false;
    }
    if (!s.holdKey.empty() && !s.playing) {
        s.holdT += dt;
        if (s.holdT > 0.3) {
            s.holdAcc += static_cast<double>(dt) * (s.holdWhole ? 60.0 : std::max(30.0, static_cast<double>(s.parts)));
            int const n = std::min(s.holdWhole ? 16 : std::max(16, s.parts / 10), static_cast<int>(s.holdAcc));
            s.holdAcc -= n;
            s.want += 1LL * n * s.holdDir * (s.holdWhole ? s.parts : 1);
        }
    }
    // a click edited where its frame already ran: back to before it (now), then here again (next frame)
    if (s.editEarliest != LLONG_MAX) {
        long long const k = s.editEarliest;
        s.editEarliest = LLONG_MAX;
        int const editFrame = static_cast<int>(std::max<long long>(0, k / kUnits));
        if (editFrame <= s.pressedUpTo) {
            if (!s.replayTo) s.replayTo = Pos{s.frame, s.part};
            s.playing = false;
            s.want = (1LL * editFrame * s.parts) - (1LL * s.frame * s.parts + s.part);
        }
    }
    else if (s.replayTo && s.want == 0) {
        long long const d = (1LL * s.replayTo->frame * s.parts + s.replayTo->part) - (1LL * s.frame * s.parts + s.part);
        s.replayTo.reset();
        if (d > 0) s.want = d;
    }
    if (s.want != 0) {
        long long const want = s.want;
        s.want = 0;
        s.note.clear();
        Pos const from{s.frame, s.part};
        auto to = moved(from, want, s.parts);
        if (want > 0) {
            int const frames = std::max(0, to.frame - from.frame);
            for (int i = 0; i < frames; ++i) {
                if (!stepFrame(pl)) {
                    to = {s.frame, s.frame == from.frame ? from.part : 0};
                    if (!s.stoppedByDeath) s.note = "The frame didn't run (the level ended?)";
                    s.holdKey.clear();
                    s.holdBlocked = true;
                    s.toolHeld = -2;
                    break;
                }
            }
            if (s.frame != to.frame) to = {s.frame, s.frame == from.frame ? to.part : 0};
            s.part = to.part;
        }
        else {
            s.stoppedByDeath = false;
            if (to.frame < s.frame) {
                while (s.back.size() > 1 && s.back.back().frame > to.frame) popBack();
                if (s.back.empty()) {
                    to = {s.frame, 0};
                    s.note = "Can't go further back (no checkpoint kept from before here)";
                }
                else {
                    auto b = s.back.back();
                    s.back.pop_back();
                    if (b.frame > to.frame) {
                        to = {b.frame, 0};
                        s.note = fmt::format("Only went back to frame {} (Stepper: frames you can go back)", b.frame);
                    }
                    load(pl, b);
                    Global::get().checkpoints.erase(b.cp);
                    b.cp->release();
                    while (s.frame < to.frame) {
                        if (!stepFrame(pl)) break;
                    }
                    if (s.frame != to.frame) to = {s.frame, 0};
                }
            }
            s.part = to.part;
        }
        if (s.note.empty()) s.note = fmt::format("At {}", posText({s.frame, s.part}, s.parts));
    }
    updateLabel();
    drawGhost(pl);
    drawBoxes(pl);
    drawPanel(pl);
}

} // namespace xdstep

// ---- hooks ----

class $modify(XDStepperLayer, GJBaseGameLayer) {
    static void onModify(auto& self) {
        // first, so a TPS bypass (xdBot's or another mod's) gets one step a call while stepping
        if (auto r = self.setHookPriority("GJBaseGameLayer::update", Priority::VeryEarly); r.isErr())
            log::warn("Couldn't set the stepper's update hook priority: {}", r.unwrapErr());
    }
    void update(float dt) {
        if (!xdstep::stepperOn() || xdstep::stepperStepping() || !PlayLayer::get() || this != static_cast<GJBaseGameLayer*>(PlayLayer::get()))
            return GJBaseGameLayer::update(dt);
        xdstep::stepperFrame(this, dt);
    }
};

class $modify(XDStepperPlay, PlayLayer) {
    void destroyPlayer(PlayerObject* player, GameObject* object) {
        if (xdstep::stepperTakeDeath(this, player)) return;
        PlayLayer::destroyPlayer(player, object);
    }
    void resetLevel() {
        auto& s = xdstep::st();
        xdstep::clearArmed();
        PlayLayer::resetLevel();
        if (s.loading) return;
        if (!s.on) {
            s.usedThisAttempt = false;
            return;
        }
        // your restart with the stepper on: it stays on, from here, with no history
        xdstep::releaseBack();
        s.want = 0;
        s.playing = false;
        s.replayTo.reset();
        s.editEarliest = LLONG_MAX;
        s.diedAt = -1;
        s.stoppedByDeath = false;
        s.part = 0;
        int const now = Global::getCurrentFrame();
        s.frame = now + 1;
        s.pressedUpTo = m_gameState.m_levelTime <= 0.0 ? -1 : now;
        s.usedThisAttempt = true;
        Global::get().safeMode = true;
        unsigned ms = FMODAudioEngine::sharedEngine()->getMusicTimeMS(0);
        s.songBaseMs = ms > 0 ? ms - m_gameState.m_levelTime * 1000.0 : s.songBaseMs;
        m_queuedButtons.clear();
    }
    void levelComplete() {
        // a stepped attempt (deaths skipped, clicks between frames placed by hand) saves no completion
        bool const was = m_isTestMode;
        if (xdstep::st().usedThisAttempt) m_isTestMode = true;
        PlayLayer::levelComplete();
        m_isTestMode = was;
    }
    void onQuit() {
        auto& s = xdstep::st();
        if (s.on) xdstep::stepperStop(this, false);
        s.usedThisAttempt = false;
        s.label = nullptr;
        s.ghost = nullptr;
        s.boxes = nullptr;
        s.panel = nullptr;
        s.graph = nullptr;
        s.status = nullptr;
        s.nums.clear();
        s.buttons.clear();
        s.builtFor = nullptr;
        PlayLayer::onQuit();
    }
};

class $modify(XDStepperPause, PauseLayer) {
    void customSetup() {
        PauseLayer::customSetup();
        auto menu = this->getChildByID("right-button-menu");
        if (!menu) return;
        auto spr = ButtonSprite::create(xdstep::stepperOn() ? "Stepper off" : "Stepper", 70, true, "bigFont.fnt", "GJ_button_04.png", 30.f, 0.6f);
        spr->setScale(0.55f);
        auto btn = CCMenuItemSpriteExtra::create(spr, this, menu_selector(XDStepperPause::onSubtickStepper));
        btn->setID("subtick-stepper-button"_spr);
        menu->addChild(btn);
        menu->updateLayout();
    }
    void onSubtickStepper(CCObject*) {
        auto pl = PlayLayer::get();
        if (!pl) return;
        if (xdstep::stepperOn()) {
            xdstep::stepperStop(pl, false);
            this->onResume(nullptr);
            return;
        }
        this->onResume(nullptr);
        xdstep::stepperStart(pl);
    }
};

// ---- keys (Mod settings; all but on / off act only while the stepper is on) ----

$on_mod(Loaded) {
    auto onKey = [](char const* key, std::function<void()> fn) {
        geode::listenForKeybindSettingPresses(key, [fn](geode::Keybind const&, bool down, bool repeat, double) {
            auto pl = PlayLayer::get();
            if (!down || repeat || !pl || pl->m_isPaused || !xdstep::stepperOn()) return false;
            fn();
            return true;
        });
    };
    geode::listenForKeybindSettingPresses("key_stepper_toggle", [](geode::Keybind const&, bool down, bool repeat, double) {
        auto pl = PlayLayer::get();
        if (!down || repeat || !pl || pl->m_isPaused) return false;
        xdstep::stepperToggle();
        return true;
    });
    // steps: one now; with the key held, more after a moment (stepperFrame); the keyboard's own repeats are ignored
    auto stepKey = [](char const* key, int dir, bool whole, int k) {
        geode::listenForKeybindSettingPresses(key, [key, dir, whole, k](geode::Keybind const&, bool down, bool repeat, double) {
            auto& s = xdstep::st();
            if (!down && s.holdKey == key) {
                s.holdKey.clear();
                s.holdDir = 0;
            }
            if (!down) s.keyStepped[k] = false;
            auto pl = PlayLayer::get();
            if (!pl || pl->m_isPaused || !xdstep::stepperOn()) return false;
            if (!down || repeat) return true;
            if (s.keyStepped[k]) return true; // (the frame saw it go down and stepped already)
            s.keyStepped[k] = true;
            xdstep::stepOnce(dir, whole);
            s.holdKey = key;
            s.holdDir = dir;
            s.holdWhole = whole;
            s.holdT = 0;
            s.holdAcc = 0;
            return true;
        });
    };
    stepKey("key_stepper_tick", 1, true, 0);
    stepKey("key_stepper_tick_back", -1, true, 1);
    stepKey("key_stepper_fine", 1, false, 2);
    stepKey("key_stepper_fine_back", -1, false, 3);
    onKey("key_stepper_hold", [] { xdstep::doAction(xdstep::kHold); });
    onKey("key_stepper_play", [] { xdstep::doAction(xdstep::kPlay); });
    onKey("key_stepper_return", [] { xdstep::doAction(xdstep::kReturn); });
    onKey("key_stepper_pull", [] { xdstep::doAction(xdstep::kPull); });
    onKey("key_stepper_delete", [] { xdstep::doAction(xdstep::kDeleteNext); });
    onKey("key_stepper_undo", [] { xdstep::doAction(xdstep::kUndo); });
    onKey("key_stepper_finer", [] { xdstep::doAction(xdstep::kFiner); });
    onKey("key_stepper_coarser", [] { xdstep::doAction(xdstep::kCoarser); });
    onKey("key_stepper_mark", [] { xdstep::doAction(xdstep::kMark); });
}
