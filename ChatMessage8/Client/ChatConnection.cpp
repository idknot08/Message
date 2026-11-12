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
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <set>

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

struct ChatConnection::NetworkWorker {
    enum class Action { Check, Register, Login, Logout, Select, Send, None, SendFile, Download, Avatar, Social, Members };
    struct Job {
        Action action;
        std::string host;
        int port = 8080;
        std::string name;
        std::string password;
        int64_t peer = 0;
        std::string content;
        std::wstring path;
        std::string operation;
        int64_t target = 0;
        std::vector<int64_t> members;
    };

    Notify notify;
    std::mutex mutex;
    std::condition_variable changed;
    std::deque<Job> jobs; // không cho phép thao 2 thao tác cùng lúc nên dùng mutex để chỉ cho 1 bên được thao tác
    std::atomic<bool> stopping{false};
    std::deque<json> incoming;
    int64_t directoryVersion = -1;
    std::set<int64_t> allowedGroups;
    std::map<int64_t,int64_t> historyAfter;
    std::atomic<bool> sseFailed{false};
    std::atomic<bool> stopSse{false};
    std::shared_ptr<httplib::Client> api;
    std::shared_ptr<httplib::Client> stream;
    std::thread worker;
    std::thread receiver;
    std::thread transfer;
    std::shared_ptr<httplib::Client> fileClient;
    std::atomic<bool> transferring{false};
    std::map<int64_t, std::pair<int64_t, std::string>> avatars; // map là kiểu dữ liệu lưu các cặp “khóa → giá trị”.
    std::map<int64_t,std::pair<int64_t,std::string>> roomAvatars;
    std::string host;
    int port = 8080;
    std::string token;
    int64_t userId = 0;
    int64_t selected = 0;
    unsigned networkFailures = 0;
    std::map<int64_t, std::vector<ChatMessage>> conversations;

    explicit NetworkWorker(Notify hamChuyenSuKienVeGiaoDien) { //Constructor phải trùng tên với struct
        notify = std::move(hamChuyenSuKienVeGiaoDien);
        worker = std::thread([this] { Run(); }); // this = instance được tạo
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
        changed.notify_one(); // đnáh thức worker thực hiện jobs nếu đang ngủ
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
        receiver = std::thread([this, client, headers] { // luồng chờ nhận thông báo
            try {
                std::string pending;
                client->Get("/events", headers,
                    [](const httplib::Response& response) { return response.status == 200; },
                    [this, &pending](const char* data, size_t size) {
                        if (stopping || stopSse) return false;
                        pending.append(data, size);
                        if (pending.size() > 4 * 1024 * 1024) return false;
                        size_t end;
                        while ((end = pending.find("\n\n")) != std::string::npos) {
                            const auto event = pending.substr(0, end);
                            pending.erase(0, end + 2);
                            const auto dataOffset = event.find("\ndata: ");
                            if (dataOffset != std::string::npos) {
                                auto packet = json::parse(event.substr(dataOffset + 7));
                                { std::lock_guard<std::mutex> lock(mutex); incoming.push_back(std::move(packet)); }
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
        if (fileClient) fileClient->stop();
        if (transfer.joinable()) transfer.join();
        transferring = false;
        avatars.clear(); roomAvatars.clear();
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
        networkFailures = 0;
        conversations.clear();
        { std::lock_guard<std::mutex> lock(mutex); incoming.clear(); }
        historyAfter.clear(); allowedGroups.clear(); directoryVersion = -1;
        sseFailed = false;
        Emit(ChatEvent::Kind::Logout);
    }
    // auto nghĩa là: “Hãy xác định kiểu của biến này từ giá trị bên phải.”
    // GET không thay đổi dữ liệu: có thể thử lại một lần nếu kết nối HTTP bị ngắt.
    json ReadJson(const std::string& path) {
        for(int attempt=0;attempt<2 && !stopping;++attempt) {
            auto result=api->Get(path,token.empty()?httplib::Headers{}:Headers());
            if(result) {networkFailures=0;return Decode(result);}
            if(attempt==1) throw ApiError(0,"Không đọc được GET "+path+": "+httplib::to_string(result.error()));
        }
        throw ApiError(0,"Đã dừng kết nối.");
    }

    void LoadUsers() { // kiểm tra trạng thái online, offline
        const auto body = ReadJson("/users");
        ChatEvent event;
        event.kind = ChatEvent::Kind::Users;
        for (const auto& user : body.at("users")) {
            const auto id = user.at("id").get<int64_t>();
            auto& cached = avatars[id];
            const auto version = user.value("avatar_version", int64_t{0});
            if (version != cached.first) {
                auto picture = api->Get("/avatars/" + std::to_string(id), Headers());
                if (picture && picture->status == 200 && picture->body.size() <= 2 * 1024 * 1024) {
                    cached = {version, picture->body};
                }
            }
            event.users.push_back({id, user.at("username").get<std::string>(),
                user.at("online").get<bool>(), cached.second});
        }
        notify(std::move(event));
    }

    // Gộp tin theo ID, giữ đúng thứ tự khi POST và SSE cùng mang về một tin.
    void MergeMessage(const json& message) {
        const auto sender = message.at("sender_id").get<int64_t>();
        const auto key = message.at("kind") == "group" ? -message.at("room_id").get<int64_t>() :
            (sender == userId ? message.at("receiver_id").get<int64_t>() : sender);
        if (key < 0 && !allowedGroups.count(-key)) return;
        ChatMessage item{message.at("id").get<int64_t>(), sender,
            message.at("content").get<std::string>(), message.at("created_at").get<std::string>(),
            message.value("filename",std::string{}),message.value("file_size",int64_t{0}),
            message.value("sender_name",std::string{})};
        auto& list = conversations[key];
        auto pos = std::lower_bound(list.begin(),list.end(),item.id,[](const ChatMessage& a,int64_t id){return a.id<id;});
        if (pos == list.end() || pos->id != item.id) list.insert(pos,std::move(item));
    }
    void ShowCurrentMessages() {
        if (!selected) return;
        ChatEvent event; event.kind=ChatEvent::Kind::Messages; event.peer=selected;
        event.messages=conversations[selected]; notify(std::move(event));
    }
    // Không dùng mốc của SSE/POST làm mốc lịch sử: có thể vẫn còn tin cũ chưa tải.
    void LoadMessages() {
        if (!selected) return;
        auto& after = historyAfter[selected];
        while (!stopping) {
            auto path = std::string("/messages?") + (selected<0 ? "room_id=" : "peer_id=") +
                std::to_string(selected<0 ? -selected : selected) + "&after_id=" + std::to_string(after);
            auto body=ReadJson(path);
            const auto& page=body.at("messages");
            for(const auto& message:page) {
                auto id=message.at("id").get<int64_t>();
                if(id<=after) throw ApiError(0,"Thứ tự lịch sử không hợp lệ.");
                MergeMessage(message); after=id;
            }
            if(page.size()<100) break;
        }
        ShowCurrentMessages();
    }
    // Chỉ nhận snapshot mới nhất; xóa dữ liệu nhóm khỏi client khi quyền bị thu hồi.
    void ApplyDirectory(const json& body) {
        auto version=body.value("version",int64_t{0});
        if(version<directoryVersion) return;
        directoryVersion=version;
        ChatEvent event; event.kind=ChatEvent::Kind::Directory;
        allowedGroups.clear();
        for(const auto& row:body.at("friends")) event.friends.push_back(row.at("user_id").get<int64_t>());
        for(const auto& row:body.at("rooms")) {
            const auto id=row.at("id").get<int64_t>(); allowedGroups.insert(id);
            auto& cached=roomAvatars[id];
            auto imageVersion=row.value("avatar_version",int64_t{0});
            if(imageVersion!=cached.first) {
                auto image=api->Get("/groups/"+std::to_string(id)+"/avatar",Headers());
                if(image && image->status==200 && image->body.size()<=2*1024*1024) cached={imageVersion,image->body};
                else if(image && image->status==403) {allowedGroups.erase(id);continue;} // Bị kick trong lúc đang tải snapshot cũ.
            }
            event.rooms.push_back({id,row.at("name").get<std::string>(),row.at("owner_id").get<int64_t>(),cached.second});
        }
        for(const auto& row:body.at("invitations")) {
            event.invitations.push_back({row.at("id").get<int64_t>(),row.at("sender_id").get<int64_t>(),
                row.at("receiver_id").get<int64_t>(),row.at("room_id").is_null()?0:row.at("room_id").get<int64_t>(),
                row.at("kind").get<std::string>(),row.at("sender_name").get<std::string>(),
                row.at("receiver_name").get<std::string>(),row.at("room_name").get<std::string>()});
        }
        for(auto it=roomAvatars.begin();it!=roomAvatars.end();) {
            if(!allowedGroups.count(it->first)) it=roomAvatars.erase(it);else ++it;
        }
        for(auto it=conversations.begin();it!=conversations.end();) {
            if(it->first<0 && !allowedGroups.count(-it->first)) {historyAfter.erase(it->first);it=conversations.erase(it);} else ++it;
        }
        if(selected<0 && !allowedGroups.count(-selected)) selected=0;
        notify(std::move(event));
    }
    void ApplyPacket(const json& packet) {
        if(packet.contains("directory")) ApplyDirectory(packet.at("directory"));
        if(packet.contains("messages")) for(const auto& message:packet.at("messages")) MergeMessage(message);
        if(packet.contains("message")) MergeMessage(packet.at("message"));
        ShowCurrentMessages();
    }

    // Đọc file trên máy gửi, tải lên server và chuyển thông tin file về worker.
    void UploadFile(const Job& job, httplib::Client& client, httplib::Headers headers) {
        const std::filesystem::path sourcePath(job.path);
        const int64_t conversationKey = job.peer;

        std::ifstream input(sourcePath, std::ios::binary | std::ios::ate);
        if (!input) {
            throw std::runtime_error("Không mở được file.");
        }
        const std::streamoff fileSize = input.tellg();
        if (fileSize < 0 || fileSize > 20 * 1024 * 1024) {
            throw std::runtime_error("File vượt giới hạn 20 MB.");
        }

        std::string fileData(static_cast<size_t>(fileSize), '\0');
        input.seekg(0);
        if (fileSize > 0 && !input.read(fileData.data(), fileSize)) {
            throw std::runtime_error("Không đọc đủ dữ liệu file.");
        }

        std::string requestPath;
        if (conversationKey < 0) {
            const int64_t roomId = -conversationKey;
            requestPath = "/files?room_id=" + std::to_string(roomId);
        } else {
            const int64_t receiverId = conversationKey;
            requestPath = "/files?receiver_id=" + std::to_string(receiverId);
        }


        const std::string fileName = sourcePath.filename().u8string();
        const std::string encodedFileName = json(fileName).dump(-1, ' ', true);
        headers.emplace("X-File-Name", encodedFileName);
        auto response = client.Post(requestPath, headers, fileData, "application/octet-stream");

        json savedMessage = Decode(response);
        {
            std::lock_guard<std::mutex> lock(mutex);
            incoming.push_back(json{{"message", savedMessage}});
        }
        changed.notify_one();
        Emit(ChatEvent::Kind::Status, "Đã gửi file: " + fileName);
    }

    // Tải các byte file từ server, ghi file tạm rồi chuyển sang tên người dùng chọn.
    void DownloadFile(const Job& job, httplib::Client& client, const httplib::Headers& headers) {
        const int64_t messageId = job.peer;
        const std::filesystem::path destinationPath(job.path);

        const std::string requestPath = "/files/" + std::to_string(messageId);
        auto response = client.Get(requestPath, headers);
        if (!response || response->status != 200) {
            Decode(response); // Đọc thông báo lỗi server hoặc lỗi kết nối.
            throw std::runtime_error("Không tải được file.");
        }
        const std::string& fileData = response->body;

        std::filesystem::path temporaryPath = destinationPath;
        temporaryPath += L".chat-" + std::to_wstring(GetCurrentProcessId()) + L".part";

        struct TemporaryFileCleanup {
            std::filesystem::path path;
            ~TemporaryFileCleanup() {
                std::error_code ignored;
                std::filesystem::remove(path, ignored);
            }
        };
        TemporaryFileCleanup cleanup;
        cleanup.path = temporaryPath;

        std::ofstream output(temporaryPath, std::ios::binary | std::ios::trunc);
        if (!output) {
            throw std::runtime_error("Không ghi được file tải xuống.");
        }
        output.write(fileData.data(), fileData.size());
        if (!output) {
            throw std::runtime_error("Không ghi được file tải xuống.");
        }
        output.close();
        if (!output) {
            throw std::runtime_error("Không lưu được file vào nơi đã chọn.");
        }

        // 3. Chỉ thay file đích sau khi ghi thành công toàn bộ file tạm.
        const bool saved = MoveFileExW(temporaryPath.c_str(), destinationPath.c_str(),
                                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
        if (!saved) {
            throw std::runtime_error("Không lưu được file vào nơi đã chọn.");
        }
        Emit(ChatEvent::Kind::Status, "Đã lưu file: " + destinationPath.u8string());
    }

    // Tạo kết nối và luồng truyền file riêng, chọn nhánh gửi/tải rồi báo hoàn tất.
    void Transfer(const Job& job) {
        if (transferring) {
            throw ApiError(409, "Đang truyền một file khác.");
        }
        if (transfer.joinable()) {
            transfer.join();
        }

        auto client = std::make_shared<httplib::Client>(host, port);
        client->set_connection_timeout(3);
        client->set_read_timeout(60);
        client->set_write_timeout(60);
        client->set_payload_max_length(20 * 1024 * 1024);
        {
            std::lock_guard<std::mutex> lock(mutex);
            fileClient = client; // Giữ kết nối để Stop() có thể ngắt khi đóng client.
        }
        const auto headers = Headers();
        transferring = true;

        transfer = std::thread([this, client, headers, job] {
            try {
                if (job.action == Action::SendFile) {
                    UploadFile(job, *client, headers);
                } else {
                    DownloadFile(job, *client, headers);
                }
            } catch (const std::exception& error) {
                if (!stopping && !stopSse) {
                    Emit(ChatEvent::Kind::Status, error.what());
                }
            }
            transferring = false;
            Emit(ChatEvent::Kind::Busy); // Báo giao diện mở lại các nút thao tác.
        });
    }

    void Execute(const Job& job) {
        if (job.action == Action::Check || job.action == Action::Register || job.action == Action::Login) {
            if (!token.empty()) throw ApiError(409, "Hãy đăng xuất trước khi đổi kết nối.");
            Connect(job);
        }
        if (job.action == Action::Check) {
            const auto body = ReadJson("/health");
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
                StartEvents(); // Tạo luồng nhận SSE, gửi GET /events để nhận các cập nhật từ server.
                ApplyDirectory(ReadJson("/directory"));
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
                if(selected<0) LoadMembers(-selected);
            } else if (job.action == Action::Send) {
                json body{{"content",job.content}};
                body[job.peer<0?"room_id":"receiver_id"]=job.peer<0?-job.peer:job.peer;
                MergeMessage(Decode(api->Post("/messages",Headers(),body.dump(),"application/json")));
                ChatEvent event;
                event.kind = ChatEvent::Kind::Sent;
                event.text = job.content;
                event.peer = job.peer;
                notify(std::move(event));
                ShowCurrentMessages();
                Emit(ChatEvent::Kind::Status, "Server đã lưu tin nhắn.");
            } else if (job.action == Action::SendFile || job.action == Action::Download) {
                Transfer(job);
            } else if (job.action == Action::Avatar) {
                if(job.peer) {
                    auto response=Decode(api->Post("/groups/"+std::to_string(job.peer)+"/avatar",Headers(),job.content,"image/png"));
                    ApplyDirectory(response.at("directory"));
                } else {
                    Decode(api->Post("/avatar",Headers(),job.content,"image/png"));
                    LoadUsers();
                }
                Emit(ChatEvent::Kind::Status, "Đã đổi ảnh đại diện.");
            } else if (job.action == Action::Social) {
                json body{{"op",job.operation},{"room_id",job.peer},{"invite_id",job.peer},
                          {"user_id",job.target},{"name",job.content},{"members",job.members}};
                auto response=Decode(api->Post("/social",Headers(),body.dump(),"application/json"));
                ApplyDirectory(response.at("directory"));
                Emit(ChatEvent::Kind::Status,"Đã cập nhật.");
                if(selected<0 && allowedGroups.count(-selected)) LoadMembers(-selected);
            } else if (job.action == Action::Members) {
                LoadMembers(job.peer);

            }
        }
    }

    void LoadMembers(int64_t room) {
        const auto body=ReadJson("/members?room_id="+std::to_string(room));
        ChatEvent event;event.kind=ChatEvent::Kind::Members;event.peer=-room;
        for(const auto& row:body.at("members")) event.members.push_back({row.at("id").get<int64_t>(),row.at("username").get<std::string>(),row.at("owner").get<int>()!=0});
        notify(std::move(event));
    }

    void Run() {
        auto nextHeartbeat = std::chrono::steady_clock::now() + 3s;
        while (!stopping) {
            Job job{Action::None};
            bool hasJob = false;
            bool transferStarted = false;
            {
                std::unique_lock<std::mutex> lock(mutex);
                changed.wait_until(lock, nextHeartbeat, [this] {
                    return stopping || !jobs.empty() || !incoming.empty() || sseFailed;
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
                if (hasJob) {
                    Execute(job);
                    transferStarted = job.action == Action::SendFile || job.action == Action::Download;
                }
                std::deque<json> packets;
                { std::lock_guard<std::mutex> lock(mutex); packets.swap(incoming); }
                if(!token.empty()) for(const auto& packet:packets) ApplyPacket(packet);
                if (std::chrono::steady_clock::now() >= nextHeartbeat) {
                    if (!token.empty()) LoadUsers();
                    nextHeartbeat = std::chrono::steady_clock::now() + 3s;
                }
            } catch (const ApiError& error) {
                // Một lỗi HTTP tạm thời không đồng nghĩa phiên đăng nhập đã hết hạn.
                // SSE mất hoặc ba lượt liên tiếp không liên lạc được mới kết thúc phiên.
                if(!token.empty() && (error.status==401 || (error.status==0 && ++networkFailures>=3))) EndSession(false);
                std::string detail=error.what();
                if(error.status==0 && hasJob && job.action==Action::Send)
                    detail+=" Tin có thể đã được lưu; kiểm tra hội thoại trước khi gửi lại.";
                Emit(ChatEvent::Kind::Status,detail);
            } catch (const std::exception& error) {
                if (!token.empty()) EndSession(false);
                Emit(ChatEvent::Kind::Status, std::string("Lỗi xử lý: ") + error.what());
            }
            if (hasJob && !transferStarted) Emit(ChatEvent::Kind::Busy);
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
            if (fileClient) fileClient->stop();
        }
        changed.notify_one();
        if (worker.joinable()) worker.join();
    }
};

ChatConnection::ChatConnection(Notify notify) : networkWorker_(std::make_unique<NetworkWorker>(std::move(notify))) {}
ChatConnection::~ChatConnection() { Stop(); }
void ChatConnection::Stop() { networkWorker_->Stop(); }
void ChatConnection::Check(std::string host, int port) {
    networkWorker_->Queue({NetworkWorker::Action::Check, std::move(host), port});
}
void ChatConnection::Register(std::string host, int port, std::string name, std::string password) {
    networkWorker_->Queue({NetworkWorker::Action::Register, std::move(host), port, std::move(name), std::move(password)});
}
void ChatConnection::Login(std::string host, int port, std::string name, std::string password) {
    networkWorker_->Queue({NetworkWorker::Action::Login, std::move(host), port, std::move(name), std::move(password)});
}
void ChatConnection::Logout() { networkWorker_->Queue({NetworkWorker::Action::Logout}); }
void ChatConnection::Select(int64_t peer) {
    NetworkWorker::Job job{NetworkWorker::Action::Select}; job.peer = peer; networkWorker_->Queue(std::move(job));
}
void ChatConnection::Send(int64_t peer, std::string content) {
    NetworkWorker::Job job{NetworkWorker::Action::Send}; job.peer = peer; job.content = std::move(content);
    networkWorker_->Queue(std::move(job)); // Đưa vào hàng đợi, một luồng khác gọi execute()
    // move() chuyển giá trị của biến content vào trường job.content, content không đảm bảo nội dung sau đó
}

void ChatConnection::SendFile(int64_t peer, std::wstring path) {
    NetworkWorker::Job job{NetworkWorker::Action::SendFile}; job.peer = peer; job.path = std::move(path);
    networkWorker_->Queue(std::move(job));
}
void ChatConnection::Download(int64_t messageId, std::wstring path) {
    NetworkWorker::Job job{NetworkWorker::Action::Download}; job.peer = messageId; job.path = std::move(path);
    networkWorker_->Queue(std::move(job));
}
void ChatConnection::SetAvatar(std::string png, int64_t room) {
    NetworkWorker::Job job{NetworkWorker::Action::Avatar}; job.content = std::move(png); job.peer = room;
    networkWorker_->Queue(std::move(job));
}

void ChatConnection::Social(std::string operation,int64_t id,int64_t user,std::string text,std::vector<int64_t> members) {
    NetworkWorker::Job job{NetworkWorker::Action::Social};job.operation=std::move(operation);
    job.peer=id;job.target=user;job.content=std::move(text);job.members=std::move(members);
    networkWorker_->Queue(std::move(job));
}
void ChatConnection::Members(int64_t room) {
    NetworkWorker::Job job{NetworkWorker::Action::Members};job.peer=room;networkWorker_->Queue(std::move(job));
}
