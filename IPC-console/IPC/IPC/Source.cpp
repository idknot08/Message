
#include <Windows.h>
#include <iostream>
#include <string>
#include <cstring>

#pragma comment(lib, "User32.lib")

using namespace std;

// Hàm báo lỗi đã có trong IPC.cpp.
void printError(const wchar_t* message, DWORD error);

constexpr wchar_t WINDOW_CLASS[] = L"IPC.CopyData.Demo.Window";
constexpr wchar_t FIRST_TITLE[] = L"IPC.CopyData.Demo.First";
constexpr wchar_t SECOND_TITLE[] = L"IPC.CopyData.Demo.Second";
constexpr ULONG_PTR TEXT_MESSAGE = 1;
constexpr DWORD MAX_TEXT_BYTES = 64 * 1024;
constexpr UINT WM_TEXT_READY = WM_APP + 1;

struct CopyDataState {
	HWND hPeer = nullptr;       // HWND bên kia để gửi phản hồi
	wstring messageNhan;       // Bản sao nội dung đã nhận
	bool daNhanMessage = false; // Đã nhận được tin nhắn chưa?
};

// Windows gọi hàm này khi cửa sổ của mình nhận message.
LRESULT CALLBACK CopyDataWindowProc(HWND hWindow, UINT message, WPARAM wParam, LPARAM lParam) {
	if (message == WM_NCCREATE) {
		CREATESTRUCTW* ptrCreate = reinterpret_cast<CREATESTRUCTW*>(lParam);
		SetWindowLongPtrW(
			hWindow,
			GWLP_USERDATA,
			reinterpret_cast<LONG_PTR>(ptrCreate->lpCreateParams)
		);
		return DefWindowProcW(hWindow, message, wParam, lParam);
	}

	CopyDataState* ptrState = reinterpret_cast<CopyDataState*>(
		GetWindowLongPtrW(hWindow, GWLP_USERDATA));

	if (message == WM_COPYDATA) {
		const COPYDATASTRUCT* ptrData = reinterpret_cast<const COPYDATASTRUCT*>(lParam);
		HWND hSender = reinterpret_cast<HWND>(wParam);

		if (!ptrState || !ptrData || !hSender || ptrState->daNhanMessage)
			return FALSE;
		if (ptrState->hPeer && ptrState->hPeer != hSender)
			return FALSE;
		if (ptrData->dwData != TEXT_MESSAGE || ptrData->cbData > MAX_TEXT_BYTES ||
			ptrData->cbData % sizeof(wchar_t) != 0)
			return FALSE;
		if (ptrData->cbData != 0 && ptrData->lpData == nullptr)
			return FALSE;

		// Copy ngay vì lpData chỉ hợp lệ trong lúc xử lý WM_COPYDATA.
		ptrState->messageNhan.assign(ptrData->cbData / sizeof(wchar_t), L'\0');
		if (ptrData->cbData != 0)
			memcpy(&ptrState->messageNhan[0], ptrData->lpData, ptrData->cbData);

		// Đánh thức GetMessage để vòng chờ kiểm tra daNhanMessage.
		// Đây là message nội bộ, không dùng PostMessage để gửi WM_COPYDATA.
		if (!PostMessageW(hWindow, WM_TEXT_READY, 0, 0))
			return FALSE;

		ptrState->hPeer = hSender;
		ptrState->daNhanMessage = true;
		return TRUE;
	}

	if (message == WM_TEXT_READY)
		return 0;
	if (message == WM_DESTROY) {
		PostQuitMessage(0);
		return 0;
	}

	return DefWindowProcW(hWindow, message, wParam, lParam);
}

BOOL SendCopyData(HWND hPeer, HWND hWindow, const wstring& message) {
	// 1. Kiểm tra kích thước trước khi đổi sang DWORD.
	if (message.size() > MAX_TEXT_BYTES / sizeof(wchar_t)) {
		SetLastError(ERROR_BUFFER_OVERFLOW);
		return false;
	}

	// 2. Chuẩn bị loại dữ liệu, số byte và địa chỉ chuỗi nguồn.
	DWORD messageByte = static_cast<DWORD>(message.size() * sizeof(wchar_t));
	COPYDATASTRUCT data{};
	data.dwData = TEXT_MESSAGE;
	data.cbData = messageByte;
	data.lpData = const_cast<wchar_t*>(message.c_str());

	// 3. Gửi và chờ bên kia xử lý, tối đa 5 giây.
	DWORD_PTR ketQuaNhan = FALSE; // Giá trị WindowProc bên nhận trả về
	SetLastError(ERROR_SUCCESS);
	LRESULT checkSend = SendMessageTimeoutW(
		hPeer,
		WM_COPYDATA,
		reinterpret_cast<WPARAM>(hWindow),
		reinterpret_cast<LPARAM>(&data),
		SMTO_BLOCK,
		5000,
		&ketQuaNhan
	);

	if (checkSend == 0) {
		if (GetLastError() == ERROR_SUCCESS)
			SetLastError(ERROR_GEN_FAILURE);
		return false;
	}
	if (ketQuaNhan == FALSE) {
		SetLastError(ERROR_INVALID_DATA);
		return false;
	}
	return true;
}

BOOL ReceiveCopyData(CopyDataState& state, wstring& message) {
	// 1. Chờ Windows gọi WindowProc và copy được dữ liệu.
	MSG windowMessage{};
	while (!state.daNhanMessage) {
		BOOL checkGet = GetMessageW(&windowMessage, nullptr, 0, 0);
		if (checkGet == -1)
			return false;
		if (checkGet == 0) {
			SetLastError(ERROR_OPERATION_ABORTED);
			return false;
		}
		TranslateMessage(&windowMessage);
		DispatchMessageW(&windowMessage);
	}

	// 2. Lấy nội dung và chuẩn bị cho lần nhận tiếp theo.
	message = state.messageNhan;
	state.daNhanMessage = false;
	return true;
}


int RunCopyDataMode(bool isServer) {
	// isServer chỉ xác định instance đầu tiên, nhận trước.
	HINSTANCE hInstance = GetModuleHandleW(nullptr);
	CopyDataState state{};

	// 1. Đăng ký class cửa sổ và hàm nhận message.
	WNDCLASSW windowClass{};
	windowClass.lpfnWndProc = CopyDataWindowProc;
	windowClass.hInstance = hInstance;
	windowClass.lpszClassName = WINDOW_CLASS;
	ATOM checkRegister = RegisterClassW(&windowClass);
	if (!checkRegister) {
		printError(L"RegisterClassW", GetLastError());
		return 1;
	}

	// 2. Tạo cửa sổ ẩn; tham số cuối chuyển &state đến WM_NCCREATE.
	const wchar_t* windowTitle = isServer ? FIRST_TITLE : SECOND_TITLE;
	HWND hWindow = CreateWindowExW(
		0,
		WINDOW_CLASS,
		windowTitle,
		0,             // Không có WS_VISIBLE nên cửa sổ không hiện lên
		0, 0, 0, 0,    // Vị trí và kích thước
		nullptr,
		nullptr,
		hInstance,
		&state
	);
	if (!hWindow) {
		printError(L"CreateWindowExW", GetLastError());
		UnregisterClassW(WINDOW_CLASS, hInstance);
		return 1;
	}

	// 3. Instance sau tìm instance đầu. Instance đầu biết HWND bên kia qua wParam.
	if (!isServer) {
		for (int attempt = 0; attempt < 100 && !state.hPeer; ++attempt) {
			state.hPeer = FindWindowW(WINDOW_CLASS, FIRST_TITLE);
			if (!state.hPeer)
				Sleep(100);
		}
		if (!state.hPeer) {
			wcerr << L"Không tìm thấy cửa sổ instance đầu sau khoảng 10 giây.\n";
			DestroyWindow(hWindow);
			UnregisterClassW(WINDOW_CLASS, hInstance);
			return 1;
		}
	}

	wcout << (isServer ? L"Instance 1: nhan truoc.\n" : L"Instance 2: gui truoc.\n");
	wcout << L"Nhap /quit khi den luot gui de thoat.\n";

	// 4. Cả hai instance dùng chung logic gửi và nhận luân phiên.
	bool denLuotGui = !isServer;
	int result = 0;
	while (1) {
		wstring message;
		if (denLuotGui) {
			wcout << L"User1: " << flush;
			if (!getline(wcin, message))
				message = L"/quit";

			BOOL checkSend = SendCopyData(state.hPeer, hWindow, message);
			if (!checkSend) {
				printError(L"SendCopyData", GetLastError());
				result = 1;
				break;
			}
		}
		else {
			BOOL checkRead = ReceiveCopyData(state, message);
			if (!checkRead) {
				printError(L"ReceiveCopyData", GetLastError());
				result = 1;
				break;
			}
			wcout << L"User2: " << message << L'\n';
		}

		if (message == L"/quit")
			break;
		denLuotGui = !denLuotGui;
	}

	// 5. HWND dùng DestroyWindow; roleMutex vẫn do wmain đóng.
	if (IsWindow(hWindow))
		DestroyWindow(hWindow);
	UnregisterClassW(WINDOW_CLASS, hInstance);
	return result;
}
