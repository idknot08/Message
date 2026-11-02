// CAuthDlg.cpp : implementation file
//

#include "pch.h"
#include "Message.h"
#include "CAuthDlg.h"
#include "afxdialogex.h"
#include <string>


// CAuthDlg dialog

IMPLEMENT_DYNAMIC(CAuthDlg, CDialogEx)

CAuthDlg::CAuthDlg(CWnd* pParent /*=nullptr*/)
	: CDialogEx(IDD_DIALOG1, pParent)
{

}

CAuthDlg::~CAuthDlg()
{
}

void CAuthDlg::DoDataExchange(CDataExchange* pDX)
{
	CDialogEx::DoDataExchange(pDX);
}


BEGIN_MESSAGE_MAP(CAuthDlg, CDialogEx)
	ON_BN_CLICKED(IDC_LOGIN, &CAuthDlg::OnBnClickedLogin)
	ON_BN_CLICKED(IDC_REGISTER, &CAuthDlg::OnBnClickedRegister)
	ON_STN_CLICKED(IDC_STATIC_A, &CAuthDlg::OnStnClickedStaticA)
	ON_STN_CLICKED(IDC_STATIC_B, &CAuthDlg::OnStnClickedStaticB)
	ON_BN_CLICKED(IDC_BUTTON_MODE, &CAuthDlg::OnBnClickedButtonMode)
END_MESSAGE_MAP()


// CAuthDlg message handlers


void CAuthDlg::OnStnClickedStaticA()
{
	// TODO: Add your control notification handler code here
}


void CAuthDlg::OnStnClickedStaticB()
{
	// TODO: Add your control notification handler code here
}

BOOL CAuthDlg::OnInitDialog() {
	// Khởi tạo dialog gốc trước khi tùy chỉnh giao diện.
	CDialogEx::OnInitDialog();

	// Giao diện ban đầu khi chạy chương trình
	UpdateAuthUI();
    dumpAccount();
	return TRUE;
}


// Cập nhật giao diện đăng nhập/đăng ký.
void CAuthDlg::UpdateAuthUI() {
	if (m_bRegisterMode) {
		GetDlgItem(IDC_STATIC_A)->ShowWindow(SW_SHOW);
		GetDlgItem(IDC_EDIT_COMFIRM_PASSWORD)->ShowWindow(SW_SHOW);
		GetDlgItem(IDC_LOGIN)->ShowWindow(SW_HIDE);
		GetDlgItem(IDC_REGISTER)->ShowWindow(SW_SHOW);

		// Đổi tiêu đề.
		SetDlgItemTextW(IDC_STATIC_B, L"Đăng ký");
		SetDlgItemTextW(IDC_BUTTON_MODE, L"Đăng nhập");
	}
	else {
		GetDlgItem(IDC_STATIC_A)->ShowWindow(SW_HIDE);
		GetDlgItem(IDC_EDIT_COMFIRM_PASSWORD)->ShowWindow(SW_HIDE);
		GetDlgItem(IDC_LOGIN)->ShowWindow(SW_SHOW);
		GetDlgItem(IDC_REGISTER)->ShowWindow(SW_HIDE);

		// Đổi tiêu đề.
		SetDlgItemTextW(IDC_STATIC_B, L"Đăng nhập");
		SetDlgItemTextW(IDC_BUTTON_MODE, L"Đăng ký");
	}

}

// Đọc danh sách tài khoản từ file.
void CAuthDlg::dumpAccount()
{
    accounts.clear();

    HANDLE hFile = ::CreateFileW(
        m_filePath.GetString(),
        GENERIC_READ,
        FILE_SHARE_READ,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr
    );

    if (hFile == INVALID_HANDLE_VALUE)
    {
        // Lần đầu chạy, file chưa tồn tại thì để vector rỗng
        if (::GetLastError() == ERROR_FILE_NOT_FOUND)
            return;

        ::MessageBoxW(
            m_hWnd,
            L"Không thể mở file tài khoản",
            L"Lỗi",
            MB_OK | MB_ICONERROR
        );
        return;
    }

    LARGE_INTEGER fileSize{};

    if (!::GetFileSizeEx(hFile, &fileSize))
    {
        ::CloseHandle(hFile);
        return;
    }

    if (fileSize.QuadPart == 0)
    {
        ::CloseHandle(hFile);
        return;
    }

    if (fileSize.QuadPart > MAXDWORD ||
        fileSize.QuadPart % sizeof(wchar_t) != 0)
    {
        ::CloseHandle(hFile);

        ::MessageBoxW(
            m_hWnd,
            L"File không đúng định dạng UTF-16LE",
            L"Lỗi",
            MB_OK | MB_ICONERROR
        );
        return;
    }

    DWORD nBytesToRead =
        static_cast<DWORD>(fileSize.QuadPart);

    SIZE_T nBufferSize =
        static_cast<SIZE_T>(fileSize.QuadPart)
        + sizeof(wchar_t);

    void* pBuffer = ::VirtualAlloc(
        nullptr,
        nBufferSize,
        MEM_RESERVE | MEM_COMMIT,
        PAGE_READWRITE
    );

    if (pBuffer == nullptr)
    {
        ::CloseHandle(hFile);
        return;
    }

    DWORD nBytesRead = 0;

    BOOL bReadSuccess = ::ReadFile(
        hFile,
        pBuffer,
        nBytesToRead,
        &nBytesRead,
        nullptr
    );

    ::CloseHandle(hFile);

    if (!bReadSuccess || nBytesRead != nBytesToRead)
    {
        ::VirtualFree(pBuffer, 0, MEM_RELEASE);
        return;
    }

    wchar_t* pText = static_cast<wchar_t*>(pBuffer);
    pText[nBytesRead / sizeof(wchar_t)] = L'\0';

    std::wstring fileContent(
        pText,
        nBytesRead / sizeof(wchar_t)
    );

    ::VirtualFree(pBuffer, 0, MEM_RELEASE);

    // Bỏ BOM của file UTF-16LE
    if (!fileContent.empty() &&
        fileContent.front() == 0xFEFF)
    {
        fileContent.erase(0, 1);
    }

    size_t nStart = 0;

    while (nStart < fileContent.length())
    {
        size_t nEnd = fileContent.find(L'\n', nStart);

        std::wstring line;

        if (nEnd == std::wstring::npos)
        {
            line = fileContent.substr(nStart);
        }
        else
        {
            line = fileContent.substr(
                nStart,
                nEnd - nStart
            );
        }

        // Bỏ ký tự \r trong cặp \r\n
        if (!line.empty() && line.back() == L'\r')
        {
            line.pop_back();
        }

        size_t nSeparator = line.find(L'|');

        if (nSeparator != std::wstring::npos &&
            nSeparator > 0 &&
            nSeparator < line.length() - 1)
        {
            Account account;

            account.sUserName =
                line.substr(0, nSeparator).c_str();

            account.sPass =
                line.substr(nSeparator + 1).c_str();

            accounts.push_back(account);
        }

        if (nEnd == std::wstring::npos)
            break;

        nStart = nEnd + 1;
    }
}

// Kiểm tra tên tài khoản đã tồn tại.
BOOL CAuthDlg::checkAccount(
    const CString& sUserName,
    const CString& sPass)
{
    for (const Account& account : accounts)
    {
        if (account.sUserName == sUserName &&
            account.sPass == sPass)
        {
            return TRUE;
        }
    }

    return FALSE;
}

// Thêm tài khoản mới
BOOL CAuthDlg::addAccount(
    const CString& sUserName,
    const CString& sPass)
{
    if (sUserName.IsEmpty() || sPass.IsEmpty())
        return FALSE;

    // Không cho nhập ký tự đang dùng làm dấu phân cách
    if (sUserName.Find(L'|') >= 0 ||
        sPass.Find(L'|') >= 0 ||
        sUserName.Find(L'\r') >= 0 ||
        sUserName.Find(L'\n') >= 0 ||
        sPass.Find(L'\r') >= 0 ||
        sPass.Find(L'\n') >= 0)
    {
        return FALSE;
    }

    // Kiểm tra trùng tên tài khoản.
    for (const Account& account : accounts)
    {
        if (account.sUserName == sUserName)
        {
            return FALSE;
        }
    }

    HANDLE hFile = ::CreateFileW(
        m_filePath.GetString(),
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ,
        nullptr,
        OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr
    );

    if (hFile == INVALID_HANDLE_VALUE)
        return FALSE;

    LARGE_INTEGER fileSize{};

    if (!::GetFileSizeEx(hFile, &fileSize))
    {
        ::CloseHandle(hFile);
        return FALSE;
    }

    DWORD nBytesWritten = 0;

    // File mới: ghi BOM UTF-16LE
    if (fileSize.QuadPart == 0)
    {
        wchar_t bom = 0xFEFF;

        if (!::WriteFile(
            hFile,
            &bom,
            sizeof(bom),
            &nBytesWritten,
            nullptr))
        {
            ::CloseHandle(hFile);
            return FALSE;
        }
    }

    LARGE_INTEGER distance{};

    if (!::SetFilePointerEx(
        hFile,
        distance,
        nullptr,
        FILE_END))
    {
        ::CloseHandle(hFile);
        return FALSE;
    }

    CString sLine;
    sLine.Format(
        L"%s|%s\r\n",
        sUserName.GetString(),
        sPass.GetString()
    );

    DWORD nBytesToWrite =
        static_cast<DWORD>(
            sLine.GetLength() * sizeof(wchar_t)
            );

    nBytesWritten = 0;

    BOOL bWriteSuccess = ::WriteFile(
        hFile,
        sLine.GetString(),
        nBytesToWrite,
        &nBytesWritten,
        nullptr
    );

    ::CloseHandle(hFile);

    if (!bWriteSuccess ||
        nBytesWritten != nBytesToWrite)
    {
        return FALSE;
    }

    // Chỉ thêm vào vector sau khi ghi file thành công
    Account account;
    account.sUserName = sUserName;
    account.sPass = sPass;

    accounts.push_back(account);

    return TRUE;
}

void CAuthDlg::OnBnClickedLogin()
{
	// TODO: Add your control notification handler code here


	// Lấy thông tin từ ô nhập.
	CString strUserName, strPass;
	GetDlgItemTextW(IDC_EDIT_USERNAME, strUserName);
	GetDlgItemTextW(IDC_EDIT_PASSWORD, strPass);

    if (checkAccount(strUserName, strPass))
    {
        MessageBoxW(L"Đăng nhập thành công");
        m_sLoggedInUser = strUserName;
        EndDialog(IDOK);
        return;
    }else
        {
        MessageBoxW(L"Sai tài khoản hoặc mật khẩu");
        }
}


void CAuthDlg::OnBnClickedRegister()
{
	// TODO: Add your control notification handler code here

	CString strUserName, strPass, strCPass;
	GetDlgItemTextW(IDC_EDIT_USERNAME, strUserName);
	GetDlgItemTextW(IDC_EDIT_PASSWORD, strPass);
	GetDlgItemTextW(IDC_EDIT_COMFIRM_PASSWORD, strCPass);


	if (strPass != strCPass) {
		MessageBoxW(L"Mật khẩu không trùng khớp");
		return;
	}


    if (addAccount(strUserName, strPass))
    {
        MessageBoxW(L"Đăng ký thành công");
    }
    else
    {
        MessageBoxW(L"Tài khoản đã tồn tại hoặc không thể ghi file");
    }

}




void CAuthDlg::OnBnClickedButtonMode()
{
	m_bRegisterMode = !m_bRegisterMode;
	UpdateAuthUI();

}



CString CAuthDlg::GetLoggedInUser() const
{
    return m_sLoggedInUser;
}
