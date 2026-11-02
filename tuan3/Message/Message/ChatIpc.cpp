#include "pch.h"
#include "ChatIpc.h"

#include <cstring>
#include <vector>

namespace
{
	constexpr DWORD MESSAGE_MAGIC = 0x32544354;
	constexpr size_t MAX_PAYLOAD_BYTES = 64 * 1024;
	constexpr DWORD PIPE_TIMEOUT_MS = 1000;

	struct Header
	{
		DWORD magic;
		DWORD senderBytes;
		DWORD receiverBytes;
		DWORD textBytes;
	};

	bool ReadExact(HANDLE hPipe, void* ptrNhanByte, DWORD nByteCanDoc)
	{
		BYTE* current = static_cast<BYTE*>(ptrNhanByte);
		DWORD byteDaDoc = 0;
		while (byteDaDoc < nByteCanDoc)
		{
			DWORD nByteDaDoc = 0;
			BOOL checkRead = ::ReadFile(hPipe, current + byteDaDoc,
				nByteCanDoc - byteDaDoc, &nByteDaDoc, nullptr);
			if (!checkRead || nByteDaDoc == 0)
				return false;
			byteDaDoc += nByteDaDoc;
		}
		return true;
	}

	bool WriteExact(HANDLE hPipe, const void* ptrGuiByte, DWORD nByteCanGui)
	{
		const BYTE* current = static_cast<const BYTE*>(ptrGuiByte);
		DWORD byteDaGui = 0;
		while (byteDaGui < nByteCanGui)
		{
			DWORD nByteDaGui = 0;
			BOOL checkWrite = ::WriteFile(hPipe, current + byteDaGui,
				nByteCanGui - byteDaGui, &nByteDaGui, nullptr);
			if (!checkWrite || nByteDaGui == 0)
				return false;
			byteDaGui += nByteDaGui;
		}
		return true;
	}

	struct FindWindowData
	{
		std::wstring title;
		HWND found = nullptr;
	};

	BOOL CALLBACK FindChatWindow(HWND window, LPARAM param)
	{
		auto* search = reinterpret_cast<FindWindowData*>(param);
		int length = ::GetWindowTextLengthW(window);
		if (length != static_cast<int>(search->title.size()))
			return TRUE;
		std::vector<wchar_t> title(static_cast<size_t>(length) + 1);
		::GetWindowTextW(window, title.data(), static_cast<int>(title.size()));
		if (search->title == title.data())
		{
			search->found = window;
			return FALSE;
		}
		return TRUE;
	}
}

ChatIpc::~ChatIpc()
{
	Stop();
}

std::wstring ChatIpc::PipeName(const std::wstring& user)
{
	// Đổi tên tài khoản sang hex để dùng làm tên pipe.
	std::wstring name = L"\\\\.\\pipe\\Tuan3Chat_";
	const wchar_t hex[] = L"0123456789ABCDEF";
	for (wchar_t ch : user)
	{
		unsigned int value = static_cast<unsigned short>(ch);
		for (int shift = 12; shift >= 0; shift -= 4)
			name += hex[(value >> shift) & 15];
	}
	return name;
}

std::wstring ChatIpc::WindowTitle(const std::wstring& user)
{
	return L"Tuan3 Chat IPC - " + user;
}

bool ChatIpc::Start(HWND window, const std::wstring& user)
{
	if (!window || user.empty() || user.size() > 48 || m_userMutex)
		return false;

	m_window = window;
	m_user = user;
	std::wstring mutexName = L"Local\\" +
		PipeName(user).substr(std::wstring(L"\\\\.\\pipe\\").size());
	m_userMutex = ::CreateMutexW(nullptr, FALSE, mutexName.c_str());
	if (!m_userMutex)
		return false;
	if (::GetLastError() == ERROR_ALREADY_EXISTS)
	{
		::CloseHandle(m_userMutex);
		m_userMutex = nullptr;
		return false;
	}

	m_stopping = false;
	::SetWindowTextW(window, WindowTitle(user).c_str());
	m_thread = std::thread(&ChatIpc::Listen, this);
	return true;
}

void ChatIpc::Stop()
{
	if (!m_userMutex)
		return;
	m_stopping = true;
	if (m_thread.joinable())
	{
		// Đánh thức luồng đang chờ kết nối hoặc đang đọc pipe.
		for (int attempt = 0; attempt < 20 &&
			::WaitForSingleObject(m_thread.native_handle(), 0) != WAIT_OBJECT_0;
			++attempt)
		{
			::CancelSynchronousIo(m_thread.native_handle());
			HANDLE wake = ::CreateFileW(PipeName(m_user).c_str(), GENERIC_WRITE,
				0, nullptr, OPEN_EXISTING, 0, nullptr);
			if (wake != INVALID_HANDLE_VALUE)
				::CloseHandle(wake);
			::WaitForSingleObject(m_thread.native_handle(), 50);
		}
		m_thread.join();
	}
	::CloseHandle(m_userMutex);
	m_userMutex = nullptr;
	m_window = nullptr;
}

bool ChatIpc::Encode(const IpcChatMessage& message, std::vector<BYTE>& data)
{
	const size_t senderBytes = message.sender.size() * sizeof(wchar_t);
	const size_t receiverBytes = message.receiver.size() * sizeof(wchar_t);
	const size_t textBytes = message.text.size() * sizeof(wchar_t);
	const size_t total = sizeof(Header) + senderBytes + receiverBytes + textBytes;
	if (message.sender.empty() || message.receiver.empty() || message.text.empty() ||
		total > MAX_PAYLOAD_BYTES)
		return false;

	Header header{ MESSAGE_MAGIC, static_cast<DWORD>(senderBytes),
		static_cast<DWORD>(receiverBytes), static_cast<DWORD>(textBytes) };
	data.resize(total);
	BYTE* current = data.data();
	std::memcpy(current, &header, sizeof(header));
	current += sizeof(header);
	std::memcpy(current, message.sender.data(), senderBytes);
	current += senderBytes;
	std::memcpy(current, message.receiver.data(), receiverBytes);
	current += receiverBytes;
	std::memcpy(current, message.text.data(), textBytes);
	return true;
}

bool ChatIpc::Decode(const void* bytes, size_t size, IpcChatMessage& message)
{
	if (!bytes || size < sizeof(Header) || size > MAX_PAYLOAD_BYTES)
		return false;
	Header header{};
	std::memcpy(&header, bytes, sizeof(header));
	if (header.magic != MESSAGE_MAGIC || header.senderBytes == 0 ||
		header.receiverBytes == 0 || header.textBytes == 0 ||
		(header.senderBytes | header.receiverBytes | header.textBytes) % sizeof(wchar_t) != 0 ||
		size != sizeof(Header) + static_cast<size_t>(header.senderBytes) +
		static_cast<size_t>(header.receiverBytes) + header.textBytes)
		return false;

	const BYTE* current = static_cast<const BYTE*>(bytes) + sizeof(Header);
	message.sender.assign(reinterpret_cast<const wchar_t*>(current), header.senderBytes / sizeof(wchar_t));
	current += header.senderBytes;
	message.receiver.assign(reinterpret_cast<const wchar_t*>(current), header.receiverBytes / sizeof(wchar_t));
	current += header.receiverBytes;
	message.text.assign(reinterpret_cast<const wchar_t*>(current), header.textBytes / sizeof(wchar_t));
	return message.sender.find(L'\0') == std::wstring::npos &&
		message.receiver.find(L'\0') == std::wstring::npos &&
		message.text.find(L'\0') == std::wstring::npos;
}

void ChatIpc::Listen()
{
	const std::wstring pipeName = PipeName(m_user);
	while (!m_stopping)
	{
		HANDLE hPipe = ::CreateNamedPipeW(pipeName.c_str(),
			PIPE_ACCESS_INBOUND | FILE_FLAG_FIRST_PIPE_INSTANCE,
			PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
			1, 4096, 4096, 0, nullptr);
		if (hPipe == INVALID_HANDLE_VALUE)
			return;

		BOOL checkConnect = ::ConnectNamedPipe(hPipe, nullptr);
		if (!checkConnect && ::GetLastError() != ERROR_PIPE_CONNECTED)
		{
			::CloseHandle(hPipe);
			continue;
		}
		if (!m_stopping)
		{
			// Đọc số byte trước, sau đó mới đọc nội dung tin nhắn.
			DWORD nByteCanDoc = 0;
			if (ReadExact(hPipe, &nByteCanDoc, sizeof(nByteCanDoc)) &&
				nByteCanDoc <= MAX_PAYLOAD_BYTES && nByteCanDoc >= sizeof(Header))
			{
				std::vector<BYTE> data(nByteCanDoc);
				IpcChatMessage messageNhan;
				if (ReadExact(hPipe, data.data(), nByteCanDoc) &&
					Decode(data.data(), nByteCanDoc, messageNhan) &&
					messageNhan.receiver == m_user)
				{
					// ListView chỉ được cập nhật bên luồng giao diện.
					auto* ptrMessage = new IpcChatMessage(std::move(messageNhan));
					if (!::PostMessageW(m_window, WM_CHAT_RECEIVED, 0,
						reinterpret_cast<LPARAM>(ptrMessage)))
						delete ptrMessage;
				}
			}
		}
		::CloseHandle(hPipe);
	}
}

bool ChatIpc::SendPipe(const IpcChatMessage& message, DWORD& error) const
{
	std::vector<BYTE> data;
	if (!Encode(message, data))
	{
		error = ERROR_INVALID_DATA;
		return false;
	}
	const std::wstring pipeName = PipeName(message.receiver);
	HANDLE hPipe = ::CreateFileW(pipeName.c_str(), GENERIC_WRITE, 0,
		nullptr, OPEN_EXISTING, 0, nullptr);
	if (hPipe == INVALID_HANDLE_VALUE && ::GetLastError() == ERROR_PIPE_BUSY &&
		::WaitNamedPipeW(pipeName.c_str(), PIPE_TIMEOUT_MS))
		hPipe = ::CreateFileW(pipeName.c_str(), GENERIC_WRITE, 0,
			nullptr, OPEN_EXISTING, 0, nullptr);
	if (hPipe == INVALID_HANDLE_VALUE)
	{
		error = ::GetLastError();
		return false;
	}
	DWORD nByteGui = static_cast<DWORD>(data.size());
	// Gửi kích thước trước để bên nhận biết cần đọc bao nhiêu byte.
	BOOL checkSend = WriteExact(hPipe, &nByteGui, sizeof(nByteGui)) &&
		WriteExact(hPipe, data.data(), nByteGui);
	error = checkSend ? ERROR_SUCCESS : ::GetLastError();
	::CloseHandle(hPipe);
	return checkSend != FALSE;
}

bool ChatIpc::SendCopyData(const IpcChatMessage& message, DWORD& error) const
{
	std::vector<BYTE> data;
	if (!Encode(message, data))
	{
		error = ERROR_INVALID_DATA;
		return false;
	}
	FindWindowData windowData{ WindowTitle(message.receiver), nullptr };
	::EnumWindows(FindChatWindow, reinterpret_cast<LPARAM>(&windowData));
	if (!windowData.found)
	{
		error = ERROR_FILE_NOT_FOUND;
		return false;
	}
	COPYDATASTRUCT copyData{ CHAT_COPYDATA_ID, static_cast<DWORD>(data.size()), data.data() };
	DWORD_PTR ketQuaNhan = FALSE;
	if (!::SendMessageTimeoutW(windowData.found, WM_COPYDATA,
		reinterpret_cast<WPARAM>(m_window), reinterpret_cast<LPARAM>(&copyData),
		SMTO_ABORTIFHUNG, 2000, &ketQuaNhan))
	{
		error = ::GetLastError();
		return false;
	}
	error = ketQuaNhan ? ERROR_SUCCESS : ERROR_INVALID_DATA;
	return ketQuaNhan != FALSE;
}
