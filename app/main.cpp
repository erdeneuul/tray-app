#define UNICODE
#define _UNICODE
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <strsafe.h>
#include <tlhelp32.h>
#include <cstdlib>

#include "rpc_interface_h.h"

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "rpcrt4.lib")
#pragma comment(lib, "advapi32.lib")

extern "C" {
    void* __RPC_USER MIDL_user_allocate(size_t len) { return malloc(len); }
    void  __RPC_USER MIDL_user_free(void* ptr)      { free(ptr); }
}

#define WM_TRAYICON     (WM_USER + 1)
#define IDM_OPEN        1001
#define IDM_EXIT        1002
#define IDM_FILE_EXIT   2001
#define TRAY_ICON_ID    1

// Screen 1 — Login
#define IDC_LBL_LOGIN_TITLE  100
#define IDC_LBL_EMAIL        101
#define IDC_EDT_EMAIL        102
#define IDC_LBL_PASSWORD     103
#define IDC_EDT_PASSWORD     104
#define IDC_BTN_LOGIN        105
#define IDC_LBL_LOGIN_ERR    106

// Screen 2 — Activation
#define IDC_LBL_ACT_USER     201
#define IDC_LBL_ACT_PROMPT   202
#define IDC_EDT_ACT_CODE     203
#define IDC_BTN_ACTIVATE     204
#define IDC_LBL_ACT_ERR      205
#define IDC_LBL_ACT_DISABLED 206

// Screen 3 — Main
#define IDC_LBL_MAIN_USER    301
#define IDC_LBL_MAIN_EXPIRY  302
#define IDC_LBL_MAIN_STATUS  303
#define IDC_BTN_LOGOUT       304

#define TIMER_POLL_LICENSE   1

static const wchar_t* const kServiceName = L"TrayAppService";
static const wchar_t* const kServiceExe  = L"TrayService.exe";
static const wchar_t* const kRpcEndpoint = L"TrayAppService";

static NOTIFYICONDATA g_nid        = {};
static UINT           g_taskbarMsg = 0;
static handle_t       g_hBinding   = NULL;
static int            g_curScreen  = 0;   // 1=login, 2=activation, 3=main
static bool           g_wasLicensed = false;

// ---------------------------------------------------------------------------
// Check whether this process was launched by TrayService.exe
// ---------------------------------------------------------------------------
static bool IsParentTrayService()
{
    DWORD myPid    = GetCurrentProcessId();
    DWORD parentId = 0;

    HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnap == INVALID_HANDLE_VALUE) return false;

    PROCESSENTRY32W pe = { sizeof(pe) };
    if (Process32FirstW(hSnap, &pe))
        do {
            if (pe.th32ProcessID == myPid)
            {
                parentId = pe.th32ParentProcessID;
                break;
            }
        } while (Process32NextW(hSnap, &pe));
    CloseHandle(hSnap);

    if (!parentId) return false;

    HANDLE hParent = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, parentId);
    if (!hParent) return false;

    wchar_t image[MAX_PATH] = {};
    DWORD   len = MAX_PATH;
    BOOL    ok  = QueryFullProcessImageNameW(hParent, 0, image, &len);
    CloseHandle(hParent);

    if (!ok) return false;

    const wchar_t* name = wcsrchr(image, L'\\');
    name = name ? name + 1 : image;
    return _wcsicmp(name, kServiceExe) == 0;
}

static bool HandleServiceStartup()
{
    if (IsParentTrayService())
        return true;

    SC_HANDLE hSCM = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT);
    if (!hSCM)
        return true;

    SC_HANDLE hSvc = OpenServiceW(hSCM, kServiceName,
                                  SERVICE_QUERY_STATUS | SERVICE_START);
    if (!hSvc)
    {
        CloseServiceHandle(hSCM);
        return true;
    }

    SERVICE_STATUS ss = {};
    QueryServiceStatus(hSvc, &ss);

    if (ss.dwCurrentState == SERVICE_RUNNING)
    {
        CloseServiceHandle(hSvc);
        CloseServiceHandle(hSCM);
        return false;
    }

    if (ss.dwCurrentState == SERVICE_STOPPED)
    {
        StartServiceW(hSvc, 0, NULL);
        for (int i = 0; i < 60; i++)
        {
            Sleep(500);
            QueryServiceStatus(hSvc, &ss);
            if (ss.dwCurrentState == SERVICE_RUNNING) break;
        }
    }

    CloseServiceHandle(hSvc);
    CloseServiceHandle(hSCM);
    return false;
}

// ---------------------------------------------------------------------------
// RPC binding (shared, established once)
// ---------------------------------------------------------------------------
static bool BindRpc()
{
    if (g_hBinding) return true;
    RPC_WSTR szBinding = NULL;
    RPC_STATUS st = RpcStringBindingComposeW(
        NULL,
        reinterpret_cast<RPC_WSTR>(const_cast<wchar_t*>(L"ncalrpc")),
        NULL,
        reinterpret_cast<RPC_WSTR>(const_cast<wchar_t*>(kRpcEndpoint)),
        NULL,
        &szBinding);
    if (st != RPC_S_OK) return false;
    st = RpcBindingFromStringBindingW(szBinding, &g_hBinding);
    RpcStringFreeW(&szBinding);
    return (st == RPC_S_OK);
}

static bool CallRpcStopService()
{
    if (!BindRpc()) return false;
    bool ok = true;
    RpcTryExcept { StopService(g_hBinding); }
    RpcExcept(1) { ok = false; }
    RpcEndExcept
    return ok;
}

// ---------------------------------------------------------------------------
// Tray helpers
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
    if (!CallRpcStopService())
    {
        MessageBoxW(hwnd,
                    L"Could not contact TrayAppService.\n"
                    L"The service may not be running.",
                    L"TrayApp — Exit",
                    MB_ICONWARNING | MB_OK);
    }
    DestroyWindow(hwnd);
}

// ---------------------------------------------------------------------------
// Screen helpers
// ---------------------------------------------------------------------------
static void HideControl(HWND hwnd, int id)
{
    ShowWindow(GetDlgItem(hwnd, id), SW_HIDE);
}

static void ShowControl(HWND hwnd, int id)
{
    ShowWindow(GetDlgItem(hwnd, id), SW_SHOW);
}

static void SetControlText(HWND hwnd, int id, const wchar_t* text)
{
    SetWindowTextW(GetDlgItem(hwnd, id), text);
}

static void ShowScreen(HWND hwnd, int screen)
{
    // Hide all screens first
    int login[]  = { IDC_LBL_LOGIN_TITLE, IDC_LBL_EMAIL, IDC_EDT_EMAIL,
                     IDC_LBL_PASSWORD, IDC_EDT_PASSWORD,
                     IDC_BTN_LOGIN, IDC_LBL_LOGIN_ERR };
    int activ[]  = { IDC_LBL_ACT_USER, IDC_LBL_ACT_PROMPT, IDC_EDT_ACT_CODE,
                     IDC_BTN_ACTIVATE, IDC_LBL_ACT_ERR, IDC_LBL_ACT_DISABLED };
    int main3[]  = { IDC_LBL_MAIN_USER, IDC_LBL_MAIN_EXPIRY,
                     IDC_LBL_MAIN_STATUS, IDC_BTN_LOGOUT };

    for (int id : login)  HideControl(hwnd, id);
    for (int id : activ)  HideControl(hwnd, id);
    for (int id : main3)  HideControl(hwnd, id);

    g_curScreen = screen;

    if (screen == 1)
    {
        SetWindowTextW(hwnd, L"TrayApp - Login");
        for (int id : login) ShowControl(hwnd, id);
        HideControl(hwnd, IDC_LBL_LOGIN_ERR);
        SetControlText(hwnd, IDC_EDT_EMAIL, L"");
        SetControlText(hwnd, IDC_EDT_PASSWORD, L"");
    }
    else if (screen == 2)
    {
        SetWindowTextW(hwnd, L"TrayApp - No License");
        for (int id : activ) ShowControl(hwnd, id);
        HideControl(hwnd, IDC_LBL_ACT_ERR);

        if (BindRpc())
        {
            wchar_t* email = nullptr;
            RpcTryExcept { GetCurrentUser(g_hBinding, &email); }
            RpcExcept(1) { email = nullptr; }
            RpcEndExcept

            if (email)
            {
                wchar_t buf[512];
                StringCchPrintfW(buf, 512, L"Logged in as: %s", email);
                SetControlText(hwnd, IDC_LBL_ACT_USER, buf);
                MIDL_user_free(email);
            }
        }
        SetControlText(hwnd, IDC_EDT_ACT_CODE, L"");
    }
    else if (screen == 3)
    {
        SetWindowTextW(hwnd, L"TrayApp");
        for (int id : main3) ShowControl(hwnd, id);

        if (BindRpc())
        {
            wchar_t* email = nullptr;
            RpcTryExcept { GetCurrentUser(g_hBinding, &email); }
            RpcExcept(1) { email = nullptr; }
            RpcEndExcept

            if (email)
            {
                wchar_t buf[512];
                StringCchPrintfW(buf, 512, L"Logged in as: %s", email);
                SetControlText(hwnd, IDC_LBL_MAIN_USER, buf);
                MIDL_user_free(email);
            }

            LICENSE_INFO lic = {};
            RpcTryExcept { GetLicenseInfo(g_hBinding, &lic); }
            RpcExcept(1) {}
            RpcEndExcept

            if (lic.expiry_date[0])
            {
                wchar_t buf[512];
                StringCchPrintfW(buf, 512, L"License valid until: %s", lic.expiry_date);
                SetControlText(hwnd, IDC_LBL_MAIN_EXPIRY, buf);
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Window procedure
// ---------------------------------------------------------------------------
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (msg == g_taskbarMsg)
    {
        AddTrayIcon(hwnd);
        return 0;
    }

    switch (msg)
    {
    case WM_CREATE:
    {
        HINSTANCE hInst = ((CREATESTRUCT*)lParam)->hInstance;
        HFONT hFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);

        auto MakeCtrl = [&](const wchar_t* cls, const wchar_t* text, DWORD style,
                             int x, int y, int w, int h, int id) -> HWND {
            HWND hc = CreateWindowExW(0, cls, text,
                                      WS_CHILD | style,
                                      x, y, w, h, hwnd,
                                      (HMENU)(INT_PTR)id, hInst, NULL);
            if (hFont) SendMessage(hc, WM_SETFONT, (WPARAM)hFont, TRUE);
            return hc;
        };

        // Screen 1 — Login
        MakeCtrl(L"STATIC",  L"TrayApp Login",  SS_CENTER, 140, 20,  200, 24,  IDC_LBL_LOGIN_TITLE);
        MakeCtrl(L"STATIC",  L"Email:",          0,          60,  65,  80,  20,  IDC_LBL_EMAIL);
        MakeCtrl(L"EDIT",    L"",               WS_BORDER | ES_AUTOHSCROLL,
                                                            150,  60, 250,  24,  IDC_EDT_EMAIL);
        MakeCtrl(L"STATIC",  L"Password:",       0,          60, 105,  80,  20,  IDC_LBL_PASSWORD);
        MakeCtrl(L"EDIT",    L"",               WS_BORDER | ES_PASSWORD | ES_AUTOHSCROLL,
                                                            150, 100, 250,  24,  IDC_EDT_PASSWORD);
        MakeCtrl(L"BUTTON",  L"Login",           BS_DEFPUSHBUTTON,
                                                            175, 145, 130,  30,  IDC_BTN_LOGIN);
        {
            HWND hErr = MakeCtrl(L"STATIC", L"", SS_CENTER, 60, 190, 360, 20, IDC_LBL_LOGIN_ERR);
            // Red text for error label
            (void)hErr;
        }

        // Screen 2 — Activation
        MakeCtrl(L"STATIC",  L"",              0,           60,  20, 360,  20, IDC_LBL_ACT_USER);
        MakeCtrl(L"STATIC",  L"No active license. Enter activation code:",
                                               0,           60,  55, 360,  20, IDC_LBL_ACT_PROMPT);
        MakeCtrl(L"EDIT",    L"",              WS_BORDER | ES_AUTOHSCROLL,
                                                           150,  85, 250,  24, IDC_EDT_ACT_CODE);
        MakeCtrl(L"BUTTON",  L"Activate",      BS_DEFPUSHBUTTON,
                                                           175, 125, 130,  30, IDC_BTN_ACTIVATE);
        MakeCtrl(L"STATIC",  L"",              SS_CENTER,   60, 170, 360,  20, IDC_LBL_ACT_ERR);
        MakeCtrl(L"STATIC",  L"Antivirus functionality is disabled",
                              SS_CENTER,                    60, 200, 360,  20, IDC_LBL_ACT_DISABLED);

        // Screen 3 — Main
        MakeCtrl(L"STATIC",  L"",              0,           60,  30, 360,  20, IDC_LBL_MAIN_USER);
        MakeCtrl(L"STATIC",  L"",              0,           60,  60, 360,  20, IDC_LBL_MAIN_EXPIRY);
        MakeCtrl(L"STATIC",  L"Antivirus functionality: ENABLED",
                              SS_CENTER,                    60,  95, 360,  20, IDC_LBL_MAIN_STATUS);
        MakeCtrl(L"BUTTON",  L"Logout",        0,           175, 135, 130,  30, IDC_BTN_LOGOUT);

        // Menu bar
        HMENU hBar  = CreateMenu();
        HMENU hFile = CreatePopupMenu();
        AppendMenu(hFile, MF_STRING, IDM_FILE_EXIT, L"Exit");
        AppendMenu(hBar,  MF_POPUP, (UINT_PTR)hFile, L"&File");
        SetMenu(hwnd, hBar);

        // Color disabled label gray (done at paint time via WM_CTLCOLORSTATIC)

        return 0;
    }

    case WM_CTLCOLORSTATIC:
    {
        int id = GetDlgCtrlID((HWND)lParam);
        HDC hdc = (HDC)wParam;
        if (id == IDC_LBL_LOGIN_ERR || id == IDC_LBL_ACT_ERR)
        {
            SetTextColor(hdc, RGB(200, 0, 0));
            SetBkMode(hdc, TRANSPARENT);
            return (LRESULT)GetStockObject(NULL_BRUSH);
        }
        if (id == IDC_LBL_ACT_DISABLED)
        {
            SetTextColor(hdc, RGB(128, 128, 128));
            SetBkMode(hdc, TRANSPARENT);
            return (LRESULT)GetStockObject(NULL_BRUSH);
        }
        if (id == IDC_LBL_MAIN_STATUS)
        {
            SetTextColor(hdc, RGB(0, 160, 0));
            SetBkMode(hdc, TRANSPARENT);
            return (LRESULT)GetStockObject(NULL_BRUSH);
        }
        return DefWindowProc(hwnd, msg, wParam, lParam);
    }

    case WM_TIMER:
    {
        if (wParam != TIMER_POLL_LICENSE || !BindRpc()) break;

        LICENSE_INFO lic = {};
        long rc = 0;
        RpcTryExcept { rc = GetLicenseInfo(g_hBinding, &lic); }
        RpcExcept(1) { rc = 1; }
        RpcEndExcept

        if (rc != 0) break;

        if (g_curScreen == 3 && lic.is_licensed == 0)
        {
            // Was licensed, now expired or blocked — drop to activation screen
            SetControlText(hwnd, IDC_LBL_MAIN_STATUS,
                           lic.error_code == 3 ? L"Antivirus functionality: LICENSE EXPIRED"
                                               : L"Antivirus functionality: LICENSE BLOCKED");
            ShowScreen(hwnd, 2);
        }
        else if (g_curScreen == 2 && lic.is_licensed != 0)
        {
            // Background activation succeeded
            ShowScreen(hwnd, 3);
        }
        break;
    }

    case WM_COMMAND:
    {
        switch (LOWORD(wParam))
        {
        case IDM_OPEN:      ShowMainWindow(hwnd); break;
        case IDM_EXIT:
        case IDM_FILE_EXIT: DoExit(hwnd); break;

        case IDC_BTN_LOGIN:
        {
            if (!BindRpc()) break;
            wchar_t email[256] = {}, pass[256] = {};
            GetDlgItemTextW(hwnd, IDC_EDT_EMAIL,    email, 256);
            GetDlgItemTextW(hwnd, IDC_EDT_PASSWORD, pass,  256);

            long rc = 1;
            RpcTryExcept { rc = LoginUser(g_hBinding, email, pass); }
            RpcExcept(1) { rc = 1; }
            RpcEndExcept

            if (rc == 0)
            {
                HideControl(hwnd, IDC_LBL_LOGIN_ERR);
                LICENSE_INFO lic = {};
                RpcTryExcept { GetLicenseInfo(g_hBinding, &lic); }
                RpcExcept(1) {}
                RpcEndExcept
                ShowScreen(hwnd, lic.is_licensed ? 3 : 2);
            }
            else
            {
                SetControlText(hwnd, IDC_LBL_LOGIN_ERR,
                               L"Invalid email or password. Please try again.");
                ShowControl(hwnd, IDC_LBL_LOGIN_ERR);
            }
            break;
        }

        case IDC_BTN_ACTIVATE:
        {
            if (!BindRpc()) break;
            wchar_t code[256] = {};
            GetDlgItemTextW(hwnd, IDC_EDT_ACT_CODE, code, 256);

            long rc = 1;
            RpcTryExcept { rc = ActivateProduct(g_hBinding, code); }
            RpcExcept(1) { rc = 1; }
            RpcEndExcept

            if (rc == 0)
            {
                ShowScreen(hwnd, 3);
            }
            else
            {
                const wchar_t* msg2 = L"Activation failed. Please try again.";
                if      (rc == 2) msg2 = L"Activation code not found.";
                else if (rc == 3) msg2 = L"License has expired.";
                else if (rc == 4) msg2 = L"License is blocked.";
                else if (rc == 5) msg2 = L"Activation code already used.";
                SetControlText(hwnd, IDC_LBL_ACT_ERR, msg2);
                ShowControl(hwnd, IDC_LBL_ACT_ERR);
            }
            break;
        }

        case IDC_BTN_LOGOUT:
        {
            if (!BindRpc()) break;
            RpcTryExcept { LogoutUser(g_hBinding); }
            RpcExcept(1) {}
            RpcEndExcept
            ShowScreen(hwnd, 1);
            break;
        }
        }
        return 0;
    }

    case WM_TRAYICON:
        switch ((UINT)lParam)
        {
        case WM_LBUTTONUP: ShowMainWindow(hwnd); break;
        case WM_RBUTTONUP: ShowContextMenu(hwnd); break;
        }
        return 0;

    case WM_CLOSE:
        ShowWindow(hwnd, SW_HIDE);
        return 0;

    case WM_DESTROY:
        KillTimer(hwnd, TIMER_POLL_LICENSE);
        if (g_hBinding)
        {
            RpcBindingFree(&g_hBinding);
            g_hBinding = NULL;
        }
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

    if (!HandleServiceStartup())
        return 0;

    HANDLE hMutex = CreateMutexW(NULL, TRUE, L"Local\\TrayApp_SingleInstance");
    if (!hMutex || GetLastError() == ERROR_ALREADY_EXISTS)
    {
        if (hMutex) CloseHandle(hMutex);
        return 0;
    }

    g_taskbarMsg = RegisterWindowMessage(L"TaskbarCreated");

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
        0, L"TrayAppClass", L"TrayApp",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 500, 300,
        NULL, NULL, hInstance, NULL);

    if (!hwnd)
    {
        CloseHandle(hMutex);
        return 1;
    }

    AddTrayIcon(hwnd);

    // Startup flow: check login/license state and show appropriate screen
    int startScreen = 1;
    if (BindRpc())
    {
        wchar_t* email = nullptr;
        long rc = 1;
        RpcTryExcept { rc = GetCurrentUser(g_hBinding, &email); }
        RpcExcept(1) { rc = 1; email = nullptr; }
        RpcEndExcept

        if (rc == 0 && email && email[0] != L'\0')
        {
            LICENSE_INFO lic = {};
            RpcTryExcept { GetLicenseInfo(g_hBinding, &lic); }
            RpcExcept(1) {}
            RpcEndExcept
            startScreen = lic.is_licensed ? 3 : 2;
        }
        if (email) MIDL_user_free(email);
    }

    ShowScreen(hwnd, startScreen);
    SetTimer(hwnd, TIMER_POLL_LICENSE, 30000, NULL);

    ShowWindow(hwnd, hidden ? SW_HIDE : nCmdShow);
    UpdateWindow(hwnd);

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0))
    {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    CloseHandle(hMutex);
    return (int)msg.wParam;
}
