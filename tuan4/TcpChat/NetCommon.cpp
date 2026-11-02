#include "NetCommon.h"
#include <Windows.h>
#include <algorithm>
#include <cstring>
#include <stdexcept>
#pragma comment(lib, "Ws2_32.lib")

// Chuyển socket sang chế độ không chặn.
bool SetNonBlocking(SOCKET hSocket) {
    u_long mode = 1;
    return ioctlsocket(hSocket, FIONBIO, &mode) == 0;
}

// Chờ tối đa 100ms mỗi lần để kiểm tra được cờ dừng.
static bool WaitSocket(SOCKET hSocket, bool canGui, const std::atomic<bool>& dangDung,
    ULONGLONG deadline) {
    while (!dangDung && GetTickCount64() < deadline) {
        fd_set sockets;
        FD_ZERO(&sockets);
        FD_SET(hSocket, &sockets);
        timeval timeout{ 0, 100000 };
        int result = select(0, canGui ? nullptr : &sockets,
            canGui ? &sockets : nullptr, nullptr, &timeout);
        if (result > 0) return true;
        if (result == SOCKET_ERROR) return false;
    }
    WSASetLastError(dangDung ? WSAEINTR : WSAETIMEDOUT);
    return false;
}

// Gửi đủ số byte yêu cầu, kể cả khi send chỉ gửi được một phần.
bool SendAll(SOCKET hSocket, const void* ptrGuiByte, size_t nByteCanGui,
    const std::atomic<bool>& dangDung) {
    const char* current = static_cast<const char*>(ptrGuiByte);
    size_t byteDaGui = 0;
    ULONGLONG deadline = GetTickCount64() + 30000;
    while (byteDaGui < nByteCanGui) {
        if (!WaitSocket(hSocket, true, dangDung, deadline)) return false;
        int nByteGui = send(hSocket, current + byteDaGui,
            static_cast<int>((std::min)(nByteCanGui - byteDaGui, size_t(65536))), 0);
        if (nByteGui == SOCKET_ERROR) {
            if (WSAGetLastError() == WSAEWOULDBLOCK) continue;
            return false;
        }
        if (nByteGui == 0) return false;
        byteDaGui += nByteGui;
    }
    return true;
}

// Đọc đủ số byte yêu cầu và phân biệt đóng kết nối với lỗi.
ReadResult RecvExact(SOCKET hSocket, void* ptrNhanByte, size_t nByteCanDoc,
    const std::atomic<bool>& dangDung) {
    char* current = static_cast<char*>(ptrNhanByte);
    size_t byteDaDoc = 0;
    ULONGLONG deadline = GetTickCount64() + 30000;
    while (byteDaDoc < nByteCanDoc) {
        if (!WaitSocket(hSocket, false, dangDung, deadline)) return ReadResult::Error;
        int nByteDoc = recv(hSocket, current + byteDaDoc,
            static_cast<int>((std::min)(nByteCanDoc - byteDaDoc, size_t(65536))), 0);
        if (nByteDoc == 0) return ReadResult::Closed;
        if (nByteDoc == SOCKET_ERROR) {
            if (WSAGetLastError() == WSAEWOULDBLOCK) continue;
            return ReadResult::Error;
        }
        byteDaDoc += nByteDoc;
    }
    return ReadResult::Ok;
}

// Đóng gói loại tin, tên và nội dung vào frame có độ dài ở đầu.
std::vector<char> EncodePacket(const Packet& packet) {
    size_t nByte = 5 + packet.name.size() + packet.text.size();
    if (packet.name.empty() || packet.name.size() > 256 || nByte > MAX_PAYLOAD_BYTES)
        throw std::runtime_error("Invalid packet size");
    // 4 byte đầu là độ dài body, dùng network byte order.
    std::vector<char> data(4 + nByte);
    uint32_t size = htonl(static_cast<uint32_t>(nByte));
    memcpy(data.data(), &size, 4);
    data[4] = static_cast<char>(packet.type);
    uint32_t nameSize = htonl(static_cast<uint32_t>(packet.name.size()));
    memcpy(data.data() + 5, &nameSize, 4);
    memcpy(data.data() + 9, packet.name.data(), packet.name.size());
    memcpy(data.data() + 9 + packet.name.size(), packet.text.data(), packet.text.size());
    return data;
}

// Đóng gói rồi gửi toàn bộ một packet.
bool SendPacket(SOCKET hSocket, const Packet& packet, const std::atomic<bool>& dangDung) {
    auto data = EncodePacket(packet);
    return SendAll(hSocket, data.data(), data.size(), dangDung);
}

// Đọc và kiểm tra một packet hoàn chỉnh từ luồng TCP.
ReadResult RecvPacket(SOCKET hSocket, Packet& packet, const std::atomic<bool>& dangDung) {
    uint32_t size = 0;
    ReadResult result = RecvExact(hSocket, &size, 4, dangDung);
    if (result != ReadResult::Ok) return result;
    size = ntohl(size);
    if (size < 6 || size > MAX_PAYLOAD_BYTES) return ReadResult::Error;
    std::vector<char> data(size);
    result = RecvExact(hSocket, data.data(), data.size(), dangDung);
    if (result != ReadResult::Ok) return result;
    unsigned char type = static_cast<unsigned char>(data[0]);
    uint32_t nameSize = 0;
    memcpy(&nameSize, data.data() + 1, 4);
    nameSize = ntohl(nameSize);
    if (type < 1 || type > 4 || nameSize == 0 || nameSize > 256 || nameSize > size - 5)
        return ReadResult::Error;
    packet.type = static_cast<PacketType>(type);
    packet.name.assign(data.data() + 5, nameSize);
    packet.text.assign(data.data() + 5 + nameSize, size - 5 - nameSize);
    if (packet.name.find('\0') != std::string::npos || packet.text.find('\0') != std::string::npos)
        return ReadResult::Error;
    try { FromUtf8(packet.name); FromUtf8(packet.text); }
    catch (...) { return ReadResult::Error; }
    return ReadResult::Ok;
}

// Chuyển chuỗi Unicode của Windows sang UTF-8 để truyền qua mạng.
std::string ToUtf8(const std::wstring& text) {
    if (text.empty()) return {};
    int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(),
        static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (!size) throw std::runtime_error("Invalid Unicode");
    std::string result(size, '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
        &result[0], size, nullptr, nullptr);
    return result;
}

// Chuyển UTF-8 nhận từ mạng sang chuỗi Unicode của Windows.
std::wstring FromUtf8(const std::string& text) {
    if (text.empty()) return {};
    int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
        static_cast<int>(text.size()), nullptr, 0);
    if (!size) throw std::runtime_error("Invalid UTF-8");
    std::wstring result(size, L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
        &result[0], size);
    return result;
}
