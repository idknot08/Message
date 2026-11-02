#pragma once
#include <vector>
// CAuthDlg dialog

struct Account {
	CString sUserName;
	CString sPass;
};

class CAuthDlg : public CDialogEx
{
	DECLARE_DYNAMIC(CAuthDlg)

public:
	CAuthDlg(CWnd* pParent = nullptr);   // standard constructor
	virtual ~CAuthDlg();
	void UpdateAuthUI();
	BOOL m_bRegisterMode = FALSE;
	std::vector<Account> accounts;
	void dumpAccount();
	BOOL checkAccount(
		const CString& sUserName,
		const CString& sPass
	);

	BOOL addAccount(
		const CString& sUserName,
		const CString& sPass
	);


// Dialog Data
#ifdef AFX_DESIGN_TIME
	enum { IDD = IDD_DIALOG1 };
#endif

protected:
	virtual void DoDataExchange(CDataExchange* pDX);    // DDX/DDV support
	virtual BOOL OnInitDialog();
	CString m_filePath = L"D:\\DEV\\DEVing\\TTC\\tuan3\\account.txt";


	DECLARE_MESSAGE_MAP()
public:
	afx_msg void OnEnChangeEdit1();
	afx_msg void OnBnClickedLogin();
	afx_msg void OnBnClickedRegister();
	afx_msg void OnStnClickedStaticA();
	afx_msg void OnStnClickedStaticB();
	afx_msg void OnBnClickedButtonMode();

private:
	CString m_sLoggedInUser;

public:
	CString GetLoggedInUser() const;

};
