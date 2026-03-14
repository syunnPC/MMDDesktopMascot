#pragma once

#include <windows.h>

namespace Win32UiUtil
{
	inline constexpr UINT kDarkScrollbarsRefreshMessage = WM_APP + 0x4A3;

	HMENU ControlIdToMenuHandle(int id) noexcept;
	HFONT CreateSegoeUiFont(int logicalHeight, int weight);
	void ApplyControlFont(HWND hwnd, HFONT font, BOOL redraw = TRUE);
	void RegisterWindowClassOrThrow(const WNDCLASSEXW& windowClass, const char* context, bool allowAlreadyExists = false);

	SIZE GetClientSize(HWND hwnd);
	RECT GetWorkArea();

	void RequireWindows11();
	void EnablePerMonitorDpiAwareness();

	void EnableImmersiveDarkMode(HWND hwnd);
	void InitializeDarkModeSupport();
	void ApplyDarkControlTheme(HWND hwnd);
	void ResetDarkControlTheme(HWND hwnd);
	bool ConsumeThemeChange(HWND hwnd);
	void ScheduleDarkScrollbarsRefresh(HWND hwnd);
	void CompleteDarkScrollbarsRefresh(HWND ownerHwnd, HWND themedHwnd);

	template <typename T>
	T* InitializeWindowUserData(HWND hwnd, UINT msg, LPARAM lParam)
	{
		if (msg == WM_NCCREATE)
		{
			const auto* createStruct = reinterpret_cast<const CREATESTRUCTW*>(lParam);
			auto* instance = static_cast<T*>(createStruct->lpCreateParams);
			SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(instance));
			return instance;
		}

		return reinterpret_cast<T*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
	}

	template <typename T>
	T* GetWindowUserData(HWND hwnd)
	{
		return reinterpret_cast<T*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
	}
}
