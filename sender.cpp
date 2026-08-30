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
#include <cwchar>
#include <cwctype>
#include <array>
#include <vector>
#include <thread>
#include <mutex>

#include "clipboard_protocol.h"
#include "keyboard_protocol.h"
#include "tray_icon.h"
#include "windows_app.h"

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "uxtheme.lib")
#pragma comment(lib, "advapi32.lib")

// ------------------------- config -------------------------
static constexpr int PORT = 7777;
static constexpr int CLIP_PORT = 7778;
static constexpr int HOTKEY_ID = 1;
static constexpr UINT TRAY_ICON_ID = 1;
static constexpr UINT STATUS_TIMER_ID = 2;
static constexpr UINT STATUS_TIMER_MS = 1000;
static constexpr DWORD STATUS_ACK_TIMEOUT_MS = 600;
static constexpr DWORD CLIP_CONNECT_TIMEOUT_MS = 1000;
static constexpr DWORD CLIP_IO_TIMEOUT_MS = 3000;
static constexpr wchar_t STARTUP_VALUE_NAME[] = L"LeanKeyboardSharingSender";

static constexpr int IDC_TARGET_EDIT = 1001;
static constexpr int IDC_CLIPBOARD_CHECK = 1002;
static constexpr int IDC_SAVE_SETTINGS = 1003;
static constexpr int IDC_CANCEL_SETTINGS = 1004;
static constexpr int IDC_HOTKEY_EDIT = 1005;
static constexpr int IDC_STARTUP_CHECK = 1006;
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
static HotkeyConfig g_pendingHotkey;

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
struct ClipboardTransferRequest
{
    bool pushToReceiver = false;
    in_addr receiverAddress{};
};
static std::mutex g_clipboardTransferMutex;
static bool g_clipboardTransferActive = false;
static bool g_clipboardTransferPending = false;
static ClipboardTransferRequest g_pendingClipboardTransfer;
static std::thread g_clipboardThread;
static std::uint16_t g_statusRequestId = 0;
static HWND g_msgWnd = nullptr;
static HWND g_targetLabel = nullptr;
static HWND g_targetEdit = nullptr;
static HWND g_clipboardCheck = nullptr;
static HWND g_hotkeyLabel = nullptr;
static HWND g_hotkeyEdit = nullptr;
static HWND g_startupCheck = nullptr;
static HWND g_saveButton = nullptr;
static HWND g_cancelButton = nullptr;
static HFONT g_uiFont = nullptr;
static HBRUSH g_darkBackgroundBrush = nullptr;
static HBRUSH g_darkControlBrush = nullptr;
static TrayIconState g_trayIcon;
static UINT g_taskbarCreatedMessage = 0;
static WNDPROC g_originalHotkeyEditProc = nullptr;
static bool g_hotkeyRegistered = false;
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
        HWND controls[] = {g_targetLabel, g_targetEdit, g_hotkeyLabel,
                           g_hotkeyEdit, g_clipboardCheck, g_startupCheck,
                           g_saveButton, g_cancelButton};
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
    MoveWindow(g_hotkeyLabel,
               ScaleForDpi(20, dpi), ScaleForDpi(82, dpi),
               ScaleForDpi(110, dpi), ScaleForDpi(20, dpi), TRUE);
    MoveWindow(g_hotkeyEdit,
               ScaleForDpi(140, dpi), ScaleForDpi(78, dpi),
               ScaleForDpi(230, dpi), ScaleForDpi(25, dpi), TRUE);
    MoveWindow(g_clipboardCheck,
               ScaleForDpi(20, dpi), ScaleForDpi(117, dpi),
               ScaleForDpi(260, dpi), ScaleForDpi(24, dpi), TRUE);
    MoveWindow(g_startupCheck,
               ScaleForDpi(20, dpi), ScaleForDpi(147, dpi),
               ScaleForDpi(260, dpi), ScaleForDpi(24, dpi), TRUE);
    MoveWindow(g_saveButton,
               ScaleForDpi(214, dpi), ScaleForDpi(181, dpi),
               ScaleForDpi(75, dpi), ScaleForDpi(26, dpi), TRUE);
    MoveWindow(g_cancelButton,
               ScaleForDpi(295, dpi), ScaleForDpi(181, dpi),
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

static void SendStatusPacket(
    bool receiverActive, int repeatCount = 1, std::uint16_t requestId = 0)
{
    if (!g_targetConfigured || g_sock == INVALID_SOCKET)
        return;

    KeyboardStatusPacket packet{};
    packet.receiverActive = receiverActive ? 1 : 0;
    packet.requestId = requestId;
    for (int i = 0; i < repeatCount; ++i)
    {
        sendto(g_sock, reinterpret_cast<const char *>(&packet), sizeof(packet), 0,
               reinterpret_cast<const sockaddr *>(&g_recvAddr), sizeof(g_recvAddr));
    }
}

static void DrainStatusResponses()
{
    for (;;)
    {
        fd_set readSet;
        FD_ZERO(&readSet);
        FD_SET(g_sock, &readSet);
        timeval timeout{};
        if (select(0, &readSet, nullptr, nullptr, &timeout) <= 0)
            return;

        std::array<char, sizeof(KeyboardAckPacket)> bytes{};
        sockaddr_in source{};
        int sourceSize = sizeof(source);
        if (recvfrom(g_sock, bytes.data(), static_cast<int>(bytes.size()), 0,
                     reinterpret_cast<sockaddr *>(&source), &sourceSize) <= 0)
        {
            return;
        }
    }
}

static bool SendStatusAndWaitForAck(bool receiverActive)
{
    if (!g_targetConfigured || g_sock == INVALID_SOCKET)
        return false;

    DrainStatusResponses();
    if (++g_statusRequestId == 0)
        ++g_statusRequestId;
    const std::uint16_t requestId = g_statusRequestId;
    SendStatusPacket(receiverActive, 3, requestId);

    ULONGLONG deadline = GetTickCount64() + STATUS_ACK_TIMEOUT_MS;
    for (;;)
    {
        ULONGLONG now = GetTickCount64();
        if (now >= deadline)
            return false;

        DWORD remaining = static_cast<DWORD>(deadline - now);
        fd_set readSet;
        FD_ZERO(&readSet);
        FD_SET(g_sock, &readSet);
        timeval timeout{
            static_cast<long>(remaining / 1000),
            static_cast<long>((remaining % 1000) * 1000)};
        if (select(0, &readSet, nullptr, nullptr, &timeout) <= 0)
            return false;

        KeyboardAckPacket acknowledgement{};
        sockaddr_in source{};
        int sourceSize = sizeof(source);
        int received = recvfrom(
            g_sock, reinterpret_cast<char *>(&acknowledgement),
            sizeof(acknowledgement), 0,
            reinterpret_cast<sockaddr *>(&source), &sourceSize);
        if (received == sizeof(acknowledgement) &&
            source.sin_addr.s_addr == g_recvAddr.sin_addr.s_addr &&
            source.sin_port == g_recvAddr.sin_port &&
            acknowledgement.magic == KEYBOARD_ACK_MAGIC &&
            acknowledgement.version == KEYBOARD_PACKET_VERSION &&
            acknowledgement.receiverActive == (receiverActive ? 1 : 0) &&
            acknowledgement.requestId == requestId)
        {
            return true;
        }
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
    ReadLocalClipboard(clipCt, clipPayload);

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
            IsClipboardDataHeaderValid(resp))
        {
            clipData.resize(resp.data_length);
            if (resp.data_length == 0 ||
                TcpRecvAll(cs, clipData.data(), static_cast<int>(resp.data_length)))
            {
                if (IsClipboardPayloadValid(
                        resp.content_type, clipData.data(), clipData.size()))
                {
                    respType = resp.content_type;
                    gotClipboard = true;
                }
            }
        }
    }

    closesocket(cs);

    if (gotClipboard && !g_shuttingDown.load(std::memory_order_relaxed))
        WriteLocalClipboard(respType, clipData);
}

static void StartClipboardTransfer(bool pushToReceiver)
{
    if (g_shuttingDown.load(std::memory_order_relaxed))
        return;

    ClipboardTransferRequest request{pushToReceiver, g_recvAddr.sin_addr};
    {
        std::lock_guard<std::mutex> lock(g_clipboardTransferMutex);
        if (g_clipboardTransferActive)
        {
            g_pendingClipboardTransfer = request;
            g_clipboardTransferPending = true;
            return;
        }
        g_clipboardTransferActive = true;
    }

    if (g_clipboardThread.joinable())
        g_clipboardThread.join();

    try
    {
        g_clipboardThread = std::thread([request]() mutable {
            for (;;)
            {
                try
                {
                    if (request.pushToReceiver)
                        PushClipboardToReceiver(request.receiverAddress);
                    else
                        PullClipboardFromReceiver(request.receiverAddress);
                }
                catch (...)
                {
                }

                std::lock_guard<std::mutex> lock(g_clipboardTransferMutex);
                if (!g_shuttingDown.load(std::memory_order_relaxed) &&
                    g_clipboardTransferPending)
                {
                    request = g_pendingClipboardTransfer;
                    g_clipboardTransferPending = false;
                    continue;
                }

                g_clipboardTransferPending = false;
                g_clipboardTransferActive = false;
                return;
            }
        });
    }
    catch (...)
    {
        std::lock_guard<std::mutex> lock(g_clipboardTransferMutex);
        g_clipboardTransferPending = false;
        g_clipboardTransferActive = false;
    }
}

// ------------------------- hotkey parsing -----------------
static bool GetHotkeyKeyName(UINT key, std::wstring &name)
{
    if ((key >= '0' && key <= '9') || (key >= 'A' && key <= 'Z'))
    {
        name.assign(1, static_cast<wchar_t>(key));
        return true;
    }
    if (key >= VK_F1 && key <= VK_F24)
    {
        name = L"F" + std::to_wstring(key - VK_F1 + 1);
        return true;
    }
    return false;
}

static std::wstring FormatHotkey(const HotkeyConfig &config)
{
    std::wstring description;
    if (config.modifiers & MOD_CONTROL)
        description += L"CTRL+";
    if (config.modifiers & MOD_SHIFT)
        description += L"SHIFT+";
    if (config.modifiers & MOD_ALT)
        description += L"ALT+";
    if (config.modifiers & MOD_WIN)
        description += L"WIN+";

    std::wstring keyName;
    if (GetHotkeyKeyName(config.key, keyName))
        description += keyName;
    return description;
}

static bool ParseHotkey(const std::wstring &hotkeyText, HotkeyConfig &config)
{
    std::wstring text = hotkeyText;
    std::transform(text.begin(), text.end(), text.begin(), ::towupper);

    std::vector<std::wstring> parts;
    std::size_t start = 0;
    for (;;)
    {
        std::size_t plus = text.find(L'+', start);
        std::wstring part = text.substr(
            start, plus == std::wstring::npos ? plus : plus - start);
        std::size_t first = part.find_first_not_of(L" \t");
        std::size_t last = part.find_last_not_of(L" \t");
        if (first == std::wstring::npos)
            return false;
        parts.push_back(part.substr(first, last - first + 1));
        if (plus == std::wstring::npos)
            break;
        start = plus + 1;
    }
    if (parts.empty())
        return false;

    HotkeyConfig parsed{};
    parsed.modifiers = 0;
    parsed.key = 0;
    for (std::size_t i = 0; i + 1 < parts.size(); ++i)
    {
        if (parts[i] == L"CTRL" || parts[i] == L"CONTROL")
            parsed.modifiers |= MOD_CONTROL;
        else if (parts[i] == L"SHIFT")
            parsed.modifiers |= MOD_SHIFT;
        else if (parts[i] == L"ALT")
            parsed.modifiers |= MOD_ALT;
        else if (parts[i] == L"WIN")
            parsed.modifiers |= MOD_WIN;
        else
            return false;
    }

    const std::wstring &keyName = parts.back();
    if (keyName.size() == 1 &&
        ((keyName[0] >= L'0' && keyName[0] <= L'9') ||
         (keyName[0] >= L'A' && keyName[0] <= L'Z')))
    {
        parsed.key = static_cast<UINT>(keyName[0]);
    }
    else if (keyName.size() >= 2 && keyName[0] == L'F')
    {
        wchar_t *end = nullptr;
        long number = std::wcstol(keyName.c_str() + 1, &end, 10);
        if (!end || *end != L'\0' || number < 1 || number > 24)
            return false;
        parsed.key = VK_F1 + static_cast<UINT>(number - 1);
    }
    else
    {
        return false;
    }

    parsed.description = FormatHotkey(parsed);
    config = parsed;
    return true;
}

static bool IsModifierKey(UINT key)
{
    return key == VK_SHIFT || key == VK_LSHIFT || key == VK_RSHIFT ||
           key == VK_CONTROL || key == VK_LCONTROL || key == VK_RCONTROL ||
           key == VK_MENU || key == VK_LMENU || key == VK_RMENU ||
           key == VK_LWIN || key == VK_RWIN;
}

static UINT GetPressedHotkeyModifiers()
{
    UINT modifiers = 0;
    if (GetKeyState(VK_CONTROL) < 0)
        modifiers |= MOD_CONTROL;
    if (GetKeyState(VK_SHIFT) < 0)
        modifiers |= MOD_SHIFT;
    if (GetKeyState(VK_MENU) < 0)
        modifiers |= MOD_ALT;
    if (GetKeyState(VK_LWIN) < 0 || GetKeyState(VK_RWIN) < 0)
        modifiers |= MOD_WIN;
    return modifiers;
}

static bool RegisterConfiguredHotkey(const HotkeyConfig &config)
{
    if (!RegisterHotKey(g_msgWnd, HOTKEY_ID, config.modifiers, config.key))
        return false;
    g_hotkeyRegistered = true;
    return true;
}

static void UnregisterConfiguredHotkey()
{
    if (g_hotkeyRegistered)
    {
        UnregisterHotKey(g_msgWnd, HOTKEY_ID);
        g_hotkeyRegistered = false;
    }
}

static LRESULT CALLBACK HotkeyEditProc(
    HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message)
    {
    case WM_GETDLGCODE:
        return DLGC_WANTALLKEYS;

    case WM_SETFOCUS:
        UnregisterConfiguredHotkey();
        break;

    case WM_KILLFOCUS:
        if (!g_hotkeyRegistered && g_msgWnd)
            RegisterConfiguredHotkey(g_hotkey);
        break;

    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
    {
        UINT key = static_cast<UINT>(wParam);
        if (key == VK_ESCAPE)
        {
            g_pendingHotkey = g_hotkey;
            SetWindowTextW(window, g_pendingHotkey.description.c_str());
            return 0;
        }
        if (IsModifierKey(key))
            return 0;

        std::wstring keyName;
        UINT modifiers = GetPressedHotkeyModifiers();
        if (modifiers == 0 || !GetHotkeyKeyName(key, keyName))
        {
            MessageBeep(MB_ICONWARNING);
            return 0;
        }

        g_pendingHotkey.modifiers = modifiers;
        g_pendingHotkey.key = key;
        g_pendingHotkey.description = FormatHotkey(g_pendingHotkey);
        SetWindowTextW(window, g_pendingHotkey.description.c_str());
        return 0;
    }

    case WM_KEYUP:
    case WM_SYSKEYUP:
    case WM_CHAR:
    case WM_SYSCHAR:
        return 0;
    }

    return CallWindowProcW(
        g_originalHotkeyEditProc, window, message, wParam, lParam);
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

static void ShowReceiverConnectionFailureIfNeeded(bool reached)
{
    if (reached)
        return;

    ShowTrayNotification(
        g_trayIcon, L"Keyboard Sharing", L"No response from receiver",
        NIIF_WARNING);
}

static void ShowSettingsWindow()
{
    if (g_capturing.load(std::memory_order_relaxed))
        StopCapture();

    SetWindowTextW(g_targetEdit, g_targetText.c_str());
    g_pendingHotkey = g_hotkey;
    SetWindowTextW(g_hotkeyEdit, g_pendingHotkey.description.c_str());
    SendMessageW(g_clipboardCheck, BM_SETCHECK,
                 g_clipboardEnabled ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(g_startupCheck, BM_SETCHECK,
                 IsStartupEnabled(STARTUP_VALUE_NAME) ? BST_CHECKED : BST_UNCHECKED, 0);

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

static bool ApplyPendingHotkey()
{
    if (g_pendingHotkey.modifiers == g_hotkey.modifiers &&
        g_pendingHotkey.key == g_hotkey.key)
    {
        if (g_hotkeyRegistered || RegisterConfiguredHotkey(g_hotkey))
            return true;
    }
    else
    {
        HotkeyConfig previous = g_hotkey;
        UnregisterConfiguredHotkey();
        if (RegisterConfiguredHotkey(g_pendingHotkey))
        {
            g_hotkey = g_pendingHotkey;
            return true;
        }
        RegisterConfiguredHotkey(previous);
    }

    MessageBoxW(g_msgWnd, L"That keyboard shortcut is in use by another application.",
                L"Keyboard Sharing", MB_OK | MB_ICONWARNING);
    SetFocus(g_hotkeyEdit);
    return false;
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

    const sockaddr_in previousAddress = g_recvAddr;
    const std::string previousHost = g_targetHost;
    const std::wstring previousTargetText = g_targetText;
    const bool previousLiteral = g_targetIsLiteralIp;
    const bool previousTargetConfigured = g_targetConfigured;
    const bool previousClipboardEnabled = g_clipboardEnabled;
    const HotkeyConfig previousHotkey = g_hotkey;

    auto restoreRuntimeSettings = [&]() {
        g_recvAddr = previousAddress;
        g_targetHost = previousHost;
        g_targetText = previousTargetText;
        g_targetIsLiteralIp = previousLiteral;
        g_targetConfigured = previousTargetConfigured;
        g_clipboardEnabled = previousClipboardEnabled;

        if (g_hotkey.modifiers != previousHotkey.modifiers ||
            g_hotkey.key != previousHotkey.key)
        {
            HotkeyConfig currentHotkey = g_hotkey;
            UnregisterConfiguredHotkey();
            if (RegisterConfiguredHotkey(previousHotkey))
            {
                g_hotkey = previousHotkey;
            }
            else
            {
                RegisterConfiguredHotkey(currentHotkey);
                g_hotkey = currentHotkey;
            }
        }
    };

    if (!ConfigureTarget(target))
    {
        MessageBoxW(g_msgWnd, L"The receiver address could not be found.",
                    L"Keyboard Sharing", MB_OK | MB_ICONERROR);
        SetFocus(g_targetEdit);
        return false;
    }

    if (!ApplyPendingHotkey())
    {
        restoreRuntimeSettings();
        return false;
    }

    g_clipboardEnabled =
        SendMessageW(g_clipboardCheck, BM_GETCHECK, 0, 0) == BST_CHECKED;
    if (!SaveSettingsFile())
    {
        restoreRuntimeSettings();
        MessageBoxW(g_msgWnd, L"The settings file could not be saved beside sender.exe.",
                    L"Keyboard Sharing", MB_OK | MB_ICONERROR);
        return false;
    }

    bool startWithWindows =
        SendMessageW(g_startupCheck, BM_GETCHECK, 0, 0) == BST_CHECKED;
    if (!SetStartupEnabled(STARTUP_VALUE_NAME, startWithWindows))
    {
        MessageBoxW(
            g_msgWnd,
            L"The other settings were saved, but the Windows startup setting could not be changed.",
            L"Keyboard Sharing", MB_OK | MB_ICONERROR);
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

    bool receiverReached = SendStatusAndWaitForAck(true);
    SetTimer(g_msgWnd, STATUS_TIMER_ID, STATUS_TIMER_MS, nullptr);
    UpdateSenderTrayIcon();
    ShowReceiverConnectionFailureIfNeeded(receiverReached);

    if (g_clipboardEnabled && !g_shuttingDown.load(std::memory_order_relaxed))
        StartClipboardTransfer(true);

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

    bool receiverReached = SendStatusAndWaitForAck(false);
    UpdateSenderTrayIcon();
    ShowReceiverConnectionFailureIfNeeded(receiverReached);

    if (g_clipboardEnabled && !g_shuttingDown.load(std::memory_order_relaxed))
        StartClipboardTransfer(false);

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
        g_hotkeyLabel = CreateWindowExW(0, L"STATIC", L"Keyboard toggle:",
                                        WS_CHILD | WS_VISIBLE,
                                        0, 0, 1, 1, hwnd, nullptr, nullptr, nullptr);
        g_hotkeyEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", g_hotkey.description.c_str(),
                                        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_READONLY | ES_CENTER,
                                        0, 0, 1, 1, hwnd,
                                        reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_HOTKEY_EDIT)),
                                        nullptr, nullptr);
        g_startupCheck = CreateWindowExW(0, L"BUTTON", L"Start with Windows",
                                          WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                                          0, 0, 1, 1, hwnd,
                                          reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_STARTUP_CHECK)),
                                          nullptr, nullptr);
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
        SetWindowTheme(g_hotkeyEdit, L"DarkMode_CFD", nullptr);
        SetWindowTheme(g_clipboardCheck, L"DarkMode_Explorer", nullptr);
        SetWindowTheme(g_startupCheck, L"DarkMode_Explorer", nullptr);
        SetWindowTheme(g_saveButton, L"DarkMode_Explorer", nullptr);
        SetWindowTheme(g_cancelButton, L"DarkMode_Explorer", nullptr);

        SetLastError(ERROR_SUCCESS);
        g_originalHotkeyEditProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(
            g_hotkeyEdit, GWLP_WNDPROC,
            reinterpret_cast<LONG_PTR>(HotkeyEditProc)));
        if (!g_originalHotkeyEditProc && GetLastError() != ERROR_SUCCESS)
            return -1;

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
            g_pendingHotkey = g_hotkey;
            SetWindowTextW(g_hotkeyEdit, g_hotkey.description.c_str());
            if (!g_hotkeyRegistered)
                RegisterConfiguredHotkey(g_hotkey);
            ShowWindow(hwnd, SW_HIDE);
            return 0;
        }
        break;

    case WM_CLOSE:
        g_pendingHotkey = g_hotkey;
        SetWindowTextW(g_hotkeyEdit, g_hotkey.description.c_str());
        if (!g_hotkeyRegistered)
            RegisterConfiguredHotkey(g_hotkey);
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
    SingleInstanceGuard instanceGuard;
    if (!instanceGuard.Acquire(L"Local\\LeanKeyboardSharingSenderInstance"))
    {
        MessageBoxW(
            nullptr,
            instanceGuard.AlreadyRunning()
                ? L"Keyboard Sharing Sender is already running."
                : L"Keyboard Sharing Sender could not create its instance lock.",
            L"Keyboard Sharing", MB_OK | MB_ICONINFORMATION | MB_SETFOREGROUND);
        return instanceGuard.AlreadyRunning() ? 0 : 1;
    }

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
        ScaleForDpi(225, initialDpi)};
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
    if (!RegisterConfiguredHotkey(g_hotkey))
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
    UnregisterConfiguredHotkey();
    RemoveTrayIcon(g_trayIcon);
    if (g_msgWnd && IsWindow(g_msgWnd))
        DestroyWindow(g_msgWnd);
    if (g_clipboardThread.joinable())
        g_clipboardThread.join();
    if (g_sock != INVALID_SOCKET)
        closesocket(g_sock);
    WSACleanup();
    DestroyDarkThemeResources();
    return 0;
}
