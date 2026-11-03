#include "pch.h"
#include "ChatClient.h"
#include "ChatDlg.h"

CChatClientApp theApp;

BOOL CChatClientApp::InitInstance() {
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_LISTVIEW_CLASSES};
    InitCommonControlsEx(&controls);
    CWinApp::InitInstance();
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
