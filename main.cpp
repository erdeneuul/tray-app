#define UNICODE
#define _UNICODE
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <strsafe.h>

#define WM_TRAYICON     (WM_USER + 1)
#define IDM_OPEN        1001
#define IDM_EXIT        1002
#define IDM_FILE_EXIT   2001
#define TRAY_ICON_ID    1

static NOTIFYICONDATA g_nid  = {};
static UINT g_taskbarCreated = 0;

static void AddTrayIcon(HWND hwnd)
{
    g_nid.cbSize           = sizeof(g_nid);
    g_nid.hWnd             = hwnd;
    g_nid.uID              = TRAY_ICON_ID;
    g_nid.uFlags           = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAYICON;
    g_nid.hIcon            = LoadIcon(NULL, IDI_APPLICATION);
    StringCchCopy(g_nid.szTip, ARRAYSIZE(g_nid.szTip), L"TrayApp");
    Shell_NotifyIcon(NIM_ADD, &g_nid);
}

static void RemoveTrayIcon()
{
    Shell_NotifyIcon(NIM_DELETE, &g_nid);
}

static void ShowMainWindow(HWND hwnd)
{
    ShowWindow(hwnd, SW_RESTORE);
    SetForegroundWindow(hwnd);
}

static void ShowContextMenu(HWND hwnd)
{
    HMENU hMenu = CreatePopupMenu();
    AppendMenu(hMenu, MF_STRING, IDM_OPEN, L"Open");
    AppendMenu(hMenu, MF_STRING, IDM_EXIT, L"Exit");

    POINT pt;
    GetCursorPos(&pt);

    // Required so the menu dismisses when clicking elsewhere.
    SetForegroundWindow(hwnd);
    TrackPopupMenu(hMenu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN | TPM_LEFTALIGN,
                   pt.x, pt.y, 0, hwnd, NULL);
    PostMessage(hwnd, WM_NULL, 0, 0);
    DestroyMenu(hMenu);
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    // Re-add icon when Explorer restarts and recreates the taskbar.
    if (msg == g_taskbarCreated)
    {
        AddTrayIcon(hwnd);
        return 0;
    }

    switch (msg)
    {
    case WM_CREATE:
    {
        HMENU hMenuBar  = CreateMenu();
        HMENU hFileMenu = CreatePopupMenu();
        AppendMenu(hFileMenu, MF_STRING, IDM_FILE_EXIT, L"Exit");
        AppendMenu(hMenuBar,  MF_POPUP, (UINT_PTR)hFileMenu, L"&File");
        SetMenu(hwnd, hMenuBar);
        return 0;
    }

    case WM_TRAYICON:
        switch ((UINT)lParam)
        {
        case WM_LBUTTONUP:
            ShowMainWindow(hwnd);
            break;
        case WM_RBUTTONUP:
            ShowContextMenu(hwnd);
            break;
        }
        return 0;

    case WM_COMMAND:
        switch (LOWORD(wParam))
        {
        case IDM_OPEN:
            ShowMainWindow(hwnd);
            break;
        case IDM_EXIT:
        case IDM_FILE_EXIT:
            RemoveTrayIcon();
            DestroyWindow(hwnd);
            break;
        }
        return 0;

    case WM_CLOSE:
        // Hide to tray instead of closing.
        ShowWindow(hwnd, SW_HIDE);
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProc(hwnd, msg, wParam, lParam);
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR, int nCmdShow)
{
    // Single-instance guard (per user session — Local\ namespace is session-scoped).
    HANDLE hMutex = CreateMutex(NULL, TRUE, L"Local\\TrayApp_SingleInstance");
    if (!hMutex || GetLastError() == ERROR_ALREADY_EXISTS)
    {
        if (hMutex) CloseHandle(hMutex);
        return 0;
    }

    g_taskbarCreated = RegisterWindowMessage(L"TaskbarCreated");

    WNDCLASSEX wc    = {};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInstance;
    wc.hIcon         = LoadIcon(NULL, IDI_APPLICATION);
    wc.hIconSm       = LoadIcon(NULL, IDI_APPLICATION);
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = L"TrayAppClass";
    RegisterClassEx(&wc);

    HWND hwnd = CreateWindowEx(
        0, L"TrayAppClass", L"Tray App",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 480, 320,
        NULL, NULL, hInstance, NULL);

    if (!hwnd)
    {
        CloseHandle(hMutex);
        return 1;
    }

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);
    AddTrayIcon(hwnd);

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0))
    {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    CloseHandle(hMutex);
    return (int)msg.wParam;
}
