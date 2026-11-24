// Host tests for the sub-tick stepper's timeline (src/stepper/core.hpp) and the CML reader / writer it saves with
// (src/fwc/Macro.cpp). No game needed. From the repo root: tests/run.sh
#include "../src/stepper/core.hpp"
#include "../src/fwc/Macro.hpp"

#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <vector>

using namespace xdstep;

namespace {
int failures = 0, checks = 0;
#define CHECK(c)                                                                                                       \
    do {                                                                                                               \
        ++checks;                                                                                                      \
        if (!(c)) {                                                                                                    \
            ++failures;                                                                                                \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);                                                   \
        }                                                                                                              \
    } while (0)

// the fields the templates use, like xdBot's input
struct In {
    unsigned frame = 0;
    long long sub = 0;
    int button = 1;
    bool player2 = false;
    bool down = true;
    long long origin = kNoOrigin;
};
In at(unsigned f, long long sub, bool down, int button = 1, bool p2 = false) {
    In e;
    e.frame = f;
    e.sub = sub;
    e.down = down;
    e.button = button;
    e.player2 = p2;
    return e;
}

void testRates() {
    CHECK(partsFor(2400, 240) == 10);
    CHECK(partsFor(240, 240) == 1);
    CHECK(partsFor(2'400'000'000LL, 240) == kUnits);
    CHECK(partsFor(4800, 480) == 10);
    CHECK(partsFor(1, 240) == 1);
    CHECK(hzOf(10, 240) == 2400);
    CHECK(subOf(3, 10) == 3'000'000);
    CHECK(subOf(0, 10) == 0);
    CHECK(subOf(1, kUnits) == 1);
    CHECK(decimalsFor(1) == 0);
    CHECK(decimalsFor(10) == 1);
    CHECK(decimalsFor(11) == 2);
    CHECK(decimalsFor(kUnits) == 7);
    CHECK(hzText(2'400'000'000LL) == "2,400,000,000");
    CHECK(hzText(240) == "240");
    CHECK(nextHz(240, 1, 240) == 2400);
    CHECK(nextHz(2400, -1, 240) == 240);
    CHECK(nextHz(240, -1, 240) == 240);
    CHECK(nextHz(2'400'000'000LL, 1, 240) == 2'400'000'000LL);
    CHECK(nextHz(3000, 1, 240) == 24000);
    CHECK(nextHz(3000, -1, 240) == 2400);
    CHECK(unitsText(1832LL * kUnits + 3'000'000, 10) == "1832.3");
    CHECK(unitsText(1832LL * kUnits, 1) == "1832.0");
    CHECK(unitsText(5LL * kUnits + 1, kUnits) == "5.0000001");
    CHECK(posText({1832, 3}, 10) == "1832.3");
}

void testMoves() {
    CHECK((moved({10, 3}, 4, 10) == Pos{10, 7}));
    CHECK((moved({10, 7}, 4, 10) == Pos{11, 1}));
    CHECK((moved({10, 1}, -3, 10) == Pos{9, 8}));
    CHECK((moved({0, 1}, -30, 10) == Pos{0, 0}));
    CHECK((moved({10, 0}, 10, 10) == Pos{11, 0}));
    CHECK((moved({10, 0}, -1, 1) == Pos{9, 0}));
    CHECK(keyOf(Pos{10, 5}, 10) == 10 * kUnits + kUnits / 2);
}

void testTimeline() {
    std::vector<In> tl;
    insertClickAt(tl, at(20, 0, true));
    insertClickAt(tl, at(10, 5'000'000, true));
    insertClickAt(tl, at(10, 0, false));
    insertClickAt(tl, at(30, 0, false));
    // same moment: goes after the one already there
    auto i = insertClickAt(tl, at(20, 0, false, 2));
    CHECK(i == 3);
    CHECK(isSorted(tl));
    CHECK(tl[0].frame == 10 && tl[0].sub == 0);
    CHECK(tl[1].frame == 10 && tl[1].sub == 5'000'000);
    CHECK(firstAtOrAfter(tl, 10 * kUnits + 1) == 1);
    CHECK(firstAtOrAfter(tl, 10 * kUnits + 5'000'001) == 2);
    CHECK(firstAtOrAfter(tl, 31 * kUnits) == tl.size());
    // held: the last click on jump at or before a moment
    CHECK(!heldAt(tl, 9 * kUnits, 1, false).has_value());
    CHECK(heldAt(tl, 10 * kUnits, 1, false) == std::optional<bool>(false));
    CHECK(heldAt(tl, 10 * kUnits + 4'999'999, 1, false) == std::optional<bool>(false));
    CHECK(heldAt(tl, 10 * kUnits + 5'000'000, 1, false) == std::optional<bool>(true));
    CHECK(heldAt(tl, 29 * kUnits, 1, false) == std::optional<bool>(true));
    CHECK(heldAt(tl, 30 * kUnits, 1, false) == std::optional<bool>(false));
    CHECK(!heldAt(tl, 30 * kUnits, 1, true).has_value()); // (the other flag has no clicks)
    CHECK(heldAt(tl, 25 * kUnits, 2, false) == std::optional<bool>(false));
    // due: clicks with frame in (after, upTo]
    auto due = dueIn(tl, 9, 10);
    CHECK(due.size() == 2 && due[0] == 0 && due[1] == 1);
    CHECK(dueIn(tl, 10, 19).empty());
    CHECK(dueIn(tl, 10, 20).size() == 2);
    CHECK(dueIn(tl, -1, 1000).size() == tl.size());
    // input numbers: presses counted, a release takes its press's number
    std::vector<In> n = {at(1, 0, true), at(2, 0, false), at(3, 0, true, 2), at(4, 0, true), at(5, 0, false, 2), at(6, 0, false)};
    auto nums = inputNumbers(n);
    CHECK((nums == std::vector<int>{1, 1, 2, 3, 2, 3}));
}

void testEdits() {
    std::vector<In> tl = {at(10, 0, true), at(12, 0, false), at(15, 0, true)};
    // the next click from frame 11 moved to 11.4
    int i = moveNextHere(tl, 11 * kUnits + 4'000'000);
    CHECK(i == 1);
    CHECK(tl[1].frame == 11 && tl[1].sub == 4'000'000);
    CHECK(tl[1].origin == 12 * kUnits);
    CHECK(isSorted(tl));
    CHECK(movedCount(tl) == 1);
    CHECK(std::fabs(movedFrames(tl[1]) - (-0.6)) < 1e-9);
    // moved again: still measured from where it first was
    auto m = movedBy(tl[1], 2'000'000);
    CHECK(m.frame == 11 && m.sub == 6'000'000 && m.origin == 12 * kUnits);
    auto m2 = movedBy(tl[1], 7'000'000); // across into the next frame
    CHECK(m2.frame == 12 && m2.sub == 1'000'000);
    auto m3 = movedBy(at(0, 1000, true), -5000);
    CHECK(m3.frame == 0 && m3.sub == 0);
    CHECK(moveNextHere(tl, 16 * kUnits) == -1);
    // delete the next
    auto copy = tl;
    auto gone = eraseNext(tl, 11 * kUnits);
    CHECK(gone && gone->frame == 11 && tl.size() == 2);
    CHECK(!eraseNext(tl, 16 * kUnits));
    // undo: where two timelines first differ
    CHECK(firstDifference(copy, tl) == 11 * kUnits + 4'000'000);
    CHECK(firstDifference(copy, copy) == LLONG_MAX);
    auto other = copy;
    other[2].down = false;
    CHECK(firstDifference(copy, other) == 15 * kUnits);
    other = copy;
    other.push_back(at(40, 0, true));
    CHECK(firstDifference(copy, other) == 40 * kUnits);
}

void testMarks() {
    Marks mk;
    CHECK(mk.text(10).empty());
    mk.add(10 * kUnits + 5'000'000);
    CHECK(!mk.done());
    mk.add(10 * kUnits + 2'000'000);
    CHECK(mk.done());
    CHECK(*mk.lo == 10 * kUnits + 2'000'000);
    CHECK(std::fabs(mk.windowFrames(10) - 0.4) < 1e-9); // 0.3 between + one step of 0.1
    mk.add(5);
    CHECK(!mk.done() && *mk.lo == 5);
}

// CML (xdBot's format): write → read keeps frames, buttons, flags, and the timing between frames to 1e-6 frame
void testCml() {
    fwc::MacroFile m;
    m.botName = "xdBot";
    m.botVersion = "v2.5.0-cbf.1";
    m.author = "me";
    m.description = "test";
    m.macroName = "level";
    m.framerate = 240;
    m.duration = 3.5;
    std::mt19937 rng(7);
    std::vector<std::pair<int, long long>> want; // frame, sub in kUnits (rounded to the file's 1e-6 frame)
    int frame = 3;
    for (int k = 0; k < 500; ++k) {
        frame += 1 + static_cast<int>(rng() % 30); // (one input a frame: the writer sorts by frame, then sub)
        long long sub = (k % 3 == 0) ? 0 : static_cast<long long>(rng() % kUnits);
        fwc::MacroInput in;
        in.frame = frame;
        in.sub = static_cast<int>(sub / 10'000);
        in.fine = static_cast<int>(sub % 10'000);
        in.down = k % 2 == 0;
        in.button = 1 + static_cast<int>(rng() % 3);
        in.p2 = rng() % 2;
        m.inputs.push_back(in);
        want.push_back({frame, sub / 10 * 10});
    }
    // (the writer sorts by frame then sub: keep that order here too)
    std::string data = fwc::writeCml(m);
    std::string err;
    auto back = fwc::parseCml(data, &err);
    CHECK(back.has_value());
    if (!back) {
        std::printf("  parseCml: %s\n", err.c_str());
        return;
    }
    CHECK(back->inputs.size() == m.inputs.size());
    CHECK(back->author == "me" && back->macroName == "level" && back->botName == "xdBot");
    CHECK(std::fabs(back->framerate - 240) < 1e-3);
    int subs = 0;
    for (std::size_t i = 0; i < std::min(back->inputs.size(), m.inputs.size()); ++i) {
        auto const& a = m.inputs[i];
        auto const& b = back->inputs[i];
        long long const sub = static_cast<long long>(b.sub) * 10'000 + b.fine;
        CHECK(b.frame == a.frame);
        CHECK(sub == want[i].second);
        CHECK(b.down == a.down && b.button == a.button && b.p2 == a.p2);
        subs += sub > 0;
    }
    CHECK(back->subtickInputs == static_cast<std::size_t>(subs));
    // the smallest timing CML holds: 1e-6 frame
    fwc::MacroFile one = m;
    one.inputs.clear();
    fwc::MacroInput in;
    in.frame = 100;
    in.sub = 0;
    in.fine = 10;
    one.inputs.push_back(in);
    auto b1 = fwc::parseCml(fwc::writeCml(one));
    CHECK(b1 && b1->inputs.size() == 1 && b1->inputs[0].frame == 100 && b1->inputs[0].sub == 0 && b1->inputs[0].fine == 10);
    // and just below a frame
    one.inputs[0].sub = 999;
    one.inputs[0].fine = 9999;
    auto b2 = fwc::parseCml(fwc::writeCml(one));
    CHECK(b2 && b2->inputs[0].frame == 100 && b2->inputs[0].sub == 999 && b2->inputs[0].fine == 9990);
}
} // namespace

int main() {
    testRates();
    testMoves();
    testTimeline();
    testEdits();
    testMarks();
    testCml();
    std::printf("%d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
