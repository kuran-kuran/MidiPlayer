#pragma once
#include <windows.h>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>
constexpr WPARAM MIDI_OUTPUT_TIMEOUT = 0x10000, MIDI_OUTPUT_HOST_ERROR = 0x10001;
// WinMM runs in an expendable child process; callers only enqueue messages.
class MidiOutput {
    struct Request { DWORD kind, message; std::vector<unsigned char> bytes; };
    HWND window;
    int device;
    std::mutex mutex;
    std::condition_variable wake;
    std::deque<Request> requests;
    size_t queuedBytes = 0;
    std::atomic<bool> stopping{false}, failed{false};
    HANDLE pipe = INVALID_HANDLE_VALUE, process = nullptr, job = nullptr;
    DWORD error = 0;
    std::atomic<DWORD> completed{0};
    DWORD operationKind = 0;
    bool closing = false;
    std::vector<unsigned char> shutdownReset;
    std::thread worker;
    bool Transfer(void *data, DWORD size, bool write);
    bool Exchange(const Request &request);
    void Run();
    void Queue(Request request);
  public:
    MidiOutput(HWND window, int device);
    ~MidiOutput();
    void Short(DWORD message);
    void Long(const std::vector<unsigned char> &bytes);
    void Reset();
    void Shutdown(const std::vector<unsigned char> &reset = {});
    DWORD CompletedMessages() const { return completed.load(); }
};
// Returns -1 for an ordinary invocation. Test executables enable simulated drivers.
int RunMidiOutputHostFromCommandLine(bool testDrivers = false);
