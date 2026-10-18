#include "a2m_convert.h"

#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>

#include <atomic>
#include <cwchar>
#include <filesystem>
#include <string>
#include <thread>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' " \
                        "version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

//owo
enum : int
{
    IDC_LBL_IN = 101,
    IDC_IN,
    IDC_BROWSE,
    IDC_LBL_OUT,
    IDC_OUT,
    IDC_PROG,
    IDC_STATUS,
    IDC_EXPORT,
    IDC_SHOW,
};
constexpr UINT WM_APP_PROGRESS = WM_APP + 1; //wparam -> percent
constexpr UINT WM_APP_DONE = WM_APP + 2; //lparam -> DoneMsg

struct DoneMsg
{
    bool ok = false;
    std::wstring err;
    std::wstring outPath;
    A2MResult res;
};

static HWND gWnd, gLblIn, gIn, gBrowse, gLblOut, gOut, gProg, gStatus, gExport, gShow;
static HFONT gFont;
static std::thread gWorker;
static std::atomic<bool> gCancel{ false };
static bool gRunning = false;
static bool gCloseAfter = false;
static std::wstring gLastOut;

static std::wstring Trim(std::wstring s)
{
    const wchar_t* ws = L" \t\r\n\"";
    const size_t a = s.find_first_not_of(ws);
    if (a == std::wstring::npos) return {};
    const size_t b = s.find_last_not_of(ws);
    return s.substr(a, b - a + 1);
}

static std::wstring GetText(HWND h)
{
    std::wstring s((size_t)GetWindowTextLengthW(h), L'\0');
    GetWindowTextW(h, s.data(), (int)s.size() + 1);
    return s;
}

static std::wstring InputPath() { return Trim(GetText(gIn)); }

static void UpdateOutputLabel()
{
    const std::wstring in = InputPath();
    SetWindowTextW(gOut, in.empty() ? L"-" : A2MOutputPathFor(in).c_str());
}

static int Scale(int v) { return MulDiv(v, (int)GetDpiForWindow(gWnd), 96); }

static void ApplyFont()
{
    if (gFont) DeleteObject(gFont);
    NONCLIENTMETRICSW m{ sizeof(m) };
    SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(m), &m, 0, GetDpiForWindow(gWnd));
    gFont = CreateFontIndirectW(&m.lfMessageFont);
    for (HWND h : { gLblIn, gIn, gBrowse, gLblOut, gOut, gStatus, gExport, gShow })
        SendMessageW(h, WM_SETFONT, (WPARAM)gFont, TRUE);
}

static void Layout()
{
    RECT rc;
    GetClientRect(gWnd, &rc);
    const int W = rc.right, m = Scale(14), bw = Scale(96), bh = Scale(28), gap = Scale(8);
    MoveWindow(gLblIn, m, Scale(12), W - 2 * m, Scale(20), TRUE);
    MoveWindow(gIn, m, Scale(34), W - 2 * m - bw - gap, Scale(26), TRUE);
    MoveWindow(gBrowse, W - m - bw, Scale(33), bw, bh, TRUE);
    MoveWindow(gLblOut, m, Scale(72), W - 2 * m, Scale(20), TRUE);
    MoveWindow(gOut, m, Scale(94), W - 2 * m, Scale(20), TRUE);
    MoveWindow(gProg, m, Scale(126), W - 2 * m, Scale(16), TRUE);
    MoveWindow(gStatus, m, Scale(150), W - 2 * m, Scale(20), TRUE);
    const int y = rc.bottom - m - bh;
    MoveWindow(gExport, W - m - Scale(120), y, Scale(120), bh, TRUE);
    MoveWindow(gShow, W - m - Scale(120) - gap - Scale(130), y, Scale(130), bh, TRUE);
}

static void SetRunning(bool on)
{
    gRunning = on;
    SetWindowTextW(gExport, on ? L"Cancel" : L"Export");
    EnableWindow(gIn, !on);
    EnableWindow(gBrowse, !on);
    if (on) EnableWindow(gShow, FALSE);
}

static void BrowseInput()
{
    wchar_t file[32768] = {};
    OPENFILENAMEW ofn{ sizeof(ofn) };
    ofn.hwndOwner = gWnd;
    ofn.lpstrFilter = L"Audio files\0*.mp3;*.wav;*.ogg;*.opus;*.flac;*.m4a;*.aac;*.wma;*.aif;*.aiff;*.webm;*.mka\0"
                      L"All files\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = (DWORD)std::size(file);
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER;
    if (GetOpenFileNameW(&ofn)) SetWindowTextW(gIn, file);
}

static void StartExport()
{
    const std::wstring in = InputPath();
    const DWORD attr = in.empty() ? INVALID_FILE_ATTRIBUTES : GetFileAttributesW(in.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES || (attr & FILE_ATTRIBUTE_DIRECTORY))
    {
        MessageBoxW(gWnd, L"Choose an input audio file first.", L"Audio2Midi", MB_ICONWARNING);
        return;
    }
    const std::wstring out = A2MOutputPathFor(in);
    if (GetFileAttributesW(out.c_str()) != INVALID_FILE_ATTRIBUTES)
    {
        const std::wstring q = out + L"\n\nalready exists. Overwrite it?";
        if (MessageBoxW(gWnd, q.c_str(), L"Audio2Midi", MB_ICONQUESTION | MB_YESNO) != IDYES) return;
    }

    gCancel = false;
    SetRunning(true);
    SendMessageW(gProg, PBM_SETPOS, 0, 0);
    SetWindowTextW(gStatus, L"Decoding...");
    if (gWorker.joinable()) gWorker.join();
    gWorker = std::thread([in, out] {
        auto* d = new DoneMsg;
        d->outPath = out;
        d->ok = A2MConvert(in, out, gCancel, [](int p) { PostMessageW(gWnd, WM_APP_PROGRESS, (WPARAM)p, 0); }, d->res, d->err);
        PostMessageW(gWnd, WM_APP_DONE, 0, (LPARAM)d);
    });
}

static void OnProgress(int p)
{
    SendMessageW(gProg, PBM_SETPOS, (WPARAM)p, 0);
    const wchar_t* phase = L"Saving...";
    if (p < 10) phase = L"Decoding...";
    else if (p < 60) phase = L"Writing notes...";
    else if (p < 95) phase = L"Building spectrogram...";
    wchar_t s[128];
    swprintf_s(s, L"%s %d%%", phase, p);
    SetWindowTextW(gStatus, s);
}

static void OnDone(DoneMsg* d)
{
    if (gWorker.joinable()) gWorker.join();
    SetRunning(false);
    if (d->ok)
    {
        gLastOut = d->outPath;
        EnableWindow(gShow, TRUE);
        SendMessageW(gProg, PBM_SETPOS, 100, 0);
        const std::wstring folder = std::filesystem::path(d->outPath).parent_path().filename().wstring();
        const int sec = (int)(d->res.seconds + 0.5);
        wchar_t s[512];
        swprintf_s(s, L"Saved to folder %s - mid + sf2 (%d:%02d, %lld notes)", folder.c_str(),
                   sec / 60, sec % 60, d->res.soundNotes + d->res.specNotes);
        SetWindowTextW(gStatus, s);
    }
    else
    {
        SendMessageW(gProg, PBM_SETPOS, 0, 0);
        SetWindowTextW(gStatus, gCancel ? L"Cancelled." : L"Failed.");
        if (!gCancel && !gCloseAfter) MessageBoxW(gWnd, d->err.c_str(), L"Audio2Midi", MB_ICONERROR);
    }
    delete d;
    if (gCloseAfter) DestroyWindow(gWnd);
}

static void ShowInFolder()
{
    if (gLastOut.empty()) return;
    if (PIDLIST_ABSOLUTE pidl = ILCreateFromPathW(gLastOut.c_str()))
    {
        SHOpenFolderAndSelectItems(pidl, 0, nullptr, 0);
        ILFree(pidl);
    }
}

static HWND MakeChild(const wchar_t* cls, const wchar_t* text, DWORD style, int id, DWORD ex = 0)
{
    return CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 0, 0,
                           gWnd, (HMENU)(INT_PTR)id, GetModuleHandleW(nullptr), nullptr);
}

static LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_CREATE:
        gWnd = hWnd;
        gLblIn = MakeChild(L"STATIC", L"Input audio:", 0, IDC_LBL_IN);
        gIn = MakeChild(L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL, IDC_IN, WS_EX_CLIENTEDGE);
        gBrowse = MakeChild(L"BUTTON", L"Browse...", WS_TABSTOP | BS_PUSHBUTTON, IDC_BROWSE);
        gLblOut = MakeChild(L"STATIC", L"Output", 0, IDC_LBL_OUT);
        gOut = MakeChild(L"STATIC", L"-", SS_PATHELLIPSIS | SS_NOPREFIX, IDC_OUT);
        gProg = MakeChild(PROGRESS_CLASSW, L"", 0, IDC_PROG);
        gStatus = MakeChild(L"STATIC", L"", SS_ENDELLIPSIS | SS_NOPREFIX, IDC_STATUS);
        gShow = MakeChild(L"BUTTON", L"Show in folder", WS_TABSTOP | BS_PUSHBUTTON | WS_DISABLED, IDC_SHOW);
        gExport = MakeChild(L"BUTTON", L"Export", WS_TABSTOP | BS_DEFPUSHBUTTON, IDC_EXPORT);
        SendMessageW(gProg, PBM_SETRANGE32, 0, 100);
        ApplyFont();
        DragAcceptFiles(hWnd, TRUE);
        return 0;

    case WM_SIZE:
        Layout();
        return 0;

    case WM_DPICHANGED:
    {
        const RECT* r = (const RECT*)lp;
        SetWindowPos(hWnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        ApplyFont();
        Layout();
        return 0;
    }

    case WM_GETMINMAXINFO:
    {
        auto* mm = (MINMAXINFO*)lp;
        mm->ptMinTrackSize = { Scale(460), Scale(260) };
        return 0;
    }

    case WM_DROPFILES:
    {
        HDROP drop = (HDROP)wp;
        if (!gRunning)
        {
            wchar_t file[32768];
            if (DragQueryFileW(drop, 0, file, (UINT)std::size(file))) SetWindowTextW(gIn, file);
        }
        DragFinish(drop);
        return 0;
    }

    case WM_COMMAND:
        switch (LOWORD(wp))
        {
        case IDC_IN:
            if (HIWORD(wp) == EN_CHANGE) UpdateOutputLabel();
            return 0;
        case IDC_BROWSE: BrowseInput(); return 0;
        case IDC_SHOW: ShowInFolder(); return 0;
        case IDC_EXPORT:
        case IDOK:
            if (gRunning)
            {
                gCancel = true;
                SetWindowTextW(gStatus, L"Cancelling...");
            }
            else StartExport();
            return 0;
        }
        break;

    case WM_APP_PROGRESS:
        if (gRunning) OnProgress((int)wp);
        return 0;

    case WM_APP_DONE:
        OnDone((DoneMsg*)lp);
        return 0;

    case WM_CLOSE:
        //let the worker stop before the window goes away
        if (gRunning)
        {
            gCancel = true;
            gCloseAfter = true;
            SetWindowTextW(gStatus, L"Cancelling...");
            return 0;
        }
        DestroyWindow(hWnd);
        return 0;

    case WM_DESTROY:
        if (gFont) DeleteObject(gFont);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hWnd, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR, int nShow)
{
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    //a file opened with the exe only fills the input box
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);

    INITCOMMONCONTROLSEX icc{ sizeof(icc), ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);

    WNDCLASSEXW wc{ sizeof(wc) };
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"Audio2MidiWnd";
    RegisterClassExW(&wc);

    HWND hWnd = CreateWindowExW(0, wc.lpszClassName, L"Audio2Midi", WS_OVERLAPPEDWINDOW,
                                CW_USEDEFAULT, CW_USEDEFAULT, 600, 300, nullptr, nullptr, hInst, nullptr);
    if (!hWnd) return 1;

    //size the client area for the monitor dpi
    const UINT dpi = GetDpiForWindow(hWnd);
    RECT r{ 0, 0, MulDiv(600, (int)dpi, 96), MulDiv(232, (int)dpi, 96) };
    AdjustWindowRectExForDpi(&r, WS_OVERLAPPEDWINDOW, FALSE, 0, dpi);
    SetWindowPos(hWnd, nullptr, 0, 0, r.right - r.left, r.bottom - r.top, SWP_NOMOVE | SWP_NOZORDER);

    if (argv && argc >= 2)
    {
        const std::wstring arg = Trim(argv[1]);
        const DWORD attr = GetFileAttributesW(arg.c_str());
        if (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY)) SetWindowTextW(gIn, arg.c_str());
    }
    if (argv) LocalFree(argv);
    UpdateOutputLabel();

    ShowWindow(hWnd, nShow);
    UpdateWindow(hWnd);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0))
    {
        if (IsDialogMessageW(hWnd, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (gWorker.joinable()) gWorker.join();
    return (int)msg.wParam;
}
