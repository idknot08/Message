#include "ChatDlg.h"
#include <algorithm>

BEGIN_MESSAGE_MAP(ChatDlg, CDialog)
    ON_BN_CLICKED(IDC_CONNECT, &ChatDlg::OnConnect)
    ON_BN_CLICKED(IDC_DISCONNECT, &ChatDlg::OnDisconnect)
    ON_BN_CLICKED(IDC_SEND, &ChatDlg::OnSend)
    ON_NOTIFY(LVN_ITEMCHANGED, IDC_USERS, &ChatDlg::OnUserChanged)
    ON_NOTIFY(LVN_ITEMCHANGED, IDC_CHAT, &ChatDlg::OnMessageChanged)
    ON_MESSAGE(WM_NET_EVENT, &ChatDlg::OnNetEvent)
END_MESSAGE_MAP()

// Đặt giá trị ban đầu và tạo các cột ListView khi dialog được mở.
BOOL ChatDlg::OnInitDialog() {
    CDialog::OnInitDialog();
#ifdef CHAT_SERVER
    SetWindowTextW(L"Chat Server - TCP");
    SetDlgItemTextW(IDC_NAME, L"Server");
    SetDlgItemTextW(IDC_IP, L"0.0.0.0");
    SetDlgItemTextW(IDC_CONNECT, L"Lắng nghe");
#else
    SetWindowTextW(L"Chat Client - TCP");
    SetDlgItemTextW(IDC_NAME, L"Client");
    SetDlgItemTextW(IDC_IP, L"127.0.0.1");
#endif
    SetDlgItemTextW(IDC_PORT, L"5500");
    SendDlgItemMessage(IDC_NAME, EM_SETLIMITTEXT, 64);
    SendDlgItemMessage(IDC_INPUT, EM_SETLIMITTEXT, 4 * 1024 * 1024);
    SendDlgItemMessage(IDC_DETAIL, EM_SETLIMITTEXT, 16 * 1024 * 1024);
    m_userList.SubclassDlgItem(IDC_USERS, this);
    m_chatList.SubclassDlgItem(IDC_CHAT, this);
    m_userList.SetExtendedStyle(LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);
    m_chatList.SetExtendedStyle(LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);
    CRect rect;
    m_userList.GetClientRect(&rect);
    m_userList.InsertColumn(0, L"Người dùng", LVCFMT_LEFT, rect.Width());
    m_chatList.GetClientRect(&rect);
    m_chatList.InsertColumn(0, L"Tin nhận", LVCFMT_LEFT, rect.Width() / 2);
    m_chatList.InsertColumn(1, L"Tin gửi", LVCFMT_LEFT, rect.Width() - rect.Width() / 2);
    SetRunning(false);
    return TRUE;
}

// Bật hoặc khóa các ô nhập và nút theo trạng thái kết nối.
void ChatDlg::SetRunning(bool running) {
    m_running = running;
    GetDlgItem(IDC_CONNECT)->EnableWindow(!running);
    GetDlgItem(IDC_DISCONNECT)->EnableWindow(running);
    GetDlgItem(IDC_NAME)->EnableWindow(!running);
    GetDlgItem(IDC_PORT)->EnableWindow(!running);
#ifdef CHAT_SERVER
    GetDlgItem(IDC_IP)->EnableWindow(FALSE);
#else
    GetDlgItem(IDC_IP)->EnableWindow(!running);
#endif
    bool online = false;
    for (const auto& user : m_users)
        if (user.id == m_selected && user.online) online = true;
    GetDlgItem(IDC_SEND)->EnableWindow(running && online);
}

// Đưa sự kiện từ luồng mạng sang hàng đợi của giao diện.
void ChatDlg::NotifyEvent(const NetEvent& event) {
    // Worker chỉ đưa sự kiện vào hàng đợi, giao diện tự lấy ra xử lý.
    {
        std::lock_guard<std::mutex> lock(m_eventMutex);
        m_events.push_back(event);
    }
    PostMessage(WM_NET_EVENT);
}

// Đọc thông tin kết nối rồi khởi động client hoặc server.
void ChatDlg::OnConnect() {
    CString sName, sIp, sPort;
    GetDlgItemTextW(IDC_NAME, sName);
    GetDlgItemTextW(IDC_IP, sIp);
    GetDlgItemTextW(IDC_PORT, sPort);
    sName.Trim(); sIp.Trim();
    wchar_t* end = nullptr;
    long port = wcstol(sPort, &end, 10);
    if (sName.IsEmpty() || sName.GetLength() > 64 || end == sPort.GetString() ||
        *end != L'\0' || port < 1 || port > 65535) {
        MessageBoxW(L"Nhập tên và port từ 1 đến 65535.");
        return;
    }
    m_client.Stop(); m_server.Stop();
    OnNetEvent(0, 0);
    m_sCurrentUser = sName;
    m_users.clear(); m_messages.clear(); m_rows.clear(); m_selected = 0;
    DisplayUsers(); DisplayConversation();
    SetDlgItemTextW(IDC_DETAIL, L"");
    SetRunning(true);
    try {
        auto notify = [this](const NetEvent& event) { NotifyEvent(event); };
#ifdef CHAT_SERVER
        int error = 0;
        if (!m_server.Start(static_cast<unsigned short>(port), ToUtf8(sName.GetString()), notify, error)) {
            CString sError;
            sError.Format(L"Không lắng nghe được. Mã Winsock: %d", error);
            SetDlgItemTextW(IDC_STATUS, sError);
            SetRunning(false);
            return;
        }
        SetDlgItemTextW(IDC_STATUS, L"Đang lắng nghe. Client nhập địa chỉ IPv4 của máy này.");
#else
        SetDlgItemTextW(IDC_STATUS, L"Đang kết nối...");
        m_client.Start(ToUtf8(sIp.GetString()), static_cast<unsigned short>(port),
            ToUtf8(sName.GetString()), notify);
#endif
    }
    catch (const std::exception&) {
        SetRunning(false);
        SetDlgItemTextW(IDC_STATUS, L"Không khởi tạo được kết nối.");
    }
}

// Dừng kết nối đang chạy và cập nhật giao diện.
void ChatDlg::OnDisconnect() {
    m_client.Stop(); m_server.Stop();
    OnNetEvent(0, 0);
    SetRunning(false);
    SetDlgItemTextW(IDC_STATUS, L"Đã ngắt kết nối. Tin còn trong hàng đợi chưa được gửi.");
}

// Dừng các luồng mạng trước khi đóng dialog.
void ChatDlg::OnCancel() {
    m_client.Stop(); m_server.Stop();
    CDialog::OnCancel();
}

// Lấy tin từ ô nhập và đưa vào hàng đợi gửi.
void ChatDlg::OnSend() {
    CString sContent;
    GetDlgItemTextW(IDC_INPUT, sContent);
    if (sContent.IsEmpty()) return;
    try {
        std::string messageGui = ToUtf8(sContent.GetString());
#ifdef CHAT_SERVER
        bool checkSend = m_server.QueueMessage(m_selected, messageGui);
#else
        bool checkSend = m_client.QueueMessage(messageGui);
#endif
        if (!checkSend) {
            MessageBoxW(L"Không xếp được tin để gửi: mất kết nối, chưa chọn người nhận, "
                L"hàng đợi đầy hoặc nội dung vượt giới hạn 16 MiB.");
            return;
        }
        SetDlgItemTextW(IDC_INPUT, L"");
        SetDlgItemTextW(IDC_STATUS, L"Đã xếp tin vào hàng đợi gửi.");
    }
    catch (...) { MessageBoxW(L"Nội dung Unicode không hợp lệ hoặc thiếu bộ nhớ."); }
}

// Lấy sự kiện mạng từ hàng đợi để cập nhật danh sách và tin nhắn.
LRESULT ChatDlg::OnNetEvent(WPARAM, LPARAM) {
    std::deque<NetEvent> events;
    {
        std::lock_guard<std::mutex> lock(m_eventMutex);
        events.swap(m_events);
    }
    for (const auto& event : events) {
        CString name(FromUtf8(event.name).c_str());
        if (event.type == EventType::Connected) {
            m_users.push_back({ event.clientId, name, true });
            if (!m_selected) m_selected = event.clientId;
            DisplayUsers();
            DisplayConversation();
            SetDlgItemTextW(IDC_STATUS, L"Đã kết nối với " + name);
        }
        else if (event.type == EventType::Disconnected) {
            for (auto& user : m_users) if (user.id == event.clientId) user.online = false;
            DisplayUsers();
            SetDlgItemTextW(IDC_STATUS, name + L" đã ngắt kết nối. Tin chưa xác nhận có thể chưa tới nơi.");
        }
        else if (event.type == EventType::Received || event.type == EventType::Sent) {
            SYSTEMTIME now{};
            GetLocalTime(&now);
            ChatMessage message;
            message.clientId = event.clientId;
            message.daGui = event.type == EventType::Sent;
            message.sSender = message.daGui ? m_sCurrentUser : name;
            message.sContent = FromUtf8(event.text).c_str();
            message.sTime.Format(L"%02d:%02d:%02d", now.wHour, now.wMinute, now.wSecond);
            m_messages.push_back(std::move(message));
            if (m_selected == event.clientId) DisplayConversation();
            SetDlgItemTextW(IDC_STATUS, event.type == EventType::Sent
                ? L"Tin gửi đã được xác nhận." : L"Đã nhận tin từ " + name);
        }
        else if (event.type == EventType::Status)
            SetDlgItemTextW(IDC_STATUS, FromUtf8(event.text).c_str());
        else if (event.type == EventType::Stopped) SetRunning(false);
    }
    return 0;
}

// Vẽ lại danh sách người dùng và trạng thái online.
void ChatDlg::DisplayUsers() {
    m_userList.DeleteAllItems();
    for (size_t i = 0; i < m_users.size(); ++i) {
        const auto& user = m_users[i];
        CString label;
        label.Format(L"%s #%llu%s", user.name.GetString(), user.id, user.online ? L"" : L" (offline)");
        m_userList.InsertItem(static_cast<int>(i), label);
        if (user.id == m_selected)
            m_userList.SetItemState(static_cast<int>(i), LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    }
    SetRunning(m_running);
}

// Vẽ lại hội thoại với người dùng đang được chọn.
void ChatDlg::DisplayConversation() {
    m_chatList.DeleteAllItems();
    m_rows.clear();
    for (size_t i = 0; i < m_messages.size(); ++i) {
        const auto& message = m_messages[i];
        if (message.clientId != m_selected) continue;
        CString preview = message.sContent.Left(180);
        preview.Replace(L"\r", L" "); preview.Replace(L"\n", L" ");
        if (message.sContent.GetLength() > 180) preview += L"...";
        CString line = message.sTime + L" - " + message.sSender + L": " + preview;
        int row = m_chatList.InsertItem(m_chatList.GetItemCount(), L"");
        m_chatList.SetItemText(row, message.daGui ? 1 : 0, line);
        m_rows.push_back(i);
    }
    if (m_chatList.GetItemCount() > 0) m_chatList.EnsureVisible(m_chatList.GetItemCount() - 1, FALSE);
}

// Đổi hội thoại khi chọn người dùng khác.
void ChatDlg::OnUserChanged(NMHDR* header, LRESULT* result) {
    auto* item = reinterpret_cast<NMLISTVIEW*>(header);
    if ((item->uChanged & LVIF_STATE) && (item->uNewState & LVIS_SELECTED) &&
        item->iItem >= 0 && static_cast<size_t>(item->iItem) < m_users.size()) {
        m_selected = m_users[item->iItem].id;
        DisplayConversation();
        SetDlgItemTextW(IDC_DETAIL, L"");
        SetRunning(m_running);
    }
    *result = 0;
}

// Hiển thị đầy đủ nội dung của dòng tin nhắn được chọn.
void ChatDlg::OnMessageChanged(NMHDR* header, LRESULT* result) {
    auto* item = reinterpret_cast<NMLISTVIEW*>(header);
    if ((item->uChanged & LVIF_STATE) && (item->uNewState & LVIS_SELECTED) &&
        item->iItem >= 0 && static_cast<size_t>(item->iItem) < m_rows.size())
        SetDlgItemTextW(IDC_DETAIL, m_messages[m_rows[item->iItem]].sContent);
    *result = 0;
}
