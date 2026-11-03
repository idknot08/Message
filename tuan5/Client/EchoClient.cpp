#include "pch.h"
#include "EchoClient.h"
#include "EchoClientDlg.h"

namespace {
constexpr wchar_t RunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t RunName[] = L"TTCWeek5Chat";

LSTATUS RegisterAutorun() {
    wchar_t path[MAX_PATH];
    DWORD length = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (!length) return GetLastError();
    if (length == MAX_PATH) return ERROR_FILENAME_EXCED_RANGE;
    CString command;
    command.Format(L"\"%s\"", path);
    HKEY key{};
    LSTATUS error = RegCreateKeyExW(HKEY_CURRENT_USER, RunKey, 0, nullptr, 0,
        KEY_QUERY_VALUE | KEY_SET_VALUE, nullptr, &key, nullptr);
    if (error != ERROR_SUCCESS) return error;
    wchar_t existing[MAX_PATH + 3]{};
    DWORD bytes = sizeof(existing);
    error = RegGetValueW(key, nullptr, RunName, RRF_RT_REG_SZ, nullptr, existing, &bytes);
    if (error == ERROR_FILE_NOT_FOUND || error == ERROR_MORE_DATA || error == ERROR_UNSUPPORTED_TYPE ||
        (error == ERROR_SUCCESS && command.Compare(existing) != 0)) {
        error = RegSetValueExW(key, RunName, 0, REG_SZ,
            reinterpret_cast<const BYTE*>(command.GetString()),
            static_cast<DWORD>((command.GetLength() + 1) * sizeof(wchar_t)));
    }
    RegCloseKey(key);
    return error;
}

}

CEchoClientApp theApp;

BOOL CEchoClientApp::InitInstance() {
    CWinApp::InitInstance();
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data)) {
        AfxMessageBox(L"Không khởi tạo được Winsock.");
        return FALSE;
    }
    LSTATUS error = RegisterAutorun();
    if (error) {
        CString notice;
        notice.Format(L"Không đăng ký được autorun (lỗi %ld).", error);
        AfxMessageBox(notice);
    }
    CEchoClientDlg dialog;
    m_pMainWnd = &dialog;
    dialog.DoModal();
    m_pMainWnd = nullptr;
    WSACleanup();
    return FALSE;
}
