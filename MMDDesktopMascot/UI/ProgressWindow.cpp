#include "ProgressWindow.hpp"
#include "Win32UiUtil.hpp"

#include <stdexcept>
#include <uxtheme.h>

namespace
{
	constexpr wchar_t kClassName[] = L"MMDDesk.ProgressWindow";
	constexpr int ID_PROGRESS = 1001;

	constexpr COLORREF kDarkBkColor = RGB(32, 32, 32);
	constexpr COLORREF kTextColor = RGB(240, 240, 240);
}

ProgressWindow::ProgressWindow(HINSTANCE hInst, HWND parent)
	: m_hInst(hInst), m_parent(parent)
{
	Win32UiUtil::InitializeDarkModeSupport();
	m_darkBrush = CreateSolidBrush(kDarkBkColor);
	m_hFont = Win32UiUtil::CreateSegoeUiFont(-12, FW_NORMAL);

	WNDCLASSEXW wc{};
	wc.cbSize = sizeof(wc);
	wc.lpfnWndProc = WndProcThunk;
	wc.hInstance = m_hInst;
	wc.lpszClassName = kClassName;
	wc.hbrBackground = m_darkBrush;
	wc.hCursor = LoadCursor(nullptr, IDC_WAIT);
	Win32UiUtil::RegisterWindowClassOrThrow(wc, "ProgressWindow", true);
}

ProgressWindow::~ProgressWindow()
{
	Hide();
	if (m_darkBrush) DeleteObject(m_darkBrush);
	if (m_hFont) DeleteObject(m_hFont);
}

void ProgressWindow::SetModernFont(HWND hChild)
{
	Win32UiUtil::ApplyControlFont(hChild, m_hFont);
}

void ProgressWindow::ConfigureProgressBar()
{
	if (!m_progressBar)
	{
		return;
	}

	// Keep the progress bar on the standard themed path so it renders with
	// the normal green fill instead of the dark-mode white fallback.
	SetWindowTheme(m_progressBar, L"Explorer", nullptr);
	SendMessageW(m_progressBar, PBM_SETSTATE, PBST_NORMAL, 0);
}

void ProgressWindow::Show()
{
	if (m_hwnd) return;

	RECT rcParent{};
	GetWindowRect(m_parent, &rcParent);
	const int w = 400;
	const int h = 120;
	const int x = rcParent.left + (rcParent.right - rcParent.left - w) / 2;
	const int y = rcParent.top + (rcParent.bottom - rcParent.top - h) / 2;

	m_hwnd = CreateWindowExW(
		WS_EX_DLGMODALFRAME | WS_EX_TOPMOST,
		kClassName, L"読み込み中...",
		WS_POPUP | WS_CAPTION | WS_BORDER,
		x, y, w, h,
		m_parent, nullptr, m_hInst, this
	);
	if (!m_hwnd)
	{
		throw std::runtime_error("CreateWindowExW (ProgressWindow) failed.");
	}

	Win32UiUtil::EnableImmersiveDarkMode(m_hwnd);

	RECT rcClient{};
	GetClientRect(m_hwnd, &rcClient);
	const int clientW = rcClient.right - rcClient.left;

	const int barW = 360;
	const int barH = 20;
	const int labelW = 380;
	const int labelH = 20;

	const int labelX = (clientW - labelW) / 2;
	const int barX = (clientW - barW) / 2;

	m_statusLabel = CreateWindowExW(
		0,
		L"STATIC",
		L"準備中...",
		WS_CHILD | WS_VISIBLE | SS_CENTER,
		labelX,
		20,
		labelW,
		labelH,
		m_hwnd,
		nullptr,
		m_hInst,
		nullptr);
	SetModernFont(m_statusLabel);

	m_progressBar = CreateWindowExW(
		0,
		PROGRESS_CLASSW,
		nullptr,
		WS_CHILD | WS_VISIBLE | PBS_SMOOTH,
		barX,
		50,
		barW,
		barH,
		m_hwnd,
		Win32UiUtil::ControlIdToMenuHandle(ID_PROGRESS),
		m_hInst,
		nullptr);
	ConfigureProgressBar();
	SendMessageW(m_progressBar, PBM_SETRANGE, 0, MAKELPARAM(0, 100));

	ShowWindow(m_hwnd, SW_SHOW);
	UpdateWindow(m_hwnd);
	EnableWindow(m_parent, FALSE);
}

void ProgressWindow::Hide()
{
	if (!m_hwnd)
	{
		return;
	}

	EnableWindow(m_parent, TRUE);
	SetForegroundWindow(m_parent);
	DestroyWindow(m_hwnd);
	m_hwnd = nullptr;
}

void ProgressWindow::SetProgress(float percentage)
{
	if (!m_progressBar)
	{
		return;
	}

	const int pos = static_cast<int>(percentage * 100.0f);
	SendMessageW(m_progressBar, PBM_SETPOS, pos, 0);
}

void ProgressWindow::SetMessage(const std::wstring& msg)
{
	if (m_statusLabel)
	{
		SetWindowTextW(m_statusLabel, msg.c_str());
	}
}

LRESULT CALLBACK ProgressWindow::WndProcThunk(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	ProgressWindow* self = Win32UiUtil::InitializeWindowUserData<ProgressWindow>(hWnd, msg, lParam);

	if (self)
	{
		return self->WndProc(hWnd, msg, wParam, lParam);
	}

	return DefWindowProcW(hWnd, msg, wParam, lParam);
}

LRESULT ProgressWindow::WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	switch (msg)
	{
		case WM_CTLCOLORSTATIC:
		{
			HDC hdc = reinterpret_cast<HDC>(wParam);
			SetTextColor(hdc, kTextColor);
			SetBkMode(hdc, TRANSPARENT);
			return reinterpret_cast<LRESULT>(m_darkBrush);
		}
		default:
			return DefWindowProcW(hWnd, msg, wParam, lParam);
	}
}
