#define _WIN32_WINNT 0x0A00
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <dwmapi.h>
#include <uxtheme.h>
#include <stdint.h>
#include <atomic>
#include <iostream>
#include <fstream>
#include <string>
#include <algorithm>
#include <cwctype>
#include <vector>
#include <thread>

#include "keyboard_protocol.h"
#include "tray_icon.h"

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "uxtheme.lib")

// ------------------------- config -------------------------
static constexpr int PORT = 7777;
static constexpr int CLIP_PORT = 7778;
static constexpr int HOTKEY_ID = 1;
static constexpr UINT TRAY_ICON_ID = 1;
static constexpr UINT STATUS_TIMER_ID = 2;
static constexpr UINT STATUS_TIMER_MS = 1000;
static constexpr uint32_t CLIP_MAX_SIZE = 1024 * 1024 * 1024; // 1 GB
static constexpr DWORD CLIP_CONNECT_TIMEOUT_MS = 1000;
static constexpr DWORD CLIP_IO_TIMEOUT_MS = 3000;

static constexpr int IDC_TARGET_EDIT = 1001;
static constexpr int IDC_CLIPBOARD_CHECK = 1002;
static constexpr int IDC_SAVE_SETTINGS = 1003;
static constexpr int IDC_CANCEL_SETTINGS = 1004;
static constexpr int IDM_TOGGLE_KEYBOARD = 2001;
static constexpr int IDM_SETTINGS = 2002;
static constexpr int IDM_EXIT = 2003;

static constexpr COLORREF DARK_BACKGROUND = RGB(30, 30, 30);
static constexpr COLORREF DARK_CONTROL = RGB(45, 45, 48);
static constexpr COLORREF DARK_TEXT = RGB(240, 240, 240);

// hotkey configuration (defaults to Alt+1)
struct HotkeyConfig {
    UINT modifiers = MOD_ALT;
    UINT key = '1';
    std::wstring description = L"ALT+1";
};
static HotkeyConfig g_hotkey;

// ------------------------- clipboard protocol -------------
#pragma pack(push, 1)
struct ClipHeader
{
    uint8_t msg_type;      // 0=CLIP_DATA, 1=CLIP_REQUEST
    uint8_t content_type;  // 0=empty, 1=text(UTF-16LE), 2=image(DIB)
    uint32_t data_length;  // followed by this many bytes
};
#pragma pack(pop)

static constexpr uint8_t CLIP_MSG_DATA = 0;
static constexpr uint8_t CLIP_MSG_REQUEST = 1;
static constexpr uint8_t CLIP_CONTENT_EMPTY = 0;
static constexpr uint8_t CLIP_CONTENT_TEXT = 1;
static constexpr uint8_t CLIP_CONTENT_IMAGE = 2;

// ------------------------- TCP helpers --------------------
static bool TcpSendAll(SOCKET s, const char *buf, int len)
{
    while (len > 0)
    {
        int sent = send(s, buf, len, 0);
        if (sent <= 0) return false;
        buf += sent;
        len -= sent;
    }
    return true;
}

static bool TcpRecvAll(SOCKET s, char *buf, int len)
{
    while (len > 0)
    {
        int got = recv(s, buf, len, 0);
        if (got <= 0) return false;
        buf += got;
        len -= got;
    }
    return true;
}

// ------------------------- clipboard helpers ---------------
static void ReadClipboard(uint8_t &content_type, std::vector<char> &data)
{
    content_type = CLIP_CONTENT_EMPTY;
    data.clear();

    // Retry a few times — the clipboard may be briefly locked by another process.
    for (int attempt = 0; attempt < 10; ++attempt)
    {
        if (OpenClipboard(nullptr))
            break;
        Sleep(50);
        if (attempt == 9)
            return;
    }

    // try image first (CF_DIB)
    HANDLE hDib = GetClipboardData(CF_DIB);
    if (hDib)
    {
        SIZE_T sz = GlobalSize(hDib);
        if (sz > 0 && sz <= CLIP_MAX_SIZE)
        {
            void *ptr = GlobalLock(hDib);
            if (ptr)
            {
                data.assign(static_cast<char *>(ptr), static_cast<char *>(ptr) + sz);
                GlobalUnlock(hDib);
                content_type = CLIP_CONTENT_IMAGE;
                CloseClipboard();
                return;
            }
        }
    }

    // fall back to text (CF_UNICODETEXT)
    HANDLE hText = GetClipboardData(CF_UNICODETEXT);
    if (hText)
    {
        SIZE_T sz = GlobalSize(hText);
        if (sz > 0 && sz <= CLIP_MAX_SIZE)
        {
            void *ptr = GlobalLock(hText);
            if (ptr)
            {
                data.assign(static_cast<char *>(ptr), static_cast<char *>(ptr) + sz);
                GlobalUnlock(hText);
                content_type = CLIP_CONTENT_TEXT;
            }
        }
    }

    CloseClipboard();
}

static void WriteClipboard(uint8_t content_type, const std::vector<char> &data)
{
    if (content_type == CLIP_CONTENT_EMPTY || data.empty())
        return;

    if (!OpenClipboard(nullptr))
        return;
    EmptyClipboard();

    UINT fmt = (content_type == CLIP_CONTENT_IMAGE) ? CF_DIB : CF_UNICODETEXT;
    HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, data.size());
    if (hMem)
    {
        void *ptr = GlobalLock(hMem);
        if (ptr)
        {
            memcpy(ptr, data.data(), data.size());
            GlobalUnlock(hMem);
            SetClipboardData(fmt, hMem);
        }
        else
        {
            GlobalFree(hMem);
        }
    }

    CloseClipboard();
}

// ------------------------- globals ------------------------
static std::atomic<bool> g_capturing{false};
static std::atomic<bool> is_shift_pressed_down{false};
static std::atomic<bool> is_ctrl_pressed_down{false};
static std::atomic<bool> is_alt_pressed_down{false};
static SOCKET g_sock = INVALID_SOCKET;
static sockaddr_in g_recvAddr{};
static std::string g_targetHost;
static std::wstring g_targetText;
static std::wstring g_configPath;
static bool g_targetIsLiteralIp = false;
static bool g_targetConfigured = false;
static bool g_clipboardEnabled = false;
static std::atomic<bool> g_shuttingDown{false};
static HWND g_msgWnd = nullptr;
static HWND g_targetLabel = nullptr;
static HWND g_targetEdit = nullptr;
static HWND g_clipboardCheck = nullptr;
static HWND g_hotkeyLabel = nullptr;
static HWND g_saveButton = nullptr;
static HWND g_cancelButton = nullptr;
static HFONT g_uiFont = nullptr;
static HBRUSH g_darkBackgroundBrush = nullptr;
static HBRUSH g_darkControlBrush = nullptr;
static TrayIconState g_trayIcon;
static UINT g_taskbarCreatedMessage = 0;
constexpr UINT WM_APP_TOGGLE = WM_APP + 1; // used by LL hook to toggle when capturing
constexpr UINT WM_APP_TRAY = WM_APP + 2;

// Hook used only to block and relay local keyboard input while capturing.
static HHOOK g_keyHook = nullptr;

static void StartCapture();
static void StopCapture();
static void ShowSettingsWindow();
static void UpdateSenderTrayIcon();

static int ScaleForDpi(int value, UINT dpi)
{
    return MulDiv(value, static_cast<int>(dpi), 96);
}

static void ApplySettingsDpi(HWND window, UINT dpi)
{
    HFONT newFont = CreateFontW(
        -MulDiv(9, static_cast<int>(dpi), 72), 0, 0, 0, FW_NORMAL,
        FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
        CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
        L"Segoe UI");
    if (newFont)
    {
        HWND controls[] = {g_targetLabel, g_targetEdit, g_clipboardCheck,
                           g_hotkeyLabel, g_saveButton, g_cancelButton};
        for (HWND control : controls)
            SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(newFont), TRUE);
        if (g_uiFont)
            DeleteObject(g_uiFont);
        g_uiFont = newFont;
    }

    MoveWindow(g_targetLabel,
               ScaleForDpi(20, dpi), ScaleForDpi(20, dpi),
               ScaleForDpi(350, dpi), ScaleForDpi(20, dpi), TRUE);
    MoveWindow(g_targetEdit,
               ScaleForDpi(20, dpi), ScaleForDpi(43, dpi),
               ScaleForDpi(350, dpi), ScaleForDpi(25, dpi), TRUE);
    MoveWindow(g_clipboardCheck,
               ScaleForDpi(20, dpi), ScaleForDpi(82, dpi),
               ScaleForDpi(260, dpi), ScaleForDpi(24, dpi), TRUE);
    MoveWindow(g_hotkeyLabel,
               ScaleForDpi(20, dpi), ScaleForDpi(113, dpi),
               ScaleForDpi(250, dpi), ScaleForDpi(20, dpi), TRUE);
    MoveWindow(g_saveButton,
               ScaleForDpi(214, dpi), ScaleForDpi(145, dpi),
               ScaleForDpi(75, dpi), ScaleForDpi(26, dpi), TRUE);
    MoveWindow(g_cancelButton,
               ScaleForDpi(295, dpi), ScaleForDpi(145, dpi),
               ScaleForDpi(75, dpi), ScaleForDpi(26, dpi), TRUE);

    RedrawWindow(window, nullptr, nullptr,
                 RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_UPDATENOW);
}

static void EnableDarkWindowTheme(HWND window)
{
    BOOL enabled = TRUE;
    constexpr DWORD immersiveDarkMode = 20;
    constexpr DWORD immersiveDarkModeBefore20H1 = 19;
    if (FAILED(DwmSetWindowAttribute(window, immersiveDarkMode,
                                     &enabled, sizeof(enabled))))
    {
        DwmSetWindowAttribute(window, immersiveDarkModeBefore20H1,
                              &enabled, sizeof(enabled));
    }
    SetWindowTheme(window, L"DarkMode_Explorer", nullptr);
}

static void DestroyDarkThemeResources()
{
    if (g_uiFont)
        DeleteObject(g_uiFont);
    if (g_darkBackgroundBrush)
        DeleteObject(g_darkBackgroundBrush);
    if (g_darkControlBrush)
        DeleteObject(g_darkControlBrush);
    g_uiFont = nullptr;
    g_darkBackgroundBrush = nullptr;
    g_darkControlBrush = nullptr;
}

// ------------------------- net ----------------------------
static bool ResolveTarget()
{
    if (g_targetIsLiteralIp)
        return true;
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = IPPROTO_UDP;
    addrinfo *res = nullptr;
    if (getaddrinfo(g_targetHost.c_str(), nullptr, &hints, &res) != 0 || !res)
        return false;
    g_recvAddr.sin_addr = reinterpret_cast<sockaddr_in *>(res->ai_addr)->sin_addr;
    char resolved[INET_ADDRSTRLEN]{};
    inet_ntop(AF_INET, &g_recvAddr.sin_addr, resolved, sizeof(resolved));
    std::cout << "resolved " << g_targetHost << " -> " << resolved << "\n";
    freeaddrinfo(res);
    return true;
}

static std::string WideToUtf8(const std::wstring &value)
{
    if (value.empty())
        return {};

    int size = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1,
                                   nullptr, 0, nullptr, nullptr);
    if (size <= 1)
        return {};

    std::vector<char> buffer(static_cast<size_t>(size));
    WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1,
                        buffer.data(), size, nullptr, nullptr);
    return std::string(buffer.data());
}

static bool ConfigureTarget(const std::wstring &targetText)
{
    std::string targetHost = WideToUtf8(targetText);
    if (targetHost.empty())
        return false;

    sockaddr_in previousAddress = g_recvAddr;
    std::string previousHost = g_targetHost;
    bool previousLiteral = g_targetIsLiteralIp;

    g_recvAddr = {};
    g_recvAddr.sin_family = AF_INET;
    g_recvAddr.sin_port = htons(PORT);
    g_targetHost = targetHost;
    g_targetIsLiteralIp = inet_pton(AF_INET, targetHost.c_str(), &g_recvAddr.sin_addr) == 1;

    if (!ResolveTarget())
    {
        g_recvAddr = previousAddress;
        g_targetHost = previousHost;
        g_targetIsLiteralIp = previousLiteral;
        return false;
    }

    g_targetText = targetText;
    g_targetConfigured = true;
    return true;
}

static inline void SendPacket(const KeyboardPacket &p)
{
    if (!g_targetConfigured || g_sock == INVALID_SOCKET)
        return;

    sendto(g_sock, reinterpret_cast<const char *>(&p), sizeof(p), 0,
           reinterpret_cast<const sockaddr *>(&g_recvAddr), sizeof(g_recvAddr));
}

static void SendStatusPacket(bool receiverActive, int repeatCount = 1)
{
    if (!g_targetConfigured || g_sock == INVALID_SOCKET)
        return;

    KeyboardStatusPacket packet{};
    packet.receiverActive = receiverActive ? 1 : 0;
    for (int i = 0; i < repeatCount; ++i)
    {
        sendto(g_sock, reinterpret_cast<const char *>(&packet), sizeof(packet), 0,
               reinterpret_cast<const sockaddr *>(&g_recvAddr), sizeof(g_recvAddr));
    }
}

static void SetTcpTimeouts(SOCKET s, DWORD timeout_ms)
{
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO,
               reinterpret_cast<const char *>(&timeout_ms), sizeof(timeout_ms));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO,
               reinterpret_cast<const char *>(&timeout_ms), sizeof(timeout_ms));
}

static SOCKET ConnectClipboardSocket(in_addr receiverAddress)
{
    SOCKET cs = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (cs == INVALID_SOCKET)
        return INVALID_SOCKET;

    u_long nonblocking = 1;
    ioctlsocket(cs, FIONBIO, &nonblocking);

    sockaddr_in clipAddr{};
    clipAddr.sin_family = AF_INET;
    clipAddr.sin_port = htons(CLIP_PORT);
    clipAddr.sin_addr = receiverAddress;

    int rc = connect(cs, reinterpret_cast<sockaddr *>(&clipAddr), sizeof(clipAddr));
    if (rc == SOCKET_ERROR)
    {
        int err = WSAGetLastError();
        if (err != WSAEWOULDBLOCK && err != WSAEINPROGRESS && err != WSAEALREADY)
        {
            closesocket(cs);
            return INVALID_SOCKET;
        }

        fd_set wset;
        FD_ZERO(&wset);
        FD_SET(cs, &wset);
        timeval tv{
            static_cast<long>(CLIP_CONNECT_TIMEOUT_MS / 1000),
            static_cast<long>((CLIP_CONNECT_TIMEOUT_MS % 1000) * 1000)};

        if (select(0, nullptr, &wset, nullptr, &tv) <= 0)
        {
            closesocket(cs);
            return INVALID_SOCKET;
        }

        int soerr = 0;
        int soerrLen = sizeof(soerr);
        if (getsockopt(cs, SOL_SOCKET, SO_ERROR,
                       reinterpret_cast<char *>(&soerr), &soerrLen) == SOCKET_ERROR ||
            soerr != 0)
        {
            closesocket(cs);
            return INVALID_SOCKET;
        }
    }

    u_long blocking = 0;
    ioctlsocket(cs, FIONBIO, &blocking);
    SetTcpTimeouts(cs, CLIP_IO_TIMEOUT_MS);
    return cs;
}

static void PushClipboardToReceiver(in_addr receiverAddress)
{
    uint8_t clipCt;
    std::vector<char> clipPayload;
    ReadClipboard(clipCt, clipPayload);

    if (g_shuttingDown.load(std::memory_order_relaxed))
        return;

    SOCKET cs = ConnectClipboardSocket(receiverAddress);
    if (cs == INVALID_SOCKET)
        return;

    ClipHeader hdr{};
    hdr.msg_type = CLIP_MSG_DATA;
    hdr.content_type = clipCt;
    hdr.data_length = static_cast<uint32_t>(clipPayload.size());

    if (TcpSendAll(cs, reinterpret_cast<char *>(&hdr), sizeof(hdr)) && !clipPayload.empty())
        TcpSendAll(cs, clipPayload.data(), static_cast<int>(clipPayload.size()));

    closesocket(cs);
}

static void PullClipboardFromReceiver(in_addr receiverAddress)
{
    SOCKET cs = ConnectClipboardSocket(receiverAddress);
    if (cs == INVALID_SOCKET)
        return;

    ClipHeader hdr{};
    hdr.msg_type = CLIP_MSG_REQUEST;
    hdr.content_type = CLIP_CONTENT_EMPTY;
    hdr.data_length = 0;

    uint8_t respType = CLIP_CONTENT_EMPTY;
    std::vector<char> clipData;
    bool gotClipboard = false;

    if (TcpSendAll(cs, reinterpret_cast<char *>(&hdr), sizeof(hdr)))
    {
        ClipHeader resp{};
        if (TcpRecvAll(cs, reinterpret_cast<char *>(&resp), sizeof(resp)) &&
            resp.msg_type == CLIP_MSG_DATA &&
            resp.data_length <= CLIP_MAX_SIZE)
        {
            clipData.resize(resp.data_length);
            if (resp.data_length == 0 ||
                TcpRecvAll(cs, clipData.data(), static_cast<int>(resp.data_length)))
            {
                respType = resp.content_type;
                gotClipboard = true;
            }
        }
    }

    closesocket(cs);

    if (gotClipboard && !g_shuttingDown.load(std::memory_order_relaxed))
        WriteClipboard(respType, clipData);
}

// ------------------------- hotkey parsing -----------------
static bool ParseHotkey(const std::wstring& hotkeyStr, HotkeyConfig& config)
{
    std::wstring str = hotkeyStr;
    // convert to uppercase for case-insensitive parsing
    std::transform(str.begin(), str.end(), str.begin(), ::towupper);

    UINT mods = 0;
    UINT key = 0;

    // parse modifiers
    if (str.find(L"CTRL") != std::wstring::npos || str.find(L"CONTROL") != std::wstring::npos)
        mods |= MOD_CONTROL;
    if (str.find(L"SHIFT") != std::wstring::npos)
        mods |= MOD_SHIFT;
    if (str.find(L"ALT") != std::wstring::npos)
        mods |= MOD_ALT;
    if (str.find(L"WIN") != std::wstring::npos)
        mods |= MOD_WIN;

    // find the key (last character that's not '+')
    size_t lastPlus = str.rfind(L'+');
    if (lastPlus != std::wstring::npos && lastPlus < str.length() - 1)
    {
        std::wstring keyStr = str.substr(lastPlus + 1);
        // trim whitespace
        keyStr.erase(0, keyStr.find_first_not_of(L" \t"));
        keyStr.erase(keyStr.find_last_not_of(L" \t") + 1);

        if (keyStr.length() == 1)
        {
            wchar_t c = keyStr[0];
            if ((c >= L'0' && c <= L'9') || (c >= L'A' && c <= L'Z'))
            {
                key = (UINT)c;
            }
        }
        else if (keyStr.length() == 2 && keyStr[0] == L'F' && keyStr[1] >= L'1' && keyStr[1] <= L'9')
        {
            // F1-F9
            key = VK_F1 + (keyStr[1] - L'1');
        }
        else if (keyStr.length() == 3 && keyStr[0] == L'F' && keyStr[1] == L'1' && keyStr[2] >= L'0' && keyStr[2] <= L'2')
        {
            // F10-F12
            key = VK_F10 + (keyStr[2] - L'0');
        }
    }

    if (key == 0)
        return false;

    config.modifiers = mods;
    config.key = key;
    config.description = hotkeyStr;
    return true;
}

static std::wstring Trim(const std::wstring &value)
{
    size_t first = value.find_first_not_of(L" \t\r\n");
    if (first == std::wstring::npos)
        return {};
    size_t last = value.find_last_not_of(L" \t\r\n");
    return value.substr(first, last - first + 1);
}

static std::wstring GetConfigPath()
{
    wchar_t modulePath[MAX_PATH]{};
    DWORD length = GetModuleFileNameW(nullptr, modulePath, ARRAYSIZE(modulePath));
    if (length == 0 || length >= ARRAYSIZE(modulePath))
        return L"sender.cfg";

    std::wstring path(modulePath, length);
    size_t slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos)
        return L"sender.cfg";
    return path.substr(0, slash + 1) + L"sender.cfg";
}

static void ReadSettingsFile()
{
    std::wifstream file(g_configPath.c_str());
    if (!file.is_open())
        return;

    std::wstring line;
    while (std::getline(file, line))
    {
        line = Trim(line);
        if (line.empty() || line[0] == L'#' || line[0] == L';')
            continue;

        size_t equals = line.find(L'=');
        if (equals == std::wstring::npos)
            continue;

        std::wstring key = Trim(line.substr(0, equals));
        std::wstring value = Trim(line.substr(equals + 1));
        std::transform(key.begin(), key.end(), key.begin(), ::towupper);

        if (key == L"RECEIVER")
            g_targetText = value;
        else if (key == L"HOTKEY")
            ParseHotkey(value, g_hotkey);
        else if (key == L"CLIPBOARD")
            g_clipboardEnabled = value == L"1" || value == L"TRUE" || value == L"ON";
    }
}

static bool SaveSettingsFile()
{
    std::wofstream file(g_configPath.c_str(), std::ios::trunc);
    if (!file.is_open())
        return false;

    file << L"RECEIVER=" << g_targetText << L"\n";
    file << L"HOTKEY=" << g_hotkey.description << L"\n";
    file << L"CLIPBOARD=" << (g_clipboardEnabled ? 1 : 0) << L"\n";
    return file.good();
}

static bool IsHotkeyPressed(DWORD vk)
{
    // check key matches
    if (vk != g_hotkey.key)
        return false;

    auto key_down = [](int key) {
        return (GetAsyncKeyState(key) & 0x8000) != 0;
    };

    // check that required modifiers are pressed (using tracked state)
    if (g_hotkey.modifiers & MOD_CONTROL)
        if (!is_ctrl_pressed_down.load(std::memory_order_relaxed) &&
            !key_down(VK_CONTROL) && !key_down(VK_LCONTROL) && !key_down(VK_RCONTROL))
            return false;

    if (g_hotkey.modifiers & MOD_SHIFT)
        if (!is_shift_pressed_down.load(std::memory_order_relaxed) &&
            !key_down(VK_SHIFT) && !key_down(VK_LSHIFT) && !key_down(VK_RSHIFT))
            return false;

    if (g_hotkey.modifiers & MOD_ALT)
        if (!is_alt_pressed_down.load(std::memory_order_relaxed) &&
            !key_down(VK_MENU) && !key_down(VK_LMENU) && !key_down(VK_RMENU))
            return false;

    if (g_hotkey.modifiers & MOD_WIN)
        if (!key_down(VK_LWIN) && !key_down(VK_RWIN))
            return false;

    return true;
}

static LRESULT CALLBACK LLKbdHook(int nCode, WPARAM wParam, LPARAM lParam)
{
    if (nCode < 0)
        return CallNextHookEx(nullptr, nCode, wParam, lParam);
    if (!g_capturing.load(std::memory_order_relaxed))
        return CallNextHookEx(nullptr, nCode, wParam, lParam);

    const KBDLLHOOKSTRUCT *k = reinterpret_cast<const KBDLLHOOKSTRUCT *>(lParam);
    const bool isDown = (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN);
    const DWORD vk = k->vkCode;

    // track ctrl/shift/alt state
    if (vk == VK_SHIFT || vk == VK_LSHIFT || vk == VK_RSHIFT)
        is_shift_pressed_down.store(isDown, std::memory_order_relaxed);
    else if (vk == VK_CONTROL || vk == VK_LCONTROL || vk == VK_RCONTROL)
        is_ctrl_pressed_down.store(isDown, std::memory_order_relaxed);
    else if (vk == VK_MENU || vk == VK_LMENU || vk == VK_RMENU)
        is_alt_pressed_down.store(isDown, std::memory_order_relaxed);

    // dynamic hotkey toggle
    if (isDown && IsHotkeyPressed(vk))
    {
        PostMessageW(g_msgWnd, WM_APP_TOGGLE, 0, 0);
        return 1;
    }

    // Send keyboard packet directly from the hook since RIDEV_INPUTSINK doesn't work for keyboards
    // Note: this WOULD be better over Raw Input, but no matter what, I could NOT get the Raw Input messages for the keyboard to fire.
    // So for now, LL hook it is.
    KeyboardPacket p{};
    p.vkCode = static_cast<uint16_t>(vk);
    p.down = isDown ? 1 : 0;
    SendPacket(p);

    // block locally
    return 1;
}

// ------------------------- tray and settings -------------
static void UpdateSenderTrayIcon()
{
    bool localActive = !g_capturing.load(std::memory_order_relaxed);
    const wchar_t *tooltip = localActive
        ? L"Keyboard: this PC is in use"
        : L"Keyboard: receiver is in use";
    SetTrayIcon(g_trayIcon, g_msgWnd, TRAY_ICON_ID, WM_APP_TRAY, localActive, tooltip);
}

static void ShowSettingsWindow()
{
    if (g_capturing.load(std::memory_order_relaxed))
        StopCapture();

    SetWindowTextW(g_targetEdit, g_targetText.c_str());
    SendMessageW(g_clipboardCheck, BM_SETCHECK,
                 g_clipboardEnabled ? BST_CHECKED : BST_UNCHECKED, 0);

    RECT windowRect{};
    GetWindowRect(g_msgWnd, &windowRect);
    int width = windowRect.right - windowRect.left;
    int height = windowRect.bottom - windowRect.top;
    RECT workArea{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &workArea, 0);
    int x = workArea.left + (workArea.right - workArea.left - width) / 2;
    int y = workArea.top + (workArea.bottom - workArea.top - height) / 2;
    SetWindowPos(g_msgWnd, HWND_TOP, x, y, 0, 0, SWP_NOSIZE | SWP_SHOWWINDOW);
    SetForegroundWindow(g_msgWnd);
    SetFocus(g_targetEdit);
    SendMessageW(g_targetEdit, EM_SETSEL, 0, -1);
}

static bool SaveSettingsFromWindow()
{
    int length = GetWindowTextLengthW(g_targetEdit);
    std::vector<wchar_t> buffer(static_cast<size_t>(length) + 1);
    GetWindowTextW(g_targetEdit, buffer.data(), static_cast<int>(buffer.size()));
    std::wstring target = Trim(buffer.data());

    if (target.empty())
    {
        MessageBoxW(g_msgWnd, L"Enter the receiver IP address or host name.",
                    L"Keyboard Sharing", MB_OK | MB_ICONWARNING);
        SetFocus(g_targetEdit);
        return false;
    }

    if (!ConfigureTarget(target))
    {
        MessageBoxW(g_msgWnd, L"The receiver address could not be found.",
                    L"Keyboard Sharing", MB_OK | MB_ICONERROR);
        SetFocus(g_targetEdit);
        return false;
    }

    g_clipboardEnabled = SendMessageW(g_clipboardCheck, BM_GETCHECK, 0, 0) == BST_CHECKED;
    if (!SaveSettingsFile())
    {
        MessageBoxW(g_msgWnd, L"The settings file could not be saved beside sender.exe.",
                    L"Keyboard Sharing", MB_OK | MB_ICONERROR);
        return false;
    }

    ShowWindow(g_msgWnd, SW_HIDE);
    UpdateSenderTrayIcon();
    return true;
}

static void ShowSenderTrayMenu()
{
    HMENU menu = CreatePopupMenu();
    if (!menu)
        return;

    bool receiverActive = g_capturing.load(std::memory_order_relaxed);
    AppendMenuW(menu, MF_STRING | MF_GRAYED, 0,
                receiverActive ? L"Receiver has the keyboard" : L"This PC has the keyboard");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, IDM_TOGGLE_KEYBOARD, L"Toggle keyboard");
    AppendMenuW(menu, MF_STRING, IDM_SETTINGS, L"Settings...");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, IDM_EXIT, L"Exit");

    POINT cursor{};
    GetCursorPos(&cursor);
    SetForegroundWindow(g_msgWnd);
    int command = TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY,
                                 cursor.x, cursor.y, 0, g_msgWnd, nullptr);
    DestroyMenu(menu);

    if (command == IDM_TOGGLE_KEYBOARD)
    {
        if (receiverActive)
            StopCapture();
        else
            StartCapture();
    }
    else if (command == IDM_SETTINGS)
    {
        ShowSettingsWindow();
    }
    else if (command == IDM_EXIT)
    {
        g_shuttingDown.store(true, std::memory_order_relaxed);
        if (g_capturing.load(std::memory_order_relaxed))
            StopCapture();
        RemoveTrayIcon(g_trayIcon);
        DestroyWindow(g_msgWnd);
    }
}

// ------------------------- capture on/off -----------------
static void StartCapture()
{
    if (!g_targetConfigured)
    {
        ShowSettingsWindow();
        return;
    }

    if (g_capturing.exchange(true))
        return;

    // to make sure that ctrl, shift, and alt don't get stuck down when toggling capture while holding them
    keybd_event(VK_CONTROL, 0, KEYEVENTF_KEYUP, 0);
    keybd_event(VK_SHIFT, 0, KEYEVENTF_KEYUP, 0);
    keybd_event(VK_MENU, 0, KEYEVENTF_KEYUP, 0);
    is_ctrl_pressed_down.store(false, std::memory_order_relaxed);
    is_shift_pressed_down.store(false, std::memory_order_relaxed);
    is_alt_pressed_down.store(false, std::memory_order_relaxed);

    // Install the keyboard hook only. The mouse remains local and untouched.
    g_keyHook = SetWindowsHookExW(WH_KEYBOARD_LL, LLKbdHook, GetModuleHandleW(nullptr), 0);
    if (!g_keyHook)
    {
        std::wcerr << L"SetWindowsHookEx failed: " << GetLastError() << L"\n";
        g_capturing = false;
        MessageBoxW(g_msgWnd, L"The keyboard hook could not start.",
                    L"Keyboard Sharing", MB_OK | MB_ICONERROR);
        UpdateSenderTrayIcon();
        return;
    }

    SendStatusPacket(true, 3);
    SetTimer(g_msgWnd, STATUS_TIMER_ID, STATUS_TIMER_MS, nullptr);
    UpdateSenderTrayIcon();

    if (g_clipboardEnabled && !g_shuttingDown.load(std::memory_order_relaxed))
        std::thread(PushClipboardToReceiver, g_recvAddr.sin_addr).detach();

    std::wcout << L"capture: ON (local keyboard is blocked; mouse remains local)\n";
}

static void StopCapture()
{
    if (!g_capturing.exchange(false))
        return;

    KillTimer(g_msgWnd, STATUS_TIMER_ID);

    // Key-up spam is harmless and prevents stuck remote modifiers after lag spikes.
    {
        KeyboardPacket p{};
        p.down = 0;

        p.vkCode = VK_CONTROL;
        SendPacket(p);
        p.vkCode = VK_SHIFT;
        SendPacket(p);
        p.vkCode = VK_MENU;
        SendPacket(p);

        is_ctrl_pressed_down.store(false, std::memory_order_relaxed);
        is_shift_pressed_down.store(false, std::memory_order_relaxed);
        is_alt_pressed_down.store(false, std::memory_order_relaxed);
    }

    // Remove the keyboard hook.
    if (g_keyHook)
    {
        UnhookWindowsHookEx(g_keyHook);
        g_keyHook = nullptr;
    }

    SendStatusPacket(false, 3);
    UpdateSenderTrayIcon();

    if (g_clipboardEnabled && !g_shuttingDown.load(std::memory_order_relaxed))
        std::thread(PullClipboardFromReceiver, g_recvAddr.sin_addr).detach();

    std::wcout << L"capture: OFF (local keyboard restored)\n";
}

// ------------------------- window proc --------------------
static LRESULT CALLBACK HiddenWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (g_taskbarCreatedMessage != 0 && msg == g_taskbarCreatedMessage)
    {
        g_trayIcon.added = false;
        UpdateSenderTrayIcon();
        return 0;
    }

    switch (msg)
    {
    case WM_CREATE:
    {
        g_targetLabel = CreateWindowExW(0, L"STATIC", L"Receiver IP address or host name:",
                                        WS_CHILD | WS_VISIBLE,
                                        0, 0, 1, 1, hwnd, nullptr, nullptr, nullptr);
        g_targetEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                      WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                                      0, 0, 1, 1, hwnd,
                                      reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_TARGET_EDIT)),
                                      nullptr, nullptr);
        g_clipboardCheck = CreateWindowExW(0, L"BUTTON", L"Enable clipboard sharing",
                                           WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                                           0, 0, 1, 1, hwnd,
                                           reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_CLIPBOARD_CHECK)),
                                           nullptr, nullptr);
        std::wstring hotkeyText = L"Keyboard toggle: " + g_hotkey.description;
        g_hotkeyLabel = CreateWindowExW(0, L"STATIC", hotkeyText.c_str(),
                                        WS_CHILD | WS_VISIBLE,
                                        0, 0, 1, 1, hwnd, nullptr, nullptr, nullptr);
        g_saveButton = CreateWindowExW(0, L"BUTTON", L"Save",
                                       WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                                       0, 0, 1, 1, hwnd,
                                       reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_SAVE_SETTINGS)),
                                       nullptr, nullptr);
        g_cancelButton = CreateWindowExW(0, L"BUTTON", L"Cancel",
                                         WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                         0, 0, 1, 1, hwnd,
                                         reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_CANCEL_SETTINGS)),
                                         nullptr, nullptr);

        SetWindowTheme(g_targetEdit, L"DarkMode_CFD", nullptr);
        SetWindowTheme(g_clipboardCheck, L"DarkMode_Explorer", nullptr);
        SetWindowTheme(g_saveButton, L"DarkMode_Explorer", nullptr);
        SetWindowTheme(g_cancelButton, L"DarkMode_Explorer", nullptr);
        ApplySettingsDpi(hwnd, GetDpiForWindow(hwnd));
        return 0;
    }

    case WM_DPICHANGED:
    {
        RECT *suggestedRect = reinterpret_cast<RECT *>(lParam);
        SetWindowPos(hwnd, nullptr,
                     suggestedRect->left, suggestedRect->top,
                     suggestedRect->right - suggestedRect->left,
                     suggestedRect->bottom - suggestedRect->top,
                     SWP_NOACTIVATE | SWP_NOZORDER);
        ApplySettingsDpi(hwnd, HIWORD(wParam));
        return 0;
    }

    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
    {
        HDC deviceContext = reinterpret_cast<HDC>(wParam);
        SetTextColor(deviceContext, DARK_TEXT);
        SetBkColor(deviceContext, DARK_BACKGROUND);
        SetBkMode(deviceContext, TRANSPARENT);
        return reinterpret_cast<LRESULT>(g_darkBackgroundBrush);
    }

    case WM_CTLCOLOREDIT:
    {
        HDC deviceContext = reinterpret_cast<HDC>(wParam);
        SetTextColor(deviceContext, DARK_TEXT);
        SetBkColor(deviceContext, DARK_CONTROL);
        return reinterpret_cast<LRESULT>(g_darkControlBrush);
    }

    case WM_APP_TOGGLE:
        if (g_capturing.load())
            StopCapture();
        else
            StartCapture();
        return 0;

    case WM_APP_TRAY:
        if (lParam == WM_RBUTTONUP || lParam == WM_CONTEXTMENU)
            ShowSenderTrayMenu();
        else if (lParam == WM_LBUTTONDBLCLK)
            ShowSettingsWindow();
        return 0;

    case WM_HOTKEY:
        if (wParam == HOTKEY_ID)
        {
            if (g_capturing.load())
                StopCapture();
            else
                StartCapture();
        }
        return 0;

    case WM_TIMER:
        if (wParam == STATUS_TIMER_ID && g_capturing.load(std::memory_order_relaxed))
            SendStatusPacket(true);
        return 0;

    case WM_COMMAND:
        if (LOWORD(wParam) == IDC_SAVE_SETTINGS)
        {
            SaveSettingsFromWindow();
            return 0;
        }
        if (LOWORD(wParam) == IDC_CANCEL_SETTINGS)
        {
            ShowWindow(hwnd, SW_HIDE);
            return 0;
        }
        break;

    case WM_CLOSE:
        ShowWindow(hwnd, SW_HIDE);
        return 0;

    case WM_DESTROY:
        RemoveTrayIcon(g_trayIcon);
        PostQuitMessage(0);
        return 0;

    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// ------------------------- entry --------------------------
int wmain(int argc, wchar_t *argv[])
{
    if (!SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2))
        SetProcessDPIAware();

    g_configPath = GetConfigPath();
    ReadSettingsFile();

    // A command-line address is still accepted as a temporary override.
    for (int i = 1; i < argc; ++i)
    {
        std::wstring arg = argv[i];
        if (arg.find(L"--hotkey=") == 0)
        {
            std::wstring hotkeyStr = arg.substr(9);
            HotkeyConfig tempConfig;
            if (ParseHotkey(hotkeyStr, tempConfig))
            {
                g_hotkey = tempConfig;
                std::wcout << L"CLI override: Using hotkey " << g_hotkey.description << L"\n";
            }
            else
            {
                MessageBoxW(nullptr, L"The command-line hotkey is not valid.",
                            L"Keyboard Sharing", MB_OK | MB_ICONERROR);
                return 1;
            }
        }
        else if (!arg.empty() && arg[0] != L'-')
        {
            g_targetText = arg;
        }
    }

    // init sockets
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
    {
        std::cerr << "WSAStartup failed\n";
        return 1;
    }
    g_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (g_sock == INVALID_SOCKET)
    {
        std::cerr << "socket() failed\n";
        WSACleanup();
        return 1;
    }
    if (!g_targetText.empty() && !ConfigureTarget(g_targetText))
    {
        g_targetText.clear();
        g_targetConfigured = false;
    }

    g_darkBackgroundBrush = CreateSolidBrush(DARK_BACKGROUND);
    g_darkControlBrush = CreateSolidBrush(DARK_CONTROL);
    if (!g_darkBackgroundBrush || !g_darkControlBrush)
    {
        DestroyDarkThemeResources();
        closesocket(g_sock);
        WSACleanup();
        return 1;
    }

    // Window class and hidden settings window.
    WNDCLASSEXW wc{sizeof(WNDCLASSEXW)};
    wc.lpfnWndProc = HiddenWndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"KeyboardSharingSenderWnd";
    wc.hbrBackground = g_darkBackgroundBrush;
    if (!RegisterClassExW(&wc))
    {
        std::cerr << "RegisterClassExW failed\n";
        DestroyDarkThemeResources();
        closesocket(g_sock);
        WSACleanup();
        return 1;
    }

    UINT initialDpi = GetDpiForSystem();
    RECT windowRect{
        0, 0,
        ScaleForDpi(390, initialDpi),
        ScaleForDpi(190, initialDpi)};
    AdjustWindowRectExForDpi(
        &windowRect, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
        FALSE, WS_EX_TOOLWINDOW, initialDpi);
    g_msgWnd = CreateWindowExW(
        WS_EX_TOOLWINDOW, wc.lpszClassName, L"Keyboard Sharing Settings",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
        CW_USEDEFAULT, CW_USEDEFAULT,
        windowRect.right - windowRect.left, windowRect.bottom - windowRect.top,
        nullptr, nullptr, wc.hInstance, nullptr);
    if (!g_msgWnd)
    {
        std::cerr << "CreateWindowExW failed\n";
        DestroyDarkThemeResources();
        closesocket(g_sock);
        WSACleanup();
        return 1;
    }

    // register dynamic hotkey (toggle capture)
    if (!RegisterHotKey(g_msgWnd, HOTKEY_ID, g_hotkey.modifiers, g_hotkey.key))
    {
        MessageBoxW(nullptr, L"The keyboard hotkey is in use by another application.",
                    L"Keyboard Sharing", MB_OK | MB_ICONERROR);
        DestroyWindow(g_msgWnd);
        DestroyDarkThemeResources();
        closesocket(g_sock);
        WSACleanup();
        return 1;
    }

    EnableDarkWindowTheme(g_msgWnd);

    g_taskbarCreatedMessage = RegisterWindowMessageW(L"TaskbarCreated");

    UpdateSenderTrayIcon();
    if (!g_targetConfigured)
        ShowSettingsWindow();

    // message loop
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0))
    {
        if (IsWindowVisible(g_msgWnd) && IsDialogMessageW(g_msgWnd, &msg))
            continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    // cleanup
    g_shuttingDown.store(true, std::memory_order_relaxed);
    if (g_capturing.load(std::memory_order_relaxed))
        StopCapture();
    UnregisterHotKey(g_msgWnd, HOTKEY_ID);
    RemoveTrayIcon(g_trayIcon);
    if (g_msgWnd && IsWindow(g_msgWnd))
        DestroyWindow(g_msgWnd);
    if (g_sock != INVALID_SOCKET)
        closesocket(g_sock);
    WSACleanup();
    DestroyDarkThemeResources();
    return 0;
}
