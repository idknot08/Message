#include <Windows.h>

#include <iostream>
#include <string>

// lỗi tiếng việt
#include <fcntl.h>
#include <io.h>

using namespace std;

// Role: server : đọc phản hổi rồi gửi 
// Role: client : gửi rồi chờ phản hổi
// Process tự coi chính nó là server

constexpr wchar_t PIPE_NAME[] = L"\\\\.\\pipe\\TestPipe";
constexpr wchar_t PIPE_MUTEX_NAME[] = L"TestMutex";
constexpr wchar_t COPYDATA_MUTEX_NAME[] =L"Local\\IPC_CopyData_RoleMutex";

constexpr DWORD MAX_PAYLOAD_BYTES = 1024 * 1024;
constexpr DWORD PIPE_BUFFER_BYTES = 4096;

void printError(const wchar_t* message, DWORD error) {
	wcerr << "Chương trình gặp lỗi " << message << "Mã Lỗi " << error <<L"\n";
}

BOOL ReadExact(HANDLE hPipe, void* ptrNhanByte, DWORD nByteCanDoc) { // dùng void để tương thích với số cách kiểu dữ liệu khác khi truyền vào
	// chuẩn hóa dữ liệu
	BYTE* current = static_cast<BYTE*> (ptrNhanByte); 

	//khởi tạo các dữ liệu cần thiết
	DWORD byteDaDoc = 0;

	// logic chính của hàm : XỬ lý vấn đề ReadFile 1 lần không thể đọc hết số Byte cần đọc
	while(byteDaDoc < nByteCanDoc){
		DWORD nByteDaDocTrong1While = 0;

		BOOL checkRead = ReadFile(
				hPipe,
				current + byteDaDoc, // chuyển đến địa chỉ vùng nhớ tiếp theo cần ghi
				nByteCanDoc - byteDaDoc, // sô byte max có thể đọc
				&nByteDaDocTrong1While, // số byte thực tế đọc được
				nullptr
			);
		if (!checkRead) return false;
		if (nByteDaDocTrong1While == 0) {
			SetLastError(ERROR_BROKEN_PIPE);
			return false;
		}

		byteDaDoc += nByteDaDocTrong1While;
	}
	return true;
}

BOOL SendText(HANDLE hPipe, const wstring& message) { // & có nghĩa là không tạo bản sao mà truy cập vào vùng nhớ gốc, không phải có thể sửa
	// Lấy kích thước message để ghi lần 1
	DWORD messageByte = static_cast<DWORD>(message.size()  * sizeof(wchar_t));
	DWORD NumberOfBytesWritten1 = 0;

	// Gửi header
	BOOL header = WriteFile(
		hPipe,
		&messageByte,
		sizeof(messageByte),
		&NumberOfBytesWritten1,
		nullptr
	);

	if (!header || NumberOfBytesWritten1 != sizeof(messageByte)) return false;
	
	// Gui noi dung
	DWORD NumberOfBytesWritten2 = 0;

	BOOL content = WriteFile(
		hPipe,
		message.c_str(),
		messageByte,
		&NumberOfBytesWritten2,
		nullptr
	);

	return  content && NumberOfBytesWritten2 == messageByte;
}

BOOL ReceiveText(HANDLE hPipe, wstring& message) {
	// Doc header
	DWORD nByteCanDocCon = 0; // output
	BOOL checkRead1 = ReadExact(hPipe, &nByteCanDocCon, sizeof(nByteCanDocCon));
	if (!checkRead1) return false;

	// kiem tra kich thuoc 
	if (nByteCanDocCon > MAX_PAYLOAD_BYTES ||
		nByteCanDocCon % sizeof(wchar_t) != 0) {
		SetLastError(ERROR_INVALID_DATA);
		return false;
	}

	// Khai báo 1 chuỗi ký tự
	DWORD sizeMessage = nByteCanDocCon / sizeof(wchar_t);
	message.assign(sizeMessage, L'\0');

	if (nByteCanDocCon == 0) return true;

	return  ReadExact(hPipe, &message[0], nByteCanDocCon);
}

BOOL RunServer() {
	HANDLE hPipe = CreateNamedPipeW(
		PIPE_NAME,
		PIPE_ACCESS_DUPLEX,
		PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
		3,
		PIPE_BUFFER_BYTES,
		PIPE_BUFFER_BYTES,
		5000,
		nullptr
	);
	if (hPipe == INVALID_HANDLE_VALUE) return false;

	wcout << L"Dang lang nghe (User 1)\n";
	wcout << L"Waiting for connector... \n";

	BOOL checkConect = ConnectNamedPipe(hPipe, nullptr) ? FALSE : (GetLastError() == ERROR_PIPE_CONNECTED);
	if (!checkConect){
		printError(L"ConnectNamedPipe", GetLastError());
		CloseHandle(hPipe);
		return 1;
	}

	wcout << L"Connected! \n";

	while (1) {
		// Server nhận trước, sau đó phản hồi.

		std:wstring received;
		if (!ReceiveText(hPipe, received)) {
			printError(L"ReceiveText", GetLastError());
			break;
		}

		std::wcout << L"\nUser2: " << received << L'\n';

		if (received == L"/quit") {
			break;
		}

		wcout << L"You :";

		wstring response;
		if (!getline(wcin, response)) {
			response = L"/quit";
		}

		if (!SendText(hPipe, response)) {
			printError(L"SendText", GetLastError());
			break;
		}
		if (response == L"/quit") {
			break;
		}
	}

	FlushFileBuffers(hPipe);
	DisconnectNamedPipe(hPipe);
	CloseHandle(hPipe);
}

HANDLE ConnectToServer() {
	// Retry trong 10 giây nếu server chưa tạo xong pipe.
	for (int attempt = 0; attempt < 100; ++attempt)
	{
		HANDLE pipe = CreateFileW(
			PIPE_NAME,
			GENERIC_READ | GENERIC_WRITE,
			0,
			nullptr,
			OPEN_EXISTING,
			0,
			nullptr
		);

		if (pipe != INVALID_HANDLE_VALUE)
			return pipe;

		DWORD error = GetLastError();

		if (error == ERROR_FILE_NOT_FOUND)
		{
			Sleep(100);
			continue;
		}

		if (error == ERROR_PIPE_BUSY)
		{
			if (!WaitNamedPipeW(PIPE_NAME, 5000))
			{
				printError(L"WaitNamedPipeW", GetLastError());
				return INVALID_HANDLE_VALUE;
			}

			continue;
		}

		printError(L"CreateFileW", error);
		return INVALID_HANDLE_VALUE;
	}

	std::wcerr << L"Timed out waiting for server.\n";
	return INVALID_HANDLE_VALUE;
}

int RunClient()
{
	std::wcout << L"Role: Connector (User 2)\n";
	std::wcout << L"Connecting...\n";

	HANDLE pipe = ConnectToServer();

	if (pipe == INVALID_HANDLE_VALUE)
		return 1;

	std::wcout << L"Connected.\n";
	std::wcout << L"Enter /quit to stop.\n";

	// Client gửi trước, sau đó chờ phản hồi.
	while (true)
	{
		std::wcout << L"You: ";

		std::wstring message;

		if (!std::getline(std::wcin, message))
			message = L"/quit";

		if (!SendText(pipe, message))
		{
			printError(L"SendText", GetLastError());
			break;
		}

		if (message == L"/quit")
			break;

		std::wstring response;

		if (!ReceiveText(pipe, response))
		{
			printError(L"ReceiveText", GetLastError());
			break;
		}

		std::wcout << L"\nUser1: " << response << L'\n';

		if (response == L"/quit")
			break;
	}

	CloseHandle(pipe);
	return 0;
}

int RunPipeMode(bool isServer)
{
	return isServer ? RunServer() : RunClient();
}


int RunCopyDataMode(bool isServer);


int wmain(int argc, wchar_t* argv[])
{
	_setmode(_fileno(stdin), _O_U16TEXT);
	_setmode(_fileno(stdout), _O_U16TEXT);
	_setmode(_fileno(stderr), _O_U16TEXT);

	if (argc < 2)
	{
		std::wcout << L"Command: IPC.exe pipe | copydata\n";
		return 1;
	}

	std::wstring mode = argv[1];
	const wchar_t* mutexName = nullptr;

	if (mode == L"pipe")
	{
		mutexName = PIPE_MUTEX_NAME;
	}
	else if (mode == L"copydata")
	{
		mutexName = COPYDATA_MUTEX_NAME;
	}
	else
	{
		std::wcerr << L"Unknown IPC mode: " << mode << L'\n';
		return 1;
	}

	HANDLE roleMutex =
		CreateMutexW(nullptr, FALSE, mutexName);

	if (roleMutex == nullptr)
	{
		printError(L"CreateMutexW", GetLastError());
		return 1;
	}

	// Phải đọc ngay sau CreateMutexW.
	bool isServer =
		GetLastError() != ERROR_ALREADY_EXISTS;

	int result = 0;

	if (mode == L"pipe")
		result = RunPipeMode(isServer);
	else
		result = RunCopyDataMode(isServer);

	// Giữ mutex tồn tại cho đến khi mode kết thúc.
	CloseHandle(roleMutex);

	return result;
}