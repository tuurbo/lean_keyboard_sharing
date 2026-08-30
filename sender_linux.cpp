// Linux sender for the keyboard-only switcher.
// Captures keyboard input through evdev and sends it to the receiver over UDP.
// Compile: g++ -O3 -o sender_linux sender_linux.cpp -lpthread

#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <algorithm>
#include <atomic>
#include <thread>
#include <chrono>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <linux/input.h>

#include "keyboard_protocol.h"

// ------------------------- config -------------------------
static constexpr int PORT = 7777;

// Linux evdev keycode to Windows virtual key mapping
static uint16_t LinuxToWindowsVK(int linuxCode)
{
    // Most common keys - based on linux/input-event-codes.h to Windows VK codes
    switch (linuxCode)
    {
    // Letters (KEY_A = 30, VK_A = 0x41)
    case KEY_A: return 0x41;
    case KEY_B: return 0x42;
    case KEY_C: return 0x43;
    case KEY_D: return 0x44;
    case KEY_E: return 0x45;
    case KEY_F: return 0x46;
    case KEY_G: return 0x47;
    case KEY_H: return 0x48;
    case KEY_I: return 0x49;
    case KEY_J: return 0x4A;
    case KEY_K: return 0x4B;
    case KEY_L: return 0x4C;
    case KEY_M: return 0x4D;
    case KEY_N: return 0x4E;
    case KEY_O: return 0x4F;
    case KEY_P: return 0x50;
    case KEY_Q: return 0x51;
    case KEY_R: return 0x52;
    case KEY_S: return 0x53;
    case KEY_T: return 0x54;
    case KEY_U: return 0x55;
    case KEY_V: return 0x56;
    case KEY_W: return 0x57;
    case KEY_X: return 0x58;
    case KEY_Y: return 0x59;
    case KEY_Z: return 0x5A;

    // Numbers (top row)
    case KEY_0: return 0x30;
    case KEY_1: return 0x31;
    case KEY_2: return 0x32;
    case KEY_3: return 0x33;
    case KEY_4: return 0x34;
    case KEY_5: return 0x35;
    case KEY_6: return 0x36;
    case KEY_7: return 0x37;
    case KEY_8: return 0x38;
    case KEY_9: return 0x39;

    // Function keys
    case KEY_F1:  return 0x70;
    case KEY_F2:  return 0x71;
    case KEY_F3:  return 0x72;
    case KEY_F4:  return 0x73;
    case KEY_F5:  return 0x74;
    case KEY_F6:  return 0x75;
    case KEY_F7:  return 0x76;
    case KEY_F8:  return 0x77;
    case KEY_F9:  return 0x78;
    case KEY_F10: return 0x79;
    case KEY_F11: return 0x7A;
    case KEY_F12: return 0x7B;

    // Modifiers
    case KEY_LEFTSHIFT:  return 0xA0; // VK_LSHIFT
    case KEY_RIGHTSHIFT: return 0xA1; // VK_RSHIFT
    case KEY_LEFTCTRL:   return 0xA2; // VK_LCONTROL
    case KEY_RIGHTCTRL:  return 0xA3; // VK_RCONTROL
    case KEY_LEFTALT:    return 0xA4; // VK_LMENU
    case KEY_RIGHTALT:   return 0xA5; // VK_RMENU
    case KEY_LEFTMETA:   return 0x5B; // VK_LWIN
    case KEY_RIGHTMETA:  return 0x5C; // VK_RWIN

    // Special keys
    case KEY_ESC:        return 0x1B; // VK_ESCAPE
    case KEY_TAB:        return 0x09; // VK_TAB
    case KEY_CAPSLOCK:   return 0x14; // VK_CAPITAL
    case KEY_ENTER:      return 0x0D; // VK_RETURN
    case KEY_BACKSPACE:  return 0x08; // VK_BACK
    case KEY_SPACE:      return 0x20; // VK_SPACE
    case KEY_INSERT:     return 0x2D; // VK_INSERT
    case KEY_DELETE:     return 0x2E; // VK_DELETE
    case KEY_HOME:       return 0x24; // VK_HOME
    case KEY_END:        return 0x23; // VK_END
    case KEY_PAGEUP:     return 0x21; // VK_PRIOR
    case KEY_PAGEDOWN:   return 0x22; // VK_NEXT
    case KEY_UP:         return 0x26; // VK_UP
    case KEY_DOWN:       return 0x28; // VK_DOWN
    case KEY_LEFT:       return 0x25; // VK_LEFT
    case KEY_RIGHT:      return 0x27; // VK_RIGHT

    // Punctuation and symbols
    case KEY_MINUS:        return 0xBD; // VK_OEM_MINUS
    case KEY_EQUAL:        return 0xBB; // VK_OEM_PLUS
    case KEY_LEFTBRACE:    return 0xDB; // VK_OEM_4 [
    case KEY_RIGHTBRACE:   return 0xDD; // VK_OEM_6 ]
    case KEY_BACKSLASH:    return 0xDC; // VK_OEM_5
    case KEY_SEMICOLON:    return 0xBA; // VK_OEM_1 ;
    case KEY_APOSTROPHE:   return 0xDE; // VK_OEM_7 '
    case KEY_GRAVE:        return 0xC0; // VK_OEM_3 `
    case KEY_COMMA:        return 0xBC; // VK_OEM_COMMA
    case KEY_DOT:          return 0xBE; // VK_OEM_PERIOD
    case KEY_SLASH:        return 0xBF; // VK_OEM_2 /

    // Numpad
    case KEY_KP0: return 0x60; // VK_NUMPAD0
    case KEY_KP1: return 0x61;
    case KEY_KP2: return 0x62;
    case KEY_KP3: return 0x63;
    case KEY_KP4: return 0x64;
    case KEY_KP5: return 0x65;
    case KEY_KP6: return 0x66;
    case KEY_KP7: return 0x67;
    case KEY_KP8: return 0x68;
    case KEY_KP9: return 0x69;
    case KEY_KPMINUS:    return 0x6D; // VK_SUBTRACT
    case KEY_KPPLUS:     return 0x6B; // VK_ADD
    case KEY_KPASTERISK: return 0x6A; // VK_MULTIPLY
    case KEY_KPSLASH:    return 0x6F; // VK_DIVIDE
    case KEY_KPDOT:      return 0x6E; // VK_DECIMAL
    case KEY_KPENTER:    return 0x0D; // VK_RETURN
    case KEY_NUMLOCK:    return 0x90; // VK_NUMLOCK

    // Other
    case KEY_SCROLLLOCK: return 0x91; // VK_SCROLL
    case KEY_PAUSE:      return 0x13; // VK_PAUSE
    case KEY_SYSRQ:      return 0x2C; // VK_SNAPSHOT (PrintScreen)
    case KEY_MENU:       return 0x5D; // VK_APPS (context menu)

    default:
        // Return the linux code + 0x100 as fallback (won't match Windows, but preserves info)
        return (uint16_t)(linuxCode & 0xFF);
    }
}

// ------------------------- hotkey configuration -----------
struct HotkeyConfig {
    bool ctrl = false;
    bool shift = false;
    bool alt = false;
    bool meta = false;  // Windows/Super key
    int keycode = KEY_1;  // Linux evdev keycode
    std::string description = "ALT+1";
};

static HotkeyConfig g_hotkey;

// ------------------------- globals ------------------------
static std::atomic<bool> g_capturing{false};
static std::atomic<bool> g_running{true};
static std::atomic<bool> is_shift_pressed{false};
static std::atomic<bool> is_ctrl_pressed{false};
static std::atomic<bool> is_alt_pressed{false};
static std::atomic<bool> is_meta_pressed{false};

static int g_sock = -1;
static sockaddr_in g_recvAddr{};

// evdev device file descriptors
static std::vector<int> g_keyboardFds;

// ------------------------- signal handler -----------------
static void SignalHandler(int)
{
    g_running = false;
}

// ------------------------- net ----------------------------
static inline void SendPacket(const KeyboardPacket &p)
{
    sendto(g_sock, reinterpret_cast<const char *>(&p), sizeof(p), 0,
           reinterpret_cast<const sockaddr *>(&g_recvAddr), sizeof(g_recvAddr));
}

// ------------------------- hotkey parsing -----------------
static int ParseKeyName(const std::string& keyName)
{
    std::string key = keyName;
    std::transform(key.begin(), key.end(), key.begin(), ::toupper);

    // Single character keys
    if (key.length() == 1) {
        char c = key[0];
        if (c >= 'A' && c <= 'Z') {
            return KEY_A + (c - 'A');
        }
        if (c >= '0' && c <= '9') {
            if (c == '0') return KEY_0;
            return KEY_1 + (c - '1');
        }
    }

    // Function keys
    if (key[0] == 'F' && key.length() >= 2) {
        int fnum = std::stoi(key.substr(1));
        if (fnum >= 1 && fnum <= 12) {
            return KEY_F1 + (fnum - 1);
        }
    }

    // Special keys
    if (key == "ESC" || key == "ESCAPE") return KEY_ESC;
    if (key == "TAB") return KEY_TAB;
    if (key == "SPACE") return KEY_SPACE;
    if (key == "ENTER" || key == "RETURN") return KEY_ENTER;
    if (key == "BACKSPACE") return KEY_BACKSPACE;

    return -1;
}

static bool ParseHotkey(const std::string& hotkeyStr, HotkeyConfig& config)
{
    std::string str = hotkeyStr;
    std::transform(str.begin(), str.end(), str.begin(), ::toupper);

    config.ctrl = (str.find("CTRL") != std::string::npos || str.find("CONTROL") != std::string::npos);
    config.shift = (str.find("SHIFT") != std::string::npos);
    config.alt = (str.find("ALT") != std::string::npos);
    config.meta = (str.find("WIN") != std::string::npos || str.find("SUPER") != std::string::npos || str.find("META") != std::string::npos);

    // Find the key (last part after +)
    size_t lastPlus = str.rfind('+');
    if (lastPlus != std::string::npos && lastPlus < str.length() - 1) {
        std::string keyStr = str.substr(lastPlus + 1);
        // Trim whitespace
        keyStr.erase(0, keyStr.find_first_not_of(" \t"));
        keyStr.erase(keyStr.find_last_not_of(" \t") + 1);

        int keycode = ParseKeyName(keyStr);
        if (keycode >= 0) {
            config.keycode = keycode;
            config.description = hotkeyStr;
            return true;
        }
    }

    return false;
}

static void ReadConfigFile(HotkeyConfig& config)
{
    std::ifstream file("sender.cfg");
    if (!file.is_open()) return;

    std::string line;
    while (std::getline(file, line)) {
        if (line.empty() || line[0] == '#' || line[0] == ';')
            continue;

        size_t eqPos = line.find('=');
        if (eqPos != std::string::npos) {
            std::string key = line.substr(0, eqPos);
            std::string value = line.substr(eqPos + 1);

            // Trim
            key.erase(0, key.find_first_not_of(" \t"));
            key.erase(key.find_last_not_of(" \t") + 1);
            value.erase(0, value.find_first_not_of(" \t"));
            value.erase(value.find_last_not_of(" \t") + 1);

            std::transform(key.begin(), key.end(), key.begin(), ::toupper);

            if (key == "HOTKEY") {
                if (ParseHotkey(value, config)) {
                    std::cout << "Config file: Using hotkey " << config.description << "\n";
                }
                break;
            }
        }
    }
}

// ------------------------- hotkey check -------------------
static bool IsHotkeyPressed(int keycode)
{
    if (keycode != g_hotkey.keycode) return false;

    if (g_hotkey.ctrl && !is_ctrl_pressed.load()) return false;
    if (g_hotkey.shift && !is_shift_pressed.load()) return false;
    if (g_hotkey.alt && !is_alt_pressed.load()) return false;
    if (g_hotkey.meta && !is_meta_pressed.load()) return false;

    return true;
}

// Exclusively grab only the keyboard devices. Mouse devices are never opened.
static void GrabEvdevDevices(bool grab)
{
    for (int fd : g_keyboardFds) {
        ioctl(fd, EVIOCGRAB, grab ? 1 : 0);
    }
}

// ------------------------- capture on/off -----------------
static void StartCapture()
{
    if (g_capturing.exchange(true)) return;

    // Grab evdev devices exclusively to block local input
    GrabEvdevDevices(true);

    std::cout << "capture: ON (local keyboard is blocked; mouse remains local)\n";
}

static void StopCapture()
{
    if (!g_capturing.exchange(false)) return;

    // Send key-up events for modifiers to the receiver
    if (is_ctrl_pressed.load()) {
        KeyboardPacket p{};
        p.vkCode = 0x11; // VK_CONTROL
        p.down = 0;
        SendPacket(p);
        is_ctrl_pressed = false;
    }
    if (is_shift_pressed.load()) {
        KeyboardPacket p{};
        p.vkCode = 0x10; // VK_SHIFT
        p.down = 0;
        SendPacket(p);
        is_shift_pressed = false;
    }
    if (is_alt_pressed.load()) {
        KeyboardPacket p{};
        p.vkCode = 0x12; // VK_MENU (ALT)
        p.down = 0;
        SendPacket(p);
        is_alt_pressed = false;
    }

    // Release evdev exclusive grab
    GrabEvdevDevices(false);

    std::cout << "capture: OFF (local keyboard restored)\n";
}

// ------------------------- device discovery ---------------
static bool IsKeyboardDevice(const std::string& path)
{
    int fd = open(path.c_str(), O_RDONLY | O_NONBLOCK);
    if (fd < 0) return false;

    unsigned long evbit = 0;
    if (ioctl(fd, EVIOCGBIT(0, sizeof(evbit)), &evbit) < 0) {
        close(fd);
        return false;
    }

    bool result = (evbit & (1 << EV_KEY)) != 0;

    if (result) {
        unsigned char keybit[KEY_CNT / 8 + 1] = {0};
        if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keybit)), keybit) >= 0) {
            result = (keybit[KEY_A / 8] & (1 << (KEY_A % 8))) != 0;
        }
    }

    close(fd);
    return result;
}

static std::vector<std::string> FindKeyboardDevices()
{
    std::vector<std::string> devices;
    DIR* dir = opendir("/dev/input");
    if (!dir) return devices;

    dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        std::string name = entry->d_name;
        if (name.find("event") == 0) {
            std::string path = "/dev/input/" + name;
            if (IsKeyboardDevice(path)) {
                devices.push_back(path);
            }
        }
    }
    closedir(dir);
    return devices;
}

// ------------------------- input processing ---------------
static void ProcessKeyboardEvent(const input_event& ev, bool& hotkey_triggered)
{
    if (ev.type != EV_KEY) return;

    int code = ev.code;
    bool down = (ev.value == 1);  // 1 = press, 0 = release, 2 = repeat
    bool is_press_or_release = (ev.value == 0 || ev.value == 1);

    // Track modifier state
    if (code == KEY_LEFTSHIFT || code == KEY_RIGHTSHIFT) {
        if (is_press_or_release) is_shift_pressed = down;
    } else if (code == KEY_LEFTCTRL || code == KEY_RIGHTCTRL) {
        if (is_press_or_release) is_ctrl_pressed = down;
    } else if (code == KEY_LEFTALT || code == KEY_RIGHTALT) {
        if (is_press_or_release) is_alt_pressed = down;
    } else if (code == KEY_LEFTMETA || code == KEY_RIGHTMETA) {
        if (is_press_or_release) is_meta_pressed = down;
    }

    // Check hotkey (only on key down, not repeat)
    if (ev.value == 1 && IsHotkeyPressed(code)) {
        hotkey_triggered = true;
        return;
    }

    // Only forward if capturing and it's a press or release (not repeat)
    if (!g_capturing.load() || !is_press_or_release) return;

    KeyboardPacket p{};
    p.vkCode = LinuxToWindowsVK(code);
    p.down = down ? 1 : 0;
    SendPacket(p);
}

// ------------------------- main ---------------------------
int main(int argc, char* argv[])
{
    if (argc < 2) {
        std::cout << "usage: sender_linux <receiver_ip> [--hotkey=ALT+1]\n"
                     "example: ./sender_linux 192.168.1.100\n"
                     "         ./sender_linux 192.168.1.100 --hotkey=ALT+1\n"
                     "         ./sender_linux 192.168.1.100 --hotkey=CTRL+SHIFT+K\n\n"
                     "NOTE: Run as root or add your user to the 'input' group:\n"
                     "  sudo usermod -a -G input $USER\n"
                     "  (then log out and back in)\n";
        return 1;
    }

    // Set default hotkey
    g_hotkey.alt = true;
    g_hotkey.keycode = KEY_1;
    g_hotkey.description = "ALT+1";

    // Read config file
    ReadConfigFile(g_hotkey);

    // Parse CLI arguments
    for (int i = 2; i < argc; i++) {
        std::string arg = argv[i];
        if (arg.find("--hotkey=") == 0) {
            std::string hotkeyStr = arg.substr(9);
            HotkeyConfig tempConfig;
            if (ParseHotkey(hotkeyStr, tempConfig)) {
                g_hotkey = tempConfig;
                std::cout << "CLI override: Using hotkey " << g_hotkey.description << "\n";
            } else {
                std::cerr << "Invalid hotkey format: " << hotkeyStr << "\n";
                return 1;
            }
        }
    }

    // Setup signal handler
    signal(SIGINT, SignalHandler);
    signal(SIGTERM, SignalHandler);

    // Initialize socket
    g_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (g_sock < 0) {
        std::cerr << "socket() failed\n";
        return 1;
    }

    g_recvAddr.sin_family = AF_INET;
    g_recvAddr.sin_port = htons(PORT);
    if (inet_pton(AF_INET, argv[1], &g_recvAddr.sin_addr) <= 0) {
        std::cerr << "invalid IP address\n";
        close(g_sock);
        return 1;
    }

    // Find and open input devices
    auto keyboards = FindKeyboardDevices();

    if (keyboards.empty()) {
        std::cerr << "No keyboard devices found. Make sure you have permission to read /dev/input/*\n";
        std::cerr << "Run as root or: sudo usermod -a -G input $USER\n";
        close(g_sock);
        return 1;
    }

    std::cout << "Found " << keyboards.size() << " keyboard(s)\n";

    for (const auto& path : keyboards) {
        int fd = open(path.c_str(), O_RDONLY | O_NONBLOCK);
        if (fd >= 0) {
            g_keyboardFds.push_back(fd);
            std::cout << "  Keyboard: " << path << "\n";
        }
    }

    std::cout << "sender running -> target " << argv[1] << ":" << PORT << "\n";
    std::cout << "press " << g_hotkey.description << " to toggle the keyboard (mouse stays local)\n";

    // Main event loop using poll/select
    fd_set readfds;
    int maxfd = 0;

    for (int fd : g_keyboardFds) if (fd > maxfd) maxfd = fd;
    while (g_running) {
        FD_ZERO(&readfds);
        for (int fd : g_keyboardFds) FD_SET(fd, &readfds);

        timeval timeout{0, 10000};  // 10ms timeout
        int ret = select(maxfd + 1, &readfds, nullptr, nullptr, &timeout);

        if (ret < 0) {
            if (errno == EINTR) continue;
            break;
        }

        bool hotkey_triggered = false;

        // Process keyboard events
        for (int fd : g_keyboardFds) {
            if (!FD_ISSET(fd, &readfds)) continue;

            input_event ev;
            while (read(fd, &ev, sizeof(ev)) == sizeof(ev)) {
                ProcessKeyboardEvent(ev, hotkey_triggered);
            }
        }

        // Handle hotkey toggle
        if (hotkey_triggered) {
            if (g_capturing.load()) {
                StopCapture();
            } else {
                StartCapture();
            }
        }
    }

    // Cleanup
    std::cout << "\nShutting down...\n";
    StopCapture();

    for (int fd : g_keyboardFds) close(fd);

    if (g_sock >= 0) close(g_sock);

    return 0;
}
