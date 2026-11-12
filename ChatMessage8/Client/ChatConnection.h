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
    std::string avatar;
};

struct ChatMessage {
    int64_t id = 0;
    int64_t sender = 0;
    std::string content;
    std::string time;
    std::string filename;
    int64_t fileSize = 0;
    std::string senderName;
};

struct ChatRoom {
    int64_t id = 0;
    std::string name;
    int64_t owner = 0;
    std::string avatar;
};
struct ChatInvite {
    int64_t id = 0, sender = 0, receiver = 0, room = 0;
    std::string kind, senderName, receiverName, roomName;
};
struct ChatMember {
    int64_t id = 0;
    std::string name;
    bool owner = false;
};

struct ChatEvent {
    enum class Kind { Status, Login, Logout, Users, Messages, Sent, Busy, Directory, Members };
    Kind kind = Kind::Status;
    std::string text;
    int64_t userId = 0;
    int64_t peer = 0;
    bool busy = false;
    std::vector<ChatUser> users;
    std::vector<ChatMessage> messages;
    std::vector<int64_t> friends;
    std::vector<ChatRoom> rooms;
    std::vector<ChatInvite> invitations;
    std::vector<ChatMember> members;
};

// HTTP thường ở worker; SSE và truyền file có luồng riêng.
// Callback chỉ chuyển dữ liệu sang hàng đợi của UI.
class ChatConnection {
public:
    using Notify = std::function<void(ChatEvent)>;
    // Tạo phần triển khai NetworkWorker chứa worker, hàng đợi và các kết nối HTTP/SSE.
    explicit ChatConnection(Notify notify);
    //connection là object của class chatconnection, xong cái hàm khởi tạo của nó là explicit ChatConnection(Notify notify); 
    // Dừng các luồng mạng trước khi hủy đối tượng kết nối.
    ~ChatConnection();
    // Xếp Job kiểm tra kết nối để worker gửi GET /health.
    void Check(std::string host, int port);
    // Xếp Job đăng ký để worker gửi tên và mật khẩu tới POST /register.
    void Register(std::string host, int port, std::string name, std::string password);
    // Xếp Job đăng nhập để worker gọi POST /login và lưu token khi thành công.
    void Login(std::string host, int port, std::string name, std::string password);
    // Xếp Job đăng xuất để worker dừng SSE và hủy phiên.
    void Logout();
    // Khóa hội thoại nội bộ: số dương là userId, số âm là -roomId của nhóm.
    // Request HTTP vẫn gửi room_id hoặc receiver_id dương. Chọn hội thoại để tải lịch sử.
    void Select(int64_t peer);
    // Gói người nhận/nội dung thành Job gửi tin, đưa qua Queue() để worker gọi Execute().
    void Send(int64_t peer, std::string content);
    // Tác vụ kết bạn/nhóm được xếp hàng và xác thực ở server.
    void Social(std::string operation, int64_t id = 0, int64_t user = 0,
                std::string text = {}, std::vector<int64_t> members = {});
    void Members(int64_t room);
    // Truyền file ở luồng riêng để worker vẫn cập nhật người dùng và tin mới.
    void SendFile(int64_t peer, std::wstring path);
    void Download(int64_t messageId, std::wstring path);
    // Nhận byte PNG đã thu nhỏ từ giao diện và lưu ảnh đại diện trên server.
    void SetAvatar(std::string png, int64_t room = 0);
    // Yêu cầu dừng, ngắt HTTP/SSE đang chờ và đợi worker kết thúc.
    void Stop();

private:
    struct NetworkWorker;
    std::unique_ptr<NetworkWorker> networkWorker_;
};
