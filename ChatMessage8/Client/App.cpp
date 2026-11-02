#include "ChatDlg.h"
class ChatApp : public CWinApp {
public:
    BOOL InitInstance() override {
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
};
ChatApp theApp;
