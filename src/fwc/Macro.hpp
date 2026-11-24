// Copied from the Frame Window Counter (PacificOC1/frame-windows, src/Macro.hpp, v1.150.0).
#pragma once

// Macro files from replay bots, so a macro can be measured straight from its file
// instead of from an attempt the mod watched a bot play. Pure logic (no Geode), so it
// can be tested on its own.
//
// Supported: GDR (.gdr, MessagePack) and GDR JSON (.gdr.json / .json), the format
// xdBot saves and several other bots read and write, and xdBot's compressed format
// (.cml, versions 1-3 and 5-7; 5-7 are gzip-compressed). v1.78: GDR2 (.gdr2) and Silicate's slc v2.

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace fwc {

// A parsed MessagePack / JSON value (only what GDR files use).
struct MVal {
    enum class Kind { Null, Bool, Int, Float, String, Array, Map };
    Kind kind = Kind::Null;
    bool b = false;
    std::int64_t i = 0;
    double f = 0;
    std::string s;
    std::vector<MVal> arr;
    std::vector<std::string> keys; // map keys, in file order
    std::vector<MVal> vals;        // ...and their values

    MVal const* get(std::string_view key) const;  // nullptr if not a map or no such key
    bool isNumber() const { return kind == Kind::Int || kind == Kind::Float; }
    double number(double fallback = 0) const;     // Int / Float / Bool as a number
    std::int64_t integer(std::int64_t fallback = 0) const;
    bool boolean(bool fallback = false) const;    // Bool, or a number != 0
    std::string string(std::string fallback = {}) const;
};

// nullopt (and `error`) if the bytes aren't a complete value of that format.
std::optional<MVal> parseMsgpack(std::string_view data, std::string* error = nullptr);
std::optional<MVal> parseJson(std::string_view data, std::string* error = nullptr);

struct MacroInput {
    int frame = 0;        // as stored in the file (version offset already added, see MacroFile)
    int button = 1;       // 1 jump, 2 left, 3 right
    bool p2 = false;      // the file's "2p" flag
    bool down = true;
    // v1.122 how far past `frame` it is, in 1/1000 of a frame (CML v5-7 keep CBF timings between frames); 0 = on the frame
    int sub = 0;
    // v1.137 a stepper click's finer timing past `sub`, in 1/10,000 of it (written to .cml down to its 1e-6 frame)
    int fine = 0;
};

struct MacroFile {
    std::string format;        // "GDR", "GDR JSON", "CML v7", ...
    bool xdBotStyle = false;   // frames counted the way xdBot counts (xdBot GDR files and every CML file)
    std::string macroName;     // as stored in the file (CML)
    std::size_t subtickInputs = 0; // inputs between two ticks (recorded with CBF): pressed on the tick they fall in
    std::string botName, botVersion;
    std::string levelName;
    std::uint32_t levelId = 0;
    std::string author, description;
    double framerate = 240.0;  // the macro's TPS
    double duration = 0;       // seconds, as stored
    int versionOffset = 0;     // added to every frame on load (old xdBot macros, as xdBot does)
    std::size_t frameFixes = 0; // position corrections in the file (not replayed)
    std::vector<MacroInput> inputs; // sorted by frame (stable)

    bool isXdBot() const { return botName == "xdBot"; }
    int lastFrame() const { return inputs.empty() ? 0 : inputs.back().frame; }
};

// v1.122 Writes a CML v6 macro (xdBot's format; plain "CML\0" header, the body zlib-wrapped in stored deflate blocks),
// with frames between ticks kept (frame × 1,000,000 + sub × 1000). parseCml reads it back the same.
std::string writeCml(MacroFile const& m);

// Reads a macro (GDR binary / JSON, or CML). nullopt and `error` if it can't be read.
// v1.97: `extension` (".slc", ".xd", lower case) for formats with no header to tell them by.
std::optional<MacroFile> parseMacro(std::string_view data, std::string* error = nullptr, std::string_view extension = {});
std::optional<MacroFile> parseCml(std::string_view data, std::string* error = nullptr);
// v1.78 (roadmap 5.8): GDR2 (.gdr2, "GDR" + varints: xdBot and others' newer format) and Silicate's slc v2
// (.slc, "SILL"). parseMacro detects both; slc v3 ("SLC3RPLY") is recognised but not read.
std::optional<MacroFile> parseGdr2(std::string_view data, std::string* error = nullptr);
std::optional<MacroFile> parseSlc2(std::string_view data, std::string* error = nullptr);
// v1.97 (roadmap 5.8): ReplayEngine 2 / 4 (.re2, .re4), Silicate's first format (.slc without a header), xdBot's
// old text format (.xd), Echo (.echo: old JSON or binary), ToastyReplay (.ttr: TTR, TTR2, TTR3).
std::optional<MacroFile> parseRe2(std::string_view data, std::string* error = nullptr);
std::optional<MacroFile> parseRe4(std::string_view data, std::string* error = nullptr);
std::optional<MacroFile> parseSlc1(std::string_view data, std::string* error = nullptr);
std::optional<MacroFile> parseXdText(std::string_view data, std::string* error = nullptr);
std::optional<MacroFile> parseEchoBinary(std::string_view data, std::string* error = nullptr);
std::optional<MacroFile> parseTtr(std::string_view data, std::string* error = nullptr);
std::optional<MacroFile> parseTcm(std::string_view data, std::string* error = nullptr); // TCBot .tcm v1 / v2
// v1.105 (roadmap 5.8): ReplayEngine 1 (.re, no header: told by the extension) and 3 (.re3), Silicate's slc v3
// ("SLC3RPLY"), ToastyReplay Lite (.ttrl, "TTRL")
std::optional<MacroFile> parseRe1(std::string_view data, std::string* error = nullptr);
std::optional<MacroFile> parseRe3(std::string_view data, std::string* error = nullptr);
std::optional<MacroFile> parseSlc3(std::string_view data, std::string* error = nullptr);
std::optional<MacroFile> parseTtrl(std::string_view data, std::string* error = nullptr);
struct MVal;
std::optional<MacroFile> parseEchoJson(MVal const& root, std::string* error = nullptr);

// Decompresses gzip, zlib or raw deflate data. nullopt (and `error`) if it's broken or would
// come out bigger than `maxOut`.
std::optional<std::string> inflateData(std::string_view data, std::size_t maxOut, std::string* error = nullptr);

// Old xdBot macros (before v2.3.6) store every frame one lower; xdBot adds 1 when it loads them.
int xdBotFrameOffset(std::string const& version);

// "xdBot v2.4.1, 812 inputs (406 presses), 240 TPS, level "Bloodbath" (10565740)"
std::string macroDescription(MacroFile const& m);

// Re-timing single inputs of a macro that dies part-way (some files have the odd input a tick
// off from the rest). `adjust` holds ticks added to each input, on top of the whole-file shift.
struct MacroRetime {
    std::vector<int> adjust;  // the whole adjust list to try (one entry per input)
    std::size_t input = 0;    // the input it moves
    int delta = 0;            // ...by this many ticks (its release moves with a press)
    // v1.117 between ticks: how far into its tick each input is pressed, in 1/kMacroSubUnits of a tick (0 = on
    // the tick, as the file has it; empty = all 0). `subDelta`: how far this candidate moves `input`, in the same
    // units (0 for whole-tick ones).
    std::vector<int> sub;
    int subDelta = 0;
    bool withRelease = false; // the press's release moved the same way
    // v1.120 wide re-timing: moved by more than the usual few ticks; `restFrom` >= 0: every input from that one on moved
    // by `delta` (the rest of the file a few ticks off from here)
    bool wide = false;
    long long restFrom = -1;
    // v1.130 press / release pair: the press moved `delta`, its release `releaseDelta` (whole ticks)
    bool pair = false;
    int releaseDelta = 0;
};

// Parts of a tick for MacroRetime::sub (the same as InputEvent::sub's kSubUnits).
constexpr int kMacroSubUnits = 1000;

// v1.117 Inputs that keep the file's order when played: by tick (adjust) for every input, as the
// file's inputs are pressed one after another, and strictly in time (tick, then how far into it)
// on each button / player, so nothing on one button overtakes another.
bool macroOrderOk(MacroFile const& m, std::vector<int> const& adjust, std::vector<int> const& sub);

// v1.117 After a play-through died having played `played` inputs (with `adjust` / `sub`), and moving
// inputs by whole ticks didn't get further: the same inputs between ticks, every `step` of a tick
// (1/kMacroSubUnits): the next input (not played) up to a tick earlier, then each of the last `back`
// played inputs up to a tick either way, nearest the file first. A press is tried alone and with its
// release. Positions on a whole tick are left out (whole-tick re-timing tries those).
std::vector<MacroRetime> macroSubCandidates(MacroFile const& m, std::vector<int> const& adjust, std::vector<int> const& sub,
    std::size_t played, std::size_t back = 3, int step = 100, int maxAdjust = 3);

// v1.120 when nothing closer works: moves of `from`..`maxShift` ticks, nearest first. At each size: the next input
// (not played) earlier, then each of the last `back` played inputs either way (a press alone and with its release),
// then the rest of the file from each of those inputs on, all by the same amount. No input ends up more than
// `maxTotal` ticks from the file; the order is kept (macroOrderOk).
std::vector<MacroRetime> macroWideCandidates(MacroFile const& m, std::vector<int> const& adjust, std::vector<int> const& sub,
    std::size_t played, std::size_t back = 3, int from = 3, int maxShift = 12, int maxTotal = 15);

// v1.130 a press and its release moved by different amounts (whole ticks): the hold made longer or shorter, or the
// press moved alone. For the next press (not played) and each of the last `back` played presses, every (press,
// release) move up to `maxMove` ticks each where the two differ, smallest total first. No input ends up more than
// `maxAdjust` ticks from the file; the file's order is kept.
std::vector<MacroRetime> macroPairCandidates(MacroFile const& m, std::vector<int> const& adjust, std::vector<int> const& sub,
    std::size_t played, std::size_t back = 3, int maxMove = 2, int maxAdjust = 3);

// v1.130 which play-through re-timing searches from. Before, any play-through that got even a tick further became
// the base, so "release later" (one tick later death) always won and a wrong move got built on. Now:
// - a re-timing is built on only if the player dies at least `bar` ticks later than the base (or not at all);
// - one that gets only a little further goes into a small beam, tried as a base when the current one runs out;
// - when the beam is empty too, the last move built on is undone and the rest of that base's list is tried.
struct MacroSearchBase {
    std::vector<int> adjust, sub;
    std::vector<std::pair<int, int>> ghosts;
    std::size_t played = 0;
    bool died = true;
    int diedAt = -1;
    std::size_t candNext = 0; // where its candidate list was when a move from it was built on
};
struct MacroSearch {
    int bar = 6;                 // ticks further to build on a move (<= 0: any reach further, as before v1.130)
    bool beamOn = true;          // the beam and the undo
    std::size_t beamMax = 4;
    int beamPicksMax = 24;
    int undoMax = 8;
    MacroSearchBase base;
    std::vector<MacroSearchBase> beam, undo;
    int beamPicks = 0, undos = 0, smallGains = 0; // (smallGains: play-throughs that got only a little further)
    enum class Verdict { Adopt, Beam, Drop };
    // a re-timing's play-through against the base; `wide`: a big move (v1.120: only a later death counts)
    Verdict judge(std::size_t played, bool died, int diedAt, bool wide = false) const;
    // build on `next` (the base's list was at `candNext`)
    void adopt(MacroSearchBase next, std::size_t candNext);
    // a play-through that got a little further: kept in the beam (furthest first, at most beamMax)
    void offer(MacroSearchBase b);
    // the base's list ran out: the next base (its candNext says where to go on in its list). None: nothing left.
    enum class Next { None, Beam, Undo };
    Next next();
    void reset(MacroSearchBase b);
};

// The press's release: the next release of the same button and player before that button is
// pressed again. npos if there's none.
std::size_t macroReleaseOf(MacroFile const& m, std::size_t press);

// What to try after a play-through died having played `played` inputs with `base`: the next
// input (not played) a tick or two earlier, then each of the last `back` played inputs, nearest
// first, by -1, +1, -2, +2. A press brings its release along. No input moves more than
// `maxAdjust` ticks from the file in total, and inputs stay in the file's order.
std::vector<MacroRetime> macroRetimeCandidates(MacroFile const& m, std::vector<int> const& base, std::size_t played,
    std::size_t back = 4, int maxAdjust = 3);

// v1.73 checkpoint mode for re-timing. Two play-throughs that read the file the same way press every
// input on the same tick until the first input their adjust lists move differently. `ticks`: the tick
// each input was pressed in play-through A (only the inputs it reached), `endTick`: where A stopped.
// Returns the earliest tick at which B can press something differently from A (INT_MAX if the lists
// are the same). A missing adjust entry counts as 0.
int macroFirstDifferentTick(std::vector<int> const& ticks, int endTick, std::vector<int> const& adjustA,
    std::vector<int> const& adjustB);
// v1.117 the same with inputs between ticks (MacroRetime::sub): one pressed another way inside its tick
// differs from the tick before the one A recorded it on (a press between ticks is recorded on the tick whose
// physics it's pressed in, a whole one at the end of the tick before).
int macroFirstDifferentTick(std::vector<int> const& ticks, int endTick, std::vector<int> const& adjustA,
    std::vector<int> const& adjustB, std::vector<int> const& subA, std::vector<int> const& subB);

// Of checkpoints taken at `cpTicks` (ascending), the latest one a play-through can go on from when it
// first presses differently at `firstDifferent`: taken more than `margin` ticks before that, and not at
// the very start (nothing to save there). -1 if none.
int macroPickCheckpoint(std::vector<int> const& cpTicks, int firstDifferent, int margin = 2);

} // namespace fwc
