#include "player.h"
#include <algorithm>
#include <cmath>
Player::Player(HWND w, std::function<void(int, DWORD)> transport)
    : window(w), shortTransport(std::move(transport)) {
    ClearVisual();
    worker = std::thread(&Player::Run, this);
}
Player::~Player() {
    {
        std::lock_guard<std::mutex> l(mutex);
        quit = true;
        cv.notify_all();
    }
    worker.join();
    for (auto &output : outputs)
        output.second->Shutdown(resetOnSong ? MidiResetMessage(resetKind) : std::vector<uint8_t>{});
    outputs.clear();
}
void Player::ClearVisual() {
    for (int p = 0; p < 6; ++p) {
        masterVolume[p] = 16383;
        view.displays[p] = {};
        displaySysex[p].clear();
    }
    ZeroMemory(view.notes, sizeof(view.notes));
    ZeroMemory(view.levels, sizeof(view.levels));
    ZeroMemory(held, sizeof(held));
    ZeroMemory(view.sustain, sizeof(view.sustain));
    for (int p = 0; p < 6; p++)
        for (int c = 0; c < 16; c++) {
            view.programs[p][c] = -1;
            view.volume[p][c] = 100;
            view.pitchBend[p][c] = 8192;
            view.modulation[p][c] = 0;
            view.reverb[p][c] = view.chorus[p][c] = -1;
            view.pan[p][c] = 64;
            view.expression[p][c] = 127;
        }
}
MidiOutput *Player::Output(int p) {
    if (p < 0 || p >= 6 || devices[p] == -2 || shortTransport)
        return nullptr;
    int id = devices[p];
    auto it = outputs.find(id);
    if (it != outputs.end())
        return it->second.get();
    auto output = std::make_unique<MidiOutput>(window, id);
    auto result = output.get();
    outputs.emplace(id, std::move(output));
    return result;
}
void Player::SendShort(int port, DWORD message) {
    if (shortTransport) {
        shortTransport(port, message);
        return;
    }
    auto h = Output(port);
    if (h)
        h->Short(message);
}
void Player::Revoice(int p, int c) {
    SendShort(p, 0xb0 | c | (64 << 8) | ((view.sustain[p][c] ? 127 : 0) << 16));
    if (view.muted[p][c])
        return;
    for (int n = 0; n < 128; ++n) {
        if (!view.notes[p][c][n])
            continue;
        SendShort(p, 0x90 | c | (n << 8) | (view.notes[p][c][n] << 16));
        if (!held[p][c][n])
            SendShort(p, 0x80 | c | (n << 8));
    }
}
void Player::ToggleMute(int p, int c) {
    if (p < 0 || p >= 6 || c < 0 || c >= 16)
        return;
    std::lock_guard<std::mutex> l(mutex);
    view.muted[p][c] = !view.muted[p][c];
    if (view.muted[p][c]) {
        // All Sound Off kills existing voices, including sustained ones, without resetting controllers.
        SendShort(p, 0xb0 | c | (120 << 8));
        // Logical ports routed to the same device share physical channels. Restore other active ports.
        if (view.playing && devices[p] != -2)
            for (int other = 0; other < 6; ++other)
                if (other != p && devices[other] == devices[p] && !view.muted[other][c])
                    Revoice(other, c);
    } else if (view.playing) {
        Revoice(p, c);
    }
}
void Player::SendLong(MidiOutput *h, const std::vector<uint8_t> &bytes) {
    if (h && !bytes.empty())
        h->Long(bytes);
}
void Player::Silence(bool erase) {
    for (auto &p : outputs) {
        p.second->Reset();
        for (int c = 0; c < 16; c++) {
            p.second->Short(0xb0 | c | (64 << 8));
            p.second->Short(0xb0 | c | (123 << 8));
            p.second->Short(0xb0 | c | (120 << 8));
        }
    }
    if (erase)
        ClearVisual();
}
const std::vector<uint8_t> &MidiResetMessage(int kind) {
    static const std::vector<uint8_t> gs{0xf0, 0x41, 0x10, 0x42, 0x12, 0x40, 0, 0x7f, 0, 0x41, 0xf7},
        xg{0xf0, 0x43, 0x10, 0x4c, 0, 0, 0x7e, 0, 0xf7}, gm{0xf0, 0x7e, 0x7f, 9, 1, 0xf7};
    return kind == 0 ? gs : kind == 1 ? xg : gm;
}
void Player::ResetInternal(int kind) {
    std::vector<MidiOutput *> sent;
    for (int p = 0; p < 6; p++) {
        auto h = Output(p);
        if (h && std::find(sent.begin(), sent.end(), h) == sent.end()) {
            SendLong(h, MidiResetMessage(kind));
            sent.push_back(h);
        }
    }
    ClearVisual();
}
void Player::Start(std::shared_ptr<MidiSong> s) {
    std::lock_guard<std::mutex> l(mutex);
    ++generation;
    Silence(true);
    song = s;
    ZeroMemory(songMode, sizeof(songMode));
    ZeroMemory(songMaps, sizeof(songMaps));
    ZeroMemory(bankLsb, sizeof(bankLsb));
    view.songSystem.clear();
    cursor = 0;
    base = 0;
    view.position = 0;
    view.bpm = 120;
    lastSoundPosition = 0;
    view.multiPortMeters = false;
    view.playing = true;
    view.paused = false;
    if (resetOnSong)
        ResetInternal(resetKind);
    origin = std::chrono::steady_clock::now() + std::chrono::milliseconds(resetOnSong ? 100 : 0);
    cv.notify_all();
}
void Player::Stop() {
    std::lock_guard<std::mutex> l(mutex);
    ++generation;
    view.playing = false;
    view.paused = false;
    view.position = 0;
    base = 0;
    cursor = 0;
    Silence(true);
    cv.notify_all();
}
void Player::Pause() {
    std::lock_guard<std::mutex> l(mutex);
    if (view.playing) {
        base = view.position;
        view.playing = false;
        view.paused = true;
        Silence(false);
        cv.notify_all();
    }
}
void Player::Resume() {
    std::lock_guard<std::mutex> l(mutex);
    if (view.paused) {
        for (int p = 0; p < 6; p++)
            for (int c = 0; c < 16; c++)
                Revoice(p, c);
        origin = std::chrono::steady_clock::now();
        view.playing = true;
        view.paused = false;
        cv.notify_all();
    }
}
void Player::SetDevice(int p, int d) {
    std::lock_guard<std::mutex> l(mutex);
    ++generation;
    view.playing = false;
    view.paused = false;
    view.position = 0;
    cursor = 0;
    base = 0;
    Silence(true);
    outputs.clear();
    devices[p] = d;
    // Opening a driver may block: defer it until the playback thread needs output.
    cv.notify_all();
}
int Player::GetDevice(int p) {
    std::lock_guard<std::mutex> l(mutex);
    return devices[p];
}
void Player::SetAutoReset(bool v, int kind) {
    std::lock_guard<std::mutex> l(mutex);
    resetOnSong = v;
    resetKind = std::clamp(kind, 0, 2);
}
void Player::Reset(int kind) {
    std::lock_guard<std::mutex> l(mutex);
    Silence(true);
    ResetInternal(kind);
    if (view.playing) {
        base = view.position;
        origin = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
    }
    cv.notify_all();
}
PlayerView Player::Snapshot() {
    std::lock_guard<std::mutex> l(mutex);
    return view;
}
void Player::SetSilenceSkip(bool enabled) {
    std::lock_guard<std::mutex> l(mutex);
    silenceSkip = enabled;
    lastSoundPosition = view.position;
}
void Player::UpdateSongSystem() {
    view.songSystem.clear();
    for (int mode = 1; mode <= 4; ++mode) {
        bool found = false;
        unsigned maps = 0;
        for (int p = 0; p < 6; ++p)
            if (songMode[p] == mode) { found = true; maps |= songMaps[p]; }
        if (!found) continue;
        if (!view.songSystem.empty()) view.songSystem += L" / ";
        view.songSystem += mode == 1 ? L"GM" : mode == 2 ? L"GM2" : mode == 3 ? L"GS" : L"XG";
        if (mode == 3 && maps) {
            view.songSystem += L": ";
            const wchar_t *names[] = {L"SC-55 MAP", L"SC-88 MAP", L"SC-88Pro MAP", L"SC-8850 MAP"};
            bool first = true;
            for (int i = 0; i < 4; ++i)
                if (maps & (1u << i)) {
                    if (!first) view.songSystem += L" + ";
                    view.songSystem += names[i];
                    first = false;
                }
        }
    }
}
void Player::DisplayMessage(int port, const std::vector<uint8_t> &bytes) {
    auto &pending = displaySysex[port];
    if (bytes.empty()) return;
    if (bytes.front() == 0xf0) pending.clear();
    else if (pending.empty()) return;
    if (pending.size() + bytes.size() > 1024) { pending.clear(); return; }
    pending.insert(pending.end(), bytes.begin(), bytes.end());
    if (pending.back() != 0xf7) return;
    auto message = std::move(pending);
    pending.clear();
    if (message.size() == 6 && message[0] == 0xf0 && message[1] == 0x7e &&
        message[2] < 128 && message[3] == 9 && (message[4] == 1 || message[4] == 3)) {
        songMode[port] = message[4] == 1 ? 1 : 2;
        songMaps[port] = 0;
        ZeroMemory(bankLsb[port], sizeof(bankLsb[port]));
        masterVolume[port] = 16383;
        UpdateSongSystem();
    }
    if (message.size() == 9 && message[0] == 0xf0 && message[1] == 0x43 &&
        (message[2] & 0xf0) == 0x10 && message[3] == 0x4c && message[4] == 0 &&
        message[5] == 0 && message[6] == 0x7e && message[7] == 0) {
        songMode[port] = 4;
        songMaps[port] = 0;
        ZeroMemory(bankLsb[port], sizeof(bankLsb[port]));
        masterVolume[port] = 16383;
        UpdateSongSystem();
    }
    // Track volume commands as they arrive; never inspect future song events.
    if (message.size() == 8 && message[0] == 0xf0 && message[1] == 0x7f &&
        message[3] == 4 && message[4] == 1 && message[5] < 128 && message[6] < 128) {
        masterVolume[port] = message[5] | (message[6] << 7);
        return;
    }
    if (message == MidiResetMessage(0) || message == MidiResetMessage(1) ||
        message == MidiResetMessage(2))
        masterVolume[port] = 16383;
    if (message.size() < 11 || message[0] != 0xf0 || message[1] != 0x41 ||
        message[2] > 0x1f || message[4] != 0x12) return;
    unsigned sum = 0;
    for (size_t i = 5; i + 1 < message.size(); ++i) {
        if (message[i] > 127) return;
        sum += message[i];
    }
    if (sum % 128) return;
    if (message[3] == 0x42 && message.size() == 11 && message[5] == 0x40 &&
        message[6] == 0 && message[7] == 4)
        masterVolume[port] = message[8] * 129;
    auto &display = view.displays[port];
    // A GS Reset in the song restores normal activity display too.
    if (message[3] == 0x42 && message.size() == 11 && message[5] == 0x40 &&
        message[6] == 0 && message[7] == 0x7f && message[8] == 0) {
        display = {};
        songMode[port] = 3;
        songMaps[port] = 0;
        ZeroMemory(bankLsb[port], sizeof(bankLsb[port]));
        masterVolume[port] = 16383;
        UpdateSongSystem();
        return;
    }
    if (message[3] == 0x42 && message.size() == 11 &&
        (message[5] == 0x40 || message[5] == 0x50) && (message[6] & 0xf0) == 0x40 &&
        message[7] == 0 && message[8] >= 1 && message[8] <= 4) {
        songMode[port] = 3;
        songMaps[port] |= 1u << (message[8] - 1);
        UpdateSongSystem();
    }
    if (message[3] != 0x45 || message[5] != 0x10 || message[7] != 0) return;
    size_t size = message.size() - 10;
    if (message[6] == 0 && size >= 1 && size <= 32) {
        std::wstring text;
        for (size_t i = 8; i < message.size() - 2; ++i) {
            if (message[i] < 0x20 || message[i] > 0x7f) return;
            text += (wchar_t)message[i];
        }
        display.text = std::move(text);
        display.textStart = view.position;
        display.textUntil = view.position + 3 + (size > 16 ? (size - 16) * 0.25 : 0);
    } else if (message[6] == 1 && size == 64) {
        for (size_t i = 0; i < 64; ++i)
            if (message[8 + i] > 31) return;
        std::copy_n(message.begin() + 8, 64, display.dots.begin());
        display.dotsUntil = view.position + 3;
    }
}
void Player::Dispatch(const MidiEvent &e) {
    if (e.portCommand) {
        view.multiPortMeters = true;
        return;
    }
    if (e.tempo) {
        view.bpm = (int)std::round(60000000.0 / e.tempo);
        return;
    }
    if (e.port >= 6)
        return;
    auto h = Output(e.port);
    if (!e.sysex.empty()) {
        DisplayMessage(e.port, e.sysex);
        SendLong(h, e.sysex);
        return;
    }
    int p = e.port, c = e.message & 15, k = (e.message >> 8) & 127, v = (e.message >> 16) & 127,
        type = e.message & 0xf0;
    // Keep the original event for visualization; only suppress audible note starts on output.
    if (!(view.muted[p][c] && type == 0x90 && v))
        SendShort(p, e.message);
    if (type == 0xc0) {
        view.programs[p][c] = k;
        if (songMode[p] == 3 && bankLsb[p][c] >= 1 && bankLsb[p][c] <= 4) {
            songMaps[p] |= 1u << (bankLsb[p][c] - 1);
            UpdateSongSystem();
        }
    } else if (type == 0x90 && v) {
        held[p][c][k] = (uint8_t)v;
        view.notes[p][c][k] = (uint8_t)v;
    } else if (type == 0x80 || (type == 0x90 && !v)) {
        held[p][c][k] = 0;
        if (!view.sustain[p][c])
            view.notes[p][c][k] = 0;
    } else if (type == 0xe0) {
        view.pitchBend[p][c] = (uint16_t)(k | (v << 7));
    } else if (type == 0xb0) {
        if (k == 32)
            bankLsb[p][c] = (uint8_t)v;
        if (k == 1)
            view.modulation[p][c] = (uint8_t)v;
        if (k == 91)
            view.reverb[p][c] = v;
        if (k == 93)
            view.chorus[p][c] = v;
        if (k == 7)
            view.volume[p][c] = (uint8_t)v;
        if (k == 11)
            view.expression[p][c] = (uint8_t)v;
        if (k == 10)
            view.pan[p][c] = (uint8_t)v;
        if (k == 64) {
            view.sustain[p][c] = v >= 64;
            if (v < 64)
                for (int n = 0; n < 128; n++)
                    if (!held[p][c][n])
                        view.notes[p][c][n] = 0;
        }
        if (k == 120 || k == 123) {
            ZeroMemory(held[p][c], 128);
            if (k == 120 || !view.sustain[p][c])
                ZeroMemory(view.notes[p][c], 128);
        }
        if (k == 121) {
            view.pitchBend[p][c] = 8192;
            view.modulation[p][c] = 0;
            view.sustain[p][c] = false;
            view.expression[p][c] = 127;
            for (int n = 0; n < 128; n++)
                if (!held[p][c][n])
                    view.notes[p][c][n] = 0;
        }
    }
}
void Player::Run() {
    timeBeginPeriod(1);
    std::unique_lock<std::mutex> l(mutex);
    while (!quit) {
        if (view.playing && song) {
            auto now = std::chrono::steady_clock::now();
            double elapsed = std::chrono::duration<double>(now - origin).count();
            view.position = std::min(song->duration, std::max(0.0, base + elapsed));
            if (elapsed >= 0) {
                while (cursor < song->events.size() && song->events[cursor].seconds <= view.position)
                    Dispatch(song->events[cursor++]);
                bool audible = false;
                for (int p = 0; p < 6; ++p)
                    for (int c = 0; c < 16; ++c)
                        if (masterVolume[p] && view.volume[p][c] && view.expression[p][c])
                            for (int n = 0; n < 128; ++n)
                                audible = audible || view.notes[p][c][n] != 0;
                if (audible)
                    lastSoundPosition = view.position;
                if ((view.position >= song->duration && cursor == song->events.size()) ||
                    (silenceSkip && view.position - lastSoundPosition >= 10.0)) {
                    view.playing = false;
                    Silence(true);
                    PostMessage(window, WM_SONG_DONE, generation, 0);
                }
            }
        }
        for (int p = 0; p < 6; p++)
            for (int c = 0; c < 16; c++) {
                int velocity = 0;
                for (int n = 0; n < 128; n++)
                    velocity = std::max(velocity, (int)view.notes[p][c][n]);
                float target = velocity / 127.f * view.volume[p][c] / 127.f * view.expression[p][c] / 127.f;
                view.levels[p][c] = std::max(target, view.levels[p][c] - 0.006f);
            }
        cv.wait_for(l, std::chrono::milliseconds(view.playing ? 1 : 10));
    }
    l.unlock();
    timeEndPeriod(1);
}

bool Player::IsCompleted(WPARAM token) {
    std::lock_guard<std::mutex> l(mutex);
    return token == generation && !view.playing && !view.paused;
}
