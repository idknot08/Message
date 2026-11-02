#include <vector>
#include "ChatIpc.h"
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
	void SetUsers(const std::vector<CString>& users);

private:
    CString m_sCurrentUser;
    CString m_sSelectedUser;

    std::vector<CString> m_users;
    std::vector<ChatMessage> m_messages;
	ChatIpc m_ipc;
	CComboBox m_ipcMode;

    void InitUserList();
    void InitChatList();
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
	afx_msg void OnDestroy();
	afx_msg LRESULT OnChatReceived(WPARAM, LPARAM data);
	afx_msg BOOL OnCopyData(CWnd* sender, COPYDATASTRUCT* data);

    DECLARE_MESSAGE_MAP()
};


