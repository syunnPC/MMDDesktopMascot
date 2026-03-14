#include "TrayIcon.hpp"
#include "Resource.h"
#include <stdexcept>

TrayIcon::TrayIcon(HWND owner, UINT id)
	: m_owner(owner), m_id(id)
{
}

TrayIcon::~TrayIcon()
{
	Hide();
}

void TrayIcon::Show(const wchar_t* tooltip)
{
	if (m_visible) return;

	m_nid = {};
	m_nid.cbSize = sizeof(m_nid);
	m_nid.hWnd = m_owner;
	m_nid.uID = m_id;

	m_nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
	m_nid.uCallbackMessage = m_callbackMsg;
	HINSTANCE hInst = reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(m_owner, GWLP_HINSTANCE));
	m_nid.hIcon = LoadIconW(hInst, MAKEINTRESOURCEW(IDI_MMDDESKTOPMASCOT));
	if (!m_nid.hIcon)
	{
		m_nid.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
	}

	if (tooltip)
	{
		wcsncpy_s(m_nid.szTip, tooltip, _countof(m_nid.szTip) - 1);
	}

	if (!Shell_NotifyIconW(NIM_ADD, &m_nid))
	{
		throw std::runtime_error("Shell_NotifyIconW(NIM_ADD) failed.");
	}

	m_nid.uVersion = NOTIFYICON_VERSION_4;
	if (!Shell_NotifyIconW(NIM_SETVERSION, &m_nid))
	{
#ifdef _DEBUG
		OutputDebugStringW(L"Failed to set NOTIFYICON_VERSION_4.\r\n");
#endif
	}

	m_visible = true;
}

void TrayIcon::Hide()
{
	if (!m_visible) return;

	Shell_NotifyIconW(NIM_DELETE, &m_nid);
	m_visible = false;
}

void TrayIcon::ShowBalloon(const wchar_t* title, const wchar_t* message, DWORD infoFlags) const
{
	if (!m_visible) return;

	NOTIFYICONDATAW nid = m_nid;
	nid.uFlags |= NIF_INFO;
	wcsncpy_s(nid.szInfoTitle, title ? title : L"", _countof(nid.szInfoTitle) - 1);
	wcsncpy_s(nid.szInfo, message ? message : L"", _countof(nid.szInfo) - 1);
	nid.dwInfoFlags = infoFlags;
	Shell_NotifyIconW(NIM_MODIFY, &nid);
}
