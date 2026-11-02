#pragma once
#include <WinSock2.h>
#include <WS2tcpip.h>
#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

constexpr uint32_t MAX_PAYLOAD_BYTES = 16 * 1024 * 1024;
enum class PacketType : unsigned char { Hello = 1, Chat = 2, Poll = 3, Response = 4 };
struct Packet {
    PacketType type = PacketType::Poll;
    std::string name;
    std::string text;
};
enum class ReadResult { Ok, Closed, Error };
bool SetNonBlocking(SOCKET hSocket);
bool SendAll(SOCKET hSocket, const void* ptrGuiByte, size_t nByteCanGui, const std::atomic<bool>& dangDung);
ReadResult RecvExact(SOCKET hSocket, void* ptrNhanByte, size_t nByteCanDoc, const std::atomic<bool>& dangDung);
std::vector<char> EncodePacket(const Packet& packet);
bool SendPacket(SOCKET hSocket, const Packet& packet, const std::atomic<bool>& dangDung);
ReadResult RecvPacket(SOCKET hSocket, Packet& packet, const std::atomic<bool>& dangDung);
std::string ToUtf8(const std::wstring& text);
std::wstring FromUtf8(const std::string& text);
