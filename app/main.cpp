#define UNICODE
#define _UNICODE
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <strsafe.h>
#include <tlhelp32.h>
#include <cstdlib>

// Generated RPC header
#include "TrayService_h.h"

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "rpcrt4.lib")
#pragma comment(lib, "advapi32.lib")

// Required by the RPC runtime / MIDL-generated stubs
extern "C" {
    void* __RPC_USER MIDL_user_allocate(size_t len) { return malloc(len); }
    void  __RPC_USER MIDL_user_free(void* ptr)      { free(ptr); }
}

#define WM_TRAYICON     (WM_USER + 1)
#define IDM_OPEN        1001
#define IDM_EXIT        1002
#define IDM_FILE_EXIT   2001
#define TRAY_ICON_ID    1

#define SERVICE_NAME    L"TrayService"
#define SERVICE_EXE     L"TrayService.exe"

static NOTIFYICONDATA g_nid  = {};
static UINT g_taskbarCreated = 0;

// ---------------------------------------------------------------------------
// RPC helpers
// ---------------------------------------------------------------------------
static void CallRpcStopService()
{
    RPC_WSTR szBinding = NULL;
    handle_t hBinding  = NULL;

    RPC_STATUS st = RpcStringBindingComposeW(
        NULL,
        reinterpret_cast<RPC_WSTR>(const_cast<wchar_t*>(L"ncalrpc")),
        NULL,
        reinterpret_cast<RPC_WSTR>(const_cast<wchar_t*>(L"TrayServiceEndpoint")),
        NULL,
        &szBinding);
    if (st != RPC_S_OK) return;

    st = RpcBindingFromStringBindingW(szBinding, &hBinding);
    RpcStringFreeW(&szBinding);
    if (st != RPC_S_OK) return;

    RpcTryExcept
    {
        StopService(hBinding);
    }
    RpcExcept(1)
    {
        // Ignore — service may already be stopping
    }
    RpcEndExcept

    RpcBindingFree(&hBinding);
}

// ---------------------------------------------------------------------------
// Parent process check — returns true if parent image is TrayService.exe
// ---------------------------------------------------------------------------
static bool IsParentTrayService()
{
    DWORD myPid    = GetCurrentProcessId();
    DWORD parentId = 0;

    HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnap == INVALID_HANDLE_VALUE) return false;

    PROCESSENTRY32W pe = { sizeof(pe) };
    if (Process32FirstW(hSnap, &pe))
    {
        do {
            if (pe.th32ProcessID == myPid)
            {
                parentId = pe.th32ParentProcessID;
                break;
            }
        } while (Process32NextW(hSnap, &pe));
    }
    CloseHandle(hSnap);

    if (parentId == 0) return false;

    HANDLE hParent = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, parentId);
    if (!hParent) return false;

    wchar_t imagePath[MAX_PATH] = {};
    DWORD   len = MAX_PATH;
    QueryFullProcessImageNameW(hParent, 0, imagePath, &len);
    CloseHandle(hParent);

    const wchar_t* name = wcsrchr(imagePath, L'\\');
    name = name ? name + 1 : imagePath;
    return _wcsicmp(name, SERVICE_EXE) == 0;
}

// ---------------------------------------------------------------------------
// TrayService management — returns true if service exists
// ---------------------------------------------------------------------------
static bool EnsureTrayServiceRunning()
{
    SC_HANDLE hSCM = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT);
    if (!hSCM) return false;

    SC_HANDLE hSvc = OpenServiceW(hSCM, SERVICE_NAME,
                                  SERVICE_QUERY_STATUS | SERVICE_START);
    if (!hSvc)
    {
        CloseServiceHandle(hSCM);
        return false; // service not installed
    }

    SERVICE_STATUS ss = {};
    QueryServiceStatus(hSvc, &ss);

    bool alreadyRunning = (ss.dwCurrentState == SERVICE_RUNNING);

    if (!alreadyRunning)
    {
        StartServiceW(hSvc, 0, NULL);

        // Wait up to 30 s for SERVICE_RUNNING
        for (int i = 0; i < 30; i++)
        {
            Sleep(1000);
            QueryServiceStatus(hSvc, &ss);
            if (ss.dwCurrentState == SERVICE_RUNNING) break;
        }
    }

    CloseServiceHandle(hSvc);
    CloseServiceHandle(hSCM);

    // If we just started the service, it will relaunch us — exit now
    return alreadyRunning;
}

// ---------------------------------------------------------------------------
// Tray / window helpers
// ---------------------------------------------------------------------------
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

    SetForegroundWindow(hwnd);
    TrackPopupMenu(hMenu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN | TPM_LEFTALIGN,
                   pt.x, pt.y, 0, hwnd, NULL);
    PostMessage(hwnd, WM_NULL, 0, 0);
    DestroyMenu(hMenu);
}

static void DoExit(HWND hwnd)
{
    RemoveTrayIcon();
    DestroyWindow(hwnd);
    CallRpcStopService();
}

// ---------------------------------------------------------------------------
// Window procedure
// ---------------------------------------------------------------------------
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (msg == g_taskbarCreated)
    {
        AddTrayIcon(hwnd);
        return 0;
    }

    switch (msg)
    {
    case WM_CREATE:
    {
        HMENU hBar  = CreateMenu();
        HMENU hFile = CreatePopupMenu();
        AppendMenu(hFile, MF_STRING, IDM_FILE_EXIT, L"Exit");
        AppendMenu(hBar,  MF_POPUP, (UINT_PTR)hFile, L"&File");
        SetMenu(hwnd, hBar);
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
            DoExit(hwnd);
            break;
        }
        return 0;

    case WM_CLOSE:
        ShowWindow(hwnd, SW_HIDE);
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProc(hwnd, msg, wParam, lParam);
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------
int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR lpCmdLine, int nCmdShow)
{
    bool hidden = (lpCmdLine && wcsstr(lpCmdLine, L"--hidden") != nullptr);

    // 1. Check / start TrayService
    bool serviceAlreadyRunning = EnsureTrayServiceRunning();
    if (!serviceAlreadyRunning)
    {
        // We either just started the service (it will relaunch us) or the
        // service is not installed.  In the first case we must exit so the
        // service-launched instance takes over.  In the second case we fall
        // through and run standalone (service binary not deployed).
        //
        // Distinguish by checking whether the service now exists:
        SC_HANDLE hSCM = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT);
        bool exists = false;
        if (hSCM)
        {
            SC_HANDLE hSvc = OpenServiceW(hSCM, SERVICE_NAME, SERVICE_QUERY_STATUS);
            if (hSvc) { exists = true; CloseServiceHandle(hSvc); }
            CloseServiceHandle(hSCM);
        }
        if (exists)
            return 0; // service was stopped, we started it, now exit
        // else: service not installed, fall through to run standalone
    }
    else
    {
        // Service is running — we must be launched by it
        if (!IsParentTrayService())
            return 0;
    }

    // 2. Single-instance guard (per session)
    HANDLE hMutex = CreateMutexW(NULL, TRUE, L"Local\\TrayApp_SingleInstance");
    if (!hMutex || GetLastError() == ERROR_ALREADY_EXISTS)
    {
        if (hMutex) CloseHandle(hMutex);
        return 0;
    }

    g_taskbarCreated = RegisterWindowMessage(L"TaskbarCreated");

    WNDCLASSEXW wc   = {};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInstance;
    wc.hIcon         = LoadIcon(NULL, IDI_APPLICATION);
    wc.hIconSm       = LoadIcon(NULL, IDI_APPLICATION);
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = L"TrayAppClass";
    RegisterClassExW(&wc);

    HWND hwnd = CreateWindowExW(
        0, L"TrayAppClass", L"Tray App",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 480, 320,
        NULL, NULL, hInstance, NULL);

    if (!hwnd)
    {
        CloseHandle(hMutex);
        return 1;
    }

    ShowWindow(hwnd, hidden ? SW_HIDE : nCmdShow);
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
