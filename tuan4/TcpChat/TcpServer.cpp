#include "TcpChat.h"
#include <Windows.h>
#include <algorithm>
#include <stdexcept>

// Bảo đảm listener và các worker đã kết thúc khi hủy server.
TcpServer::~TcpServer() { Stop(); }

// Mở socket lắng nghe và tạo luồng nhận kết nối.
bool TcpServer::Start(unsigned short port, const std::string& name, Notify notify, int& error) {
    Stop();
    WSADATA wsaData{};
    error = WSAStartup(MAKEWORD(2, 2), &wsaData);
    if (error != 0) return false;
    m_listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    BOOL exclusive = TRUE;
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(port);
    if (m_listener == INVALID_SOCKET ||
        setsockopt(m_listener, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
            reinterpret_cast<const char*>(&exclusive), sizeof(exclusive)) != 0 ||
        !SetNonBlocking(m_listener) ||
        bind(m_listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR ||
        listen(m_listener, SOMAXCONN) == SOCKET_ERROR) {
        error = WSAGetLastError();
        if (m_listener != INVALID_SOCKET) closesocket(m_listener);
        m_listener = INVALID_SOCKET;
        WSACleanup();
        return false;
    }
    int length = sizeof(address);
    getsockname(m_listener, reinterpret_cast<sockaddr*>(&address), &length);
    m_port = ntohs(address.sin_port);
    m_name = name;
    m_notify = std::move(notify);
    m_stopping = false;
    try { m_thread = std::thread(&TcpServer::Run, this); }
    catch (...) {
        m_stopping = true;
        closesocket(m_listener);
        m_listener = INVALID_SOCKET;
        WSACleanup();
        error = WSAENOBUFS;
        return false;
    }
    return true;
}

// Báo dừng rồi chờ luồng server dọn các kết nối.
void TcpServer::Stop() {
    m_stopping = true;
    if (m_thread.joinable()) m_thread.join();
}

// Xếp tin gửi cho đúng client đang kết nối.
bool TcpServer::QueueMessage(uint64_t clientId, const std::string& text) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_stopping || text.empty() || text.size() + m_name.size() + 5 > MAX_PAYLOAD_BYTES) return false;
    for (const auto& client : m_clients) {
        if (client->id != clientId || !client->ready) continue;
        std::lock_guard<std::mutex> queueLock(client->mutex);
        if (client->messages.size() >= 128 || client->queueBytes + text.size() > 32 * 1024 * 1024) return false;
        client->messages.push_back(text);
        client->queueBytes += text.size();
        return true;
    }
    return false;
}

// Chờ kết nối mới, tạo worker và dọn socket khi server dừng.
void TcpServer::Run() {
    try {
        while (!m_stopping) {
            // Thu lại worker đã kết thúc để không giữ thread cũ mãi.
            for (auto it = m_workers.begin(); it != m_workers.end();) {
                if ((*it)->done) { (*it)->worker.join(); it = m_workers.erase(it); }
                else ++it;
            }
            fd_set readSet;
            FD_ZERO(&readSet); FD_SET(m_listener, &readSet);
            timeval timeout{ 0, 100000 };
            int result = select(0, &readSet, nullptr, nullptr, &timeout);
            if (result == SOCKET_ERROR) throw std::runtime_error("select listener failed");
            if (result == 0 || m_stopping) continue;
            SOCKET hClient = accept(m_listener, nullptr, nullptr);
            if (hClient == INVALID_SOCKET) {
                int error = WSAGetLastError();
                if (error == WSAEWOULDBLOCK || error == WSAECONNRESET || error == WSAENOBUFS) {
                    Sleep(50);
                    continue;
                }
                throw std::runtime_error("accept: " + std::to_string(error));
            }
            if (!SetNonBlocking(hClient)) { closesocket(hClient); continue; }
            auto client = std::make_shared<Client>();
            client->id = m_nextId++;
            client->socket = hClient;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                if (m_clients.size() >= 32) { closesocket(hClient); continue; }
                m_clients.push_back(client);
            }
            // Socket được đăng ký trước khi giao cho worker.
            try {
                m_workers.push_back(client);
                client->worker = std::thread(&TcpServer::RunWorker, this, client);
            }
            catch (...) {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_clients.erase(std::remove(m_clients.begin(), m_clients.end(), client), m_clients.end());
                m_workers.erase(std::remove(m_workers.begin(), m_workers.end(), client), m_workers.end());
                closesocket(hClient);
                throw;
            }
        }
    }
    catch (const std::exception& ex) { m_notify({ EventType::Status, 0, {}, ex.what() }); }
    m_stopping = true;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (const auto& client : m_clients) shutdown(client->socket, SD_BOTH);
    }
    for (const auto& client : m_workers) client->worker.join();
    m_workers.clear();
    closesocket(m_listener);
    m_listener = INVALID_SOCKET;
    WSACleanup();
    m_notify({ EventType::Stopped });
}

// Xử lý Hello và từng request của một client trên socket riêng.
void TcpServer::RunWorker(std::shared_ptr<Client> client) {
    bool daKetNoi = false;
    try {
        Packet request;
        if (RecvPacket(client->socket, request, m_stopping) != ReadResult::Ok ||
            request.type != PacketType::Hello || !request.text.empty())
            throw std::runtime_error("Hello khong hop le");
        client->name = request.name;
        if (!SendPacket(client->socket, { PacketType::Hello, m_name, {} }, m_stopping))
            throw std::runtime_error("Gui Hello that bai");
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            client->ready = true;
        }
        daKetNoi = true;
        m_notify({ EventType::Connected, client->id, client->name, {} });
        std::string choXacNhan;
        while (!m_stopping) {
            ReadResult result = RecvPacket(client->socket, request, m_stopping);
            if (result == ReadResult::Closed) break;
            if (result != ReadResult::Ok || request.name != client->name ||
                (request.type != PacketType::Chat && request.type != PacketType::Poll) ||
                (request.type == PacketType::Poll && !request.text.empty()) ||
                (request.type == PacketType::Chat && request.text.empty()))
                throw std::runtime_error("Request loi hoac ket noi bi ngat");
            // Client chỉ gửi request tiếp theo sau khi đọc đủ response trước.
            if (!choXacNhan.empty()) {
                m_notify({ EventType::Sent, client->id, client->name, choXacNhan });
                choXacNhan.clear();
            }
            if (request.type == PacketType::Chat)
                m_notify({ EventType::Received, client->id, client->name, request.text });
            std::string messageGui;
            {
                std::lock_guard<std::mutex> lock(client->mutex);
                if (!client->messages.empty()) {
                    messageGui = std::move(client->messages.front());
                    client->messages.pop_front();
                    client->queueBytes -= messageGui.size();
                }
            }
            // Response xác nhận request và mang theo một tin server đang chờ gửi.
            if (!SendPacket(client->socket, { PacketType::Response, m_name, messageGui }, m_stopping))
                throw std::runtime_error("Gui response that bai");
            choXacNhan = std::move(messageGui);
        }
    }
    catch (const std::exception& ex) {
        if (!m_stopping) m_notify({ EventType::Status, client->id, client->name, ex.what() });
    }
    // Gỡ socket khỏi danh sách trước khi worker đóng nó.
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_clients.erase(std::remove(m_clients.begin(), m_clients.end(), client), m_clients.end());
    }
    shutdown(client->socket, SD_BOTH);
    closesocket(client->socket);
    if (daKetNoi) m_notify({ EventType::Disconnected, client->id, client->name, {} });
    client->done = true;
}
