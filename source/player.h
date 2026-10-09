#pragma once
#include <windows.h>
#include <mmsystem.h>
#include "midi.h"
#include "midi-output.h"
#include <array>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <functional>
constexpr UINT WM_SONG_DONE = WM_APP + 1, WM_PLAYER_ERROR = WM_APP + 2;
const std::vector<uint8_t> &MidiResetMessage(int kind);
struct MidiDisplay {
    std::array<uint8_t, 64> dots{};
    std::wstring text;
    double dotsUntil = 0, textUntil = 0, textStart = 0;
};
struct PlayerView {
    double position = 0;
    bool playing = false, paused = false;
    int bpm = 120;
    bool multiPortMeters = false;
    std::wstring songSystem;
    MidiDisplay displays[6];
    int programs[6][16]{}; // -1 until the song specifies a Program Change, otherwise MIDI 0..127.
    uint8_t notes[6][16][128]{};
    float levels[6][16]{};
    bool muted[6][16]{};
    uint8_t pan[6][16]{}, volume[6][16]{}, expression[6][16]{};
    bool sustain[6][16]{};
    uint16_t pitchBend[6][16]{};
    uint8_t modulation[6][16]{};
    int reverb[6][16]{}, chorus[6][16]{}; // -1 until the song sends CC91/CC93.
};
class Player {
    std::mutex mutex;
    std::condition_variable cv;
    std::thread worker;
    bool quit = false;
    HWND window;
    std::shared_ptr<MidiSong> song;
    PlayerView view;
    size_t cursor = 0;
    WPARAM generation = 0;
    std::array<int, 6> devices{{-1, -2, -2, -2, -2, -2}};
    std::map<int, std::unique_ptr<MidiOutput>> outputs;
    std::chrono::steady_clock::time_point origin;
    double base = 0;
    bool resetOnSong = true;
    int resetKind = 0;
    bool silenceSkip = false;
    double lastSoundPosition = 0;
    uint16_t masterVolume[6]{16383, 16383, 16383, 16383, 16383, 16383};
    int songMode[6]{}; // 1: GM, 2: GM2, 3: GS, 4: XG.
    unsigned songMaps[6]{};
    uint8_t bankLsb[6][16]{};
    void UpdateSongSystem();
    uint8_t held[6][16][128]{};
    // Optional short-message transport lets tests verify the actual output stream without hardware.
    std::function<void(int, DWORD)> shortTransport;
    MidiOutput *Output(int port);
    void SendShort(int port, DWORD message);
    void Revoice(int port, int channel);
    void SendLong(MidiOutput *h, const std::vector<uint8_t> &bytes);
    void Silence(bool erase);
    void ResetInternal(int kind);
    void Run();
    void Dispatch(const MidiEvent &e);
    void ClearVisual();
    std::vector<uint8_t> displaySysex[6];
    void DisplayMessage(int port, const std::vector<uint8_t> &bytes);

  public:
    explicit Player(HWND w, std::function<void(int, DWORD)> transport = {});
    ~Player();
    void Start(std::shared_ptr<MidiSong> s);
    void Stop();
    void Pause();
    void Resume();
    void SetDevice(int port, int device);
    int GetDevice(int port);
    void SetAutoReset(bool v, int kind = 0);
    void SetSilenceSkip(bool enabled);
    void Reset(int kind);
    PlayerView Snapshot();
    bool IsCompleted(WPARAM token);
    void ToggleMute(int port, int channel);
};
