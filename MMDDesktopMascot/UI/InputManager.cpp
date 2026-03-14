#include "InputManager.hpp"
#include "App.hpp"
#include <array>
#include <string>
#include <windowsx.h>

namespace
{
	struct HotKeyBinding
	{
		int id;
		UINT modifiers;
		UINT virtualKey;
		const char* debugName;
		void (App::*action)();
	};

	constexpr std::array kHotKeys = {
		HotKeyBinding{ 1, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'G', "Ctrl+Alt+G", &App::ToggleGizmoWindow },
		HotKeyBinding{ 2, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'P', "Ctrl+Alt+P", &App::TogglePhysics },
		HotKeyBinding{ 3, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'R', "Ctrl+Alt+R", &App::ToggleWindowManipulation }
	};
}

InputManager::InputManager(App& app)
	: m_app(app)
{
}

void InputManager::RegisterHotkeys(HWND renderWnd)
{
	if (!renderWnd) return;

	for (const HotKeyBinding& hotKey : kHotKeys)
	{
		if (!RegisterHotKey(renderWnd, hotKey.id, hotKey.modifiers, hotKey.virtualKey))
		{
			OutputDebugStringA((std::string("RegisterHotKey failed (") + hotKey.debugName + ").\n").c_str());
		}
	}
}

void InputManager::UnregisterHotkeys(HWND renderWnd)
{
	if (!renderWnd) return;

	for (const HotKeyBinding& hotKey : kHotKeys)
	{
		UnregisterHotKey(renderWnd, hotKey.id);
	}
}

void InputManager::SetWindows(HWND gizmoWnd)
{
	m_gizmoWnd = gizmoWnd;
}

bool InputManager::HandleHotkey(WPARAM wParam)
{
	for (const HotKeyBinding& hotKey : kHotKeys)
	{
		if (wParam == static_cast<WPARAM>(hotKey.id))
		{
			(m_app.*(hotKey.action))();
			return true;
		}
	}

	return false;
}

bool InputManager::HandleMouseDown(HWND hWnd, UINT msg)
{
	if (!IsGizmoWindow(hWnd)) return false;

	if (msg == WM_LBUTTONDOWN)
	{
		BeginGizmoDrag(hWnd, GizmoDragMode::MoveWindow);
		return true;
	}
	if (msg == WM_RBUTTONDOWN)
	{
		BeginGizmoDrag(hWnd, GizmoDragMode::RotateCamera);
		return true;
	}
	return false;
}

bool InputManager::HandleMouseUp(HWND hWnd, UINT msg)
{
	if (!IsGizmoWindow(hWnd)) return false;

	if (msg == WM_LBUTTONUP && m_gizmoDragMode == GizmoDragMode::MoveWindow)
	{
		EndGizmoDrag();
		return true;
	}
	if (msg == WM_RBUTTONUP && m_gizmoDragMode == GizmoDragMode::RotateCamera)
	{
		EndGizmoDrag();
		return true;
	}
	return false;
}

bool InputManager::HandleMouseMove(HWND hWnd)
{
	if (!IsGizmoWindow(hWnd) || m_gizmoDragMode == GizmoDragMode::None) return false;

	POINT cursorNow{};
	GetCursorPos(&cursorNow);
	const int dx = cursorNow.x - m_gizmoLastCursor.x;
	const int dy = cursorNow.y - m_gizmoLastCursor.y;
	m_gizmoLastCursor = cursorNow;

	if (m_gizmoDragMode == GizmoDragMode::MoveWindow)
	{
		m_app.MoveRenderWindowBy(dx, dy);
	}
	else
	{
		m_app.AddCameraRotation(static_cast<float>(dx), static_cast<float>(dy));
	}

	m_app.RenderGizmo();
	return true;
}

bool InputManager::HandleMouseWheel(HWND hWnd, int delta)
{
	if (!IsGizmoWindow(hWnd)) return false;

	const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
	const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;

	if (ctrl && shift)
	{
		const float adjustment = (delta > 0) ? 0.1f : -0.1f;
		m_app.AdjustScale(adjustment);
		return true;
	}
	if (ctrl)
	{
		const float adjustment = (delta > 0) ? 0.1f : -0.1f;
		m_app.AdjustBrightness(adjustment);
		return true;
	}

	const float steps = static_cast<float>(delta) / static_cast<float>(WHEEL_DELTA);
	m_app.AddCameraRotation(steps * 12.0f, 0.0f);
	return true;
}

bool InputManager::HandleCaptureChanged(HWND hWnd)
{
	if (!IsGizmoWindow(hWnd)) return false;
	ResetGizmoDrag();
	return true;
}

void InputManager::CancelGizmoDrag(HWND hWnd)
{
	if (!IsGizmoWindow(hWnd)) return;
	ResetGizmoDrag();
	ReleaseCapture();
}

void InputManager::ResetGizmoDrag()
{
	m_gizmoDragMode = GizmoDragMode::None;
}

bool InputManager::IsGizmoWindow(HWND hWnd) const
{
	return hWnd == m_gizmoWnd;
}

void InputManager::BeginGizmoDrag(HWND hWnd, GizmoDragMode mode)
{
	m_gizmoDragMode = mode;
	SetCapture(hWnd);
	GetCursorPos(&m_gizmoLastCursor);
}

void InputManager::EndGizmoDrag()
{
	m_gizmoDragMode = GizmoDragMode::None;
	ReleaseCapture();
}
