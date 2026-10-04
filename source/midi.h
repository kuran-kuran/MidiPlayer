#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>
struct MidiEvent {
    uint64_t tick = 0;
    double seconds = 0;
    unsigned track = 0, order = 0;
    uint8_t port = 0;
    bool portCommand = false;
    uint32_t message = 0;
    std::vector<uint8_t> sysex;
    uint32_t tempo = 0;
};
struct MidiSong {
    std::wstring title;
    int format = 0, tracks = 0;
    double duration = 0;
    std::vector<MidiEvent> events;
};
MidiSong ReadMidi(const std::filesystem::path &path);
