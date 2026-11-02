#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

struct ChatUser {
    int64_t id = 0;
    std::string name;
    bool online = false;
};

struct ChatMessage {
    int64_t id = 0;
    int64_t sender = 0;
    std::string content;
    std::string time;
};

struct ChatEvent {
    enum class Kind { Status, Login, Logout, Users, Messages, Sent, Busy };
    Kind kind = Kind::Status;
    std::string text;
    int64_t userId = 0;
    int64_t peer = 0;
    bool busy = false;
    std::vector<ChatUser> users;
    std::vector<ChatMessage> messages;
};

// Toàn bộ HTTP ở worker; SSE có một luồng nhận riêng.
// Callback chỉ chuyển dữ liệu sang hàng đợi của UI.
class ChatConnection {
public:
    using Notify = std::function<void(ChatEvent)>;
    explicit ChatConnection(Notify notify);
    ~ChatConnection();
    void Check(std::string host, int port);
    void Register(std::string host, int port, std::string name, std::string password);
    void Login(std::string host, int port, std::string name, std::string password);
    void Logout();
    void Select(int64_t peer);
    void Send(int64_t peer, std::string content);
    void Refresh();
    void Stop();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
