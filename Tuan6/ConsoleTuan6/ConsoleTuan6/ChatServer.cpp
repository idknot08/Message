// Server chat tuần 6. Xem README.md để biết vị trí các hàm và ví dụ gọi API.
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
#include <limits>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>

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
constexpr size_t kMaxBody = 64 * 1024;
constexpr ULONGLONG kPasswordIterations = 600000;
constexpr wchar_t kServiceName[] = L"TTCChatServer";

// 1. Mật khẩu và token phiên ngẫu nhiên --------------------------------------
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

int HexDigit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

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
    if (!DecodeHex(stored, prefix.size(), salt.data(), salt.size()) ||
        !DecodeHex(stored, prefix.size() + 33, expected.data(), expected.size()) ||
        !DerivePassword(password, salt.data(), salt.size(), actual.data(),
                        actual.size())) return false;

    // So sánh đủ mọi byte, kể cả khi byte trước đó đã khác.
    unsigned int difference = 0;
    for (size_t i = 0; i < actual.size(); ++i)
        difference |= actual[i] ^ expected[i];
    return difference == 0;
}

string NewToken() {
    array<UCHAR, 32> bytes{};
    if (!BCRYPT_SUCCESS(BCryptGenRandom(nullptr, bytes.data(),
            static_cast<ULONG>(bytes.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG))) return {};
    return Hex(bytes.data(), bytes.size());
}

// 2. Các hàm phụ để đọc JSON và kiểm tra dữ liệu đầu vào ---------------------
void Reply(httplib::Response& res, int status, const json& body) {
    res.status = status;
    res.set_content(body.dump(), "application/json; charset=utf-8");
}

void Error(httplib::Response& res, int status, const char* message) {
    Reply(res, status, json{{"error", message}});
}

bool JsonBody(const httplib::Request& req, httplib::Response& res, json& body) {
    body = json::parse(req.body, nullptr, false);
    if (!body.is_discarded() && body.is_object()) return true;
    Error(res, 400, "Cần gửi một đối tượng JSON");
    return false;
}

bool StringField(const json& body, const char* name, string& value) {
    auto it = body.find(name);
    if (it == body.end() || !it->is_string()) return false;
    value = it->get<string>();
    return true;
}

bool ValidUsername(const string& name) {
    if (name.empty() || name.size() > kMaxUsername) return false;
    for (unsigned char c : name) {
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_')) return false;
    }
    return true;
}

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

string ColumnText(sqlite3_stmt* stmt, int column) {
    const auto* value = sqlite3_column_text(stmt, column);
    if (!value) return {};
    return string(reinterpret_cast<const char*>(value),
                       static_cast<size_t>(sqlite3_column_bytes(stmt, column)));
}

// 3. SQLite: một kết nối DB, được bảo vệ bằng Database::mutex_ --------------
class Statement {
public:
    Statement(sqlite3* db, const char* sql)
        : status_(sqlite3_prepare_v2(db, sql, -1, &stmt_, nullptr)) {}
    ~Statement() { if (stmt_) sqlite3_finalize(stmt_); }
    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;
    sqlite3_stmt* get() const { return stmt_; }
    int status() const { return status_; }

private:
    sqlite3_stmt* stmt_ = nullptr;
    int status_ = SQLITE_ERROR;
};

int BindText(sqlite3_stmt* stmt, int index, const string& value) {
    return sqlite3_bind_text(stmt, index, value.data(),
                             static_cast<int>(value.size()), SQLITE_TRANSIENT);
}

enum class DbCode { ok, duplicate, notFound, error };

class Database {
public:
    ~Database() { if (db_) sqlite3_close(db_); }

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
        )SQL";
        char* sqliteError = nullptr;
        if (sqlite3_exec(db_, schema, nullptr, nullptr, &sqliteError) != SQLITE_OK) {
            error = sqliteError ? sqliteError : sqlite3_errmsg(db_);
            sqlite3_free(sqliteError);
            return false;
        }
        return true;
    }

    DbCode Register(const string& username, const string& hash) {
        lock_guard<mutex> lock(mutex_);
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

    DbCode Credentials(const string& username, sqlite3_int64& userId,
                       string& storedHash) {
        lock_guard<mutex> lock(mutex_);
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

    DbCode Users(json& users) {
        lock_guard<mutex> lock(mutex_);
        Statement stmt(db_, "SELECT id, username FROM users ORDER BY id LIMIT 100;");
        if (stmt.status() != SQLITE_OK) return DbCode::error;
        users = json::array();
        int rc;
        while ((rc = sqlite3_step(stmt.get())) == SQLITE_ROW) {
            users.push_back({{"id", sqlite3_column_int64(stmt.get(), 0)},
                             {"username", ColumnText(stmt.get(), 1)}});
        }
        return rc == SQLITE_DONE ? DbCode::ok : DbCode::error;
    }

    DbCode AddFriend(sqlite3_int64 current, sqlite3_int64 other) {
        lock_guard<mutex> lock(mutex_);
        Statement exists(db_, "SELECT 1 FROM users WHERE id=?1;");
        if (exists.status() != SQLITE_OK ||
            sqlite3_bind_int64(exists.get(), 1, other) != SQLITE_OK)
            return DbCode::error;
        const int found = sqlite3_step(exists.get());
        if (found == SQLITE_DONE) return DbCode::notFound;
        if (found != SQLITE_ROW) return DbCode::error;

        Statement stmt(db_, "INSERT INTO friendships(user_low_id,user_high_id) "
                             "VALUES(?1,?2);");
        if (stmt.status() != SQLITE_OK ||
            sqlite3_bind_int64(stmt.get(), 1, (min)(current, other)) != SQLITE_OK ||
            sqlite3_bind_int64(stmt.get(), 2, (max)(current, other)) != SQLITE_OK)
            return DbCode::error;
        const int rc = sqlite3_step(stmt.get());
        if (rc == SQLITE_DONE) return DbCode::ok;
        if ((rc & 0xff) == SQLITE_CONSTRAINT) return DbCode::duplicate;
        return DbCode::error;
    }

    DbCode Friends(sqlite3_int64 current, json& friends) {
        lock_guard<mutex> lock(mutex_);
        Statement stmt(db_,
            "SELECT u.id, u.username FROM friendships f JOIN users u "
            "ON u.id=CASE WHEN f.user_low_id=?1 THEN f.user_high_id "
            "ELSE f.user_low_id END "
            "WHERE f.user_low_id=?1 OR f.user_high_id=?1 ORDER BY u.id;");
        if (stmt.status() != SQLITE_OK ||
            sqlite3_bind_int64(stmt.get(), 1, current) != SQLITE_OK) return DbCode::error;
        friends = json::array();
        int rc;
        while ((rc = sqlite3_step(stmt.get())) == SQLITE_ROW) {
            friends.push_back({{"id", sqlite3_column_int64(stmt.get(), 0)},
                               {"username", ColumnText(stmt.get(), 1)}});
        }
        return rc == SQLITE_DONE ? DbCode::ok : DbCode::error;
    }

    DbCode SendMessage(sqlite3_int64 sender, sqlite3_int64 receiver,
                       const string& content, sqlite3_int64& messageId) {
        lock_guard<mutex> lock(mutex_);
        // SELECT kiểm tra người nhận tồn tại, không cần truy vấn riêng.
        Statement stmt(db_, "INSERT INTO messages(sender_id,receiver_id,content) "
                             "SELECT ?1,id,?2 FROM users WHERE id=?3;");
        if (stmt.status() != SQLITE_OK ||
            sqlite3_bind_int64(stmt.get(), 1, sender) != SQLITE_OK ||
            BindText(stmt.get(), 2, content) != SQLITE_OK ||
            sqlite3_bind_int64(stmt.get(), 3, receiver) != SQLITE_OK)
            return DbCode::error;
        const int rc = sqlite3_step(stmt.get());
        if (rc != SQLITE_DONE) return DbCode::error;
        if (sqlite3_changes(db_) == 0) return DbCode::notFound;
        messageId = sqlite3_last_insert_rowid(db_);
        return DbCode::ok;
    }

    DbCode Messages(sqlite3_int64 current, sqlite3_int64 peer,
                    sqlite3_int64 afterId, int limit, json& messages) {
        lock_guard<mutex> lock(mutex_);
        Statement stmt(db_,
            "SELECT id,sender_id,receiver_id,content,created_at FROM messages "
            "WHERE id>?1 AND ((sender_id=?2 AND receiver_id=?3) "
            "OR (sender_id=?3 AND receiver_id=?2)) "
            "ORDER BY id ASC LIMIT ?4;");
        if (stmt.status() != SQLITE_OK ||
            sqlite3_bind_int64(stmt.get(), 1, afterId) != SQLITE_OK ||
            sqlite3_bind_int64(stmt.get(), 2, current) != SQLITE_OK ||
            sqlite3_bind_int64(stmt.get(), 3, peer) != SQLITE_OK ||
            sqlite3_bind_int(stmt.get(), 4, limit) != SQLITE_OK) return DbCode::error;
        messages = json::array();
        int rc;
        while ((rc = sqlite3_step(stmt.get())) == SQLITE_ROW) {
            messages.push_back({
                {"id", sqlite3_column_int64(stmt.get(), 0)},
                {"sender_id", sqlite3_column_int64(stmt.get(), 1)},
                {"receiver_id", sqlite3_column_int64(stmt.get(), 2)},
                {"content", ColumnText(stmt.get(), 3)},
                {"created_at", ColumnText(stmt.get(), 4)}});
        }
        return rc == SQLITE_DONE ? DbCode::ok : DbCode::error;
    }

private:
    sqlite3* db_ = nullptr;
    mutex mutex_;
};

// 4. Phiên đăng nhập và thông báo tin mới theo thời gian thực ----------------
class Sessions {
public:
    bool Create(sqlite3_int64 userId, string& token) {
        for (int attempt = 0; attempt < 3; ++attempt) {
            token = NewToken();
            if (token.empty()) return false;
            lock_guard<mutex> lock(mutex_);
            if (tokens_.emplace(token, userId).second) return true;
        }
        return false;
    }

    bool Find(const string& token, sqlite3_int64& userId) {
        lock_guard<mutex> lock(mutex_);
        const auto it = tokens_.find(token);
        if (it == tokens_.end()) return false;
        userId = it->second;
        return true;
    }

    void Remove(const string& token) {
        lock_guard<mutex> lock(mutex_);
        tokens_.erase(token);
    }

private:
    mutex mutex_;
    unordered_map<string, sqlite3_int64> tokens_;
};

class MessageEvents {
public:
    void Notify(sqlite3_int64 receiver, sqlite3_int64 messageId) {
        {
            lock_guard<mutex> lock(mutex_);
            latest_[receiver] = messageId; // ID tin mới nhất
        }
        changed_.notify_all(); // // đánh thức các luồng đang chờ
    }

    sqlite3_int64 Current(sqlite3_int64 receiver) {
        lock_guard<mutex> lock(mutex_);
        return latest_[receiver];
    }

    sqlite3_int64 Wait(sqlite3_int64 receiver, sqlite3_int64 seen,
                       const atomic<bool>& stopping) {
        unique_lock<mutex> lock(mutex_);
        changed_.wait_for(lock, chrono::seconds(15), [&] {
            return stopping.load() || latest_[receiver] > seen;
        });
        return latest_[receiver];
    }

    void WakeAll() { changed_.notify_all(); }

private:
    mutex mutex_;
    condition_variable changed_;
    unordered_map<sqlite3_int64, sqlite3_int64> latest_;
};

// 5. API HTTP: nhận yêu cầu, kiểm tra quyền, gọi DB và trả lời ---------------
// _ : nhận diện biến thành viên của class
class ChatServer {
public:
    bool Initialize(const string& dbPath, string& error) {
        if (!database_.Open(dbPath, error)) return false;
        server_.set_payload_max_length(kMaxBody);
        // Trong bản đơn giản này, mỗi kết nối SSE chiếm một worker.
        server_.new_task_queue = [] { return new httplib::ThreadPool(32); };
        ConfigureRoutes();
        return true;
    }



    bool Bind(const string& host, int port) { // kiểm tra, chuẩn bị IP:Port trước khi lắng nghe
        return server_.bind_to_port(host, port);
    }
    bool Listen() { return server_.listen_after_bind(); }
    void Stop() {
        stopping_.store(true);
        events_.WakeAll();
        server_.stop();
    }



private:
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

    void ConfigureRoutes() {
    // req: request do httplib đưa vào; có body JSON, header token… const nghĩa là code này không sửa req.
    // res: response do httplib đưa vào để code điền kết quả trả cho client.
    // [this] : không phải tham số của Post hay của request.Nó cho lambda dùng các thành viên của đối tượng ChatServer hiện tại, như database_ và events_.
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
            const DbCode result = database_.Credentials(username, userId, storedHash);
            if (result == DbCode::error) {
                Error(res, 500, "Lỗi cơ sở dữ liệu");
                return;
            }
            if (result == DbCode::notFound ||
                !VerifyPassword(password, storedHash)) {
                Error(res, 401, "Tên tài khoản hoặc mật khẩu không đúng");
                return;
            }
            string token;
            if (!sessions_.Create(userId, token)) {
                Error(res, 500, "Không thể tạo phiên đăng nhập");
                return;
            }
            Reply(res, 200, json{{"token", token}, {"user_id", userId}});
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
            else Reply(res, 200, json{{"users", users}});
        });

        server_.Post("/friends", [this](const httplib::Request& req,
                                        httplib::Response& res) {
            sqlite3_int64 userId = 0;
            if (!Authenticate(req, res, userId)) return;
            json body;
            if (!JsonBody(req, res, body)) return;
            sqlite3_int64 other = 0;
            if (!PositiveId(body, "user_id", other) || other == userId) {
                Error(res, 400, "ID bạn bè không hợp lệ");
                return;
            }
            const DbCode result = database_.AddFriend(userId, other);
            if (result == DbCode::ok) Reply(res, 201, json{{"ok", true}});
            else if (result == DbCode::notFound) Error(res, 404, "Không tìm thấy người dùng");
            else if (result == DbCode::duplicate)
                Error(res, 409, "Hai người đã là bạn bè");
            else Error(res, 500, "Lỗi cơ sở dữ liệu");
        });

        server_.Get("/friends", [this](const httplib::Request& req,
                                       httplib::Response& res) {
            sqlite3_int64 userId = 0;
            if (!Authenticate(req, res, userId)) return;
            json friends;
            if (database_.Friends(userId, friends) != DbCode::ok)
                Error(res, 500, "Lỗi cơ sở dữ liệu");
            else Reply(res, 200, json{{"friends", friends}});
        });

        server_.Post("/messages", [this](const httplib::Request& req,
                                         httplib::Response& res) {
            sqlite3_int64 sender = 0;
            if (!Authenticate(req, res, sender)) return;
            json body;
            if (!JsonBody(req, res, body)) return;
            sqlite3_int64 receiver = 0;
            string content;
            if (!PositiveId(body, "receiver_id", receiver) ||
                !StringField(body, "content", content) || content.empty() ||
                content.size() > kMaxContent || receiver == sender) {
                Error(res, 400, "Người nhận hoặc nội dung không hợp lệ");
                return;
            }
            sqlite3_int64 messageId = 0;
            const DbCode result = database_.SendMessage(sender, receiver,
                                                        content, messageId);
            if (result == DbCode::notFound) {
                Error(res, 404, "Không tìm thấy người nhận");
            } else if (result != DbCode::ok) {
                Error(res, 500, "Lỗi cơ sở dữ liệu");
            } else {
                events_.Notify(receiver, messageId);
                Reply(res, 201, json{{"id", messageId}});
            }
        });


    //      POST / messages
    //    -> xác định người gửi là An từ token
    //    -> đọc JSON, kiểm tra người nhận và nội dung
    //    -> lưu tin vào SQLite
    //    -> báo Bình có tin mới
    //    -> trả ID tin nhắn cho An
    //    ->  SSE

        server_.Get("/messages", [this](const httplib::Request& req,
                                        httplib::Response& res) {
            sqlite3_int64 userId = 0;
            if (!Authenticate(req, res, userId)) return;
            sqlite3_int64 peer = 0, after = 0, pageSize = 50;
            if (!req.has_param("peer_id") ||
                !DecimalId(req.get_param_value("peer_id"), peer) || peer <= 0 ||
                (req.has_param("after_id") &&
                 !DecimalId(req.get_param_value("after_id"), after)) ||
                (req.has_param("limit") &&
                 !DecimalId(req.get_param_value("limit"), pageSize)) ||
                pageSize < 1 || pageSize > 100) {
                Error(res, 400, "Tham số truy vấn không hợp lệ");
                return;
            }
            json messages;
            if (database_.Messages(userId, peer, after,
                                   static_cast<int>(pageSize), messages) != DbCode::ok) {
                Error(res, 500, "Lỗi cơ sở dữ liệu");
                return;
            }
            const sqlite3_int64 next = messages.empty()
                ? after : messages.back().at("id").get<sqlite3_int64>();
            Reply(res, 200, json{{"messages", messages}, {"next_after_id", next}});
        });

        // SSE đẩy thông báo; GET /messages lấy nội dung đã lưu trong DB.
        server_.Get("/events", [this](const httplib::Request& req,
                                      httplib::Response& res) {
            sqlite3_int64 userId = 0;
            if (!Authenticate(req, res, userId)) return;
            res.set_header("Cache-Control", "no-cache");
            res.set_chunked_content_provider("text/event-stream",
                [this, userId, seen = events_.Current(userId)]
                (size_t, httplib::DataSink& sink) mutable {
                    if (stopping_.load()) return false;
                    const sqlite3_int64 latest = events_.Wait(userId, seen, stopping_);
                    if (stopping_.load()) return false;
                    string chunk;
                    if (latest > seen) {
                        seen = latest;
                        chunk = "event: message\ndata: " +
                            json{{"latest_id", latest}}.dump() + "\n\n";
                    } else {
                        chunk = ": keepalive\n\n";
                    }
                    return sink.write(chunk.data(), chunk.size());
                });
        });
    }

    Database database_;
    Sessions sessions_;
    MessageEvents events_;
    atomic<bool> stopping_{false};
    httplib::Server server_; // Hủy trước các đối tượng mà callback đang dùng.
};

// 6. Cùng một server chạy ở console hoặc dưới dạng Windows Service ----------
struct Options {
    bool service = false;
    string host = "127.0.0.1";
    int port = 8080;
    fs::path dbPath;
};

Options g_options;
atomic<ChatServer*> g_activeServer{nullptr}; 
atomic<bool> g_stopRequested{false};
SERVICE_STATUS_HANDLE g_serviceHandle = nullptr;
// atomic là template kiểu dữ liệu
// Luồng chạy server : store(&server)   -> Gán giá trị vào biến hiện tại
// Luồng nhận Stop : load()          -> đọc giá trị hiện tại

void ReportServiceStatus(DWORD state, DWORD error = NO_ERROR) {
    if (!g_serviceHandle) return;
    SERVICE_STATUS status{};
    status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    status.dwCurrentState = state;
    status.dwControlsAccepted = state == SERVICE_RUNNING ? SERVICE_ACCEPT_STOP : 0;
    status.dwWin32ExitCode = error;
    status.dwWaitHint = state == SERVICE_START_PENDING ||
                        state == SERVICE_STOP_PENDING ? 30000 : 0;
    SetServiceStatus(g_serviceHandle, &status);
}

// Callback : phần thân hàm tùy ý, nhưng chữ ký của hàm phải đúng mẫu callback mà Windows yêu cầu.
DWORD WINAPI ServiceControl(DWORD control, DWORD, LPVOID, LPVOID) { // Dừng khi chạy qua service sc.exe
    if (control == SERVICE_CONTROL_STOP) {
        g_stopRequested.store(true); // g_stopRequested = true;
        ReportServiceStatus(SERVICE_STOP_PENDING);
        if (ChatServer* server = g_activeServer.load()) server->Stop();
    }
    return NO_ERROR;
}

BOOL WINAPI ConsoleControl(DWORD control) { // Ctrl+c khi chạy console, cũng để dùng chương trình
    if (control == CTRL_C_EVENT || control == CTRL_BREAK_EVENT ||
        control == CTRL_CLOSE_EVENT) {
        g_stopRequested.store(true);
        if (ChatServer* server = g_activeServer.load()) server->Stop();
        return TRUE;
    }
    return FALSE;
}

int RunServer(const Options& options) {
    ChatServer server;
    string error;
    const string dbPath = options.dbPath.u8string();
    if (!server.Initialize(dbPath, error)) {
        cerr << "Cơ sở dữ liệu: " << error << '\n';
        return 1;
    }
    if (!server.Bind(options.host, options.port)) {
        cerr << "Không thể lắng nghe tại " << options.host << ':' << options.port << '\n';
        return 1;
    }

    g_activeServer.store(&server);
    if (g_stopRequested.load()) {
        server.Stop();
        g_activeServer.store(nullptr);
        return 0;
    }
    if (options.service) ReportServiceStatus(SERVICE_RUNNING);
    else cout << "Server HTTP đang nghe tại http://" << options.host << ':'
                   << options.port << "  DB: " << dbPath << '\n';

    const bool ok = server.Listen();
    g_activeServer.store(nullptr);
    return ok || g_stopRequested.load() ? 0 : 1;
}

void WINAPI ServiceMain(DWORD, LPWSTR*) {
    g_serviceHandle = RegisterServiceCtrlHandlerExW(kServiceName,
                                                     ServiceControl, nullptr);
    if (!g_serviceHandle) return;
    ReportServiceStatus(SERVICE_START_PENDING);
    const int result = RunServer(g_options);
    ReportServiceStatus(SERVICE_STOPPED,
                        result == 0 ? NO_ERROR : ERROR_SERVICE_SPECIFIC_ERROR);
}

bool ReadOptions(int argc, char* argv[], Options& options) {
    for (int i = 1; i < argc; ++i) {
        const string arg = argv[i];
        if (arg == "--service") {
            options.service = true;
        } else if (arg == "--host" && i + 1 < argc) {
            options.host = argv[++i];
        } else if (arg == "--db" && i + 1 < argc) {
            options.dbPath = fs::absolute(fs::path(argv[++i]));
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
        if (options.service) {
            wchar_t modulePath[MAX_PATH]{};
            const DWORD length = GetModuleFileNameW(nullptr, modulePath, MAX_PATH);
            if (length == 0 || length >= MAX_PATH) return false;
            options.dbPath = fs::path(modulePath).parent_path() / L"chat.db";
        } else {
            // Dùng lại database đã tạo khi chạy thử bằng console trước đó.
            options.dbPath = fs::absolute("chat.db");
        }
    }
    return true;
}
} // Kết thúc vùng tên nội bộ.

int main(int argc, char* argv[]) {
    // Chuỗi trong mã nguồn là UTF-8; console cũng cần đọc các byte đó theo UTF-8.
    SetConsoleOutputCP(CP_UTF8);
    try {
        if (!ReadOptions(argc, argv, g_options)) {
            cerr << "Cách dùng: ConsoleTuan6.exe [--service] [--host IP] "
                         "[--port 8080] [--db DUONG_DAN_TUYET_DOI]\n";
            return 2;
        }
        if (g_options.service) {
            // sc.exe create TTCChatServer binPath = '"D:\duong-dan\ConsoleTuan6.exe" --service' start = auto
            // sc.exe start TTCChatServer
            // sc.exe stop TTCChatServer
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

