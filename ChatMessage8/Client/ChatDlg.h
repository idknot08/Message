#pragma once
#include "framework.h"
#include <deque>
#include <mutex>
#include "ChatConnection.h"
#include "resource.h"

constexpr UINT WM_CHAT_EVENT = WM_APP + 1;
struct ClientSession {
    ClientSession();
    ~ClientSession();
    void Attach(HWND window);
    bool Next(ChatEvent& event);
    ChatConnection connection;
    int64_t userId = 0;
    CString username, host = L"127.0.0.1", port = L"8080", status;
private:
    std::mutex mutex_;
    std::deque<ChatEvent> events_;
    HWND window_ = nullptr;
};

class ChatDlg : public CDialog {
public:
    explicit ChatDlg(ClientSession& session) : CDialog(session.userId ? IDD_CHAT : IDD_AUTH),
        session_(session), auth_(!session.userId), userId_(session.userId) {}
protected:
    BOOL OnInitDialog() override;
    void OnCancel() override;
    void OnOK() override;
    afx_msg void OnCheck();
    afx_msg void OnMode();
    afx_msg void OnLogin();
    afx_msg void OnLogout();
    afx_msg void OnSend();
    afx_msg void OnRefresh();
    afx_msg void OnUserChanged(NMHDR*, LRESULT*);
    afx_msg void OnMessageChanged(NMHDR*, LRESULT*);
    afx_msg LRESULT OnChatEvent(WPARAM, LPARAM);
    DECLARE_MESSAGE_MAP()
private:
    bool ReadEndpoint(std::string& host, int& port);
    void SwitchScreen();
    void Authenticate(bool registration);
    void UpdateControls();
    void ShowUsers();
    void ShowMessages();
    ClientSession& session_;
    bool auth_;
    bool registration_ = false;
    std::vector<ChatUser> users_;
    std::vector<ChatMessage> messages_;
    CListCtrl userList_;
    CListCtrl chatList_;
    int64_t userId_ = 0;
    int64_t selected_ = 0;
    bool busy_ = false;
    bool renderingUsers_ = false;
};
