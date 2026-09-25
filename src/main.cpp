#include <atlbase.h>
#include <atlapp.h>
#include <atltypes.h>
#include <atlwin.h>
#include <atlctrls.h>
#include <atlctrlw.h>
#include <FLAC/format.h>
#include <taglib/tstring.h>
#include <kessoku/core/library_root.h>
#include <kessoku/library/scanner.h>
#include <kessoku/library/metadata.h>

#include <shobjidl_core.h>

#include <algorithm>
#include <thread>
#include <vector>
#include <string>

CAppModule _Module;

static constexpr UINT WM_SCAN_COMPLETE = WM_APP + 1;

struct ScanResultData {
    std::vector<std::pair<std::filesystem::path, kessoku::library::TrackMetadata>> tracks;
};

class MainWindow : public CWindowImpl<MainWindow>
{
public:
    BEGIN_MSG_MAP(MainWindow)
        MESSAGE_HANDLER(WM_CREATE, OnCreate)
        MESSAGE_HANDLER(WM_DESTROY, OnDestroy)
        MESSAGE_HANDLER(WM_SCAN_COMPLETE, OnScanComplete)
    END_MSG_MAP()

    LRESULT OnCreate(UINT, WPARAM, LPARAM, BOOL&)
    {
        RECT listRect = { 0, 0, 0, 0 };
        m_listView.Create(*this, listRect, nullptr,
            WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
            WS_EX_CLIENTEDGE);

        m_listView.InsertColumn(0, L"Title", LVCFMT_LEFT, 200);
        m_listView.InsertColumn(1, L"Artist", LVCFMT_LEFT, 150);
        m_listView.InsertColumn(2, L"Album", LVCFMT_LEFT, 150);
        m_listView.InsertColumn(3, L"Track#", LVCFMT_LEFT, 60);

        ShowEmptyState();
        return 0;
    }

    LRESULT OnDestroy(UINT, WPARAM, LPARAM, BOOL&)
    {
        PostQuitMessage(0);
        return 0;
    }

    LRESULT OnScanComplete(UINT, WPARAM wParam, LPARAM, BOOL&)
    {
        ScanResultData* data = reinterpret_cast<ScanResultData*>(wParam);
        if (!data) {
            return 0;
        }

        m_listView.DeleteAllItems();

        if (data->tracks.empty()) {
            ShowEmptyState();
        } else {
            for (size_t i = 0; i < data->tracks.size(); ++i) {
                const auto& [filePath, track] = data->tracks[i];
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

        delete data;
        return 0;
    }

    void ShowEmptyState()
    {
        m_listView.DeleteAllItems();
        int row = m_listView.InsertItem(0, L"No audio files found in this folder.");
        m_listView.SetItemText(row, 1, L"");
        m_listView.SetItemText(row, 2, L"");
        m_listView.SetItemText(row, 3, L"");
    }

private:
    CListViewCtrl m_listView;
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
