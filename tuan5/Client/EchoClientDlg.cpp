#include "pch.h"
#include "EchoClientDlg.h"

namespace {
constexpr UINT EchoFinished = WM_APP + 1;
int Exchange(SOCKET sock, const sockaddr_in& address, const std::string& message, std::string& reply) {
    u_long mode = 1;
    if (ioctlsocket(sock, FIONBIO, &mode)) return WSAGetLastError();
    if (connect(sock, reinterpret_cast<const sockaddr*>(&address), sizeof(address))) {
        int err = WSAGetLastError();
        if (err != WSAEWOULDBLOCK) return err;
        fd_set writes{}, errors{};
        FD_SET(sock, &writes);
        FD_SET(sock, &errors);
        timeval timeout{3, 0};
        int ready = select(0, nullptr, &writes, &errors, &timeout);
        if (!ready) return WSAETIMEDOUT;
        if (ready == SOCKET_ERROR) return WSAGetLastError();
        int size = sizeof(err);
        if (getsockopt(sock, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&err), &size)) return WSAGetLastError();
        if (err) return err;
    }
    mode = 0;
    DWORD timeoutMs = 3000;
    if (ioctlsocket(sock, FIONBIO, &mode) ||
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeoutMs), sizeof(timeoutMs)) ||
        setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeoutMs), sizeof(timeoutMs))) return WSAGetLastError();

    const int total = static_cast<int>(message.size());
    for (int sent = 0; sent < total;) {
        int n = send(sock, message.data() + sent, total - sent, 0);
        if (n <= 0) return n == 0 ? WSAECONNRESET : WSAGetLastError();
        sent += n;
    }
    reply.resize(total);
    for (int received = 0; received < total;) {
        int n = recv(sock, reply.data() + received, total - received, 0);
        if (n <= 0) return n == 0 ? WSAECONNRESET : WSAGetLastError();
        received += n;
    }
    return 0;
}

}

CEchoClientDlg::CEchoClientDlg(CWnd* parent)
    : CDialog(IDD_ECHO, parent) {}

CEchoClientDlg::~CEchoClientDlg() {
    if (worker_.joinable()) worker_.join();
}

BOOL CEchoClientDlg::OnInitDialog() {
    CDialog::OnInitDialog();
    SetDlgItemText(IDC_ADDRESS, L"127.0.0.1");
    SendDlgItemMessage(IDC_TEXT, EM_SETLIMITTEXT, 2048);
    return TRUE;
}
void CEchoClientDlg::OnOK() { OnSend(); }
void CEchoClientDlg::OnCancel() {
    if (worker_.joinable()) {
        closing_ = true;
        ShowWindow(SW_HIDE);
    } else CDialog::OnCancel();
}
void CEchoClientDlg::OnSend() {
    if (worker_.joinable()) return;
    CString ip, text;
    GetDlgItemText(IDC_ADDRESS, ip);
    GetDlgItemText(IDC_TEXT, text);
    ip.Trim();
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(19999);
    if (InetPtonW(AF_INET, ip, &address.sin_addr) != 1) {
        MessageBox(L"Địa chỉ IPv4 không hợp lệ.");
        return;
    }
    if (text.IsEmpty()) return;
    std::string message(CW2A(text, CP_UTF8));
    const HWND window = m_hWnd;
    GetDlgItem(IDC_SEND)->EnableWindow(FALSE);
    try {
        worker_ = std::thread([this, address, message, window] {
            SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            if (sock == INVALID_SOCKET) error_ = WSAGetLastError();
            else {
                try { error_ = Exchange(sock, address, message, reply_); }
                catch (const std::bad_alloc&) { error_ = WSAENOBUFS; }
                closesocket(sock);
            }
            ::PostMessage(window, EchoFinished, 0, 0);
        });
    } catch (const std::system_error&) {
        GetDlgItem(IDC_SEND)->EnableWindow(TRUE);
        MessageBox(L"Không tạo được luồng gửi.");
    }
}
LRESULT CEchoClientDlg::OnFinished(WPARAM, LPARAM) {
    worker_.join();
    if (closing_) { CDialog::OnCancel(); return 0; }
    CString line;
    if (error_) line.Format(L"Lỗi Winsock %d\r\n", error_);
    else {
        line = L"Echo: ";
        line += CA2W(reply_.c_str(), CP_UTF8);
        line += L"\r\n";
    }
    auto* history = static_cast<CEdit*>(GetDlgItem(IDC_HISTORY));
    history->SetSel(-1, -1);
    history->ReplaceSel(line);
    GetDlgItem(IDC_SEND)->EnableWindow(TRUE);
    return 0;
}

BEGIN_MESSAGE_MAP(CEchoClientDlg, CDialog)
    ON_BN_CLICKED(IDC_SEND, &CEchoClientDlg::OnSend)
    ON_MESSAGE(EchoFinished, &CEchoClientDlg::OnFinished)
END_MESSAGE_MAP()
