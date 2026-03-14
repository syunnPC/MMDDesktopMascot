#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "WindowManager.hpp"
#include "InputManager.hpp"
#include "DcompRenderer.hpp"
#include "Settings.hpp"
#include "TrayIcon.hpp"
#include "Win32UiUtil.hpp"
#include <algorithm>
#include <format>
#include <stdexcept>
#include <windowsx.h>

#include <d2d1.h>

#pragma comment(lib, "d2d1.lib")

namespace
{
	constexpr wchar_t kMsgClassName[] = L"MMDDesk.MsgWindow";
	constexpr wchar_t kRenderClassName[] = L"MMDDesk.RenderWindow";
	constexpr wchar_t kGizmoClassName[] = L"MMDDesk.GizmoWindow";

	constexpr int kGizmoSizePx = 140;

	DWORD GetWindowStyleExForRender()
	{
		return WS_EX_TOOLWINDOW | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_NOREDIRECTIONBITMAP;
	}

	DWORD GetWindowStyleForRender()
	{
		return WS_POPUP;
	}

	constexpr wchar_t kPropWindowManipulationMode[] = L"MMDDesk.WindowManipulationMode";

	bool IsWindowManipulationMode(HWND hWnd)
	{
		return hWnd && GetPropW(hWnd, kPropWindowManipulationMode) != nullptr;
	}

	void SetWindowManipulationModeProp(HWND hWnd, bool enabled)
	{
		if (!hWnd) return;
		if (enabled)
		{
			SetPropW(hWnd, kPropWindowManipulationMode, reinterpret_cast<HANDLE>(1));
		}
		else
		{
			RemovePropW(hWnd, kPropWindowManipulationMode);
		}
	}

	void SetWindowInteractivity(HWND hWnd, bool interactive)
	{
		if (!hWnd) return;

		EnableWindow(hWnd, interactive ? TRUE : FALSE);

		LONG_PTR ex = GetWindowLongPtrW(hWnd, GWL_EXSTYLE);
		if (interactive)
		{
			ex &= ~(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE);
		}
		else
		{
			ex |= (WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE);
		}
		SetWindowLongPtrW(hWnd, GWL_EXSTYLE, ex);

		if (!interactive)
		{
			// A layered + transparent top-level window is treated as input-transparent
			// by User32, which is required for true click-through outside our thread.
			SetLayeredWindowAttributes(hWnd, 0, 255, LWA_ALPHA);
		}

		SetWindowPos(
			hWnd,
			nullptr,
			0,
			0,
			0,
			0,
			SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
	}

	int ClampWindowSpan(int value, int preferredMin, int maxValue)
	{
		if (maxValue <= 0)
		{
			return std::max(1, value);
		}

		if (maxValue < preferredMin)
		{
			return maxValue;
		}

		return std::clamp(value, preferredMin, maxValue);
	}

	RECT ComputeInitialRenderBounds(const RECT& workArea, const AppSettings& settings)
	{
		const int workW = std::max(1L, workArea.right - workArea.left);
		const int workH = std::max(1L, workArea.bottom - workArea.top);
		const int insetX = std::min(24, std::max(8, workW / 20));
		const int insetY = std::min(40, std::max(12, workH / 16));
		const int maxW = std::max(120, workW - insetX * 2);
		const int maxH = std::max(160, workH - insetY * 2);

		int width = 400;
		int height = 600;
		if (settings.windowWidth > 0 && settings.windowHeight > 0)
		{
			width = settings.windowWidth;
			height = settings.windowHeight;
		}
		else
		{
			width = ClampWindowSpan(workW / 3, 400, maxW);
			height = ClampWindowSpan((workH * 3) / 5, 480, maxH);
		}

		width = ClampWindowSpan(width, 320, maxW);
		height = ClampWindowSpan(height, 360, maxH);

		const int x = std::max(workArea.left, workArea.right - width - insetX);
		const int y = std::max(workArea.top, workArea.bottom - height - insetY);
		return RECT{ x, y, x + width, y + height };
	}

	void UpdateStoredRenderSize(HWND hWnd, AppSettings& settings)
	{
		if (!hWnd || !IsWindow(hWnd)) return;

		RECT clientRect{};
		if (!GetClientRect(hWnd, &clientRect)) return;

		const int width = clientRect.right - clientRect.left;
		const int height = clientRect.bottom - clientRect.top;
		if (width <= 0 || height <= 0) return;

		settings.windowWidth = width;
		settings.windowHeight = height;
	}

	RECT ComputeWindowRectForClientSize(const POINT& clientOrigin, const SIZE& clientSize, DWORD style, DWORD exStyle, UINT dpi)
	{
		RECT targetRect{
			0,
			0,
			std::max(1L, static_cast<LONG>(clientSize.cx)),
			std::max(1L, static_cast<LONG>(clientSize.cy))
		};

		if (!AdjustWindowRectExForDpi(&targetRect, style, FALSE, exStyle, dpi))
		{
			AdjustWindowRectEx(&targetRect, style, FALSE, exStyle);
		}

		OffsetRect(&targetRect, clientOrigin.x, clientOrigin.y);
		return targetRect;
	}
}

WindowManager::WindowManager(HINSTANCE hInst, InputManager& input, AppSettings& settings, Callbacks callbacks)
	: m_hInst(hInst)
	, m_input(input)
	, m_settings(settings)
	, m_callbacks(callbacks)
{
}

WindowManager::~WindowManager()
{
	if (m_gizmoWnd)
	{
		DestroyWindow(m_gizmoWnd);
	}

	if (m_gizmoOldBmp && m_gizmoDc) SelectObject(m_gizmoDc, m_gizmoOldBmp);
	if (m_gizmoBmp) DeleteObject(m_gizmoBmp);
	if (m_gizmoDc) DeleteDC(m_gizmoDc);
}

void WindowManager::Initialize()
{
	CreateHiddenMessageWindow();
	CreateRenderWindow();
	CreateGizmoWindow();
}

void WindowManager::SetRenderer(DcompRenderer* renderer)
{
	m_renderer = renderer;
}

void WindowManager::SetTray(TrayIcon* tray)
{
	m_tray = tray;
}

void WindowManager::ApplyTopmost(bool alwaysOnTop) const
{
	if (!m_renderWnd) return;

	HWND insertAfter = alwaysOnTop ? HWND_TOPMOST : HWND_NOTOPMOST;
	SetWindowPos(m_renderWnd, insertAfter, 0, 0, 0, 0,
				 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
	if (m_gizmoWnd)
	{
		SetWindowPos(m_gizmoWnd, insertAfter, 0, 0, 0, 0,
					 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
	}
}

void WindowManager::ToggleGizmoWindow()
{
	SetGizmoVisible(!m_gizmoVisible);
}

void WindowManager::PositionGizmoWindow()
{
	if (!m_gizmoWnd || !m_renderWnd) return;

	RECT renderRect{};
	GetWindowRect(m_renderWnd, &renderRect);
	PositionGizmoWindowForRenderRect(renderRect);
}

void WindowManager::PositionGizmoWindowForRenderRect(const RECT& renderWindowRect)
{
	if (!m_gizmoWnd || !m_renderWnd) return;

	POINT clientOrigin{ 0, 0 };
	ClientToScreen(m_renderWnd, &clientOrigin);

	RECT currentWindowRect{};
	GetWindowRect(m_renderWnd, &currentWindowRect);
	const int clientOffsetX = clientOrigin.x - currentWindowRect.left;
	const int clientOffsetY = clientOrigin.y - currentWindowRect.top;

	const SIZE clientSize = Win32UiUtil::GetClientSize(m_renderWnd);
	const int x = renderWindowRect.left + clientOffsetX + (clientSize.cx - kGizmoSizePx) / 2;
	const int y = renderWindowRect.top + clientOffsetY + (clientSize.cy - kGizmoSizePx) / 2;

	HWND insertAfter = m_settings.alwaysOnTop ? HWND_TOPMOST : HWND_NOTOPMOST;
	SetWindowPos(
		m_gizmoWnd,
		insertAfter,
		x,
		y,
		kGizmoSizePx,
		kGizmoSizePx,
		SWP_NOACTIVATE);
}

void WindowManager::EnsureGizmoD2D()
{
	if (!m_gizmoWnd) return;

	if (!m_d2dFactory)
	{
		HRESULT hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, m_d2dFactory.put());
		if (FAILED(hr))
		{
			throw std::runtime_error("D2D1CreateFactory failed.");
		}
	}

	if (!m_gizmoDc)
	{
		HDC screenDc = GetDC(nullptr);
		m_gizmoDc = CreateCompatibleDC(screenDc);
		ReleaseDC(nullptr, screenDc);
		if (!m_gizmoDc)
		{
			throw std::runtime_error("CreateCompatibleDC failed.");
		}
	}

	if (!m_gizmoBmp)
	{
		BITMAPINFO bmi = {};
		bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
		bmi.bmiHeader.biWidth = kGizmoSizePx;
		bmi.bmiHeader.biHeight = -kGizmoSizePx;
		bmi.bmiHeader.biPlanes = 1;
		bmi.bmiHeader.biBitCount = 32;
		bmi.bmiHeader.biCompression = BI_RGB;

		m_gizmoBmp = CreateDIBSection(m_gizmoDc, &bmi, DIB_RGB_COLORS, &m_gizmoBits, nullptr, 0);
		if (!m_gizmoBmp)
		{
			throw std::runtime_error("CreateDIBSection failed.");
		}

		m_gizmoOldBmp = SelectObject(m_gizmoDc, m_gizmoBmp);
	}

	if (!m_gizmoRt)
	{
		D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
			D2D1_RENDER_TARGET_TYPE_SOFTWARE,
			D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
			0.0f, 0.0f,
			D2D1_RENDER_TARGET_USAGE_NONE,
			D2D1_FEATURE_LEVEL_DEFAULT
		);

		HRESULT hr = m_d2dFactory->CreateDCRenderTarget(&props, m_gizmoRt.put());
		if (FAILED(hr))
		{
			throw std::runtime_error("CreateDCRenderTarget failed.");
		}

		m_gizmoRt->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

		hr = m_gizmoRt->CreateSolidColorBrush(
			D2D1::ColorF(0.08f, 0.08f, 0.08f, 0.6f),
			m_gizmoBrushFill.put());
		if (FAILED(hr))
		{
			throw std::runtime_error("CreateSolidColorBrush (Fill) failed.");
		}

		hr = m_gizmoRt->CreateSolidColorBrush(
			D2D1::ColorF(0.85f, 0.85f, 0.85f, 0.9f),
			m_gizmoBrushStroke.put());
		if (FAILED(hr))
		{
			throw std::runtime_error("CreateSolidColorBrush (Stroke) failed.");
		}
	}
}

void WindowManager::DiscardGizmoD2D()
{
	m_gizmoRt = nullptr;
	m_gizmoBrushFill = nullptr;
	m_gizmoBrushStroke = nullptr;
}

void WindowManager::RenderGizmo()
{
	if (!m_gizmoVisible || !m_gizmoWnd) return;
	EnsureGizmoD2D();
	if (!m_gizmoRt || !m_gizmoDc) return;

	const float width = static_cast<float>(kGizmoSizePx);
	const float height = static_cast<float>(kGizmoSizePx);
	const float cx = width * 0.5f;
	const float cy = height * 0.5f;
	const float radius = (std::min)(width, height) * 0.5f - 2.0f;

	RECT rc = { 0, 0, kGizmoSizePx, kGizmoSizePx };
	HRESULT hr = m_gizmoRt->BindDC(m_gizmoDc, &rc);

	if (FAILED(hr))
	{
		if (hr == D2DERR_RECREATE_TARGET) DiscardGizmoD2D();
		return;
	}

	m_gizmoRt->BeginDraw();
	m_gizmoRt->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));

	D2D1_ELLIPSE el = D2D1::Ellipse(D2D1::Point2F(cx, cy), radius, radius);
	m_gizmoRt->FillEllipse(el, m_gizmoBrushFill.get());
	m_gizmoRt->DrawEllipse(el, m_gizmoBrushStroke.get(), 2.0f);

	const float tick = radius * 0.55f;
	m_gizmoRt->DrawLine(D2D1::Point2F(cx - tick, cy), D2D1::Point2F(cx + tick, cy), m_gizmoBrushStroke.get(), 1.5f);
	m_gizmoRt->DrawLine(D2D1::Point2F(cx, cy - tick), D2D1::Point2F(cx, cy + tick), m_gizmoBrushStroke.get(), 1.5f);

	hr = m_gizmoRt->EndDraw();
	if (FAILED(hr))
	{
		if (hr == D2DERR_RECREATE_TARGET)
		{
			DiscardGizmoD2D();
		}
		else
		{
			OutputDebugStringW(std::format(L"EndDraw hr=0x{:08X}\n", static_cast<unsigned>(hr)).c_str());
		}
		return;
	}

	BLENDFUNCTION bf = {};
	bf.BlendOp = AC_SRC_OVER;
	bf.SourceConstantAlpha = 255;
	bf.AlphaFormat = AC_SRC_ALPHA;

	POINT ptSrc = { 0, 0 };
	SIZE wndSize = { kGizmoSizePx, kGizmoSizePx };

	RECT wndRect;
	GetWindowRect(m_gizmoWnd, &wndRect);
	POINT ptDst = { wndRect.left, wndRect.top };

	UpdateLayeredWindow(m_gizmoWnd, nullptr, &ptDst, &wndSize, m_gizmoDc, &ptSrc, 0, &bf, ULW_ALPHA);
}

void WindowManager::InstallRenderClickThrough()
{
	if (!m_renderWnd) return;

	SetWindowInteractivity(m_renderWnd, false);

	if (!m_prevRenderWndProc)
	{
		m_prevRenderWndProc = reinterpret_cast<WNDPROC>(
			SetWindowLongPtrW(
				m_renderWnd,
				GWLP_WNDPROC,
				reinterpret_cast<LONG_PTR>(
					reinterpret_cast<WNDPROC>(WindowManager::RenderClickThroughProc)
					)
			)
			);
	}
}

void WindowManager::MakeClickThrough(HWND hWnd)
{
	if (!hWnd) return;
	SetWindowInteractivity(hWnd, false);
}

BOOL CALLBACK WindowManager::EnumChildForClickThrough(HWND hWnd, LPARAM)
{
	MakeClickThrough(hWnd);
	return TRUE;
}

void WindowManager::ForceRenderTreeClickThroughFor(HWND hWnd)
{
	MakeClickThrough(hWnd);
	EnumChildWindows(hWnd, &WindowManager::EnumChildForClickThrough, 0);
}

void WindowManager::ForceRenderTreeClickThrough()
{
	if (!m_renderWnd) return;
	ForceRenderTreeClickThroughFor(m_renderWnd);
}

void WindowManager::ToggleWindowManipulationMode()
{
	SetGizmoVisible(!m_gizmoVisible);
}

bool WindowManager::IsWindowManipulationMode() const
{
	return m_gizmoVisible;
}

void WindowManager::ApplyWindowManipulationMode(bool enabled)
{
	if (!m_renderWnd) return;

	const SIZE clientSize = Win32UiUtil::GetClientSize(m_renderWnd);
	POINT clientOrigin{ 0, 0 };
	ClientToScreen(m_renderWnd, &clientOrigin);

	SetWindowManipulationModeProp(m_renderWnd, enabled);

	if (m_renderer)
	{
		m_renderer->SetResizeOverlayEnabled(enabled);
	}

	SetRenderTreeInteractivity(enabled);

	LONG_PTR style = GetWindowLongPtrW(m_renderWnd, GWL_STYLE);
	if (enabled)
	{
		style |= WS_THICKFRAME;
	}
	else
	{
		style &= ~WS_THICKFRAME;
	}
	SetWindowLongPtrW(m_renderWnd, GWL_STYLE, style);

	const LONG_PTR exStyle = GetWindowLongPtrW(m_renderWnd, GWL_EXSTYLE);
	const UINT dpi = GetDpiForWindow(m_renderWnd);
	const RECT targetRect = ComputeWindowRectForClientSize(
		clientOrigin,
		clientSize,
		static_cast<DWORD>(style),
		static_cast<DWORD>(exStyle),
		dpi);

	SetWindowPos(
		m_renderWnd,
		nullptr,
		targetRect.left,
		targetRect.top,
		targetRect.right - targetRect.left,
		targetRect.bottom - targetRect.top,
		SWP_NOZORDER | (enabled ? 0 : SWP_NOACTIVATE) | SWP_FRAMECHANGED);

	if (!enabled)
	{
		ForceRenderTreeClickThrough();
	}

	RepositionVisibleGizmoWindow();
	InvalidateRect(m_renderWnd, nullptr, FALSE);
}

void WindowManager::SetGizmoVisible(bool visible)
{
	if (!m_gizmoWnd) return;
	if (m_gizmoVisible == visible) return;

	m_gizmoVisible = visible;
	ApplyWindowManipulationMode(visible);

	if (!visible)
	{
		m_input.ResetGizmoDrag();
		ReleaseCapture();
		ShowWindow(m_gizmoWnd, SW_HIDE);
		return;
	}

	PositionGizmoWindow();
	ShowWindow(m_gizmoWnd, SW_SHOWNOACTIVATE);
	InvalidateRect(m_gizmoWnd, nullptr, FALSE);
}

void WindowManager::SetRenderTreeInteractivity(bool interactive) const
{
	if (!m_renderWnd) return;

	SetWindowInteractivity(m_renderWnd, interactive);
	EnumChildWindows(
		m_renderWnd,
		[](HWND child, LPARAM isInteractive) -> BOOL
		{
			SetWindowInteractivity(child, isInteractive != 0);
			return TRUE;
		},
		interactive ? 1 : 0);
}

void WindowManager::UpdateStoredRenderSize()
{
	::UpdateStoredRenderSize(m_renderWnd, m_settings);
}

void WindowManager::RepositionVisibleGizmoWindow()
{
	if (m_gizmoVisible && m_gizmoWnd)
	{
		PositionGizmoWindow();
	}
}

void WindowManager::UpdateSettingsForRenderSize()
{
	UpdateStoredRenderSize();
}

bool WindowManager::HandleTrayCallbackMessage(UINT msg, LPARAM lParam) const
{
	if (!m_tray || msg != m_tray->CallbackMessage())
	{
		return false;
	}

	const UINT evRaw = static_cast<UINT>(lParam);
	const UINT ev = LOWORD(lParam);
	const auto isEvent = [&](UINT candidate)
	{
		return (ev == candidate) || (evRaw == candidate);
	};

	const bool requestMenu =
		isEvent(WM_CONTEXTMENU) ||
		isEvent(WM_RBUTTONUP) ||
		isEvent(WM_RBUTTONDOWN) ||
		isEvent(NIN_SELECT) ||
		isEvent(NIN_KEYSELECT);

	if (requestMenu && m_callbacks.onTrayMenuRequested)
	{
		POINT pt{};
		GetCursorPos(&pt);
		m_callbacks.onTrayMenuRequested(pt);
	}

	return true;
}

bool WindowManager::HandleOwnedMessage(UINT msg, WPARAM wParam, LPARAM lParam) const
{
	if (msg == kLoadCompleteMessage)
	{
		if (m_callbacks.onLoadComplete)
		{
			m_callbacks.onLoadComplete(wParam, lParam);
		}
		return true;
	}

	if (msg == kLoadProgressMessage)
	{
		if (m_callbacks.onLoadProgress)
		{
			m_callbacks.onLoadProgress(wParam, lParam);
		}
		return true;
	}

	return false;
}

void WindowManager::SaveSettingsIfRequested() const
{
	if (m_callbacks.onSaveSettings)
	{
		m_callbacks.onSaveSettings();
	}
}

void WindowManager::HandleRenderWindowSized(WPARAM sizeType)
{
	if (sizeType == SIZE_MINIMIZED)
	{
		return;
	}

	UpdateStoredRenderSize();
	RepositionVisibleGizmoWindow();
}

void WindowManager::HandleRenderWindowMoving(LPARAM lParam)
{
	if (!lParam)
	{
		return;
	}

	const RECT* movingRect = reinterpret_cast<const RECT*>(lParam);
	PositionGizmoWindowForRenderRect(*movingRect);
}

void WindowManager::HandleRenderWindowMoved()
{
	RepositionVisibleGizmoWindow();
}

LRESULT WindowManager::HandleRenderWindowDpiChanged(HWND hWnd, LPARAM lParam)
{
	if (lParam)
	{
		const RECT* suggested = reinterpret_cast<const RECT*>(lParam);
		SetWindowPos(
			hWnd,
			nullptr,
			suggested->left,
			suggested->top,
			suggested->right - suggested->left,
			suggested->bottom - suggested->top,
			SWP_NOZORDER | SWP_NOACTIVATE);
	}

	UpdateStoredRenderSize();
	RepositionVisibleGizmoWindow();
	return 0;
}

LRESULT WindowManager::HandleRenderWindowClose(HWND hWnd)
{
	if (m_gizmoWnd && IsWindow(m_gizmoWnd))
	{
		DestroyWindow(m_gizmoWnd);
		m_gizmoWnd = nullptr;
		m_gizmoVisible = false;
	}

	DestroyWindow(hWnd);
	return 0;
}

LRESULT WindowManager::HandleRenderWindowDestroy()
{
	SaveSettingsIfRequested();
	m_renderWnd = nullptr;
	PostQuitMessage(0);
	return 0;
}

void WindowManager::CreateHiddenMessageWindow()
{
	WNDCLASSEXW wc{};
	wc.cbSize = sizeof(wc);
	wc.lpfnWndProc = WndProcThunk;
	wc.hInstance = m_hInst;
	wc.lpszClassName = kMsgClassName;

	Win32UiUtil::RegisterWindowClassOrThrow(wc, "MsgWindow");

	m_msgWnd = CreateWindowExW(
		0, kMsgClassName, L"MMDDesk Message Window",
		0, 0, 0, 0, 0,
		HWND_MESSAGE, nullptr, m_hInst, this);

	if (!m_msgWnd)
	{
		throw std::runtime_error("CreateWindowExW (MsgWindow) failed.");
	}
}

void WindowManager::CreateRenderWindow()
{
	WNDCLASSEXW wc{};
	wc.cbSize = sizeof(wc);
	wc.lpfnWndProc = WndProcThunk;
	wc.hInstance = m_hInst;
	wc.lpszClassName = kRenderClassName;
	wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
	wc.hbrBackground = nullptr;

	Win32UiUtil::RegisterWindowClassOrThrow(wc, "RenderWindow");

	const RECT workArea = Win32UiUtil::GetWorkArea();
	const RECT bounds = ComputeInitialRenderBounds(workArea, m_settings);

	m_renderWnd = CreateWindowExW(
		GetWindowStyleExForRender(),
		kRenderClassName, L"MMDDesk",
		GetWindowStyleForRender(),
		bounds.left,
		bounds.top,
		bounds.right - bounds.left,
		bounds.bottom - bounds.top,
		nullptr, nullptr, m_hInst, this);

	if (!m_renderWnd)
	{
		throw std::runtime_error("CreateWindowExW (RenderWindow) failed.");
	}

	Win32UiUtil::EnableImmersiveDarkMode(m_renderWnd);

	ShowWindow(m_renderWnd, SW_SHOWNOACTIVATE);
}

void WindowManager::CreateGizmoWindow()
{
	WNDCLASSEXW wc{};
	wc.cbSize = sizeof(wc);
	wc.lpfnWndProc = WndProcThunk;
	wc.hInstance = m_hInst;
	wc.lpszClassName = kGizmoClassName;
	wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
	wc.hbrBackground = nullptr;

	Win32UiUtil::RegisterWindowClassOrThrow(wc, "GizmoWindow");

	const DWORD exStyle = WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_LAYERED;
	const DWORD style = WS_POPUP;

	m_gizmoWnd = CreateWindowExW(
		exStyle,
		kGizmoClassName,
		L"MMDDesk Gizmo",
		style,
		0, 0, kGizmoSizePx, kGizmoSizePx,
		nullptr, nullptr, m_hInst, this);

	if (!m_gizmoWnd)
	{
		throw std::runtime_error("CreateWindowExW (GizmoWindow) failed.");
	}

	ShowWindow(m_gizmoWnd, SW_HIDE);
}

LRESULT CALLBACK WindowManager::RenderClickThroughProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	WindowManager* self = Win32UiUtil::GetWindowUserData<WindowManager>(hWnd);

	switch (msg)
	{
		case WM_NCHITTEST:
		{
			if (!::IsWindowManipulationMode(hWnd))
			{
				return HTTRANSPARENT;
			}

			POINT pt{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
			RECT rc{};
			GetWindowRect(hWnd, &rc);

			const UINT dpi = GetDpiForWindow(hWnd);
			const int paddedBorder = (dpi > 0) ? GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi) : GetSystemMetrics(SM_CXPADDEDBORDER);
			const int borderX = std::max(
				8,
				((dpi > 0) ? GetSystemMetricsForDpi(SM_CXSIZEFRAME, dpi) : GetSystemMetrics(SM_CXSIZEFRAME)) + paddedBorder);
			const int borderY = std::max(
				8,
				((dpi > 0) ? GetSystemMetricsForDpi(SM_CYSIZEFRAME, dpi) : GetSystemMetrics(SM_CYSIZEFRAME)) + paddedBorder);

			const bool left = (pt.x >= rc.left && pt.x < rc.left + borderX);
			const bool right = (pt.x <= rc.right && pt.x > rc.right - borderX);
			const bool top = (pt.y >= rc.top && pt.y < rc.top + borderY);
			const bool bottom = (pt.y <= rc.bottom && pt.y > rc.bottom - borderY);

			if (top && left) return HTTOPLEFT;
			if (top && right) return HTTOPRIGHT;
			if (bottom && left) return HTBOTTOMLEFT;
			if (bottom && right) return HTBOTTOMRIGHT;
			if (left) return HTLEFT;
			if (right) return HTRIGHT;
			if (top) return HTTOP;
			if (bottom) return HTBOTTOM;

			return HTCAPTION;
		}
		case WM_MOUSEACTIVATE:
			return ::IsWindowManipulationMode(hWnd) ? MA_ACTIVATE : MA_NOACTIVATE;
		default:
			break;
	}

	if (self && self->m_prevRenderWndProc)
	{
		return CallWindowProcW(self->m_prevRenderWndProc, hWnd, msg, wParam, lParam);
	}
	return DefWindowProcW(hWnd, msg, wParam, lParam);
}

LRESULT CALLBACK WindowManager::WndProcThunk(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	WindowManager* self = Win32UiUtil::InitializeWindowUserData<WindowManager>(hWnd, msg, lParam);

	if (self)
	{
		return self->WndProc(hWnd, msg, wParam, lParam);
	}

	return DefWindowProcW(hWnd, msg, wParam, lParam);
}

LRESULT WindowManager::WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	if (HandleTrayCallbackMessage(msg, lParam))
	{
		return 0;
	}

	if (HandleOwnedMessage(msg, wParam, lParam))
	{
		return 0;
	}

	switch (msg)
	{
		case WM_COMMAND:
			if (m_callbacks.onTrayCommand)
			{
				m_callbacks.onTrayCommand(LOWORD(wParam));
				return 0;
			}
			break;

		case WM_HOTKEY:
			if (m_input.HandleHotkey(wParam))
			{
				return 0;
			}
			break;

		case WM_LBUTTONDOWN:
		case WM_RBUTTONDOWN:
			if (m_input.HandleMouseDown(hWnd, msg))
			{
				return 0;
			}
			break;

		case WM_MOUSEMOVE:
			if (m_input.HandleMouseMove(hWnd))
			{
				return 0;
			}
			break;

		case WM_RBUTTONUP:
		case WM_LBUTTONUP:
			if (m_input.HandleMouseUp(hWnd, msg))
			{
				return 0;
			}
			break;

		case WM_MOUSEWHEEL:
			if (m_input.HandleMouseWheel(hWnd, GET_WHEEL_DELTA_WPARAM(wParam)))
			{
				return 0;
			}
			break;

		case WM_SIZE:
			if (hWnd == m_renderWnd)
			{
				HandleRenderWindowSized(wParam);
			}
			break;

		case WM_MOVING:
			if (hWnd == m_renderWnd && lParam)
			{
				HandleRenderWindowMoving(lParam);
			}
			break;

		case WM_MOVE:
			if (hWnd == m_renderWnd)
			{
				HandleRenderWindowMoved();
			}
			break;

		case WM_EXITSIZEMOVE:
			if (hWnd == m_renderWnd)
			{
				SaveSettingsIfRequested();
				return 0;
			}
			break;

		case WM_DPICHANGED:
			if (hWnd == m_renderWnd)
			{
				return HandleRenderWindowDpiChanged(hWnd, lParam);
			}
			break;

		case WM_CLOSE:
			if (hWnd == m_renderWnd)
			{
				return HandleRenderWindowClose(hWnd);
			}
			break;

		case WM_DESTROY:
			if (hWnd == m_renderWnd)
			{
				return HandleRenderWindowDestroy();
			}
			return 0;

		case WM_CANCELMODE:
		case WM_KILLFOCUS:
		case WM_ACTIVATEAPP:
			if (hWnd == m_gizmoWnd)
			{
				m_input.CancelGizmoDrag(hWnd);
			}
			break;

		case WM_CAPTURECHANGED:
			if (m_input.HandleCaptureChanged(hWnd))
			{
				return 0;
			}
			break;

		case WM_ERASEBKGND:
			if (hWnd == m_gizmoWnd)
			{
				return 1;
			}
			break;

		case WM_PAINT:
			if (hWnd == m_gizmoWnd)
			{
				PAINTSTRUCT ps{};
				BeginPaint(hWnd, &ps);
				RenderGizmo();
				EndPaint(hWnd, &ps);
				return 0;
			}
			break;

		case WM_QUERYENDSESSION:
			return TRUE;

		case WM_ENDSESSION:
			if (wParam && hWnd == m_renderWnd)
			{
				SaveSettingsIfRequested();
			}
			return 0;

		default:
			break;
	}

	return DefWindowProcW(hWnd, msg, wParam, lParam);
}
