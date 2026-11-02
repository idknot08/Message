#pragma once

#include <atomic>
#include <string>
#include <thread>
#include <vector>

constexpr UINT WM_CHAT_RECEIVED = WM_APP + 42;
constexpr ULONG_PTR CHAT_COPYDATA_ID = 0x54544332;

struct IpcChatMessage
{
	std::wstring sender;
	std::wstring receiver;
	std::wstring text;
};

class ChatIpc
{
public:
	~ChatIpc();
	bool Start(HWND window, const std::wstring& user);
	void Stop();
	bool SendPipe(const IpcChatMessage& message, DWORD& error) const;
	bool SendCopyData(const IpcChatMessage& message, DWORD& error) const;
	static bool Decode(const void* data, size_t bytes, IpcChatMessage& message);

private:
	void Listen();
	static std::wstring PipeName(const std::wstring& user);
	static std::wstring WindowTitle(const std::wstring& user);
	static bool Encode(const IpcChatMessage& message, std::vector<BYTE>& data);

	HWND m_window = nullptr;
	HANDLE m_userMutex = nullptr;
	std::wstring m_user;
	std::thread m_thread;
	std::atomic<bool> m_stopping{ false };
};
