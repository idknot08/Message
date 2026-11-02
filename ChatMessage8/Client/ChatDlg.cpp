#include "ChatDlg.h"
#include <stdexcept>

namespace {
std::string Utf8(const CString& text) {
    if (text.IsEmpty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text, text.GetLength(), nullptr, 0, nullptr, nullptr);
    if (!size) throw std::runtime_error("Invalid Unicode");
    std::string result(size, '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text, text.GetLength(), result.data(), size, nullptr, nullptr);
    return result;
}
CString Wide(const std::string& text) {
    if (text.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (!size) return L"[UTF-8 không hợp lệ]";
    CString result;
    auto* buffer = result.GetBuffer(size);
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), buffer, size);
    result.ReleaseBuffer(size);
    return result;
}
}

BEGIN_MESSAGE_MAP(ChatDlg, CDialog)
    ON_BN_CLICKED(IDC_CHECK, &ChatDlg::OnCheck)
    ON_BN_CLICKED(IDC_MODE, &ChatDlg::OnMode)
    ON_BN_CLICKED(IDC_CONNECT, &ChatDlg::OnLogin)
    ON_BN_CLICKED(IDC_DISCONNECT, &ChatDlg::OnLogout)
    ON_BN_CLICKED(IDC_SEND, &ChatDlg::OnSend)
    ON_BN_CLICKED(IDC_REFRESH, &ChatDlg::OnRefresh)
    ON_NOTIFY(LVN_ITEMCHANGED, IDC_USERS, &ChatDlg::OnUserChanged)
    ON_NOTIFY(LVN_ITEMCHANGED, IDC_CHAT, &ChatDlg::OnMessageChanged)
    ON_MESSAGE(WM_CHAT_EVENT, &ChatDlg::OnChatEvent)
END_MESSAGE_MAP()

BOOL ChatDlg::OnInitDialog() {
    CDialog::OnInitDialog();
    session_.Attach(m_hWnd);
    if (auth_) {
        SetDlgItemTextW(IDC_IP, session_.host);
        SetDlgItemTextW(IDC_PORT, session_.port);
        SetDlgItemTextW(IDC_NAME, session_.username);
        SetDlgItemTextW(IDC_STATUS, session_.status);
        SendDlgItemMessage(IDC_NAME, EM_SETLIMITTEXT, 32);
        SendDlgItemMessage(IDC_PASSWORD, EM_SETLIMITTEXT, 128);
        SendDlgItemMessage(IDC_CONFIRM, EM_SETLIMITTEXT, 128);
        UpdateControls();
        GetDlgItem(IDC_NAME)->SetFocus();
        return FALSE;
    }
    SetWindowTextW(L"Chat — " + session_.username);
    SendDlgItemMessage(IDC_INPUT, EM_SETLIMITTEXT, 4096);
    SendDlgItemMessage(IDC_DETAIL, EM_SETLIMITTEXT, 8192);
    userList_.SubclassDlgItem(IDC_USERS, this);
    chatList_.SubclassDlgItem(IDC_CHAT, this);
    userList_.SetExtendedStyle(LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    chatList_.SetExtendedStyle(LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER);
    userList_.InsertColumn(0, L"Người dùng", LVCFMT_LEFT, 130);
    userList_.InsertColumn(1, L"Trạng thái", LVCFMT_LEFT);
    userList_.SetColumnWidth(1, LVSCW_AUTOSIZE_USEHEADER);
    chatList_.InsertColumn(0, L"Thời gian UTC", LVCFMT_LEFT, 140);
    chatList_.InsertColumn(1, L"Người gửi", LVCFMT_LEFT, 100);
    chatList_.InsertColumn(2, L"Nội dung", LVCFMT_LEFT);
    chatList_.SetColumnWidth(2, LVSCW_AUTOSIZE_USEHEADER);
    UpdateControls();
    return TRUE;
}

ClientSession::ClientSession() : connection([this](ChatEvent event) {
    std::lock_guard<std::mutex> lock(mutex_);
    events_.push_back(std::move(event));
    if (window_) ::PostMessage(window_, WM_CHAT_EVENT, 0, 0);
}) {}

ClientSession::~ClientSession() { connection.Stop(); }

void ClientSession::Attach(HWND window) {
    std::lock_guard<std::mutex> lock(mutex_);
    window_ = window;
    if (window_ && !events_.empty()) ::PostMessage(window_, WM_CHAT_EVENT, 0, 0);
}

bool ClientSession::Next(ChatEvent& event) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (events_.empty()) return false;
    event = std::move(events_.front());
    events_.pop_front();
    return true;
}

bool ChatDlg::ReadEndpoint(std::string& host, int& port) {
    CString address, number;
    GetDlgItemTextW(IDC_IP, address);
    GetDlgItemTextW(IDC_PORT, number);
    address.Trim(); number.Trim();
    wchar_t* end = nullptr;
    const auto value = wcstol(number, &end, 10);
    if (address.IsEmpty() || end == number.GetString() || *end || value < 1 || value > 65535 ||
        address.Find(L'/') >= 0 || address.Find(L' ') >= 0) {
        MessageBoxW(L"Nhập IP hoặc tên máy (không kèm http://), cổng từ 1 đến 65535.");
        return false;
    }
    session_.host = address;
    session_.port = number;
    host = Utf8(address);
    port = static_cast<int>(value);
    return true;
}

void ChatDlg::UpdateControls() {
    const bool loggedIn = userId_ != 0;
    if (auth_) {
        for (int id : {IDC_IP, IDC_PORT, IDC_NAME, IDC_PASSWORD, IDC_CONFIRM, IDC_CONNECT, IDC_CHECK, IDC_MODE})
            GetDlgItem(id)->EnableWindow(!busy_);
        GetDlgItem(IDC_CONFIRM)->ShowWindow(registration_ ? SW_SHOW : SW_HIDE);
        GetDlgItem(IDC_CONFIRM_LABEL)->ShowWindow(registration_ ? SW_SHOW : SW_HIDE);
        SetWindowTextW(registration_ ? L"Đăng ký tài khoản" : L"Đăng nhập");
        SetDlgItemTextW(IDC_CONNECT, registration_ ? L"Đăng ký" : L"Đăng nhập");
        SetDlgItemTextW(IDC_MODE, registration_ ? L"Về đăng nhập" : L"Tạo tài khoản");
        return;
    }
    GetDlgItem(IDC_DISCONNECT)->EnableWindow(loggedIn && !busy_);
    GetDlgItem(IDC_SEND)->EnableWindow(loggedIn && selected_ != 0 && !busy_);
    GetDlgItem(IDC_REFRESH)->EnableWindow(loggedIn && !busy_);
    userList_.EnableWindow(loggedIn && !busy_);
    GetDlgItem(IDC_INPUT)->EnableWindow(loggedIn);
}

void ChatDlg::OnCheck() {
    std::string host; int port;
    if (!ReadEndpoint(host, port)) return;
    busy_ = true; UpdateControls();
    SetDlgItemTextW(IDC_STATUS, L"Đang kiểm tra kết nối...");
    session_.connection.Check(host, port);
}
void ChatDlg::Authenticate(bool registration) {
    std::string host; int port;
    if (!ReadEndpoint(host, port)) return;
    CString name, password;
    GetDlgItemTextW(IDC_NAME, name);
    GetDlgItemTextW(IDC_PASSWORD, password);
    name.Trim();
    if (name.IsEmpty() || password.IsEmpty()) {
        MessageBoxW(L"Nhập tên tài khoản và mật khẩu.");
        return;
    }
    if (registration) {
        CString confirm;
        GetDlgItemTextW(IDC_CONFIRM, confirm);
        if (password != confirm) {
            SetDlgItemTextW(IDC_STATUS, L"Mật khẩu nhập lại chưa khớp.");
            GetDlgItem(IDC_CONFIRM)->SetFocus();
            return;
        }
    }
    busy_ = true; UpdateControls();
    SetDlgItemTextW(IDC_STATUS, registration ? L"Đang tạo tài khoản..." : L"Đang đăng nhập...");
    if (registration) session_.connection.Register(host, port, Utf8(name), Utf8(password));
    else session_.connection.Login(host, port, Utf8(name), Utf8(password));
}
void ChatDlg::OnMode() {
    registration_ = !registration_;
    SetDlgItemTextW(IDC_CONFIRM, L"");
    SetDlgItemTextW(IDC_STATUS, L"");
    UpdateControls();
}
void ChatDlg::OnLogin() { if (!busy_) Authenticate(registration_); }
void ChatDlg::OnOK() { if (auth_ && !busy_) OnLogin(); }
void ChatDlg::OnLogout() {
    busy_ = true; UpdateControls();
    SetDlgItemTextW(IDC_STATUS, L"Đang đăng xuất...");
    session_.connection.Logout();
}
void ChatDlg::OnRefresh() {
    busy_ = true; UpdateControls();
    session_.connection.Refresh();
}
void ChatDlg::OnSend() {
    CString text;
    GetDlgItemTextW(IDC_INPUT, text);
    if (text.IsEmpty()) return;
    const auto content = Utf8(text);
    if (content.size() > 4096) {
        MessageBoxW(L"Tin nhắn vượt giới hạn 4096 byte UTF-8.");
        return;
    }
    busy_ = true; UpdateControls();
    SetDlgItemTextW(IDC_STATUS, L"Đang gửi tin...");
    session_.connection.Send(selected_, content);
}

void ChatDlg::ShowUsers() {
    renderingUsers_ = true;
    userList_.SetRedraw(FALSE);
    userList_.DeleteAllItems();
    for (size_t i = 0; i < users_.size(); ++i) {
        const int row = userList_.InsertItem(static_cast<int>(i), Wide(users_[i].name));
        userList_.SetItemText(row, 1, users_[i].online ? L"Online" : L"Offline");
        if (users_[i].id == selected_)
            userList_.SetItemState(row, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    }
    userList_.SetRedraw(TRUE);
    userList_.SetColumnWidth(1, LVSCW_AUTOSIZE_USEHEADER);
    userList_.Invalidate();
    renderingUsers_ = false;
}
void ChatDlg::ShowMessages() {
    chatList_.SetRedraw(FALSE);
    chatList_.DeleteAllItems();
    CString peerName = L"Người nhận";
    for (const auto& user : users_) if (user.id == selected_) peerName = Wide(user.name);
    for (const auto& message : messages_) {
        const int row = chatList_.InsertItem(chatList_.GetItemCount(), Wide(message.time));
        chatList_.SetItemText(row, 1, message.sender == userId_ ? L"Bạn" : peerName);
        CString preview = Wide(message.content);
        preview.Replace(L"\r", L" "); preview.Replace(L"\n", L" ");
        chatList_.SetItemText(row, 2, preview.Left(300));
    }
    chatList_.SetRedraw(TRUE);
    chatList_.SetColumnWidth(2, LVSCW_AUTOSIZE_USEHEADER);
    chatList_.Invalidate();
    if (!messages_.empty()) chatList_.EnsureVisible(chatList_.GetItemCount() - 1, FALSE);
}

void ChatDlg::OnUserChanged(NMHDR* header, LRESULT* result) {
    *result = 0;
    const auto* item = reinterpret_cast<NMLISTVIEW*>(header);
    if (renderingUsers_ || !(item->uChanged & LVIF_STATE) || !(item->uNewState & LVIS_SELECTED) ||
        item->iItem < 0 || static_cast<size_t>(item->iItem) >= users_.size()) return;
    const auto peer = users_[item->iItem].id;
    if (peer == selected_) return;
    selected_ = peer;
    messages_.clear(); ShowMessages();
    SetDlgItemTextW(IDC_DETAIL, L"");
    busy_ = true; UpdateControls();
    session_.connection.Select(selected_);
}
void ChatDlg::OnMessageChanged(NMHDR* header, LRESULT* result) {
    *result = 0;
    const auto* item = reinterpret_cast<NMLISTVIEW*>(header);
    if ((item->uNewState & LVIS_SELECTED) && item->iItem >= 0 &&
        static_cast<size_t>(item->iItem) < messages_.size())
        SetDlgItemTextW(IDC_DETAIL, Wide(messages_[item->iItem].content));
}

void ChatDlg::SwitchScreen() {
    session_.Attach(nullptr);
    // Để App mở dialog kế tiếp khi dialog hiện tại đóng.
    AfxGetApp()->m_pMainWnd = nullptr;
    EndDialog(IDRETRY);
}

LRESULT ChatDlg::OnChatEvent(WPARAM, LPARAM) {
    ChatEvent event;
    while (session_.Next(event)) {
        switch (event.kind) {
        case ChatEvent::Kind::Status:
            SetDlgItemTextW(IDC_STATUS, Wide(event.text));
            break;
        case ChatEvent::Kind::Login:
            session_.userId = event.userId;
            session_.username = Wide(event.text);
            session_.status.Empty();
            SwitchScreen();
            return 0;
        case ChatEvent::Kind::Logout:
            session_.userId = 0;
            if (!auth_) {
                SwitchScreen();
                return 0;
            }
            break;
        case ChatEvent::Kind::Users:
            if (!auth_) { users_ = std::move(event.users); ShowUsers(); }
            break;
        case ChatEvent::Kind::Messages:
            if (!auth_ && event.peer == selected_) {
                messages_ = std::move(event.messages); ShowMessages();
            }
            break;
        case ChatEvent::Kind::Sent: {
            CString current;
            GetDlgItemTextW(IDC_INPUT, current);
            if (event.peer == selected_ && Utf8(current) == event.text)
                SetDlgItemTextW(IDC_INPUT, L"");
            break;
        }
        case ChatEvent::Kind::Busy:
            busy_ = false;
            break;
        }
    }
    UpdateControls();
    return 0;
}
void ChatDlg::OnCancel() {
    session_.Attach(nullptr);
    CDialog::OnCancel();
}
