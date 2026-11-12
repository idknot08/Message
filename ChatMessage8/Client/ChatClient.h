#pragma once
#include "framework.h"

class CChatClientApp : public CWinApp {
public:
    // Khởi tạo MFC và phiên, mở dialog tài khoản/chat rồi lặp lại khi cần chuyển màn hình.
    BOOL InitInstance() override;
};

extern CChatClientApp theApp;
