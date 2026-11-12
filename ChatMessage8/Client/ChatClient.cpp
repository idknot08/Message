#include "pch.h"
#include "ChatClient.h"
#include "ChatDlg.h"

CChatClientApp theApp;

// Khởi tạo MFC và phiên, mở dialog tài khoản/chat rồi lặp lại khi cần chuyển màn hình.
BOOL CChatClientApp::InitInstance() {
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_LISTVIEW_CLASSES};
    InitCommonControlsEx(&controls);
    CWinApp::InitInstance();
    if (!AfxInitRichEdit2()) { AfxMessageBox(L"Không khởi tạo được Rich Edit 2.0."); return FALSE; }
    ClientSession session;
    INT_PTR result;
    do {
        ChatDlg dialog(session);
        m_pMainWnd = &dialog;
        result = dialog.DoModal();
        session.Attach(nullptr);
        m_pMainWnd = nullptr;
    } while (result == IDRETRY);
    return FALSE;
}
