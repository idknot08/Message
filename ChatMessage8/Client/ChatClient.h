#pragma once
#include "framework.h"

class CChatClientApp : public CWinApp {
public:
    BOOL InitInstance() override;
};

extern CChatClientApp theApp;
