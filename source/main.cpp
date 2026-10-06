#include "player.h"
#include <algorithm>
#include <commctrl.h>
#include <commdlg.h>
#include <filesystem>
#include <set>
#include <shellapi.h>
#include <sstream>
#include <windows.h>
#include <fstream>
#include <iterator>
#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "comdlg32.lib")
namespace fs = std::filesystem;
constexpr wchar_t AppTitle[] = L"MIDI Player 1.02";
enum {
    OPEN = 100,
    STOP,
    PAUSE,
    PREV,
    PLAY,
    NEXT,
    LIST,
    KEYS,
    BOTH,
    GS,
    XG,
    GM,
    AUTORESET,
    CLEAR,
    EXITAPP,
    MINI,
    REMOVE,
    LOOP_ONE,
    LOOP_ALL,
    LOOP_OFF,
    FILEINFO,
    AUTORESET_XG,
    AUTORESET_GM,
    AUTORESET_OFF,
    SILENCE_SKIP,
    PORTVIEW = 200,
    DEVICE = 1000
};
struct Entry {
    fs::path path;
    std::shared_ptr<MidiSong> song;
};
HWND win, list;
int dragRow = -1, dragGap = -1;
std::unique_ptr<Player> player;
std::vector<Entry> playlist;
int current = -1, mode = 0, visiblePort = 0;
bool autoReset = true;
bool silenceSkip = false;
int autoResetKind = 0;
bool miniMode = false;
bool restoreMini = false;
int savedMode = 0;
int restoreSongIndex = -1;
std::wstring restoreSongPath;
int loopMode = 0; // 0: off, 1: current song, 2: playlist
WINDOWPLACEMENT savedPlacement{sizeof(WINDOWPLACEMENT)};
HFONT font, meterFont;
HMENU menu;
std::vector<HWND> buttons;
int width = 1000, height = 720;
std::wstring status = L"MIDIファイルやフォルダをここにドロップしてください";
std::vector<std::wstring> deviceNames;
HDC buffer = nullptr;
HBITMAP dib = nullptr, oldBitmap = nullptr;
int dibW = 0, dibH = 0;
fs::path playlistDirectory;
fs::path PlaylistFile(const wchar_t *name) {
    if (playlistDirectory.empty()) {
        std::vector<wchar_t> path(32768);
        DWORD length = GetModuleFileNameW(nullptr, path.data(), (DWORD)path.size());
        playlistDirectory = fs::path(std::wstring(path.data(), length)).parent_path();
    }
    return playlistDirectory / name;
}
void LoadSettings() {
    auto path = PlaylistFile(L"MidiPlayer.ini");
    auto read = [&](const wchar_t *section, const wchar_t *key, int fallback) {
        return (int)GetPrivateProfileIntW(section, key, fallback, path.c_str());
    };
    restoreSongIndex = read(L"Playlist", L"Index", -1);
    std::vector<wchar_t> songPath(32768);
    GetPrivateProfileStringW(L"Playlist", L"Path", L"", songPath.data(), (DWORD)songPath.size(), path.c_str());
    restoreSongPath = songPath.data();
    mode = std::clamp(read(L"Display", L"Mode", 0), 0, 2);
    visiblePort = std::clamp(read(L"Display", L"Port", 0), 0, 5);
    restoreMini = read(L"Display", L"Mini", 0) == 1;
    autoReset = read(L"Options", L"AutoReset", 1) != 0;
    autoResetKind = std::clamp(read(L"Options", L"AutoResetKind", 0), 0, 2);
    silenceSkip = read(L"Options", L"SilenceSkip", 0) != 0;
    player->SetSilenceSkip(silenceSkip);
    player->SetAutoReset(autoReset, autoResetKind);
    loopMode = std::clamp(read(L"Options", L"Loop", 0), 0, 2);
    for (int p = 0; p < 6; ++p) {
        auto key = L"Port" + std::to_wstring(p);
        int id = read(L"MIDIOutput", key.c_str(), player->GetDevice(p));
        wchar_t name[256]{};
        GetPrivateProfileStringW(L"MIDIOutput", (key + L"Name").c_str(), L"", name, 256, path.c_str());
        if (id >= 0 && *name) {
            auto found = std::find(deviceNames.begin(), deviceNames.end(), name);
            id = found == deviceNames.end() ? -2 : (int)(found - deviceNames.begin());
        }
        if (id < -2 || id >= (int)deviceNames.size())
            id = -2;
        if (id != player->GetDevice(p))
            player->SetDevice(p, id);
    }
}
bool SaveSettings() {
    auto path = PlaylistFile(L"MidiPlayer.ini.tmp");
    bool ok = true;
    auto write = [&](const wchar_t *section, const std::wstring &key, const std::wstring &value) {
        if (!WritePrivateProfileStringW(section, key.c_str(), value.c_str(), path.c_str()))
            ok = false;
    };
    for (int p = 0; p < 6; ++p) {
        int id = player->GetDevice(p);
        auto key = L"Port" + std::to_wstring(p);
        write(L"MIDIOutput", key, std::to_wstring(id));
        write(L"MIDIOutput", key + L"Name", id >= 0 && id < (int)deviceNames.size() ? deviceNames[id] : L"");
    }
    write(L"Display", L"Mode", std::to_wstring(miniMode ? savedMode : mode));
    write(L"Display", L"Port", std::to_wstring(visiblePort));
    write(L"Display", L"Mini", miniMode ? L"1" : L"0");
    write(L"Options", L"AutoReset", autoReset ? L"1" : L"0");
    write(L"Options", L"AutoResetKind", std::to_wstring(autoResetKind));
    write(L"Options", L"SilenceSkip", silenceSkip ? L"1" : L"0");
    write(L"Options", L"Loop", std::to_wstring(loopMode));
    auto state = player->Snapshot();
    int selected = list && IsWindow(list) ? ListView_GetNextItem(list, -1, LVNI_SELECTED) : -1;
    int remembered = (state.playing || state.paused) && current >= 0 ? current :
                     selected >= 0 ? selected : current;
    if (remembered < 0 || remembered >= (int)playlist.size()) remembered = -1;
    write(L"Playlist", L"Index", std::to_wstring(remembered));
    write(L"Playlist", L"Path", remembered >= 0 ? fs::absolute(playlist[remembered].path).wstring() : L"");
    WritePrivateProfileStringW(nullptr, nullptr, nullptr, path.c_str());
    if (ok)
        ok = MoveFileExW(path.c_str(), PlaylistFile(L"MidiPlayer.ini").c_str(),
                         MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
    if (!ok)
        status = L"MidiPlayer.ini の保存に失敗しました";
    return ok;
}
bool PlaylistError(const wchar_t *action) {
    status = std::wstring(L"プレイリスト") + action + L"に失敗しました (Windows error " +
             std::to_wstring(GetLastError()) + L")";
    return false;
}
bool SavePlaylist(bool empty = false) {
    std::wstring lines;
    if (!empty)
        for (const auto &entry : playlist)
            lines += fs::absolute(entry.path).wstring() + L"\r\n";
    int size = WideCharToMultiByte(CP_UTF8, 0, lines.data(), (int)lines.size(), nullptr, 0, nullptr, nullptr);
    std::string data("\xef\xbb\xbf"); // UTF-8 BOM makes Japanese paths readable in text editors.
    if (size) {
        data.resize(3 + size);
        WideCharToMultiByte(CP_UTF8, 0, lines.data(), (int)lines.size(), data.data() + 3, size, nullptr,
                            nullptr);
    }
    auto temporary = PlaylistFile(L"playlist.tmp");
    auto destination = PlaylistFile(L"playlist.txt");
    HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return PlaylistError(L"保存");
    DWORD written = 0;
    bool ok = WriteFile(file, data.data(), (DWORD)data.size(), &written, nullptr) && written == data.size();
    if (ok)
        ok = FlushFileBuffers(file) != FALSE;
    DWORD error = GetLastError();
    CloseHandle(file);
    if (!ok) {
        DeleteFileW(temporary.c_str());
        SetLastError(error);
        return PlaylistError(L"保存");
    }
    if (!MoveFileExW(temporary.c_str(), destination.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        error = GetLastError();
        DeleteFileW(temporary.c_str());
        SetLastError(error);
        return PlaylistError(L"保存");
    }
    return true;
}
bool BackupAndClearPlaylist() {
    auto source = PlaylistFile(L"playlist.txt"), backup = PlaylistFile(L"playlist.bak");
    DWORD attributes = GetFileAttributesW(source.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES) {
        if (!CopyFileW(source.c_str(), backup.c_str(), FALSE))
            return PlaylistError(L"バックアップ");
    } else if (GetLastError() != ERROR_FILE_NOT_FOUND)
        return PlaylistError(L"バックアップ");
    return SavePlaylist(true);
}
std::wstring Time(double v) {
    int s = (int)v;
    wchar_t b[32];
    swprintf_s(b, L"%02d:%02d", s / 60, s % 60);
    return b;
}
void Fill(HDC dc, RECT r, COLORREF color) {
    auto b = CreateSolidBrush(color);
    FillRect(dc, &r, b);
    DeleteObject(b);
}
void Text(HDC dc, RECT r, const std::wstring &s, COLORREF c = RGB(218, 229, 244),
          UINT flags = DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS) {
    SetTextColor(dc, c);
    DrawTextW(dc, s.c_str(), -1, &r, flags | DT_NOPREFIX);
}
void SelectCurrent() {
    if (current >= 0) {
        ListView_SetItemState(list, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
        ListView_SetItemState(list, current, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        ListView_EnsureVisible(list, current, FALSE);
    }
}
void RestorePlaylistSelection() {
    if (playlist.empty() || restoreSongIndex < 0) return;
    int index = -1;
    if (!restoreSongPath.empty())
        for (int i = 0; i < (int)playlist.size(); ++i)
            if (_wcsicmp(fs::absolute(playlist[i].path).c_str(), restoreSongPath.c_str()) == 0) {
                index = i;
                break;
            }
    if (index < 0) index = std::clamp(restoreSongIndex, 0, (int)playlist.size() - 1);
    current = index;
    SelectCurrent();
}
std::wstring EntryTitle(const Entry &entry) {
    auto title = entry.song->title;
    auto first = title.find_first_not_of(L" \t\r\n");
    auto last = title.find_last_not_of(L" \t\r\n");
    auto trimmed = first == std::wstring::npos ? L"" : title.substr(first, last - first + 1);
    return _wcsicmp(trimmed.c_str(), L"untitled") == 0 ? entry.path.filename().wstring() : title;
}
INT_PTR CALLBACK FileInfoProc(HWND dialog, UINT message, WPARAM w, LPARAM l) {
    if (message == WM_INITDIALOG) {
        auto path = reinterpret_cast<const fs::path *>(l);
        RECT client;
        GetClientRect(dialog, &client);
        int dialogWidth = client.right, dialogHeight = client.bottom;
        auto control = [&](const wchar_t *type, const wchar_t *text, DWORD style,
                           int x, int y, int cx, int cy, int id) {
            auto child = CreateWindowExW(type == std::wstring(L"EDIT") ? WS_EX_CLIENTEDGE : 0,
                type, text, WS_CHILD | WS_VISIBLE | style, x, y, cx, cy, dialog,
                (HMENU)(INT_PTR)id, GetModuleHandleW(nullptr), nullptr);
            SendMessageW(child, WM_SETFONT, (WPARAM)font, TRUE);
            return child;
        };
        control(L"STATIC", L"ファイル名", 0, 12, 12, dialogWidth - 24, 22, 0);
        auto name = control(L"EDIT", path->filename().c_str(), WS_TABSTOP | ES_READONLY | ES_AUTOHSCROLL,
                            12, 36, dialogWidth - 24, 28, 201);
        control(L"STATIC", L"フルパス（選択して Ctrl+C でコピーできます）", 0, 12, 74, dialogWidth - 24, 22, 0);
        control(L"EDIT", path->c_str(), WS_TABSTOP | ES_READONLY | ES_MULTILINE | ES_AUTOVSCROLL | WS_VSCROLL,
                12, 98, dialogWidth - 24, dialogHeight - 148, 202);
        control(L"BUTTON", L"閉じる", WS_TABSTOP | BS_DEFPUSHBUTTON,
                dialogWidth - 98, dialogHeight - 38, 86, 28, IDOK);
        RECT owner;
        GetWindowRect(win, &owner);
        RECT bounds;
        GetWindowRect(dialog, &bounds);
        SetWindowPos(dialog, nullptr, owner.left + (owner.right - owner.left - bounds.right + bounds.left) / 2,
                     owner.top + (owner.bottom - owner.top - bounds.bottom + bounds.top) / 2,
                     0, 0, SWP_NOSIZE | SWP_NOZORDER);
        SetFocus(name);
        SendMessageW(name, EM_SETSEL, 0, -1);
        return FALSE;
    }
    if ((message == WM_COMMAND && (LOWORD(w) == IDOK || LOWORD(w) == IDCANCEL)) || message == WM_CLOSE) {
        EndDialog(dialog, IDOK);
        return TRUE;
    }
    return FALSE;
}
void ShowFileInfo(int row) {
    if (row < 0 || row >= (int)playlist.size()) return;
    auto path = fs::absolute(playlist[row].path);
    struct Template {
        DLGTEMPLATE dialog;
        WORD menu = 0, windowClass = 0;
        wchar_t title[7] = L"ファイル情報";
    } layout{};
    layout.dialog.style = WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_MODALFRAME;
    layout.dialog.cx = 340;
    layout.dialog.cy = 142;
    DialogBoxIndirectParamW(GetModuleHandleW(nullptr), &layout.dialog, win, FileInfoProc, (LPARAM)&path);
}
void Start(int i) {
    if (i < 0 || i >= (int)playlist.size())
        return;
    current = i;
    player->Start(playlist[i].song);
    SelectCurrent();
    status = L"再生中";
    std::wstring title = EntryTitle(playlist[i]) + L" — " + AppTitle;
    SetWindowTextW(win, title.c_str());
    InvalidateRect(list, nullptr, FALSE);
    SavePlaylist();
}
void UpdateRow(int i) {
    auto &e = playlist[i];
    LVITEMW item{};
    item.mask = LVIF_TEXT;
    item.iItem = i;
    auto title = EntryTitle(e);
    item.pszText = title.data();
    ListView_InsertItem(list, &item);
    auto duration = Time(e.song->duration);
    ListView_SetItemText(list, i, 1, duration.data());
    auto format = L"SMF " + std::to_wstring(e.song->format);
    ListView_SetItemText(list, i, 2, format.data());
}
void EndPlaylistDrag() {
    dragRow = dragGap = -1;
    KillTimer(win, 2);
    LVINSERTMARK mark{sizeof(mark)};
    mark.iItem = -1;
    ListView_SetInsertMark(list, &mark);
    if (GetCapture() == win)
        ReleaseCapture();
}
void UpdatePlaylistDrag(POINT point) {
    ScreenToClient(list, &point);
    RECT client;
    GetClientRect(list, &client);
    dragGap = -1;
    LVINSERTMARK mark{sizeof(mark)};
    mark.iItem = -1;
    if (PtInRect(&client, point) && !playlist.empty()) {
        LVHITTESTINFO hit{};
        hit.pt = point;
        int row = ListView_HitTest(list, &hit);
        if (row >= 0) {
            RECT bounds;
            ListView_GetItemRect(list, row, &bounds, LVIR_BOUNDS);
            bool after = point.y >= (bounds.top + bounds.bottom) / 2;
            dragGap = row + (after ? 1 : 0);
            mark.iItem = row;
            mark.dwFlags = after ? LVIM_AFTER : 0;
        } else {
            RECT last;
            int end = (int)playlist.size() - 1;
            ListView_GetItemRect(list, end, &last, LVIR_BOUNDS);
            if (point.y >= last.bottom) {
                dragGap = end + 1;
                mark.iItem = end;
                mark.dwFlags = LVIM_AFTER;
            }
        }
    }
    ListView_SetInsertMark(list, &mark);
}
void MovePlaylistRow(int source, int gap) {
    int count = (int)playlist.size();
    if (source < 0 || source >= count || gap < 0 || gap > count)
        return;
    int target = gap > source ? gap - 1 : gap;
    if (source == target)
        return;
    auto remap = [&](int index) {
        if (index == source) return target;
        if (source < index && index <= target) return index - 1;
        if (target <= index && index < source) return index + 1;
        return index;
    };
    int selected = ListView_GetNextItem(list, -1, LVNI_SELECTED);
    auto original = playlist;
    auto moved = playlist[source];
    playlist.erase(playlist.begin() + source);
    playlist.insert(playlist.begin() + target, std::move(moved));
    if (!SavePlaylist()) {
        playlist = std::move(original);
        InvalidateRect(win, nullptr, FALSE);
        return;
    }
    current = remap(current);
    SendMessageW(list, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(list);
    for (int i = 0; i < count; ++i)
        UpdateRow(i);
    if (selected >= 0)
        ListView_SetItemState(list, remap(selected), LVIS_SELECTED | LVIS_FOCUSED,
                             LVIS_SELECTED | LVIS_FOCUSED);
    ListView_EnsureVisible(list, target, FALSE);
    SendMessageW(list, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(list, nullptr, TRUE);
    status = L"プレイリストの順番を変更しました";
    InvalidateRect(win, nullptr, FALSE);
}
void RemoveSelectedPlaylistRows() {
    std::vector<int> selected;
    for (int row = -1; (row = ListView_GetNextItem(list, row, LVNI_SELECTED)) >= 0;)
        selected.push_back(row);
    if (selected.empty())
        return;
    EndPlaylistDrag();
    std::wstring question = selected.size() == 1
        ? L"「" + EntryTitle(playlist[selected.front()]) + L"」をプレイリストから削除しますか？"
        : L"選択した " + std::to_wstring(selected.size()) + L" 曲をプレイリストから削除しますか？";
    question += L"\n\n元のMIDIファイルは削除されません。";
    if (MessageBoxW(win, question.c_str(), L"曲の削除確認",
                    MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES)
        return;
    auto original = playlist;
    for (auto row = selected.rbegin(); row != selected.rend(); ++row)
        playlist.erase(playlist.begin() + *row);
    if (!SavePlaylist()) {
        playlist = std::move(original);
        InvalidateRect(win, nullptr, FALSE);
        return;
    }
    if (std::binary_search(selected.begin(), selected.end(), current)) {
        player->Stop();
        current = -1;
        SetWindowTextW(win, AppTitle);
    } else if (current >= 0)
        current -= (int)(std::lower_bound(selected.begin(), selected.end(), current) - selected.begin());
    SendMessageW(list, WM_SETREDRAW, FALSE, 0);
    for (auto row = selected.rbegin(); row != selected.rend(); ++row)
        ListView_DeleteItem(list, *row);
    if (!playlist.empty()) {
        int next = std::min(selected.front(), (int)playlist.size() - 1);
        ListView_SetItemState(list, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
        ListView_SetItemState(list, next, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        ListView_EnsureVisible(list, next, FALSE);
    }
    SendMessageW(list, WM_SETREDRAW, TRUE, 0);
    status = L"プレイリストから削除: " + std::to_wstring(selected.size()) + L" 曲";
    InvalidateRect(list, nullptr, TRUE);
    InvalidateRect(win, nullptr, FALSE);
}
bool IsMidi(const fs::path &p) {
    auto ext = p.extension().wstring();
    std::transform(ext.begin(), ext.end(), ext.begin(), towlower);
    return ext == L".mid" || ext == L".midi";
}
void LoadPlaylist() {
    std::ifstream file(PlaylistFile(L"playlist.txt"), std::ios::binary);
    if (!file)
        return;
    file.seekg(0, std::ios::end);
    auto size = file.tellg();
    if (size < 0 || size > 16 * 1024 * 1024) {
        status = L"playlist.txt が大きすぎるため読み込めません";
        return;
    }
    file.seekg(0);
    std::string data((size_t)size, '\0');
    if (size && !file.read(data.data(), size)) {
        status = L"playlist.txt を読み込めません";
        return;
    }
    if (data.compare(0, 3, "\xef\xbb\xbf") == 0)
        data.erase(0, 3);
    if (data.empty())
        return;
    int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, data.data(), (int)data.size(), nullptr, 0);
    if (!count) {
        status = L"playlist.txt はUTF-8で保存してください";
        return;
    }
    std::wstring lines(count, L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, data.data(), (int)data.size(), lines.data(), count);
    std::wistringstream stream(lines);
    std::wstring line;
    std::set<fs::path> known;
    size_t skipped = 0;
    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == L'\r')
            line.pop_back();
        if (line.empty())
            continue;
        fs::path path(line);
        try {
            if (!path.is_absolute() || !IsMidi(path)) {
                ++skipped;
                continue;
            }
            path = fs::weakly_canonical(path);
            if (!known.insert(path).second)
                continue;
            playlist.push_back({path, std::make_shared<MidiSong>(ReadMidi(path))});
            UpdateRow((int)playlist.size() - 1);
        } catch (...) {
            ++skipped;
        }
    }
    status = L"保存したプレイリストを復元: " + std::to_wstring(playlist.size()) + L" 曲";
    if (skipped)
        status += L" (読み込み不可 " + std::to_wstring(skipped) + L" 件)";
}
void AddPaths(const std::vector<fs::path> &paths) {
    int first = (int)playlist.size();
    std::vector<fs::path> files;
    std::wstring errors;
    size_t failures = 0;
    std::set<fs::path> known;
    for (auto &e : playlist)
        known.insert(e.path);
    for (auto &p : paths) {
        std::error_code ec;
        if (fs::is_directory(p, ec)) {
            std::vector<fs::path> folder;
            fs::recursive_directory_iterator it(p, fs::directory_options::skip_permission_denied, ec), end;
            for (; it != end; it.increment(ec)) {
                if (ec) {
                    ec.clear();
                    continue;
                }
                if (it->is_regular_file(ec) && IsMidi(it->path()))
                    folder.push_back(it->path());
            }
            std::sort(folder.begin(), folder.end());
            files.insert(files.end(), folder.begin(), folder.end());
        } else if (IsMidi(p))
            files.push_back(p);
    }
    SetCursor(LoadCursor(nullptr, IDC_WAIT));
    for (auto &p : files) {
        std::error_code ec;
        auto full = fs::weakly_canonical(p, ec);
        if (ec)
            full = p;
        if (!known.insert(full).second)
            continue;
        try {
            auto song = std::make_shared<MidiSong>(ReadMidi(full));
            playlist.push_back({full, song});
            UpdateRow((int)playlist.size() - 1);
        } catch (const std::exception &e) {
            failures++;
            if (failures <= 5) {
                std::string err = e.what();
                errors += p.filename().wstring() + L": " + std::wstring(err.begin(), err.end()) + L"\n";
            }
        }
    }
    SetCursor(LoadCursor(nullptr, IDC_ARROW));
    if ((int)playlist.size() > first) {
        auto v = player->Snapshot();
        if (!v.playing && !v.paused)
            Start(first);
        status = L"追加: " + std::to_wstring(playlist.size() - first) + L" 曲";
    } else
        status = L"追加できる新しいMIDIファイルがありません";
    if ((int)playlist.size() > first)
        SavePlaylist();
    if (failures)
        MessageBoxW(win, (errors + L"読み込めなかったファイル: " + std::to_wstring(failures)).c_str(),
                    L"MIDI読み込み", MB_OK | MB_ICONWARNING);
    InvalidateRect(win, nullptr, FALSE);
}
void OpenFiles() {
    std::vector<wchar_t> b(65536);
    OPENFILENAMEW o{};
    o.lStructSize = sizeof(o);
    o.hwndOwner = win;
    o.lpstrFilter = L"MIDI files (*.mid;*.midi)\0*.mid;*.midi\0\0";
    o.lpstrFile = b.data();
    o.nMaxFile = (DWORD)b.size();
    o.Flags = OFN_FILEMUSTEXIST | OFN_EXPLORER | OFN_ALLOWMULTISELECT;
    if (GetOpenFileNameW(&o)) {
        std::vector<fs::path> paths;
        fs::path root = b.data();
        auto p = b.data() + wcslen(b.data()) + 1;
        if (!*p)
            paths.push_back(root);
        else
            while (*p) {
                paths.push_back(root / p);
                p += wcslen(p) + 1;
            }
        AddPaths(paths);
    }
}
void UpdateResetMenu() {
    const int ids[] = {AUTORESET, AUTORESET_XG, AUTORESET_GM};
    for (int kind = 0; kind < 3; ++kind)
        CheckMenuItem(menu, ids[kind], MF_BYCOMMAND |
                      (autoReset && autoResetKind == kind ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(menu, AUTORESET_OFF, MF_BYCOMMAND | (autoReset ? MF_UNCHECKED : MF_CHECKED));
}
void BuildMenu() {
    menu = CreateMenu();
    auto file = CreatePopupMenu();
    AppendMenuW(file, MF_STRING, OPEN, L"ファイルを追加...\tCtrl+O");
    AppendMenuW(file, MF_STRING, CLEAR, L"プレイリストをクリア");
    AppendMenuW(file, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(file, MF_STRING, EXITAPP, L"終了");
    AppendMenuW(menu, MF_POPUP, (UINT_PTR)file, L"ファイル");
    auto outputs = CreatePopupMenu();
    for (int p = 0; p < 6; p++) {
        auto sub = CreatePopupMenu();
        int base = DEVICE + p * 256;
        AppendMenuW(sub, MF_STRING, base, L"無効");
        AppendMenuW(sub, MF_STRING, base + 1, L"Windows既定 (MIDI Mapper)");
        for (int d = 0; d < (int)deviceNames.size(); d++)
            AppendMenuW(sub, MF_STRING, base + d + 2, deviceNames[d].c_str());
        int selected = player->GetDevice(p);
        CheckMenuRadioItem(sub, base, base + (UINT)deviceNames.size() + 1,
                           base + (selected == -2   ? 0
                                   : selected == -1 ? 1
                                                    : selected + 2),
                           MF_BYCOMMAND);
        auto label = L"ポート " + std::to_wstring(p) + L" (" + std::wstring(1, (wchar_t)(L'A' + p)) + L")";
        AppendMenuW(outputs, MF_POPUP, (UINT_PTR)sub, label.c_str());
    }
    AppendMenuW(menu, MF_POPUP, (UINT_PTR)outputs, L"MIDI出力");
    auto display = CreatePopupMenu();
    AppendMenuW(display, MF_STRING, LIST, L"プレイリスト");
    AppendMenuW(display, MF_STRING, KEYS, L"鍵盤 (16ch)");
    AppendMenuW(display, MF_STRING, BOTH, L"プレイリスト + 鍵盤");
    AppendMenuW(display, MF_STRING, MINI, L"ミニウインドウ / 元に戻す");
    AppendMenuW(display, MF_SEPARATOR, 0, nullptr);
    for (int p = 0; p < 6; p++) {
        auto s = L"鍵盤・メーター: ポート " + std::to_wstring(p);
        AppendMenuW(display, MF_STRING, PORTVIEW + p, s.c_str());
    }
    AppendMenuW(menu, MF_POPUP, (UINT_PTR)display, L"表示");
    auto opts = CreatePopupMenu();
    AppendMenuW(opts, MF_STRING, AUTORESET, L"曲の開始時にGS RESET");
    AppendMenuW(opts, MF_STRING, AUTORESET_XG, L"曲の開始時にXG RESET");
    AppendMenuW(opts, MF_STRING, AUTORESET_GM, L"曲の開始時にGM RESET");
    AppendMenuW(opts, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(opts, MF_STRING, AUTORESET_OFF, L"曲の開始時にRESETしない");
    AppendMenuW(opts, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(opts, MF_STRING | (silenceSkip ? MF_CHECKED : MF_UNCHECKED),
                SILENCE_SKIP, L"10秒以上無音なら曲を終了");
    AppendMenuW(menu, MF_POPUP, (UINT_PTR)opts, L"オプション");
    UpdateResetMenu();
    SetMenu(win, menu);
}
int CombinedListWidth() {
    return std::max(280, width * 30 / 100);
}
void Layout() {
    if (!miniMode && mode == 2 && !IsZoomed(win)) {
        RECT bounds;
        GetWindowRect(win, &bounds);
        if (bounds.right - bounds.left < 1080) {
            SetWindowPos(win, nullptr, 0, 0, 1080, bounds.bottom - bounds.top,
                         SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
            return;
        }
    }
    int bottom = height - 152;
    int listW = mode == 2 ? CombinedListWidth() : width;
    ShowWindow(list, mode == 1 ? SW_HIDE : SW_SHOW);
    MoveWindow(list, 8, 8, listW - 16, std::max(10, bottom - 16), TRUE);
    ListView_SetColumnWidth(list, 0, std::max(120, listW - (mode == 2 ? 105 : 170)));
    ListView_SetColumnWidth(list, 1, 65);
    ListView_SetColumnWidth(list, 2, mode == 2 ? 0 : 65);
    if (miniMode) {
        ListView_SetColumnWidth(list, 0, std::max(120, listW - 110));
        ListView_SetColumnWidth(list, 1, 70);
        ListView_SetColumnWidth(list, 2, 0);
    }
    int x = 12, y = height - 40;
    for (size_t i = 0; i < buttons.size(); i++) {
        bool visible = (i < 8 || i > 10) && (!miniMode || i < 5 || i >= 11);
        ShowWindow(buttons[i], visible ? SW_SHOW : SW_HIDE);
        if (!visible)
            continue;
        int w = i >= 12 ? 78 : i == 11 ? 58 : i >= 5 ? 72 : miniMode ? 56 : 64;
        int buttonY = miniMode ? height - 34 : y;
        MoveWindow(buttons[i], x, buttonY, w, 28, TRUE);
        if (i >= 12)
            SendMessageW(buttons[i], BM_SETCHECK, (i == 12 ? loopMode == 1 :
                          i == 13 ? loopMode == 2 : loopMode == 0) ? BST_CHECKED : BST_UNCHECKED, 0);
        x += w + 5;
    }
    SetWindowTextW(buttons[11], miniMode ? L"戻す" : L"Mini");
    CheckMenuItem(menu, MINI, MF_BYCOMMAND | (miniMode ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuRadioItem(menu, LIST, BOTH, mode == 0 ? LIST : mode == 1 ? KEYS : BOTH, MF_BYCOMMAND);
    CheckMenuRadioItem(menu, PORTVIEW, PORTVIEW + 5, PORTVIEW + visiblePort, MF_BYCOMMAND);
    InvalidateRect(win, nullptr, FALSE);
}
void ToggleMini() {
    if (!miniMode) {
        savedMode = mode;
        savedPlacement.length = sizeof(savedPlacement);
        GetWindowPlacement(win, &savedPlacement);
        miniMode = true;
        mode = 0;
        // WINDOWPLACEMENT uses workspace coordinates and preserves the monitor/restore position.
        WINDOWPLACEMENT compactPlacement = savedPlacement;
        compactPlacement.showCmd = IsWindowVisible(win) ? SW_SHOWNORMAL : SW_HIDE;
        compactPlacement.flags = 0;
        compactPlacement.rcNormalPosition.right = compactPlacement.rcNormalPosition.left + 720;
        compactPlacement.rcNormalPosition.bottom = compactPlacement.rcNormalPosition.top + 324;
        SetWindowPlacement(win, &compactPlacement);
    } else {
        miniMode = false;
        mode = savedMode;
        SetWindowPlacement(win, &savedPlacement);
    }
    Layout();
    SelectCurrent();
}
RECT KeyboardArea() {
    return {mode == 2 ? CombinedListWidth() : 8, 8, width - 8, height - 160};
}
int ChannelAt(int x, int y) {
    if (miniMode || !mode)
        return -1;
    auto area = KeyboardArea();
    int top = area.top + 32, row = std::max(12, (int)(area.bottom - top) / 16);
    if (x < area.left + 4 || x >= area.left + 41 || y < top || y >= top + row * 16)
        return -1;
    int c = (y - top) / row;
    return (y - top) % row < row - 2 ? c : -1;
}
bool Black(int n) {
    int k = n % 12;
    return k == 1 || k == 3 || k == 6 || k == 8 || k == 10;
}
void Keyboard(HDC dc, RECT area, const PlayerView &v) {
    bool overlay = visiblePort <= 1 && v.multiPortMeters;
    if (overlay) {
        Text(dc, {area.left + 8, area.top, area.left + 90, area.top + 28}, L"PORT 0", RGB(65, 210, 168));
        Text(dc, {area.left + 96, area.top, area.left + 178, area.top + 28}, L"PORT 1", RGB(255, 183, 75));
        Text(dc, {area.left + 184, area.top, area.right, area.top + 28},
             L"番号クリック: PORT " + std::to_wstring(visiblePort) + L" のミュート");
    } else {
        Text(dc, {area.left + 8, area.top, area.right, area.top + 28},
             L"PORT " + std::to_wstring(visiblePort) + L"  ·  16 CHANNELS  ·  番号クリックでミュート");
    }
    auto noteColor = [&](int channel, int note, COLORREF background) {
        COLORREF color = background;
        int first = overlay ? 0 : visiblePort, last = overlay ? 1 : visiblePort;
        for (int port = first; port <= last; ++port)
            if (v.notes[port][channel][note])
                color = port == 1 ? RGB(255, 183, 75) : RGB(65, 210, 168);
        return color;
    };
    int top = area.top + 32, available = area.bottom - top;
    int row = std::max(12, available / 16);
    int left = area.left + (overlay ? 121 : 83), right = area.right - 12;
    int whiteCount = 0;
    for (int n = 0; n < 128; n++)
        if (!Black(n))
            whiteCount++;
    for (int c = 0; c < 16; c++) {
        int y = top + c * row;
        RECT label{area.left + 4, y, area.left + 41, y + row - 2};
        bool muted = v.muted[visiblePort][c];
        Fill(dc, label, muted ? RGB(97, 48, 45) : RGB(31, 47, 63));
        Text(dc, label, std::to_wstring(c + 1), muted ? RGB(255, 168, 130) : RGB(140, 190, 210),
             DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        auto programText = [&](int port) {
            int program = v.programs[port][c];
            return program < 0 ? std::wstring(L"—") : std::to_wstring(program + 1);
        };
        Fill(dc, {area.left + 45, y, left - 4, y + row - 2}, RGB(23, 35, 49));
        if (overlay) {
            Text(dc, {area.left + 45, y, area.left + 80, y + row - 2}, programText(0),
                 RGB(65, 210, 168), DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            Text(dc, {area.left + 81, y, left - 4, y + row - 2}, programText(1),
                 RGB(255, 183, 75), DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        } else {
            Text(dc, {area.left + 45, y, left - 4, y + row - 2}, programText(visiblePort),
                 RGB(65, 210, 168), DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
        int widx = 0;
        for (int n = 0; n < 128; n++)
            if (!Black(n)) {
                int x = left + widx * (right - left) / whiteCount,
                    x2 = left + (widx + 1) * (right - left) / whiteCount;
                Fill(dc, {x, y, x2 - 1, y + row - 3},
                     noteColor(c, n, RGB(210, 220, 229)));
                widx++;
            }
        widx = 0;
        for (int n = 0; n < 128; n++) {
            if (!Black(n)) {
                widx++;
                continue;
            }
            int x = left + widx * (right - left) / whiteCount;
            int kw = std::max(2, ((right - left) * 3 + whiteCount * 5 / 2) / (whiteCount * 5));
            Fill(dc, {x - kw / 2, y, x + (kw + 1) / 2, y + std::max(5, (row - 3) * 60 / 100)},
                 noteColor(c, n, RGB(24, 32, 45)));
        }
    }
}
void Indicator(HDC dc, const PlayerView &v, int port, int top, bool compact) {
    Fill(dc, {0, top, 240, top + (compact ? 53 : 108)}, RGB(27, 39, 55));
    const auto &display = v.displays[port];
    COLORREF color = port == 1 ? RGB(255, 183, 75) : RGB(57, 213, 165);
    SelectObject(dc, meterFont);
    if (!display.text.empty() && v.position < display.textUntil) {
        size_t offset = display.text.size() <= 16 ? 0 : std::min(display.text.size() - 16,
            (size_t)(std::max(0.0, v.position - display.textStart) * 4));
        Text(dc, {12, top, 240, top + 15},
             L"PORT " + std::to_wstring(port) + L" · " + display.text.substr(offset, 16), color);
    } else {
        Text(dc, {12, top, 240, top + 15}, L"CHANNEL ACTIVITY · PORT " + std::to_wstring(port),
             RGB(136, 173, 198));
    }
    if (v.position < display.dotsUntil) {
        int pitch = compact ? 2 : 4;
        for (int y = 0; y < 16; ++y)
            for (int x = 0; x < 16; ++x) {
                bool on = display.dots[(x / 5) * 16 + y] & (1 << (4 - x % 5));
                Fill(dc, {12 + x * 14, top + 16 + y * pitch,
                          22 + x * 14, top + 16 + y * pitch + pitch - 1},
                     on ? color : RGB(44, 62, 78));
            }
    } else {
        int steps = compact ? 6 : 12;
        int baseY = compact ? top + 38 : top + 83;
        for (int c = 0; c < 16; ++c) {
            int x = 12 + c * 14;
            for (int step = 0; step < steps; ++step) {
                int y = baseY - step * 4;
                bool on = v.levels[port][c] * steps > step;
                Fill(dc, {x, y, x + 10, y + 3},
                     on ? (v.muted[port][c] ? RGB(160, 105, 91)
                           : step >= steps - 2 ? RGB(255, 177, 79) : RGB(57, 213, 165))
                        : RGB(44, 62, 78));
            }
            Text(dc, {x - 1, baseY + 4, x + 13, baseY + 17}, std::to_wstring(c + 1),
                 RGB(149, 173, 193), DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
    }
    SelectObject(dc, font);
}
void Paint(HDC target) {
    if (width <= 0 || height <= 0)
        return;
    if (!buffer)
        buffer = CreateCompatibleDC(target);
    if (dibW != width || dibH != height) {
        if (dib) {
            SelectObject(buffer, oldBitmap);
            DeleteObject(dib);
        }
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = width;
        bi.bmiHeader.biHeight = -height;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        void *pixels;
        dib = CreateDIBSection(target, &bi, DIB_RGB_COLORS, &pixels, nullptr, 0);
        if (!dib)
            return;
        oldBitmap = (HBITMAP)SelectObject(buffer, dib);
        dibW = width;
        dibH = height;
    }
    auto dc = buffer;
    SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    Fill(dc, {0, 0, width, height}, RGB(18, 26, 39));
    auto v = player->Snapshot();
    int bottom = height - 152;
    if (mode)
        Keyboard(dc, KeyboardArea(), v);
    else if (playlist.empty())
        Text(dc, {20, 40, width - 20, bottom - 20}, L".mid またはフォルダをドロップして再生",
             RGB(140, 169, 195));
    Fill(dc, {0, bottom, width, height}, RGB(27, 39, 55));
    int meterW = 240;
    int rows = v.multiPortMeters ? 2 : 1;
    for (int row = 0; row < rows; ++row) {
        int port = rows == 2 ? 1 - row : visiblePort;
        int top = bottom + (rows == 2 ? 3 + row * 53 : 5);
        Indicator(dc, v, port, top, rows == 2);
    }
    int x = meterW + 12;
    std::wstring title = current >= 0 ? EntryTitle(playlist[current]) : AppTitle;
    Text(dc, {x, bottom + 8, width - 16, bottom + 36}, title, RGB(236, 243, 252));
    std::wstring state = v.paused ? L"一時停止" : v.playing ? L"再生中" : L"停止";
    if (loopMode == 1)
        state += L"  [1曲ループ]";
    else if (loopMode == 2)
        state += L"  [全曲ループ]";
    auto dur = current >= 0 ? playlist[current].song->duration : 0;
    std::wstring info =
        state + L"  " + Time(v.position) + L" / " + Time(dur) + L"    BPM " + std::to_wstring(v.bpm);
    if (current >= 0)
        info += L"    SMF " + std::to_wstring(playlist[current].song->format) + L"    " +
                std::to_wstring(current + 1) + L" / " + std::to_wstring(playlist.size());
    Text(dc, {x, bottom + 38, width - 16, bottom + 60}, info, RGB(87, 218, 185));
    RECT progress{x, bottom + 68, width - 20, bottom + 74};
    Fill(dc, progress, RGB(52, 68, 84));
    progress.right = progress.left + (int)((width - 20 - x) * (dur > 0 ? v.position / dur : 0));
    Fill(dc, progress, RGB(57, 213, 165));
    Text(dc, {x, bottom + 81, width - 16, bottom + 106}, status, RGB(155, 177, 197));
    BitBlt(target, 0, 0, width, height, dc, 0, 0, SRCCOPY);
}
void Command(int id) {
    if (id >= DEVICE && id < DEVICE + 6 * 256) {
        int p = (id - DEVICE) / 256, d = (id - DEVICE) % 256;
        player->SetDevice(p, d == 0 ? -2 : d == 1 ? -1 : d - 2);
        CheckMenuRadioItem(menu, DEVICE + p * 256, DEVICE + p * 256 + (UINT)deviceNames.size() + 1, id,
                           MF_BYCOMMAND);
        status = L"MIDI出力を変更しました。再生ボタンで曲の先頭から再生します";
        SaveSettings();
        InvalidateRect(win, nullptr, FALSE);
        return;
    }
    if (id >= PORTVIEW && id < PORTVIEW + 6) {
        visiblePort = id - PORTVIEW;
        SaveSettings();
        Layout();
        return;
    }
    switch (id) {
    case OPEN:
        OpenFiles();
        break;
    case STOP:
        player->Stop();
        status = L"停止";
        break;
    case PAUSE: {
        auto v = player->Snapshot();
        if (v.paused)
            player->Resume();
        else
            player->Pause();
        break;
    }
    case PLAY: {
        auto v = player->Snapshot();
        if (v.paused)
            player->Resume();
        else if (!v.playing) {
            int i = ListView_GetNextItem(list, -1, LVNI_SELECTED);
            Start(i >= 0 ? i : current >= 0 ? current : 0);
        }
        SavePlaylist();
        break;
    }
    case PREV:
        Start(std::max(0, current - 1));
        break;
    case NEXT:
        if (current + 1 < (int)playlist.size())
            Start(current + 1);
        else {
            player->Stop();
            status = L"プレイリストの最後です";
        }
        break;
    case LIST:
        if (miniMode)
            ToggleMini();
        mode = 0;
        Layout();
        break;
    case KEYS:
        if (miniMode)
            ToggleMini();
        mode = 1;
        Layout();
        break;
    case BOTH:
        if (miniMode)
            ToggleMini();
        mode = 2;
        Layout();
        break;
    case LOOP_ONE:
    case LOOP_ALL:
    case LOOP_OFF:
        loopMode = id == LOOP_ONE ? 1 : id == LOOP_ALL ? 2 : 0;
        SaveSettings();
        Layout();
        break;
    case MINI:
        ToggleMini();
        break;
    case GS:
    case XG:
    case GM:
        player->Reset(id - GS);
        status = id == GS ? L"GS RESET送信" : id == XG ? L"XG RESET送信" : L"GM RESET送信";
        break;
    case AUTORESET:
    case AUTORESET_XG:
    case AUTORESET_GM:
    case AUTORESET_OFF: {
        int kind = id == AUTORESET_XG ? 1 : id == AUTORESET_GM ? 2 : 0;
        if (id == AUTORESET_OFF)
            autoReset = false;
        else {
            autoReset = !(autoReset && autoResetKind == kind);
            autoResetKind = kind;
        }
        player->SetAutoReset(autoReset, autoResetKind);
        UpdateResetMenu();
        SaveSettings();
        break;
    }
    case CLEAR:
        if (!BackupAndClearPlaylist()) {
            InvalidateRect(win, nullptr, FALSE);
            break;
        }
        player->Stop();
        playlist.clear();
        current = -1;
        ListView_DeleteAllItems(list);
        SetWindowTextW(win, AppTitle);
        status = L"プレイリストをクリアしました";
        break;
    case FILEINFO:
        ShowFileInfo(ListView_GetNextItem(list, -1, LVNI_SELECTED));
        break;
    case REMOVE:
        RemoveSelectedPlaylistRows();
        break;
    case SILENCE_SKIP:
        silenceSkip = !silenceSkip;
        player->SetSilenceSkip(silenceSkip);
        CheckMenuItem(menu, SILENCE_SKIP, MF_BYCOMMAND | (silenceSkip ? MF_CHECKED : MF_UNCHECKED));
        SaveSettings();
        break;
    case EXITAPP:
        DestroyWindow(win);
        break;
    }
    if (id == LIST || id == KEYS || id == BOTH || id == MINI || id == AUTORESET)
        SaveSettings();
    InvalidateRect(win, nullptr, FALSE);
}
LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    switch (msg) {
    case WM_CREATE: {
        win = h;
        font = CreateFontW(-15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                           CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Yu Gothic UI");
        for (UINT d = 0; d < std::min(254u, midiOutGetNumDevs()); d++) {
            MIDIOUTCAPSW caps{};
            if (!midiOutGetDevCapsW(d, &caps, sizeof(caps)))
                deviceNames.push_back(caps.szPname);
            else
                deviceNames.push_back(L"MIDI device " + std::to_wstring(d));
        }
        player = std::make_unique<Player>(h);
        LoadSettings();
        meterFont =
            CreateFontW(-11, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                        CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        BuildMenu();
        list = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
                               WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SHOWSELALWAYS, 0, 0, 10, 10, h,
                               (HMENU)10, GetModuleHandle(nullptr), nullptr);
        SendMessageW(list, WM_SETFONT, (WPARAM)font, TRUE);
        ListView_SetExtendedListViewStyle(list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
        ListView_SetBkColor(list, RGB(18, 26, 39));
        ListView_SetTextBkColor(list, RGB(18, 26, 39));
        ListView_SetTextColor(list, RGB(220, 231, 243));
        const wchar_t *cols[] = {L"曲名", L"時間", L"形式"};
        for (int i = 0; i < 3; i++) {
            LVCOLUMNW col{};
            col.mask = LVCF_TEXT | LVCF_WIDTH;
            col.pszText = (LPWSTR)cols[i];
            col.cx = 100;
            ListView_InsertColumn(list, i, &col);
        }
        const wchar_t *labels[] = {L"停止", L"一時停止", L"前へ",     L"再生",     L"次へ",     L"リスト",
                                   L"鍵盤", L"両方",     L"GS RESET", L"XG RESET", L"GM RESET", L"Mini",
                                   L"1曲ループ", L"全曲ループ", L"ループ解除"};
        const int ids[] = {STOP, PAUSE, PREV, PLAY, NEXT, LIST, KEYS, BOTH, GS, XG, GM, MINI, LOOP_ONE, LOOP_ALL, LOOP_OFF};
        for (int i = 0; i < 15; i++) {
            auto b = CreateWindowW(L"BUTTON", labels[i], WS_CHILD | WS_VISIBLE | (i >= 12 ? BS_AUTOCHECKBOX | BS_PUSHLIKE : BS_PUSHBUTTON), 0, 0, 10, 10,
                                   h, (HMENU)(INT_PTR)ids[i], GetModuleHandle(nullptr), nullptr);
            SendMessageW(b, WM_SETFONT, (WPARAM)font, TRUE);
            buttons.push_back(b);
        }
        DragAcceptFiles(h, TRUE);
        LoadPlaylist();
        RestorePlaylistSelection();
        SetTimer(h, 1, 33, nullptr);
        return 0;
    }
    case WM_SIZE:
        width = LOWORD(l);
        height = HIWORD(l);
        if (list)
            Layout();
        return 0;
    case WM_GETMINMAXINFO:
        ((MINMAXINFO *)l)->ptMinTrackSize = miniMode ? POINT{660, 274} : POINT{mode == 2 ? 1080 : 920, 604};
        return 0;
    case WM_MOUSEMOVE:
        if (dragRow >= 0) {
            POINT point{(short)LOWORD(l), (short)HIWORD(l)};
            ClientToScreen(h, &point);
            UpdatePlaylistDrag(point);
            SetCursor(LoadCursor(nullptr, dragGap >= 0 ? IDC_SIZEALL : IDC_NO));
            return 0;
        }
        break;
    case WM_LBUTTONUP:
        if (dragRow >= 0) {
            POINT point{(short)LOWORD(l), (short)HIWORD(l)};
            ClientToScreen(h, &point);
            UpdatePlaylistDrag(point);
            int source = dragRow, gap = dragGap;
            EndPlaylistDrag();
            MovePlaylistRow(source, gap);
            SetFocus(list);
            return 0;
        }
        break;
    case WM_KEYDOWN:
        if (w == VK_ESCAPE && dragRow >= 0) {
            EndPlaylistDrag();
            SetFocus(list);
            return 0;
        }
        break;
    case WM_CANCELMODE:
    case WM_CAPTURECHANGED:
        if (dragRow >= 0)
            EndPlaylistDrag();
        break;
    case WM_LBUTTONDOWN: {
        int channel = ChannelAt((short)LOWORD(l), (short)HIWORD(l));
        if (channel >= 0) {
            player->ToggleMute(visiblePort, channel);
            status = L"ポート " + std::to_wstring(visiblePort) + L" / CH " + std::to_wstring(channel + 1) +
                     (player->Snapshot().muted[visiblePort][channel] ? L" ミュート" : L" ミュート解除");
            InvalidateRect(h, nullptr, FALSE);
        }
        return 0;
    }
    case WM_SETCURSOR: {
        if (LOWORD(l) == HTCLIENT) {
            POINT point;
            GetCursorPos(&point);
            ScreenToClient(h, &point);
            if (ChannelAt(point.x, point.y) >= 0) {
                SetCursor(LoadCursor(nullptr, IDC_HAND));
                return TRUE;
            }
        }
        break;
    }
    case WM_DROPFILES: {
        HDROP drop = (HDROP)w;
        UINT count = DragQueryFileW(drop, 0xffffffff, nullptr, 0);
        std::vector<fs::path> paths;
        for (UINT i = 0; i < count; i++) {
            UINT n = DragQueryFileW(drop, i, nullptr, 0);
            std::wstring p(n + 1, L'\0');
            DragQueryFileW(drop, i, p.data(), n + 1);
            p.resize(n);
            paths.emplace_back(p);
        }
        DragFinish(drop);
        AddPaths(paths);
        return 0;
    }
    case WM_CONTEXTMENU: {
        if ((HWND)w != list)
            break;
        int infoRow = -1;
        POINT point{(short)LOWORD(l), (short)HIWORD(l)};
        if (point.x == -1 && point.y == -1) {
            int row = ListView_GetNextItem(list, -1, LVNI_SELECTED);
            if (row < 0)
                return 0;
            infoRow = row;
            RECT bounds;
            ListView_GetItemRect(list, row, &bounds, LVIR_BOUNDS);
            point = {bounds.left + 20, bounds.bottom};
            ClientToScreen(list, &point);
        } else {
            LVHITTESTINFO hit{};
            hit.pt = point;
            ScreenToClient(list, &hit.pt);
            int row = ListView_HitTest(list, &hit);
            if (row < 0)
                return 0;
            infoRow = row;
            if (!ListView_GetItemState(list, row, LVIS_SELECTED)) {
                ListView_SetItemState(list, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
                ListView_SetItemState(list, row, LVIS_SELECTED | LVIS_FOCUSED,
                                     LVIS_SELECTED | LVIS_FOCUSED);
            }
        }
        SetFocus(list);
        auto popup = CreatePopupMenu();
        AppendMenuW(popup, MF_STRING, FILEINFO, L"ファイル情報...");
        AppendMenuW(popup, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(popup, MF_STRING, REMOVE, L"削除\tDel");
        int command = TrackPopupMenu(popup, TPM_RETURNCMD | TPM_RIGHTBUTTON,
                                     point.x, point.y, 0, h, nullptr);
        DestroyMenu(popup);
        if (command == FILEINFO)
            ShowFileInfo(infoRow);
        else if (command)
            Command(command);
        return 0;
    }
    case WM_COMMAND:
        Command(LOWORD(w));
        return 0;
    case WM_NOTIFY: {
        auto n = (NMHDR *)l;
        if (n->hwndFrom == list && n->code == LVN_KEYDOWN) {
            if (((NMLVKEYDOWN *)l)->wVKey == VK_DELETE)
                Command(REMOVE);
            return 0;
        }
        if (n->hwndFrom == list && n->code == LVN_BEGINDRAG) {
            auto item = (NMLISTVIEW *)l;
            if (item->iItem >= 0) {
                dragRow = item->iItem;
                SetFocus(h);
                SetCapture(h);
                ListView_SetInsertMarkColor(list, RGB(57, 213, 165));
                POINT point;
                GetCursorPos(&point);
                UpdatePlaylistDrag(point);
                SetTimer(h, 2, 100, nullptr);
            }
            return 0;
        }
        if (n->hwndFrom == list && n->code == NM_CUSTOMDRAW) {
            auto draw = (NMLVCUSTOMDRAW *)l;
            if (draw->nmcd.dwDrawStage == CDDS_PREPAINT)
                return CDRF_NOTIFYITEMDRAW;
            if (draw->nmcd.dwDrawStage == CDDS_ITEMPREPAINT && (int)draw->nmcd.dwItemSpec == current) {
                auto state = player->Snapshot();
                if (state.playing || state.paused) {
                    int row = (int)draw->nmcd.dwItemSpec;
                    RECT bounds;
                    ListView_GetItemRect(list, row, &bounds, LVIR_BOUNDS);
                    auto dc = draw->nmcd.hdc;
                    Fill(dc, bounds, state.paused ? RGB(73, 64, 39) : RGB(32, 84, 76));
                    SelectObject(dc, font);
                    SetBkMode(dc, TRANSPARENT);
                    for (int column = 0; column < 3; ++column) {
                        if (!ListView_GetColumnWidth(list, column))
                            continue;
                        RECT cell;
                        ListView_GetSubItemRect(list, row, column, LVIR_LABEL, &cell);
                        cell.right = std::min(cell.right, cell.left + ListView_GetColumnWidth(list, column));
                        cell.left += 4;
                        cell.right -= 3;
                        wchar_t value[2048];
                        ListView_GetItemText(list, row, column, value, 2048);
                        Text(dc, cell, value, RGB(231, 255, 245));
                    }
                    if (ListView_GetItemState(list, row, LVIS_SELECTED))
                        DrawFocusRect(dc, &bounds);
                    return CDRF_SKIPDEFAULT;
                }
            }
            return CDRF_DODEFAULT;
        }
        if (n->hwndFrom == list && n->code == NM_DBLCLK) {
            auto item = (NMITEMACTIVATE *)l;
            if (item->iItem >= 0)
                Start(item->iItem);
        }
        return 0;
    }
    case WM_SONG_DONE:
        if (player->IsCompleted(w)) {
            if (loopMode == 1 && current >= 0 && current < (int)playlist.size())
                Start(current);
            else if (current + 1 < (int)playlist.size())
                Start(current + 1);
            else if (loopMode == 2 && !playlist.empty())
                Start(0);
            else
                status = L"プレイリストの再生が終了しました";
        }
        return 0;
    case WM_PLAYER_ERROR: {
        int device = (int)l;
        std::wstring name = device == -1 ? L"MIDI Mapper" :
            device >= 0 && device < (int)deviceNames.size() ? deviceNames[device] : L"MIDI出力";
        if (w == MIDI_OUTPUT_TIMEOUT) {
            unsigned operation = (unsigned)((unsigned long long)l >> 32);
            const wchar_t *stage = operation == 1 ? L"出力を開く処理" : operation == 2 ? L"ノート送信" :
                                   operation == 3 ? L"SysEx送信" : operation == 4 ? L"消音処理" : L"出力の起動";
            status = name + L": " + stage + L"が応答しないため停止しました。接続を確認し出力を選び直してください";
        }
        else if (w == MIDI_OUTPUT_HOST_ERROR)
            status = name + L": 出力プロセスの通信に失敗しました。出力を選び直してください";
        else {
            wchar_t err[256]{};
            midiOutGetErrorTextW((MMRESULT)w, err, 256);
            status = name + L": " + err;
        }
        return 0;
    }
    case WM_TIMER:
        if (w == 2 && dragRow >= 0) {
            POINT point;
            GetCursorPos(&point);
            POINT local = point;
            ScreenToClient(list, &local);
            RECT client;
            GetClientRect(list, &client);
            if (PtInRect(&client, local)) {
                int direction = local.y < 40 ? -1 : local.y > client.bottom - 24 ? 1 : 0;
                if (direction) {
                    int top = ListView_GetTopIndex(list);
                    int row = std::clamp(top + (direction < 0 ? -1 : ListView_GetCountPerPage(list)),
                                         0, (int)playlist.size() - 1);
                    ListView_EnsureVisible(list, row, FALSE);
                }
            }
            UpdatePlaylistDrag(point);
            return 0;
        }
        InvalidateRect(h, nullptr, FALSE);
        InvalidateRect(list, nullptr, FALSE);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        auto dc = BeginPaint(h, &ps);
        Paint(dc);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_DESTROY:
        EndPlaylistDrag();
        KillTimer(h, 1);
        SaveSettings();
        player.reset();
        if (buffer) {
            if (dib) {
                SelectObject(buffer, oldBitmap);
                DeleteObject(dib);
            }
            DeleteDC(buffer);
        }
        DeleteObject(font);
        DeleteObject(meterFont);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, msg, w, l);
}
int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, LPWSTR, int show) {
    int hostResult = RunMidiOutputHostFromCommandLine();
    if (hostResult >= 0)
        return hostResult;
    SetProcessDPIAware();
    INITCOMMONCONTROLSEX cc{sizeof(cc), ICC_LISTVIEW_CLASSES};
    InitCommonControlsEx(&cc);
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    wc.lpszClassName = L"MidiPlayerWindow";
    RegisterClassExW(&wc);
    auto h =
        CreateWindowExW(0, wc.lpszClassName, AppTitle, WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                        CW_USEDEFAULT, CW_USEDEFAULT, 1100, 764, nullptr, nullptr, instance, nullptr);
    if (!h)
        return 1;
    if (restoreMini)
        ToggleMini();
    ShowWindow(h, show);
    UpdateWindow(h);
    ACCEL keys[] = {{FVIRTKEY | FCONTROL, 'O', OPEN}, {FVIRTKEY, VK_SPACE, PAUSE}};
    auto accel = CreateAcceleratorTableW(keys, 2);
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!TranslateAcceleratorW(h, accel, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    DestroyAcceleratorTable(accel);
    return (int)msg.wParam;
}

