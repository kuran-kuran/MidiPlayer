#include "midi-output.h"
#include <mmsystem.h>
#include <shellapi.h>
#include <string>
#include <memory>
#include <algorithm>
#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "shell32.lib")
namespace {
constexpr DWORD Open = 1, ShortMessage = 2, LongMessage = 3, ResetDevice = 4, CloseDevice = 5;
struct Packet { DWORD kind, message, size; };
constexpr DWORD Timeout = 2000;
constexpr size_t MaxQueueBytes = 16 * 1024 * 1024;
bool HostTransfer(HANDLE pipe, void *data, DWORD size, bool write) {
    auto bytes = static_cast<unsigned char *>(data);
    while (size) {
        DWORD done = 0;
        BOOL ok = write ? WriteFile(pipe, bytes, size, &done, nullptr)
                        : ReadFile(pipe, bytes, size, &done, nullptr);
        if (!ok || !done) return false;
        bytes += done;
        size -= done;
    }
    return true;
}
}
MidiOutput::MidiOutput(HWND w, int id) : window(w), device(id), worker(&MidiOutput::Run, this) {}
MidiOutput::~MidiOutput() { Shutdown(); }
void MidiOutput::Shutdown(const std::vector<unsigned char> &reset) {
    if (!worker.joinable()) return;
    {
        std::lock_guard<std::mutex> lock(mutex);
        shutdownReset = reset;
    }
    stopping = true;
    wake.notify_all();
    if (worker.joinable()) worker.join();
}
void MidiOutput::Queue(Request request) {
    if (stopping || failed) return;
    std::lock_guard<std::mutex> lock(mutex);
    if (requests.size() >= 65536 || queuedBytes + request.bytes.size() > MaxQueueBytes) {
        failed = true;
        PostMessageW(window, WM_APP + 2, MIDI_OUTPUT_HOST_ERROR, device);
        wake.notify_all();
        return;
    }
    queuedBytes += request.bytes.size();
    requests.push_back(std::move(request));
    wake.notify_all();
}
void MidiOutput::Short(DWORD message) { Queue({ShortMessage, message, {}}); }
void MidiOutput::Long(const std::vector<unsigned char> &bytes) { Queue({LongMessage, 0, bytes}); }
void MidiOutput::Reset() {
    // Stop/pause must discard old note starts still waiting for a slow output.
    {
        std::lock_guard<std::mutex> lock(mutex);
        requests.clear();
        queuedBytes = 0;
    }
    Queue({ResetDevice, 0, {}});
}
bool MidiOutput::Transfer(void *data, DWORD size, bool write) {
    auto bytes = static_cast<unsigned char *>(data);
    ULONGLONG deadline = GetTickCount64() + (closing ? 1000 : Timeout);
    while (size && !failed) {
        OVERLAPPED operation{};
        operation.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!operation.hEvent) { error = MIDI_OUTPUT_HOST_ERROR; return false; }
        DWORD done = 0;
        BOOL ok = write ? WriteFile(pipe, bytes, std::min(size, 65536ul), &done, &operation)
                        : ReadFile(pipe, bytes, size, &done, &operation);
        if (!ok && GetLastError() == ERROR_IO_PENDING) {
            bool shortening = false;
            while (!failed && GetTickCount64() < deadline &&
                   WaitForSingleObject(operation.hEvent, 10) == WAIT_TIMEOUT) {
                if (stopping && !closing && !shortening) {
                    deadline = std::min(deadline, GetTickCount64() + 150);
                    shortening = true;
                }
            }
            if (failed || WaitForSingleObject(operation.hEvent, 0) != WAIT_OBJECT_0) {
                CancelIoEx(pipe, &operation);
                GetOverlappedResult(pipe, &operation, &done, TRUE);
                CloseHandle(operation.hEvent);
                error = MIDI_OUTPUT_TIMEOUT;
                return false;
            }
            ok = GetOverlappedResult(pipe, &operation, &done, FALSE);
        }
        CloseHandle(operation.hEvent);
        if (!ok || !done) { error = MIDI_OUTPUT_HOST_ERROR; return false; }
        bytes += done;
        size -= done;
    }
    return !size;
}
bool MidiOutput::Exchange(const Request &request) {
    operationKind = request.kind;
    Packet packet{request.kind, request.message, (DWORD)request.bytes.size()};
    DWORD result = 0;
    if (!Transfer(&packet, sizeof(packet), true) ||
        (!request.bytes.empty() && !Transfer(const_cast<unsigned char *>(request.bytes.data()),
                                            packet.size, true)) ||
        !Transfer(&result, sizeof(result), false)) return false;
    if (result) { error = result; return false; }
    ++completed;
    return true;
}
void MidiOutput::Run() {
    static std::atomic<unsigned> serial{0};
    auto name = std::wstring(LR"(\\.\pipe\MidiPlayer-)") + std::to_wstring(GetCurrentProcessId()) + L"-" +
                std::to_wstring(++serial);
    pipe = CreateNamedPipeW(name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
                           PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
                           1, 65536, 65536, 0, nullptr);
    if (pipe == INVALID_HANDLE_VALUE) error = MIDI_OUTPUT_HOST_ERROR;
    else {
        OVERLAPPED connection{};
        connection.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        BOOL connected = connection.hEvent && ConnectNamedPipe(pipe, &connection);
        DWORD connectionError = GetLastError();
        std::vector<wchar_t> executable(32768);
        GetModuleFileNameW(nullptr, executable.data(), (DWORD)executable.size());
        std::wstring command = L"\"" + std::wstring(executable.data()) + L"\" --midi-output-host \"" +
                               name + L"\" " + std::to_wstring(device);
        STARTUPINFOW startup{sizeof(startup)};
        startup.dwFlags = STARTF_USESHOWWINDOW;
        startup.wShowWindow = SW_HIDE;
        PROCESS_INFORMATION child{};
        if (!connection.hEvent || (!connected && connectionError != ERROR_IO_PENDING) ||
            !CreateProcessW(executable.data(), command.data(), nullptr, nullptr, FALSE,
                            CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, nullptr, &startup, &child)) {
            error = MIDI_OUTPUT_HOST_ERROR;
        } else {
            process = child.hProcess;
            job = CreateJobObjectW(nullptr, nullptr);
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
            limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            if (!job || !SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits,
                                                 sizeof(limits)) || !AssignProcessToJobObject(job, process))
                error = MIDI_OUTPUT_HOST_ERROR;
            else
                ResumeThread(child.hThread);
            CloseHandle(child.hThread);
            auto deadline = GetTickCount64() + Timeout;
            while (!error && !stopping && GetTickCount64() < deadline &&
                   WaitForSingleObject(connection.hEvent, 10) == WAIT_TIMEOUT &&
                   WaitForSingleObject(process, 0) == WAIT_TIMEOUT) {}
            if (!error && (stopping || WaitForSingleObject(connection.hEvent, 0) != WAIT_OBJECT_0))
                error = MIDI_OUTPUT_TIMEOUT;
        }
        // Complete/cancel pending connection before its OVERLAPPED leaves scope.
        if (connection.hEvent) {
            CancelIoEx(pipe, &connection);
            DWORD done;
            GetOverlappedResult(pipe, &connection, &done, TRUE);
            CloseHandle(connection.hEvent);
        }
        if (!error && !stopping && Exchange({Open, (DWORD)device, {}})) {
            while (!stopping && !failed) {
                Request request{};
                {
                    std::unique_lock<std::mutex> lock(mutex);
                    wake.wait(lock, [&] { return stopping || failed || !requests.empty(); });
                    if (stopping || failed) break;
                    request = std::move(requests.front());
                    requests.pop_front();
                    queuedBytes -= request.bytes.size();
                }
                if (!Exchange(request)) break;
            }
        }
    }
    if (error && !stopping && !failed) {
        failed = true;
        PostMessageW(window, WM_APP + 2, error,
                     (LPARAM)(((unsigned long long)operationKind << 32) | (DWORD)device));
    }
    if (process && !error && !failed) {
        closing = true;
        Exchange({CloseDevice, 0, shutdownReset});
    }
    // Killing only our helper also releases handles if a driver never returns.
    if (process) {
        TerminateProcess(process, 0);
        WaitForSingleObject(process, 500);
        CloseHandle(process);
    }
    if (job) CloseHandle(job);
    if (pipe != INVALID_HANDLE_VALUE) CloseHandle(pipe);
}
int RunMidiOutputHostFromCommandLine(bool testDrivers) {
    int count = 0;
    auto args = CommandLineToArgvW(GetCommandLineW(), &count);
    if (!args) return -1;
    if (count != 4 || std::wstring(args[1]) != L"--midi-output-host") { LocalFree(args); return -1; }
    std::wstring pipeName = args[2];
    int id = _wtoi(args[3]);
    LocalFree(args);
    HANDLE pipe = CreateFileW(pipeName.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                              OPEN_EXISTING, 0, nullptr);
    if (pipe == INVALID_HANDLE_VALUE) return 1;
    HMIDIOUT output = nullptr;
    struct Pending {
        MIDIHDR header{};
        std::vector<unsigned char> bytes;
    };
    std::vector<std::unique_ptr<Pending>> pending;
    bool simulated = testDrivers && id <= -3;
    while (true) {
        Packet packet{};
        if (!HostTransfer(pipe, &packet, sizeof(packet), false)) break;
        if (packet.size > 128 * 1024 * 1024) break;
        std::vector<unsigned char> bytes(packet.size);
        if (packet.size && !HostTransfer(pipe, bytes.data(), packet.size, false)) break;
        for (auto it = pending.begin(); it != pending.end();) {
            if (((*it)->header.dwFlags & MHDR_DONE) &&
                midiOutUnprepareHeader(output, &(*it)->header, sizeof(MIDIHDR)) == MMSYSERR_NOERROR)
                it = pending.erase(it);
            else ++it;
        }
        DWORD result = 0;
        if (simulated) {
            if ((id == -3 && packet.kind == Open) || (id == -4 && packet.kind == ShortMessage) ||
                (id == -5 && packet.kind == LongMessage) || (id == -6 && packet.kind == ResetDevice) ||
                (id == -12 && packet.kind == CloseDevice))
                Sleep(INFINITE);
            if (packet.kind == CloseDevice && id <= -8 && id >= -11) {
                const std::vector<unsigned char> gs{0xf0,0x41,0x10,0x42,0x12,0x40,0,0x7f,0,0x41,0xf7};
                const std::vector<unsigned char> xg{0xf0,0x43,0x10,0x4c,0,0,0x7e,0,0xf7};
                const std::vector<unsigned char> gm{0xf0,0x7e,0x7f,9,1,0xf7};
                const std::vector<unsigned char> empty;
                const auto &expected = id == -8 ? gs : id == -9 ? xg : id == -10 ? gm : empty;
                if (bytes != expected) result = MMSYSERR_INVALPARAM;
            }
        } else if (packet.kind == Open) {
            result = midiOutOpen(&output, id == -1 ? MIDI_MAPPER : (UINT)id, 0, 0, CALLBACK_NULL);
        } else if (!output) result = MMSYSERR_INVALHANDLE;
        else if (packet.kind == ShortMessage) result = midiOutShortMsg(output, packet.message);
        else if (packet.kind == LongMessage) {
            auto message = std::make_unique<Pending>();
            message->bytes = std::move(bytes);
            message->header.lpData = (LPSTR)message->bytes.data();
            message->header.dwBufferLength = (DWORD)message->bytes.size();
            result = midiOutPrepareHeader(output, &message->header, sizeof(MIDIHDR));
            if (!result) {
                result = midiOutLongMsg(output, &message->header, sizeof(MIDIHDR));
                if (result) midiOutUnprepareHeader(output, &message->header, sizeof(MIDIHDR));
                else pending.push_back(std::move(message));
            }
        } else if (packet.kind == CloseDevice) {
            result = midiOutReset(output);
            for (int channel = 0; channel < 16 && !result; ++channel) {
                for (int controller : {64, 123, 120}) {
                    result = midiOutShortMsg(output, 0xb0 | channel | (controller << 8));
                    if (result) break;
                }
            }
            if (!result && !bytes.empty()) {
                auto message = std::make_unique<Pending>();
                message->bytes = std::move(bytes);
                message->header.lpData = (LPSTR)message->bytes.data();
                message->header.dwBufferLength = (DWORD)message->bytes.size();
                result = midiOutPrepareHeader(output, &message->header, sizeof(MIDIHDR));
                if (!result) {
                    result = midiOutLongMsg(output, &message->header, sizeof(MIDIHDR));
                    if (result) midiOutUnprepareHeader(output, &message->header, sizeof(MIDIHDR));
                    else pending.push_back(std::move(message));
                }
            }
            // ACK only after queued SysEx really completes, not merely after midiOutLongMsg.
            while (!pending.empty() && !result) {
                for (auto it = pending.begin(); it != pending.end();) {
                    if (((*it)->header.dwFlags & MHDR_DONE) &&
                        midiOutUnprepareHeader(output, &(*it)->header, sizeof(MIDIHDR)) == MMSYSERR_NOERROR)
                        it = pending.erase(it);
                    else ++it;
                }
                if (!pending.empty()) Sleep(1);
            }
            if (!result) result = midiOutClose(output);
            if (!result) output = nullptr;
        } else if (packet.kind == ResetDevice) result = midiOutReset(output);
        else result = MMSYSERR_INVALPARAM;
        if (!HostTransfer(pipe, &result, sizeof(result), true)) break;
        if (packet.kind == CloseDevice) break;
    }
    // OS process teardown owns stuck-driver cleanup; do not wait in WinMM on exit.
    CloseHandle(pipe);
    ExitProcess(0);
}
