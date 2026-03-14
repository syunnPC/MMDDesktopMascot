#include "Win32UiUtil.hpp"

#include <algorithm>
#include <format>
#include <stdexcept>
#include <string>

#include <dwmapi.h>
#include <uxtheme.h>

#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "uxtheme.lib")

namespace
{
	constexpr DWORD kImmersiveDarkModeAttribute = 20;

	constexpr wchar_t kThemePendingProperty[] = L"__MMD_ThemeRefreshPending";
	constexpr wchar_t kThemeAppliedProperty[] = L"__MMD_DarkThemeApplied";
	constexpr wchar_t kIgnoreThemeChangeProperty[] = L"__MMD_IgnoreThemeChangeCount";

	using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);

	enum class PreferredAppMode : int
	{
		Default = 0,
		AllowDark = 1,
		ForceDark = 2,
		ForceLight = 3,
		Max = 4
	};

	using SetPreferredAppModeFn = PreferredAppMode(WINAPI*)(PreferredAppMode);
	using AllowDarkModeForWindowFn = BOOL(WINAPI*)(HWND, BOOL);
	using RefreshImmersiveColorPolicyStateFn = void (WINAPI*)();
	using FlushMenuThemesFn = void (WINAPI*)();

	struct DarkModeApi
	{
		HMODULE module{};
		SetPreferredAppModeFn setPreferredAppMode{};
		AllowDarkModeForWindowFn allowDarkModeForWindow{};
		RefreshImmersiveColorPolicyStateFn refreshImmersiveColorPolicyState{};
		FlushMenuThemesFn flushMenuThemes{};

		DarkModeApi()
		{
			module = LoadLibraryW(L"uxtheme.dll");
			if (!module)
			{
				throw std::runtime_error("LoadLibraryW(uxtheme.dll) failed.");
			}

			setPreferredAppMode = reinterpret_cast<SetPreferredAppModeFn>(GetProcAddress(module, MAKEINTRESOURCEA(135)));
			allowDarkModeForWindow = reinterpret_cast<AllowDarkModeForWindowFn>(GetProcAddress(module, MAKEINTRESOURCEA(133)));
			refreshImmersiveColorPolicyState = reinterpret_cast<RefreshImmersiveColorPolicyStateFn>(GetProcAddress(module, MAKEINTRESOURCEA(104)));
			flushMenuThemes = reinterpret_cast<FlushMenuThemesFn>(GetProcAddress(module, MAKEINTRESOURCEA(136)));

			if (!setPreferredAppMode || !allowDarkModeForWindow || !refreshImmersiveColorPolicyState || !flushMenuThemes)
			{
				throw std::runtime_error("Windows 11 dark mode APIs are unavailable.");
			}

			setPreferredAppMode(PreferredAppMode::AllowDark);
			refreshImmersiveColorPolicyState();
			flushMenuThemes();
		}

		~DarkModeApi()
		{
			if (module)
			{
				FreeLibrary(module);
			}
		}
	};

	DarkModeApi& GetDarkModeApi()
	{
		static DarkModeApi api;
		return api;
	}

	OSVERSIONINFOEXW GetWindowsVersion()
	{
		const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
		if (!ntdll)
		{
			throw std::runtime_error("GetModuleHandleW(ntdll.dll) failed.");
		}

		const auto rtlGetVersion = reinterpret_cast<RtlGetVersionFn>(GetProcAddress(ntdll, "RtlGetVersion"));
		if (!rtlGetVersion)
		{
			throw std::runtime_error("RtlGetVersion is unavailable.");
		}

		OSVERSIONINFOEXW versionInfo{};
		versionInfo.dwOSVersionInfoSize = sizeof(versionInfo);
		if (rtlGetVersion(reinterpret_cast<OSVERSIONINFOW*>(&versionInfo)) != 0)
		{
			throw std::runtime_error("RtlGetVersion failed.");
		}

		return versionInfo;
	}

	bool IsPerMonitorAwareV2Context(DPI_AWARENESS_CONTEXT context)
	{
		return AreDpiAwarenessContextsEqual(context, DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2) != FALSE;
	}
}

namespace Win32UiUtil
{
	HMENU ControlIdToMenuHandle(int id) noexcept
	{
		return reinterpret_cast<HMENU>(static_cast<INT_PTR>(id));
	}

	HFONT CreateSegoeUiFont(int logicalHeight, int weight)
	{
		return CreateFontW(
			logicalHeight, 0, 0, 0, weight, FALSE, FALSE, FALSE,
			DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
			CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
	}

	void RegisterWindowClassOrThrow(const WNDCLASSEXW& windowClass, const char* context, bool allowAlreadyExists)
	{
		if (RegisterClassExW(&windowClass))
		{
			return;
		}

		if (allowAlreadyExists && GetLastError() == ERROR_CLASS_ALREADY_EXISTS)
		{
			return;
		}

		throw std::runtime_error(std::format("RegisterClassExW ({}) failed.", context));
	}

	void ApplyControlFont(HWND hwnd, HFONT font, BOOL redraw)
	{
		if (!hwnd || !font)
		{
			return;
		}

		SendMessageW(hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(font), redraw);
	}

	SIZE GetClientSize(HWND hwnd)
	{
		RECT rect{};
		GetClientRect(hwnd, &rect);
		return {
			std::max(1L, rect.right - rect.left),
			std::max(1L, rect.bottom - rect.top)
		};
	}

	RECT GetWorkArea()
	{
		RECT workArea{};
		if (!SystemParametersInfoW(SPI_GETWORKAREA, 0, &workArea, 0))
		{
			throw std::runtime_error("SystemParametersInfoW(SPI_GETWORKAREA) failed.");
		}

		return workArea;
	}

	void RequireWindows11()
	{
		const OSVERSIONINFOEXW versionInfo = GetWindowsVersion();
		if (versionInfo.dwMajorVersion > 10)
		{
			return;
		}

		if (versionInfo.dwMajorVersion == 10 && versionInfo.dwBuildNumber >= 22000)
		{
			return;
		}

		throw std::runtime_error("This app supports Windows 11 only.");
	}

	void EnablePerMonitorDpiAwareness()
	{
		if (IsPerMonitorAwareV2Context(GetThreadDpiAwarenessContext()))
		{
			return;
		}

		if (!SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2))
		{
			const DWORD error = GetLastError();
			if (error == ERROR_ACCESS_DENIED && IsPerMonitorAwareV2Context(GetThreadDpiAwarenessContext()))
			{
				return;
			}

			throw std::runtime_error(
				"SetProcessDpiAwarenessContext(PER_MONITOR_AWARE_V2) failed. GetLastError=" + std::to_string(error));
		}
	}

	void EnableImmersiveDarkMode(HWND hwnd)
	{
		const BOOL enabled = TRUE;
		const HRESULT hr = DwmSetWindowAttribute(
			hwnd,
			kImmersiveDarkModeAttribute,
			&enabled,
			sizeof(enabled));

		if (FAILED(hr))
		{
			throw std::runtime_error("DwmSetWindowAttribute(DWMWA_USE_IMMERSIVE_DARK_MODE) failed.");
		}
	}

	void InitializeDarkModeSupport()
	{
		static_cast<void>(GetDarkModeApi());
	}

	void ApplyDarkControlTheme(HWND hwnd)
	{
		if (!hwnd)
		{
			return;
		}

		auto& api = GetDarkModeApi();
		api.allowDarkModeForWindow(hwnd, TRUE);

		if (GetPropW(hwnd, kThemeAppliedProperty))
		{
			return;
		}

		SetPropW(hwnd, kIgnoreThemeChangeProperty, reinterpret_cast<HANDLE>(2));
		SetWindowTheme(hwnd, L"DarkMode_Explorer", nullptr);
		SetPropW(hwnd, kThemeAppliedProperty, reinterpret_cast<HANDLE>(1));
	}

	void ResetDarkControlTheme(HWND hwnd)
	{
		if (!hwnd)
		{
			return;
		}

		RemovePropW(hwnd, kThemeAppliedProperty);
	}

	bool ConsumeThemeChange(HWND hwnd)
	{
		const auto ignoreCount = reinterpret_cast<UINT_PTR>(GetPropW(hwnd, kIgnoreThemeChangeProperty));
		if (!ignoreCount)
		{
			return false;
		}

		if (ignoreCount <= 1)
		{
			RemovePropW(hwnd, kIgnoreThemeChangeProperty);
		}
		else
		{
			SetPropW(hwnd, kIgnoreThemeChangeProperty, reinterpret_cast<HANDLE>(ignoreCount - 1));
		}

		return true;
	}

	void ScheduleDarkScrollbarsRefresh(HWND hwnd)
	{
		if (!hwnd || !IsWindow(hwnd))
		{
			return;
		}

		if (GetPropW(hwnd, kThemePendingProperty))
		{
			return;
		}

		SetPropW(hwnd, kThemePendingProperty, reinterpret_cast<HANDLE>(1));
		PostMessageW(hwnd, Win32UiUtil::kDarkScrollbarsRefreshMessage, 0, 0);
	}

	void CompleteDarkScrollbarsRefresh(HWND ownerHwnd, HWND themedHwnd)
	{
		if (ownerHwnd)
		{
			RemovePropW(ownerHwnd, kThemePendingProperty);
		}

		if (themedHwnd)
		{
			ApplyDarkControlTheme(themedHwnd);
		}
	}
}
