#pragma once

#include <windows.h>
#include <shellapi.h>
#include <cstdint>

struct TrayIconState
{
    NOTIFYICONDATAW data{};
    HICON icon = nullptr;
    bool added = false;
};

static inline bool TrayPointInRoundedRect(
    int x, int y, int left, int top, int right, int bottom, int radius)
{
    if (x < left || x >= right || y < top || y >= bottom)
        return false;
    if ((x >= left + radius && x < right - radius) ||
        (y >= top + radius && y < bottom - radius))
        return true;

    int centerX = x < left + radius ? left + radius : right - radius - 1;
    int centerY = y < top + radius ? top + radius : bottom - radius - 1;
    int dx = x - centerX;
    int dy = y - centerY;
    return dx * dx + dy * dy <= radius * radius;
}

static inline HICON CreateKeyboardKeyIcon(bool active)
{
    constexpr int size = 32;
    BITMAPV5HEADER header{};
    header.bV5Size = sizeof(header);
    header.bV5Width = size;
    header.bV5Height = -size;
    header.bV5Planes = 1;
    header.bV5BitCount = 32;
    header.bV5Compression = BI_BITFIELDS;
    header.bV5RedMask = 0x00FF0000;
    header.bV5GreenMask = 0x0000FF00;
    header.bV5BlueMask = 0x000000FF;
    header.bV5AlphaMask = 0xFF000000;

    void *pixelData = nullptr;
    HDC screenDc = GetDC(nullptr);
    HBITMAP colorBitmap = CreateDIBSection(
        screenDc, reinterpret_cast<BITMAPINFO *>(&header), DIB_RGB_COLORS,
        &pixelData, nullptr, 0);
    ReleaseDC(nullptr, screenDc);
    if (!colorBitmap || !pixelData)
    {
        if (colorBitmap)
            DeleteObject(colorBitmap);
        return nullptr;
    }

    auto *pixels = static_cast<std::uint32_t *>(pixelData);
    for (int i = 0; i < size * size; ++i)
        pixels[i] = 0;

    const std::uint32_t face = active ? 0xFF34C759 : 0xFF8E8E93;
    const std::uint32_t highlight = active ? 0xFF65D980 : 0xFFB4B4B8;
    const std::uint32_t bevel = active ? 0xFF208C40 : 0xFF626267;
    const std::uint32_t border = 0xFF252525;
    const std::uint32_t legend = 0xFFF8F8F8;

    for (int y = 0; y < size; ++y)
    {
        for (int x = 0; x < size; ++x)
        {
            std::uint32_t color = 0;
            if (TrayPointInRoundedRect(x, y, 2, 3, 30, 29, 6))
                color = border;
            if (TrayPointInRoundedRect(x, y, 4, 5, 28, 27, 4))
                color = y < 9 ? highlight : (y >= 23 ? bevel : face);
            if (x >= 9 && x < 23 && y >= 15 && y < 18)
                color = legend;
            pixels[y * size + x] = color;
        }
    }

    std::uint32_t maskBits[size]{};
    HBITMAP maskBitmap = CreateBitmap(size, size, 1, 1, maskBits);
    if (!maskBitmap)
    {
        DeleteObject(colorBitmap);
        return nullptr;
    }

    ICONINFO iconInfo{};
    iconInfo.fIcon = TRUE;
    iconInfo.hbmColor = colorBitmap;
    iconInfo.hbmMask = maskBitmap;
    HICON icon = CreateIconIndirect(&iconInfo);
    DeleteObject(maskBitmap);
    DeleteObject(colorBitmap);
    return icon;
}

static inline bool SetTrayIcon(
    TrayIconState &state, HWND window, UINT id, UINT callbackMessage,
    bool active, const wchar_t *tooltip)
{
    HICON newIcon = CreateKeyboardKeyIcon(active);
    if (!newIcon)
        return false;

    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd = window;
    data.uID = id;
    data.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    data.uCallbackMessage = callbackMessage;
    data.hIcon = newIcon;
    lstrcpynW(data.szTip, tooltip, ARRAYSIZE(data.szTip));

    bool success = Shell_NotifyIconW(state.added ? NIM_MODIFY : NIM_ADD, &data) != FALSE;
    if (!success)
    {
        DestroyIcon(newIcon);
        return false;
    }

    if (state.icon)
        DestroyIcon(state.icon);
    state.data = data;
    state.icon = newIcon;
    state.added = true;
    return true;
}

static inline void RemoveTrayIcon(TrayIconState &state)
{
    if (state.added)
        Shell_NotifyIconW(NIM_DELETE, &state.data);
    if (state.icon)
        DestroyIcon(state.icon);
    state = {};
}
