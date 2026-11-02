#pragma once
#include "NetCommon.h"
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

enum class EventType { Status, Connected, Disconnected, Received, Sent, Stopped };
struct NetEvent {
    EventType type;
    uint64_t clientId = 0;
    std::string name;
    std::string text;
};
using Notify = std::function<void(const NetEvent&)>;

class TcpClient {
public:
    ~TcpClient();
    void Start(const std::string& ip, unsigned short port, const std::string& name, Notify notify);
    void Stop();
    bool QueueMessage(const std::string& text);
private:
    void Run(std::string ip, unsigned short port);
    std::atomic<bool> m_stopping{ true };
    std::atomic<bool> m_connected{ false };
    std::thread m_thread;
    std::mutex m_mutex;
    std::condition_variable m_wake;
    std::deque<std::string> m_messages;
    size_t m_queueBytes = 0;
    std::string m_name;
    Notify m_notify;
};

class TcpServer {
public:
    ~TcpServer();
    bool Start(unsigned short port, const std::string& name, Notify notify, int& error);
    void Stop();
    bool QueueMessage(uint64_t clientId, const std::string& text);
    // Trả về cổng đang lắng nghe, dùng cả khi hệ thống tự cấp cổng.
    unsigned short Port() const { return m_port; }
private:
    struct Client {
        uint64_t id = 0;
        SOCKET socket = INVALID_SOCKET;
        std::string name;
        bool ready = false;
        std::mutex mutex;
        std::deque<std::string> messages;
        size_t queueBytes = 0;
        std::atomic<bool> done{ false };
        std::thread worker;
    };
    void Run();
    void RunWorker(std::shared_ptr<Client> client);
    std::atomic<bool> m_stopping{ true };
    std::thread m_thread;
    SOCKET m_listener = INVALID_SOCKET;
    unsigned short m_port = 0;
    uint64_t m_nextId = 1;
    std::mutex m_mutex;
    std::vector<std::shared_ptr<Client>> m_clients;
    std::vector<std::shared_ptr<Client>> m_workers;
    std::string m_name;
    Notify m_notify;
};
