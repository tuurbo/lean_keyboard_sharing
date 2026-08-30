#pragma once

#include <windows.h>
#include <cstdint>
#include <cstring>
#include <vector>

static constexpr std::uint32_t CLIP_MAX_TEXT_SIZE = 8 * 1024 * 1024;
static constexpr std::uint32_t CLIP_MAX_IMAGE_SIZE = 64 * 1024 * 1024;

#pragma pack(push, 1)
struct ClipHeader
{
    std::uint8_t msg_type;
    std::uint8_t content_type;
    std::uint32_t data_length;
};
#pragma pack(pop)

static_assert(sizeof(ClipHeader) == 6, "Unexpected clipboard header size");

static constexpr std::uint8_t CLIP_MSG_DATA = 0;
static constexpr std::uint8_t CLIP_MSG_REQUEST = 1;
static constexpr std::uint8_t CLIP_CONTENT_EMPTY = 0;
static constexpr std::uint8_t CLIP_CONTENT_TEXT = 1;
static constexpr std::uint8_t CLIP_CONTENT_IMAGE = 2;

static inline bool IsClipboardDataShapeValid(
    std::uint8_t contentType, std::uint32_t dataLength)
{
    switch (contentType)
    {
    case CLIP_CONTENT_EMPTY:
        return dataLength == 0;
    case CLIP_CONTENT_TEXT:
        return dataLength >= sizeof(wchar_t) &&
               dataLength <= CLIP_MAX_TEXT_SIZE &&
               dataLength % sizeof(wchar_t) == 0;
    case CLIP_CONTENT_IMAGE:
        return dataLength >= sizeof(BITMAPINFOHEADER) &&
               dataLength <= CLIP_MAX_IMAGE_SIZE;
    default:
        return false;
    }
}

static inline bool IsClipboardRequestValid(const ClipHeader &header)
{
    return header.msg_type == CLIP_MSG_REQUEST &&
           header.content_type == CLIP_CONTENT_EMPTY &&
           header.data_length == 0;
}

static inline bool IsClipboardDataHeaderValid(const ClipHeader &header)
{
    return header.msg_type == CLIP_MSG_DATA &&
           IsClipboardDataShapeValid(header.content_type, header.data_length);
}

static inline bool IsDibPayloadValid(const char *data, std::size_t size)
{
    if (!data || size < sizeof(BITMAPINFOHEADER) || size > CLIP_MAX_IMAGE_SIZE)
        return false;

    BITMAPINFOHEADER header{};
    std::memcpy(&header, data, sizeof(header));
    const bool supportedHeaderSize =
        header.biSize == sizeof(BITMAPINFOHEADER) ||
        header.biSize == 52 ||
        header.biSize == 56 ||
        header.biSize == sizeof(BITMAPV4HEADER) ||
        header.biSize == sizeof(BITMAPV5HEADER);
    if (!supportedHeaderSize || header.biSize > size ||
        header.biWidth <= 0 || header.biHeight == 0 ||
        header.biHeight == static_cast<LONG>(0x80000000) ||
        header.biPlanes != 1)
    {
        return false;
    }

    if (header.biBitCount != 1 && header.biBitCount != 4 &&
        header.biBitCount != 8 && header.biBitCount != 16 &&
        header.biBitCount != 24 && header.biBitCount != 32)
    {
        return false;
    }

    if (header.biCompression != BI_RGB && header.biCompression != BI_BITFIELDS)
        return false;
    if (header.biCompression == BI_BITFIELDS &&
        header.biBitCount != 16 && header.biBitCount != 32)
    {
        return false;
    }

    std::uint64_t height = header.biHeight < 0
        ? static_cast<std::uint64_t>(-static_cast<std::int64_t>(header.biHeight))
        : static_cast<std::uint64_t>(header.biHeight);
    if (height > 32768 || static_cast<std::uint64_t>(header.biWidth) > 32768)
        return false;

    std::uint64_t colorCount = header.biClrUsed;
    if (colorCount == 0 && header.biBitCount <= 8)
        colorCount = std::uint64_t{1} << header.biBitCount;
    if (colorCount > 256)
        return false;

    std::uint64_t maskBytes =
        header.biCompression == BI_BITFIELDS &&
        header.biSize == sizeof(BITMAPINFOHEADER)
            ? 3 * sizeof(DWORD)
            : 0;
    std::uint64_t pixelOffset = static_cast<std::uint64_t>(header.biSize) +
                                maskBytes + colorCount * sizeof(RGBQUAD);
    std::uint64_t rowBits = static_cast<std::uint64_t>(header.biWidth) *
                            header.biBitCount;
    std::uint64_t rowBytes = ((rowBits + 31) / 32) * 4;
    std::uint64_t pixelBytes = rowBytes * height;
    std::uint64_t requiredSize = pixelOffset + pixelBytes;
    if (pixelOffset > size || requiredSize > size)
        return false;

    if (header.biSizeImage != 0 &&
        (header.biSizeImage < pixelBytes ||
         static_cast<std::uint64_t>(header.biSizeImage) > size - pixelOffset))
    {
        return false;
    }
    return true;
}

static inline bool IsClipboardPayloadValid(
    std::uint8_t contentType, const char *data, std::size_t size)
{
    if (size > UINT32_MAX ||
        !IsClipboardDataShapeValid(contentType, static_cast<std::uint32_t>(size)))
    {
        return false;
    }

    if (contentType == CLIP_CONTENT_EMPTY)
        return true;
    if (!data)
        return false;
    if (contentType == CLIP_CONTENT_TEXT)
    {
        wchar_t terminator = L'X';
        std::memcpy(&terminator, data + size - sizeof(wchar_t), sizeof(terminator));
        return terminator == L'\0';
    }
    return IsDibPayloadValid(data, size);
}

static inline bool OpenClipboardWithRetry()
{
    for (int attempt = 0; attempt < 10; ++attempt)
    {
        if (OpenClipboard(nullptr))
            return true;
        Sleep(50);
    }
    return false;
}

static inline void ReadLocalClipboard(
    std::uint8_t &contentType, std::vector<char> &data)
{
    contentType = CLIP_CONTENT_EMPTY;
    data.clear();
    if (!OpenClipboardWithRetry())
        return;

    HANDLE dibHandle = GetClipboardData(CF_DIB);
    if (dibHandle)
    {
        SIZE_T size = GlobalSize(dibHandle);
        if (size >= sizeof(BITMAPINFOHEADER) && size <= CLIP_MAX_IMAGE_SIZE)
        {
            const char *bytes = static_cast<const char *>(GlobalLock(dibHandle));
            if (bytes)
            {
                if (IsDibPayloadValid(bytes, size))
                {
                    data.assign(bytes, bytes + size);
                    contentType = CLIP_CONTENT_IMAGE;
                }
                GlobalUnlock(dibHandle);
                if (contentType == CLIP_CONTENT_IMAGE)
                {
                    CloseClipboard();
                    return;
                }
            }
        }
    }

    HANDLE textHandle = GetClipboardData(CF_UNICODETEXT);
    if (textHandle)
    {
        SIZE_T allocationSize = GlobalSize(textHandle);
        const wchar_t *text = static_cast<const wchar_t *>(GlobalLock(textHandle));
        if (text)
        {
            std::size_t available = allocationSize / sizeof(wchar_t);
            std::size_t maximum = CLIP_MAX_TEXT_SIZE / sizeof(wchar_t);
            std::size_t limit = available < maximum ? available : maximum;
            std::size_t length = 0;
            while (length < limit && text[length] != L'\0')
                ++length;
            if (length < limit)
            {
                std::size_t byteCount = (length + 1) * sizeof(wchar_t);
                const char *bytes = reinterpret_cast<const char *>(text);
                data.assign(bytes, bytes + byteCount);
                contentType = CLIP_CONTENT_TEXT;
            }
            GlobalUnlock(textHandle);
        }
    }

    CloseClipboard();
}

static inline bool WriteLocalClipboard(
    std::uint8_t contentType, const std::vector<char> &data)
{
    if (!IsClipboardPayloadValid(contentType, data.data(), data.size()))
        return false;

    // An empty message can also mean that the source has no supported format.
    // Keep the destination clipboard unchanged in both cases.
    if (contentType == CLIP_CONTENT_EMPTY)
        return true;

    HGLOBAL memory = nullptr;
    memory = GlobalAlloc(GMEM_MOVEABLE, data.size());
    if (!memory)
        return false;

    void *destination = GlobalLock(memory);
    if (!destination)
    {
        GlobalFree(memory);
        return false;
    }
    std::memcpy(destination, data.data(), data.size());
    GlobalUnlock(memory);

    if (!OpenClipboardWithRetry())
    {
        if (memory)
            GlobalFree(memory);
        return false;
    }

    if (!EmptyClipboard())
    {
        CloseClipboard();
        if (memory)
            GlobalFree(memory);
        return false;
    }

    UINT format = contentType == CLIP_CONTENT_IMAGE ? CF_DIB : CF_UNICODETEXT;
    if (!SetClipboardData(format, memory))
    {
        CloseClipboard();
        GlobalFree(memory);
        return false;
    }

    CloseClipboard();
    return true;
}
