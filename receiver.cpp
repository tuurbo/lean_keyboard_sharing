#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <thread>
#include <atomic>
#include <vector>

#include "keyboard_protocol.h"

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "user32.lib")

constexpr int PORT = 7777;

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

std::atomic<bool> g_running(true);
static SOCKET g_clipListenSock = INVALID_SOCKET;

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

    g_clipListenSock = listenSock;

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

        closesocket(client);
    }

    closesocket(listenSock);
    g_clipListenSock = INVALID_SOCKET;
}

static void ProcessPacket(const KeyboardPacket &packet)
{
    if (packet.magic != KEYBOARD_PACKET_MAGIC ||
        packet.version != KEYBOARD_PACKET_VERSION ||
        packet.vkCode == 0 || packet.down > 1)
        return;

    INPUT input{};
    input.type = INPUT_KEYBOARD;
    input.ki.wVk = packet.vkCode;
    input.ki.dwFlags = packet.down ? 0 : KEYEVENTF_KEYUP;
    SendInput(1, &input, sizeof(input));
}

void ReceiverThread()
{
    SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == INVALID_SOCKET)
        return;

    sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(PORT);

    if (bind(sock, (sockaddr *)&addr, sizeof(addr)) == SOCKET_ERROR)
    { closesocket(sock); WSACleanup(); return; }

    KeyboardPacket packet{};
    sockaddr_in senderAddr{};
    int senderAddrSize = sizeof(senderAddr);

    while (g_running)
    {
        int received = recvfrom(sock, (char *)&packet, sizeof(packet), 0,
                                (sockaddr *)&senderAddr, &senderAddrSize);

        if (received == sizeof(packet))
        {
            ProcessPacket(packet);
        }
        else if (received == SOCKET_ERROR)
        {
            int error = WSAGetLastError();
            if (error != WSAEWOULDBLOCK && error != WSAEINTR)
                break;
        }
    }

    closesocket(sock);
}

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int)
{
    // Initialize Winsock before any threads that need sockets
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0)
        return 1;

    // start clipboard sync thread
    std::thread clipThread(ClipboardThread);
    clipThread.detach();

    std::thread receiverThread(ReceiverThread);
    receiverThread.join();

    // cleanup
    g_running = false;
    if (g_clipListenSock != INVALID_SOCKET)
        closesocket(g_clipListenSock);

    return 0;
}
