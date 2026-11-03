#pragma once
#include "framework.h"

class CEchoClientApp : public CWinApp {
public:
    BOOL InitInstance() override;
};

extern CEchoClientApp theApp;
