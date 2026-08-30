#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iomanip>
#include <iostream>

#include "keyboard_protocol.h"

#pragma comment(lib, "ws2_32.lib")

static constexpr int PORT = 7777;

static const char *GetKeyName(std::uint16_t vkCode)
{
    static char buffer[32];

    if ((vkCode >= 'A' && vkCode <= 'Z') ||
        (vkCode >= '0' && vkCode <= '9'))
    {
        sprintf_s(buffer, "%c", static_cast<char>(vkCode));
        return buffer;
    }

    switch (vkCode)
    {
    case VK_SPACE: return "SPACE";
    case VK_RETURN: return "ENTER";
    case VK_BACK: return "BACKSPACE";
    case VK_TAB: return "TAB";
    case VK_ESCAPE: return "ESC";
    case VK_SHIFT: return "SHIFT";
    case VK_CONTROL: return "CTRL";
    case VK_MENU: return "ALT";
    default:
        sprintf_s(buffer, "VK_0x%02X", vkCode);
        return buffer;
    }
}

int main()
{
    std::cout << "Keyboard packet monitor on UDP port " << PORT << "\n";
    std::cout << "Press Ctrl+C to exit\n\n";

    WSADATA wsaData{};
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0)
        return 1;

    SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == INVALID_SOCKET)
    {
        WSACleanup();
        return 1;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(PORT);

    if (bind(sock, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == SOCKET_ERROR)
    {
        closesocket(sock);
        WSACleanup();
        return 1;
    }

    int packetCount = 0;
    while (true)
    {
        KeyboardPacket packet{};
        sockaddr_in senderAddr{};
        int senderAddrSize = sizeof(senderAddr);
        int received = recvfrom(sock, reinterpret_cast<char *>(&packet), sizeof(packet), 0,
                                reinterpret_cast<sockaddr *>(&senderAddr), &senderAddrSize);

        if (received != sizeof(packet) ||
            packet.magic != KEYBOARD_PACKET_MAGIC ||
            packet.version != KEYBOARD_PACKET_VERSION ||
            packet.vkCode == 0 || packet.down > 1)
            continue;

        char senderIP[INET_ADDRSTRLEN]{};
        inet_ntop(AF_INET, &senderAddr.sin_addr, senderIP, sizeof(senderIP));
        std::cout << "[#" << std::setw(4) << ++packetCount << "] From " << senderIP
                  << " - Key: " << GetKeyName(packet.vkCode)
                  << " (" << packet.vkCode << ") "
                  << (packet.down ? "DOWN" : "UP") << std::endl;
    }
}
