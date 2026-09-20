#include <atlbase.h>
#include <atlapp.h>
#include <atltypes.h>
#include <FLAC/format.h>
#include <taglib/tstring.h>

CAppModule _Module;

class MainWindow : public CWindowImpl<MainWindow>
{
public:
    BEGIN_MSG_MAP(MainWindow)
        MESSAGE_HANDLER(WM_DESTROY, OnDestroy)
    END_MSG_MAP()

    LRESULT OnDestroy(UINT, WPARAM, LPARAM, BOOL&)
    {
        PostQuitMessage(0);
        return 0;
    }
};

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int)
{
    _Module.Init(nullptr, hInstance);

    OutputDebugStringA("Kessoku: FLAC version = ");
    OutputDebugStringA(FLAC__VERSION_STRING);
    OutputDebugStringA(FLAC__format_sample_rate_is_valid(44100) != 0 ? " (44100 valid)\n" : " (44100 invalid)\n");

    const TagLib::String tagString("taglib");
    OutputDebugStringW(tagString.toWString().c_str());
    OutputDebugStringA(": TagLib String constructed OK\n");

    MainWindow win;
    RECT rc = { 0, 0, 800, 600 };
    win.Create(nullptr, rc, L"Kessoku",
               WS_OVERLAPPEDWINDOW | WS_VISIBLE);

    CMessageLoop loop;
    _Module.AddMessageLoop(&loop);
    loop.Run();
    _Module.RemoveMessageLoop();

    _Module.Term();
    return 0;
}
