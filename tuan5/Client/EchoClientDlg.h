#pragma once
#include "framework.h"
#include "resource.h"
#include <string>
#include <thread>

class CEchoClientDlg : public CDialog {
public:
    explicit CEchoClientDlg(CWnd* parent = nullptr);
    ~CEchoClientDlg() override;
    enum { IDD = IDD_ECHO };

protected:
    BOOL OnInitDialog() override;
    void OnOK() override;
    void OnCancel() override;
    afx_msg void OnSend();
    afx_msg LRESULT OnFinished(WPARAM, LPARAM);
    DECLARE_MESSAGE_MAP()

private:
    std::thread worker_;
    std::string reply_;
    int error_ = 0;
    bool closing_ = false;
};
