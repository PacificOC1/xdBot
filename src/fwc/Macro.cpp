// Copied from the Frame Window Counter (PacificOC1/frame-windows, src/Macro.cpp, v1.150.0) for its CML reader and writer.
// Change here: parseCml keeps a CML timing to the file's 1e-6 frame (MacroInput::sub + fine), not rounded to 1/1000.
#include "Macro.hpp"

#include <limits>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <type_traits>

namespace fwc {

// ---- MVal -----------------------------------------------------------------

MVal const* MVal::get(std::string_view key) const {
    if (kind != Kind::Map) return nullptr;
    for (std::size_t k = 0; k < keys.size() && k < vals.size(); ++k)
        if (keys[k] == key) return &vals[k];
    return nullptr;
}

double MVal::number(double fallback) const {
    switch (kind) {
        case Kind::Int: return static_cast<double>(i);
        case Kind::Float: return f;
        case Kind::Bool: return b ? 1.0 : 0.0;
        default: return fallback;
    }
}

std::int64_t MVal::integer(std::int64_t fallback) const {
    switch (kind) {
        case Kind::Int: return i;
        case Kind::Float: return std::isfinite(f) ? static_cast<std::int64_t>(f) : fallback;
        case Kind::Bool: return b ? 1 : 0;
        default: return fallback;
    }
}

bool MVal::boolean(bool fallback) const {
    switch (kind) {
        case Kind::Bool: return b;
        case Kind::Int: return i != 0;
        case Kind::Float: return f != 0;
        default: return fallback;
    }
}

std::string MVal::string(std::string fallback) const {
    return kind == Kind::String ? s : fallback;
}

namespace {

constexpr int kMaxDepth = 64;

// ---- MessagePack ------------------------------------------------------------

struct MsgReader {
    std::string_view d;
    std::size_t p = 0;
    std::string err;

    bool need(std::size_t n) {
        if (d.size() - p < n) {
            if (err.empty()) err = "the file ends in the middle of a value";
            return false;
        }
        return true;
    }
    std::uint64_t be(std::size_t n) {
        std::uint64_t v = 0;
        for (std::size_t k = 0; k < n; ++k) v = (v << 8) | static_cast<unsigned char>(d[p + k]);
        p += n;
        return v;
    }

    bool str(std::size_t n, MVal& out) {
        if (!need(n)) return false;
        out.kind = MVal::Kind::String;
        out.s.assign(d.substr(p, n));
        p += n;
        return true;
    }

    bool array(std::size_t n, MVal& out, int depth) {
        // every element takes at least one byte: a bad count can't make us allocate much
        if (d.size() - p < n) return need(n);
        out.kind = MVal::Kind::Array;
        out.arr.resize(n);
        for (auto& e : out.arr)
            if (!value(e, depth + 1)) return false;
        return true;
    }

    bool mapOf(std::size_t n, MVal& out, int depth) {
        if ((d.size() - p) / 2 < n) return need(n * 2);
        out.kind = MVal::Kind::Map;
        out.keys.resize(n);
        out.vals.resize(n);
        for (std::size_t k = 0; k < n; ++k) {
            MVal key;
            if (!value(key, depth + 1)) return false;
            if (key.kind == MVal::Kind::String) out.keys[k] = key.s;
            else if (key.kind == MVal::Kind::Int) out.keys[k] = std::to_string(key.i);
            else out.keys[k] = "?";
            if (!value(out.vals[k], depth + 1)) return false;
        }
        return true;
    }

    bool value(MVal& out, int depth) {
        if (depth > kMaxDepth) {
            err = "values are nested too deeply";
            return false;
        }
        if (!need(1)) return false;
        auto t = static_cast<unsigned char>(d[p++]);
        if (t <= 0x7f) { out.kind = MVal::Kind::Int; out.i = t; return true; }
        if (t >= 0xe0) { out.kind = MVal::Kind::Int; out.i = static_cast<std::int8_t>(t); return true; }
        if ((t & 0xf0) == 0x80) return mapOf(t & 0x0f, out, depth);
        if ((t & 0xf0) == 0x90) return array(t & 0x0f, out, depth);
        if ((t & 0xe0) == 0xa0) return str(t & 0x1f, out);
        switch (t) {
            case 0xc0: out.kind = MVal::Kind::Null; return true;
            case 0xc2: out.kind = MVal::Kind::Bool; out.b = false; return true;
            case 0xc3: out.kind = MVal::Kind::Bool; out.b = true; return true;
            case 0xc4: case 0xd9: if (!need(1)) return false; return str(be(1), out);
            case 0xc5: case 0xda: if (!need(2)) return false; return str(be(2), out);
            case 0xc6: case 0xdb: if (!need(4)) return false; return str(be(4), out);
            case 0xc7: case 0xc8: case 0xc9: { // ext: skipped
                std::size_t lenBytes = t == 0xc7 ? 1 : t == 0xc8 ? 2 : 4;
                if (!need(lenBytes)) return false;
                auto n = be(lenBytes);
                if (!need(1 + n)) return false;
                p += 1 + n;
                out.kind = MVal::Kind::Null;
                return true;
            }
            case 0xca: {
                if (!need(4)) return false;
                auto bits = static_cast<std::uint32_t>(be(4));
                float v;
                std::memcpy(&v, &bits, 4);
                out.kind = MVal::Kind::Float;
                out.f = v;
                return true;
            }
            case 0xcb: {
                if (!need(8)) return false;
                auto bits = be(8);
                double v;
                std::memcpy(&v, &bits, 8);
                out.kind = MVal::Kind::Float;
                out.f = v;
                return true;
            }
            case 0xcc: case 0xcd: case 0xce: case 0xcf: {
                std::size_t n = std::size_t(1) << (t - 0xcc);
                if (!need(n)) return false;
                auto v = be(n);
                out.kind = MVal::Kind::Int;
                out.i = static_cast<std::int64_t>(v);
                return true;
            }
            case 0xd0: case 0xd1: case 0xd2: case 0xd3: {
                std::size_t n = std::size_t(1) << (t - 0xd0);
                if (!need(n)) return false;
                auto v = be(n);
                std::int64_t sv;
                if (n == 1) sv = static_cast<std::int8_t>(v);
                else if (n == 2) sv = static_cast<std::int16_t>(v);
                else if (n == 4) sv = static_cast<std::int32_t>(v);
                else sv = static_cast<std::int64_t>(v);
                out.kind = MVal::Kind::Int;
                out.i = sv;
                return true;
            }
            case 0xd4: case 0xd5: case 0xd6: case 0xd7: case 0xd8: { // fixext: skipped
                std::size_t n = std::size_t(1) << (t - 0xd4);
                if (!need(1 + n)) return false;
                p += 1 + n;
                out.kind = MVal::Kind::Null;
                return true;
            }
            case 0xdc: if (!need(2)) return false; return array(be(2), out, depth);
            case 0xdd: if (!need(4)) return false; return array(be(4), out, depth);
            case 0xde: if (!need(2)) return false; return mapOf(be(2), out, depth);
            case 0xdf: if (!need(4)) return false; return mapOf(be(4), out, depth);
            default:
                err = "not MessagePack (unknown type byte)";
                return false;
        }
    }
};

// ---- JSON --------------------------------------------------------------------

struct JsonReader {
    std::string_view d;
    std::size_t p = 0;
    std::string err;

    void ws() {
        while (p < d.size() && (d[p] == ' ' || d[p] == '\t' || d[p] == '\n' || d[p] == '\r')) ++p;
    }
    bool fail(char const* what) {
        if (err.empty()) err = std::string(what) + " at byte " + std::to_string(p);
        return false;
    }
    bool lit(std::string_view w) {
        if (d.substr(p, w.size()) != w) return fail("unexpected text");
        p += w.size();
        return true;
    }
    static void utf8(std::string& out, std::uint32_t cp) {
        if (cp < 0x80) out += static_cast<char>(cp);
        else if (cp < 0x800) {
            out += static_cast<char>(0xc0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3f));
        }
        else if (cp < 0x10000) {
            out += static_cast<char>(0xe0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3f));
            out += static_cast<char>(0x80 | (cp & 0x3f));
        }
        else {
            out += static_cast<char>(0xf0 | (cp >> 18));
            out += static_cast<char>(0x80 | ((cp >> 12) & 0x3f));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3f));
            out += static_cast<char>(0x80 | (cp & 0x3f));
        }
    }
    bool hex4(std::uint32_t& v) {
        if (d.size() - p < 4) return fail("short \\u escape");
        v = 0;
        for (int k = 0; k < 4; ++k) {
            char c = d[p++];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= c - '0';
            else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10;
            else return fail("bad \\u escape");
        }
        return true;
    }
    bool string(std::string& out) {
        ++p; // opening quote
        while (true) {
            if (p >= d.size()) return fail("unterminated string");
            char c = d[p++];
            if (c == '"') return true;
            if (c != '\\') { out += c; continue; }
            if (p >= d.size()) return fail("unterminated string");
            char e = d[p++];
            switch (e) {
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case '/': out += '/'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'u': {
                    std::uint32_t cp;
                    if (!hex4(cp)) return false;
                    if (cp >= 0xd800 && cp < 0xdc00 && d.substr(p, 2) == "\\u") {
                        p += 2;
                        std::uint32_t lo;
                        if (!hex4(lo)) return false;
                        if (lo >= 0xdc00 && lo < 0xe000) cp = 0x10000 + ((cp - 0xd800) << 10) + (lo - 0xdc00);
                    }
                    utf8(out, cp);
                    break;
                }
                default: return fail("bad escape");
            }
        }
    }
    bool number(MVal& out) {
        std::size_t start = p;
        if (p < d.size() && d[p] == '-') ++p;
        bool isFloat = false;
        while (p < d.size()) {
            char c = d[p];
            if (c >= '0' && c <= '9') ++p;
            else if (c == '.' || c == 'e' || c == 'E' || c == '+' || c == '-') { isFloat = true; ++p; }
            else break;
        }
        std::string text(d.substr(start, p - start));
        if (text.empty() || text == "-") return fail("bad number");
        try {
            if (!isFloat) {
                out.kind = MVal::Kind::Int;
                out.i = std::stoll(text);
            }
            else {
                out.kind = MVal::Kind::Float;
                out.f = std::stod(text);
            }
        }
        catch (...) {
            out.kind = MVal::Kind::Float; // out of range for an integer
            try { out.f = std::stod(text); } catch (...) { return fail("bad number"); }
        }
        return true;
    }
    bool value(MVal& out, int depth) {
        if (depth > kMaxDepth) return fail("values nested too deeply");
        ws();
        if (p >= d.size()) return fail("unexpected end");
        char c = d[p];
        if (c == '{') {
            ++p;
            out.kind = MVal::Kind::Map;
            ws();
            if (p < d.size() && d[p] == '}') { ++p; return true; }
            while (true) {
                ws();
                if (p >= d.size() || d[p] != '"') return fail("expected a key");
                std::string key;
                if (!string(key)) return false;
                ws();
                if (p >= d.size() || d[p] != ':') return fail("expected ':'");
                ++p;
                MVal v;
                if (!value(v, depth + 1)) return false;
                out.keys.push_back(std::move(key));
                out.vals.push_back(std::move(v));
                ws();
                if (p < d.size() && d[p] == ',') { ++p; continue; }
                if (p < d.size() && d[p] == '}') { ++p; return true; }
                return fail("expected ',' or '}'");
            }
        }
        if (c == '[') {
            ++p;
            out.kind = MVal::Kind::Array;
            ws();
            if (p < d.size() && d[p] == ']') { ++p; return true; }
            while (true) {
                MVal v;
                if (!value(v, depth + 1)) return false;
                out.arr.push_back(std::move(v));
                ws();
                if (p < d.size() && d[p] == ',') { ++p; continue; }
                if (p < d.size() && d[p] == ']') { ++p; return true; }
                return fail("expected ',' or ']'");
            }
        }
        if (c == '"') {
            out.kind = MVal::Kind::String;
            return string(out.s);
        }
        if (c == 't') { out.kind = MVal::Kind::Bool; out.b = true; return lit("true"); }
        if (c == 'f') { out.kind = MVal::Kind::Bool; out.b = false; return lit("false"); }
        if (c == 'n') { out.kind = MVal::Kind::Null; return lit("null"); }
        if (c == '-' || (c >= '0' && c <= '9')) return number(out);
        return fail("unexpected character");
    }
};

std::vector<int> versionNumbers(std::vector<std::string> const& parts) {
    std::vector<int> out;
    for (auto const& s : parts) {
        int v = 0;
        std::size_t k = 0;
        while (k < s.size() && s[k] >= '0' && s[k] <= '9' && v < 100000) v = v * 10 + (s[k++] - '0');
        out.push_back(v);
    }
    return out;
}

} // namespace

std::optional<MVal> parseMsgpack(std::string_view data, std::string* error) {
    MsgReader r{data};
    MVal v;
    if (!r.value(v, 0)) {
        if (error) *error = r.err;
        return std::nullopt;
    }
    if (r.p != data.size()) {
        if (error) *error = "extra bytes after the value";
        return std::nullopt;
    }
    return v;
}

std::optional<MVal> parseJson(std::string_view data, std::string* error) {
    if (data.substr(0, 3) == "\xEF\xBB\xBF") data.remove_prefix(3); // UTF-8 BOM
    JsonReader r{data};
    MVal v;
    if (!r.value(v, 0)) {
        if (error) *error = r.err;
        return std::nullopt;
    }
    r.ws();
    if (r.p != data.size()) {
        if (error) *error = "extra text after the value";
        return std::nullopt;
    }
    return v;
}

int xdBotFrameOffset(std::string const& version) {
    std::string ver = version;
    if (!ver.empty() && ver.front() == 'v') ver = ver.substr(1);
    std::vector<std::string> parts;
    std::stringstream ss(ver);
    std::string item;
    while (std::getline(ss, item, '.')) parts.push_back(item);
    if (parts.empty() || parts.size() > 3) return 1; // pre-releases ("2.0.0-beta.1") are old
    auto n = versionNumbers(parts);
    while (n.size() < 3) n.push_back(0);
    std::vector<int> check{2, 3, 6};
    return n >= check ? 0 : 1;
}

// ---- v1.78 GDR2 (maxnut/GDReplayFormat): "GDR", then LEB128 varints, length-prefixed strings, and the
// two floating-point fields big-endian. Inputs: every player 1 input, then every player 2 input, each a
// varint "delta from the previous frame << 1 | down" (platformer: "<< 3 | button << 1 | down").
namespace {
struct ByteReader {
    std::string_view d;
    std::size_t p = 0;
    bool bad = false;
    std::uint64_t varint() {
        std::uint64_t v = 0;
        for (int shift = 0; shift < 64; shift += 7) {
            if (p >= d.size()) {
                bad = true;
                return 0;
            }
            auto b = static_cast<unsigned char>(d[p++]);
            v |= static_cast<std::uint64_t>(b & 0x7F) << shift;
            if (!(b & 0x80)) return v;
        }
        bad = true;
        return 0;
    }
    std::string str() {
        auto n = varint();
        if (bad || n > 0xFFFF || p + n > d.size()) {
            bad = true;
            return {};
        }
        std::string s(d.substr(p, n));
        p += n;
        return s;
    }
    template <typename T> T bigEndian() {
        if (p + sizeof(T) > d.size()) {
            bad = true;
            return T{};
        }
        unsigned char b[sizeof(T)];
        for (std::size_t i = 0; i < sizeof(T); ++i) b[i] = static_cast<unsigned char>(d[p + sizeof(T) - 1 - i]);
        p += sizeof(T);
        T v;
        std::memcpy(&v, b, sizeof(T));
        return v;
    }
    template <typename T> T little() {
        if (p + sizeof(T) > d.size()) {
            bad = true;
            return T{};
        }
        T v;
        std::memcpy(&v, d.data() + p, sizeof(T));
        p += sizeof(T);
        return v;
    }
    void skip(std::uint64_t n) {
        if (p + n > d.size()) bad = true;
        else p += n;
    }
};
} // namespace

std::optional<MacroFile> parseGdr2(std::string_view data, std::string* error) {
    auto fail = [&](std::string why) -> std::optional<MacroFile> {
        if (error) *error = "GDR2: " + why;
        return std::nullopt;
    };
    if (data.substr(0, 3) != "GDR") return fail("no GDR header");
    ByteReader r{data, 3};
    MacroFile m;
    auto version = r.varint();
    std::string inputTag = r.str();
    m.author = r.str();
    m.description = r.str();
    m.duration = r.bigEndian<float>();
    (void)r.varint(); // game version
    m.framerate = r.bigEndian<double>();
    (void)r.varint(); // seed
    (void)r.varint(); // coins
    (void)r.varint(); // low detail
    bool platformer = r.varint() != 0;
    m.botName = r.str();
    m.botVersion = std::to_string(r.varint());
    m.levelId = static_cast<std::uint32_t>(r.varint());
    m.levelName = r.str();
    r.skip(r.varint()); // the replay's extension
    auto deaths = r.varint();
    for (std::uint64_t i = 0; i < deaths && !r.bad; ++i) (void)r.varint();
    (void)r.varint(); // input count
    auto p1Left = r.varint();
    if (r.bad) return fail("the header is cut short");
    if (!(m.framerate > 0.0 && m.framerate < 100000.0)) m.framerate = 240.0;
    m.format = "GDR2 v" + std::to_string(version);
    m.xdBotStyle = m.isXdBot();
    std::uint64_t prev = 0;
    while (r.p < r.d.size()) {
        auto packed = r.varint();
        if (r.bad) return fail("an input is cut short");
        MacroInput mi;
        mi.p2 = p1Left == 0;
        mi.down = packed & 1;
        std::uint64_t delta = platformer ? packed >> 3 : packed >> 1;
        mi.button = platformer ? static_cast<int>((packed >> 1) & 3) : 1;
        std::uint64_t frame = prev + delta;
        if (!inputTag.empty()) r.skip(r.varint()); // an input extension
        if (r.bad) return fail("an input extension is cut short");
        prev = frame;
        if (p1Left > 0 && --p1Left == 0) prev = 0; // player 2's inputs start from frame 0 again
        if (mi.button < 1 || mi.button > 3 || frame > 100000000) continue;
        mi.frame = static_cast<int>(frame);
        m.inputs.push_back(mi);
    }
    std::stable_sort(m.inputs.begin(), m.inputs.end(), [](MacroInput const& a, MacroInput const& b) { return a.frame < b.frame; });
    if (m.inputs.empty()) return fail("the macro has no inputs");
    return m;
}

// ---- v1.78 Silicate's slc v2: "SILL", tps (double), meta size + meta, input count, blobs (byte size, start,
// count: u64 each), then the inputs blob by blob, each `byte size` bytes of "delta << 5 | type << 2 | p2 << 1 |
// down" (type 1-3 = jump / left / right, 4-5 restarts, 6 death, 7 TPS followed by a double), then "EOM".
// Everything little-endian.
std::optional<MacroFile> parseSlc2(std::string_view data, std::string* error) {
    auto fail = [&](std::string why) -> std::optional<MacroFile> {
        if (error) *error = "slc: " + why;
        return std::nullopt;
    };
    if (data.substr(0, 4) != "SILL") return fail("no SILL header");
    ByteReader r{data, 4};
    MacroFile m;
    m.format = "slc v2";
    m.botName = "Silicate";
    m.framerate = r.little<double>();
    r.skip(r.little<std::uint64_t>()); // meta
    auto count = r.little<std::uint64_t>();
    auto blobs = r.little<std::uint64_t>();
    if (r.bad || blobs > 1000000 || count > 100000000) return fail("the header is cut short");
    struct Blob { std::uint64_t size, start, length; };
    std::vector<Blob> list;
    for (std::uint64_t i = 0; i < blobs && !r.bad; ++i) {
        Blob b{r.little<std::uint64_t>(), r.little<std::uint64_t>(), r.little<std::uint64_t>()};
        if (b.size != 1 && b.size != 2 && b.size != 4 && b.size != 8) return fail("a blob of " + std::to_string(b.size) + " bytes");
        list.push_back(b);
    }
    if (r.bad) return fail("the blob list is cut short");
    if (!(m.framerate > 0.0 && m.framerate < 100000.0)) m.framerate = 240.0;
    std::uint64_t frame = 0;
    std::size_t restarts = 0;
    for (auto const& b : list) {
        for (std::uint64_t i = 0; i < b.length; ++i) {
            std::uint64_t state = 0;
            if (r.p + b.size > r.d.size()) return fail("an input is cut short");
            std::memcpy(&state, r.d.data() + r.p, b.size);
            r.p += b.size;
            frame += state >> 5;
            int type = static_cast<int>((state >> 2) & 7);
            if (type == 7) {
                double tps = r.little<double>();
                if (r.bad) return fail("a TPS change is cut short");
                if (tps > 0.0 && m.inputs.empty()) m.framerate = tps; // only a change before the first input is taken
                continue;
            }
            if (type == 4 || type == 5) {
                // a restart: only the attempt after the last one is the macro's run
                ++restarts;
                m.inputs.clear();
                continue;
            }
            if (type < 1 || type > 3) continue; // skip / death
            MacroInput mi;
            mi.frame = static_cast<int>(std::min<std::uint64_t>(frame, 100000000));
            mi.button = type;
            mi.p2 = (state >> 1) & 1;
            mi.down = state & 1;
            m.inputs.push_back(mi);
        }
    }
    if (r.d.substr(r.p, 3) != "EOM") return fail("no EOM footer");
    if (restarts > 0) m.description = std::to_string(restarts) + " restart(s) in the file: the last attempt is used";
    if (m.inputs.empty()) return fail("the macro has no inputs");
    return m;
}

// ---- v1.97 (roadmap 5.8): more bots' formats. Byte layouts as zcb3 (zeozeozeo/zcb3, public domain) reads them;
// the code here is our own.
namespace {

std::optional<MacroFile> macroFail(std::string* error, std::string what, std::string why) {
    if (error) *error = what + ": " + why;
    return std::nullopt;
}

void sortInputs(MacroFile& m) {
    std::stable_sort(m.inputs.begin(), m.inputs.end(), [](MacroInput const& a, MacroInput const& b) { return a.frame < b.frame; });
}

MacroInput makeInput(std::int64_t frame, int button, bool p2, bool down) {
    MacroInput mi;
    mi.frame = static_cast<int>(std::clamp<std::int64_t>(frame, 0, 100000000));
    mi.button = button == 2 || button == 3 ? button : 1;
    mi.p2 = p2;
    mi.down = down;
    return mi;
}

} // namespace

// ReplayEngine 2 (TobyAdd's GDH): "RE2", u32 count, then {u32 frame, bool hold, i32 button, bool player2} as a
// C struct (16 bytes with padding). Always 240 TPS.
std::optional<MacroFile> parseRe2(std::string_view data, std::string* error) {
    if (data.substr(0, 3) != "RE2") return macroFail(error, "re2", "no RE2 header");
    ByteReader r{data, 3};
    auto count = r.little<std::uint32_t>();
    if (r.bad || static_cast<std::uint64_t>(count) * 16 > data.size()) return macroFail(error, "re2", "the input count doesn't fit the file");
    MacroFile m;
    m.format = "ReplayEngine 2";
    m.botName = "ReplayEngine";
    m.framerate = 240.0;
    for (std::uint32_t i = 0; i < count && !r.bad; ++i) {
        auto frame = r.little<std::uint32_t>();
        bool hold = r.little<std::uint8_t>() != 0;
        r.skip(3);
        auto button = r.little<std::int32_t>();
        bool p2 = r.little<std::uint8_t>() != 0;
        r.skip(3);
        if (!r.bad) m.inputs.push_back(makeInput(frame, button, p2, hold));
    }
    if (r.bad) return macroFail(error, "re2", "the inputs are cut short");
    sortInputs(m);
    if (m.inputs.empty()) return macroFail(error, "re2", "the macro has no inputs");
    return m;
}

// ReplayEngine 4: "RE4", f32 TPS, u64 count of physics records {u64 frame, f32 x, f32 y, f64 y speed, u8 p2}, u64
// count of inputs {u64 frame, u8 down, i32 button, u8 p2}. Little-endian, packed.
std::optional<MacroFile> parseRe4(std::string_view data, std::string* error) {
    if (data.substr(0, 3) != "RE4") return macroFail(error, "re4", "no RE4 header");
    ByteReader r{data, 3};
    MacroFile m;
    m.format = "ReplayEngine 4";
    m.botName = "ReplayEngine";
    m.framerate = r.little<float>();
    auto physics = r.little<std::uint64_t>();
    if (r.bad || physics > data.size() / 25 + 1) return macroFail(error, "re4", "the header is cut short");
    r.skip(physics * 25);
    auto count = r.little<std::uint64_t>();
    if (r.bad || count > data.size() / 14 + 1) return macroFail(error, "re4", "the input count doesn't fit the file");
    for (std::uint64_t i = 0; i < count && !r.bad; ++i) {
        auto frame = r.little<std::uint64_t>();
        bool down = r.little<std::uint8_t>() != 0;
        auto button = r.little<std::int32_t>();
        bool p2 = r.little<std::uint8_t>() != 0;
        if (!r.bad) m.inputs.push_back(makeInput(static_cast<std::int64_t>(std::min<std::uint64_t>(frame, 100000000)), button, p2, down));
    }
    if (r.bad) return macroFail(error, "re4", "the inputs are cut short");
    if (!(m.framerate > 0.0) || m.framerate > 100000.0) m.framerate = 240.0;
    sortInputs(m);
    if (m.inputs.empty()) return macroFail(error, "re4", "the macro has no inputs");
    return m;
}

// Silicate's first format (.slc, no header): f64 TPS, u32 count, then u32 per input: frame << 4 | p2 << 3 |
// button << 1 | down.
std::optional<MacroFile> parseSlc1(std::string_view data, std::string* error) {
    ByteReader r{data, 0};
    MacroFile m;
    m.format = "slc v1";
    m.botName = "Silicate";
    m.framerate = r.little<double>();
    auto count = r.little<std::uint32_t>();
    if (r.bad || !(m.framerate > 0.0) || m.framerate > 100000.0) return macroFail(error, "slc", "not an slc macro (no frame rate)");
    if (static_cast<std::uint64_t>(count) * 4 + 12 > data.size()) return macroFail(error, "slc", "the input count doesn't fit the file");
    for (std::uint32_t i = 0; i < count; ++i) {
        auto a = r.little<std::uint32_t>();
        int button = static_cast<int>((a >> 1) & 3);
        if (button < 1) continue;
        m.inputs.push_back(makeInput(a >> 4, button, (a & 8) != 0, (a & 1) != 0));
    }
    sortInputs(m);
    if (m.inputs.empty()) return macroFail(error, "slc", "the macro has no inputs");
    return m;
}

// xdBot's old text format (.xd): the TPS on the first line, then one input per line:
// frame|holding|button|player1|pos_only|x|y|... ("player1" 1 = player 1).
std::optional<MacroFile> parseXdText(std::string_view data, std::string* error) {
    MacroFile m;
    m.format = "xdBot .xd";
    m.botName = "xdBot";
    m.xdBotStyle = true;
    m.versionOffset = 1; // an old xdBot file: frames stored one lower (as for GDR files before v2.3.6)
    std::size_t pos = 0;
    bool any = false;
    while (pos < data.size()) {
        std::size_t end = data.find('\n', pos);
        if (end == std::string_view::npos) end = data.size();
        std::string_view line = data.substr(pos, end - pos);
        pos = end + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.remove_suffix(1);
        if (line.empty()) continue;
        std::vector<std::string_view> f;
        for (std::size_t a = 0;;) {
            std::size_t b = line.find('|', a);
            f.push_back(line.substr(a, b == std::string_view::npos ? std::string_view::npos : b - a));
            if (b == std::string_view::npos) break;
            a = b + 1;
        }
        auto num = [](std::string_view t, double& out) {
            std::string s(t);
            char* endp = nullptr;
            out = std::strtod(s.c_str(), &endp);
            return !s.empty() && endp && *endp == '\0';
        };
        double v = 0;
        if (f.size() == 1) {
            if (!num(f[0], v) || !(v > 0.0)) return macroFail(error, "xd", "a line that isn't a frame rate or an input");
            m.framerate = v;
            continue;
        }
        double frame = 0, hold = 0, button = 1, p1 = 1;
        if (f.size() < 4 || !num(f[0], frame) || !num(f[1], hold) || !num(f[2], button) || !num(f[3], p1) || frame < 0)
            return macroFail(error, "xd", "a line that isn't an input");
        m.inputs.push_back(makeInput(static_cast<std::int64_t>(frame) + m.versionOffset, static_cast<int>(button), p1 == 0, hold != 0));
        any = true;
    }
    if (!any) return macroFail(error, "xd", "the macro has no inputs");
    sortInputs(m);
    return m;
}

// Echo: the old JSON ({"FPS", "Starting Frame", "Echo Replay": [{"Frame", "Hold", "Player 2"}]}) or the binary one
// ("META", type, TPS (f32) at byte 24, inputs from byte 48: u32 frame, u8 down, u8 player (0 = player 1), plus 18
// bytes of position in a debug file ("DBG" type)).
std::optional<MacroFile> parseEchoJson(MVal const& root, std::string* error) {
    auto list = root.get("Echo Replay");
    if (!list || list->kind != MVal::Kind::Array) return macroFail(error, "echo", "no \"Echo Replay\" list");
    MacroFile m;
    m.format = "Echo JSON";
    m.botName = "Echo";
    if (auto v = root.get("FPS"); v && v->isNumber()) m.framerate = v->number(240.0);
    std::int64_t start = 0;
    if (auto v = root.get("Starting Frame"); v && v->isNumber()) start = v->integer();
    for (auto const& a : list->arr) {
        auto frame = a.get("Frame");
        if (!frame || !frame->isNumber()) continue;
        bool p2 = false, hold = true;
        if (auto v = a.get("Player 2")) p2 = v->boolean();
        if (auto v = a.get("Hold")) hold = v->boolean(true);
        m.inputs.push_back(makeInput(frame->integer() + start, 1, p2, hold));
    }
    sortInputs(m);
    if (m.inputs.empty()) return macroFail(error, "echo", "the macro has no inputs");
    return m;
}

std::optional<MacroFile> parseEchoBinary(std::string_view data, std::string* error) {
    if (data.substr(0, 4) != "META") return macroFail(error, "echo", "no META header");
    if (data.size() < 48) return macroFail(error, "echo", "the header is cut short");
    ByteReader r{data, 4};
    std::uint32_t type = r.bigEndian<std::uint32_t>();
    std::size_t size = type == 0x44424700u ? 24 : 6;
    r.p = 24;
    MacroFile m;
    m.format = "Echo binary";
    m.botName = "Echo";
    m.framerate = r.little<float>();
    if (!(m.framerate > 0.0) || m.framerate > 100000.0) m.framerate = 240.0;
    for (std::size_t at = 48; at + size <= data.size(); at += size) {
        r.p = at;
        auto frame = r.little<std::uint32_t>();
        bool down = r.little<std::uint8_t>() == 1;
        bool p2 = r.little<std::uint8_t>() != 0;
        m.inputs.push_back(makeInput(frame, 1, p2, down));
    }
    sortInputs(m);
    if (m.inputs.empty()) return macroFail(error, "echo", "the macro has no inputs");
    return m;
}

// ToastyReplay (.ttr): "TTR\0" (versions 1-6+), "TTR2" and "TTR3". Inputs are ticks plus, in CBS / CBF files, a
// time inside the tick (counted as inputs between ticks, pressed on their tick). Files that keep playing on after
// deaths (persistence) are refused: the run doesn't start from the level start.
namespace {
std::string ttrString(ByteReader& r) {
    auto n = r.little<std::uint16_t>();
    if (r.bad || r.p + n > r.d.size()) {
        r.bad = true;
        return {};
    }
    std::string s(r.d.substr(r.p, n));
    r.p += n;
    return s;
}

int ttrButton(std::uint8_t type) { return type == 2 || type == 3 ? type : 1; }
} // namespace

std::optional<MacroFile> parseTtr(std::string_view data, std::string* error) {
    constexpr std::uint32_t kCbs = 1u << 0, kCbf = 1u << 5, kSubstep = 1u << 6, kExactCbs = 1u << 7, kPersist = 1u << 8;
    MacroFile m;
    m.botName = "ToastyReplay";
    auto magic = data.substr(0, 4);
    ByteReader r{data, 4};
    if (magic == std::string_view("TTR\0", 4) || magic == "TTR2") {
        bool two = magic == "TTR2";
        m.format = two ? "ToastyReplay 2" : "ToastyReplay";
        auto version = r.little<std::uint16_t>();
        auto flags = r.little<std::uint32_t>();
        auto headerSize = r.little<std::uint32_t>();
        m.author = ttrString(r);
        ttrString(r); // replay name
        m.levelName = ttrString(r);
        m.levelId = static_cast<std::uint32_t>(r.little<std::int32_t>());
        m.framerate = r.little<double>();
        m.duration = r.little<double>();
        if (r.bad) return macroFail(error, "ttr", "the header is cut short");
        if (!(m.framerate > 0.0) || m.framerate > 100000.0) m.framerate = 240.0;
        if (two && version > 2) return macroFail(error, "ttr", "ToastyReplay 2 version " + std::to_string(version) + " isn't known");
        if (flags & kPersist) return macroFail(error, "ttr", "this macro carries on after deaths (persistence), so it can't be played from the level start");
        if (headerSize > r.p) r.p = headerSize;
        std::string payload;
        if (!two && version == 1) payload = std::string(data.substr(std::min(r.p, data.size())));
        else {
            r.little<std::uint32_t>(); // uncompressed size
            if (r.bad) return macroFail(error, "ttr", "the inputs are missing");
            std::string why;
            auto out = inflateData(data.substr(r.p), 256u << 20, &why);
            if (!out) return macroFail(error, "ttr", "couldn't unpack the inputs (" + why + ")");
            payload = std::move(*out);
        }
        ByteReader p{payload, 0};
        auto count = p.little<std::uint32_t>();
        if (p.bad || count > payload.size()) return macroFail(error, "ttr", "the input count doesn't fit the file");
        bool timed = (flags & (kCbs | kCbf | kSubstep)) != 0;
        bool exact = !two && version >= 6 && (flags & kExactCbs) != 0;
        bool twoCbs = two && (flags & (kCbs | kCbf)) != 0;
        std::int64_t tick = 0;
        for (std::uint32_t i = 0; i < count && !p.bad; ++i) {
            bool sub = false;
            if (!two && version == 1) {
                tick = p.little<std::int32_t>();
                auto type = p.little<std::uint8_t>();
                auto fl = p.little<std::uint8_t>();
                float off = p.little<float>();
                sub = off > 0.f;
                if (!p.bad) m.inputs.push_back(makeInput(tick, ttrButton(type), fl & 1, fl & 2));
            }
            else {
                tick += static_cast<std::int64_t>(p.varint());
                double cbs = -1.0;
                if (twoCbs) cbs = p.little<double>();
                auto type = p.little<std::uint8_t>();
                auto fl = p.little<std::uint8_t>();
                if (!two && timed) sub = p.little<float>() > 0.f;
                if (exact) sub = p.little<float>() > 0.f || sub;
                if (twoCbs) sub = cbs > 0.0;
                if (!p.bad) m.inputs.push_back(makeInput(tick, ttrButton(type), fl & 1, fl & 2));
            }
            if (sub && !p.bad) ++m.subtickInputs;
        }
        if (p.bad) return macroFail(error, "ttr", "the inputs are cut short");
    }
    else if (magic == "TTR3") {
        constexpr std::uint32_t kHasPersistence = 1u << 7, kCompressed = 1u << 10;
        m.format = "ToastyReplay 3";
        auto version = r.little<std::uint16_t>();
        auto reserved = r.little<std::uint16_t>();
        auto flags = r.little<std::uint32_t>();
        auto headerLen = r.little<std::uint32_t>();
        if (r.bad || version == 0 || reserved != 0) return macroFail(error, "ttr", "not a ToastyReplay 3 header");
        r.little<std::uint64_t>(); // source format
        r.little<std::uint64_t>(); // game version
        m.levelId = static_cast<std::uint32_t>(r.little<std::int32_t>());
        m.levelName = ttrString(r);
        m.author = ttrString(r);
        m.framerate = r.little<double>();
        if (r.bad) return macroFail(error, "ttr", "the header is cut short");
        if (!(m.framerate > 0.0) || m.framerate > 100000.0) m.framerate = 240.0;
        r.p = headerLen;
        auto sections = r.little<std::uint16_t>();
        struct Section { std::uint8_t kind; std::uint64_t offset, size; };
        std::vector<Section> list;
        for (std::uint16_t i = 0; i < sections && !r.bad; ++i) {
            Section sec{};
            sec.kind = r.little<std::uint8_t>();
            r.skip(3);
            sec.offset = r.little<std::uint64_t>();
            sec.size = r.little<std::uint64_t>();
            list.push_back(sec);
        }
        if (r.bad) return macroFail(error, "ttr", "the section list is cut short");
        std::string payload(data.substr(std::min(r.p, data.size())));
        if (flags & kCompressed) {
            std::string why;
            auto out = inflateData(payload, 256u << 20, &why);
            if (!out) return macroFail(error, "ttr", "couldn't unpack the inputs (" + why + ")");
            payload = std::move(*out);
        }
        bool found = false;
        for (auto const& sec : list) {
            if (sec.kind == 5) return macroFail(error, "ttr", "this macro carries on after deaths (persistence), so it can't be played from the level start");
            if (sec.kind != 1) continue;
            if (sec.offset > payload.size() || sec.size > payload.size() - sec.offset) return macroFail(error, "ttr", "a section is outside the file");
            ByteReader p{std::string_view(payload).substr(sec.offset, sec.size), 0};
            auto count = p.little<std::uint64_t>();
            if (p.bad || count > sec.size / 12 + 1) return macroFail(error, "ttr", "the input count doesn't fit the file");
            for (std::uint64_t i = 0; i < count && !p.bad; ++i) {
                double t = p.little<double>();
                auto type = p.little<std::uint8_t>();
                auto fl = p.little<std::uint8_t>();
                p.skip(2);
                if (p.bad || !(t >= 0.0)) continue;
                double exactTick = t * m.framerate;
                auto tick = static_cast<std::int64_t>(std::floor(exactTick + 1e-9));
                if (exactTick - static_cast<double>(tick) > 1e-6) ++m.subtickInputs;
                m.inputs.push_back(makeInput(tick, ttrButton(type), fl & 1, fl & 2));
            }
            if (p.bad) return macroFail(error, "ttr", "the inputs are cut short");
            found = true;
        }
        if (!found && (flags & kHasPersistence)) return macroFail(error, "ttr", "this macro carries on after deaths (persistence)");
    }
    else return macroFail(error, "ttr", "not a ToastyReplay macro");
    sortInputs(m);
    if (m.inputs.empty()) return macroFail(error, "ttr", "the macro has no inputs");
    return m;
}

// TCBot (.tcm): a 16-byte header, 64 bytes of meta (version, ..., TPS or step time as f32 at byte 4; v2: flag 2 =
// it's the TPS), then v1: varint count, (varint frame, input byte) per input; v2: a varint start frame and a stream
// of input bytes, each followed by a frame delta of 0, 1, 2 or 4 bytes (optionally added to the last delta).
// Restarts / deaths start the frames again: only the last attempt is kept. A "swift" input is a press and its
// release on the same frame. As the tcm crate (MIT) reads it.
std::optional<MacroFile> parseTcm(std::string_view data, std::string* error) {
    static constexpr unsigned char kHeader[16] = {0x9f, 0x88, 0x89, 0x84, 0x9f, 0x3b, 0x1d, 0xd8, 0xcc, 0xa1, 0x86, 0x8a, 0x88, 0x99, 0x84, 0x00};
    if (data.size() < 80 || std::memcmp(data.data(), kHeader, 16) != 0) return macroFail(error, "tcm", "no TCBot header");
    MacroFile m;
    m.botName = "TCBot";
    auto version = static_cast<unsigned char>(data[16]);
    unsigned char flags = static_cast<unsigned char>(data[18]);
    float tpsOrDt = 0;
    std::memcpy(&tpsOrDt, data.data() + 20, 4);
    if (version == 1) m.framerate = tpsOrDt;
    else if (version == 2) m.framerate = (flags & 2) ? tpsOrDt : (tpsOrDt > 0.f ? 1.0 / tpsOrDt : 0.0);
    else return macroFail(error, "tcm", "TCBot version " + std::to_string(version) + " isn't known");
    if (!(m.framerate > 0.0) || m.framerate > 100000.0) return macroFail(error, "tcm", "the frame rate isn't readable");
    m.format = "TCBot v" + std::to_string(version);
    ByteReader r{data, 80};
    std::size_t restarts = 0;
    auto restart = [&] {
        m.inputs.clear(); // a new attempt: only the last one is played
        ++restarts;
    };
    if (version == 1) {
        auto count = r.varint();
        if (r.bad || count > data.size()) return macroFail(error, "tcm", "the input count doesn't fit the file");
        for (std::uint64_t i = 0; i < count && !r.bad; ++i) {
            auto frame = r.varint();
            auto b = r.little<std::uint8_t>();
            if (r.bad) break;
            int kind = b & 7;
            if (kind < 3) m.inputs.push_back(makeInput(static_cast<std::int64_t>(std::min<std::uint64_t>(frame, 100000000)), kind + 1, (b & 0x40) != 0, (b & 0x80) != 0));
            else if (kind <= 5) restart();
        }
        if (r.bad) return macroFail(error, "tcm", "the inputs are cut short");
    }
    else {
        std::uint64_t frame = r.varint();
        std::uint64_t lastDelta = 0;
        if (r.bad) return macroFail(error, "tcm", "the inputs are missing");
        while (r.p < data.size()) {
            auto b = r.little<std::uint8_t>();
            int deltaData = (b >> 5) & 7;
            int in = b & 3;
            if (in > 0) {
                bool push = (b & 4) != 0, p2 = (b & 8) != 0;
                auto f = static_cast<std::int64_t>(std::min<std::uint64_t>(frame, 100000000));
                m.inputs.push_back(makeInput(f, in, p2, push));
                if (b & 16) m.inputs.push_back(makeInput(f, in, p2, !push)); // swift: the other half on the same frame
            }
            else {
                int custom = (b >> 2) & 3;
                bool extra = (b & 16) != 0;
                if (custom == 3) {
                    if (!extra) r.skip(4); // a TPS change
                }
                else {
                    if (extra) r.skip(8); // the new seed
                    restart();
                    frame = 0;
                }
            }
            // the frame delta after it
            int bytes = (deltaData >> 1) & 3;
            bool magic = (deltaData & 1) != 0;
            std::uint64_t dt = magic ? lastDelta : 0;
            if (bytes > 0) {
                std::uint64_t v = bytes == 1 ? r.little<std::uint8_t>() : bytes == 2 ? r.little<std::uint16_t>() : r.little<std::uint32_t>();
                if (r.bad) break; // the end of the file
                dt += v;
                if (dt != 0) lastDelta = dt;
            }
            if (r.bad) break;
            frame += dt;
        }
    }
    if (restarts > 0) m.description = std::to_string(restarts) + " restart(s) in the file: the last attempt is used";
    sortInputs(m);
    if (m.inputs.empty()) return macroFail(error, "tcm", "the macro has no inputs");
    return m;
}

// ---- v1.105 (roadmap 5.8): ReplayEngine 1 / 3, slc v3, ToastyReplay Lite. Byte layouts as zcb3 (public domain)
// and slc_oxide (MIT) read them; the code here is our own.
namespace {
// ReplayEngine's C structs: a frame record {u32 frame, f32 x, f32 y, f32 rot, f64 y accel, bool p2} is 32 bytes with
// padding; an action is {u32 frame, bool hold, bool p2} (8, the first v1) or {u32 frame, bool hold, i32 button,
// bool p2} (16).
constexpr std::size_t kReFrame = 32;
MacroInput reAction(ByteReader& r, bool withButton) {
    auto frame = r.little<std::uint32_t>();
    bool hold = r.little<std::uint8_t>() != 0;
    std::int32_t button = 1;
    bool p2 = false;
    if (withButton) {
        r.skip(3);
        button = r.little<std::int32_t>();
        p2 = r.little<std::uint8_t>() != 0;
        r.skip(3);
    }
    else {
        p2 = r.little<std::uint8_t>() != 0;
        r.skip(2);
    }
    return makeInput(frame, button, p2, hold);
}
bool sane(double tps) { return tps > 0.0 && tps <= 100000.0; }
} // namespace

// ReplayEngine 1 (.re): f32 TPS, u32 frame records, u32 actions, the frame records (skipped), the actions (either size:
// told by what's left in the file, like zcb3 does).
std::optional<MacroFile> parseRe1(std::string_view data, std::string* error) {
    ByteReader r{data, 0};
    MacroFile m;
    m.format = "ReplayEngine 1";
    m.botName = "ReplayEngine";
    m.framerate = r.little<float>();
    auto frames = r.little<std::uint32_t>();
    auto actions = r.little<std::uint32_t>();
    if (r.bad || !sane(m.framerate)) return macroFail(error, "re", "not a ReplayEngine macro (no frame rate)");
    if (static_cast<std::uint64_t>(frames) * kReFrame + 12 > data.size()) return macroFail(error, "re", "the frame records don't fit the file");
    r.skip(static_cast<std::uint64_t>(frames) * kReFrame);
    if (actions == 0) return macroFail(error, "re", "the macro has no inputs");
    std::size_t left = data.size() - r.p;
    bool withButton = false;
    if (left == static_cast<std::size_t>(actions) * 16) withButton = true;
    else if (left != static_cast<std::size_t>(actions) * 8) return macroFail(error, "re", "the inputs don't fit the file (an unknown kind of .re)");
    for (std::uint32_t i = 0; i < actions && !r.bad; ++i) {
        auto in = reAction(r, withButton);
        if (!r.bad) m.inputs.push_back(in);
    }
    if (r.bad) return macroFail(error, "re", "the inputs are cut short");
    m.frameFixes = frames;
    sortInputs(m);
    return m;
}

// ReplayEngine 3 (.re3): f32 TPS, u32 counts of player 1 / player 2 frame records and player 1 / player 2 actions,
// then each list ({u32 frame, bool down, i32 button, bool} actions, 16 bytes). Which player is the list's.
std::optional<MacroFile> parseRe3(std::string_view data, std::string* error) {
    ByteReader r{data, 0};
    MacroFile m;
    m.format = "ReplayEngine 3";
    m.botName = "ReplayEngine";
    m.framerate = r.little<float>();
    std::uint64_t n[4];
    for (auto& v : n) v = r.little<std::uint32_t>();
    if (r.bad || !sane(m.framerate)) return macroFail(error, "re3", "not a ReplayEngine 3 macro (no frame rate)");
    if (20 + (n[0] + n[1]) * kReFrame + (n[2] + n[3]) * 16 != data.size()) return macroFail(error, "re3", "the lists don't fit the file");
    r.skip((n[0] + n[1]) * kReFrame);
    for (int list = 0; list < 2; ++list)
        for (std::uint64_t i = 0; i < n[2 + list] && !r.bad; ++i) {
            auto in = reAction(r, true);
            in.p2 = list == 1;
            if (!r.bad) m.inputs.push_back(in);
        }
    if (r.bad) return macroFail(error, "re3", "the inputs are cut short");
    m.frameFixes = n[0] + n[1];
    sortInputs(m);
    if (m.inputs.empty()) return macroFail(error, "re3", "the macro has no inputs");
    return m;
}

// slc v3: "SLC3RPLY", u16 meta size (64), meta {f64 TPS, u64 seed, u32 version, u32 build, 40 spare}, atoms
// {u32 id, u64 size, data} up to a last 0xCC byte. An action atom (id 1): u64 count, then sections with a u16 header
// (top 2 bits: 0 inputs, 1 repeated inputs, 2 special). Inputs: 2^((h >> 8) & 15) states of 2^((h >> 12) & 3) bytes,
// each delta << 4 | button << 2 | p2 << 1 | hold (button 0 = a swift: press and release on the same frame). Repeated:
// those states 2^((h >> 3) & 31) times over. Special (restart 0, full restart 1, death 2, TPS 3 in (h >> 10) & 15):
// a frame delta of 2^((h >> 8) & 3) bytes and 8 bytes (seed or TPS). After a restart or death, the last attempt.
std::optional<MacroFile> parseSlc3(std::string_view data, std::string* error) {
    if (data.substr(0, 8) != "SLC3RPLY") return macroFail(error, "slc3", "no SLC3RPLY header");
    if (data.empty() || static_cast<unsigned char>(data.back()) != 0xCC) return macroFail(error, "slc3", "the file is cut short (no end mark)");
    ByteReader r{data.substr(0, data.size() - 1), 8};
    MacroFile m;
    m.format = "slc v3";
    m.botName = "Silicate";
    if (r.little<std::uint16_t>() != 64) return macroFail(error, "slc3", "unknown header size");
    m.framerate = r.little<double>();
    r.skip(8 + 4 + 4 + 40);
    if (r.bad || !sane(m.framerate)) return macroFail(error, "slc3", "the header is cut short");
    constexpr std::uint64_t kMax = 4000000;
    std::uint64_t frame = 0, cutAt = 0;
    std::size_t restarts = 0, cutFrom = 0, swifts = 0;
    bool tpsChanged = false;
    auto readN = [&](unsigned bytes) {
        std::uint64_t v = 0;
        for (unsigned i = 0; i < bytes; ++i) v |= static_cast<std::uint64_t>(r.little<std::uint8_t>()) << (8 * i);
        return v;
    };
    auto add = [&](std::uint64_t state) {
        frame += state >> 4;
        int button = static_cast<int>((state >> 2) & 3);
        bool p2 = (state & 2) != 0, hold = (state & 1) != 0;
        auto f = static_cast<std::int64_t>(std::min<std::uint64_t>(frame, 100000000));
        if (button == 0) {
            ++swifts;
            m.inputs.push_back(makeInput(f, 1, p2, true));
            m.inputs.push_back(makeInput(f, 1, p2, false));
        }
        else m.inputs.push_back(makeInput(f, button, p2, hold));
    };
    while (!r.bad && r.p < r.d.size()) {
        auto id = r.little<std::uint32_t>();
        auto size = r.little<std::uint64_t>();
        if (r.bad) break;
        if (id != 1) {
            r.skip(size); // null / marker atoms
            continue;
        }
        auto count = r.little<std::uint64_t>();
        std::uint64_t got = 0;
        while (!r.bad && got < count && m.inputs.size() < kMax) {
            auto h = r.little<std::uint16_t>();
            int kind = h >> 14;
            if (kind == 0 || kind == 1) {
                unsigned bytes = 1u << ((h >> 12) & 3);
                std::uint64_t length = 1ull << ((h >> 8) & 15);
                std::uint64_t repeats = kind == 1 ? 1ull << ((h >> 3) & 31) : 1;
                std::vector<std::uint64_t> states;
                for (std::uint64_t i = 0; i < length && !r.bad; ++i) states.push_back(readN(bytes));
                for (std::uint64_t k = 0; k < repeats && !r.bad && m.inputs.size() < kMax; ++k)
                    for (auto st : states) {
                        add(st);
                        got += ((st >> 2) & 3) == 0 ? 2 : 1;
                    }
            }
            else if (kind == 2) {
                unsigned bytes = 1u << ((h >> 8) & 3);
                int type = (h >> 10) & 15;
                frame += readN(bytes);
                double extra = r.little<double>();
                ++got;
                if (type == 3) tpsChanged = tpsChanged || extra != m.framerate;
                else {
                    ++restarts;
                    cutFrom = m.inputs.size();
                    cutAt = frame;
                }
            }
            else return macroFail(error, "slc3", "unknown section");
        }
    }
    if (r.bad) return macroFail(error, "slc3", "the inputs are cut short");
    if (m.inputs.size() >= kMax) return macroFail(error, "slc3", "too many inputs");
    if (restarts > 0) {
        m.inputs.erase(m.inputs.begin(), m.inputs.begin() + static_cast<std::ptrdiff_t>(cutFrom));
        for (auto& in : m.inputs) in.frame -= static_cast<int>(std::min<std::uint64_t>(cutAt, static_cast<std::uint64_t>(in.frame)));
        m.description = std::to_string(restarts) + " restart(s) / death(s) in the file: the last attempt is used";
    }
    if (swifts > 0) m.description += (m.description.empty() ? "" : "; ") + std::to_string(swifts) + " swift click(s) (press and release on one frame)";
    if (tpsChanged) m.description += (m.description.empty() ? "" : "; ") + std::string("the TPS changes in the file: only the first is used");
    sortInputs(m);
    if (m.inputs.empty()) return macroFail(error, "slc3", "the macro has no inputs");
    return m;
}

namespace {
std::uint32_t crc32(std::string_view d) {
    std::uint32_t c = 0xFFFFFFFFu;
    for (unsigned char b : d) {
        c ^= b;
        for (int k = 0; k < 8; ++k) c = (c & 1) ? (c >> 1) ^ 0xEDB88320u : c >> 1;
    }
    return ~c;
}
} // namespace

// ToastyReplay Lite: "TTRL", u8 version 1, u8 flags (1 frame fixes, 2 input channels, 4 platformer, 8 seed, 16 Start
// Pos, 32 flipped controls), varints TPS numerator / denominator, game version, level id, revision, a u64, tick count,
// [seed], [f32 Start Pos], [u8 flip], then a varint byte count of inputs: varints delta << 4 | state (channels: button
// - 1 in bits 2-3, p2 bit 1, down bit 0) or delta << 1 | down. Frame fixes follow (counted, not read). A CRC-32 of
// everything before it ends the file.
std::optional<MacroFile> parseTtrl(std::string_view data, std::string* error) {
    if (data.substr(0, 4) != "TTRL") return macroFail(error, "ttrl", "no TTRL header");
    if (data.size() < 25) return macroFail(error, "ttrl", "the file is too small");
    std::uint32_t stored = 0;
    std::memcpy(&stored, data.data() + data.size() - 4, 4);
    auto body = data.substr(0, data.size() - 4);
    if (crc32(body) != stored) return macroFail(error, "ttrl", "the checksum doesn't match (the file is damaged)");
    ByteReader r{body, 4};
    MacroFile m;
    m.format = "ToastyReplay Lite";
    m.botName = "ToastyReplay";
    if (r.little<std::uint8_t>() != 1) return macroFail(error, "ttrl", "unknown TTRL version");
    auto flags = r.little<std::uint8_t>();
    if (flags & ~0x3F) return macroFail(error, "ttrl", "the file uses features this mod doesn't know");
    bool channels = flags & 2;
    auto num = r.varint(), den = r.varint();
    if (r.bad || num == 0 || den == 0) return macroFail(error, "ttrl", "no TPS");
    m.framerate = static_cast<double>(num) / static_cast<double>(den);
    (void)r.varint(); // game version
    m.levelId = static_cast<std::uint32_t>(std::min<std::uint64_t>(r.varint(), 0xFFFFFFFFu));
    (void)r.varint(); // revision
    r.skip(8);        // fingerprint
    auto ticks = r.varint();
    if (flags & 8) (void)r.varint();
    if (flags & 16) r.skip(4);
    if (flags & 32) r.skip(1);
    auto size = r.varint();
    if (r.bad || !sane(m.framerate) || r.p + size > body.size()) return macroFail(error, "ttrl", "the header is cut short");
    std::size_t end = r.p + size;
    std::uint64_t tick = 0;
    while (!r.bad && r.p < end) {
        auto ev = r.varint();
        std::uint64_t state = channels ? ev & 15 : ev & 1;
        tick += ev >> (channels ? 4 : 1);
        if (tick >= ticks) return macroFail(error, "ttrl", "an input is past the end of the replay");
        int button = channels ? static_cast<int>((state >> 2) + 1) : 1;
        if (button > 3) return macroFail(error, "ttrl", "an input has an unknown button");
        m.inputs.push_back(makeInput(static_cast<std::int64_t>(std::min<std::uint64_t>(tick, 100000000)), button,
            channels && (state & 2), (state & 1) != 0));
        if (m.inputs.size() > 1048576) return macroFail(error, "ttrl", "too many inputs");
    }
    if (r.bad) return macroFail(error, "ttrl", "the inputs are cut short");
    if (flags & 1) {
        r.p = end;
        (void)r.little<std::uint8_t>(); // schema
        (void)r.varint();               // section size
        auto fixes = r.varint();
        if (!r.bad) m.frameFixes = static_cast<std::size_t>(std::min<std::uint64_t>(fixes, 1u << 20));
    }
    m.duration = m.framerate > 0 ? static_cast<double>(ticks) / m.framerate : 0;
    sortInputs(m);
    if (m.inputs.empty()) return macroFail(error, "ttrl", "the macro has no inputs");
    return m;
}

std::optional<MacroFile> parseMacro(std::string_view data, std::string* error, std::string_view extension) {
    if (data.size() >= 4 && (data.substr(0, 4) == std::string_view("\xd7\x8a\x3e\x91", 4)
        || data.substr(0, 4) == std::string_view("CML\0", 4))) {
        return parseCml(data, error);
    }
    if (data.substr(0, 4) == "SILL") return parseSlc2(data, error);
    // v1.97
    if (data.substr(0, 3) == "RE4") return parseRe4(data, error);
    if (data.substr(0, 3) == "RE2") return parseRe2(data, error);
    if (data.substr(0, 4) == std::string_view("TTR\0", 4) || data.substr(0, 4) == "TTR2" || data.substr(0, 4) == "TTR3")
        return parseTtr(data, error);
    if (data.substr(0, 4) == "META") return parseEchoBinary(data, error);
    if (data.size() >= 2 && static_cast<unsigned char>(data[0]) == 0x9f && static_cast<unsigned char>(data[1]) == 0x88)
        return parseTcm(data, error);
    if (extension == ".xd" || (!data.empty() && data[0] >= '0' && data[0] <= '9' && data.find('|') != std::string_view::npos
        && data.find('|') > data.find('\n'))) {
        return parseXdText(data, error);
    }
    if (data.substr(0, 8) == "SLC3RPLY") return parseSlc3(data, error); // v1.105
    if (data.substr(0, 4) == "TTRL") return parseTtrl(data, error);       // v1.105
    if (extension == ".re3") return parseRe3(data, error);                 // v1.105 (no header)
    if (extension == ".re") return parseRe1(data, error);
    if (extension == ".slc") return parseSlc1(data, error); // v1.97: Silicate's first format has no header
    if (data.substr(0, 3) == "GDR") {
        std::string e;
        if (auto m = parseGdr2(data, &e)) return m;
        if (error) *error = e;
        return std::nullopt;
    }
    std::string e1, e2;
    std::string format = "GDR";
    auto root = parseMsgpack(data, &e1);
    if (!root || root->kind != MVal::Kind::Map) {
        root = parseJson(data, &e2);
        format = "GDR JSON";
    }
    if (!root) {
        if (error) *error = "not a GDR macro (neither MessagePack: " + e1 + ", nor JSON: " + e2 + ")";
        return std::nullopt;
    }
    if (root->kind != MVal::Kind::Map) {
        if (error) *error = "not a GDR macro (no fields at the top level)";
        return std::nullopt;
    }
    if (root->get("Echo Replay")) return parseEchoJson(*root, error); // v1.97

    MacroFile m;
    m.format = format;
    if (auto bot = root->get("bot")) {
        if (auto v = bot->get("name")) m.botName = v->string();
        if (auto v = bot->get("version")) m.botVersion = v->string();
    }
    if (auto lvl = root->get("level")) {
        if (auto v = lvl->get("name")) m.levelName = v->string();
        if (auto v = lvl->get("id")) m.levelId = static_cast<std::uint32_t>(v->integer());
    }
    if (auto v = root->get("author")) m.author = v->string();
    if (auto v = root->get("description")) m.description = v->string();
    if (auto v = root->get("framerate"); v && v->isNumber()) m.framerate = v->number(240.0);
    if (auto v = root->get("duration"); v && v->isNumber()) m.duration = v->number();
    if (auto v = root->get("frameFixes"); v && v->kind == MVal::Kind::Array) m.frameFixes = v->arr.size();
    m.versionOffset = m.isXdBot() ? xdBotFrameOffset(m.botVersion) : 0;
    m.xdBotStyle = m.isXdBot();

    auto inputs = root->get("inputs");
    if (!inputs || inputs->kind != MVal::Kind::Array) {
        if (error) *error = "the macro has no inputs list";
        return std::nullopt;
    }
    for (auto const& in : inputs->arr) {
        auto frame = in.get("frame");
        if (!frame || !frame->isNumber()) continue; // as xdBot: inputs without a frame are skipped
        MacroInput mi;
        mi.frame = static_cast<int>(frame->integer()) + m.versionOffset;
        if (auto v = in.get("btn")) mi.button = static_cast<int>(v->integer(1));
        if (auto v = in.get("2p")) mi.p2 = v->boolean();
        if (auto v = in.get("down")) mi.down = v->boolean(true);
        if (mi.button < 1 || mi.button > 3 || mi.frame < 0) continue;
        m.inputs.push_back(mi);
    }
    std::stable_sort(m.inputs.begin(), m.inputs.end(),
        [](MacroInput const& a, MacroInput const& b) { return a.frame < b.frame; });
    if (m.inputs.empty()) {
        if (error) *error = "the macro has no inputs";
        return std::nullopt;
    }
    return m;
}

std::string macroDescription(MacroFile const& m) {
    std::size_t presses = std::count_if(m.inputs.begin(), m.inputs.end(), [](MacroInput const& i) { return i.down; });
    std::ostringstream ss;
    if (!m.format.empty()) ss << m.format << ", ";
    ss << (m.botName.empty() ? std::string("unknown bot") : m.botName);
    if (!m.botVersion.empty()) ss << " " << m.botVersion;
    ss << ", " << m.inputs.size() << " inputs (" << presses << " presses), ";
    if (std::fabs(m.framerate - std::round(m.framerate)) < 1e-6) ss << static_cast<long long>(std::llround(m.framerate));
    else ss << m.framerate;
    ss << " TPS";
    if (!m.levelName.empty()) {
        ss << ", level \"" << m.levelName << "\"";
        if (m.levelId) ss << " (" << m.levelId << ")";
    }
    if (m.frameFixes) ss << ", " << m.frameFixes << " frame fixes";
    if (m.subtickInputs) ss << ", " << m.subtickInputs << " inputs between ticks (CBF)";
    return ss.str();
}

// ---- inflate (RFC 1951), with gzip (RFC 1952) / zlib (RFC 1950) headers -----------------

namespace {

struct Huffman {
    std::uint16_t count[16]{};
    std::uint16_t symbol[320]{};
};

class Inflater {
public:
    Inflater(std::string_view in, std::size_t pos, std::size_t maxOut) : m_in(in), m_pos(pos), m_max(maxOut) {}

    bool run(std::string& out, std::string& err) {
        while (true) {
            std::uint32_t last, type;
            if (!bits(1, last) || !bits(2, type)) return fail(err, "the compressed data ends early");
            bool ok = false;
            if (type == 0) ok = stored(out, err);
            else if (type == 1) ok = fixedBlock(out, err);
            else if (type == 2) ok = dynamicBlock(out, err);
            else return fail(err, "bad compressed block type");
            if (!ok) return false;
            if (last) return true;
        }
    }

private:
    std::string_view m_in;
    std::size_t m_pos;
    std::size_t m_max;
    std::uint32_t m_buf = 0;
    int m_cnt = 0;

    static bool fail(std::string& err, char const* what) {
        if (err.empty()) err = what;
        return false;
    }

    bool bits(int n, std::uint32_t& v) {
        while (m_cnt < n) {
            if (m_pos >= m_in.size()) return false;
            m_buf |= static_cast<std::uint32_t>(static_cast<unsigned char>(m_in[m_pos++])) << m_cnt;
            m_cnt += 8;
        }
        v = n == 0 ? 0 : (m_buf & ((1u << n) - 1));
        m_buf = n >= 32 ? 0 : (m_buf >> n);
        m_cnt -= n;
        return true;
    }

    // Canonical Huffman table from code lengths; false if over-subscribed. Incomplete codes are
    // allowed (as zlib allows them for single-code distance tables).
    static bool build(Huffman& h, std::uint8_t const* lengths, int n) {
        for (auto& c : h.count) c = 0;
        for (int s = 0; s < n; ++s) h.count[lengths[s]]++;
        if (h.count[0] == n) return true; // no codes
        int left = 1;
        for (int len = 1; len < 16; ++len) {
            left <<= 1;
            left -= h.count[len];
            if (left < 0) return false;
        }
        std::uint16_t offs[16]{};
        for (int len = 1; len < 15; ++len) offs[len + 1] = offs[len] + h.count[len];
        for (int s = 0; s < n; ++s)
            if (lengths[s] != 0) h.symbol[offs[lengths[s]]++] = static_cast<std::uint16_t>(s);
        return true;
    }

    bool decode(Huffman const& h, int& sym) {
        int code = 0, first = 0, index = 0;
        for (int len = 1; len < 16; ++len) {
            std::uint32_t b;
            if (!bits(1, b)) return false;
            code |= static_cast<int>(b);
            int count = h.count[len];
            if (code - count < first) {
                sym = h.symbol[index + (code - first)];
                return true;
            }
            index += count;
            first += count;
            first <<= 1;
            code <<= 1;
        }
        return false;
    }

    bool stored(std::string& out, std::string& err) {
        m_buf = 0; // to the next byte boundary
        m_cnt = 0;
        if (m_in.size() - m_pos < 4) return fail(err, "the compressed data ends early");
        auto b = [&](std::size_t k) { return static_cast<unsigned>(static_cast<unsigned char>(m_in[m_pos + k])); };
        unsigned len = b(0) | (b(1) << 8);
        unsigned nlen = b(2) | (b(3) << 8);
        if (len != (~nlen & 0xffffu)) return fail(err, "bad stored block");
        m_pos += 4;
        if (m_in.size() - m_pos < len) return fail(err, "the compressed data ends early");
        if (out.size() + len > m_max) return fail(err, "it decompresses to more than the size limit");
        out.append(m_in.substr(m_pos, len));
        m_pos += len;
        return true;
    }

    bool codes(std::string& out, std::string& err, Huffman const& lencode, Huffman const& distcode) {
        static constexpr std::uint16_t lbase[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51,
            59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
        static constexpr std::uint8_t lext[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4,
            5, 5, 5, 5, 0};
        static constexpr std::uint16_t dbase[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385,
            513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
        static constexpr std::uint8_t dext[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10,
            10, 11, 11, 12, 12, 13, 13};
        while (true) {
            int sym;
            if (!decode(lencode, sym)) return fail(err, "bad compressed data (length code)");
            if (sym < 256) {
                if (out.size() >= m_max) return fail(err, "it decompresses to more than the size limit");
                out += static_cast<char>(sym);
                continue;
            }
            if (sym == 256) return true;
            sym -= 257;
            if (sym >= 29) return fail(err, "bad compressed data (length)");
            std::uint32_t extra;
            if (!bits(lext[sym], extra)) return fail(err, "the compressed data ends early");
            std::size_t len = lbase[sym] + extra;
            int dsym;
            if (!decode(distcode, dsym) || dsym >= 30) return fail(err, "bad compressed data (distance code)");
            if (!bits(dext[dsym], extra)) return fail(err, "the compressed data ends early");
            std::size_t dist = dbase[dsym] + extra;
            if (dist > out.size()) return fail(err, "bad compressed data (distance too far back)");
            if (out.size() + len > m_max) return fail(err, "it decompresses to more than the size limit");
            std::size_t from = out.size() - dist;
            for (std::size_t k = 0; k < len; ++k) out += out[from + k];
        }
    }

    bool fixedBlock(std::string& out, std::string& err) {
        static Huffman lencode, distcode;
        static bool built = false;
        if (!built) {
            std::uint8_t lengths[288];
            int s = 0;
            for (; s < 144; ++s) lengths[s] = 8;
            for (; s < 256; ++s) lengths[s] = 9;
            for (; s < 280; ++s) lengths[s] = 7;
            for (; s < 288; ++s) lengths[s] = 8;
            build(lencode, lengths, 288);
            for (s = 0; s < 30; ++s) lengths[s] = 5;
            build(distcode, lengths, 30);
            built = true;
        }
        return codes(out, err, lencode, distcode);
    }

    bool dynamicBlock(std::string& out, std::string& err) {
        static constexpr std::uint8_t order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
        std::uint32_t nlen, ndist, ncode;
        if (!bits(5, nlen) || !bits(5, ndist) || !bits(4, ncode)) return fail(err, "the compressed data ends early");
        nlen += 257;
        ndist += 1;
        ncode += 4;
        if (nlen > 286 || ndist > 30) return fail(err, "bad compressed data (table sizes)");
        std::uint8_t lengths[320]{};
        for (std::uint32_t k = 0; k < ncode; ++k) {
            std::uint32_t v;
            if (!bits(3, v)) return fail(err, "the compressed data ends early");
            lengths[order[k]] = static_cast<std::uint8_t>(v);
        }
        Huffman lencode, distcode;
        if (!build(lencode, lengths, 19)) return fail(err, "bad compressed data (code lengths)");
        std::uint32_t idx = 0;
        while (idx < nlen + ndist) {
            int sym;
            if (!decode(lencode, sym)) return fail(err, "bad compressed data (code lengths)");
            if (sym < 16) {
                lengths[idx++] = static_cast<std::uint8_t>(sym);
                continue;
            }
            std::uint8_t len = 0;
            std::uint32_t rep;
            if (sym == 16) {
                if (idx == 0) return fail(err, "bad compressed data (repeat with nothing before)");
                len = lengths[idx - 1];
                if (!bits(2, rep)) return fail(err, "the compressed data ends early");
                rep += 3;
            }
            else if (sym == 17) {
                if (!bits(3, rep)) return fail(err, "the compressed data ends early");
                rep += 3;
            }
            else {
                if (!bits(7, rep)) return fail(err, "the compressed data ends early");
                rep += 11;
            }
            if (idx + rep > nlen + ndist) return fail(err, "bad compressed data (too many lengths)");
            while (rep--) lengths[idx++] = len;
        }
        if (lengths[256] == 0) return fail(err, "bad compressed data (no end code)");
        if (!build(lencode, lengths, static_cast<int>(nlen))) return fail(err, "bad compressed data (literal table)");
        if (!build(distcode, lengths + nlen, static_cast<int>(ndist))) return fail(err, "bad compressed data (distance table)");
        return codes(out, err, lencode, distcode);
    }
};

} // namespace

std::optional<std::string> inflateData(std::string_view data, std::size_t maxOut, std::string* error) {
    std::string err;
    std::size_t pos = 0;
    auto byte = [&](std::size_t k) { return static_cast<unsigned char>(data[k]); };
    if (data.size() >= 10 && byte(0) == 0x1f && byte(1) == 0x8b) {
        // gzip: fixed header, then the optional fields its flags announce
        if (byte(2) != 8) err = "not deflate-compressed";
        unsigned flags = byte(3);
        pos = 10;
        if (err.empty() && (flags & 4)) {
            if (data.size() - pos < 2) err = "the gzip header ends early";
            else {
                std::size_t xlen = byte(pos) | (byte(pos + 1) << 8);
                pos += 2 + xlen;
            }
        }
        for (unsigned bit : {8u, 16u}) {
            if (!err.empty() || !(flags & bit)) continue;
            while (pos < data.size() && data[pos] != 0) ++pos;
            ++pos; // its zero
        }
        if (err.empty() && (flags & 2)) pos += 2;
        if (err.empty() && pos > data.size()) err = "the gzip header ends early";
    }
    else if (data.size() >= 2 && (byte(0) & 0x0f) == 8 && ((byte(0) << 8) | byte(1)) % 31 == 0) {
        if (byte(1) & 0x20) err = "zlib data with a preset dictionary isn't supported";
        pos = 2;
    }
    std::string out;
    if (err.empty()) {
        Inflater inf(data, pos, maxOut);
        inf.run(out, err);
    }
    if (!err.empty()) {
        if (error) *error = err;
        return std::nullopt;
    }
    return out;
}

// ---- CML (xdBot's compressed macros) ------------------------------------------------------

namespace {

// Strings are XORed with this per-byte key in files that start with the binary magic.
std::uint8_t cmlStringKey(std::uint64_t i) {
    return static_cast<std::uint8_t>((i * 0x3d + 0xa7 + ((i >> 1) * 0x11)) & 0xff);
}

std::int64_t zigzag(std::uint64_t v) {
    return static_cast<std::int64_t>(v >> 1) ^ -static_cast<std::int64_t>(v & 1);
}

// Versions 1-3 and 5-6: bytes, little-endian floats, LEB128 varints.
struct CmlBytes {
    std::string_view d;
    std::size_t p = 0;
    bool encoded = true;
    bool ok = true;

    std::uint8_t u8() {
        if (p >= d.size()) { ok = false; return 0; }
        return static_cast<std::uint8_t>(d[p++]);
    }
    float f32() {
        if (d.size() - std::min(p, d.size()) < 4) { ok = false; p = d.size(); return 0.f; }
        std::uint32_t bits = 0;
        for (int k = 0; k < 4; ++k) bits |= static_cast<std::uint32_t>(static_cast<std::uint8_t>(d[p + k])) << (8 * k);
        p += 4;
        float f;
        std::memcpy(&f, &bits, 4);
        return f;
    }
    std::uint64_t varU() {
        std::uint64_t v = 0;
        for (int shift = 0; shift < 64 && ok; shift += 7) {
            auto b = u8();
            v |= static_cast<std::uint64_t>(b & 0x7f) << shift;
            if (!(b & 0x80)) return v;
        }
        ok = false;
        return v;
    }
    std::int64_t varI() { return zigzag(varU()); }
    std::string str() {
        auto len = varU();
        if (!ok || len > d.size() - std::min(p, d.size())) { ok = false; return {}; }
        std::string s(d.substr(p, static_cast<std::size_t>(len)));
        p += static_cast<std::size_t>(len);
        if (encoded) for (std::size_t i = 0; i < s.size(); ++i) s[i] = static_cast<char>(s[i] ^ cmlStringKey(i));
        return s;
    }
};

// Version 7: one bit stream (most significant bit first); varints are 1 continue bit + 7 bits,
// low groups first.
struct CmlBits {
    std::string_view d;
    std::size_t p = 0;
    std::uint8_t buf = 0;
    int left = 0;
    bool encoded = true;
    bool ok = true;

    bool bit() {
        if (left == 0) {
            if (p >= d.size()) { ok = false; return false; }
            buf = static_cast<std::uint8_t>(d[p++]);
            left = 8;
        }
        --left;
        return (buf >> left) & 1;
    }
    std::uint64_t bitsN(int n) {
        std::uint64_t v = 0;
        for (int k = 0; k < n && ok; ++k) v = (v << 1) | (bit() ? 1u : 0u);
        return v;
    }
    std::uint8_t u8() { return static_cast<std::uint8_t>(bitsN(8)); }
    float f32() {
        auto bits = static_cast<std::uint32_t>(bitsN(32));
        float f;
        std::memcpy(&f, &bits, 4);
        return f;
    }
    std::uint64_t varU() {
        std::uint64_t v = 0;
        for (int shift = 0; ok; shift += 7) {
            if (shift >= 64) { ok = false; break; }
            bool more = bit();
            v |= bitsN(7) << shift;
            if (!more) return v;
        }
        return v;
    }
    std::int64_t varI() { return zigzag(varU()); }
    std::string str() {
        auto len = varU();
        // each byte takes 8 bits: a bad length can't make us allocate much
        if (!ok || len > (d.size() - std::min(p, d.size())) + 1) { ok = false; return {}; }
        std::string s;
        s.reserve(static_cast<std::size_t>(len));
        for (std::uint64_t i = 0; i < len && ok; ++i) {
            auto b = u8();
            if (encoded) b ^= cmlStringKey(i);
            s += static_cast<char>(b);
        }
        return s;
    }
};

// The part after the header, the same fields in every version; R is CmlBytes or CmlBits.
template <class R>
bool readCmlBody(R& r, int version, MacroFile& m, std::string& err) {
    m.author = r.str();
    m.description = r.str();
    r.f32(); // accuracy
    m.duration = r.f32();
    r.f32(); // speedhack
    m.framerate = r.f32();
    r.varI();
    r.varI();
    if constexpr (std::is_same_v<R, CmlBits>) r.bit();
    else r.u8();
    r.varI(); // last frame
    m.botName = r.str();
    m.botVersion = r.str();
    r.varU(); // seed
    m.macroName = r.str();
    auto count = r.varU();
    if (!r.ok) {
        err = "the macro's header is cut short";
        return false;
    }
    // every input takes at least a byte: a bad count can't make us allocate much
    if (count > r.d.size()) {
        err = "the macro says it has more inputs than fit in the file";
        return false;
    }
    bool subtick = version >= 5;
    constexpr std::int64_t kScale = 1'000'000;
    std::int64_t t = 0;
    m.inputs.reserve(static_cast<std::size_t>(count));
    for (std::uint64_t k = 0; k < count && r.ok; ++k) {
        std::int64_t delta;
        if constexpr (std::is_same_v<R, CmlBits>) delta = r.bit() ? kScale : r.varI();
        else delta = r.varI();
        t += delta;
        auto flags = r.u8();
        if (!r.ok) break;
        if (t < 0) {
            err = "an input's frame became negative";
            return false;
        }
        std::int64_t frame = subtick ? t / kScale : t;
        if (subtick && t % kScale != 0) ++m.subtickInputs;
        if (frame > 100'000'000) {
            err = "an input's frame is far too large";
            return false;
        }
        MacroInput in;
        in.frame = static_cast<int>(frame);
        if (subtick) {
            // (xdBot fork) kept to the file's own 1/1,000,000 of a frame: sub in 1/1000, fine in 1/10,000 of that
            auto const rest = static_cast<int>(t % kScale);
            in.sub = rest / 1000;
            in.fine = (rest % 1000) * 10;
        }
        in.down = flags & 0x01;
        in.p2 = flags & 0x02;
        int idx = (flags >> 2) & 0x0f;
        in.button = idx == 2 || idx == 3 ? idx : 1;
        m.inputs.push_back(in);
    }
    if (!r.ok) {
        err = "the macro's inputs are cut short";
        return false;
    }
    m.frameFixes = static_cast<std::size_t>(std::min<std::uint64_t>(r.varU(), 100'000'000));
    return true;
}

} // namespace

std::optional<MacroFile> parseCml(std::string_view data, std::string* error) {
    auto fail = [&](std::string msg) -> std::optional<MacroFile> {
        if (error) *error = std::move(msg);
        return std::nullopt;
    };
    if (data.size() < 5) return fail("the file is too short to be a CML macro");
    bool encoded;
    if (data.substr(0, 4) == std::string_view("\xd7\x8a\x3e\x91", 4)) encoded = true;
    else if (data.substr(0, 4) == std::string_view("CML\0", 4)) encoded = false;
    else return fail("not a CML macro");

    CmlBytes head{data, 4, encoded};
    auto version = head.varU();
    if (!head.ok || !((version >= 1 && version <= 3) || (version >= 5 && version <= 7)))
        return fail("unsupported CML version " + std::to_string(version) + " (versions 1-3 and 5-7 can be read)");

    MacroFile m;
    m.format = "CML v" + std::to_string(version);
    m.xdBotStyle = true;
    std::string err;
    std::string body;
    if (version >= 5) {
        auto declared = head.varU();
        if (!head.ok) return fail("the CML header is cut short");
        constexpr std::size_t kMax = 256u * 1024u * 1024u;
        std::size_t limit = declared > 0 && declared < kMax ? static_cast<std::size_t>(declared) + 1024 : kMax;
        auto out = inflateData(data.substr(head.p), limit, &err);
        if (!out) return fail("couldn't decompress the macro: " + err);
        body = std::move(*out);
    }
    bool ok;
    if (version == 7) {
        CmlBits r{body, 0, 0, 0, encoded};
        ok = readCmlBody(r, 7, m, err);
    }
    else if (version >= 5) {
        CmlBytes r{body, 0, encoded};
        ok = readCmlBody(r, static_cast<int>(version), m, err);
    }
    else {
        CmlBytes r{data, head.p, encoded};
        ok = readCmlBody(r, static_cast<int>(version), m, err);
    }
    if (!ok) return fail(err);
    std::stable_sort(m.inputs.begin(), m.inputs.end(),
        [](MacroInput const& a, MacroInput const& b) { return a.frame < b.frame; });
    if (m.inputs.empty()) return fail("the macro has no inputs");
    if (!(m.framerate > 0.f) || !std::isfinite(m.framerate)) m.framerate = 240.0;
    return m;
}

std::size_t macroReleaseOf(MacroFile const& m, std::size_t press) {
    if (press >= m.inputs.size() || !m.inputs[press].down) return std::string::npos;
    auto const& p = m.inputs[press];
    for (std::size_t j = press + 1; j < m.inputs.size(); ++j) {
        auto const& in = m.inputs[j];
        if (in.button != p.button || in.p2 != p.p2) continue;
        return in.down ? std::string::npos : j;
    }
    return std::string::npos;
}

std::vector<MacroRetime> macroRetimeCandidates(MacroFile const& m, std::vector<int> const& base, std::size_t played,
    std::size_t back, int maxAdjust) {
    std::vector<MacroRetime> out;
    auto const n = m.inputs.size();
    if (n == 0) return out;
    std::vector<int> adj = base;
    adj.resize(n, 0);
    auto inOrder = [&](std::vector<int> const& a) {
        for (std::size_t i = 1; i < n; ++i)
            if (m.inputs[i].frame + a[i] < m.inputs[i - 1].frame + a[i - 1]) return false;
        return true;
    };
    auto tryOne = [&](std::size_t i, int d) {
        auto a = adj;
        a[i] += d;
        if (std::abs(a[i]) > maxAdjust) return;
        auto rel = macroReleaseOf(m, i);
        if (rel != std::string::npos) {
            a[rel] += d;
            if (std::abs(a[rel]) > maxAdjust) return;
        }
        if (!inOrder(a)) return;
        for (auto const& c : out) if (c.adjust == a) return;
        out.push_back({std::move(a), i, d});
    };
    if (played < n) for (int d : {-1, -2}) tryOne(played, d);
    for (std::size_t k = 1; k <= back && k <= played && played - k < n; ++k)
        for (int d : {-1, 1, -2, 2}) tryOne(played - k, d);
    return out;
}


bool macroOrderOk(MacroFile const& m, std::vector<int> const& adjust, std::vector<int> const& sub) {
    auto const n = m.inputs.size();
    auto adj = [&](std::size_t i) { return i < adjust.size() ? adjust[i] : 0; };
    auto sb = [&](std::size_t i) { return i < sub.size() ? sub[i] : 0; };
    for (std::size_t i = 1; i < n; ++i)
        if (m.inputs[i].frame + adj(i) < m.inputs[i - 1].frame + adj(i - 1)) return false;
    for (std::size_t i = 0; i < n; ++i) {
        if (sb(i) < 0 || sb(i) >= kMacroSubUnits) return false;
        for (std::size_t j = i + 1; j < n; ++j) {
            if (m.inputs[j].button != m.inputs[i].button || m.inputs[j].p2 != m.inputs[i].p2) continue;
            // only between ticks is the order on one button strict (the file may press and release on one frame)
            if (sb(i) > 0 || sb(j) > 0) {
                long long a = (static_cast<long long>(m.inputs[i].frame) + adj(i)) * kMacroSubUnits + sb(i);
                long long b = (static_cast<long long>(m.inputs[j].frame) + adj(j)) * kMacroSubUnits + sb(j);
                if (b <= a) return false;
            }
            break;
        }
    }
    return true;
}

std::vector<MacroRetime> macroSubCandidates(MacroFile const& m, std::vector<int> const& adjust, std::vector<int> const& sub,
    std::size_t played, std::size_t back, int step, int maxAdjust) {
    std::vector<MacroRetime> out;
    auto const n = m.inputs.size();
    if (n == 0 || step <= 0 || step >= kMacroSubUnits) return out;
    std::vector<int> adj = adjust, sb = sub;
    adj.resize(n, 0);
    sb.resize(n, 0);
    auto floorDivU = [](long long v) { return v >= 0 ? v / kMacroSubUnits : -((-v + kMacroSubUnits - 1) / kMacroSubUnits); };
    // moves input i (and its release) by d parts of a tick from where it is now
    auto tryOne = [&](std::size_t i, int d, bool withRelease) {
        auto a = adj;
        auto s = sb;
        auto move = [&](std::size_t q) {
            long long pos = static_cast<long long>(a[q]) * kMacroSubUnits + s[q] + d;
            long long whole = floorDivU(pos);
            a[q] = static_cast<int>(whole);
            s[q] = static_cast<int>(pos - whole * kMacroSubUnits);
            return std::abs(a[q]) <= maxAdjust;
        };
        if (!move(i)) return;
        if (s[i] == 0) return; // on a whole tick: whole-tick re-timing's job
        if (withRelease) {
            auto rel = macroReleaseOf(m, i);
            if (rel == std::string::npos) return;
            if (!move(rel)) return;
        }
        if (!macroOrderOk(m, a, s)) return;
        for (auto const& c : out) if (c.adjust == a && c.sub == s) return;
        MacroRetime r;
        r.adjust = std::move(a);
        r.sub = std::move(s);
        r.input = i;
        r.subDelta = d;
        r.withRelease = withRelease;
        out.push_back(std::move(r));
    };
    auto tryInput = [&](std::size_t i, bool earlierOnly) {
        bool const press = m.inputs[i].down && macroReleaseOf(m, i) != std::string::npos;
        for (int u = step; u < kMacroSubUnits; u += step) {
            for (int d : {-u, u}) {
                if (earlierOnly && d > 0) continue;
                tryOne(i, d, false);
                if (press) tryOne(i, d, true);
            }
        }
    };
    if (played < n) tryInput(played, true);
    for (std::size_t k = 1; k <= back && k <= played && played - k < n; ++k) tryInput(played - k, false);
    return out;
}

std::vector<MacroRetime> macroWideCandidates(MacroFile const& m, std::vector<int> const& adjust, std::vector<int> const& sub,
    std::size_t played, std::size_t back, int from, int maxShift, int maxTotal) {
    std::vector<MacroRetime> out;
    auto const n = m.inputs.size();
    if (n == 0) return out;
    std::vector<int> adj = adjust, sb = sub;
    adj.resize(n, 0);
    sb.resize(n, 0);
    auto keep = [&](std::vector<int> a, std::size_t input, int d, bool withRelease, long long restFrom) {
        if (!macroOrderOk(m, a, sb)) return;
        for (auto const& c : out) if (c.adjust == a) return;
        MacroRetime r;
        r.adjust = std::move(a);
        r.sub = sb;
        r.input = input;
        r.delta = d;
        r.withRelease = withRelease;
        r.wide = true;
        r.restFrom = restFrom;
        out.push_back(std::move(r));
    };
    auto single = [&](std::size_t i, int d, bool withRelease) {
        auto a = adj;
        a[i] += d;
        if (std::abs(a[i]) > maxTotal) return;
        if (withRelease) {
            auto rel = macroReleaseOf(m, i);
            if (rel == std::string::npos) return;
            a[rel] += d;
            if (std::abs(a[rel]) > maxTotal) return;
        }
        keep(std::move(a), i, d, withRelease, -1);
    };
    auto rest = [&](std::size_t j, int d) {
        auto a = adj;
        for (std::size_t q = j; q < n; ++q) {
            a[q] += d;
            if (std::abs(a[q]) > maxTotal) return;
        }
        keep(std::move(a), j, d, false, static_cast<long long>(j));
    };
    std::vector<std::size_t> closeBy; // the next input, then the last played ones, nearest first
    if (played < n) closeBy.push_back(played);
    for (std::size_t k = 1; k <= back && k <= played && played - k < n; ++k) closeBy.push_back(played - k);
    for (int mag = std::max(1, from); mag <= maxShift; ++mag) {
        for (int d : {-mag, mag}) {
            for (std::size_t i : closeBy) {
                if (i == played && d > 0) continue; // not played yet: only earlier can change anything before the death
                bool const press = m.inputs[i].down && macroReleaseOf(m, i) != std::string::npos;
                single(i, d, false);
                if (press) single(i, d, true);
            }
            for (std::size_t i : closeBy) {
                if (i == played && d > 0) continue;
                rest(i, d);
            }
        }
    }
    return out;
}


std::vector<MacroRetime> macroPairCandidates(MacroFile const& m, std::vector<int> const& adjust, std::vector<int> const& sub,
    std::size_t played, std::size_t back, int maxMove, int maxAdjust) {
    std::vector<MacroRetime> out;
    auto const n = m.inputs.size();
    if (n == 0 || maxMove <= 0) return out;
    std::vector<int> adj = adjust, sb = sub;
    adj.resize(n, 0);
    sb.resize(n, 0);
    std::vector<std::size_t> presses; // the next input, then the last played ones, nearest first: presses with a release
    auto addPress = [&](std::size_t i) {
        if (i < n && m.inputs[i].down && macroReleaseOf(m, i) != std::string::npos) presses.push_back(i);
    };
    if (played < n) addPress(played);
    for (std::size_t k = 1; k <= back && k <= played && played - k < n; ++k) addPress(played - k);
    // (press, release) moves, smallest total first, then the press earlier first
    std::vector<std::pair<int, int>> moves;
    for (int dp = -maxMove; dp <= maxMove; ++dp)
        for (int dr = -maxMove; dr <= maxMove; ++dr)
            if (dp != dr) moves.push_back({dp, dr});
    std::stable_sort(moves.begin(), moves.end(), [](auto const& a, auto const& b) {
        int ta = std::abs(a.first) + std::abs(a.second), tb = std::abs(b.first) + std::abs(b.second);
        if (ta != tb) return ta < tb;
        int ma = std::max(std::abs(a.first), std::abs(a.second)), mb = std::max(std::abs(b.first), std::abs(b.second));
        if (ma != mb) return ma < mb;
        return a.first < b.first;
    });
    for (auto const& [dp, dr] : moves) {
        for (std::size_t i : presses) {
            if (i == played && dp > 0) continue; // not played yet: only earlier can change anything before the death
            auto rel = macroReleaseOf(m, i);
            auto a = adj;
            a[i] += dp;
            a[rel] += dr;
            if (std::abs(a[i]) > maxAdjust || std::abs(a[rel]) > maxAdjust) continue;
            if (!macroOrderOk(m, a, sb)) continue;
            // a press can't be let go of before it's pressed (the file may press and release on one frame)
            if (static_cast<long long>(m.inputs[rel].frame) + a[rel] < static_cast<long long>(m.inputs[i].frame) + a[i]) continue;
            bool dup = false;
            for (auto const& c : out) if (c.adjust == a) { dup = true; break; }
            if (dup) continue;
            MacroRetime r;
            r.adjust = std::move(a);
            r.sub = sb;
            r.input = i;
            r.delta = dp;
            r.releaseDelta = dr;
            r.pair = true;
            out.push_back(std::move(r));
        }
    }
    return out;
}

MacroSearch::Verdict MacroSearch::judge(std::size_t played, bool died, int diedAt, bool wide) const {
    auto reach = [](std::size_t p, bool d, int at) {
        return std::make_pair(p, d ? static_cast<long long>(at) : std::numeric_limits<long long>::max());
    };
    auto const r = reach(played, died, diedAt), rb = reach(base.played, base.died, base.diedAt);
    bool further = r > rb;
    if (wide && died && base.died) further = further && diedAt > base.diedAt;
    if (!further) return Verdict::Drop;
    if (bar <= 0 || !died || !base.died) return Verdict::Adopt;
    if (static_cast<long long>(diedAt) - base.diedAt >= bar) return Verdict::Adopt;
    return beamOn ? Verdict::Beam : Verdict::Drop;
}

void MacroSearch::adopt(MacroSearchBase next, std::size_t candNext) {
    if (beamOn) {
        base.candNext = candNext;
        undo.push_back(std::move(base));
        if (undo.size() > 16) undo.erase(undo.begin());
    }
    base = std::move(next);
    base.candNext = 0;
}

void MacroSearch::offer(MacroSearchBase b) {
    ++smallGains;
    if (!beamOn || beamMax == 0) return;
    for (auto const& o : beam)
        if (o.adjust == b.adjust && o.sub == b.sub && o.ghosts == b.ghosts) return;
    b.candNext = 0;
    beam.push_back(std::move(b));
    auto key = [](MacroSearchBase const& x) {
        return std::make_pair(x.played, x.died ? static_cast<long long>(x.diedAt) : std::numeric_limits<long long>::max());
    };
    std::stable_sort(beam.begin(), beam.end(), [&](auto const& a, auto const& c) { return key(a) > key(c); });
    if (beam.size() > beamMax) beam.resize(beamMax);
}

MacroSearch::Next MacroSearch::next() {
    if (!beamOn) return Next::None;
    if (!beam.empty() && beamPicks < beamPicksMax) {
        base = std::move(beam.front());
        beam.erase(beam.begin());
        base.candNext = 0;
        ++beamPicks;
        return Next::Beam;
    }
    if (!undo.empty() && undos < undoMax) {
        base = std::move(undo.back());
        undo.pop_back();
        ++base.candNext; // past the move that was built on
        ++undos;
        return Next::Undo;
    }
    return Next::None;
}

void MacroSearch::reset(MacroSearchBase b) {
    base = std::move(b);
    base.candNext = 0;
    beam.clear();
    undo.clear();
}
int macroFirstDifferentTick(std::vector<int> const& ticks, int endTick, std::vector<int> const& adjustA,
    std::vector<int> const& adjustB, std::vector<int> const& subA, std::vector<int> const& subB) {
    int first = macroFirstDifferentTick(ticks, endTick, adjustA, adjustB);
    std::size_t n = std::max(subA.size(), subB.size());
    for (std::size_t i = 0; i < n; ++i) {
        int a = i < subA.size() ? subA[i] : 0;
        int b = i < subB.size() ? subB[i] : 0;
        if (a == b) continue;
        int aa = i < adjustA.size() ? adjustA[i] : 0;
        int ab = i < adjustB.size() ? adjustB[i] : 0;
        int at = i < ticks.size() ? ticks[i] : endTick;
        first = std::min(first, at + std::min(0, ab - aa) - 1);
    }
    return first;
}

int macroFirstDifferentTick(std::vector<int> const& ticks, int endTick, std::vector<int> const& adjustA,
    std::vector<int> const& adjustB) {
    int first = std::numeric_limits<int>::max();
    std::size_t n = std::max(adjustA.size(), adjustB.size());
    for (std::size_t i = 0; i < n; ++i) {
        int a = i < adjustA.size() ? adjustA[i] : 0;
        int b = i < adjustB.size() ? adjustB[i] : 0;
        if (a == b) continue;
        // where A pressed it (or, if A never got to it, no earlier than where A stopped), moved by the
        // difference when B presses it earlier
        int at = i < ticks.size() ? ticks[i] : endTick;
        first = std::min(first, at + std::min(0, b - a));
    }
    return first;
}

int macroPickCheckpoint(std::vector<int> const& cpTicks, int firstDifferent, int margin) {
    int pick = -1;
    for (std::size_t i = 0; i < cpTicks.size(); ++i) {
        long long t = cpTicks[i];
        if (t > 0 && t + margin < static_cast<long long>(firstDifferent)) pick = static_cast<int>(i);
    }
    return pick;
}

} // namespace fwc

namespace fwc {

// ---- v1.122 writing CML ---------------------------------------------------------------------------

namespace {
struct CmlWriter {
    std::string out;
    void u8(std::uint8_t b) { out.push_back(static_cast<char>(b)); }
    void varU(std::uint64_t v) {
        do {
            std::uint8_t b = v & 0x7f;
            v >>= 7;
            if (v) b |= 0x80;
            u8(b);
        } while (v);
    }
    void varI(std::int64_t v) { varU((static_cast<std::uint64_t>(v) << 1) ^ static_cast<std::uint64_t>(v >> 63)); }
    void f32(float f) {
        std::uint32_t bits;
        std::memcpy(&bits, &f, 4);
        for (int k = 0; k < 4; ++k) u8(static_cast<std::uint8_t>(bits >> (8 * k)));
    }
    void str(std::string const& s) {
        varU(s.size());
        out += s;
    }
};

// zlib wrapper around stored (uncompressed) deflate blocks: any inflater reads it.
std::string zlibStored(std::string const& data) {
    std::string out;
    out.push_back(static_cast<char>(0x78));
    out.push_back(static_cast<char>(0x01));
    std::size_t pos = 0;
    do {
        std::size_t n = std::min<std::size_t>(65535, data.size() - pos);
        bool last = pos + n >= data.size();
        out.push_back(static_cast<char>(last ? 1 : 0));
        out.push_back(static_cast<char>(n & 0xff));
        out.push_back(static_cast<char>(n >> 8));
        out.push_back(static_cast<char>(~n & 0xff));
        out.push_back(static_cast<char>((~n >> 8) & 0xff));
        out.append(data, pos, n);
        pos += n;
    } while (pos < data.size());
    std::uint32_t a = 1, b = 0;
    for (unsigned char c : data) {
        a = (a + c) % 65521;
        b = (b + a) % 65521;
    }
    std::uint32_t adler = (b << 16) | a;
    for (int k = 3; k >= 0; --k) out.push_back(static_cast<char>((adler >> (8 * k)) & 0xff));
    return out;
}
} // namespace

std::string writeCml(MacroFile const& m) {
    constexpr std::int64_t kScale = 1'000'000;
    std::vector<MacroInput> inputs = m.inputs;
    std::stable_sort(inputs.begin(), inputs.end(), [](MacroInput const& a, MacroInput const& b) {
        return std::pair(a.frame, a.sub) < std::pair(b.frame, b.sub);
    });
    CmlWriter body;
    body.str(m.author);
    body.str(m.description);
    body.f32(0.f); // accuracy
    body.f32(static_cast<float>(m.duration));
    body.f32(1.f); // speedhack
    body.f32(static_cast<float>(m.framerate > 0 ? m.framerate : 240.0));
    body.varI(0);
    body.varI(0);
    body.u8(0);
    body.varI(inputs.empty() ? 0 : inputs.back().frame);
    body.str(m.botName.empty() ? "xdBot" : m.botName);
    body.str(m.botVersion);
    body.varU(0); // seed
    body.str(m.macroName);
    body.varU(inputs.size());
    std::int64_t prev = 0;
    for (auto const& in : inputs) {
        std::int64_t t = static_cast<std::int64_t>(in.frame) * kScale + static_cast<std::int64_t>(std::clamp(in.sub, 0, 999)) * 1000
            + std::clamp(in.fine, 0, 9999) / 10; // v1.137 CML keeps 1e-6 frame
        body.varI(t - prev);
        prev = t;
        int idx = in.button == 2 || in.button == 3 ? in.button : 1;
        body.u8(static_cast<std::uint8_t>((in.down ? 1 : 0) | (in.p2 ? 2 : 0) | (idx << 2)));
    }
    body.varU(0); // frame fixes
    CmlWriter file;
    file.out = std::string("CML\0", 4);
    file.varU(6);
    file.varU(body.out.size());
    file.out += zlibStored(body.out);
    return file.out;
}

} // namespace fwc
