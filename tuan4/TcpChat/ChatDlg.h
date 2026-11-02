#pragma once
#define VC_EXTRALEAN
#define WIN32_LEAN_AND_MEAN
#include <afxwin.h>
#include <afxcmn.h>
#include "TcpChat.h"
#include "resource.h"

constexpr UINT WM_NET_EVENT = WM_APP + 1;
struct ChatMessage {
    uint64_t clientId;
    CString sSender;
    CString sContent;
    CString sTime;
    bool daGui;
};
struct ChatUser {
    uint64_t id;
    CString name;
    bool online;
};

class ChatDlg : public CDialog {
public:
    // Gắn dialog với mẫu giao diện IDD_CHAT.
    ChatDlg() : CDialog(IDD_CHAT) {}
protected:
    BOOL OnInitDialog() override;
    void OnCancel() override;
    // Bỏ thao tác đóng dialog bằng phím Enter.
    void OnOK() override {}
    afx_msg void OnConnect();
    afx_msg void OnDisconnect();
    afx_msg void OnSend();
    afx_msg void OnUserChanged(NMHDR* header, LRESULT* result);
    afx_msg void OnMessageChanged(NMHDR* header, LRESULT* result);
    afx_msg LRESULT OnNetEvent(WPARAM, LPARAM);
    DECLARE_MESSAGE_MAP()
private:
    void NotifyEvent(const NetEvent& event);
    void DisplayUsers();
    void DisplayConversation();
    void SetRunning(bool running);
    TcpClient m_client;
    TcpServer m_server;
    std::mutex m_eventMutex;
    std::deque<NetEvent> m_events;
    std::vector<ChatUser> m_users;
    std::vector<ChatMessage> m_messages;
    std::vector<size_t> m_rows;
    CString m_sCurrentUser;
    uint64_t m_selected = 0;
    bool m_running = false;
    CListCtrl m_userList;
    CListCtrl m_chatList;
};
