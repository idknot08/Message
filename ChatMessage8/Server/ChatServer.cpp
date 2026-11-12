#define NOMINMAX
#include <winsock2.h>
#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <fstream>
#include <limits>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>
#include <set>
#include <map>

#include "third_party/httplib.h"
#include "third_party/json.hpp"
#include "sqlite3.h"

#pragma comment(lib, "Bcrypt.lib")
#pragma comment(lib, "Advapi32.lib")

using json = nlohmann::json;
using namespace std;
namespace fs = filesystem;

namespace {
constexpr size_t kMaxUsername = 32;
constexpr size_t kMaxPassword = 128;
constexpr size_t kMaxContent = 4096;
constexpr size_t kMaxBody = 20 * 1024 * 1024;
constexpr ULONGLONG kPasswordIterations = 600000;
constexpr wchar_t kServiceName[] = L"TTCChatServer";

mutex g_logMutex;
ofstream g_log;
// Ghi thông điệp kèm thời gian vào file log và console.
void Log(const string& message) {
    lock_guard<mutex> lock(g_logMutex);
    SYSTEMTIME now{};
    GetLocalTime(&now);
    ostringstream line;
    line << now.wYear << '-' << now.wMonth << '-' << now.wDay << ' '
         << now.wHour << ':' << now.wMinute << ':' << now.wSecond << ' ' << message;
    if (g_log) { g_log << line.str() << endl; }
    cout << line.str() << endl;
}

// 1. Mật khẩu và token phiên ngẫu nhiên --------------------------------------
// Chuyển dãy byte thành chuỗi hexadecimal để lưu hash, salt hoặc token.
string Hex(const UCHAR* bytes, size_t count) {
    static const char digits[] = "0123456789abcdef";
    string result;
    result.reserve(count * 2);
    for (size_t i = 0; i < count; ++i) {
        result.push_back(digits[bytes[i] >> 4]);
        result.push_back(digits[bytes[i] & 15]);
    }
    return result;
}

// Đổi một ký tự hexadecimal thành giá trị từ 0 đến 15.
int HexDigit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

// Đọc một đoạn chuỗi hexadecimal thành dãy byte và báo lỗi khi định dạng sai.
bool DecodeHex(const string& text, size_t offset, UCHAR* out,
               size_t count) {
    if (offset > text.size() || count > (text.size() - offset) / 2) return false;
    for (size_t i = 0; i < count; ++i) {
        const int high = HexDigit(text[offset + 2 * i]);
        const int low = HexDigit(text[offset + 2 * i + 1]);
        if (high < 0 || low < 0) return false;
        out[i] = static_cast<UCHAR>((high << 4) | low);
    }
    return true;
}

// Tính PBKDF2-SHA256 từ mật khẩu và salt bằng Windows BCrypt.
bool DerivePassword(const string& password, const UCHAR* salt,
                    size_t saltSize, UCHAR* output, size_t outputSize) {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(
            &algorithm, BCRYPT_SHA256_ALGORITHM, nullptr,
            BCRYPT_ALG_HANDLE_HMAC_FLAG))) return false;

    const NTSTATUS status = BCryptDeriveKeyPBKDF2(
        algorithm,
        reinterpret_cast<PUCHAR>(const_cast<char*>(password.data())),
        static_cast<ULONG>(password.size()), const_cast<PUCHAR>(salt),
        static_cast<ULONG>(saltSize), kPasswordIterations, output,
        static_cast<ULONG>(outputSize), 0);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    return BCRYPT_SUCCESS(status);
}

// Tạo salt ngẫu nhiên và chuỗi hash PBKDF2 để lưu mật khẩu trong DB.
string HashPassword(const string& password) {
    if (password.empty() || password.size() > kMaxPassword) return {};
    array<UCHAR, 16> salt{};
    array<UCHAR, 32> hash{};
    if (!BCRYPT_SUCCESS(BCryptGenRandom(nullptr, salt.data(),
            static_cast<ULONG>(salt.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG))) return {};
    if (!DerivePassword(password, salt.data(), salt.size(), hash.data(),
                        hash.size())) return {};
    return "pbkdf2-sha256$" + to_string(kPasswordIterations) + "$" +
           Hex(salt.data(), salt.size()) + "$" + Hex(hash.data(), hash.size());
}

// Tính lại hash với salt đã lưu rồi so sánh để xác nhận mật khẩu.
bool VerifyPassword(const string& password, const string& stored) {
    const string prefix = "pbkdf2-sha256$" +
                               to_string(kPasswordIterations) + "$";
    if (password.empty() || password.size() > kMaxPassword ||
        stored.size() != prefix.size() + 32 + 1 + 64 ||
        stored.compare(0, prefix.size(), prefix) != 0 ||
        stored[prefix.size() + 32] != '$') return false;

    array<UCHAR, 16> salt{};
    array<UCHAR, 32> expected{};
    array<UCHAR, 32> actual{};
    if (!DecodeHex(stored, prefix.size(), salt.data(), salt.size()) ||                                          // lấy phần salt từ chuỗi đã lưu trong DB
        !DecodeHex(stored, prefix.size() + 33, expected.data(), expected.size()) ||                             // lấy phần hash đã lưu trong DB
        !DerivePassword(password, salt.data(), salt.size(), actual.data(),                                      // ính hash từ mật khẩu vừa nhập và salt vừa lấy từ DB lưu vào actual
                        actual.size())) return false;

    // So sánh đủ mọi byte, kể cả khi byte trước đó đã khác.
    unsigned int difference = 0;
    for (size_t i = 0; i < actual.size(); ++i)
        difference |= actual[i] ^ expected[i];
    return difference == 0;
}

// Tạo token phiên ngẫu nhiên từ 32 byte và biểu diễn thành chuỗi hexadecimal.
string NewToken() {
    array<UCHAR, 32> bytes{};
    if (!BCRYPT_SUCCESS(BCryptGenRandom(nullptr, bytes.data(),
            static_cast<ULONG>(bytes.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG))) return {};
    return Hex(bytes.data(), bytes.size());
}

// 2. Các hàm phụ để đọc JSON và kiểm tra dữ liệu đầu vào ---------------------
// Đặt mã HTTP và nội dung JSON vào response trả về client.
void Reply(httplib::Response& res, int status, const json& body) {
    res.status = status;
    res.set_content(body.dump(), "application/json; charset=utf-8");
}

// Trả response JSON có trường error cùng mã HTTP tương ứng.
void Error(httplib::Response& res, int status, const char* message) {
    Reply(res, status, json{{"error", message}});
}

// Phân tích body request thành đối tượng JSON và báo lỗi nếu dữ liệu không hợp lệ.
bool JsonBody(const httplib::Request& req, httplib::Response& res, json& body) {
    body = json::parse(req.body, nullptr, false);
    if (!body.is_discarded() && body.is_object()) return true;
    Error(res, 400, "Cần gửi một đối tượng JSON");
    return false;
}

// Đọc trường chuỗi trong JSON vào biến đầu ra và cho biết có đọc được hay không.
bool StringField(const json& body, const char* name, string& value) {
    auto it = body.find(name);
    if (it == body.end() || !it->is_string()) return false;
    value = it->get<string>();
    return true;
}

// Kiểm tra độ dài và ký tự hợp lệ của tên tài khoản.
bool ValidUsername(const string& name) {
    if (name.empty() || name.size() > kMaxUsername) return false;
    for (unsigned char c : name) {
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_')) return false;
    }
    return true;
}

// Đọc ID nguyên dương từ trường JSON và kiểm tra giới hạn kiểu số.
bool PositiveId(const json& body, const char* name, sqlite3_int64& id) {
    auto it = body.find(name);
    if (it == body.end()) return false;
    if (it->is_number_unsigned()) {
        const auto value = it->get<uint64_t>();
        if (value == 0 || value > static_cast<uint64_t>(INT64_MAX)) return false;
        id = static_cast<sqlite3_int64>(value);
        return true;
    }
    if (!it->is_number_integer()) return false;
    id = it->get<sqlite3_int64>();
    return id > 0;
}

// Chuyển chuỗi chữ số thành số nguyên không âm, từ chối ký tự lạ hoặc tràn số.
bool DecimalId(const string& text, sqlite3_int64& id) {
    if (text.empty()) return false;
    sqlite3_int64 value = 0;
    for (char c : text) {
        if (c < '0' || c > '9') return false;
        const int digit = c - '0';
        if (value > (INT64_MAX - digit) / 10) return false;
        value = value * 10 + digit;
    }
    id = value;
    return true;
}

// Sao chép nội dung cột văn bản SQLite sang string trước khi kết quả truy vấn bị dọn.
string ColumnText(sqlite3_stmt* stmt, int column) {
    const auto* value = sqlite3_column_text(stmt, column);
    if (!value) return {};
    return string(reinterpret_cast<const char*>(value),
                       static_cast<size_t>(sqlite3_column_bytes(stmt, column)));
}

// 3. SQLite: một kết nối DB, được bảo vệ bằng Database::mutex_ --------------
class Statement {
public:
    // Chuẩn bị câu SQL và giữ statement để các hàm DB gắn tham số rồi thực thi.
    Statement(sqlite3* db, const char* sql)
        : status_(sqlite3_prepare_v2(db, sql, -1, &stmt_, nullptr)) {}
    // Giải phóng statement SQLite khi đối tượng truy vấn hết vòng đời.
    ~Statement() { if (stmt_) sqlite3_finalize(stmt_); }
    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;
    // Trả con trỏ statement SQLite để gắn tham số, chạy hoặc đọc kết quả.
    sqlite3_stmt* get() const { return stmt_; }
    // Trả mã kết quả của bước chuẩn bị câu SQL.
    int status() const { return status_; }

private:
    sqlite3_stmt* stmt_ = nullptr;
    int status_ = SQLITE_ERROR;
};

// Gắn bản sao chuỗi vào vị trí tham số của statement SQLite.
int BindText(sqlite3_stmt* stmt, int index, const string& value) {
    return sqlite3_bind_text(stmt, index, value.data(),
                             static_cast<int>(value.size()), SQLITE_TRANSIENT);
}

enum class DbCode { ok, duplicate, notFound, error };

class Database {
public:
    // Đóng kết nối SQLite khi đối tượng Database bị hủy.
    ~Database() { if (db_) sqlite3_close(db_); }

    // Mở/tạo DB, bật khóa ngoại và tạo các bảng/chỉ mục nếu chưa tồn tại.
    bool Open(const string& path, string& error) {
        const int rc = sqlite3_open_v2(path.c_str(), &db_,
            SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, nullptr);
        if (rc != SQLITE_OK) {
            error = db_ ? sqlite3_errmsg(db_) : "Không thể mở cơ sở dữ liệu SQLite";
            return false;
        }
        sqlite3_busy_timeout(db_, 3000);
        const char* schema = R"SQL(
            PRAGMA foreign_keys = ON;
            CREATE TABLE IF NOT EXISTS users (
                id INTEGER PRIMARY KEY,
                username TEXT NOT NULL COLLATE NOCASE UNIQUE,
                password_hash TEXT NOT NULL
            );
            CREATE TABLE IF NOT EXISTS friendships (
                user_low_id INTEGER NOT NULL REFERENCES users(id),
                user_high_id INTEGER NOT NULL REFERENCES users(id),
                created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
                PRIMARY KEY(user_low_id, user_high_id),
                CHECK(user_low_id < user_high_id)
            );
            CREATE TABLE IF NOT EXISTS messages (
                id INTEGER PRIMARY KEY,
                sender_id INTEGER NOT NULL REFERENCES users(id),
                receiver_id INTEGER NOT NULL REFERENCES users(id),
                content TEXT NOT NULL,
                created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP
            );
            CREATE INDEX IF NOT EXISTS messages_sender_receiver_id
                ON messages(sender_id, receiver_id, id);
            CREATE INDEX IF NOT EXISTS messages_receiver_sender_id
                ON messages(receiver_id, sender_id, id);
            CREATE TABLE IF NOT EXISTS attachments (
                message_id INTEGER PRIMARY KEY REFERENCES messages(id),
                filename TEXT NOT NULL, data BLOB NOT NULL
            );
            CREATE TABLE IF NOT EXISTS avatars (
                user_id INTEGER PRIMARY KEY REFERENCES users(id),
                version INTEGER NOT NULL, data BLOB NOT NULL
            );
        )SQL";
        char* sqliteError = nullptr;
        if (sqlite3_exec(db_, schema, nullptr, nullptr, &sqliteError) != SQLITE_OK) {
            error = sqliteError ? sqliteError : sqlite3_errmsg(db_);
            sqlite3_free(sqliteError);
            return false;
        }
        return true;
    }

    // Chèn tài khoản và hash mật khẩu vào DB, phân biệt thành công và trùng tên.
    DbCode Register(const string& username, const string& hash) {
        lock_guard<recursive_mutex> lock(mutex_);
        Statement stmt(db_,
            "INSERT INTO users(username, password_hash) VALUES(?1, ?2);");
        if (stmt.status() != SQLITE_OK ||
            BindText(stmt.get(), 1, username) != SQLITE_OK ||
            BindText(stmt.get(), 2, hash) != SQLITE_OK) return DbCode::error;
        const int rc = sqlite3_step(stmt.get());
        if (rc == SQLITE_DONE) return DbCode::ok;
        if ((rc & 0xff) == SQLITE_CONSTRAINT) return DbCode::duplicate;
        return DbCode::error;
    }

    // Tìm ID và hash mật khẩu đã lưu theo tên tài khoản để phục vụ đăng nhập.
    DbCode Credentials(const string& username, sqlite3_int64& userId,
                       string& storedHash) {
        lock_guard<recursive_mutex> lock(mutex_);
        Statement stmt(db_, "SELECT id, password_hash FROM users WHERE username=?1;");
        if (stmt.status() != SQLITE_OK ||
            BindText(stmt.get(), 1, username) != SQLITE_OK) return DbCode::error;
        const int rc = sqlite3_step(stmt.get());
        if (rc == SQLITE_DONE) return DbCode::notFound;
        if (rc != SQLITE_ROW) return DbCode::error;
        userId = sqlite3_column_int64(stmt.get(), 0);
        storedHash = ColumnText(stmt.get(), 1); // Chép trước khi Statement được dọn.
        return storedHash.empty() ? DbCode::error : DbCode::ok;
    }

    // Đọc danh sách tài khoản và phiên bản ảnh đại diện.
    DbCode Users(json& users) {
        lock_guard<recursive_mutex> lock(mutex_);
        Statement stmt(db_, "SELECT u.id, u.username, COALESCE(a.version,0) FROM users u LEFT JOIN avatars a ON a.user_id=u.id ORDER BY u.id;");
        if (stmt.status() != SQLITE_OK) return DbCode::error;
        users = json::array();
        int rc;
        while ((rc = sqlite3_step(stmt.get())) == SQLITE_ROW) {
            users.push_back({{"id", sqlite3_column_int64(stmt.get(), 0)},
                             {"username", ColumnText(stmt.get(), 1)}, {"avatar_version", sqlite3_column_int64(stmt.get(), 2)}});
        }
        return rc == SQLITE_DONE ? DbCode::ok : DbCode::error;
    }

    // Thay ảnh đại diện và tăng phiên bản để client biết lúc nào cần tải lại.
    DbCode SetAvatar(sqlite3_int64 user, const string& data) {
        lock_guard<recursive_mutex> lock(mutex_);
        Statement stmt(db_, "INSERT INTO avatars(user_id,version,data) VALUES(?1,1,?2) ON CONFLICT(user_id) DO UPDATE SET version=version+1,data=excluded.data");
        if (stmt.status() != SQLITE_OK) return DbCode::error;
        sqlite3_bind_int64(stmt.get(), 1, user);
        sqlite3_bind_blob(stmt.get(), 2, data.data(), static_cast<int>(data.size()), SQLITE_TRANSIENT);
        return sqlite3_step(stmt.get()) == SQLITE_DONE ? DbCode::ok : DbCode::error;
    }

    // Đọc ảnh PNG của một tài khoản để các client đã đăng nhập hiển thị.
    DbCode Avatar(sqlite3_int64 user, string& data) {
        lock_guard<recursive_mutex> lock(mutex_);
        Statement stmt(db_, "SELECT data FROM avatars WHERE user_id=?1");
        if (stmt.status() != SQLITE_OK) return DbCode::error;
        sqlite3_bind_int64(stmt.get(), 1, user);
        const int rc = sqlite3_step(stmt.get());
        if (rc == SQLITE_DONE) return DbCode::notFound;
        if (rc != SQLITE_ROW) return DbCode::error;
        data.assign(static_cast<const char*>(sqlite3_column_blob(stmt.get(), 0)), sqlite3_column_bytes(stmt.get(), 0));
        return DbCode::ok;
    }

    // Ảnh nhóm được lưu riêng; chỉ lớp API kiểm tra trưởng nhóm mới được ghi.
    void SetRoomAvatar(sqlite3_int64 room, const string& data) {
        lock_guard<recursive_mutex> lock(mutex_);
        Statement stmt(db_, "INSERT INTO room_avatars(room_id,version,data) VALUES(?1,1,?2) ON CONFLICT(room_id) DO UPDATE SET version=version+1,data=excluded.data");
        if(stmt.status()!=SQLITE_OK || sqlite3_bind_int64(stmt.get(),1,room)!=SQLITE_OK ||
           sqlite3_bind_blob(stmt.get(),2,data.data(),static_cast<int>(data.size()),SQLITE_TRANSIENT)!=SQLITE_OK || sqlite3_step(stmt.get())!=SQLITE_DONE)
            throw runtime_error("Không lưu được ảnh nhóm");
    }
    string RoomAvatar(sqlite3_int64 room) {
        lock_guard<recursive_mutex> lock(mutex_);
        Statement stmt(db_,"SELECT data FROM room_avatars WHERE room_id=?");
        if(stmt.status()!=SQLITE_OK) throw runtime_error("Không đọc được ảnh nhóm");
        sqlite3_bind_int64(stmt.get(),1,room);
        if(sqlite3_step(stmt.get())!=SQLITE_ROW) return {};
        return string(static_cast<const char*>(sqlite3_column_blob(stmt.get(),0)),sqlite3_column_bytes(stmt.get(),0));
    }

    // Truy vấn có tham số; dùng chung cho dữ liệu hội thoại, thành viên và lời mời.
    json Query(const string& sql, const json& args = json::array()) {
        lock_guard<recursive_mutex> lock(mutex_);
        Statement stmt(db_, sql.c_str());
        if (stmt.status() != SQLITE_OK) throw runtime_error(sqlite3_errmsg(db_));
        int index = 1;
        for (const auto& value : args) {
            int rc;
            if (value.is_string()) rc = BindText(stmt.get(), index, value.get<string>());
            else if (value.is_null()) rc = sqlite3_bind_null(stmt.get(), index);
            else rc = sqlite3_bind_int64(stmt.get(), index, value.get<sqlite3_int64>());
            if (rc != SQLITE_OK) throw runtime_error(sqlite3_errmsg(db_));
            ++index;
        }
        json rows = json::array(); int rc;
        while ((rc = sqlite3_step(stmt.get())) == SQLITE_ROW) {
            json row = json::object();
            for (int col = 0; col < sqlite3_column_count(stmt.get()); ++col) {
                const string name = sqlite3_column_name(stmt.get(), col);
                if (sqlite3_column_type(stmt.get(), col) == SQLITE_INTEGER) row[name] = sqlite3_column_int64(stmt.get(), col);
                else if (sqlite3_column_type(stmt.get(), col) == SQLITE_NULL) row[name] = nullptr;
                else row[name] = ColumnText(stmt.get(), col);
            }
            rows.push_back(std::move(row));
        }
        if (rc != SQLITE_DONE) throw runtime_error(sqlite3_errmsg(db_));
        return rows;
    }

    // Một thay đổi nhiều bảng phải thành công toàn bộ hoặc hoàn tác toàn bộ.
    template<class Action> void Transaction(Action action) {
        lock_guard<recursive_mutex> lock(mutex_);
        Query("BEGIN IMMEDIATE");
        try { action(); Query("COMMIT"); }
        catch (...) { Query("ROLLBACK"); throw; }
    }

    // Nâng DB cũ tại chỗ: giữ ID tin/file, thêm hội thoại và ghép các cặp người dùng.
    void UpgradeConversations() {
        Transaction([&] {
            Query("CREATE TABLE IF NOT EXISTS rooms(id INTEGER PRIMARY KEY,kind TEXT NOT NULL,name TEXT NOT NULL DEFAULT '',owner_id INTEGER REFERENCES users(id),low_id INTEGER REFERENCES users(id),high_id INTEGER REFERENCES users(id),UNIQUE(low_id,high_id))");
            Query("CREATE TABLE IF NOT EXISTS room_avatars(room_id INTEGER PRIMARY KEY REFERENCES rooms(id),version INTEGER NOT NULL,data BLOB NOT NULL)");
            Query("CREATE TABLE IF NOT EXISTS members(room_id INTEGER REFERENCES rooms(id),user_id INTEGER REFERENCES users(id),PRIMARY KEY(room_id,user_id))");
            Query("CREATE TABLE IF NOT EXISTS invitations(id INTEGER PRIMARY KEY,kind TEXT NOT NULL,sender_id INTEGER NOT NULL REFERENCES users(id),receiver_id INTEGER NOT NULL REFERENCES users(id),room_id INTEGER REFERENCES rooms(id),status TEXT NOT NULL DEFAULT 'pending')");
            Query("CREATE UNIQUE INDEX IF NOT EXISTS pending_friend ON invitations(min(sender_id,receiver_id),max(sender_id,receiver_id)) WHERE kind='friend' AND status='pending'");
            Query("CREATE UNIQUE INDEX IF NOT EXISTS pending_group ON invitations(room_id,receiver_id) WHERE kind='group' AND status='pending'");
            bool hasRoom = false;
            for (const auto& col : Query("PRAGMA table_info(messages)")) if (col.at("name") == "room_id") hasRoom = true;
            if (!hasRoom) Query("ALTER TABLE messages ADD COLUMN room_id INTEGER REFERENCES rooms(id)");
            Query("INSERT OR IGNORE INTO rooms(kind,low_id,high_id) SELECT DISTINCT 'direct',min(sender_id,receiver_id),max(sender_id,receiver_id) FROM messages WHERE room_id IS NULL");
            Query("INSERT OR IGNORE INTO members SELECT id,low_id FROM rooms WHERE kind='direct'");
            Query("INSERT OR IGNORE INTO members SELECT id,high_id FROM rooms WHERE kind='direct'");
            Query("UPDATE messages SET room_id=(SELECT id FROM rooms WHERE low_id=min(messages.sender_id,messages.receiver_id) AND high_id=max(messages.sender_id,messages.receiver_id)) WHERE room_id IS NULL");
            Query("CREATE INDEX IF NOT EXISTS messages_room_id ON messages(room_id,id)");
            Query("CREATE INDEX IF NOT EXISTS members_user ON members(user_id,room_id)");
        });
    }

    // Tin và file cùng dùng ID của bảng messages; response/SSE trả chung một cấu trúc.
    json MessageRows(const string& condition, const json& args) {
        return Query("SELECT m.id,m.room_id,m.sender_id,CASE WHEN r.kind='direct' THEN m.receiver_id ELSE NULL END AS receiver_id,m.content,m.created_at,u.username AS sender_name,r.kind,"
            "COALESCE(a.filename,'') AS filename,COALESCE(length(a.data),0) AS file_size "
            "FROM messages m JOIN users u ON u.id=m.sender_id JOIN rooms r ON r.id=m.room_id "
            "LEFT JOIN attachments a ON a.message_id=m.id WHERE " + condition + " ORDER BY m.id LIMIT 100", args);
    }

    // Lưu file hoặc tin vào hội thoại và trả ngay nội dung đã lưu chính thức.
    json StoreMessage(sqlite3_int64 sender, sqlite3_int64 room, const string& content,
                      const string& filename = {}, const string& data = {}) {
        json message;
        Transaction([&] {
            const auto r = Query("SELECT * FROM rooms WHERE id=?", {room}).at(0);
            // Giữ cột receiver_id NOT NULL của DB cũ; với nhóm, room_id mới quyết định nơi nhận.
            const auto receiver = r.at("kind") == "direct" ?
                (r.at("low_id").get<sqlite3_int64>() == sender ? r.at("high_id").get<sqlite3_int64>() : r.at("low_id").get<sqlite3_int64>()) : sender;
            Query("INSERT INTO messages(sender_id,receiver_id,content,room_id) VALUES(?,?,?,?)", {sender,receiver,content,room});
            const auto id = sqlite3_last_insert_rowid(db_);
            if (!filename.empty()) {
                Statement file(db_, "INSERT INTO attachments(message_id,filename,data) VALUES(?,?,?)");
                if (file.status() != SQLITE_OK || sqlite3_bind_int64(file.get(),1,id) != SQLITE_OK ||
                    BindText(file.get(),2,filename) != SQLITE_OK ||
                    sqlite3_bind_blob(file.get(),3,data.data(),static_cast<int>(data.size()),SQLITE_TRANSIENT) != SQLITE_OK ||
                    sqlite3_step(file.get()) != SQLITE_DONE) throw runtime_error("Không lưu được file");
            }
            message = MessageRows("m.id=?", {id}).at(0);
        });
        return message;
    }

    // Chỉ gọi sau khi lớp HTTP đã kiểm tra quyền hội thoại từ bộ nhớ server.
    string FileBytes(sqlite3_int64 id) {
        lock_guard<recursive_mutex> lock(mutex_);
        Statement stmt(db_, "SELECT data FROM attachments WHERE message_id=?");
        if (stmt.status() != SQLITE_OK) throw runtime_error("Không đọc được file");
        sqlite3_bind_int64(stmt.get(),1,id);
        if (sqlite3_step(stmt.get()) != SQLITE_ROW) throw runtime_error("Không tìm thấy file");
        const auto count = sqlite3_column_bytes(stmt.get(),0);
        return count ? string(static_cast<const char*>(sqlite3_column_blob(stmt.get(),0)),count) : string{};
    }

private:
    sqlite3* db_ = nullptr;
    recursive_mutex mutex_;
};

// 4. Phiên đăng nhập và thông báo tin mới theo thời gian thực ----------------
class Sessions {
public:
    // Sinh token mới và lưu phiên gồm userId, thời điểm liên lạc trong RAM.
    bool Create(sqlite3_int64 userId, string& token) {
        for (int attempt = 0; attempt < 3; ++attempt) {
            token = NewToken();
            if (token.empty()) return false;
            lock_guard<mutex> lock(mutex_);
            if (tokens_.emplace(token, Session{userId, chrono::steady_clock::now()}).second) return true;
        }
        return false;
    }

    // Tra token lấy userId và cập nhật lastSeen nếu tham số touch cho phép.
    bool Find(const string& token, sqlite3_int64& userId, bool touch = true) {
        lock_guard<mutex> lock(mutex_);
        const auto it = tokens_.find(token);
        if (it == tokens_.end()) return false;
        userId = it->second.userId;
        if (touch) it->second.lastSeen = chrono::steady_clock::now();
        return true;
    }

    // Gắn online cho mỗi tài khoản nếu còn phiên liên lạc trong 12 giây gần nhất.
    void AddPresence(json& users) {
        lock_guard<mutex> lock(mutex_);
        const auto now = chrono::steady_clock::now();
        for (auto& user : users) {
            bool online = false;
            for (const auto& entry : tokens_) {
                if (entry.second.userId == user.at("id").get<sqlite3_int64>() &&
                    //Phiên này có thuộc người đang được xét không?
                    now - entry.second.lastSeen < chrono::seconds(12)) {
                    online = true;
                    break;
                }
            }
            user["online"] = online;
        }
    }

    // Xóa token đăng xuất khỏi danh sách phiên trong RAM.
    void Remove(const string& token) {
        lock_guard<mutex> lock(mutex_);
        tokens_.erase(token);
    }

private:
    struct Session {
        sqlite3_int64 userId;
        chrono::steady_clock::time_point lastSeen;
    };
    mutex mutex_;
    unordered_map<string, Session> tokens_;
};

class MessageEvents {
public:
    // Tăng phiên bản thay đổi của người dùng và đánh thức các worker SSE đang chờ.
    void Notify(sqlite3_int64 receiver) {
        {
            lock_guard<mutex> lock(mutex_);
            ++versions_[receiver]; // Số phiên bản thay đổi, không phải ID tin nhắn.
        }
        changed_.notify_all(); // đánh thức các luồng đang chờ
    }

    // Lấy phiên bản thay đổi hiện tại của người dùng.
    sqlite3_int64 Current(sqlite3_int64 receiver) {
        lock_guard<mutex> lock(mutex_);
        return versions_[receiver];
    }

    // Chờ phiên bản mới hơn, có lệnh dừng hoặc hết 3 giây rồi trả phiên bản hiện tại.
    sqlite3_int64 Wait(sqlite3_int64 receiver, sqlite3_int64 seen,
                       const atomic<bool>& stopping) {
        unique_lock<mutex> lock(mutex_);
        changed_.wait_for(lock, chrono::seconds(3), [&] {
            return stopping.load() || versions_[receiver] > seen;
        });
        return versions_[receiver];
    }

    // Đánh thức các worker đang chờ trong Wait() để kiểm tra lại điều kiện.
    void WakeAll() { changed_.notify_all(); }

private:
    mutex mutex_;
    condition_variable changed_;
    unordered_map<sqlite3_int64, sqlite3_int64> versions_;
};

struct RequestError : runtime_error {
    int status;
    RequestError(int code, const string& text) : runtime_error(text), status(code) {}
};

// 5. API HTTP: nhận yêu cầu, kiểm tra quyền, gọi DB và trả lời ---------------
class ChatServer {
public:
    // Mở DB, cấu hình server HTTP và đăng ký các handler bằng ConfigureRoutes().
    bool Initialize(const string& dbPath, string& error) {
        if (!database_.Open(dbPath, error)) return false;
        database_.UpgradeConversations();
        ReloadAccess();
        server_.set_payload_max_length(kMaxBody);
        server_.set_read_timeout(5);
        server_.set_write_timeout(5);
        server_.set_keep_alive_timeout(2);
        // Trong bản đơn giản này, mỗi kết nối SSE chiếm một worker.
        server_.new_task_queue = [] { return new httplib::ThreadPool(32); }; // tạo thread
        ConfigureRoutes();
        return true;
    }

    // Tạo socket lắng nghe và gắn nó vào IP/cổng được chỉ định.
    bool Bind(const string& host, int port) { // kiểm tra, chuẩn bị IP:Port trước khi lắng nghe
        return server_.bind_to_port(host, port);
    }
    // Chạy vòng nhận kết nối và phân phối request tới các handler đã đăng ký.
    bool Listen() { return server_.listen_after_bind(); }
    // Bật cờ dừng, đánh thức phần chờ SSE và dừng socket lắng nghe HTTP.
    void Stop() {
        stopping_.store(true);
        events_.WakeAll();
        server_.stop();
    }

private:
    // Kiểm tra Bearer token, lấy danh tính người gọi và trả 401 nếu phiên không hợp lệ.
    bool Authenticate(const httplib::Request& req, httplib::Response& res,
                      sqlite3_int64& userId) {
        const string header = req.get_header_value("Authorization");
        if (header.size() != 7 + 64 || header.compare(0, 7, "Bearer ") != 0 ||
            !sessions_.Find(header.substr(7), userId)) {
            Error(res, 401, "Cần đăng nhập");
            return false;
        }
        return true;
    }


    // Bộ nhớ quyền dùng chung theo userId, nên cập nhật một lần áp dụng cho mọi session.
    void ReloadAccess() {
        access_.clear();
        for (const auto& row : database_.Query("SELECT room_id,user_id FROM members"))
            access_[row.at("user_id").get<sqlite3_int64>()].insert(row.at("room_id").get<sqlite3_int64>());
    }
    void RequireAccess(sqlite3_int64 user, sqlite3_int64 room) {
        if (!access_[user].count(room)) throw RequestError(403,"Bạn không còn là thành viên hội thoại này");
    }
    json RequireOwner(sqlite3_int64 user, sqlite3_int64 room) {
        RequireAccess(user,room);
        auto rows = database_.Query("SELECT * FROM rooms WHERE id=? AND kind='group' AND owner_id=?", {room,user});
        if (rows.empty()) throw RequestError(403,"Chỉ trưởng nhóm được thực hiện thao tác này");
        return rows[0];
    }
    void RequireUser(sqlite3_int64 user) {
        if (database_.Query("SELECT id FROM users WHERE id=?",{user}).empty()) throw RequestError(404,"Không tìm thấy tài khoản");
    }
    static json ReadBody(const httplib::Request& req) {
        auto body = json::parse(req.body,nullptr,false);
        if (!body.is_object()) throw RequestError(400,"JSON không hợp lệ");
        return body;
    }
    static sqlite3_int64 Id(const json& body, const char* key) {
        sqlite3_int64 id=0;
        if (!PositiveId(body,key,id)) throw RequestError(400,string("ID không hợp lệ: ")+key);
        return id;
    }
    // Tạo hội thoại riêng khi cần; không yêu cầu hai người phải kết bạn.
    sqlite3_int64 DirectRoom(sqlite3_int64 user, sqlite3_int64 peer) {
        if (user==peer) throw RequestError(400,"Không thể tự nhắn cho mình");
        RequireUser(peer);
        sqlite3_int64 id=0;
        database_.Transaction([&] {
            database_.Query("INSERT OR IGNORE INTO rooms(kind,low_id,high_id) VALUES('direct',?,?)",{(min)(user,peer),(max)(user,peer)});
            id=database_.Query("SELECT id FROM rooms WHERE low_id=? AND high_id=?",{(min)(user,peer),(max)(user,peer)})[0]["id"].get<sqlite3_int64>();
            database_.Query("INSERT OR IGNORE INTO members VALUES(?,?)",{id,user});
            database_.Query("INSERT OR IGNORE INTO members VALUES(?,?)",{id,peer});
        });
        access_[user].insert(id); access_[peer].insert(id);
        return id;
    }
    // Các bảng nhỏ để client dựng danh sách bạn bè, nhóm và lời mời.
    json Directory(sqlite3_int64 user) {
        return {
            {"version",events_.Current(user)},
            {"friends",database_.Query("SELECT CASE WHEN user_low_id=? THEN user_high_id ELSE user_low_id END AS user_id FROM friendships WHERE user_low_id=? OR user_high_id=?",{user,user,user})},
            {"rooms",database_.Query("SELECT r.id,r.kind,r.name,r.owner_id,COALESCE(a.version,0) AS avatar_version FROM rooms r LEFT JOIN room_avatars a ON a.room_id=r.id JOIN members m ON m.room_id=r.id WHERE m.user_id=? AND r.kind='group' ORDER BY r.id",{user})},
            {"invitations",database_.Query("SELECT i.*,s.username AS sender_name,u.username AS receiver_name,COALESCE(r.name,'') AS room_name FROM invitations i JOIN users s ON s.id=i.sender_id JOIN users u ON u.id=i.receiver_id LEFT JOIN rooms r ON r.id=i.room_id WHERE (i.sender_id=? OR i.receiver_id=?) AND i.status='pending' ORDER BY i.id",{user,user})}
        };
    }
    void NotifyRoom(sqlite3_int64 room) {
        for (const auto& user : access_) if (user.second.count(room)) events_.Notify(user.first);
    }
    // Bao các thay đổi quyền trong cùng khóa với việc kiểm tra request và xây gói SSE.
    void Route(bool post, const string& path,
               function<void(const httplib::Request&,httplib::Response&,sqlite3_int64)> action) {
        auto handler=[this,action](const httplib::Request& req, httplib::Response& res) {
            sqlite3_int64 user=0;
            if (!Authenticate(req,res,user)) return;
            lock_guard<mutex> lock(accessMutex_);
            try { action(req,res,user); }
            catch (const RequestError& e) { Error(res,e.status,e.what()); }
            catch (const json::exception&) { Error(res,400,"Dữ liệu yêu cầu không hợp lệ"); }
            catch (const exception& e) { Log(string("request error: ")+e.what()); Error(res,500,"Không xử lý được yêu cầu"); }
        };
        if(post) server_.Post(path,handler); else server_.Get(path,handler);
    }

    // Gửi lời mời nhóm; người được mời chỉ có quyền sau khi tự chấp nhận.
    void InviteGroup(sqlite3_int64 sender, sqlite3_int64 room, sqlite3_int64 receiver) {
        RequireOwner(sender,room); RequireUser(receiver);
        if (access_[receiver].count(room)) throw RequestError(409,"Người này đã thuộc nhóm");
        if (database_.Query("SELECT 1 FROM friendships WHERE user_low_id=? AND user_high_id=?",{(min)(sender,receiver),(max)(sender,receiver)}).empty())
            throw RequestError(400,"Hãy kết bạn trước khi mời vào nhóm");
        if (!database_.Query("SELECT id FROM invitations WHERE kind='group' AND room_id=? AND receiver_id=? AND status='pending'",{room,receiver}).empty())
            throw RequestError(409,"Đã có lời mời đang chờ");
        database_.Query("INSERT INTO invitations(kind,sender_id,receiver_id,room_id) VALUES('group',?,?,?)",{sender,receiver,room});
    }

    // Xử lý bạn bè và quản lý nhóm; kiểm tra người thực hiện ở server, không dựa vào nút UI.
    json Social(sqlite3_int64 user, const json& b) {
        const auto op=b.at("op").get<string>();
        sqlite3_int64 created=0;
        database_.Transaction([&] {
            if(op=="friend_invite") {
                auto peer=Id(b,"user_id"); RequireUser(peer);
                if(peer==user) throw RequestError(400,"Không thể tự kết bạn");
                if(!database_.Query("SELECT 1 FROM friendships WHERE user_low_id=? AND user_high_id=?",{(min)(user,peer),(max)(user,peer)}).empty()) throw RequestError(409,"Hai người đã là bạn bè");
                if(!database_.Query("SELECT id FROM invitations WHERE kind='friend' AND status='pending' AND ((sender_id=? AND receiver_id=?) OR (sender_id=? AND receiver_id=?))",{user,peer,peer,user}).empty()) throw RequestError(409,"Đã có lời mời kết bạn đang chờ");
                database_.Query("INSERT INTO invitations(kind,sender_id,receiver_id) VALUES('friend',?,?)",{user,peer});
            } else if(op=="invite_accept" || op=="invite_decline" || op=="invite_cancel") {
                auto id=Id(b,"invite_id");
                auto rows=database_.Query("SELECT * FROM invitations WHERE id=? AND status='pending'",{id});
                if(rows.empty()) throw RequestError(409,"Lời mời đã được xử lý hoặc bị hủy");
                auto invite=rows[0];
                if(invite.at(op=="invite_cancel" ? "sender_id":"receiver_id").get<sqlite3_int64>()!=user) throw RequestError(403,"Không được xử lý lời mời này");
                if(op=="invite_accept") {
                    auto sender=invite.at("sender_id").get<sqlite3_int64>();
                    if(invite.at("kind")=="friend") {
                        database_.Query("INSERT OR IGNORE INTO friendships(user_low_id,user_high_id) VALUES(?,?)",{(min)(sender,user),(max)(sender,user)});
                    } else {
                        auto room=invite.at("room_id").get<sqlite3_int64>();
                        RequireOwner(sender,room);
                        database_.Query("INSERT OR IGNORE INTO members VALUES(?,?)",{room,user});
                    }
                }
                database_.Query("UPDATE invitations SET status=? WHERE id=?",{op=="invite_accept"?"accepted":op=="invite_decline"?"declined":"cancelled",id});
            } else if(op=="group_create") {
                auto name=b.at("name").get<string>();
                if(name.empty() || name.size()>128) throw RequestError(400,"Tên nhóm cần từ 1 đến 128 byte UTF-8");
                database_.Query("INSERT INTO rooms(kind,name,owner_id) VALUES('group',?,?)",{name,user});
                created=database_.Query("SELECT last_insert_rowid() AS id")[0]["id"].get<sqlite3_int64>();
                database_.Query("INSERT INTO members VALUES(?,?)",{created,user});
                // Quyền được nạp lại sau commit; lời mời tạo nhóm kiểm tra bạn bè trực tiếp.
                set<sqlite3_int64> peers;
                for(const auto& value:b.value("members",json::array())) {
                    auto peer=value.get<sqlite3_int64>();
                    if(peer==user || !peers.insert(peer).second) continue;
                    RequireUser(peer);
                    if(database_.Query("SELECT 1 FROM friendships WHERE user_low_id=? AND user_high_id=?",{(min)(peer,user),(max)(peer,user)}).empty()) throw RequestError(400,"Chỉ mời bạn bè vào nhóm mới");
                    database_.Query("INSERT INTO invitations(kind,sender_id,receiver_id,room_id) VALUES('group',?,?,?)",{user,peer,created});
                }
            } else {
                auto room=Id(b,"room_id");
                if(op=="group_leave") {
                    RequireAccess(user,room);
                    auto rows=database_.Query("SELECT owner_id FROM rooms WHERE id=? AND kind='group'",{room});
                    if(rows.empty()) throw RequestError(400,"Đây không phải nhóm");
                    if(rows[0]["owner_id"]==user) throw RequestError(409,"Cần chuyển trưởng nhóm hoặc giải tán trước khi rời");
                    database_.Query("DELETE FROM members WHERE room_id=? AND user_id=?",{room,user});
                } else {
                    RequireOwner(user,room);
                    if(op=="group_invite") InviteGroup(user,room,Id(b,"user_id"));
                    else if(op=="group_kick") {
                        auto peer=Id(b,"user_id");
                        if(peer==user) throw RequestError(400,"Không thể tự xóa trưởng nhóm");
                        RequireAccess(peer,room);
                        database_.Query("DELETE FROM members WHERE room_id=? AND user_id=?",{room,peer});
                    } else if(op=="group_transfer") {
                        auto peer=Id(b,"user_id"); RequireAccess(peer,room);
                        if(peer==user) throw RequestError(400,"Người này đã là trưởng nhóm");
                        database_.Query("UPDATE rooms SET owner_id=? WHERE id=?",{peer,room});
                        database_.Query("UPDATE invitations SET status='cancelled' WHERE room_id=? AND status='pending'",{room});
                    } else if(op=="group_rename") {
                        auto name=b.at("name").get<string>();
                        if(name.empty() || name.size()>128) throw RequestError(400,"Tên nhóm không hợp lệ");
                        database_.Query("UPDATE rooms SET name=? WHERE id=?",{name,room});
                    } else if(op=="group_delete") {
                        database_.Query("DELETE FROM members WHERE room_id=?",{room});
                        database_.Query("UPDATE rooms SET owner_id=NULL WHERE id=?",{room});
                        database_.Query("UPDATE invitations SET status='cancelled' WHERE room_id=? AND status='pending'",{room});
                    } else throw RequestError(400,"Thao tác không hợp lệ");
                }
            }
        });
        ReloadAccess();
        for(const auto& account:database_.Query("SELECT id FROM users")) events_.Notify(account.at("id").get<sqlite3_int64>());
        return {{"directory",Directory(user)},{"room_id",created}};
    }

    // Đăng ký các URL HTTP và hàm xử lý tài khoản, danh sách người dùng, tin nhắn và SSE.
    void ConfigureRoutes() {
        server_.Get("/health", [](const httplib::Request&, httplib::Response& res) {
            Reply(res, 200, json{{"ok", true}});
        });

        server_.Post("/register", [this](const httplib::Request& req,
                                          httplib::Response& res) {
            json body;
            if (!JsonBody(req, res, body)) return;
            string username, password;
            if (!StringField(body, "username", username) ||
                !StringField(body, "password", password) ||
                !ValidUsername(username) || password.empty() ||
                password.size() > kMaxPassword) {
                Error(res, 400, "Tên tài khoản hoặc mật khẩu không hợp lệ");
                return;
            }
            const string hash = HashPassword(password);
            if (hash.empty()) {
                Error(res, 500, "Không thể băm mật khẩu");
                return;
            }
            const DbCode result = database_.Register(username, hash);
            if (result == DbCode::ok) Reply(res, 201, json{{"ok", true}});
            else if (result == DbCode::duplicate)
                Error(res, 409, "Tên tài khoản đã tồn tại");
            else Error(res, 500, "Lỗi cơ sở dữ liệu");
        });

        server_.Post("/login", [this](const httplib::Request& req,
                                       httplib::Response& res) {
            json body;
            if (!JsonBody(req, res, body)) return;
            string username, password;
            if (!StringField(body, "username", username) ||
                !StringField(body, "password", password) ||
                !ValidUsername(username) || password.empty() ||
                password.size() > kMaxPassword) {
                Error(res, 400, "Tên tài khoản hoặc mật khẩu không hợp lệ");
                return;
            }
            sqlite3_int64 userId = 0;
            string storedHash;
            const DbCode result = database_.Credentials(username, userId, storedHash); //tìm tài khoản và lấy hash password
            if (result == DbCode::error) {
                Error(res, 500, "Lỗi cơ sở dữ liệu");
                return;
            }
            if (result == DbCode::notFound ||
                !VerifyPassword(password, storedHash)) { // xác ,minh lại pw
                Error(res, 401, "Tên tài khoản hoặc mật khẩu không đúng");
                return;
            }
            string token;
            if (!sessions_.Create(userId, token)) {
                Error(res, 500, "Không thể tạo phiên đăng nhập");
                return;
            }
            Reply(res, 200, json{{"token", token}, {"user_id", userId}});
            Log("login user=" + to_string(userId));
        });

        server_.Post("/logout", [this](const httplib::Request& req,
                                        httplib::Response& res) {
            sqlite3_int64 userId = 0;
            if (!Authenticate(req, res, userId)) return;
            sessions_.Remove(req.get_header_value("Authorization").substr(7));
            Reply(res, 200, json{{"ok", true}});
        });

        server_.Get("/users", [this](const httplib::Request& req,
                                     httplib::Response& res) {
            sqlite3_int64 userId = 0;
            if (!Authenticate(req, res, userId)) return;
            json users;
            if (database_.Users(users) != DbCode::ok)
                Error(res, 500, "Lỗi cơ sở dữ liệu");
            else {
                sessions_.AddPresence(users);
                Reply(res, 200, json{{"users", users}});
            }
        });


        Route(false,"/directory",[this](const httplib::Request&,httplib::Response& res,sqlite3_int64 user) {
            Reply(res,200,Directory(user));
        });
        Route(true,"/social",[this](const httplib::Request& req,httplib::Response& res,sqlite3_int64 user) {
            Reply(res,200,Social(user,ReadBody(req)));
        });
        Route(false,"/members",[this](const httplib::Request& req,httplib::Response& res,sqlite3_int64 user) {
            sqlite3_int64 room=0;
            if(!DecimalId(req.get_param_value("room_id"),room)) throw RequestError(400,"ID nhóm không hợp lệ");
            RequireAccess(user,room);
            Reply(res,200,{{"room_id",room},{"members",database_.Query("SELECT u.id,u.username,(r.owner_id=u.id) AS owner FROM members m JOIN users u ON u.id=m.user_id JOIN rooms r ON r.id=m.room_id WHERE m.room_id=? ORDER BY owner DESC,u.id",{room})}});
        });
        Route(true,"/messages",[this](const httplib::Request& req,httplib::Response& res,sqlite3_int64 user) {
            auto b=ReadBody(req); string content;
            if(!StringField(b,"content",content) || content.empty() || content.size()>kMaxContent) throw RequestError(400,"Nội dung không hợp lệ");
            auto room=b.contains("room_id") ? Id(b,"room_id") : DirectRoom(user,Id(b,"receiver_id"));
            RequireAccess(user,room);
            auto message=database_.StoreMessage(user,room,content);
            NotifyRoom(room);
            Reply(res,201,message);
        });
        Route(false,"/messages",[this](const httplib::Request& req,httplib::Response& res,sqlite3_int64 user) {
            sqlite3_int64 room=0,after=0,peer=0;
            if(req.has_param("room_id")) {
                if(!DecimalId(req.get_param_value("room_id"),room) || room<=0) throw RequestError(400,"ID hội thoại không hợp lệ");
            } else {
                if(!DecimalId(req.get_param_value("peer_id"),peer) || peer<=0) throw RequestError(400,"ID người dùng không hợp lệ");
                room=DirectRoom(user,peer);
            }
            RequireAccess(user,room);
            if(req.has_param("after_id") && !DecimalId(req.get_param_value("after_id"),after)) throw RequestError(400,"Mốc lịch sử không hợp lệ");
            auto messages=database_.MessageRows("m.room_id=? AND m.id>?",{room,after});
            Reply(res,200,{{"messages",messages},{"room_id",room}});
        });
        Route(true,"/files",[this](const httplib::Request& req,httplib::Response& res,sqlite3_int64 user) {
            sqlite3_int64 room=0,peer=0;
            if(req.has_param("room_id")) {
                if(!DecimalId(req.get_param_value("room_id"),room) || room<=0) throw RequestError(400,"ID hội thoại không hợp lệ");
            } else {
                if(!DecimalId(req.get_param_value("receiver_id"),peer) || peer<=0) throw RequestError(400,"ID người nhận không hợp lệ");
                room=DirectRoom(user,peer);
            }
            RequireAccess(user,room);
            auto name=json::parse(req.get_header_value("X-File-Name"),nullptr,false);
            if(!name.is_string()) throw RequestError(400,"Tên file không hợp lệ");
            const auto filename=name.get<string>();
            if(filename.empty() || filename.size()>255 || filename=="." || filename==".." || filename.find_first_of("/\\:")!=string::npos ||
               any_of(filename.begin(),filename.end(),[](unsigned char c){return c<32;})) throw RequestError(400,"Tên file không hợp lệ");
            auto message=database_.StoreMessage(user,room,"",filename,req.body);
            NotifyRoom(room);
            Reply(res,201,message);
        });
        Route(false,R"(/files/(\d+))",[this](const httplib::Request& req,httplib::Response& res,sqlite3_int64 user) {
            sqlite3_int64 id=0;
            if(!DecimalId(req.matches[1].str(),id) || id<=0) throw RequestError(400,"ID file không hợp lệ");
            auto row=database_.Query("SELECT m.room_id FROM messages m JOIN attachments a ON a.message_id=m.id WHERE m.id=?",{id});
            if(row.empty()) throw RequestError(404,"Không tìm thấy file");
            RequireAccess(user,row[0]["room_id"].get<sqlite3_int64>());
            res.set_header("Cache-Control","no-store");
            res.set_content(database_.FileBytes(id),"application/octet-stream");
        });

        Route(true,R"(/groups/(\d+)/avatar)",[this](const httplib::Request& req,httplib::Response& res,sqlite3_int64 user) {
            sqlite3_int64 room=0;
            if(!DecimalId(req.matches[1].str(),room)) throw RequestError(400,"ID nhóm không hợp lệ");
            RequireOwner(user,room);
            const auto& b=req.body;
            if(b.size()<24 || b.size()>2*1024*1024 || b.compare(0,8,"\x89PNG\r\n\x1a\n",8) ||
               b.compare(12,4,"IHDR") || b.substr(16,8)!=string("\0\0\0\x60\0\0\0\x60",8)) throw RequestError(400,"Ảnh cần là PNG 96x96");
            database_.SetRoomAvatar(room,b);NotifyRoom(room);
            Reply(res,200,{{"directory",Directory(user)}});
        });
        Route(false,R"(/groups/(\d+)/avatar)",[this](const httplib::Request& req,httplib::Response& res,sqlite3_int64 user) {
            sqlite3_int64 room=0;
            if(!DecimalId(req.matches[1].str(),room)) throw RequestError(400,"ID nhóm không hợp lệ");
            RequireAccess(user,room);
            auto image=database_.RoomAvatar(room);
            if(image.empty()) throw RequestError(404,"Nhóm chưa có ảnh");
            res.set_header("Cache-Control","no-store");res.set_content(std::move(image),"image/png");
        });
        server_.Post("/avatar", [this](const httplib::Request& req, httplib::Response& res) {
            sqlite3_int64 user = 0;
            if (!Authenticate(req, res, user)) return;
            // Client chuẩn hóa ảnh thành PNG 96x96; giới hạn cả byte và kích thước ảnh.
            const auto& b = req.body; // nên lưu thành vector thay vì string
            if (b.size() < 24 || b.size() > 2 * 1024 * 1024 || b.compare(0, 8, "\x89PNG\r\n\x1a\n", 8) ||
                b.compare(12, 4, "IHDR") || b.substr(16, 8) != string("\0\0\0\x60\0\0\0\x60", 8)) {
                Error(res, 400, "Ảnh cần là PNG 96x96"); return;
            }
            if (database_.SetAvatar(user, b) != DbCode::ok) { Error(res, 500, "Không lưu được ảnh"); return; }
            Reply(res, 200, json{{"ok", true}});
        });
        server_.Get(R"(/avatars/(\d+))", [this](const httplib::Request& req, httplib::Response& res) {
            sqlite3_int64 user = 0, id = 0;
            if (!Authenticate(req, res, user)) return;
            if (!DecimalId(req.matches[1].str(), id) || id <= 0) { Error(res, 400, "ID không hợp lệ"); return; }
            string data;
            const auto rc = database_.Avatar(id, data);
            if (rc != DbCode::ok) { Error(res, rc == DbCode::notFound ? 404 : 500, "Không tìm thấy ảnh"); return; }
            res.set_header("Cache-Control", "no-store");
            res.set_content(std::move(data), "image/png");
        });


        // SSE truyền đủ tin mới. Mốc tin và phiên bản thay đổi quyền là hai giá trị riêng.
        server_.Get("/events",[this](const httplib::Request& req,httplib::Response& res) {
            sqlite3_int64 user=0;
            if(!Authenticate(req,res,user)) return;
            auto token=req.get_header_value("Authorization").substr(7);
            sqlite3_int64 cursor=0,version=0;
            {
                lock_guard<mutex> lock(accessMutex_);
                cursor=database_.Query("SELECT COALESCE(max(id),0) AS id FROM messages")[0]["id"].get<sqlite3_int64>();
                // cursor  : ID tin nhắn đã đi qua
                version=events_.Current(user); // version : Số lần có thay đổi liên quan đến người dùng, để biết có cần cập nhật không
            }
            res.set_header("Cache-Control","no-cache");
            res.set_chunked_content_provider("text/event-stream",
                [this,user,token,cursor,version,first=true,more=false](size_t,httplib::DataSink& sink) mutable {
                    sqlite3_int64 authenticated=0;
                    if(stopping_ || !sessions_.Find(token,authenticated,false)) return false;
                    try {
                        string chunk;
                        if(first) {
                            first=false;
                            lock_guard<mutex> lock(accessMutex_);
                            chunk="event: ready\ndata: "+json{{"directory",Directory(user)}}.dump()+"\n\n";
                        } else {
                            const auto latest=more ? events_.Current(user) : events_.Wait(user,version,stopping_);
                            if(stopping_) return false;
                            if(latest!=version || more) {
                                lock_guard<mutex> lock(accessMutex_);
                                auto messages=database_.MessageRows("m.id>? AND EXISTS(SELECT 1 FROM members x WHERE x.room_id=m.room_id AND x.user_id=?)",{cursor,user});
                                if(!messages.empty()) cursor=messages.back().at("id").get<sqlite3_int64>();
                                more=messages.size()==100;
                                version=latest;
                                chunk="event: update\ndata: "+json{{"messages",messages},{"directory",Directory(user)}}.dump()+"\n\n";
                            } else chunk=": keepalive\n\n";
                        }
                        return sink.write(chunk.data(),chunk.size());
                    } catch(const exception& e) { Log(string("SSE: ")+e.what()); return false; }
                });
        });
    }

    mutex accessMutex_;
    unordered_map<sqlite3_int64,set<sqlite3_int64>> access_;
                                                                                                                                // access_[2] = { 10, 15 }  → người ID 2 thuộc hội thoại 10 và 15.
                                                                                                                                // access_[3] = { 10 }      → người ID 3 thuộc hội thoại 10.
    Database database_;
    Sessions sessions_;
    MessageEvents events_;
    atomic<bool> stopping_{false};
    httplib::Server server_; // Hủy trước các đối tượng mà callback đang dùng.
};

// 6. Cùng một server chạy ở console hoặc dưới dạng Windows Service ----------
struct Options {
    bool service = false;
    string host = "0.0.0.0";
    int port = 8080;
    fs::path dbPath;
};

Options g_options;
atomic<ChatServer*> g_activeServer{nullptr}; 
mutex g_serverMutex;
atomic<bool> g_stopRequested{false};
SERVICE_STATUS_HANDLE g_serviceHandle = nullptr;

// Báo trạng thái hiện tại cùng mã lỗi cho Service Control Manager của Windows.
void ReportServiceStatus(DWORD state, DWORD error = NO_ERROR) {
    if (!g_serviceHandle) return;
    SERVICE_STATUS status{};
    status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    status.dwCurrentState = state;
    status.dwControlsAccepted = state == SERVICE_RUNNING ? SERVICE_ACCEPT_STOP : 0;
    status.dwWin32ExitCode = error;
    status.dwServiceSpecificExitCode = error == ERROR_SERVICE_SPECIFIC_ERROR ? 1 : 0;
    status.dwWaitHint = state == SERVICE_START_PENDING ||
                        state == SERVICE_STOP_PENDING ? 30000 : 0;
    SetServiceStatus(g_serviceHandle, &status);
}

// Callback : phần thân hàm tùy ý, nhưng chữ ký của hàm phải đúng mẫu callback mà Windows yêu cầu.
// Nhận lệnh Stop từ Windows, báo STOP_PENDING và gọi ChatServer::Stop().
DWORD WINAPI ServiceControl(DWORD control, DWORD, LPVOID, LPVOID) { // Dừng khi chạy qua service sc.exe
    if (control == SERVICE_CONTROL_STOP) {
        lock_guard<mutex> lock(g_serverMutex);
        g_stopRequested.store(true); // g_stopRequested = true;
        ReportServiceStatus(SERVICE_STOP_PENDING);
        if (ChatServer* server = g_activeServer.load()) server->Stop();
    }
    return NO_ERROR;
}

// Nhận Ctrl+C hoặc lệnh đóng console và yêu cầu server dừng.
BOOL WINAPI ConsoleControl(DWORD control) { // Ctrl+c khi chạy console, cũng để dùng chương trình
    if (control == CTRL_C_EVENT || control == CTRL_BREAK_EVENT ||
        control == CTRL_CLOSE_EVENT) {
        g_stopRequested.store(true);
        lock_guard<mutex> lock(g_serverMutex);
        if (ChatServer* server = g_activeServer.load()) server->Stop();
        return TRUE;
    }
    return FALSE;
}

// Mở DB/log, khởi tạo server, gắn IP/cổng rồi chạy vòng lắng nghe.
int RunServer(const Options& options) {
    ChatServer server;
    string error;
    const string dbPath = options.dbPath.u8string();
    g_log.open(options.dbPath.parent_path() / L"server.log", ios::app);
    Log("starting db=" + dbPath);
    if (!server.Initialize(dbPath, error)) {
        cerr << "Cơ sở dữ liệu: " << error << '\n';
        return 1;
    }
    if (!server.Bind(options.host, options.port)) {
        cerr << "Không thể lắng nghe tại " << options.host << ':' << options.port << '\n';
        return 1;
    }

    // Guard dọn con trỏ trước khi server bị hủy, kể cả khi Listen ném exception.
    struct ActiveGuard {
        // Xóa con trỏ server đang hoạt động trước khi server bị hủy, kể cả khi có exception.
        ~ActiveGuard() {
            lock_guard<mutex> lock(g_serverMutex);
            g_activeServer.store(nullptr);
        }
    } activeGuard;
    {
        lock_guard<mutex> lock(g_serverMutex);
        g_activeServer.store(&server);
        if (g_stopRequested.load()) {
            server.Stop();
            return 0;
        }
        if (options.service) ReportServiceStatus(SERVICE_RUNNING);
    }
    if (!options.service) cout << "Server HTTP đang nghe tại http://" << options.host << ':'
                   << options.port << "  DB: " << dbPath << '\n';

    const bool ok = server.Listen();
    Log("stopped");
    return ok || g_stopRequested.load() ? 0 : 1;
}

// Đăng ký handler điều khiển, chạy server và báo trạng thái kết thúc cho Windows.
void WINAPI ServiceMain(DWORD, LPWSTR*) {
    g_serviceHandle = RegisterServiceCtrlHandlerExW(kServiceName,
                                                     ServiceControl, nullptr);
    if (!g_serviceHandle) return;
    ReportServiceStatus(SERVICE_START_PENDING);
    int result = 1;
    try { result = RunServer(g_options); }
    catch (const exception& ex) { Log(string("service error: ") + ex.what()); }
    ReportServiceStatus(SERVICE_STOPPED,
                        result == 0 ? NO_ERROR : ERROR_SERVICE_SPECIFIC_ERROR);
}

// Đọc tham số dòng lệnh và xác định IP, cổng, chế độ service cùng đường dẫn DB.
bool ReadOptions(int argc, char* argv[], Options& options) {
    for (int i = 1; i < argc; ++i) {
        const string arg = argv[i];
        if (arg == "--service") {
            options.service = true;
        } else if (arg == "--host" && i + 1 < argc) {
            options.host = argv[++i];
        } else if (arg == "--db" && i + 1 < argc) {
            options.dbPath = fs::absolute(fs::u8path(argv[++i]));
        } else if (arg == "--port" && i + 1 < argc) {
            sqlite3_int64 port = 0;
            if (!DecimalId(argv[++i], port) || port < 1 || port > 65535)
                return false;
            options.port = static_cast<int>(port);
        } else {
            return false;
        }
    }
    if (options.host.empty()) return false;
    if (options.dbPath.empty()) {
        wchar_t modulePath[MAX_PATH]{};
        const DWORD length = GetModuleFileNameW(nullptr, modulePath, MAX_PATH);
        if (length == 0 || length >= MAX_PATH) return false;
        options.dbPath = fs::path(modulePath).parent_path() / L"chat.db";
    }
    return true;
}
} // Kết thúc vùng tên nội bộ.

// Chuyển tham số sang UTF-8 và chọn chạy console hoặc thông qua Windows service.
int wmain(int argc, wchar_t* wideArgv[]) {
    // Chuỗi trong mã nguồn là UTF-8; console cũng cần đọc các byte đó theo UTF-8.
    SetConsoleOutputCP(CP_UTF8);
    try {
        vector<string> arguments;
        vector<char*> argv;
        arguments.reserve(argc);
        argv.reserve(argc);
        for (int i = 0; i < argc; ++i) {
            const int size = WideCharToMultiByte(CP_UTF8, 0, wideArgv[i], -1, nullptr, 0, nullptr, nullptr);
            string value(size, '\0');
            WideCharToMultiByte(CP_UTF8, 0, wideArgv[i], -1, value.data(), size, nullptr, nullptr);
            value.pop_back();
            arguments.push_back(std::move(value));
        }
        for (auto& argument : arguments) argv.push_back(argument.data());
        if (!ReadOptions(argc, argv.data(), g_options)) {
            cerr << "Cách dùng: ChatServer.exe [--service] [--host IP] "
                         "[--port 8080] [--db DUONG_DAN_TUYET_DOI]\n";
            return 2;
        }
        if (g_options.service) {
            wchar_t name[] = L"TTCChatServer";
            SERVICE_TABLE_ENTRYW table[] = {{name, ServiceMain}, {nullptr, nullptr}};
            if (!StartServiceCtrlDispatcherW(table)) {
                cerr << "Lỗi bộ điều phối Service: " << GetLastError() << '\n';
                return 1;
            }
            return 0;
        }
        SetConsoleCtrlHandler(ConsoleControl, TRUE);
        return RunServer(g_options);
    } catch (const exception& ex) {
        cerr << "Lỗi nghiêm trọng: " << ex.what() << '\n';
        return 1;
    }
}



// ChatMessage8 - Windows Service (các lệnh TTCWeek5Echo phía trên là bài tuần 5).
// Chạy các lệnh dưới đây trong PowerShell mở bằng Run as administrator.
// Dùng cổng 18080: cổng 8080 đã bị Windows dành riêng trên máy này.
//
// 1. Đăng ký lần đầu (chỉ chạy một lần):
// sc.exe create TTCChatServer binPath= "D:\DEV\DEVing\TTC\ChatMessage8\bin\Release\ChatServer.exe --service --host 0.0.0.0 --port 18080 --db D:\DEV\DEVing\TTC\ChatMessage8\bin\Release\chat.db" start= demand
//
// Nếu TTCChatServer đã tồn tại: Stop, đợi query hiện STOPPED rồi cập nhật cấu hình.
// sc.exe stop TTCChatServer
// sc.exe query TTCChatServer
// sc.exe config TTCChatServer binPath= "D:\DEV\DEVing\TTC\ChatMessage8\bin\Release\ChatServer.exe --service --host 0.0.0.0 --port 18080 --db D:\DEV\DEVing\TTC\ChatMessage8\bin\Release\chat.db" start= demand
//
// 2. Mở firewall cho máy khác kết nối (chỉ thêm một lần):
// New-NetFirewallRule -DisplayName "TTC Chat TCP 18080" -Direction Inbound -Action Allow -Protocol TCP -LocalPort 18080 -Profile Any -RemoteAddress LocalSubnet
//
// 3. Khởi chạy và kiểm tra:
// sc.exe start TTCChatServer
// sc.exe query TTCChatServer
// Invoke-RestMethod http://127.0.0.1:18080/health
// Kết quả cần thấy: STATE = RUNNING, health trả ok = True.
//
// 4. Những lần chạy sau: chỉ Start nếu service đang STOPPED, không cần Create lại.
// sc.exe start TTCChatServer
// sc.exe query TTCChatServer
// Muốn dừng:
// sc.exe stop TTCChatServer
// Muốn tự chạy khi Windows khởi động:
// sc.exe config TTCChatServer start= auto
//
// Khi build/thay ChatServer.exe: Stop -> đợi STOPPED -> build/thay file -> Start.
// Không chạy thêm bản console trên cùng cổng khi service đang RUNNING.
// Đổi vị trí EXE/DB hoặc cổng: Stop rồi dùng sc.exe config với đường dẫn/cổng mới.
// DB: D:\DEV\DEVing\TTC\ChatMessage8\bin\Release\chat.db
// Log: D:\DEV\DEVing\TTC\ChatMessage8\bin\Release\server.log
// client.ini cạnh ChatClient.exe: [Server], IP=<IP máy server>, Port=18080.
// Client cùng máy dùng IP=127.0.0.1; máy khác dùng IP máy server từ ipconfig.
// Đóng/mở lại client sau khi sửa client.ini.
