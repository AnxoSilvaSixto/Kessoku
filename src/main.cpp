#include <atlbase.h>
#include <atlapp.h>
#include <atltypes.h>
#include <atlwin.h>
#include <atlctrls.h>
#include <atlctrlw.h>
#include <FLAC/format.h>
#include <taglib/tstring.h>
#include <kessoku/audio/player.h>
#include <kessoku/core/library_root.h>
#include <kessoku/library/scanner.h>
#include <kessoku/library/metadata.h>

#include <shobjidl_core.h>

#include <algorithm>
#include <cstdint>
#include <optional>
#include <thread>
#include <vector>
#include <string>

CAppModule _Module;

static constexpr UINT WM_SCAN_COMPLETE = WM_APP + 1;

// SetTimer ID/interval for position polling. The callback arrives as WM_TIMER
// through this window's own message queue, so every Player call it drives
// runs on the same (UI) thread that called Player::Create — the thread
// Player::AssertCallingThread() requires.
static constexpr UINT_PTR kPositionTimerId = 1;
static constexpr UINT kPositionTimerMs = 250;

// Seek bar granularity: the bar spans 0..kSeekScale and maps linearly onto
// 0..totalFrames. Keeps the control in int range no matter how long the
// track is (totalFrames is uint32_t and can exceed INT_MAX in theory).
static constexpr int kSeekScale = 1000;
static constexpr int kTransportStripHeight = 84;

enum ControlIds {
    IDC_PLAY_BUTTON = 101,
    IDC_SEEK_BAR = 102,
    IDC_STATUS_LABEL = 103,
    IDC_POSITION_LABEL = 104,
};

struct ScanResultData {
    std::vector<std::pair<std::filesystem::path, kessoku::library::TrackMetadata>> tracks;
};

namespace {

std::wstring FormatTrackTime(uint32_t frames, uint32_t sampleRate)
{
    uint64_t totalSeconds = (sampleRate != 0) ? (frames / sampleRate) : 0;
    uint64_t minutes = totalSeconds / 60;
    uint64_t seconds = totalSeconds % 60;
    std::wstring text = std::to_wstring(minutes) + L":";
    if (seconds < 10) {
        text += L"0";
    }
    text += std::to_wstring(seconds);
    return text;
}

std::wstring WidenErrorMessage(const std::string& narrow)
{
    if (narrow.empty()) {
        return L"Unknown error";
    }
    int needed = MultiByteToWideChar(CP_UTF8, 0, narrow.c_str(),
                                     static_cast<int>(narrow.size()),
                                     nullptr, 0);
    if (needed <= 0) {
        return L"Unknown error";
    }
    std::wstring wide(static_cast<size_t>(needed), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, narrow.c_str(),
                        static_cast<int>(narrow.size()),
                        wide.data(), needed);
    return wide;
}

} // namespace

class MainWindow : public CWindowImpl<MainWindow>
{
public:
    BEGIN_MSG_MAP(MainWindow)
        MESSAGE_HANDLER(WM_CREATE, OnCreate)
        MESSAGE_HANDLER(WM_DESTROY, OnDestroy)
        MESSAGE_HANDLER(WM_SIZE, OnSize)
        MESSAGE_HANDLER(WM_TIMER, OnTimer)
        MESSAGE_HANDLER(WM_COMMAND, OnCommand)
        MESSAGE_HANDLER(WM_NOTIFY, OnNotify)
        MESSAGE_HANDLER(WM_HSCROLL, OnHScroll)
        MESSAGE_HANDLER(WM_SCAN_COMPLETE, OnScanComplete)
    END_MSG_MAP()

    LRESULT OnCreate(UINT, WPARAM, LPARAM, BOOL&)
    {
        RECT listRect;
        GetClientRect(&listRect);
        m_listView.Create(*this, listRect, nullptr,
            WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
            WS_EX_CLIENTEDGE);

        m_listView.InsertColumn(0, L"Title", LVCFMT_LEFT, 200);
        m_listView.InsertColumn(1, L"Artist", LVCFMT_LEFT, 150);
        m_listView.InsertColumn(2, L"Album", LVCFMT_LEFT, 150);
        m_listView.InsertColumn(3, L"Track#", LVCFMT_LEFT, 60);

        RECT emptyRect = { 0, 0, 0, 0 };
        m_playButton.Create(*this, emptyRect, L"Play",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, IDC_PLAY_BUTTON);
        m_statusLabel.Create(*this, emptyRect, L"Ready",
            WS_CHILD | WS_VISIBLE | SS_LEFT, 0, IDC_STATUS_LABEL);
        m_positionLabel.Create(*this, emptyRect, L"0:00",
            WS_CHILD | WS_VISIBLE | SS_RIGHT, 0, IDC_POSITION_LABEL);
        m_seekBar.Create(*this, emptyRect, nullptr,
            WS_CHILD | WS_VISIBLE | TBS_HORZ | TBS_AUTOTICKS,
            0, IDC_SEEK_BAR);
        m_seekBar.SetRange(0, kSeekScale);
        m_seekBar.SetPos(0);
        // No Player yet, and Seek() rejects Stopped anyway: keep the bar
        // disabled until a track is actually playing or paused.
        m_seekBar.EnableWindow(FALSE);

        LayoutChildren(listRect.right - listRect.left,
                       listRect.bottom - listRect.top);

        ShowEmptyState();
        return 0;
    }

    LRESULT OnSize(UINT, WPARAM, LPARAM lParam, BOOL&)
    {
        LayoutChildren(static_cast<int>(LOWORD(lParam)),
                       static_cast<int>(HIWORD(lParam)));
        return 0;
    }

    LRESULT OnDestroy(UINT, WPARAM, LPARAM, BOOL&)
    {
        // Same thread that created any Player (the UI thread), so Stop() is
        // legal here. Release the exclusive stream before exiting.
        KillTimer(kPositionTimerId);
        if (m_player.has_value()) {
            m_player->Stop();
            m_player.reset();
        }
        PostQuitMessage(0);
        return 0;
    }

    LRESULT OnTimer(UINT, WPARAM wParam, LPARAM, BOOL&)
    {
        if (wParam != kPositionTimerId) {
            return 0;
        }
        PollPlayer();
        return 0;
    }

    LRESULT OnCommand(UINT, WPARAM wParam, LPARAM, BOOL&)
    {
        int controlId = LOWORD(wParam);
        int notifyCode = HIWORD(wParam);
        if (controlId == IDC_PLAY_BUTTON && notifyCode == BN_CLICKED) {
            TogglePlayPause();
            return 0;
        }
        return 0;
    }

    LRESULT OnNotify(UINT, WPARAM, LPARAM lParam, BOOL&)
    {
        LPNMHDR header = reinterpret_cast<LPNMHDR>(lParam);
        if (header->hwndFrom != m_listView.m_hWnd) {
            return 0;
        }
        if (header->code == NM_DBLCLK) {
            PlaySelected();
            return 0;
        }
        if (header->code == LVN_KEYDOWN) {
            LPNMLVKEYDOWN keyDown = reinterpret_cast<LPNMLVKEYDOWN>(lParam);
            if (keyDown->wVKey == VK_RETURN) {
                PlaySelected();
                return 0;
            }
        }
        return 0;
    }

    LRESULT OnHScroll(UINT, WPARAM wParam, LPARAM lParam, BOOL&)
    {
        if (reinterpret_cast<HWND>(lParam) != m_seekBar.m_hWnd) {
            return 0;
        }
        int scrollCode = LOWORD(wParam);
        switch (scrollCode) {
        case TB_THUMBTRACK:
            // Hold programmatic updates until the drag finishes so the timer
            // doesn't fight the thumb. Only TB_THUMBTRACK carries the drag
            // position in HIWORD; every other code must use GetPos().
            m_scrubbing = true;
            SeekFromBar(static_cast<int>(HIWORD(wParam)));
            break;
        case TB_ENDTRACK:
            SeekFromBar(m_seekBar.GetPos());
            m_scrubbing = false;
            break;
        case TB_PAGEUP:
        case TB_PAGEDOWN:
        case TB_LINEUP:
        case TB_LINEDOWN:
        case TB_TOP:
        case TB_BOTTOM:
            SeekFromBar(m_seekBar.GetPos());
            break;
        default:
            break;
        }
        return 0;
    }

    LRESULT OnScanComplete(UINT, WPARAM wParam, LPARAM, BOOL&)
    {
        ScanResultData* data = reinterpret_cast<ScanResultData*>(wParam);
        if (!data) {
            return 0;
        }

        m_listView.DeleteAllItems();
        m_entries = std::move(data->tracks);

        if (m_entries.empty()) {
            ShowEmptyState();
        } else {
            for (size_t i = 0; i < m_entries.size(); ++i) {
                const auto& [filePath, track] = m_entries[i];
                std::wstring title = track.title;
                if (title.empty()) {
                    title = filePath.stem().c_str();
                }

                int row = static_cast<int>(m_listView.InsertItem(static_cast<int>(i), title.c_str()));
                if (!track.artist.empty()) {
                    m_listView.SetItemText(row, 1, track.artist.c_str());
                }
                if (!track.album.empty()) {
                    m_listView.SetItemText(row, 2, track.album.c_str());
                }
                if (track.trackNumber != 0) {
                    m_listView.SetItemText(row, 3, std::to_wstring(track.trackNumber).c_str());
                }
            }
        }

        if (!m_player.has_value()) {
            m_statusLabel.SetWindowTextW(L"Ready");
        }

        delete data;
        return 0;
    }

    void ShowEmptyState()
    {
        m_listView.DeleteAllItems();
        m_entries.clear();
        int row = m_listView.InsertItem(0, L"No audio files found in this folder.");
        m_listView.SetItemText(row, 1, L"");
        m_listView.SetItemText(row, 2, L"");
        m_listView.SetItemText(row, 3, L"");
    }

private:
    void LayoutChildren(int cx, int cy)
    {
        if (!m_listView.IsWindow()) {
            return;
        }
        int stripTop = (std::max)(0, cy - kTransportStripHeight);
        m_listView.MoveWindow(0, 0, cx, stripTop);

        if (!m_playButton.IsWindow()) {
            return;
        }
        m_playButton.MoveWindow(8, stripTop + 8, 80, 30);
        int rightWidth = 110;
        int statusWidth = (std::max)(0, cx - 96 - rightWidth - 8);
        m_statusLabel.MoveWindow(96, stripTop + 13, statusWidth, 20);
        m_positionLabel.MoveWindow((std::max)(0, cx - rightWidth - 8), stripTop + 13,
                                   rightWidth, 20);
        m_seekBar.MoveWindow(8, stripTop + 44, (std::max)(0, cx - 16), 32);
    }

    void PlaySelected()
    {
        int selected = m_listView.GetSelectedIndex();
        if (selected < 0) {
            m_statusLabel.SetWindowTextW(L"No track selected.");
            return;
        }
        StartTrack(static_cast<size_t>(selected));
    }

    void StartTrack(size_t index)
    {
        // The empty-state placeholder row has no backing entry; bounds-check
        // keeps it from ever reaching Player::Create.
        if (index >= m_entries.size()) {
            m_statusLabel.SetWindowTextW(L"No track selected.");
            return;
        }

        // Stop any active Player first: releases its exclusive stream and
        // joins its render thread before the new Create() acquires the
        // device. Everything here runs on the UI thread, the same thread
        // that created the old Player, so Stop() satisfies the
        // single-calling-thread contract.
        StopTrack();

        auto result = kessoku::audio::Player::Create(m_entries[index].first.native());
        if (result.IsErr()) {
            const auto& err = result.GetError();
            ::MessageBoxW(m_hWnd, WidenErrorMessage(err.message).c_str(),
                        L"Playback error", MB_OK | MB_ICONERROR);
            m_statusLabel.SetWindowTextW(L"Could not play selected track.");
            return;
        }

        m_player.emplace(std::move(result.Value()));

        auto playStatus = m_player->Play();
        if (playStatus.IsErr()) {
            const auto& err = playStatus.GetError();
            ::MessageBoxW(m_hWnd, WidenErrorMessage(err.message).c_str(),
                        L"Playback error", MB_OK | MB_ICONERROR);
            m_player->Stop();
            m_player.reset();
            m_statusLabel.SetWindowTextW(L"Could not play selected track.");
            return;
        }

        std::wstring status = L"Playing: ";
        status += m_entries[index].first.filename().c_str();
        m_statusLabel.SetWindowTextW(status.c_str());

        SetTimer(kPositionTimerId, kPositionTimerMs);
        UpdateTransportControls();
        UpdatePositionUI();
    }

    void TogglePlayPause()
    {
        if (!m_player.has_value()) {
            // Nothing to toggle: start the selected row, if any.
            PlaySelected();
            return;
        }

        auto state = m_player->GetState();
        if (state == kessoku::audio::PlaybackState::Playing) {
            auto status = m_player->Pause();
            if (status.IsErr()) {
                ::MessageBoxW(m_hWnd,
                            WidenErrorMessage(status.GetError().message).c_str(),
                            L"Playback error", MB_OK | MB_ICONERROR);
            } else {
                m_statusLabel.SetWindowTextW(L"Paused");
            }
        } else if (state == kessoku::audio::PlaybackState::Paused) {
            auto status = m_player->Resume();
            if (status.IsErr()) {
                ::MessageBoxW(m_hWnd,
                            WidenErrorMessage(status.GetError().message).c_str(),
                            L"Playback error", MB_OK | MB_ICONERROR);
            } else {
                m_statusLabel.SetWindowTextW(L"Playing");
            }
        } else if (state == kessoku::audio::PlaybackState::Stopped) {
            // Replay the same track from the start. Normally PollPlayer
            // observes a natural end first and resets m_player, but the
            // button can win the race inside one 250ms tick, so handle an
            // engaged-but-Stopped Player here instead of assuming Paused.
            auto status = m_player->Play();
            if (status.IsErr()) {
                ::MessageBoxW(m_hWnd,
                            WidenErrorMessage(status.GetError().message).c_str(),
                            L"Playback error", MB_OK | MB_ICONERROR);
            } else {
                m_statusLabel.SetWindowTextW(L"Playing");
                SetTimer(kPositionTimerId, kPositionTimerMs);
            }
        } else {
            OnDeviceLost();
            return;
        }
        UpdateTransportControls();
        UpdatePositionUI();
    }

    void PollPlayer()
    {
        if (!m_player.has_value()) {
            KillTimer(kPositionTimerId);
            return;
        }

        auto state = m_player->GetState();
        if (state == kessoku::audio::PlaybackState::DeviceLost) {
            OnDeviceLost();
            return;
        }
        if (state == kessoku::audio::PlaybackState::Stopped) {
            // Only reachable as a natural end of track: every UI-initiated
            // Stop() goes through StopTrack(), which resets m_player and
            // kills the timer synchronously. Release the exclusive stream
            // now rather than holding it while idle, and reset the position
            // display to the start (Stop() zeroes the frame counter).
            m_statusLabel.SetWindowTextW(L"Finished");
            m_player->Stop();
            m_player.reset();
            KillTimer(kPositionTimerId);
            ResetPositionUI();
            UpdateTransportControls();
            return;
        }

        UpdateTransportControls();
        if (!m_scrubbing) {
            UpdatePositionUI();
        }
    }

    void OnDeviceLost()
    {
        // The Player already stopped itself cleanly (render thread exited,
        // state latched to DeviceLost). Release its handles, stop polling so
        // nothing keeps running unnoticed, and say so in the status line.
        // No re-enumeration/reactivation: explicitly deferred, out of scope.
        KillTimer(kPositionTimerId);
        if (m_player.has_value()) {
            m_player->Stop();
            m_player.reset();
        }
        m_scrubbing = false;
        m_statusLabel.SetWindowTextW(L"Audio device lost - playback stopped.");
        ResetPositionUI();
        UpdateTransportControls();
    }

    void StopTrack()
    {
        if (m_player.has_value()) {
            m_player->Stop();
            m_player.reset();
        }
        KillTimer(kPositionTimerId);
        m_scrubbing = false;
        ResetPositionUI();
        UpdateTransportControls();
    }

    void SeekFromBar(int barPos)
    {
        if (!m_player.has_value()) {
            return;
        }
        // Ignore seeks while Stopped instead of fighting Seek()'s Err return.
        if (m_player->GetState() == kessoku::audio::PlaybackState::Stopped) {
            return;
        }
        uint32_t total = m_player->GetTotalFrames();
        if (total == 0) {
            return;
        }
        if (barPos < 0) {
            barPos = 0;
        }
        if (barPos > kSeekScale) {
            barPos = kSeekScale;
        }
        uint32_t frame = static_cast<uint32_t>(
            (static_cast<uint64_t>(barPos) * total) /
            static_cast<uint64_t>(kSeekScale));
        auto status = m_player->Seek(frame);
        if (status.IsErr()) {
            m_statusLabel.SetWindowTextW(L"Seek failed.");
            return;
        }
        UpdatePositionUI();
    }

    void UpdateTransportControls()
    {
        bool hasPlayer = m_player.has_value();
        auto state = hasPlayer ? m_player->GetState()
                               : kessoku::audio::PlaybackState::Stopped;
        m_playButton.SetWindowTextW(
            (state == kessoku::audio::PlaybackState::Playing) ? L"Pause" : L"Play");

        bool seekable = hasPlayer &&
            state != kessoku::audio::PlaybackState::Stopped &&
            state != kessoku::audio::PlaybackState::DeviceLost &&
            m_player->GetTotalFrames() != 0;
        m_seekBar.EnableWindow(seekable ? TRUE : FALSE);
    }

    void UpdatePositionUI()
    {
        if (!m_player.has_value()) {
            ResetPositionUI();
            return;
        }
        uint32_t position = m_player->GetPosition();
        uint32_t total = m_player->GetTotalFrames();
        uint32_t rate = m_player->GetSampleRate();
        std::wstring text = FormatTrackTime(position, rate) + L" / " +
            FormatTrackTime(total, rate);
        m_positionLabel.SetWindowTextW(text.c_str());
        int barPos = 0;
        if (total != 0) {
            barPos = static_cast<int>(
                (static_cast<uint64_t>(position) * kSeekScale) / total);
        }
        m_seekBar.SetPos(barPos);
    }

    void ResetPositionUI()
    {
        m_positionLabel.SetWindowTextW(L"0:00");
        if (m_seekBar.IsWindow()) {
            m_seekBar.SetPos(0);
        }
    }

    CListViewCtrl m_listView;
    CButton m_playButton;
    CTrackBarCtrl m_seekBar;
    CStatic m_statusLabel;
    CStatic m_positionLabel;

    // Backing store for the ListView rows, in row order (sorted by path, as
    // posted by the scan thread). A selected row resolves to m_entries[row].
    // Paths originate from Scan() inside the library root, so containment
    // holds by construction; the index is bounds-checked at every use.
    std::vector<std::pair<std::filesystem::path, kessoku::library::TrackMetadata>> m_entries;

    // Zero or one Player, per the move-only contract. Created, driven
    // (Play/Pause/Resume/Seek/Stop/GetState/GetPosition/...), and destroyed
    // exclusively on this window's UI thread.
    std::optional<kessoku::audio::Player> m_player;

    // True between TB_THUMBTRACK and TB_ENDTRACK: PollPlayer skips
    // programmatic bar updates so the timer doesn't fight the drag.
    bool m_scrubbing = false;
};

namespace {

std::wstring PickFolder(HWND owner)
{
    CComPtr<IFileOpenDialog> pDialog;
    HRESULT hr = pDialog.CoCreateInstance(CLSID_FileOpenDialog);
    if (FAILED(hr)) {
        return L"";
    }

    DWORD dwOptions;
    hr = pDialog->GetOptions(&dwOptions);
    if (SUCCEEDED(hr)) {
        pDialog->SetOptions(dwOptions | FOS_PICKFOLDERS);
    }

    pDialog->SetTitle(L"Select music library folder");

    hr = pDialog->Show(owner);
    if (FAILED(hr)) {
        return L""; // Cancel or error
    }

    CComPtr<IShellItem> pItem;
    hr = pDialog->GetResult(&pItem);
    if (FAILED(hr)) {
        return L"";
    }

    PWSTR pszPath = nullptr;
    hr = pItem->GetDisplayName(SIGDN_FILESYSPATH, &pszPath);
    std::wstring result;
    if (SUCCEEDED(hr) && pszPath) {
        result = pszPath;
        CoTaskMemFree(pszPath);
    }
    return result;
}

void RunScanAndPost(HWND hWnd, const std::wstring& folderPath)
{
    auto rootResult = kessoku::core::LibraryRoot::Create(folderPath);
    if (!rootResult.IsOk()) {
        const auto& err = rootResult.GetError();
        OutputDebugStringA("Kessoku: library root create failed: ");
        OutputDebugStringA(err.message.c_str());
        OutputDebugStringA("\n");
        PostMessage(hWnd, WM_SCAN_COMPLETE, static_cast<WPARAM>(0), 0);
        return;
    }

    const auto& root = rootResult.Value();
    auto scanResult = kessoku::library::Scan(root);

    std::vector<std::pair<std::filesystem::path, kessoku::library::TrackMetadata>> tracks;
    tracks.reserve(scanResult.files.size());

    for (const auto& skipped : scanResult.skipped) {
        OutputDebugStringA("Kessoku: scan skipped: ");
        OutputDebugStringA(skipped.reason.c_str());
        OutputDebugStringA(" - ");
        std::string utf8;
        utf8.reserve(skipped.path.wstring().size() * 3);
        for (wchar_t c : skipped.path.wstring()) {
            utf8 += static_cast<char>(c & 0xFF);
        }
        OutputDebugStringA(utf8.c_str());
        OutputDebugStringA("\n");
    }

    std::sort(scanResult.files.begin(), scanResult.files.end());

    for (const auto& filePath : scanResult.files) {
        auto metaResult = kessoku::library::ReadTrackMetadata(filePath);
        if (metaResult.IsOk()) {
            tracks.emplace_back(filePath, metaResult.Value());
        } else {
            const auto& err = metaResult.GetError();
            OutputDebugStringA("Kessoku: metadata read failed: ");
            OutputDebugStringA(err.message.c_str());
            OutputDebugStringA("\n");
        }
    }

    auto* data = new ScanResultData{std::move(tracks)};
    PostMessage(hWnd, WM_SCAN_COMPLETE, WPARAM(data), 0);
}

} // namespace

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int)
{
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    _Module.Init(nullptr, hInstance);
    AtlInitCommonControls(ICC_LISTVIEW_CLASSES | ICC_BAR_CLASSES);

    OutputDebugStringA("Kessoku: FLAC version = ");
    OutputDebugStringA(FLAC__VERSION_STRING);
    OutputDebugStringA(FLAC__format_sample_rate_is_valid(44100) != 0 ? " (44100 valid)\n" : " (44100 invalid)\n");

    const TagLib::String tagString("taglib");
    OutputDebugStringW(tagString.toWString().c_str());
    OutputDebugStringA(": TagLib String constructed OK\n");

    OutputDebugStringA("kessoku_core: LibraryRoot type available\n");
    (void)kessoku::core::ErrorCode::Ok;

    MainWindow win;
    RECT rc = { 0, 0, 800, 600 };
    win.Create(nullptr, rc, L"Kessoku",
               WS_OVERLAPPEDWINDOW | WS_VISIBLE);

    while (true) {
        std::wstring folder = PickFolder(win.m_hWnd);
        if (folder.empty()) {
            // Cancel or error — exit cleanly
            break;
        }

        auto rootResult = kessoku::core::LibraryRoot::Create(folder);
        if (!rootResult.IsOk()) {
            const auto& err = rootResult.GetError();
            MessageBoxA(win.m_hWnd, err.message.c_str(), "Library root error", MB_OK | MB_ICONERROR);
            continue; // Re-prompt
        }

        std::thread scanThread(RunScanAndPost, win.m_hWnd, folder);
        scanThread.detach();
        break;
    }

    CMessageLoop loop;
    _Module.AddMessageLoop(&loop);
    loop.Run();
    _Module.RemoveMessageLoop();

    _Module.Term();

    CoUninitialize();
    return 0;
}
