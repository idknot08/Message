// ConsoleTuan6.cpp : This file contains the 'main' function. Program execution begins and ends there.
//

#include <iostream>
#include "D:\DEV\DEVing\TTC\Tuan6\cpp-httplib-master\httplib.h"
#include "D:\DEV\DEVing\TTC\Tuan6\json-develop\single_include\nlohmann\json.hpp"
#include "sqlite3.h"
#include "string"

#define NOMINMAX // không tạo micro tên min, max (idk)
#include <Windows.h>
#include <array>
#include <iomanip>
#include <sstream> // ostringstream ghep du lieu thanh chuoi
#include <bcrypt.h> //setw, setfill
#pragma comment(lib, "Bcrypt.lib") 

#include <mutex>
#include <unordered_map>


using namespace std;

// tính HashPasswword

string HashPassword(const string& password) {
    if (password.empty() || password.size() > 128) return {};

    ULONGLONG iterations = 600000;
    array<UCHAR, 16> salt{};
    array<UCHAR, 32> hash{};

    if (!BCRYPT_SUCCESS(BCryptGenRandom( // Tao byte ngau nhien va ghi voaf mang salt
        nullptr, salt.data(), static_cast<ULONG>(salt.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG))) {
        return {};
    };

    BCRYPT_ALG_HANDLE algorithm = nullptr; // Bien kieu con tro
    if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider( // Mo bo cong cu tinh hash
        &algorithm,
        BCRYPT_SHA256_ALGORITHM, //SHA256
        nullptr,
        BCRYPT_ALG_HANDLE_HMAC_FLAG // Bat HMAC
    ))) {
        return {};
    };

    NTSTATUS result = BCryptDeriveKeyPBKDF2(
        algorithm,
        reinterpret_cast<PUCHAR>(const_cast<char*>(password.data())), // địa chỉ byte mk
        static_cast<ULONG>(password.size()),
        salt.data(), static_cast<ULONG>(salt.size()),
        iterations,
        hash.data(), static_cast<ULONG>(hash.size()), // lưu kq
        0);

    BCryptCloseAlgorithmProvider(algorithm, 0);
    if (!BCRYPT_SUCCESS(result)) return {};

    //BCRYPT_SUCCESS macro kiem tra ma trang thai

    ostringstream out;
    out << "pbkdf2-sha256$" << iterations << '$' << hex << setfill('0'); // he 16. dem 0

    for (UCHAR byte : salt) out << setw(2) << static_cast<int>(byte);
    out << '$';
    for (UCHAR byte : hash) out << setw(2) << static_cast<int>(byte);
    return out.str();
}

// Kiểm tra password khi login
// hàm của Windows làm việc với các byte => phải chuyển từ chữ hex về byte
int HexDigit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

bool DecodeHex(const string& text, size_t start, UCHAR* output, size_t count) {
    if (start + count * 2 > text.size()) return false;
    for (size_t i = 0; i < count; ++i) {
        int high = HexDigit(text[start + i * 2]);
        int low = HexDigit(text[start + i * 2 + 1]);
        if (high < 0 || low < 0) return false;
        output[i] = static_cast<UCHAR>((high << 4) | low);
    }
    return true;
}

bool VerifyPassword(const string& password, const string& stored) {
    const string prefix = "pbkdf2-sha256$600000$";
    if (password.empty() || password.size() > 128 ||
        stored.size() != prefix.size() + 32 + 1 + 64 ||
        stored.compare(0, prefix.size(), prefix) != 0 ||
        stored[prefix.size() + 32] != '$') {
        return false;
    }

    array<UCHAR, 16> salt{};
    array<UCHAR, 32> expected{};
    array<UCHAR, 32> actual{};

    if (!DecodeHex(stored, prefix.size(), salt.data(), salt.size()) ||
        !DecodeHex(stored, prefix.size() + 33, expected.data(), expected.size()))
        return false;

    BCRYPT_ALG_HANDLE algorithm = nullptr;
    if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(
        &algorithm, BCRYPT_SHA256_ALGORITHM, nullptr,
        BCRYPT_ALG_HANDLE_HMAC_FLAG)))
        return false;

    NTSTATUS result = BCryptDeriveKeyPBKDF2(
        algorithm,
        reinterpret_cast<PUCHAR>(const_cast<char*>(password.data())),
        static_cast<ULONG>(password.size()),
        salt.data(), static_cast<ULONG>(salt.size()),
        600000,
        actual.data(), static_cast<ULONG>(actual.size()),
        0);

    BCryptCloseAlgorithmProvider(algorithm, 0);
    if (!BCRYPT_SUCCESS(result)) return false;

    unsigned int difference = 0;
    for (size_t i = 0; i < actual.size(); ++i)
        difference |= actual[i] ^ expected[i];
    return difference == 0;
}

string NewToken() {
    array<UCHAR, 32> bytes{};
    if (!BCRYPT_SUCCESS(BCryptGenRandom(
        nullptr, bytes.data(), static_cast<ULONG>(bytes.size()),
        BCRYPT_USE_SYSTEM_PREFERRED_RNG)))
        return {};

    ostringstream out;
    out << hex << setfill('0');
    for (UCHAR byte : bytes)
        out << setw(2) << static_cast<int>(byte);
    return out.str();
}

int main()
{   // Kết nối DB
    sqlite3* db = nullptr;

    if (sqlite3_open("chat.db", &db) != SQLITE_OK) {
        cerr << "Khong mo duoc DB: " << sqlite3_errmsg(db) << '\n';
        sqlite3_close(db);
        return 1;
    }

    cout << "Mo DB thanh cong \n";

    // Tạo bảng dữ liệu
    const char* sql = R"(
        CREATE TABLE IF NOT EXISTS users(
            id INTEGER PRIMARY KEY,
            username TEXT NOT NULL COLLATE NOCASE UNIQUE,
            password_hash TEXT NOT NULL
        );
        )";

    char* error = nullptr;
    if (sqlite3_exec(db, sql, nullptr, nullptr, &error) != SQLITE_OK) {
        cerr << "Tao bang that bai" << error << '\n';
        sqlite3_free(error);
        sqlite3_close(db);
        return 1;
    }
    cout << "Tao bang thanh cong \n";

    // Đăng ký nhận request và response
    httplib::Server svr;

    svr.Get("/health", [](const httplib::Request& req, httplib::Response& res) {
        res.set_content(R"({"ok":true})", "application/json");
    });


    mutex dbMutex;
    svr.Post("/register", [db, &dbMutex](const httplib::Request& req, httplib::Response& res) {
        auto data = nlohmann::json::parse(req.body, nullptr, false);

        if (data.is_discarded() || !data.is_object() ||
            !data.contains("username") || !data["username"].is_string() ||
            !data.contains("password") || !data["password"].is_string())
        {
            res.status = 400;
            res.set_content(R"({"error": "ivalid username"})", "application/json");
            return;
        }

        string username = data["username"].get<string>();
        string password = data["password"].get<string>();

        cout << "Username: " << username << '\n';

        // hash password
        // cout << "password: " << password << '\n'; // tạm cho dễ debug
        string passwordHash = HashPassword(password);
        if (passwordHash.empty()) {
            res.status = 400;
            res.set_content(R"({"error":"invalid password or hashing failed"})",
                "application/json");
            return;
        }
        cout << "Da bam password\n";
        // cout << "passwordHash: " << passwordHash << '\n';

        // Khóa DB -> chuẩn bị câu INSERT -> điền username/hash -> chạy INSERT -> dọn câu lệnh

        int rc;
        {
            lock_guard<mutex> lock(dbMutex); //lock db
            sqlite3_stmt* stmt = nullptr;
            rc = sqlite3_prepare_v2( // tao mau insert
                db,
                "INSERT INTO users (username, password_hash) VALUES (?, ?);",
                -1,
                &stmt, nullptr);

            if (rc == SQLITE_OK)
                rc = sqlite3_bind_text(stmt, 1, username.data(),
                    static_cast<int>(username.size()), SQLITE_TRANSIENT);
            if (rc == SQLITE_OK)
                rc = sqlite3_bind_text(stmt, 2, passwordHash.data(),
                    static_cast<int>(passwordHash.size()), SQLITE_TRANSIENT);
            if (rc == SQLITE_OK)
                rc = sqlite3_step(stmt);

            if (stmt) sqlite3_finalize(stmt);
        };

        if (rc == SQLITE_DONE) {
            res.status = 201;
            res.set_content(R"({"ok":true})", "application/json");
        }
        else if ((rc & 0xff) == SQLITE_CONSTRAINT) {
            res.status = 409;
            res.set_content(R"({"error":"username already exists"})",
                "application/json");
        }
        else {
            res.status = 500;
            res.set_content(R"({"error":"database error"})", "application/json");
        };

        });

    // tạo session => user ID
    unordered_map<string, sqlite3_int64> sessions; // token -> user ID
    mutex sessionsMutex;
    svr.Post("/login", [db, &dbMutex, &sessions, &sessionsMutex](const httplib::Request& req,
        httplib::Response& res) {
            auto data = nlohmann::json::parse(req.body, nullptr, false);
            if (data.is_discarded() || !data.is_object() ||
                !data.contains("username") || !data["username"].is_string() ||
                !data.contains("password") || !data["password"].is_string()) {
                res.status = 400;
                res.set_content(R"({"error":"invalid input"})", "application/json");
                return;
            }

            string username = data["username"].get<string>();
            string password = data["password"].get<string>();
            if (username.empty() || username.size() > 32 ||
                password.empty() || password.size() > 128) {
                res.status = 400;
                res.set_content(R"({"error":"invalid length"})", "application/json");
                return;
            }

            string storedHash;
            sqlite3_int64 userId = 0;
            int rc;
            {
                lock_guard<mutex> lock(dbMutex);
                sqlite3_stmt* stmt = nullptr;
                rc = sqlite3_prepare_v2(
                    db, "SELECT id, password_hash FROM users WHERE username = ? ; ",
                    -1, &stmt, nullptr);

                if (rc == SQLITE_OK)
                    rc = sqlite3_bind_text(stmt, 1, username.data(),
                        static_cast<int>(username.size()), SQLITE_TRANSIENT);
                if (rc == SQLITE_OK)
                    rc = sqlite3_step(stmt);

                if (rc == SQLITE_ROW) {
                    userId = sqlite3_column_int64(stmt, 0);
                    const unsigned char* value = sqlite3_column_text(stmt, 1);
                    if (value)
                        storedHash.assign(reinterpret_cast<const char*>(value),
                            sqlite3_column_bytes(stmt, 1));
                    else
                        rc = SQLITE_ERROR;
                }
                if (stmt) sqlite3_finalize(stmt);
            }

            if (rc == SQLITE_DONE || (rc == SQLITE_ROW &&
                !VerifyPassword(password, storedHash))) {
                res.status = 401;
                res.set_content(R"({"error":"invalid credentials"})", "application/json");
            }
            else if (rc != SQLITE_ROW) {
                res.status = 500;
                res.set_content(R"({"error":"database error"})", "application/json");
            }
            else {
                string token = NewToken();
                if (token.empty()) {
                    res.status = 500;
                    res.set_content(R"({"error":"token generation failed"})",
                        "application/json");
                    return;
                }

                {
                    lock_guard<mutex> lock(sessionsMutex);
                    sessions[token] = userId;
                }

                nlohmann::json reply = { {"token", token} };
                res.set_content(reply.dump(), "application/json");
            }
        });
    
    // Bắt đầu lắng nghe
    svr.listen("127.0.0.1", 8080);
    sqlite3_close(db);
}

