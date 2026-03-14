#pragma once

#include <windows.h>
#include <functional>
#include <winrt/base.h>

struct ID2D1Factory;
struct ID2D1DCRenderTarget;
struct ID2D1SolidColorBrush;

struct AppSettings;
class DcompRenderer;
class InputManager;
class TrayIcon;

class WindowManager
{
public:
	struct Callbacks
	{
		std::function<void(const POINT&)> onTrayMenuRequested;
		std::function<void(UINT)> onTrayCommand;
		std::function<void(WPARAM, LPARAM)> onLoadProgress;
		std::function<void(WPARAM, LPARAM)> onLoadComplete;
		std::function<void()> onSaveSettings;
	};

	static constexpr UINT kLoadCompleteMessage = WM_APP + 200;
	static constexpr UINT kLoadProgressMessage = WM_APP + 201;

	WindowManager(HINSTANCE hInst, InputManager& input, AppSettings& settings, Callbacks callbacks);
	~WindowManager();

	WindowManager(const WindowManager&) = delete;
	WindowManager& operator=(const WindowManager&) = delete;

	void Initialize();

	HWND MessageWindow() const
	{
		return m_msgWnd;
	}
	HWND RenderWindow() const
	{
		return m_renderWnd;
	}
	HWND GizmoWindow() const
	{
		return m_gizmoWnd;
	}

	void SetRenderer(DcompRenderer* renderer);
	void SetTray(TrayIcon* tray);

	void ApplyTopmost(bool alwaysOnTop) const;

	void ToggleGizmoWindow();
	void PositionGizmoWindow();
	bool IsGizmoVisible() const
	{
		return m_gizmoVisible;
	}

	void RenderGizmo();

	void InstallRenderClickThrough();
	void ForceRenderTreeClickThrough();
	void ToggleWindowManipulationMode();
	bool IsWindowManipulationMode() const;

	void UpdateSettingsForRenderSize();

private:
	static LRESULT CALLBACK WndProcThunk(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
	LRESULT WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

	void CreateHiddenMessageWindow();
	void CreateRenderWindow();
	void CreateGizmoWindow();
	void SetGizmoVisible(bool visible);
	void PositionGizmoWindowForRenderRect(const RECT& renderWindowRect);

	void EnsureGizmoD2D();
	void DiscardGizmoD2D();

	static LRESULT CALLBACK RenderClickThroughProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
	void ForceRenderTreeClickThroughFor(HWND hWnd);
	static BOOL CALLBACK EnumChildForClickThrough(HWND hWnd, LPARAM lParam);
	static void MakeClickThrough(HWND hWnd);

	void ApplyWindowManipulationMode(bool enabled);
	void SetRenderTreeInteractivity(bool interactive) const;
	void UpdateStoredRenderSize();
	void RepositionVisibleGizmoWindow();
	bool HandleTrayCallbackMessage(UINT msg, LPARAM lParam) const;
	bool HandleOwnedMessage(UINT msg, WPARAM wParam, LPARAM lParam) const;
	void SaveSettingsIfRequested() const;
	void HandleRenderWindowSized(WPARAM sizeType);
	void HandleRenderWindowMoving(LPARAM lParam);
	void HandleRenderWindowMoved();
	LRESULT HandleRenderWindowDpiChanged(HWND hWnd, LPARAM lParam);
	LRESULT HandleRenderWindowClose(HWND hWnd);
	LRESULT HandleRenderWindowDestroy();

	HINSTANCE m_hInst{};
	InputManager& m_input;
	AppSettings& m_settings;
	Callbacks m_callbacks{};

	HWND m_msgWnd{};
	HWND m_renderWnd{};
	HWND m_gizmoWnd{};

	TrayIcon* m_tray{};

	DcompRenderer* m_renderer{};

	bool m_gizmoVisible{ false };

	winrt::com_ptr<ID2D1Factory> m_d2dFactory;
	winrt::com_ptr<ID2D1DCRenderTarget> m_gizmoRt;
	winrt::com_ptr<ID2D1SolidColorBrush> m_gizmoBrushFill;
	winrt::com_ptr<ID2D1SolidColorBrush> m_gizmoBrushStroke;

	HDC m_gizmoDc{ nullptr };
	HBITMAP m_gizmoBmp{ nullptr };
	HGDIOBJ m_gizmoOldBmp{ nullptr };
	void* m_gizmoBits{ nullptr };

	WNDPROC m_prevRenderWndProc = nullptr;
};
