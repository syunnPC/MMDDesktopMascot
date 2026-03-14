#pragma once
#include <windows.h>
#include <shellapi.h>

class TrayIcon
{
public:
	TrayIcon(HWND owner, UINT id);
	~TrayIcon();

	TrayIcon(const TrayIcon&) = delete;
	TrayIcon& operator=(const TrayIcon&) = delete;

	void Show(const wchar_t* tooltip);
	void Hide();
	void ShowBalloon(const wchar_t* title, const wchar_t* message, DWORD infoFlags = NIIF_INFO) const;

	UINT CallbackMessage() const
	{
		return m_callbackMsg;
	}

private:
	HWND m_owner{};
	UINT m_id{};

	NOTIFYICONDATAW m_nid{};
	UINT m_callbackMsg{ WM_APP + 10 };
	bool m_visible{ false };
};
