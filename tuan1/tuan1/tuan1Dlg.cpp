
// tuan1Dlg.cpp : implementation file
//

#include "pch.h"
#include "framework.h"
#include "tuan1.h"
#include "tuan1Dlg.h"
#include "afxdialogex.h"

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


// Ctuan1Dlg dialog



Ctuan1Dlg::Ctuan1Dlg(CWnd* pParent /*=nullptr*/)
	: CDialogEx(IDD_TUAN1_DIALOG, pParent)
{
	m_hIcon = AfxGetApp()->LoadIcon(IDR_MAINFRAME);
}

void Ctuan1Dlg::DoDataExchange(CDataExchange* pDX)
{
	CDialogEx::DoDataExchange(pDX);
}

BEGIN_MESSAGE_MAP(Ctuan1Dlg, CDialogEx)
	ON_WM_SYSCOMMAND()
	ON_WM_PAINT()
	ON_WM_QUERYDRAGICON()
	ON_BN_CLICKED(IDC_BUTTON_A, &Ctuan1Dlg::OnBnClickedButtonA)
	ON_EN_CHANGE(IDC_EDIT_B1, &Ctuan1Dlg::OnEnChangeEditB1)
	ON_BN_CLICKED(IDC_BUTTON_B, &Ctuan1Dlg::OnBnClickedButtonB)
END_MESSAGE_MAP()


// Ctuan1Dlg message handlers

BOOL Ctuan1Dlg::OnInitDialog()
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

	return TRUE;  // return TRUE  unless you set the focus to a control
}

void Ctuan1Dlg::OnSysCommand(UINT nID, LPARAM lParam)
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

void Ctuan1Dlg::OnPaint()
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
HCURSOR Ctuan1Dlg::OnQueryDragIcon()
{
	return static_cast<HCURSOR>(m_hIcon);
}




void Ctuan1Dlg::OnBnClickedButtonA()
{
	// TODO: Add your control notification handler code here

	CFolderPickerDialog ObCFolderPickerDialog;
	
	if (ObCFolderPickerDialog.DoModal() == IDOK) {
		m_folderPath = ObCFolderPickerDialog.GetPathName();
		m_filePath = m_folderPath + _T("\\data1.txt"); // Chuỗi theo cấu hình Unicode/ANSI.
		SetDlgItemTextW(IDC_EDIT_B1, m_filePath);
	}
	else {
		return; // dừng khi không chọn file
	}

	// Lấy nội dung cần ghi.
	CString strA;
	GetDlgItemTextW(IDC_EDIT_A, strA);

	HANDLE hFile = ::CreateFileW(
		m_filePath,
		FILE_APPEND_DATA,
		0,
		NULL,
		OPEN_ALWAYS, // ghi tiếp vào file
		FILE_ATTRIBUTE_NORMAL,
		NULL
	);

	if (hFile == INVALID_HANDLE_VALUE) // dừng khi chọn file nhưng lỗi
	{
		MessageBoxW(L"Lỗi, không thể tạo hoặc mở file");
		return;
	}

	DWORD bytesWriten = 0;
	DWORD bytesToWrite = strA.GetLength() * sizeof(__wchar_t);
	BOOL checkWrite = ::WriteFile(
		hFile,
		strA.GetString(),  
		bytesToWrite,
		&bytesWriten, 
		0 // Ghi đồng bộ, không dùng OVERLAPPED.
	);

	::CloseHandle(hFile);

	if (!checkWrite || bytesWriten != bytesToWrite) {
		MessageBoxW(L"Ghi file thất bại");
	}
	else {
		MessageBoxW(L"Ghi file thành công");
	}
}


void Ctuan1Dlg::OnEnChangeEditB1()
{
	// TODO:  If this is a RICHEDIT control, the control will not
	// send this notification unless you override the CDialogEx::OnInitDialog()
	// function and call CRichEditCtrl().SetEventMask()
	// with the ENM_CHANGE flag ORed into the mask.

	// TODO:  Add your control notification handler code here
}


void Ctuan1Dlg::OnBnClickedButtonB()
{
	// TODO: Add your control notification handler code here
	// Chọn file để đọc
	CFileDialog FileDialog(TRUE);
	if (FileDialog.DoModal() != IDOK) {
		return;
	};
	m_filePath = FileDialog.GetPathName();
	SetDlgItemText(IDC_EDIT_B1, m_filePath);

	// Tạo handle file
	HANDLE hFile = ::CreateFileW(
		m_filePath,
		GENERIC_READ,
		0,
		NULL,
		OPEN_EXISTING,
		FILE_ATTRIBUTE_NORMAL,
		NULL
		);

	if (hFile == INVALID_HANDLE_VALUE) // dừng khi chọn file nhưng lỗi
	{
		MessageBoxW(L"Lỗi khi chọn File");
		return;
	}


	// Đọc nội dung file.
	// 1. Lấy kích thước file
	
	LARGE_INTEGER lpFileSize;
	if (!GetFileSizeEx(hFile, &lpFileSize)) {
		MessageBoxW(L"Không thể lấy kích thước file");
		::CloseHandle(hFile);
		return;
	};

	if (lpFileSize.QuadPart > MAXDWORD) { // Chưa hỗ trợ đọc file lớn hơn MAXDWORD.
		MessageBoxW(L"File quá lớn, không để đọc trong 1 lần, chưa xử lý");
		::CloseHandle(hFile);
		return;
	}
	DWORD nNumberOfBytesToRead = static_cast<DWORD>(lpFileSize.QuadPart);

	// 2. Cấp phát bộ nhớ

	SIZE_T sizeMem = static_cast<SIZE_T>(nNumberOfBytesToRead) + sizeof(wchar_t);
	LPVOID buffer =  ::VirtualAlloc(
		NULL,
		sizeMem, // thêm chỗ cho ký tự kết thúc
		MEM_RESERVE | MEM_COMMIT,
		// Đặt chỗ và cấp bộ nhớ cùng lúc.
		
		PAGE_READWRITE
	);
	if (buffer == NULL)
	{
		MessageBoxW(L"Cấp phát bộ nhớ thất bại");
		CloseHandle(hFile);
		return;
	}

	// 3. Đọc file

	DWORD lpNumberOfBytesRead = 0;
	BOOL checkRead = ReadFile(
		hFile,
		buffer,
		nNumberOfBytesToRead,
		&lpNumberOfBytesRead,
		NULL
	);

	if (!checkRead || nNumberOfBytesToRead != lpNumberOfBytesRead) {
		MessageBoxW(L"Lỗi đọc file");
		::CloseHandle(hFile);
		return;
	}
	else {
		MessageBoxW(L"Đọc file thành công");

		// Hiển thị lên edit box

		LPWSTR lpString = static_cast<LPWSTR>(buffer);
		lpString [lpNumberOfBytesRead / sizeof(wchar_t)] = L'\0';
		SetDlgItemTextW(
			IDC_EDIT_A,
			lpString
		);

	}

	
	::CloseHandle(hFile);
	::VirtualFree(buffer, 0, MEM_RELEASE);
}
