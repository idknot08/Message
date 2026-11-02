#include "ChatDlg.h"

#pragma comment(lib, "Advapi32.lib")

// Đăng ký chạy cùng Windows cho exe hiện tại nếu chưa có trong Registry.
static LSTATUS DangKyAutoRun() {
#ifdef CHAT_SERVER
    const wchar_t* tenApp = L"TTCChatServer";
#else
    const wchar_t* tenApp = L"TTCChatClient";
#endif
    HKEY hKey = nullptr;
    LSTATUS ketQua = RegCreateKeyExW(HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
        0, nullptr, 0, KEY_QUERY_VALUE | KEY_SET_VALUE, nullptr, &hKey, nullptr);
    if (ketQua != ERROR_SUCCESS) return ketQua;

    // Có đăng ký rồi thì giữ nguyên.
    DWORD soByte = 0;
    ketQua = RegQueryValueExW(hKey, tenApp, nullptr, nullptr, nullptr, &soByte);
    if (ketQua == ERROR_FILE_NOT_FOUND) {
        wchar_t duongDan[MAX_PATH]{};
        DWORD doDai = GetModuleFileNameW(nullptr, duongDan, MAX_PATH);
        if (doDai == 0) {
            ketQua = GetLastError();
        }
        else if (doDai >= MAX_PATH || doDai + 2 > 260) {
            ketQua = ERROR_FILENAME_EXCED_RANGE;
        }
        else {
            CString lenh;
            lenh.Format(L"\"%s\"", duongDan);
            ketQua = RegSetValueExW(hKey, tenApp, 0, REG_SZ,
                reinterpret_cast<const BYTE*>(lenh.GetString()),
                static_cast<DWORD>((lenh.GetLength() + 1) * sizeof(wchar_t)));
        }
    }
    RegCloseKey(hKey);
    return ketQua;
}

class ChatApp : public CWinApp {
public:
    // Khởi tạo MFC, kiểm tra autorun rồi mở cửa sổ chat.
    BOOL InitInstance() override {
        INITCOMMONCONTROLSEX controls{ sizeof(controls), ICC_LISTVIEW_CLASSES };
        InitCommonControlsEx(&controls);
        CWinApp::InitInstance();
        LSTATUS ketQua = DangKyAutoRun();
        if (ketQua != ERROR_SUCCESS) {
            CString thongBao;
            thongBao.Format(L"Không thể đăng ký autorun (mã lỗi %ld).", ketQua);
            AfxMessageBox(thongBao);
        }
        ChatDlg dlg;
        m_pMainWnd = &dlg;
        dlg.DoModal();
        m_pMainWnd = nullptr;
        return FALSE;
    }
};
ChatApp theApp;
