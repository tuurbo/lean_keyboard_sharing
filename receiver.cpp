#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <thread>
#include <atomic>
#include <vector>
#include <array>
#include <cstring>

#include "clipboard_protocol.h"
#include "keyboard_protocol.h"
#include "tray_icon.h"
#include "windows_app.h"

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "advapi32.lib")

// MinGW's winuser.h only defines this when _WIN32_WINNT >= 0x0600.
#ifndef MAPVK_VK_TO_VSC_EX
#define MAPVK_VK_TO_VSC_EX 4
#endif

constexpr int PORT = 7777;
static constexpr UINT TRAY_ICON_ID = 1;
static constexpr UINT STATUS_TIMER_ID = 1;
static constexpr UINT WM_APP_TRAY = WM_APP + 1;
static constexpr UINT WM_APP_RECEIVER_STATE = WM_APP + 2;
static constexpr UINT WM_APP_RECEIVER_ERROR = WM_APP + 3;
static constexpr UINT STATUS_TIMEOUT_MS = 3000;
static constexpr int IDM_START_WITH_WINDOWS = 2001;
static constexpr int IDM_EXIT = 2002;
static constexpr wchar_t STARTUP_VALUE_NAME[] = L"LeanKeyboardSharingReceiver";

// ------------------------- clipboard protocol -------------
static constexpr int CLIP_PORT = 7778;
static constexpr DWORD CLIP_IO_TIMEOUT_MS = 3000;
static constexpr long CLIP_IDLE_TIMEOUT_SEC = 5;

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

static std::atomic<bool> g_running{true};
static std::atomic<bool> g_receiverActive{false};
static std::atomic<ULONGLONG> g_lastActiveTick{0};
static std::atomic<SOCKET> g_inputSock{INVALID_SOCKET};
static std::atomic<SOCKET> g_clipListenSock{INVALID_SOCKET};
static std::atomic<SOCKET> g_clipClientSock{INVALID_SOCKET};
static HWND g_window = nullptr;
static TrayIconState g_trayIcon;
static UINT g_taskbarCreatedMessage = 0;

static void ClipboardThread()
{
    SOCKET listenSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listenSock == INVALID_SOCKET)
        return;

    // allow quick restart
    int optval = 1;
    setsockopt(listenSock, SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<char *>(&optval), sizeof(optval));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(CLIP_PORT);

    if (bind(listenSock, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == SOCKET_ERROR)
    {
        closesocket(listenSock);
        return;
    }

    if (listen(listenSock, SOMAXCONN) == SOCKET_ERROR)
    {
        closesocket(listenSock);
        return;
    }

    g_clipListenSock.store(listenSock, std::memory_order_relaxed);

    while (g_running)
    {
        // use select so we can check g_running periodically
        fd_set rset;
        FD_ZERO(&rset);
        FD_SET(listenSock, &rset);
        timeval tv{1, 0}; // 1s timeout

        int sel = select(0, &rset, nullptr, nullptr, &tv);
        if (sel <= 0)
            continue;

        SOCKET client = accept(listenSock, nullptr, nullptr);
        if (client == INVALID_SOCKET)
            continue;
        g_clipClientSock.store(client, std::memory_order_relaxed);

        setsockopt(client, SOL_SOCKET, SO_RCVTIMEO,
                   reinterpret_cast<const char *>(&CLIP_IO_TIMEOUT_MS), sizeof(CLIP_IO_TIMEOUT_MS));
        setsockopt(client, SOL_SOCKET, SO_SNDTIMEO,
                   reinterpret_cast<const char *>(&CLIP_IO_TIMEOUT_MS), sizeof(CLIP_IO_TIMEOUT_MS));

        // serve this connection until it closes (use select to avoid
        // blocking forever so we can check g_running periodically)
        while (g_running)
        {
            fd_set cset;
            FD_ZERO(&cset);
            FD_SET(client, &cset);
            timeval ctv{CLIP_IDLE_TIMEOUT_SEC, 0};

            int csel = select(0, &cset, nullptr, nullptr, &ctv);
            if (csel < 0) break;   // error
            if (csel == 0) break;   // idle/stale client: accept a fresh one

            ClipHeader hdr{};
            if (!TcpRecvAll(client, reinterpret_cast<char *>(&hdr), sizeof(hdr)))
                break;

            if (hdr.msg_type == CLIP_MSG_DATA)
            {
                if (!IsClipboardDataHeaderValid(hdr))
                    break;

                std::vector<char> clipData(hdr.data_length);
                if (hdr.data_length > 0 &&
                    !TcpRecvAll(client, clipData.data(), static_cast<int>(hdr.data_length)))
                    break;
                if (!IsClipboardPayloadValid(
                        hdr.content_type, clipData.data(), clipData.size()))
                    break;

                WriteLocalClipboard(hdr.content_type, clipData);
                break;
            }
            else if (hdr.msg_type == CLIP_MSG_REQUEST)
            {
                if (!IsClipboardRequestValid(hdr))
                    break;

                uint8_t ct;
                std::vector<char> clipData;
                ReadLocalClipboard(ct, clipData);

                ClipHeader resp{};
                resp.msg_type = CLIP_MSG_DATA;
                resp.content_type = ct;
                resp.data_length = (uint32_t)clipData.size();

                if (!TcpSendAll(client, reinterpret_cast<char *>(&resp), sizeof(resp)))
                    break;
                if (!clipData.empty() && !TcpSendAll(client, clipData.data(), (int)clipData.size()))
                    break;
                break;
            }
            else
            {
                break; // unknown message
            }
        }

        SOCKET clientToClose = g_clipClientSock.exchange(INVALID_SOCKET);
        if (clientToClose != INVALID_SOCKET)
            closesocket(clientToClose);
    }

    SOCKET listenToClose = g_clipListenSock.exchange(INVALID_SOCKET);
    if (listenToClose != INVALID_SOCKET)
        closesocket(listenToClose);
}

// Returns the scan code Windows reports for this virtual key's physical key,
// with 0xE0 in the high byte when Windows flags the key as extended.
static UINT ScanCodeFromVirtualKey(UINT vkCode)
{
    switch (vkCode)
    {
    // MapVirtualKey returns the numpad scan code for these keys and drops the
    // E0 prefix that the dedicated keys send.
    case VK_INSERT:
    case VK_DELETE:
    case VK_HOME:
    case VK_END:
    case VK_PRIOR:
    case VK_NEXT:
    case VK_LEFT:
    case VK_UP:
    case VK_RIGHT:
    case VK_DOWN:
        return 0xE000 | MapVirtualKeyW(vkCode, MAPVK_VK_TO_VSC);
    // In keyboard messages Windows reports NumLock as 0x45 with the extended
    // flag and Pause as 0x45 without it. MapVirtualKey returns 0x45 for
    // NumLock and E1 1D for Pause.
    case VK_NUMLOCK:
        return 0xE045;
    case VK_PAUSE:
        return 0x45;
    // MapVirtualKey returns the SysRq scan code (0x54) for PrintScreen.
    case VK_SNAPSHOT:
        return 0xE037;
    default:
        return MapVirtualKeyW(vkCode, MAPVK_VK_TO_VSC_EX);
    }
}

static bool ProcessPacket(const KeyboardPacket &packet)
{
    if (packet.magic != KEYBOARD_PACKET_MAGIC ||
        packet.version != KEYBOARD_PACKET_VERSION ||
        packet.vkCode == 0 || packet.down > 1)
        return false;

    const UINT scanCode = ScanCodeFromVirtualKey(packet.vkCode);

    INPUT input{};
    input.type = INPUT_KEYBOARD;
    input.ki.wVk = packet.vkCode;
    input.ki.wScan = static_cast<WORD>(scanCode & 0xFF);
    input.ki.dwFlags = packet.down ? 0 : KEYEVENTF_KEYUP;
    if ((scanCode & 0xFF00) == 0xE000)
        input.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
    SendInput(1, &input, sizeof(input));
    return true;
}

static void SetReceiverActive(bool active)
{
    if (active)
        g_lastActiveTick.store(GetTickCount64(), std::memory_order_relaxed);

    bool changed = g_receiverActive.exchange(active, std::memory_order_relaxed) != active;
    if (changed && g_window)
        PostMessageW(g_window, WM_APP_RECEIVER_STATE, active ? 1 : 0, 0);
}

static void SendStatusAcknowledgement(
    SOCKET socket, const sockaddr_in &senderAddress, int senderAddressSize,
    const KeyboardStatusPacket &status)
{
    if (status.requestId == 0)
        return;

    KeyboardAckPacket acknowledgement{};
    acknowledgement.receiverActive = status.receiverActive;
    acknowledgement.requestId = status.requestId;
    sendto(socket, reinterpret_cast<const char *>(&acknowledgement),
           sizeof(acknowledgement), 0,
           reinterpret_cast<const sockaddr *>(&senderAddress), senderAddressSize);
}

static void ReceiverThread()
{
    SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == INVALID_SOCKET)
    {
        PostMessageW(g_window, WM_APP_RECEIVER_ERROR, 0, 0);
        return;
    }

    DWORD receiveTimeout = 500;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO,
               reinterpret_cast<const char *>(&receiveTimeout), sizeof(receiveTimeout));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(PORT);

    if (bind(sock, (sockaddr *)&addr, sizeof(addr)) == SOCKET_ERROR)
    {
        closesocket(sock);
        PostMessageW(g_window, WM_APP_RECEIVER_ERROR, 0, 0);
        return;
    }

    g_inputSock.store(sock, std::memory_order_relaxed);

    std::array<char, sizeof(KeyboardPacket)> packetBytes{};
    sockaddr_in senderAddr{};
    int senderAddrSize = sizeof(senderAddr);

    bool receiverFailed = false;
    while (g_running)
    {
        senderAddrSize = sizeof(senderAddr);
        int received = recvfrom(sock, packetBytes.data(), static_cast<int>(packetBytes.size()), 0,
                                (sockaddr *)&senderAddr, &senderAddrSize);

        if (received == sizeof(KeyboardPacket))
        {
            std::uint32_t magic = 0;
            std::memcpy(&magic, packetBytes.data(), sizeof(magic));
            if (magic == KEYBOARD_PACKET_MAGIC)
            {
                KeyboardPacket packet{};
                std::memcpy(&packet, packetBytes.data(), sizeof(packet));
                if (ProcessPacket(packet))
                    SetReceiverActive(true);
            }
            else if (magic == KEYBOARD_STATUS_MAGIC)
            {
                KeyboardStatusPacket status{};
                std::memcpy(&status, packetBytes.data(), sizeof(status));
                if (status.version == KEYBOARD_PACKET_VERSION && status.receiverActive <= 1)
                {
                    SetReceiverActive(status.receiverActive != 0);
                    SendStatusAcknowledgement(
                        sock, senderAddr, senderAddrSize, status);
                }
            }
        }
        else if (received == SOCKET_ERROR)
        {
            int error = WSAGetLastError();
            if (!g_running.load(std::memory_order_relaxed))
                break;
            if (error == WSAETIMEDOUT || error == WSAEWOULDBLOCK ||
                error == WSAEINTR || error == WSAEMSGSIZE)
                continue;
            receiverFailed = true;
            break;
        }
    }

    SOCKET socketToClose = g_inputSock.exchange(INVALID_SOCKET);
    if (socketToClose != INVALID_SOCKET)
        closesocket(socketToClose);
    if (receiverFailed && g_running.load(std::memory_order_relaxed))
        PostMessageW(g_window, WM_APP_RECEIVER_ERROR, 0, 0);
}

// ------------------------- tray ---------------------------
static void UpdateReceiverTrayIcon()
{
    bool active = g_receiverActive.load(std::memory_order_relaxed);
    const wchar_t *tooltip = active
        ? L"Keyboard: this PC is in use"
        : L"Keyboard: sender is in use";
    SetTrayIcon(g_trayIcon, g_window, TRAY_ICON_ID, WM_APP_TRAY, active, tooltip);
}

static void CloseSharedSocket(std::atomic<SOCKET> &sharedSocket)
{
    SOCKET socketToClose = sharedSocket.exchange(INVALID_SOCKET);
    if (socketToClose != INVALID_SOCKET)
        closesocket(socketToClose);
}

static void RequestReceiverExit()
{
    g_running.store(false, std::memory_order_relaxed);
    CloseSharedSocket(g_inputSock);
    CloseSharedSocket(g_clipClientSock);
    CloseSharedSocket(g_clipListenSock);
    if (g_window)
        DestroyWindow(g_window);
}

static void ShowReceiverTrayMenu()
{
    HMENU menu = CreatePopupMenu();
    if (!menu)
        return;

    bool active = g_receiverActive.load(std::memory_order_relaxed);
    AppendMenuW(menu, MF_STRING | MF_GRAYED, 0,
                active ? L"This PC has the keyboard" : L"Sender has the keyboard");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    bool startWithWindows = IsStartupEnabled(STARTUP_VALUE_NAME);
    AppendMenuW(menu, MF_STRING | (startWithWindows ? MF_CHECKED : MF_UNCHECKED),
                IDM_START_WITH_WINDOWS, L"Start with Windows");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, IDM_EXIT, L"Exit");

    POINT cursor{};
    GetCursorPos(&cursor);
    SetForegroundWindow(g_window);
    int command = TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY,
                                 cursor.x, cursor.y, 0, g_window, nullptr);
    DestroyMenu(menu);

    if (command == IDM_START_WITH_WINDOWS)
    {
        if (!SetStartupEnabled(STARTUP_VALUE_NAME, !startWithWindows))
        {
            MessageBoxW(nullptr, L"The Windows startup setting could not be changed.",
                        L"Keyboard Sharing", MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
        }
    }
    else if (command == IDM_EXIT)
        RequestReceiverExit();
}

static LRESULT CALLBACK ReceiverWndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (g_taskbarCreatedMessage != 0 && message == g_taskbarCreatedMessage)
    {
        g_trayIcon.added = false;
        UpdateReceiverTrayIcon();
        return 0;
    }

    switch (message)
    {
    case WM_APP_TRAY:
        if (lParam == WM_RBUTTONUP || lParam == WM_CONTEXTMENU)
            ShowReceiverTrayMenu();
        return 0;

    case WM_APP_RECEIVER_STATE:
        UpdateReceiverTrayIcon();
        return 0;

    case WM_APP_RECEIVER_ERROR:
        MessageBoxW(nullptr,
                    L"The receiver could not listen for keyboard input on UDP port 7777. "
                    L"Another receiver may already be running.",
                    L"Keyboard Sharing", MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
        RequestReceiverExit();
        return 0;

    case WM_TIMER:
        if (wParam == STATUS_TIMER_ID &&
            g_receiverActive.load(std::memory_order_relaxed) &&
            GetTickCount64() - g_lastActiveTick.load(std::memory_order_relaxed) > STATUS_TIMEOUT_MS)
        {
            SetReceiverActive(false);
        }
        return 0;

    case WM_CLOSE:
        RequestReceiverExit();
        return 0;

    case WM_DESTROY:
        KillTimer(hwnd, STATUS_TIMER_ID);
        RemoveTrayIcon(g_trayIcon);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

int WINAPI WinMain(HINSTANCE instance, HINSTANCE, LPSTR, int)
{
    SingleInstanceGuard instanceGuard;
    if (!instanceGuard.Acquire(L"Local\\LeanKeyboardSharingReceiverInstance"))
    {
        MessageBoxW(
            nullptr,
            instanceGuard.AlreadyRunning()
                ? L"Keyboard Sharing Receiver is already running."
                : L"Keyboard Sharing Receiver could not create its instance lock.",
            L"Keyboard Sharing", MB_OK | MB_ICONINFORMATION | MB_SETFOREGROUND);
        return instanceGuard.AlreadyRunning() ? 0 : 1;
    }

    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0)
        return 1;

    WNDCLASSEXW windowClass{sizeof(WNDCLASSEXW)};
    windowClass.lpfnWndProc = ReceiverWndProc;
    windowClass.hInstance = instance;
    windowClass.lpszClassName = L"KeyboardSharingReceiverWnd";
    if (!RegisterClassExW(&windowClass))
    {
        WSACleanup();
        return 1;
    }

    g_window = CreateWindowExW(0, windowClass.lpszClassName, L"", 0,
                               0, 0, 0, 0, HWND_MESSAGE, nullptr, instance, nullptr);
    if (!g_window)
    {
        WSACleanup();
        return 1;
    }

    g_taskbarCreatedMessage = RegisterWindowMessageW(L"TaskbarCreated");

    UpdateReceiverTrayIcon();
    SetTimer(g_window, STATUS_TIMER_ID, 1000, nullptr);

    std::thread clipThread(ClipboardThread);
    std::thread receiverThread(ReceiverThread);

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0))
    {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    g_running.store(false, std::memory_order_relaxed);
    CloseSharedSocket(g_inputSock);
    CloseSharedSocket(g_clipClientSock);
    CloseSharedSocket(g_clipListenSock);

    if (receiverThread.joinable())
        receiverThread.join();
    if (clipThread.joinable())
        clipThread.join();

    WSACleanup();
    return 0;
}
