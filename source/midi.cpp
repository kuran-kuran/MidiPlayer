#include "midi.h"
#include <algorithm>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <windows.h>
namespace {
struct Reader {
    const std::vector<uint8_t> &b;
    size_t p, end;
    uint8_t byte() {
        if (p >= end)
            throw std::runtime_error("Unexpected end of MIDI data");
        return b[p++];
    }
    uint32_t be(int n) {
        uint32_t v = 0;
        while (n--)
            v = (v << 8) | byte();
        return v;
    }
    uint32_t vlq() {
        uint32_t v = 0;
        for (int i = 0; i < 4; i++) {
            auto c = byte();
            v = (v << 7) | (c & 127);
            if (!(c & 128))
                return v;
        }
        throw std::runtime_error("Invalid variable-length value");
    }
    std::vector<uint8_t> data(size_t n) {
        if (n > end - p)
            throw std::runtime_error("Invalid event length");
        std::vector<uint8_t> v(b.begin() + p, b.begin() + p + n);
        p += n;
        return v;
    }
};
std::wstring Decode(const std::vector<uint8_t> &v) {
    if (v.empty())
        return {};
    UINT cp = CP_UTF8;
    int n = MultiByteToWideChar(cp, MB_ERR_INVALID_CHARS, (const char *)v.data(), (int)v.size(), nullptr, 0);
    if (!n) {
        cp = 932;
        n = MultiByteToWideChar(cp, 0, (const char *)v.data(), (int)v.size(), nullptr, 0);
    }
    std::wstring s(n, L' ');
    MultiByteToWideChar(cp, 0, (const char *)v.data(), (int)v.size(), s.data(), n);
    for (auto &c : s)
        if (c < 32)
            c = L' ';
    return s;
}
} // namespace
MidiSong ReadMidi(const std::filesystem::path &path) {
    std::ifstream f(path, std::ios::binary);
    if (!f)
        throw std::runtime_error("Cannot open file");
    f.seekg(0, std::ios::end);
    auto size = f.tellg();
    if (size < 14 || size > 128 * 1024 * 1024)
        throw std::runtime_error("Invalid or too large MIDI file");
    f.seekg(0);
    std::vector<uint8_t> b((size_t)size);
    f.read((char *)b.data(), size);
    if (!f)
        throw std::runtime_error("Cannot read file");
    Reader r{b, 0, b.size()};
    if (r.be(4) != 0x4d546864)
        throw std::runtime_error("Missing MThd");
    auto len = r.be(4);
    if (len < 6 || len > r.end - r.p)
        throw std::runtime_error("Invalid header");
    auto headerEnd = r.p + len;
    MidiSong s;
    s.format = r.be(2);
    s.tracks = r.be(2);
    uint16_t division = (uint16_t)r.be(2);
    r.p = headerEnd;
    if (s.format > 1)
        throw std::runtime_error("Only SMF format 0 and 1 are supported");
    if (!s.tracks || (!s.format && s.tracks != 1) || !division)
        throw std::runtime_error("Invalid track count or division");
    double fixed = 0;
    if (division & 0x8000) {
        int fps = -(int8_t)(division >> 8);
        if ((fps != 24 && fps != 25 && fps != 29 && fps != 30) || !(division & 255))
            throw std::runtime_error("Invalid SMPTE division");
        fixed = 1.0 / ((fps == 29 ? 29.97 : fps) * (division & 255));
    }
    uint64_t endTick = 0;
    for (int t = 0; t < s.tracks; t++) {
        if (r.be(4) != 0x4d54726b)
            throw std::runtime_error("Missing MTrk");
        auto n = r.be(4);
        if (n > r.end - r.p)
            throw std::runtime_error("Invalid track length");
        Reader tr{b, r.p, r.p + n};
        r.p += n;
        uint64_t tick = 0;
        uint8_t running = 0, port = 0;
        unsigned order = 0;
        bool ended = false;
        while (tr.p < tr.end) {
            tick += tr.vlq();
            uint8_t status = tr.byte();
            if (status < 128) {
                if (!running)
                    throw std::runtime_error("Invalid running status");
                tr.p--;
                status = running;
            }
            MidiEvent e;
            e.tick = tick;
            e.track = t;
            e.order = order++;
            e.port = port;
            if (status == 255) {
                running = 0;
                auto type = tr.byte();
                auto d = tr.data(tr.vlq());
                if (type == 0x2f) {
                    if (!d.empty())
                        throw std::runtime_error("Invalid end of track");
                    ended = true;
                    break;
                }
                if (type == 0x21) {
                    if (d.size() != 1)
                        throw std::runtime_error("Invalid MIDI port event");
                    if (s.format == 1) {
                        port = d[0];
                        e.port = port;
                        e.portCommand = true;
                        s.events.push_back(e);
                    }
                }
                if (type == 3 && s.title.empty() && !d.empty())
                    s.title = Decode(d);
                if (type == 0x51) {
                    if (d.size() != 3)
                        throw std::runtime_error("Invalid tempo");
                    e.tempo = (d[0] << 16) | (d[1] << 8) | d[2];
                    if (!e.tempo)
                        throw std::runtime_error("Zero tempo");
                    s.events.push_back(e);
                }
            } else if (status == 0xf0 || status == 0xf7) {
                running = 0;
                e.sysex = tr.data(tr.vlq());
                if (status == 0xf0)
                    e.sysex.insert(e.sysex.begin(), 0xf0);
                if (!e.sysex.empty())
                    s.events.push_back(std::move(e));
            } else if (status >= 0x80 && status <= 0xef) {
                running = status;
                auto a = tr.byte();
                auto c = (status & 0xf0);
                auto d = (c == 0xc0 || c == 0xd0) ? 0 : tr.byte();
                if (a > 127 || d > 127)
                    throw std::runtime_error("Invalid MIDI data byte");
                e.message = status | (a << 8) | (d << 16);
                s.events.push_back(e);
            } else
                throw std::runtime_error("Unsupported MIDI status");
            if (s.events.size() > 4000000)
                throw std::runtime_error("Too many MIDI events");
        }
        if (!ended)
            throw std::runtime_error("Missing end of track");
        endTick = std::max(endTick, tick);
    }
    std::stable_sort(s.events.begin(), s.events.end(), [](const MidiEvent &a, const MidiEvent &b) {
        if (a.tick != b.tick)
            return a.tick < b.tick;
        if (a.track != b.track)
            return a.track < b.track;
        return a.order < b.order;
    });
    double sec = 0;
    uint64_t prev = 0;
    uint32_t tempo = 500000;
    for (auto &e : s.events) {
        sec += (e.tick - prev) * (fixed ? fixed : tempo / 1000000.0 / division);
        prev = e.tick;
        e.seconds = sec;
        if (e.tempo)
            tempo = e.tempo;
    }
    s.duration = sec + (endTick - prev) * (fixed ? fixed : tempo / 1000000.0 / division);
    if (s.title.empty())
        s.title = path.stem().wstring();
    return s;
}
