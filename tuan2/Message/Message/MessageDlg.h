#include <vector>
// MessageDlg.h : header file
//

#pragma once

struct ChatMessage
{
    CString sSender;
    CString sReceiver;
    CString sContent;
    CString sTime;
};


// CMessageDlg dialog
class CMessageDlg : public CDialogEx
{
public:
    CMessageDlg(CWnd* pParent = nullptr);

    void SetCurrentUser(
        const CString& sUserName
    );

private:
    CString m_sCurrentUser;
    CString m_sSelectedUser;

    std::vector<CString> m_users;
    std::vector<ChatMessage> m_messages;

    void InitUserList();
    void InitChatList();
    void LoadFakeData();
    void DisplayUsers();
    void UpdateChatHeaders();
    void DisplayConversation();

protected:
    virtual void DoDataExchange(CDataExchange* pDX);
    virtual BOOL OnInitDialog();

    HICON m_hIcon;

    afx_msg void OnSysCommand(UINT nID, LPARAM lParam);
    afx_msg void OnPaint();
    afx_msg HCURSOR OnQueryDragIcon();

    afx_msg void OnLvnItemchangedListUsers(
        NMHDR* pNMHDR,
        LRESULT* pResult
    );

    afx_msg void OnBnClickedButtonSend();

    DECLARE_MESSAGE_MAP()
};


