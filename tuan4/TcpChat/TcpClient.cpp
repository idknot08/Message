#include "TcpChat.h"
#include <Windows.h>
#include <chrono>
#include <stdexcept>

// Bảo đảm luồng mạng kết thúc khi hủy client.
TcpClient::~TcpClient() { Stop(); }

// Lưu cấu hình và tạo luồng kết nối đến server.
void TcpClient::Start(const std::string& ip, unsigned short port,
    const std::string& name, Notify notify) {
    Stop();
    m_name = name;
    m_notify = std::move(notify);
    m_stopping = false;
    m_thread = std::thread(&TcpClient::Run, this, ip, port);
}

// Báo dừng, chờ luồng mạng kết thúc và xóa tin chưa gửi.
void TcpClient::Stop() {
    m_stopping = true;
    m_wake.notify_all();
    if (m_thread.joinable()) m_thread.join();
    m_connected = false;
    std::lock_guard<std::mutex> lock(m_mutex);
    m_messages.clear();
    m_queueBytes = 0;
}

// Xếp tin vào hàng đợi nếu kết nối còn hoạt động và chưa quá giới hạn.
bool TcpClient::QueueMessage(const std::string& text) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_connected || m_stopping || text.empty() || text.size() + m_name.size() + 5 > MAX_PAYLOAD_BYTES ||
        m_messages.size() >= 128 || m_queueBytes + text.size() > 32 * 1024 * 1024) return false;
    m_messages.push_back(text);
    m_queueBytes += text.size();
    m_wake.notify_one();
    return true;
}

// Kết nối, trao đổi request-response và đóng socket khi kết thúc.
void TcpClient::Run(std::string ip, unsigned short port) {
    WSADATA wsaData{};
    int error = WSAStartup(MAKEWORD(2, 2), &wsaData);
    if (error != 0) {
        m_notify({ EventType::Status, 0, {}, "WSAStartup: " + std::to_string(error) });
        m_notify({ EventType::Stopped });
        return;
    }
    SOCKET hSocket = INVALID_SOCKET;
    std::string peerName;
    bool daKetNoi = false;
    try {
        hSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (hSocket == INVALID_SOCKET || !SetNonBlocking(hSocket))
            throw std::runtime_error("socket: " + std::to_string(WSAGetLastError()));
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(port);
        if (inet_pton(AF_INET, ip.c_str(), &address.sin_addr) != 1)
            throw std::runtime_error("IPv4 khong hop le");
        int checkConnect = connect(hSocket, reinterpret_cast<sockaddr*>(&address), sizeof(address));
        if (checkConnect == SOCKET_ERROR && WSAGetLastError() != WSAEWOULDBLOCK)
            throw std::runtime_error("connect: " + std::to_string(WSAGetLastError()));
        if (checkConnect == SOCKET_ERROR) {
            bool ready = false;
            ULONGLONG deadline = GetTickCount64() + 5000;
            while (!m_stopping && GetTickCount64() < deadline) {
                fd_set writeSet, errorSet;
                FD_ZERO(&writeSet); FD_ZERO(&errorSet);
                FD_SET(hSocket, &writeSet); FD_SET(hSocket, &errorSet);
                timeval timeout{ 0, 100000 };
                int result = select(0, nullptr, &writeSet, &errorSet, &timeout);
                if (result == SOCKET_ERROR) throw std::runtime_error("select connect failed");
                if (result > 0) {
                    int socketError = 0;
                    int length = sizeof(socketError);
                    if (getsockopt(hSocket, SOL_SOCKET, SO_ERROR,
                        reinterpret_cast<char*>(&socketError), &length) != 0 || socketError != 0)
                        throw std::runtime_error("connect: " + std::to_string(socketError));
                    ready = true;
                    break;
                }
            }
            if (!ready) throw std::runtime_error("connect timeout/cancelled");
        }

        Packet response;
        if (!SendPacket(hSocket, { PacketType::Hello, m_name, {} }, m_stopping) ||
            RecvPacket(hSocket, response, m_stopping) != ReadResult::Ok ||
            response.type != PacketType::Hello || !response.text.empty())
            throw std::runtime_error("Khong nhan duoc loi chao tu server");
        peerName = response.name;
        m_connected = true;
        daKetNoi = true;
        m_notify({ EventType::Connected, 1, peerName, {} });

        while (!m_stopping) {
            std::string messageGui;
            {
                std::unique_lock<std::mutex> lock(m_mutex);
                m_wake.wait_for(lock, std::chrono::milliseconds(150),
                    [&] { return m_stopping || !m_messages.empty(); });
                if (m_stopping) break;
                if (!m_messages.empty()) {
                    messageGui = std::move(m_messages.front());
                    m_messages.pop_front();
                    m_queueBytes -= messageGui.size();
                }
            }
            // Không có tin để gửi thì hỏi server có tin mới không.
            Packet request{ messageGui.empty() ? PacketType::Poll : PacketType::Chat, m_name, messageGui };
            if (!SendPacket(hSocket, request, m_stopping))
                throw std::runtime_error("send_all failed; tin dang gui chua duoc xac nhan");
            ReadResult result = RecvPacket(hSocket, response, m_stopping);
            if (result == ReadResult::Closed)
                throw std::runtime_error("Server da dong ket noi; tin dang gui chua duoc xac nhan");
            if (result != ReadResult::Ok || response.type != PacketType::Response || response.name != peerName)
                throw std::runtime_error("Response loi/thieu; tin dang gui chua duoc xac nhan");
            if (!messageGui.empty()) m_notify({ EventType::Sent, 1, peerName, messageGui });
            if (!response.text.empty()) m_notify({ EventType::Received, 1, peerName, response.text });
        }
    }
    catch (const std::exception& ex) {
        if (!m_stopping) m_notify({ EventType::Status, 1, peerName, ex.what() });
    }
    m_connected = false;
    if (hSocket != INVALID_SOCKET) {
        shutdown(hSocket, SD_SEND);
        closesocket(hSocket);
    }
    WSACleanup();
    if (daKetNoi) m_notify({ EventType::Disconnected, 1, peerName, {} });
    m_notify({ EventType::Stopped });
}
