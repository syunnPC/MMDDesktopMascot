#pragma once

#include <windows.h>
#include <functional>
#include <string>
#include <vector>
#include <set>
#include <memory>

struct TrayMenuItem
{
	enum class Kind
	{
		Action,
		Toggle,
		Header,
		Separator
	};

	Kind kind{ Kind::Action };
	UINT commandId{};
	std::wstring title;
	std::wstring subtitle;
	bool toggled{ false };
	bool destructive{ false };
	std::vector<TrayMenuItem> children;
};

struct TrayMenuModel
{
	std::wstring title;
	std::wstring subtitle;
	std::vector<TrayMenuItem> items;
};

enum class TrayMenuThemeId
{
	DarkDefault,
	Light,
	Midnight,
	Sakura,
	SolarizedDark,
	HighContrast,
	Custom
};

struct TrayMenuTheme
{
	COLORREF background{};
	COLORREF headerBackground{};
	COLORREF cardHover{};
	COLORREF textPrimary{};
	COLORREF textMuted{};
	COLORREF accent{};
	COLORREF danger{};
	COLORREF outline{};
};


class TrayMenuWindow
{
public:
	TrayMenuWindow(HINSTANCE hInst, std::function<void(UINT)> onCommand);
	~TrayMenuWindow();

	TrayMenuWindow(const TrayMenuWindow&) = delete;
	TrayMenuWindow& operator=(const TrayMenuWindow&) = delete;

	void SetModel(const TrayMenuModel& model);
	void ShowAt(POINT anchor);
	void Hide();


	void SetTheme(TrayMenuThemeId id);
	void SetTheme(const TrayMenuTheme& customTheme);
	TrayMenuThemeId GetThemeId() const
	{
		return m_themeId;
	}
	const TrayMenuTheme& GetTheme() const
	{
		return m_theme;
	}

	bool IsVisible() const
	{
		return m_visible;
	}

	void SetIsSubMenu(bool isSubMenu);
	HWND GetHwnd() const
	{
		return m_hWnd;
	}

private:
	struct LayoutItem
	{
		TrayMenuItem data;
		RECT bounds{};
		int modelIndex{ -1 };
	};

	static LRESULT CALLBACK WndProcThunk(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
	LRESULT WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

	void EnsureWindow();
	void EnsureFonts();
	void RebuildLayout();
	void ResetFonts();
	void UpdateAdaptiveScaleForPoint(POINT anchor);
	void Paint(HDC hdc);
	void DrawContents(HDC hdc);
	void DrawHeader(HDC hdc, int offsetY) const;
	void DrawItem(HDC hdc, const LayoutItem& item, int offsetY) const;
	void DrawScrollbar(HDC hdc) const;
	void DrawToggleSwitch(HDC hdc, const RECT& pill, bool toggled) const;
	void HandleMouse(POINT pt, bool activate);
	void HandleCommand(const TrayMenuItem& item);
	void OnMouseWheelDelta(int wheelDelta);

	void HideLocal();
	void UpdateTopLevelPlacement();

	int Scale(int value) const;
	POINT AdjustAnchorToWorkArea(const SIZE& size, POINT anchor) const;

	// --- Submenu / capture routing ---
	TrayMenuWindow* GetRootWindow();
	bool ContainsScreenPoint(POINT screenPt) const;
	TrayMenuWindow* HitTestDeepestWindow(POINT screenPt);

	void RouteCapturedMouseMove();
	void RouteCapturedMouseButton(UINT msg, WPARAM wParam, LPARAM lParam);
	void RouteCapturedMouseWheel(WPARAM wParam, LPARAM lParam);

	// --- Submenu closing delay ---
	void CancelPendingSubMenuClose();
	void ArmPendingSubMenuClose(int hoveredIndex, UINT delayMs);


	HINSTANCE m_hInst{};
	HWND m_hWnd{};
	std::function<void(UINT)> m_onCommand;

	UINT m_dpi{ 96 };
	int m_resolutionScalePct{ 100 };
	HFONT m_titleFont{ nullptr };
	HFONT m_bodyFont{ nullptr };
	HFONT m_headerFont{ nullptr };


	TrayMenuTheme m_theme{};
	TrayMenuThemeId m_themeId{ TrayMenuThemeId::DarkDefault };

	TrayMenuModel m_model;
	std::vector<LayoutItem> m_layout;
	SIZE m_windowSize{ 360, 0 };
	int m_headerHeight{ 0 };
	int m_hoveredIndex{ -1 };
	int m_totalHeight{ 0 };
	int m_scrollOffset{ 0 };
	int m_maxScroll{ 0 };
	bool m_trackingMouse{ false };
	bool m_visible{ false };
	int m_maxWidthOverride{ 0 };
	int m_maxHeightOverride{ 0 };
	static constexpr UINT_PTR kTimerSubMenuClose = 1;
	bool m_subMenuCloseTimerArmed{ false };

	ULONGLONG m_openTime{ 0 };
	POINT m_lastAnchor{};
	bool m_hasLastAnchor{ false };
	std::set<std::wstring> m_collapsedHeaders;

	std::unique_ptr<TrayMenuWindow> m_subMenu;
	TrayMenuWindow* m_parentWindow = nullptr;
	bool m_isSubMenu = false;
	int m_openSubMenuIndex = -1;

	void CloseSubMenu();
	void OpenSubMenu(int index);
};
