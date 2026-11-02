
// MessageDlg.cpp : implementation file
//

#include "pch.h"
#include "framework.h"
#include "Message.h"
#include "MessageDlg.h"
#include "afxdialogex.h"
#include <memory>

#ifdef _DEBUG
#define new DEBUG_NEW
#endif


// CAboutDlg dialog used for App About

class CAboutDlg : public CDialogEx
{
public:
	CAboutDlg();

// Dialog Data
#ifdef AFX_DESIGN_TIME
	enum { IDD = IDD_ABOUTBOX };
#endif

	protected:
	virtual void DoDataExchange(CDataExchange* pDX);    // DDX/DDV support

// Implementation
protected:
	DECLARE_MESSAGE_MAP()
};

CAboutDlg::CAboutDlg() : CDialogEx(IDD_ABOUTBOX)
{
}

void CAboutDlg::DoDataExchange(CDataExchange* pDX)
{
	CDialogEx::DoDataExchange(pDX);
}

BEGIN_MESSAGE_MAP(CAboutDlg, CDialogEx)
END_MESSAGE_MAP()


// CMessageDlg dialog



CMessageDlg::CMessageDlg(CWnd* pParent /*=nullptr*/)
	: CDialogEx(IDD_MESSAGE_DIALOG, pParent)
{
	m_hIcon = AfxGetApp()->LoadIcon(IDR_MAINFRAME);
}

void CMessageDlg::DoDataExchange(CDataExchange* pDX)
{
	CDialogEx::DoDataExchange(pDX);
}

BEGIN_MESSAGE_MAP(CMessageDlg, CDialogEx)
	ON_WM_SYSCOMMAND()
	ON_WM_PAINT()
	ON_WM_QUERYDRAGICON()
	ON_WM_DESTROY()
	ON_WM_COPYDATA()
	ON_MESSAGE(WM_CHAT_RECEIVED, &CMessageDlg::OnChatReceived)
	ON_NOTIFY(
		LVN_ITEMCHANGED,
		IDC_LIST_USERS,
		&CMessageDlg::OnLvnItemchangedListUsers
	)

	ON_BN_CLICKED(
		IDC_BUTTON_SEND,
		&CMessageDlg::OnBnClickedButtonSend
	)
END_MESSAGE_MAP()


// CMessageDlg message handlers

BOOL CMessageDlg::OnInitDialog()
{
	CDialogEx::OnInitDialog();

	// Add "About..." menu item to system menu.

	// IDM_ABOUTBOX must be in the system command range.
	ASSERT((IDM_ABOUTBOX & 0xFFF0) == IDM_ABOUTBOX);
	ASSERT(IDM_ABOUTBOX < 0xF000);

	CMenu* pSysMenu = GetSystemMenu(FALSE);
	if (pSysMenu != nullptr)
	{
		BOOL bNameValid;
		CString strAboutMenu;
		bNameValid = strAboutMenu.LoadString(IDS_ABOUTBOX);
		ASSERT(bNameValid);
		if (!strAboutMenu.IsEmpty())
		{
			pSysMenu->AppendMenu(MF_SEPARATOR);
			pSysMenu->AppendMenu(MF_STRING, IDM_ABOUTBOX, strAboutMenu);
		}
	}

	// Set the icon for this dialog.  The framework does this automatically
	//  when the application's main window is not a dialog
	SetIcon(m_hIcon, TRUE);			// Set big icon
	SetIcon(m_hIcon, FALSE);		// Set small icon

	// TODO: Add extra initialization here

	InitUserList();
	InitChatList();
	DisplayUsers();

	// Chọn cách gửi tin: pipe hoặc WM_COPYDATA.
	CRect sendRect;
	GetDlgItem(IDC_BUTTON_SEND)->GetWindowRect(&sendRect);
	ScreenToClient(&sendRect);
	CRect modeRect(sendRect.left - 145, sendRect.top - 3,
		sendRect.left - 15, sendRect.top + 160);
	m_ipcMode.Create(WS_CHILD | WS_VISIBLE | WS_TABSTOP |
		CBS_DROPDOWNLIST | WS_VSCROLL, modeRect, this, IDC_IPC_MODE);
	m_ipcMode.SetFont(GetFont());
	m_ipcMode.AddString(L"Named Pipe");
	m_ipcMode.AddString(L"WM_COPYDATA");
	m_ipcMode.SetCurSel(0);

	if (!m_ipc.Start(m_hWnd, m_sCurrentUser.GetString()))
	{
		MessageBoxW(L"Không thể mở chat: tài khoản này đang chạy ở cửa sổ khác, "
			L"hoặc tên tài khoản quá dài.", L"IPC", MB_OK | MB_ICONERROR);
		EndDialog(IDCANCEL);
		return FALSE;
	}

	if (!m_users.empty())
	{
		m_sSelectedUser = m_users[0];

		HWND hUserList = ::GetDlgItem(
			m_hWnd,
			IDC_LIST_USERS
		);

		ListView_SetItemState(
			hUserList,
			0,
			LVIS_SELECTED | LVIS_FOCUSED,
			LVIS_SELECTED | LVIS_FOCUSED
		);

		UpdateChatHeaders();
		DisplayConversation();
	}

	return TRUE;  // return TRUE  unless you set the focus to a control
}

void CMessageDlg::OnSysCommand(UINT nID, LPARAM lParam)
{
	if ((nID & 0xFFF0) == IDM_ABOUTBOX)
	{
		CAboutDlg dlgAbout;
		dlgAbout.DoModal();
	}
	else
	{
		CDialogEx::OnSysCommand(nID, lParam);
	}
}

// If you add a minimize button to your dialog, you will need the code below
//  to draw the icon.  For MFC applications using the document/view model,
//  this is automatically done for you by the framework.

void CMessageDlg::OnPaint()
{
	if (IsIconic())
	{
		CPaintDC dc(this); // device context for painting

		SendMessage(WM_ICONERASEBKGND, reinterpret_cast<WPARAM>(dc.GetSafeHdc()), 0);

		// Center icon in client rectangle
		int cxIcon = GetSystemMetrics(SM_CXICON);
		int cyIcon = GetSystemMetrics(SM_CYICON);
		CRect rect;
		GetClientRect(&rect);
		int x = (rect.Width() - cxIcon + 1) / 2;
		int y = (rect.Height() - cyIcon + 1) / 2;

		// Draw the icon
		dc.DrawIcon(x, y, m_hIcon);
	}
	else
	{
		CDialogEx::OnPaint();
	}
}

// The system calls this function to obtain the cursor to display while the user drags
//  the minimized window.
HCURSOR CMessageDlg::OnQueryDragIcon()
{
	return static_cast<HCURSOR>(m_hIcon);
}


void CMessageDlg::SetCurrentUser(
	const CString& sUserName)
{
	m_sCurrentUser = sUserName;
}

void CMessageDlg::SetUsers(const std::vector<CString>& users)
{
	m_users.clear();
	for (const CString& user : users)
		if (user != m_sCurrentUser)
			m_users.push_back(user);
}






void CMessageDlg::InitUserList()
{
	HWND hUserList = ::GetDlgItem(
		m_hWnd,
		IDC_LIST_USERS
	);

	ListView_SetExtendedListViewStyle(
		hUserList,
		LVS_EX_FULLROWSELECT |
		LVS_EX_GRIDLINES
	);

	wchar_t szTitle[] = L"Người dùng";

	LVCOLUMNW column{};
	column.mask = LVCF_TEXT | LVCF_WIDTH;
	column.pszText = szTitle;
	column.cx = 120;

	ListView_InsertColumn(
		hUserList,
		0,
		&column
	);
	RECT rect{};
	::GetClientRect(hUserList, &rect);

	int width = rect.right - rect.left;

	ListView_SetColumnWidth(hUserList, 0, width);
}

void CMessageDlg::InitChatList()
{
	HWND hChatList = ::GetDlgItem(
		m_hWnd,
		IDC_LIST_CHAT
	);

	ListView_SetExtendedListViewStyle(
		hChatList,
		LVS_EX_FULLROWSELECT |
		LVS_EX_GRIDLINES
	);

	wchar_t szOtherUser[] = L"Người đang chat";
	wchar_t szCurrentUser[] = L"Tôi";

	LVCOLUMNW column{};

	column.mask = LVCF_TEXT | LVCF_WIDTH;
	column.cx = 250;

	column.pszText = szOtherUser;
	ListView_InsertColumn(hChatList, 0, &column);

	column.pszText = szCurrentUser;
	ListView_InsertColumn(hChatList, 1, &column);

	RECT rect{};
	::GetClientRect(hChatList, &rect);

	int width = rect.right - rect.left;

	ListView_SetColumnWidth(hChatList, 0, width / 2);
	ListView_SetColumnWidth(hChatList, 1, width - width / 2);
}

void CMessageDlg::DisplayUsers()
{
	HWND hUserList = ::GetDlgItem(
		m_hWnd,
		IDC_LIST_USERS
	);

	ListView_DeleteAllItems(hUserList);

	for (int i = 0;
		i < static_cast<int>(m_users.size());
		i++)
	{
		LVITEMW item{};

		item.mask = LVIF_TEXT;
		item.iItem = i;
		item.iSubItem = 0;

		item.pszText = const_cast<LPWSTR>(
			m_users[i].GetString()
			);

		ListView_InsertItem(
			hUserList,
			&item
		);
	}
}

void CMessageDlg::UpdateChatHeaders()
{
	HWND hChatList = ::GetDlgItem(
		m_hWnd,
		IDC_LIST_CHAT
	);

	LVCOLUMNW column{};
	column.mask = LVCF_TEXT;

	column.pszText = const_cast<LPWSTR>(
		m_sSelectedUser.GetString()
		);

	ListView_SetColumn(
		hChatList,
		0,
		&column
	);

	column.pszText = const_cast<LPWSTR>(
		m_sCurrentUser.GetString()
		);

	ListView_SetColumn(
		hChatList,
		1,
		&column
	);
}


void CMessageDlg::DisplayConversation()
{
	HWND hChatList = ::GetDlgItem(
		m_hWnd,
		IDC_LIST_CHAT
	);

	ListView_DeleteAllItems(hChatList);

	int nRow = 0;

	for (const ChatMessage& message : m_messages)
	{
		BOOL bRelevant =
			(message.sSender == m_sCurrentUser &&
				message.sReceiver == m_sSelectedUser)
			||
			(message.sSender == m_sSelectedUser &&
				message.sReceiver == m_sCurrentUser);

		if (!bRelevant)
			continue;

		CString sDisplay;

		sDisplay.Format(
			L"%s - %s: %s",
			message.sTime.GetString(),
			message.sSender.GetString(),
			message.sContent.GetString()
		);

		wchar_t szEmpty[] = L"";

		LVITEMW item{};
		item.mask = LVIF_TEXT;
		item.iItem = nRow;
		item.iSubItem = 0;
		item.pszText = szEmpty;

		ListView_InsertItem(
			hChatList,
			&item
		);

		if (message.sSender == m_sSelectedUser)
		{
			ListView_SetItemText(
				hChatList,
				nRow,
				0,
				const_cast<LPWSTR>(
					sDisplay.GetString()
					)
			);
		}
		else
		{
			ListView_SetItemText(
				hChatList,
				nRow,
				1,
				const_cast<LPWSTR>(
					sDisplay.GetString()
					)
			);
		}

		nRow++;
	}
}


void CMessageDlg::OnLvnItemchangedListUsers(
	NMHDR* pNMHDR,
	LRESULT* pResult)
{
	LPNMLISTVIEW pListView =
		reinterpret_cast<LPNMLISTVIEW>(pNMHDR);

	if ((pListView->uChanged & LVIF_STATE) &&
		(pListView->uNewState & LVIS_SELECTED))
	{
		wchar_t szUserName[256]{};

		ListView_GetItemText(
			pListView->hdr.hwndFrom,
			pListView->iItem,
			0,
			szUserName,
			_countof(szUserName)
		);

		m_sSelectedUser = szUserName;

		UpdateChatHeaders();
		DisplayConversation();
	}

	*pResult = 0;
}

void CMessageDlg::OnBnClickedButtonSend()
{
	if (m_sSelectedUser.IsEmpty())
	{
		MessageBoxW(L"Chưa chọn người nhận");
		return;
	}

	HWND hEdit = ::GetDlgItem(
		m_hWnd,
		IDC_EDIT_MESSAGE
	);

	int nLength =
		::GetWindowTextLengthW(hEdit);

	if (nLength <= 0)
		return;

	std::vector<wchar_t> buffer(
		static_cast<size_t>(nLength) + 1
	);

	::GetWindowTextW(
		hEdit,
		buffer.data(),
		nLength + 1
	);

	CString sContent = buffer.data();
	sContent.Trim();

	if (sContent.IsEmpty())
		return;

	IpcChatMessage messageGui{
		m_sCurrentUser.GetString(),
		m_sSelectedUser.GetString(),
		sContent.GetString()
	};
	DWORD error = ERROR_SUCCESS;
	BOOL checkSend = m_ipcMode.GetCurSel() == 1
		? m_ipc.SendCopyData(messageGui, error)
		: m_ipc.SendPipe(messageGui, error);
	if (!checkSend)
	{
		CString sError;
		sError.Format(L"Không gửi được tới %s. Hãy mở app và đăng nhập tài khoản đó. "
			L"Mã lỗi: %lu", m_sSelectedUser.GetString(), error);
		MessageBoxW(sError, L"IPC", MB_OK | MB_ICONWARNING);
		return;
	}

	SYSTEMTIME currentTime{};
	::GetLocalTime(&currentTime);

	CString sTime;

	sTime.Format(
		L"%02d:%02d",
		currentTime.wHour,
		currentTime.wMinute
	);

	ChatMessage message;
	message.sSender = m_sCurrentUser;
	message.sReceiver = m_sSelectedUser;
	message.sContent = sContent;
	message.sTime = sTime;

	m_messages.push_back(message);

	DisplayConversation();

	::SetWindowTextW(
		hEdit,
		L""
	);
}

LRESULT CMessageDlg::OnChatReceived(WPARAM, LPARAM data)
{
	std::unique_ptr<IpcChatMessage> messageNhan(
		reinterpret_cast<IpcChatMessage*>(data));
	if (!messageNhan || messageNhan->receiver != m_sCurrentUser.GetString())
		return 0;

	SYSTEMTIME currentTime{};
	::GetLocalTime(&currentTime);
	ChatMessage message;
	message.sSender = messageNhan->sender.c_str();
	message.sReceiver = messageNhan->receiver.c_str();
	message.sContent = messageNhan->text.c_str();
	message.sTime.Format(L"%02d:%02d", currentTime.wHour, currentTime.wMinute);
	m_messages.push_back(message);
	DisplayConversation();
	return 0;
}

BOOL CMessageDlg::OnCopyData(CWnd* sender, COPYDATASTRUCT* data)
{
	if (!sender || !data || data->dwData != CHAT_COPYDATA_ID)
		return FALSE;
	IpcChatMessage messageNhan;
	if (!ChatIpc::Decode(data->lpData, data->cbData, messageNhan) ||
		messageNhan.receiver != m_sCurrentUser.GetString())
		return FALSE;

	CString sTitleCanTim;
	sTitleCanTim.Format(L"Tuan3 Chat IPC - %s", messageNhan.sender.c_str());
	CString sTitleNguoiGui;
	sender->GetWindowTextW(sTitleNguoiGui);
	if (sTitleNguoiGui != sTitleCanTim)
		return FALSE;

	IpcChatMessage* ptrMessage = new IpcChatMessage(std::move(messageNhan));
	OnChatReceived(0, reinterpret_cast<LPARAM>(ptrMessage));
	return TRUE;
}

void CMessageDlg::OnDestroy()
{
	m_ipc.Stop();
	MSG pending{};
	while (::PeekMessageW(&pending, m_hWnd, WM_CHAT_RECEIVED,
		WM_CHAT_RECEIVED, PM_REMOVE))
		delete reinterpret_cast<IpcChatMessage*>(pending.lParam);
	CDialogEx::OnDestroy();
}
