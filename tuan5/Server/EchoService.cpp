#include <winsock2.h>
#include <windows.h>
#include <array>
#include <vector>
#include <mutex>
#include <algorithm>
#include <cstdio>
#pragma comment(lib, "Ws2_32.lib")
#pragma comment(lib, "Advapi32.lib")

namespace {
constexpr unsigned short Port = 19999;
wchar_t serviceName[] = L"TTCWeek5Echo";
SERVICE_STATUS_HANDLE statusHandle;
HANDLE stopEvent;
std::mutex controlMutex;

void Report(DWORD state, DWORD error = NO_ERROR) {
    SERVICE_STATUS status{};
    status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    status.dwCurrentState = state;
    status.dwWin32ExitCode = error;
    if (state == SERVICE_RUNNING) status.dwControlsAccepted = SERVICE_ACCEPT_STOP;
    if (state == SERVICE_START_PENDING || state == SERVICE_STOP_PENDING) {
        status.dwCheckPoint = 1;
        status.dwWaitHint = 3000;
    }
    SetServiceStatus(statusHandle, &status);
}

DWORD WINAPI Control(DWORD control, DWORD, void*, void*) {
    if (control == SERVICE_CONTROL_INTERROGATE) return NO_ERROR;
    if (control != SERVICE_CONTROL_STOP) return ERROR_CALL_NOT_IMPLEMENTED;
    std::lock_guard<std::mutex> lock(controlMutex);
    if (stopEvent) {
        Report(SERVICE_STOP_PENDING);
        SetEvent(stopEvent);
    }
    return NO_ERROR;
}

struct Peer {
    SOCKET socket;
    std::array<char, 4096> data{};
    int offset = 0;
    int size = 0;
};

bool Nonblocking(SOCKET socket) {
    u_long enabled = 1;
    return ioctlsocket(socket, FIONBIO, &enabled) == 0;
}

// Chỉ đọc khối kế tiếp sau khi đã gửi hết khối trước cho chính socket đó.
bool Echo(Peer& peer, const fd_set& reads, const fd_set& writes) {
    if (FD_ISSET(peer.socket, &writes)) {
        int n = send(peer.socket, peer.data.data() + peer.offset, peer.size - peer.offset, 0);
        if (n <= 0) return n == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK;
        peer.offset += n;
    } else if (FD_ISSET(peer.socket, &reads)) {
        int n = recv(peer.socket, peer.data.data(), static_cast<int>(peer.data.size()), 0);
        if (n <= 0) return n == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK;
        peer.size = n;
        peer.offset = 0;
    }
    return true;
}

DWORD Serve(SOCKET listener) {
    std::vector<Peer> peers;
    peers.reserve(FD_SETSIZE - 1);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(Port);
    if (!Nonblocking(listener) || bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) ||
        listen(listener, SOMAXCONN)) return WSAGetLastError();

    Report(SERVICE_RUNNING);
    DWORD error = NO_ERROR;
    while (WaitForSingleObject(stopEvent, 0) == WAIT_TIMEOUT) {
        fd_set reads{}, writes{};
        if (peers.size() < FD_SETSIZE - 1) FD_SET(listener, &reads);
        for (const auto& peer : peers) {
            if (peer.offset < peer.size) FD_SET(peer.socket, &writes);
            else FD_SET(peer.socket, &reads);
        }
        timeval timeout{0, 100000};
        if (select(0, &reads, &writes, nullptr, &timeout) == SOCKET_ERROR) {
            error = WSAGetLastError();
            break;
        }
        peers.erase(std::remove_if(peers.begin(), peers.end(), [&](Peer& p) {
            if (Echo(p, reads, writes)) return false;
            closesocket(p.socket);
            return true;
        }), peers.end());
        if (FD_ISSET(listener, &reads)) {
            SOCKET incoming = accept(listener, nullptr, nullptr);
            if (incoming != INVALID_SOCKET) {
                if (Nonblocking(incoming)) peers.push_back(Peer{incoming});
                else closesocket(incoming);
            }
        }
    }
    for (auto& peer : peers) closesocket(peer.socket);
    return error;
}

void WINAPI ServiceMain(DWORD, wchar_t**) {
    statusHandle = RegisterServiceCtrlHandlerExW(serviceName, Control, nullptr);
    if (!statusHandle) return;
    Report(SERVICE_START_PENDING);
    stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    DWORD error = stopEvent ? NO_ERROR : GetLastError();
    WSADATA data{};
    if (!error) {
        error = WSAStartup(MAKEWORD(2, 2), &data);
        if (!error) {
            SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            if (listener == INVALID_SOCKET) error = WSAGetLastError();
            else {
                try { error = Serve(listener); }
                catch (const std::bad_alloc&) { error = ERROR_NOT_ENOUGH_MEMORY; }
                closesocket(listener);
            }
            WSACleanup();
        }
    }
    std::lock_guard<std::mutex> lock(controlMutex);
    if (stopEvent) CloseHandle(stopEvent);
    stopEvent = nullptr;
    Report(SERVICE_STOPPED, error);
}
}

int wmain() {
    SERVICE_TABLE_ENTRYW services[] = {{serviceName, ServiceMain}, {nullptr, nullptr}};
    if (StartServiceCtrlDispatcherW(services)) return 0;
    std::fwprintf(stderr, L"Start TTCWeek5Echo using Windows Services (error %lu).\n", GetLastError());
    return 1;
}
