#define NOMINMAX
#include "third_party/httplib.h"
#include "third_party/json.hpp"
#include "ChatConnection.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <stdexcept>
#include <thread>

using json = nlohmann::json;
using namespace std::chrono_literals;

namespace {
struct ApiError : std::runtime_error {
    int status;
    ApiError(int code, const std::string& message) : runtime_error(message), status(code) {}
};

json Decode(const httplib::Result& result) {
    if (!result) throw ApiError(0, "Không liên lạc được với server. " + httplib::to_string(result.error()));
    auto body = json::parse(result->body, nullptr, false);
    if (result->status < 200 || result->status >= 300) {
        const std::string message = body.is_object() && body.contains("error") && body["error"].is_string()
            ? body["error"].get<std::string>() : "Server trả lỗi HTTP " + std::to_string(result->status);
        throw ApiError(result->status, message);
    }
    if (!body.is_object()) throw ApiError(0, "Phản hồi JSON không hợp lệ.");
    return body;
}
}

struct ChatConnection::Impl {
    enum class Action { Check, Register, Login, Logout, Select, Send, Refresh };
    struct Job {
        Action action;
        std::string host;
        int port = 8080;
        std::string name;
        std::string password;
        int64_t peer = 0;
        std::string content;
    };

    Notify notify;
    std::mutex mutex;
    std::condition_variable changed;
    std::deque<Job> jobs;
    std::atomic<bool> stopping{false};
    std::atomic<bool> dirty{false};
    std::atomic<bool> sseFailed{false};
    std::atomic<bool> stopSse{false};
    std::shared_ptr<httplib::Client> api;
    std::shared_ptr<httplib::Client> stream;
    std::thread worker;
    std::thread receiver;
    std::string host;
    int port = 8080;
    std::string token;
    int64_t userId = 0;
    int64_t selected = 0;
    std::map<int64_t, std::vector<ChatMessage>> conversations;

    explicit Impl(Notify callback) : notify(std::move(callback)) {
        worker = std::thread([this] { Run(); });
    }

    void Emit(ChatEvent::Kind kind, const std::string& text = {}) {
        ChatEvent event;
        event.kind = kind;
        event.text = text;
        event.userId = userId;
        notify(std::move(event));
    }

    void Queue(Job job) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (stopping) return;
            jobs.push_back(std::move(job));
        }
        changed.notify_one();
    }

    httplib::Headers Headers() const {
        return {{"Authorization", "Bearer " + token}};
    }

    void Connect(const Job& job) {
        host = job.host;
        port = job.port;
        auto client = std::make_shared<httplib::Client>(host, port);
        client->set_connection_timeout(3);
        client->set_read_timeout(5);
        client->set_write_timeout(5);
        client->set_keep_alive(true);
        std::lock_guard<std::mutex> lock(mutex);
        api = std::move(client);
    }

    void StartEvents() {
        stopSse = false;
        sseFailed = false;
        auto client = std::make_shared<httplib::Client>(host, port);
        client->set_connection_timeout(3);
        client->set_read_timeout(10);
        client->set_write_timeout(5);
        {
            std::lock_guard<std::mutex> lock(mutex);
            stream = client;
        }
        const auto headers = Headers();
        receiver = std::thread([this, client, headers] {
            try {
                std::string pending;
                client->Get("/events", headers,
                    [](const httplib::Response& response) { return response.status == 200; },
                    [this, &pending](const char* data, size_t size) {
                        if (stopping || stopSse) return false;
                        pending.append(data, size);
                        if (pending.size() > 65536) return false;
                        size_t end;
                        while ((end = pending.find("\n\n")) != std::string::npos) {
                            const auto event = pending.substr(0, end);
                            pending.erase(0, end + 2);
                            if (event.find("event: ready") != std::string::npos ||
                                event.find("event: message") != std::string::npos) {
                                dirty = true;
                                changed.notify_one();
                            }
                        }
                        return true;
                    });
            } catch (...) {
                // Worker đưa lỗi lên UI và kết thúc phiên một cách thống nhất.
            }
            if (!stopSse && !stopping) {
                sseFailed = true;
                changed.notify_one();
            }
        });
    }

    void EndSession(bool logout) {
        stopSse = true;
        std::shared_ptr<httplib::Client> client;
        {
            std::lock_guard<std::mutex> lock(mutex);
            client = stream;
        }
        if (client) client->stop();
        if (receiver.joinable()) receiver.join();
        if (logout && !token.empty() && api && !stopping) {
            api->Post("/logout", Headers(), "{}", "application/json");
        }
        token.clear();
        userId = 0;
        selected = 0;
        conversations.clear();
        dirty = false;
        sseFailed = false;
        Emit(ChatEvent::Kind::Logout);
    }

    void LoadUsers() {
        const auto body = Decode(api->Get("/users", Headers()));
        ChatEvent event;
        event.kind = ChatEvent::Kind::Users;
        for (const auto& user : body.at("users")) {
            const auto id = user.at("id").get<int64_t>();
            if (id != userId) event.users.push_back({id,
                user.at("username").get<std::string>(), user.at("online").get<bool>()});
        }
        notify(std::move(event));
    }

    void LoadMessages() {
        if (!selected) return;
        auto& messages = conversations[selected];
        int64_t after = messages.empty() ? 0 : messages.back().id;
        while (!stopping) {
            const auto path = "/messages?peer_id=" + std::to_string(selected) +
                "&after_id=" + std::to_string(after) + "&limit=100";
            const auto body = Decode(api->Get(path, Headers()));
            const auto& page = body.at("messages");
            for (const auto& message : page) {
                const auto id = message.at("id").get<int64_t>();
                if (id <= after) throw ApiError(0, "Thứ tự lịch sử không hợp lệ.");
                messages.push_back({id, message.at("sender_id").get<int64_t>(),
                    message.at("content").get<std::string>(),
                    message.at("created_at").get<std::string>()});
                after = id;
            }
            if (page.size() < 100) break;
        }
        ChatEvent event;
        event.kind = ChatEvent::Kind::Messages;
        event.peer = selected;
        event.messages = messages;
        notify(std::move(event));
    }

    void Execute(const Job& job) {
        if (job.action == Action::Check || job.action == Action::Register || job.action == Action::Login) {
            if (!token.empty()) throw ApiError(409, "Hãy đăng xuất trước khi đổi kết nối.");
            Connect(job);
        }
        if (job.action == Action::Check) {
            const auto body = Decode(api->Get("/health"));
            if (!body.value("ok", false)) throw ApiError(0, "Server chưa sẵn sàng.");
            Emit(ChatEvent::Kind::Status, "Server đã phản hồi.");
        } else if (job.action == Action::Register || job.action == Action::Login) {
            const auto body = json{{"username", job.name}, {"password", job.password}};
            const auto response = Decode(api->Post(job.action == Action::Register ? "/register" : "/login",
                body.dump(), "application/json"));
            if (job.action == Action::Register) {
                Emit(ChatEvent::Kind::Status, "Đã tạo tài khoản. Bạn có thể đăng nhập.");
            } else {
                token = response.at("token").get<std::string>();
                userId = response.at("user_id").get<int64_t>();
                Emit(ChatEvent::Kind::Login, job.name);
                StartEvents();
                LoadUsers();
                Emit(ChatEvent::Kind::Status, "Đăng nhập thành công. Chọn người để mở hội thoại.");
            }
        } else if (job.action == Action::Logout) {
            EndSession(true);
            Emit(ChatEvent::Kind::Status, "Đã đăng xuất.");
        } else {
            if (token.empty()) throw ApiError(401, "Cần đăng nhập.");
            if (job.action == Action::Select) {
                selected = job.peer;
                LoadMessages();
            } else if (job.action == Action::Send) {
                Decode(api->Post("/messages", Headers(),
                    json{{"receiver_id", job.peer}, {"content", job.content}}.dump(), "application/json"));
                ChatEvent event;
                event.kind = ChatEvent::Kind::Sent;
                event.text = job.content;
                event.peer = job.peer;
                notify(std::move(event));
                LoadMessages();
                Emit(ChatEvent::Kind::Status, "Server đã lưu tin nhắn.");
            } else if (job.action == Action::Refresh) {
                LoadUsers();
                LoadMessages();
            }
        }
    }

    void Run() {
        auto nextHeartbeat = std::chrono::steady_clock::now() + 3s;
        while (!stopping) {
            Job job{Action::Refresh};
            bool hasJob = false;
            {
                std::unique_lock<std::mutex> lock(mutex);
                changed.wait_until(lock, nextHeartbeat, [this] {
                    return stopping || !jobs.empty() || dirty || sseFailed;
                });
                if (stopping) break;
                if (!jobs.empty()) {
                    job = std::move(jobs.front());
                    jobs.pop_front();
                    hasJob = true;
                }
            }
            try {
                if (sseFailed && !token.empty()) {
                    EndSession(false);
                    Emit(ChatEvent::Kind::Status, "Mất kênh nhận tin. Hãy đăng nhập lại khi server sẵn sàng.");
                }
                if (hasJob) Execute(job);
                if (!token.empty() && dirty.exchange(false)) LoadMessages();
                if (std::chrono::steady_clock::now() >= nextHeartbeat) {
                    if (!token.empty()) LoadUsers();
                    nextHeartbeat = std::chrono::steady_clock::now() + 3s;
                }
            } catch (const ApiError& error) {
                if (!token.empty() && (error.status == 0 || error.status == 401)) EndSession(false);
                Emit(ChatEvent::Kind::Status, error.what());
            } catch (const std::exception& error) {
                if (!token.empty()) EndSession(false);
                Emit(ChatEvent::Kind::Status, std::string("Lỗi xử lý: ") + error.what());
            }
            if (hasJob) Emit(ChatEvent::Kind::Busy);
        }
        EndSession(false);
    }

    void Stop() {
        stopping = true;
        stopSse = true;
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (api) api->stop();
            if (stream) stream->stop();
        }
        changed.notify_one();
        if (worker.joinable()) worker.join();
    }
};

ChatConnection::ChatConnection(Notify notify) : impl_(std::make_unique<Impl>(std::move(notify))) {}
ChatConnection::~ChatConnection() { Stop(); }
void ChatConnection::Stop() { impl_->Stop(); }
void ChatConnection::Check(std::string host, int port) {
    impl_->Queue({Impl::Action::Check, std::move(host), port});
}
void ChatConnection::Register(std::string host, int port, std::string name, std::string password) {
    impl_->Queue({Impl::Action::Register, std::move(host), port, std::move(name), std::move(password)});
}
void ChatConnection::Login(std::string host, int port, std::string name, std::string password) {
    impl_->Queue({Impl::Action::Login, std::move(host), port, std::move(name), std::move(password)});
}
void ChatConnection::Logout() { impl_->Queue({Impl::Action::Logout}); }
void ChatConnection::Select(int64_t peer) {
    Impl::Job job{Impl::Action::Select}; job.peer = peer; impl_->Queue(std::move(job));
}
void ChatConnection::Send(int64_t peer, std::string content) {
    Impl::Job job{Impl::Action::Send}; job.peer = peer; job.content = std::move(content);
    impl_->Queue(std::move(job));
}
void ChatConnection::Refresh() { impl_->Queue({Impl::Action::Refresh}); }
