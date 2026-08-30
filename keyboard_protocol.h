#pragma once

#include <cstdint>

static constexpr std::uint32_t KEYBOARD_PACKET_MAGIC = 0x3153424B; // "KBS1"
static constexpr std::uint32_t KEYBOARD_STATUS_MAGIC = 0x3154534B; // "KST1"
static constexpr std::uint32_t KEYBOARD_ACK_MAGIC = 0x3141534B; // "KSA1"
static constexpr std::uint8_t KEYBOARD_PACKET_VERSION = 1;

#pragma pack(push, 1)
struct KeyboardPacket
{
    std::uint32_t magic = KEYBOARD_PACKET_MAGIC;
    std::uint8_t version = KEYBOARD_PACKET_VERSION;
    std::uint16_t vkCode = 0;
    std::uint8_t down = 0;
};
#pragma pack(pop)

static_assert(sizeof(KeyboardPacket) == 8, "Unexpected keyboard packet size");

#pragma pack(push, 1)
struct KeyboardStatusPacket
{
    std::uint32_t magic = KEYBOARD_STATUS_MAGIC;
    std::uint8_t version = KEYBOARD_PACKET_VERSION;
    std::uint8_t receiverActive = 0;
    std::uint16_t requestId = 0;
};
#pragma pack(pop)

static_assert(sizeof(KeyboardStatusPacket) == sizeof(KeyboardPacket),
              "Keyboard packet sizes must match");

#pragma pack(push, 1)
struct KeyboardAckPacket
{
    std::uint32_t magic = KEYBOARD_ACK_MAGIC;
    std::uint8_t version = KEYBOARD_PACKET_VERSION;
    std::uint8_t receiverActive = 0;
    std::uint16_t requestId = 0;
};
#pragma pack(pop)

static_assert(sizeof(KeyboardAckPacket) == sizeof(KeyboardPacket),
              "Keyboard packet sizes must match");
