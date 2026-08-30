#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <thread>
#include <atomic>
#include <vector>
#include <array>
#include <cstring>

#include "keyboard_protocol.h"
#include "tray_icon.h"

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "gdi32.lib")

constexpr int PORT = 7777;
static constexpr UINT TRAY_ICON_ID = 1;
static constexpr UINT STATUS_TIMER_ID = 1;
static constexpr UINT WM_APP_TRAY = WM_APP + 1;
static constexpr UINT WM_APP_RECEIVER_STATE = WM_APP + 2;
static constexpr UINT WM_APP_RECEIVER_ERROR = WM_APP + 3;
static constexpr UINT STATUS_TIMEOUT_MS = 3000;
static constexpr int IDM_EXIT = 2001;

// ------------------------- clipboard protocol -------------
static constexpr int CLIP_PORT = 7778;
static constexpr uint32_t CLIP_MAX_SIZE = 1024 * 1024 * 1024; // 1 GB
static constexpr DWORD CLIP_IO_TIMEOUT_MS = 3000;
static constexpr long CLIP_IDLE_TIMEOUT_SEC = 5;

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

static void ReadClipboard(uint8_t &content_type, std::vector<char> &data)
{
    content_type = CLIP_CONTENT_EMPTY;
    data.clear();

    // Retry a few times — the clipboard may be briefly locked by the
    // screenshot tool or another process that just wrote to it.
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
                // sender is pushing its clipboard to us
                if (hdr.data_length > CLIP_MAX_SIZE)
                    break;

                std::vector<char> clipData(hdr.data_length);
                if (hdr.data_length > 0 && !TcpRecvAll(client, clipData.data(), (int)hdr.data_length))
                    break;

                WriteClipboard(hdr.content_type, clipData);
                break;
            }
            else if (hdr.msg_type == CLIP_MSG_REQUEST)
            {
                // sender wants our clipboard
                uint8_t ct;
                std::vector<char> clipData;
                ReadClipboard(ct, clipData);

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

static bool ProcessPacket(const KeyboardPacket &packet)
{
    if (packet.magic != KEYBOARD_PACKET_MAGIC ||
        packet.version != KEYBOARD_PACKET_VERSION ||
        packet.vkCode == 0 || packet.down > 1)
        return false;

    INPUT input{};
    input.type = INPUT_KEYBOARD;
    input.ki.wVk = packet.vkCode;
    input.ki.dwFlags = packet.down ? 0 : KEYEVENTF_KEYUP;
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
                    SetReceiverActive(status.receiverActive != 0);
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
    AppendMenuW(menu, MF_STRING, IDM_EXIT, L"Exit");

    POINT cursor{};
    GetCursorPos(&cursor);
    SetForegroundWindow(g_window);
    int command = TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY,
                                 cursor.x, cursor.y, 0, g_window, nullptr);
    DestroyMenu(menu);

    if (command == IDM_EXIT)
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
