#pragma once

#include <windows.h>

class App;

class InputManager
{
public:
	explicit InputManager(App& app);

	void RegisterHotkeys(HWND renderWnd);
	void UnregisterHotkeys(HWND renderWnd);
	void SetWindows(HWND gizmoWnd);

	bool HandleHotkey(WPARAM wParam);
	bool HandleMouseDown(HWND hWnd, UINT msg);
	bool HandleMouseUp(HWND hWnd, UINT msg);
	bool HandleMouseMove(HWND hWnd);
	bool HandleMouseWheel(HWND hWnd, int delta);
	bool HandleCaptureChanged(HWND hWnd);
	void CancelGizmoDrag(HWND hWnd);
	void ResetGizmoDrag();

private:
	enum class GizmoDragMode
	{
		None,
		MoveWindow,
		RotateCamera
	};

	bool IsGizmoWindow(HWND hWnd) const;
	void BeginGizmoDrag(HWND hWnd, GizmoDragMode mode);
	void EndGizmoDrag();

	App& m_app;
	HWND m_gizmoWnd{};

	GizmoDragMode m_gizmoDragMode{ GizmoDragMode::None };
	POINT m_gizmoLastCursor{};
};
