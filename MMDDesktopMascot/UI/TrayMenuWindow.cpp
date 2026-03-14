#ifndef NOMINMAX
#define NOMINMAX
#endif

#ifdef WIN32_LEAN_AND_MEAN
#undef WIN32_LEAN_AND_MEAN
#endif

#include "TrayMenuWindow.hpp"
#include "Win32UiUtil.hpp"
#include <algorithm>
#include <unordered_map>
#include <utility>
#include <windowsx.h>

#include <gdiplus.h>

#pragma comment(lib, "gdiplus.lib")

namespace
{
	constexpr wchar_t kWindowClass[] = L"MMDDesk.TrayMenuWindow";

	COLORREF Rgb(BYTE r, BYTE g, BYTE b)
	{
		return RGB(r, g, b);
	}

	std::wstring ToLowerAscii(std::wstring s)
	{
		for (auto& ch : s)
		{
			if (ch >= L'A' && ch <= L'Z')
			{
				ch = static_cast<wchar_t>(ch - L'A' + L'a');
			}
		}
		return s;
	}

	bool IsMotionHeaderTitle(const std::wstring& t)
	{
		if (t == L"モーション") return true;
		const std::wstring s = ToLowerAscii(t);
		return (s == L"motion" || s == L"motions");
	}

	TrayMenuTheme MakePresetTheme(TrayMenuThemeId id)
	{
		switch (id)
		{
			case TrayMenuThemeId::DarkDefault:
				return TrayMenuTheme{
					Rgb(20, 22, 27),   // background
					Rgb(28, 30, 36),   // headerBackground
					Rgb(38, 43, 52),   // cardHover
					Rgb(235, 238, 243),// textPrimary
					Rgb(165, 169, 179),// textMuted
					Rgb(0, 120, 215),  // accent
					Rgb(203, 68, 80),  // danger
					Rgb(64, 68, 78)    // outline
				};

			case TrayMenuThemeId::Light:
				return TrayMenuTheme{
					Rgb(250, 250, 252),
					Rgb(242, 244, 248),
					Rgb(230, 234, 242),
					Rgb(20, 22, 26),
					Rgb(92, 96, 105),
					Rgb(0, 120, 215),
					Rgb(196, 59, 76),
					Rgb(200, 205, 214)
				};

			case TrayMenuThemeId::Midnight:
				return TrayMenuTheme{
					Rgb(10, 14, 24),   // deep navy
					Rgb(16, 20, 34),
					Rgb(26, 34, 56),
					Rgb(236, 241, 250),
					Rgb(150, 165, 190),
					Rgb(0, 203, 255),  // cyan accent
					Rgb(255, 82, 110),
					Rgb(52, 62, 86)
				};

			case TrayMenuThemeId::Sakura:
				// dark base + pink accent
				return TrayMenuTheme{
					Rgb(26, 20, 24),
					Rgb(36, 27, 32),
					Rgb(50, 36, 43),
					Rgb(244, 232, 240),
					Rgb(200, 176, 192),
					Rgb(255, 95, 162),
					Rgb(255, 75, 92),
					Rgb(78, 56, 66)
				};

			case TrayMenuThemeId::SolarizedDark:
				// Solarized (base03/base02/base01 etc.)
				return TrayMenuTheme{
					Rgb(0, 43, 54),    // base03
					Rgb(7, 54, 66),    // base02
					Rgb(12, 73, 88),   // hover (slightly brighter)
					Rgb(238, 232, 213),// base2
					Rgb(147, 161, 161),// base1
					Rgb(38, 139, 210), // blue
					Rgb(220, 50, 47),  // red
					Rgb(88, 110, 117)  // base01
				};

			case TrayMenuThemeId::HighContrast:
				return TrayMenuTheme{
					Rgb(0, 0, 0),
					Rgb(0, 0, 0),
					Rgb(35, 35, 35),
					Rgb(255, 255, 255),
					Rgb(200, 200, 200),
					Rgb(255, 215, 0),  // yellow accent
					Rgb(255, 80, 80),
					Rgb(255, 255, 255)
				};

			default:
				break;
		}

		return MakePresetTheme(TrayMenuThemeId::DarkDefault);
	}

	int GetSystemDpi(HWND hWnd)
	{
		if (hWnd)
		{
			const UINT dpiForWindow = GetDpiForWindow(hWnd);
			if (dpiForWindow != 0) return static_cast<int>(dpiForWindow);
		}

		const UINT dpiForSystem = GetDpiForSystem();
		return (dpiForSystem != 0) ? static_cast<int>(dpiForSystem) : 96;
	}

	int ComputeResolutionScalePercent(const RECT& workArea)
	{
		const int workW = std::max(1l, workArea.right - workArea.left);
		const int workH = std::max(1l, workArea.bottom - workArea.top);
		const int widthPct = std::clamp((workW * 100) / 1366, 85, 100);
		const int heightPct = std::clamp((workH * 100) / 800, 85, 100);
		return (std::min)(widthPct, heightPct);
	}

	Gdiplus::Color ToColor(COLORREF c, BYTE a = 255)
	{
		return Gdiplus::Color(a, GetRValue(c), GetGValue(c), GetBValue(c));
	}

	void AddRoundedRect(Gdiplus::GraphicsPath& path, const Gdiplus::RectF& rect, float radius)
	{
		const float diameter = radius * 2.0f;
		const float right = rect.X + rect.Width;
		const float bottom = rect.Y + rect.Height;

		path.AddArc(rect.X, rect.Y, diameter, diameter, 180.0f, 90.0f);
		path.AddArc(right - diameter, rect.Y, diameter, diameter, 270.0f, 90.0f);
		path.AddArc(right - diameter, bottom - diameter, diameter, diameter, 0.0f, 90.0f);
		path.AddArc(rect.X, bottom - diameter, diameter, diameter, 90.0f, 90.0f);
		path.CloseFigure();
	}

	struct GdiplusInitializer
	{
		GdiplusInitializer()
		{
			Gdiplus::GdiplusStartupInput input{};
			Gdiplus::GdiplusStartup(&token, &input, nullptr);
		}

		~GdiplusInitializer()
		{
			if (token)
			{
				Gdiplus::GdiplusShutdown(token);
			}
		}

		ULONG_PTR token{};
	};

	GdiplusInitializer g_gdiplusInit{};
}

TrayMenuWindow::TrayMenuWindow(HINSTANCE hInst, std::function<void(UINT)> onCommand)
	: m_hInst(hInst)
	, m_onCommand(std::move(onCommand))
{
	m_themeId = TrayMenuThemeId::DarkDefault;
	m_theme = MakePresetTheme(m_themeId);

	WNDCLASSEXW wc{};
	wc.cbSize = sizeof(wc);
	wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
	wc.lpfnWndProc = &TrayMenuWindow::WndProcThunk;
	wc.hInstance = m_hInst;
	wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
	wc.lpszClassName = kWindowClass;
	Win32UiUtil::RegisterWindowClassOrThrow(wc, "TrayMenuWindow", true);
}

TrayMenuWindow::~TrayMenuWindow()
{
	// 親ウィンドウとして振る舞っている場合、子ウィンドウを先に破棄
	m_subMenu.reset();

	if (m_bodyFont) DeleteObject(m_bodyFont);
	if (m_titleFont) DeleteObject(m_titleFont);
	if (m_headerFont) DeleteObject(m_headerFont);
	if (m_hWnd) DestroyWindow(m_hWnd);
}

void TrayMenuWindow::SetIsSubMenu(bool isSubMenu)
{
	m_isSubMenu = isSubMenu;
}

void TrayMenuWindow::SetModel(const TrayMenuModel& model)
{
	m_model = model;
	if (m_model.title.empty())
	{
		m_model.title = L"MMD Desktop Mascot";
	}

	// モデル変更時にレイアウト再構築
	RebuildLayout();
	if (m_hWnd && m_visible)
	{
		UpdateTopLevelPlacement();
		InvalidateRect(m_hWnd, nullptr, FALSE);
	}
}

void TrayMenuWindow::SetTheme(TrayMenuThemeId id)
{
	m_themeId = id;
	if (m_themeId == TrayMenuThemeId::Custom)
	{
		m_themeId = TrayMenuThemeId::DarkDefault;
	}

	m_theme = MakePresetTheme(m_themeId);

	if (m_hWnd && m_visible)
	{
		InvalidateRect(m_hWnd, nullptr, FALSE);
		UpdateWindow(m_hWnd);
	}
}

void TrayMenuWindow::SetTheme(const TrayMenuTheme& customTheme)
{
	m_themeId = TrayMenuThemeId::Custom;
	m_theme = customTheme;

	if (m_hWnd && m_visible)
	{
		InvalidateRect(m_hWnd, nullptr, FALSE);
		UpdateWindow(m_hWnd);
	}
}

void TrayMenuWindow::EnsureWindow()
{
	if (m_hWnd) return;

	m_dpi = GetSystemDpi(GetDesktopWindow());
	if (m_dpi <= 0) m_dpi = 96;
	EnsureFonts();

	m_hWnd = CreateWindowExW(
		WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
		kWindowClass,
		L"",
		WS_POPUP,
		0, 0,
		m_windowSize.cx,
		m_windowSize.cy,
		nullptr,
		nullptr,
		m_hInst,
		this);

	if (!m_hWnd)
	{
		OutputDebugStringW(L"TrayMenuWindow: CreateWindowExW failed.\r\n");
		return;
	}

	{
		const int newDpi = GetSystemDpi(m_hWnd);
		if (newDpi > 0 && newDpi != static_cast<int>(m_dpi))
		{
			m_dpi = static_cast<UINT>(newDpi);
			ResetFonts();
			EnsureFonts();
		}
	}

	Win32UiUtil::EnableImmersiveDarkMode(m_hWnd);
}


void TrayMenuWindow::EnsureFonts()
{
	if (m_bodyFont && m_titleFont && m_headerFont) return;

	const int effectiveDpi = std::max(1, MulDiv(static_cast<int>(m_dpi), m_resolutionScalePct, 100));
	const int titleSize = -MulDiv(18, effectiveDpi, 72);
	const int bodySize = -MulDiv(14, effectiveDpi, 72);
	const int headerSize = -MulDiv(11, effectiveDpi, 72);

	if (!m_titleFont)
	{
		m_titleFont = Win32UiUtil::CreateSegoeUiFont(titleSize, FW_SEMIBOLD);
	}
	if (!m_bodyFont)
	{
		m_bodyFont = Win32UiUtil::CreateSegoeUiFont(bodySize, FW_MEDIUM);
	}
	if (!m_headerFont)
	{
		m_headerFont = Win32UiUtil::CreateSegoeUiFont(headerSize, FW_SEMIBOLD);
	}
}

void TrayMenuWindow::ResetFonts()
{
	if (m_bodyFont) DeleteObject(m_bodyFont);
	if (m_titleFont) DeleteObject(m_titleFont);
	if (m_headerFont) DeleteObject(m_headerFont);
	m_bodyFont = nullptr;
	m_titleFont = nullptr;
	m_headerFont = nullptr;
}

void TrayMenuWindow::UpdateAdaptiveScaleForPoint(POINT anchor)
{
	HMONITOR monitor = MonitorFromPoint(anchor, MONITOR_DEFAULTTONEAREST);
	MONITORINFO info{ sizeof(info) };
	if (!GetMonitorInfoW(monitor, &info))
	{
		return;
	}

	const int newScalePct = ComputeResolutionScalePercent(info.rcWork);
	if (newScalePct == m_resolutionScalePct)
	{
		return;
	}

	m_resolutionScalePct = newScalePct;
	ResetFonts();
}

void TrayMenuWindow::RebuildLayout()
{
	EnsureFonts();

	HDC hdc = GetDC(nullptr);
	HGDIOBJ oldFont = SelectObject(hdc, m_bodyFont);

	const int padding = Scale(16);
	const int rightPadding = Scale(18);
	const int spacing = Scale(8);
	const int headerGap = Scale(10);
	const int separatorHeight = Scale(1);
	const int toggleWidth = Scale(48);
	const int minWidth = Scale(360);
	int maxHeight = Scale(540);
	if (m_maxHeightOverride > 0) maxHeight = m_maxHeightOverride;

	// --- ヘッダーの高さ計算 ---
	if (m_isSubMenu)
	{
		// サブメニューの場合はヘッダーを表示しない
		m_headerHeight = 0;
	}
	else
	{
		SIZE titleSize{ 0,0 };
		SIZE subtitleSize{ 0,0 };
		SelectObject(hdc, m_titleFont);
		GetTextExtentPoint32W(hdc, m_model.title.c_str(), static_cast<int>(m_model.title.size()), &titleSize);

		SelectObject(hdc, m_bodyFont);
		if (!m_model.subtitle.empty())
		{
			GetTextExtentPoint32W(hdc, m_model.subtitle.c_str(), static_cast<int>(m_model.subtitle.size()), &subtitleSize);
		}

		int calculatedHeaderHeight = padding + titleSize.cy;
		if (!m_model.subtitle.empty())
		{
			calculatedHeaderHeight += Scale(4) + subtitleSize.cy;
		}
		calculatedHeaderHeight += padding;

		m_headerHeight = std::max(Scale(64), calculatedHeaderHeight);
	}

	m_layout.clear();

	// サブメニューはヘッダーがないので padding から開始
	int y = m_headerHeight + (m_isSubMenu ? padding : spacing);
	int width = minWidth;
	if (m_isSubMenu) width = Scale(200); // サブメニューの最小幅

	bool currentSectionCollapsed = false;

	// 「モーション」はホバーで横に展開したいので、ヘッダー配下のフラットな項目を仮想 children にまとめて
	// 親ウィンドウ内で縦方向に展開されないようにする（Windows 95 スタートメニュー風）

	std::unordered_map<int, std::vector<TrayMenuItem>> virtualChildren;
	std::vector<bool> skip(m_model.items.size(), false);

	for (size_t i = 0; i < m_model.items.size(); ++i)
	{
		const auto& it = m_model.items[i];
		if (it.kind != TrayMenuItem::Kind::Header && it.kind != TrayMenuItem::Kind::Action) continue;
		if (!it.children.empty()) continue;
		if (!IsMotionHeaderTitle(it.title)) continue;

		std::vector<TrayMenuItem> kids;
		size_t j = i + 1;
		for (; j < m_model.items.size(); ++j)
		{
			if (m_model.items[j].kind == TrayMenuItem::Kind::Header) break;
			if (m_model.items[j].kind == TrayMenuItem::Kind::Separator) break;

			kids.push_back(m_model.items[j]);
			skip[j] = true;
		}

		if (!kids.empty())
		{
			virtualChildren[static_cast<int>(i)] = std::move(kids);
		}
	}

	for (size_t i = 0; i < m_model.items.size(); ++i)
	{
		if (skip[i]) continue;

		TrayMenuItem item = m_model.items[i];

		// Motion header -> サブメニュー用の通常行に変換（常に表示され、右/左に子を出す）
		bool forceVisible = false;
		if (auto it = virtualChildren.find(static_cast<int>(i)); it != virtualChildren.end())
		{
			// 元は Header なので、ここで「セクション境界」として collapse 状態をリセットする
			currentSectionCollapsed = false;

			TrayMenuItem converted = item;
			converted.kind = TrayMenuItem::Kind::Action;
			converted.commandId = 0;
			converted.toggled = false;
			converted.destructive = false;
			converted.children = it->second;

			item = std::move(converted);
			forceVisible = true;
		}

		LayoutItem layout{};
		layout.data = item;
		layout.modelIndex = static_cast<int>(i);

		if (item.kind == TrayMenuItem::Kind::Separator)
		{
			if (currentSectionCollapsed) continue;
			layout.bounds = { padding, y, width - rightPadding, y + separatorHeight };
			y += separatorHeight + spacing;
			m_layout.push_back(layout);
			continue;
		}

		if (item.kind == TrayMenuItem::Kind::Header)
		{
			currentSectionCollapsed = m_collapsedHeaders.count(item.title) > 0;
			SIZE headerSize{};
			HGDIOBJ old = SelectObject(hdc, m_headerFont);
			GetTextExtentPoint32W(hdc, item.title.c_str(), static_cast<int>(item.title.size()), &headerSize);
			SelectObject(hdc, old);

			layout.bounds = { padding, y, width - rightPadding, y + headerSize.cy + headerGap };
			y += headerSize.cy + headerGap;
			m_layout.push_back(layout);
			continue;
		}

		// --- 通常アイテム ---
		if (currentSectionCollapsed && !forceVisible) continue;

		int rowHeight = Scale(44);
		if (!item.subtitle.empty())
		{
			rowHeight = Scale(60);
		}

		SIZE textSize{};
		GetTextExtentPoint32W(hdc, item.title.c_str(), static_cast<int>(item.title.size()), &textSize);
		int rowWidth = textSize.cx + padding * 2;

		if (!item.subtitle.empty())
		{
			SIZE subSize{};
			GetTextExtentPoint32W(hdc, item.subtitle.c_str(), static_cast<int>(item.subtitle.size()), &subSize);
			rowWidth = std::max(static_cast<long>(rowWidth), subSize.cx + padding * 2);
		}

		// トグルスイッチまたはサブメニュー矢印のスペース
		if (item.kind == TrayMenuItem::Kind::Toggle)
		{
			rowWidth += toggleWidth + Scale(12);
		}
		else if (!item.children.empty())
		{
			rowWidth += Scale(20); // 矢印用
		}

		width = std::max(width, rowWidth);
		layout.bounds = { padding, y, width - rightPadding, y + rowHeight };
		y += rowHeight + spacing;
		m_layout.push_back(layout);
	}

	// サブメニューなら下部のパディングを追加
	if (m_isSubMenu) y += padding;

	m_totalHeight = y;

	// レイアウト計算後にすべての幅を最大幅に合わせる
	if (m_maxWidthOverride > 0) width = std::min(width, m_maxWidthOverride);
	for (auto& l : m_layout)
	{
		l.bounds.right = width - rightPadding;
	}

	m_windowSize.cx = width;
	m_windowSize.cy = std::min(m_totalHeight, maxHeight);

	const int contentHeight = std::max(0, m_totalHeight - m_headerHeight);
	const int viewportHeight = std::max(0l, m_windowSize.cy - m_headerHeight);
	m_maxScroll = std::max(0, contentHeight - viewportHeight);
	m_scrollOffset = std::min(m_scrollOffset, m_maxScroll);

	SelectObject(hdc, oldFont);
	ReleaseDC(nullptr, hdc);
}

void TrayMenuWindow::ShowAt(POINT anchor)
{
	m_lastAnchor = anchor;
	m_hasLastAnchor = true;
	m_openSubMenuIndex = -1;
	UpdateAdaptiveScaleForPoint(anchor);
	EnsureWindow();
	if (!m_hWnd) return;

	RebuildLayout();
	m_scrollOffset = 0;
	m_hoveredIndex = -1;
	m_openSubMenuIndex = -1;

	const POINT pos = AdjustAnchorToWorkArea(m_windowSize, anchor);

	m_openTime = GetTickCount64();

	SetWindowPos(
		m_hWnd,
		HWND_TOPMOST,
		pos.x,
		pos.y,
		m_windowSize.cx,
		m_windowSize.cy,
		SWP_SHOWWINDOW);

	SetForegroundWindow(m_hWnd);
	SetFocus(m_hWnd);

	UpdateWindow(m_hWnd);
	m_visible = true;

	SetCapture(m_hWnd);

	SetCursor(LoadCursor(nullptr, IDC_ARROW));
}

void TrayMenuWindow::UpdateTopLevelPlacement()
{
	if (!m_hWnd || !m_visible) return;
	if (m_isSubMenu) return;

	POINT anchor{};
	bool hasAnchor = m_hasLastAnchor;
	if (hasAnchor) anchor = m_lastAnchor;

	// アンカーが分からない場合は現在位置を基準にサイズだけ更新する
	RECT currentRc{};
	GetWindowRect(m_hWnd, &currentRc);
	POINT pos{ currentRc.left, currentRc.top };

	if (hasAnchor)
	{
		UpdateAdaptiveScaleForPoint(anchor);

		// 表示先モニターの work area に合わせてサイズ上限を更新（上下左右はみ出し対策）
		HMONITOR monitor = MonitorFromPoint(anchor, MONITOR_DEFAULTTONEAREST);
		MONITORINFO mi{ sizeof(mi) };
		if (GetMonitorInfoW(monitor, &mi))
		{
			const int margin = Scale(8);
			const int workW = static_cast<int>(mi.rcWork.right - mi.rcWork.left);
			const int workH = static_cast<int>(mi.rcWork.bottom - mi.rcWork.top);
			m_maxWidthOverride = std::max(1, workW - margin * 2);
			m_maxHeightOverride = std::max(1, workH - margin * 2);
		}

		pos = AdjustAnchorToWorkArea(m_windowSize, anchor);
	}

	SetWindowPos(
		m_hWnd,
		HWND_TOPMOST,
		pos.x,
		pos.y,
		m_windowSize.cx,
		m_windowSize.cy,
		SWP_NOACTIVATE);
}


void TrayMenuWindow::HideLocal()
{
	if (!m_hWnd || !m_visible) return;

	CancelPendingSubMenuClose();
	CloseSubMenu();

	// 実際にキャプチャを持っている場合だけ解放
	if (GetCapture() == m_hWnd)
	{
		ReleaseCapture();
	}

	ShowWindow(m_hWnd, SW_HIDE);
	m_visible = false;
	m_hoveredIndex = -1;

	m_scrollOffset = 0;
}


void TrayMenuWindow::Hide()
{
	if (!m_hWnd || !m_visible) return;

	HideLocal();

	// 親メニューも閉じる（チェーンを全て閉じる場合）
	if (m_parentWindow && m_parentWindow->m_visible)
	{
		m_parentWindow->Hide();
	}
}

// サブメニュー専用の「閉じる」処理（親は閉じない）
void TrayMenuWindow::CloseSubMenu()
{
	CancelPendingSubMenuClose();
	if (m_subMenu && m_subMenu->m_visible)
	{
		m_subMenu->HideLocal(); // 親は閉じない
	}
	m_openSubMenuIndex = -1;
}

// 特定のインデックスのサブメニューを開く
void TrayMenuWindow::OpenSubMenu(int index)
{
	CancelPendingSubMenuClose();
	if (index < 0 || index >= static_cast<int>(m_layout.size())) return;

	const auto& layoutItem = m_layout[index];
	if (layoutItem.data.children.empty())
		return;

	// 既存のサブメニューが開いている場合
	if (m_subMenu && m_subMenu->m_visible)
	{
		if (m_openSubMenuIndex == index) return;
		m_subMenu->HideLocal();
	}

	if (!m_subMenu)
	{
		m_subMenu = std::make_unique<TrayMenuWindow>(m_hInst, m_onCommand);
		m_subMenu->m_parentWindow = this;
		m_subMenu->SetIsSubMenu(true);
	}

	if (m_themeId == TrayMenuThemeId::Custom)
	{
		m_subMenu->SetTheme(m_theme);
	}
	else
	{
		m_subMenu->SetTheme(m_themeId);
	}

	TrayMenuModel subModel;
	subModel.title = layoutItem.data.title;
	subModel.subtitle.clear();
	subModel.items = layoutItem.data.children;

	m_subMenu->SetModel(subModel);
	m_openSubMenuIndex = index;

	RECT clientRc{};
	GetClientRect(m_hWnd, &clientRc);

	// Y座標はアイテムの上端に合わせる (スクロール考慮)
	RECT itemRect = layoutItem.bounds;
	OffsetRect(&itemRect, 0, -m_scrollOffset);

	// 親ウィンドウの右端・左端（スクリーン座標）
	POINT ptWindowRight{ clientRc.right, itemRect.top };
	POINT ptWindowLeft{ clientRc.left, itemRect.top };
	ClientToScreen(m_hWnd, &ptWindowRight);
	ClientToScreen(m_hWnd, &ptWindowLeft);
	m_subMenu->UpdateAdaptiveScaleForPoint(ptWindowRight);

	// モニター情報の取得
	HMONITOR monitor = MonitorFromPoint(ptWindowRight, MONITOR_DEFAULTTONEAREST);
	MONITORINFO mi{ sizeof(MONITORINFO) };
	GetMonitorInfoW(monitor, &mi);

	const int margin = Scale(8);
	const int workH = mi.rcWork.bottom - mi.rcWork.top;

	m_subMenu->m_maxHeightOverride = std::max(1, workH - margin * 2);
	m_subMenu->m_maxWidthOverride = 0;
	m_subMenu->EnsureWindow();
	m_subMenu->RebuildLayout();

	int desiredW = m_subMenu->m_windowSize.cx;
	int desiredH = m_subMenu->m_windowSize.cy;

	const int rightSpace = (mi.rcWork.right - margin) - ptWindowRight.x;
	const int leftSpace = ptWindowLeft.x - (mi.rcWork.left + margin);

	bool openRight = true;
	// 右側に十分なスペースがあるか、左側より広い場合は右に出す
	if (desiredW <= rightSpace) openRight = true;
	else if (desiredW <= leftSpace) openRight = false;
	else openRight = (rightSpace >= leftSpace);

	m_subMenu->m_maxWidthOverride = std::max(1, openRight ? rightSpace : leftSpace);
	m_subMenu->RebuildLayout();
	desiredW = m_subMenu->m_windowSize.cx;
	desiredH = m_subMenu->m_windowSize.cy;

	// 親メニューに「かぶらず」ぴったり隣へ出す。
	// ※ ClientRect の right はクライアント外側の境界なので、そのまま使えば隣接しつつ重ならない。
	const int overlap = 0;
	int subX = openRight ? (ptWindowRight.x - overlap) : (ptWindowLeft.x - desiredW + overlap);

	// Y座標は親アイテムの上端に合わせる（補正しない）
	int subY = ptWindowRight.y;

	// モニター範囲に収める
	if (subX < mi.rcWork.left + margin) subX = mi.rcWork.left + margin;
	if (subX + desiredW > mi.rcWork.right - margin) subX = mi.rcWork.right - margin - desiredW;

	if (subY < mi.rcWork.top + margin) subY = mi.rcWork.top + margin;
	if (subY + desiredH > mi.rcWork.bottom - margin) subY = mi.rcWork.bottom - margin - desiredH;

	SetWindowPos(m_subMenu->m_hWnd, HWND_TOPMOST, subX, subY, desiredW, desiredH,
				 SWP_NOACTIVATE | SWP_SHOWWINDOW);
	m_subMenu->m_visible = true;
	m_subMenu->m_openTime = GetTickCount64();
}

LRESULT CALLBACK TrayMenuWindow::WndProcThunk(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	TrayMenuWindow* self = Win32UiUtil::InitializeWindowUserData<TrayMenuWindow>(hWnd, msg, lParam);
	if (msg == WM_NCCREATE && self)
	{
		self->m_hWnd = hWnd;
	}

	if (!self)
	{
		return DefWindowProcW(hWnd, msg, wParam, lParam);
	}
	return self->WndProc(hWnd, msg, wParam, lParam);
}


LRESULT TrayMenuWindow::WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	if (!m_hWnd) m_hWnd = hWnd;

	const bool inGuardPeriod = (GetTickCount64() - m_openTime < 300);

	switch (msg)
	{
		case WM_SETCURSOR:
		{
			if (LOWORD(lParam) == HTCLIENT)
			{
				SetCursor(LoadCursor(nullptr, IDC_ARROW));
				return TRUE;
			}
			break;
		}

			case WM_NCDESTROY:
			{
				SetWindowLongPtrW(hWnd, GWLP_USERDATA, 0);
				if (GetCapture() == hWnd) ReleaseCapture();
				m_hWnd = nullptr;
				m_visible = false;
			return 0;
		}

		case WM_CAPTURECHANGED:
		{
			// キャプチャが変更されたとき
			HWND hNewCapture = reinterpret_cast<HWND>(lParam);

			// 1. 自分がキャプチャを失ったが、新しいキャプチャ先が自分のサブメニューなら閉じない
			if (m_subMenu && m_subMenu->m_visible && hNewCapture == m_subMenu->m_hWnd)
			{
				return 0;
			}

			// 2. 自分がキャプチャを失ったが、新しいキャプチャ先が自分の親なら閉じない
			if (m_parentWindow && hNewCapture == m_parentWindow->m_hWnd)
			{
				return 0;
			}

			// それ以外（外部クリックなど）なら閉じる
			if (!inGuardPeriod && m_visible)
			{
				// マウスが親ウィンドウの領域内に戻った場合は閉じずに親にキャプチャを返す
				POINT pt{};
				GetCursorPos(&pt);
				if (m_parentWindow && WindowFromPoint(pt) == m_parentWindow->m_hWnd)
				{
					// ReleaseCapture は既に呼ばれている
					return 0;
				}

				Hide();
			}
			return 0;
		}

		case WM_TIMER:
		{
			if (wParam == kTimerSubMenuClose)
			{
				KillTimer(hWnd, kTimerSubMenuClose);
				m_subMenuCloseTimerArmed = false;

				if (m_subMenu && m_subMenu->m_visible)
				{
					POINT spt{};
					GetCursorPos(&spt);
					if (!m_subMenu->ContainsScreenPoint(spt))
					{
						CloseSubMenu();
						InvalidateRect(hWnd, nullptr, FALSE);
					}
				}
				return 0;
			}
			break;
		}

		case WM_ACTIVATE:
		{
			if (LOWORD(wParam) == WA_INACTIVE)
			{
				// 親ウィンドウがアクティブなら閉じない
				HWND hOther = reinterpret_cast<HWND>(lParam);
				if (m_parentWindow && hOther == m_parentWindow->m_hWnd) return 0;
				if (m_subMenu && hOther == m_subMenu->m_hWnd) return 0;

				if (!inGuardPeriod) Hide();
			}
			return 0;
		}

		case WM_LBUTTONDOWN:
		case WM_RBUTTONDOWN:
		case WM_MBUTTONDOWN:
		case WM_LBUTTONUP:
		case WM_RBUTTONUP:
		case WM_MBUTTONUP:
		case WM_LBUTTONDBLCLK:
		case WM_RBUTTONDBLCLK:
		case WM_MBUTTONDBLCLK:
		{
			// Root window holds capture and routes mouse to the deepest submenu under cursor.
			if (!m_isSubMenu && GetCapture() == hWnd)
			{
				RouteCapturedMouseButton(msg, wParam, lParam);
				return 0;
			}

			// Fallback: if we aren't capturing, process locally.
			POINT pt{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
			RECT rc{};
			GetClientRect(hWnd, &rc);
			if (!PtInRect(&rc, pt))
			{
				Hide();
				return 0;
			}
			if (msg == WM_LBUTTONUP)
			{
				HandleMouse(pt, true);
			}
			return 0;
		}

		case WM_MOUSEMOVE:
		{
			if (!m_isSubMenu && GetCapture() == hWnd)
			{
				RouteCapturedMouseMove();
				return 0;
			}

			if (!m_trackingMouse)
			{
				TRACKMOUSEEVENT tme{ sizeof(tme) };
				tme.dwFlags = TME_LEAVE;
				tme.hwndTrack = hWnd;
				TrackMouseEvent(&tme);
				m_trackingMouse = true;
			}

			POINT pt{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
			HandleMouse(pt, false);
			return 0;
		}

		case WM_MOUSELEAVE:
		{
			m_trackingMouse = false;

			// サブメニューが開いていない場合のみハイライト解除
			if (!m_subMenu || !m_subMenu->m_visible)
			{
				if (m_hoveredIndex != -1)
				{
					m_hoveredIndex = -1;
					InvalidateRect(hWnd, nullptr, FALSE);
				}
			}
			return 0;
		}

		case WM_MOUSEWHEEL:
		{
			if (!m_isSubMenu && GetCapture() == hWnd)
			{
				RouteCapturedMouseWheel(wParam, lParam);
				return 0;
			}

			const int delta = GET_WHEEL_DELTA_WPARAM(wParam);
			OnMouseWheelDelta(delta);
			return 0;
		}

		case WM_KEYDOWN:
		{
			if (wParam == VK_ESCAPE)
			{
				Hide();
				return 0;
			}
			break;
		}

		case WM_ERASEBKGND:
			return 1;

		case WM_PAINT:
		{
			PAINTSTRUCT ps{};
			HDC hdc = BeginPaint(hWnd, &ps);
			Paint(hdc);
			EndPaint(hWnd, &ps);
			return 0;
		}
	}

	return DefWindowProcW(hWnd, msg, wParam, lParam);
}

void TrayMenuWindow::Paint(HDC hdc)
{
	if (!m_hWnd) return;

	RECT rc{};
	GetClientRect(m_hWnd, &rc);
	const int width = rc.right - rc.left;
	const int height = rc.bottom - rc.top;
	if (width <= 0 || height <= 0) return;

	HDC memDC = CreateCompatibleDC(hdc);
	HBITMAP memBmp = CreateCompatibleBitmap(hdc, width, height);
	if (!memDC || !memBmp)
	{
		if (memBmp) DeleteObject(memBmp);
		if (memDC) DeleteDC(memDC);
		DrawContents(hdc);
		return;
	}

	HGDIOBJ oldBmp = SelectObject(memDC, memBmp);
	SetBkMode(memDC, TRANSPARENT);

	DrawContents(memDC);

	BitBlt(hdc, 0, 0, width, height, memDC, 0, 0, SRCCOPY);

	SelectObject(memDC, oldBmp);
	DeleteObject(memBmp);
	DeleteDC(memDC);
}

void TrayMenuWindow::DrawContents(HDC hdc)
{
	const auto& theme = m_theme;
	RECT rc{};
	GetClientRect(m_hWnd, &rc);

	HBRUSH bg = CreateSolidBrush(theme.background);
	FillRect(hdc, &rc, bg);
	DeleteObject(bg);

	SetBkMode(hdc, TRANSPARENT);

	// 1. ヘッダーを描画 (サブメニューなら高さ0なので描画されない)
	if (m_headerHeight > 0)
	{
		DrawHeader(hdc, 0);
	}

	// クリップ領域設定
	RECT clipRc = rc;
	clipRc.top = m_headerHeight;
	HRGN hRgn = CreateRectRgnIndirect(&clipRc);
	SelectClipRgn(hdc, hRgn);

	// 2. アイテムを描画
	for (size_t i = 0; i < m_layout.size(); ++i)
	{
		const auto& item = m_layout[i];
		RECT shifted = item.bounds;
		OffsetRect(&shifted, 0, -m_scrollOffset);

		// 画面外ならスキップ
		if (shifted.bottom < m_headerHeight || shifted.top > rc.bottom) continue;

		DrawItem(hdc, item, -m_scrollOffset);
	}

	SelectClipRgn(hdc, nullptr);
	DeleteObject(hRgn);

	if (m_maxScroll > 0)
	{
		DrawScrollbar(hdc);
	}
}

void TrayMenuWindow::DrawHeader(HDC hdc, int offsetY) const
{
	const auto& theme = m_theme;
	RECT header{ 0, offsetY, m_windowSize.cx, m_headerHeight + offsetY };
	HBRUSH hb = CreateSolidBrush(theme.headerBackground);
	FillRect(hdc, &header, hb);
	DeleteObject(hb);

	const int padding = Scale(16);

	SIZE titleSize;
	HGDIOBJ old = SelectObject(hdc, m_titleFont);
	GetTextExtentPoint32W(hdc, m_model.title.c_str(), static_cast<int>(m_model.title.size()), &titleSize);

	RECT titleRc{ padding, padding + offsetY, m_windowSize.cx - padding, padding + titleSize.cy + offsetY };

	SetTextColor(hdc, theme.textPrimary);
	DrawTextW(hdc, m_model.title.c_str(), static_cast<int>(m_model.title.size()), &titleRc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

	if (!m_model.subtitle.empty())
	{
		SetTextColor(hdc, theme.textMuted);
		SelectObject(hdc, m_bodyFont);

		SIZE subSize;
		GetTextExtentPoint32W(hdc, m_model.subtitle.c_str(), static_cast<int>(m_model.subtitle.size()), &subSize);
		RECT subtitleRc{ padding, titleRc.bottom + Scale(4), m_windowSize.cx - padding, titleRc.bottom + Scale(4) + subSize.cy };

		DrawTextW(hdc, m_model.subtitle.c_str(), static_cast<int>(m_model.subtitle.size()), &subtitleRc, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);
	}
	SelectObject(hdc, old);
}

void TrayMenuWindow::DrawItem(HDC hdc, const LayoutItem& item, int offsetY) const
{
	const auto& theme = m_theme;
	RECT rc = item.bounds;
	OffsetRect(&rc, 0, offsetY);

	if (item.data.kind == TrayMenuItem::Kind::Separator)
	{
		HPEN pen = CreatePen(PS_SOLID, 1, theme.outline);
		HPEN oldPen = static_cast<HPEN>(SelectObject(hdc, pen));
		MoveToEx(hdc, rc.left, rc.top, nullptr);
		LineTo(hdc, rc.right, rc.top);
		SelectObject(hdc, oldPen);
		DeleteObject(pen);
		return;
	}

	// GDI+ の準備 (アンチエイリアス描画のため共通化)
	Gdiplus::Graphics g(hdc);
	g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
	g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);

	if (item.data.kind == TrayMenuItem::Kind::Header)
	{
		HGDIOBJ oldFont = SelectObject(hdc, m_headerFont);
		SetTextColor(hdc, theme.textMuted);

		const LayoutItem* ptr = &item;
		if (static_cast<int>(ptr - m_layout.data()) == m_hoveredIndex)
		{
			SetTextColor(hdc, theme.textPrimary);
		}

		RECT textRc = rc;
		textRc.right -= Scale(20);
		DrawTextW(hdc, item.data.title.c_str(), static_cast<int>(item.data.title.size()), &textRc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

		bool isCollapsed = m_collapsedHeaders.count(item.data.title) > 0;

		int cy = (rc.top + rc.bottom) / 2;
		int cx = rc.right - Scale(10);
		float sz = static_cast<float>(Scale(3)); // サイズ調整
		float fx = static_cast<float>(cx);
		float fy = static_cast<float>(cy);

		Gdiplus::Pen chevPen(ToColor(theme.textMuted), 1.5f); // 少し太くする

		if (isCollapsed)
		{
			// 下向き (v)
			g.DrawLine(&chevPen, fx - sz, fy - sz * 0.5f, fx, fy + sz * 0.5f);
			g.DrawLine(&chevPen, fx, fy + sz * 0.5f, fx + sz, fy - sz * 0.5f);
		}
		else
		{
			// 上向き (^)
			g.DrawLine(&chevPen, fx - sz, fy + sz * 0.5f, fx, fy - sz * 0.5f);
			g.DrawLine(&chevPen, fx, fy - sz * 0.5f, fx + sz, fy + sz * 0.5f);
		}

		SelectObject(hdc, oldFont);
		return;
	}

	const LayoutItem* ptr = &item;
	const bool hovered = static_cast<int>(ptr - m_layout.data()) == m_hoveredIndex;
	const bool subMenuOpen = (m_openSubMenuIndex != -1 && static_cast<int>(ptr - m_layout.data()) == m_openSubMenuIndex);

	if (hovered || subMenuOpen)
	{
		HBRUSH hover = CreateSolidBrush(theme.cardHover);
		FillRect(hdc, &rc, hover);
		DeleteObject(hover);
	}

	RECT textRc = rc;
	const int padding = Scale(16);
	textRc.left += padding;
	textRc.right -= padding;

	HGDIOBJ oldFont = SelectObject(hdc, m_bodyFont);
	SetTextColor(hdc, item.data.destructive ? theme.danger : theme.textPrimary);
	// サブタイトル無しの行は縦中央にして、右側の矢印/チェック/トグルと視覚的に揃える
	const UINT titleFmt = item.data.subtitle.empty()
		? (DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS)
		: (DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);
	DrawTextW(hdc, item.data.title.c_str(), static_cast<int>(item.data.title.size()), &textRc, titleFmt);

	if (!item.data.subtitle.empty())
	{
		RECT subRc = textRc;
		subRc.top += Scale(22);
		SetTextColor(hdc, theme.textMuted);
		DrawTextW(hdc, item.data.subtitle.c_str(), static_cast<int>(item.data.subtitle.size()), &subRc, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
	}

	if (!item.data.children.empty())
	{
		int cy = (rc.top + rc.bottom) / 2;
		int cx = rc.right - Scale(10);
		float sz = static_cast<float>(Scale(3));
		float fx = static_cast<float>(cx);
		float fy = static_cast<float>(cy);

		Gdiplus::Pen arrowPen(ToColor(theme.textMuted), 1.5f);

		// ">" の形状
		g.DrawLine(&arrowPen, fx - sz, fy - sz, fx + sz * 0.5f, fy);
		g.DrawLine(&arrowPen, fx + sz * 0.5f, fy, fx - sz, fy + sz);
	}

	if (item.data.kind == TrayMenuItem::Kind::Action && item.data.toggled)
	{
		// チェックマークの位置（右端、Toggleスイッチと同じあたり）
		int toggleWidth = Scale(46); // スイッチの幅分確保したスペースを利用
		int marginRight = Scale(18);

		float cx = static_cast<float>(rc.right - marginRight - toggleWidth / 2);
		float cy = static_cast<float>((rc.top + rc.bottom) / 2);
		float sz = static_cast<float>(Scale(5));

		Gdiplus::Pen checkPen(ToColor(theme.accent), 2.0f);

		// チェックマーク形状 (レ点)
		g.DrawLine(&checkPen, cx - sz, cy, cx - sz * 0.3f, cy + sz);
		g.DrawLine(&checkPen, cx - sz * 0.3f, cy + sz, cx + sz, cy - sz * 0.8f);
	}
	else if (item.data.kind == TrayMenuItem::Kind::Toggle)
	{
		const int toggleWidth = Scale(46);
		const int toggleHeight = Scale(24);
		const int marginRight = Scale(18);
		RECT pill{
			rc.right - marginRight - toggleWidth,
			rc.top + (rc.bottom - rc.top - toggleHeight) / 2,
			rc.right - marginRight,
			rc.top + (rc.bottom - rc.top - toggleHeight) / 2 + toggleHeight
		};

		DrawToggleSwitch(hdc, pill, item.data.toggled);
	}

	SelectObject(hdc, oldFont);
}

void TrayMenuWindow::DrawScrollbar(HDC hdc) const
{
	const auto& theme = m_theme;
	const int trackWidth = Scale(6);
	RECT rc{};
	GetClientRect(m_hWnd, &rc);

	// サブメニューはヘッダーが無いので上端から
	const int topOffset = m_headerHeight > 0 ? m_headerHeight + Scale(12) : Scale(4);
	const int trackLeft = rc.right - Scale(10);
	RECT track{ trackLeft, topOffset, trackLeft + trackWidth, rc.bottom - Scale(12) };

	if (track.bottom <= track.top) return;
	HBRUSH trackBrush = CreateSolidBrush(theme.outline);
	FillRect(hdc, &track, trackBrush);
	DeleteObject(trackBrush);

	const int contentHeight = std::max(0, m_totalHeight - m_headerHeight);
	const int viewportHeight = std::max(1l, m_windowSize.cy - m_headerHeight);
	const double ratio = contentHeight > 0
		? static_cast<double>(viewportHeight) / static_cast<double>(contentHeight)
		: 1.0;
	const int thumbHeight = std::max(Scale(30), static_cast<int>(ratio * (track.bottom - track.top)));
	const double scrollRatio = static_cast<double>(m_scrollOffset) / std::max(1, m_maxScroll);
	const int thumbTop = track.top + static_cast<int>((track.bottom - track.top - thumbHeight) * scrollRatio);

	RECT thumb{ track.left, thumbTop, track.right, thumbTop + thumbHeight };
	HBRUSH thumbBrush = CreateSolidBrush(theme.cardHover);
	FillRect(hdc, &thumb, thumbBrush);
	DeleteObject(thumbBrush);
}

void TrayMenuWindow::DrawToggleSwitch(HDC hdc, const RECT& pill, bool toggled) const
{
	const auto& theme = m_theme;

	Gdiplus::Graphics g(hdc);
	g.SetSmoothingMode(Gdiplus::SmoothingModeHighQuality);
	g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
	g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);

	const float radius = static_cast<float>(Scale(10));
	Gdiplus::RectF pillRect(
		static_cast<Gdiplus::REAL>(pill.left),
		static_cast<Gdiplus::REAL>(pill.top),
		static_cast<Gdiplus::REAL>(pill.right - pill.left),
		static_cast<Gdiplus::REAL>(pill.bottom - pill.top));

	Gdiplus::GraphicsPath path;
	AddRoundedRect(path, pillRect, radius);

	const COLORREF baseColor = toggled ? theme.accent : theme.outline;
	Gdiplus::SolidBrush fill(ToColor(baseColor));
	Gdiplus::Pen outline(ToColor(baseColor), 1.0f);
	g.FillPath(&fill, &path);
	g.DrawPath(&outline, &path);

	const int knobMargin = Scale(4);
	const float knobDiameter = static_cast<float>((pill.bottom - pill.top) - knobMargin * 2);
	const float knobX = toggled
		? static_cast<float>(pill.right - knobMargin) - knobDiameter
		: static_cast<float>(pill.left + knobMargin);
	const float knobY = static_cast<float>(pill.top + knobMargin);

	Gdiplus::RectF knobRect(knobX, knobY, knobDiameter, knobDiameter);

	Gdiplus::LinearGradientBrush knobBrush(
		knobRect,
		Gdiplus::Color(255, 255, 255, 255),
		Gdiplus::Color(255, 238, 238, 238),
		Gdiplus::LinearGradientModeVertical);
	Gdiplus::Pen knobPen(ToColor(Rgb(220, 220, 220)), 1.0f);

	g.FillEllipse(&knobBrush, knobRect);
	g.DrawEllipse(&knobPen, knobRect);

	// subtle inner glow to keep the switch crisp in dark backgrounds
	Gdiplus::Pen lightBorder(ToColor(Rgb(255, 255, 255), 64), 1.0f);
	const float inset = 0.6f;
	Gdiplus::RectF innerRect(
		knobRect.X + inset,
		knobRect.Y + inset,
		knobRect.Width - inset * 2.0f,
		knobRect.Height - inset * 2.0f);
	g.DrawEllipse(&lightBorder, innerRect);
}

void TrayMenuWindow::HandleMouse(POINT pt, bool activate)
{
	int index = -1;
	for (size_t i = 0; i < m_layout.size(); ++i)
	{
		RECT rc = m_layout[i].bounds;
		OffsetRect(&rc, 0, -m_scrollOffset);
		if (PtInRect(&rc, pt))
		{
			index = static_cast<int>(i);

			if (!activate && index >= 0 && m_subMenu && m_subMenu->m_visible && m_openSubMenuIndex != -1)
			{
				if (index != m_openSubMenuIndex)
				{
					CloseSubMenu();
				}
			}

			// --- ホバー時のサブメニュー制御 ---
			if (!activate) // MouseMove
			{
				// 違うアイテムに乗った & そのアイテムが子を持つならサブメニューを開く
				if (index != m_openSubMenuIndex)
				{
					if (!m_layout[i].data.children.empty())
					{
						OpenSubMenu(index);
					}
					// ここでのArmPendingSubMenuCloseは削除し、ループ外で統一的に処理する
				}
			}

			// --- クリック時の処理 ---
			if (activate)
			{
				if (m_layout[i].data.kind == TrayMenuItem::Kind::Header)
				{
					const auto& title = m_layout[i].data.title;
					if (m_collapsedHeaders.count(title))
						m_collapsedHeaders.erase(title);
					else
						m_collapsedHeaders.insert(title);

					RebuildLayout();
					UpdateTopLevelPlacement();
					InvalidateRect(m_hWnd, nullptr, FALSE);
					return;
				}
				else if (m_layout[i].data.kind != TrayMenuItem::Kind::Separator)
				{
					if (m_layout[i].data.children.empty())
					{
						HandleCommand(m_layout[i].data);
					}
				}
			}
			break;
		}
	}

	if (!activate)
	{
		// サブメニューを開いている親アイテムの上にいない場合、すべて閉じる対象とする
		if (index != m_openSubMenuIndex)
		{
			// 余白(index == -1) または 別のアイテム の場合
			ArmPendingSubMenuClose(index, 300);
		}
		else
		{
			// サブメニューを開いている親アイテム上にいるなら閉じない
			CancelPendingSubMenuClose();
		}
	}

	if (index != m_hoveredIndex)
	{
		m_hoveredIndex = index;
		InvalidateRect(m_hWnd, nullptr, FALSE);
	}
}

void TrayMenuWindow::HandleCommand(const TrayMenuItem& item)
{
	if (item.commandId == 0) return;

	TrayMenuWindow* root = this;
	while (root->m_parentWindow)
	{
		root = root->m_parentWindow;
	}
	root->Hide();

	if (m_onCommand)
	{
		m_onCommand(item.commandId);
	}
}

int TrayMenuWindow::Scale(int value) const
{
	const int dpi = (m_dpi > 0) ? static_cast<int>(m_dpi) : 96;
	const int scaledDpi = std::max(1, MulDiv(dpi, m_resolutionScalePct, 100));
	return MulDiv(value, scaledDpi, 96);
}

POINT TrayMenuWindow::AdjustAnchorToWorkArea(const SIZE& size, POINT anchor) const
{
	HMONITOR monitor = MonitorFromPoint(anchor, MONITOR_DEFAULTTONEAREST);
	MONITORINFO info{ sizeof(MONITORINFO) };
	GetMonitorInfoW(monitor, &info);

	int x = anchor.x - size.cx + Scale(12);
	int y = anchor.y - size.cy - Scale(12);

	if (x < info.rcWork.left) x = info.rcWork.left + Scale(8);
	if (y < info.rcWork.top) y = anchor.y + Scale(12);

	if (x + size.cx > info.rcWork.right) x = info.rcWork.right - size.cx - Scale(8);
	if (y + size.cy > info.rcWork.bottom) y = info.rcWork.bottom - size.cy - Scale(8);

	return POINT{ x, y };
}

void TrayMenuWindow::OnMouseWheelDelta(int wheelDelta)
{
	if (!m_hWnd) return;
	if (m_maxScroll <= 0) return;

	const int step = Scale(40);
	m_scrollOffset -= (wheelDelta / WHEEL_DELTA) * step;
	if (m_scrollOffset < 0) m_scrollOffset = 0;
	if (m_scrollOffset > m_maxScroll) m_scrollOffset = m_maxScroll;
	InvalidateRect(m_hWnd, nullptr, FALSE);
}

TrayMenuWindow* TrayMenuWindow::GetRootWindow()
{
	TrayMenuWindow* w = this;
	while (w && w->m_parentWindow) w = w->m_parentWindow;
	return w ? w : this;
}

bool TrayMenuWindow::ContainsScreenPoint(POINT screenPt) const
{
	if (!m_hWnd || !m_visible) return false;
	RECT wr{};
	GetWindowRect(m_hWnd, &wr);
	return PtInRect(&wr, screenPt) != FALSE;
}

TrayMenuWindow* TrayMenuWindow::HitTestDeepestWindow(POINT screenPt)
{
	if (m_subMenu && m_subMenu->m_visible)
	{
		if (TrayMenuWindow* deeper = m_subMenu->HitTestDeepestWindow(screenPt))
			return deeper;
	}

	if (ContainsScreenPoint(screenPt)) return this;

	return nullptr;
}

void TrayMenuWindow::RouteCapturedMouseMove()
{
	TrayMenuWindow* root = GetRootWindow();
	if (!root || !root->m_hWnd || !root->m_visible) return;

	POINT spt{};
	GetCursorPos(&spt);
	TrayMenuWindow* target = root->HitTestDeepestWindow(spt);
	if (!target)
	{
		// メニュー外: ホバー解除（サブメニューは即座には閉じない）
		if (root->m_hoveredIndex != -1)
		{
			root->m_hoveredIndex = -1;
			InvalidateRect(root->m_hWnd, nullptr, FALSE);
		}
		return;
	}

	// サブメニュー上にいる間は、親側の遅延クローズをキャンセルする
	for (TrayMenuWindow* w = target; w; w = w->m_parentWindow)
	{
		w->CancelPendingSubMenuClose();
	}

	POINT cpt = spt;
	ScreenToClient(target->m_hWnd, &cpt);
	target->HandleMouse(cpt, false);
}

void TrayMenuWindow::RouteCapturedMouseButton(UINT msg, WPARAM wParam, LPARAM lParam)
{
	TrayMenuWindow* root = GetRootWindow();
	if (!root || !root->m_hWnd || !root->m_visible) return;

	POINT cptRoot{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
	POINT spt = cptRoot;
	ClientToScreen(root->m_hWnd, &spt);

	TrayMenuWindow* target = root->HitTestDeepestWindow(spt);
	if (!target)
	{
		// メニュー外クリックで閉じる
		if (msg == WM_LBUTTONDOWN || msg == WM_RBUTTONDOWN || msg == WM_MBUTTONDOWN)
		{
			root->Hide();
		}
		return;
	}

	POINT cpt = spt;
	ScreenToClient(target->m_hWnd, &cpt);

	if (msg == WM_LBUTTONUP)
	{
		target->HandleMouse(cpt, true);
	}
	(void)wParam;
}

void TrayMenuWindow::RouteCapturedMouseWheel(WPARAM wParam, LPARAM lParam)
{
	TrayMenuWindow* root = GetRootWindow();
	if (!root || !root->m_hWnd || !root->m_visible) return;

	POINT spt{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
	TrayMenuWindow* target = root->HitTestDeepestWindow(spt);
	if (!target) target = root;

	const int delta = GET_WHEEL_DELTA_WPARAM(wParam);
	target->OnMouseWheelDelta(delta);
}

void TrayMenuWindow::CancelPendingSubMenuClose()
{
	if (!m_hWnd) return;
	if (m_subMenuCloseTimerArmed)
	{
		KillTimer(m_hWnd, kTimerSubMenuClose);
		m_subMenuCloseTimerArmed = false;
	}
}

void TrayMenuWindow::ArmPendingSubMenuClose(int hoveredIndex, UINT delayMs)
{
	if (!m_hWnd) return;
	if (!m_subMenu || !m_subMenu->m_visible) return;
	if (hoveredIndex == m_openSubMenuIndex) return;

	KillTimer(m_hWnd, kTimerSubMenuClose);
	m_subMenuCloseTimerArmed = true;
	SetTimer(m_hWnd, kTimerSubMenuClose, delayMs, nullptr);
}
