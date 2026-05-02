#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "SettingsWindow.hpp"
#include "App.hpp"
#include "Settings.hpp"
#include "Win32UiUtil.hpp"
#include <commctrl.h>
#include <commdlg.h>
#include <shlobj.h>
#include <sstream>
#include <iomanip>
#include <initializer_list>
#include <iterator>
#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <windowsx.h>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "comdlg32.lib")

namespace
{
	constexpr wchar_t kClassName[] = L"MMDDesk.SettingsWindow";
	constexpr wchar_t kContentClassName[] = L"MMDDesk.SettingsContent";
	constexpr wchar_t kSegTabsClassName[] = L"MMDDesk.SettingsSegTabs";
	constexpr COLORREF kDarkBkColor = RGB(32, 32, 32);
	constexpr COLORREF kTextColor = RGB(240, 240, 240);

	constexpr int ID_NAV_TABS = 90;
	constexpr UINT WM_APP_NAV_CHANGED = WM_APP + 0x4D;

	constexpr int ID_MODEL_PATH = 101;
	constexpr int ID_BROWSE = 102;
	constexpr int ID_TOPMOST = 103;
	constexpr int ID_FPS_EDIT = 104;
	constexpr int ID_FPS_SPIN = 1041;
	constexpr int ID_FPS_UNLIMITED = 1042;
	constexpr int ID_FPS_LIMIT_TO_REFRESH = 1043;
	constexpr int ID_FPS_VSYNC = 1044;
	constexpr int ID_PRESET_MODE_COMBO = 1045;

	constexpr int ID_SCALE_SLIDER = 105;
	constexpr int ID_BRIGHTNESS_SLIDER = 110;
	constexpr int ID_AMBIENT_SLIDER = 111;
	constexpr int ID_GLOBAL_SAT_SLIDER = 1111;
	constexpr int ID_KEY_INTENSITY_SLIDER = 112;
	constexpr int ID_FILL_INTENSITY_SLIDER = 113;
	constexpr int ID_KEY_DIR_X_SLIDER = 114;
	constexpr int ID_KEY_DIR_Y_SLIDER = 115;
	constexpr int ID_KEY_DIR_Z_SLIDER = 116;
	constexpr int ID_FILL_DIR_X_SLIDER = 117;
	constexpr int ID_FILL_DIR_Y_SLIDER = 118;
	constexpr int ID_FILL_DIR_Z_SLIDER = 119;

	constexpr int ID_RESET_LIGHT = 120;
	constexpr int ID_SAVE_PRESET = 121;
	constexpr int ID_LOAD_PRESET = 122;

	constexpr int ID_KEY_COLOR_BTN = 130;
	constexpr int ID_FILL_COLOR_BTN = 131;

	constexpr int ID_TOON_ENABLE = 140;
	constexpr int ID_TOON_CONTRAST_SLIDER = 141;
	constexpr int ID_SHADOW_HUE_SLIDER = 142;
	constexpr int ID_SHADOW_SAT_SLIDER = 143;
	constexpr int ID_RIM_WIDTH_SLIDER = 144;
	constexpr int ID_RIM_INTENSITY_SLIDER = 145;
	constexpr int ID_SPECULAR_STEP_SLIDER = 146;
	constexpr int ID_SHADOW_RAMP_SLIDER = 147;
	constexpr int ID_AA_MODE_COMBO = 154;
	constexpr int ID_SELF_SHADOW_ENABLE = 155;
	constexpr int ID_MSAA_SAMPLES_COMBO = 156;
	constexpr int ID_SHADOW_RESOLUTION_COMBO = 157;
	constexpr int ID_OUTLINE_ENABLE = 158;
	constexpr int ID_OUTLINE_STRENGTH_SLIDER = 159;
	constexpr int ID_OUTLINE_OPACITY_SLIDER = 160;

	constexpr int ID_SHADOW_DEEP_THRESH_SLIDER = 148;
	constexpr int ID_SHADOW_DEEP_SOFT_SLIDER = 149;
	constexpr int ID_SHADOW_DEEP_MUL_SLIDER = 150;
	constexpr int ID_FACE_SHADOW_MUL_SLIDER = 151;
	constexpr int ID_FACE_CONTRAST_MUL_SLIDER = 152;
	constexpr int ID_FACE_OVERRIDE_ENABLE = 153;

	constexpr int ID_PHYS_FIXED_TIMESTEP = 300;
	constexpr int ID_PHYS_MAX_SUBSTEPS = 301;
	constexpr int ID_PHYS_WARMUP_STEPS = 349;
	constexpr int ID_PHYS_GRAVITY_X = 303;
	constexpr int ID_PHYS_GRAVITY_Y = 304;
	constexpr int ID_PHYS_GRAVITY_Z = 305;
	constexpr int ID_PHYS_KIN_POS_THRESHOLD = 306;
	constexpr int ID_PHYS_KIN_ROT_THRESHOLD = 307;
	constexpr int ID_PHYS_MIN_VELOCITY_CLIP = 308;
	constexpr int ID_PHYS_JOINT_STOP_ERP = 309;
	constexpr int ID_PHYS_CCD_THRESHOLD_SCALE = 310;
	constexpr int ID_PHYS_SLEEP_LINEAR_THRESHOLD = 311;
	constexpr int ID_PHYS_SLEEP_ANGULAR_THRESHOLD = 312;
	constexpr int ID_PHYS_WRITEBACK_ANGLE_THRESHOLD = 313;

	constexpr int ID_OK = 200;
	constexpr int ID_CANCEL = 201;
	constexpr int ID_APPLY = 202;

	using Win32UiUtil::ControlIdToMenuHandle;

	std::wstring FormatFloat(float val)
	{
		std::wostringstream oss;
		oss << std::fixed << std::setprecision(2) << val;
		return oss.str();
	}

	std::wstring FormatFloatPrec(float val, int precision)
	{
		std::wostringstream oss;
		oss << std::fixed << std::setprecision(precision) << val;
		return oss.str();
	}

	COLORREF FloatToColorRef(float r, float g, float b)
	{
		return RGB(static_cast<BYTE>(r * 255.0f), static_cast<BYTE>(g * 255.0f), static_cast<BYTE>(b * 255.0f));
	}

	int GetEditBoxInt(HWND hEdit, int defaultVal)
	{
		wchar_t buf[32]{};
		GetWindowTextW(hEdit, buf, static_cast<int>(std::size(buf)));
		try
		{
			return std::stoi(buf);
		}
		catch (...)
		{
			return defaultVal;
		}
	}

	float GetEditBoxFloat(HWND hEdit, float defaultVal)
	{
		wchar_t buf[64]{};
		GetWindowTextW(hEdit, buf, static_cast<int>(std::size(buf)));
		try
		{
			return std::stof(buf);
		}
		catch (...)
		{
			return defaultVal;
		}
	}

	void SetCheckState(HWND hwnd, bool checked)
	{
		SendMessageW(hwnd, BM_SETCHECK, checked ? BST_CHECKED : BST_UNCHECKED, 0);
	}

	bool IsChecked(HWND hwnd)
	{
		return SendMessageW(hwnd, BM_GETCHECK, 0, 0) == BST_CHECKED;
	}

	void SetSliderPosition(HWND hwnd, int value)
	{
		SendMessageW(hwnd, TBM_SETPOS, TRUE, value);
	}

	void SetSliderPositionScaled(HWND hwnd, float value, float scale)
	{
		SetSliderPosition(hwnd, static_cast<int>(value * scale));
	}

	void SetSliderPositionOffsetScaled(HWND hwnd, float value, float offset, float scale)
	{
		SetSliderPosition(hwnd, static_cast<int>((value + offset) * scale));
	}

	void SetEditText(HWND hwnd, const std::wstring& text)
	{
		SetWindowTextW(hwnd, text.c_str());
	}

	void SetEditInt(HWND hwnd, int value)
	{
		SetEditText(hwnd, std::to_wstring(value));
	}

	void SetEditFloat(HWND hwnd, float value, int precision)
	{
		SetEditText(hwnd, FormatFloatPrec(value, precision));
	}

	void SetComboSelectionByItemData(HWND hwnd, int value)
	{
		const LRESULT count = SendMessageW(hwnd, CB_GETCOUNT, 0, 0);
		int nearestIndex = -1;
		int nearestDistance = (std::numeric_limits<int>::max)();
		for (int i = 0; i < static_cast<int>(count); ++i)
		{
			const LRESULT data = SendMessageW(hwnd, CB_GETITEMDATA, i, 0);
			if (data == CB_ERR)
			{
				continue;
			}

			const int itemValue = static_cast<int>(data);
			if (itemValue == value)
			{
				SendMessageW(hwnd, CB_SETCURSEL, static_cast<WPARAM>(i), 0);
				return;
			}

			const int distance = std::abs(itemValue - value);
			if (distance < nearestDistance)
			{
				nearestDistance = distance;
				nearestIndex = i;
			}
		}
		if (nearestIndex >= 0)
		{
			SendMessageW(hwnd, CB_SETCURSEL, static_cast<WPARAM>(nearestIndex), 0);
		}
		else if (count > 0)
		{
			SendMessageW(hwnd, CB_SETCURSEL, 0, 0);
		}
	}

	int GetComboSelectionItemData(HWND hwnd, int fallback)
	{
		const int sel = static_cast<int>(SendMessageW(hwnd, CB_GETCURSEL, 0, 0));
		if (sel < 0)
		{
			return fallback;
		}
		const LRESULT data = SendMessageW(hwnd, CB_GETITEMDATA, static_cast<WPARAM>(sel), 0);
		return (data == CB_ERR) ? fallback : static_cast<int>(data);
	}

	int GetVScrollBarWidthForWindow(HWND hwnd)
	{
		UINT dpi = GetDpiForWindow(hwnd);
		if (dpi == 0) dpi = 96;
		return GetSystemMetricsForDpi(SM_CXVSCROLL, dpi);
	}

	struct SettingsFrameLayout
	{
		int outerPadX{ 16 };
		int outerPadY{ 10 };
		int tabHeight{ 32 };
		int headerHeight{};
		int footerTop{};
		int footerY{};
		int buttonWidth{ 92 };
		int buttonHeight{ 30 };
		int buttonGap{ 10 };
		int xOk{};
		int xCancel{};
		int xApply{};
		int contentY{};
		int contentHeight{};
	};

	SettingsFrameLayout ComputeSettingsFrameLayout(int clientW, int clientH, int footerHeight)
	{
		SettingsFrameLayout layout{};
		layout.headerHeight = layout.tabHeight + layout.outerPadY * 2;
		layout.footerTop = std::max(layout.headerHeight, clientH - footerHeight);

		const int footerPad = 16;
		layout.footerY = layout.footerTop + (footerHeight - layout.buttonHeight) / 2;
		layout.xApply = std::max(footerPad, clientW - footerPad - layout.buttonWidth);
		layout.xCancel = std::max(footerPad, layout.xApply - layout.buttonGap - layout.buttonWidth);
		layout.xOk = std::max(footerPad, layout.xCancel - layout.buttonGap - layout.buttonWidth);
		layout.contentY = layout.headerHeight;
		layout.contentHeight = std::max(0, layout.footerTop - layout.contentY);
		return layout;
	}

	void SetControlsEnabled(std::initializer_list<HWND> controls, bool enabled)
	{
		for (HWND control : controls)
		{
			if (control)
			{
				EnableWindow(control, enabled ? TRUE : FALSE);
			}
		}
	}

	constexpr UINT SEGMSG_SETSEL = WM_USER + 0x231;
	constexpr UINT SEGMSG_GETSEL = WM_USER + 0x232;

	constexpr int kSegTabCount = 4;
	const wchar_t* const kSegTabLabels[kSegTabCount] = { L"基本", L"ライト", L"トゥーン", L"物理" };


	constexpr COLORREF kTabsPillFill = RGB(42, 42, 42);
	constexpr COLORREF kTabsPillBorder = RGB(64, 64, 64);
	constexpr COLORREF kTabsHoverFill = RGB(58, 58, 58);
	constexpr COLORREF kTabsPressedFill = RGB(66, 66, 66);
	constexpr COLORREF kTabsSelectedFill = RGB(74, 74, 74);
	constexpr COLORREF kTabsSelectedBorder = RGB(112, 112, 112);
	constexpr COLORREF kTabsText = RGB(230, 230, 230);
	constexpr COLORREF kTabsTextSelected = RGB(255, 255, 255);

	constexpr int kTabsRadius = 10;
	constexpr int kTabsGap = 8;
	constexpr int kTabsPadX = 14;
	constexpr int kTabsPadY = 2;

	struct SegTabsState
	{
		SettingsWindow* owner{};
		int sel{ 0 };
		int hover{ -1 };
		int pressed{ -1 };
		bool tracking{ false };
		HFONT font{};
	};

	struct SegTabsLayout
	{
		RECT pill{};
		RECT seg[kSegTabCount]{};
	};

	SegTabsLayout SegTabsComputeLayout(HWND hwnd, const SegTabsState* st)
	{
		SegTabsLayout lo{};
		RECT rcClient{};
		GetClientRect(hwnd, &rcClient);
		RECT rc = rcClient;
		InflateRect(&rc, -2, -2);
		InflateRect(&rc, -1, -kTabsPadY);

		const int availW = std::max(0l, rc.right - rc.left);
		const int availH = std::max(0l, rc.bottom - rc.top);
		if (availW <= 0 || availH <= 0)
		{
			lo.pill = rc;
			return lo;
		}


		int segW[kSegTabCount]{};
		{
			HDC hdc = GetDC(hwnd);
			HFONT font = st && st->font ? st->font : (HFONT)GetStockObject(DEFAULT_GUI_FONT);
			HGDIOBJ old = SelectObject(hdc, font);
			for (int i = 0; i < kSegTabCount; ++i)
			{
				SIZE sz{};
				GetTextExtentPoint32W(hdc, kSegTabLabels[i], (int)wcslen(kSegTabLabels[i]), &sz);
				segW[i] = std::max(64l, sz.cx + kTabsPadX * 2);
			}
			SelectObject(hdc, old);
			ReleaseDC(hwnd, hdc);
		}

		int totalW = 0;
		for (int i = 0; i < kSegTabCount; ++i) totalW += segW[i];
		totalW += kTabsGap * (kSegTabCount - 1);


		if (totalW > availW)
		{
			const int gap = std::min(kTabsGap, 4);
			const int usable = std::max(0, availW - gap * (kSegTabCount - 1));
			const int each = (kSegTabCount > 0) ? (usable / kSegTabCount) : usable;
			int x = rc.left;
			for (int i = 0; i < kSegTabCount; ++i)
			{
				lo.seg[i] = RECT{ x, rc.top, x + each, rc.bottom };
				x += each + gap;
			}
			lo.pill = rc;
			return lo;
		}

		int x = rc.left + (availW - totalW) / 2;
		for (int i = 0; i < kSegTabCount; ++i)
		{
			lo.seg[i] = RECT{ x, rc.top, x + segW[i], rc.bottom };
			x += segW[i] + kTabsGap;
		}
		lo.pill = RECT{ lo.seg[0].left - 6, rc.top, lo.seg[kSegTabCount - 1].right + 6, rc.bottom };
		if (lo.pill.left < rc.left) lo.pill.left = rc.left;
		if (lo.pill.right > rc.right) lo.pill.right = rc.right;
		return lo;
	}

	int SegTabsHitIndex(HWND hwnd, const SegTabsState* st, const POINT& pt)
	{
		const SegTabsLayout lo = SegTabsComputeLayout(hwnd, st);
		for (int i = 0; i < kSegTabCount; ++i)
		{
			if (PtInRect(&lo.seg[i], pt)) return i;
		}
		return -1;
	}

	void SegTabsNotifySelection(HWND hwnd, SegTabsState* st)
	{
		if (!st) return;
		HWND parent = GetParent(hwnd);
		if (parent) SendMessageW(parent, WM_APP_NAV_CHANGED, static_cast<WPARAM>(st->sel), reinterpret_cast<LPARAM>(hwnd));
	}

	void SegTabsInvalidate(HWND hwnd)
	{
		InvalidateRect(hwnd, nullptr, FALSE);
	}

	LRESULT CALLBACK SegTabsProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
	{
		auto* st = reinterpret_cast<SegTabsState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

		switch (msg)
		{
			case WM_CREATE:
			{
				auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
				auto newState = std::make_unique<SegTabsState>();
				newState->owner = static_cast<SettingsWindow*>(cs->lpCreateParams);
				newState->sel = 0;
				newState->hover = -1;
				newState->pressed = -1;
				newState->tracking = false;
				newState->font = nullptr;
				SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(newState.release()));
				return 0;
			}
			case WM_DESTROY:
			{
				std::unique_ptr<SegTabsState> ownedState(st);
				SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
				return 0;
			}
			case WM_SETFONT:
			{
				if (st) st->font = reinterpret_cast<HFONT>(wParam);
				SegTabsInvalidate(hwnd);
				return 0;
			}
			case WM_GETFONT:
				return st ? reinterpret_cast<LRESULT>(st->font) : 0;

			case SEGMSG_SETSEL:
			{
				if (!st) return 0;
				const int idx = static_cast<int>(wParam);
				st->sel = std::clamp(idx, 0, kSegTabCount - 1);
				SegTabsInvalidate(hwnd);
				return 0;
			}
			case SEGMSG_GETSEL:
				return st ? st->sel : 0;

			case WM_GETDLGCODE:
				return DLGC_WANTARROWS | DLGC_WANTCHARS;

			case WM_KEYDOWN:
			{
				if (!st) break;
				int next = st->sel;
				if (wParam == VK_LEFT) next = std::max(0, st->sel - 1);
				else if (wParam == VK_RIGHT) next = std::min(kSegTabCount - 1, st->sel + 1);
				else break;

				if (next != st->sel)
				{
					st->sel = next;
					SegTabsNotifySelection(hwnd, st);
					SegTabsInvalidate(hwnd);
				}
				return 0;
			}

			case WM_LBUTTONDOWN:
			{
				SetFocus(hwnd);
				if (!st) break;
				POINT pt{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
				const int hit = SegTabsHitIndex(hwnd, st, pt);
				st->pressed = hit;
				SetCapture(hwnd);
				if (hit >= 0 && hit != st->sel)
				{
					st->sel = hit;
					SegTabsNotifySelection(hwnd, st);
				}
				SegTabsInvalidate(hwnd);
				return 0;
			}
			case WM_LBUTTONUP:
			{
				if (!st) break;
				if (GetCapture() == hwnd) ReleaseCapture();
				st->pressed = -1;
				SegTabsInvalidate(hwnd);
				return 0;
			}

			case WM_MOUSEMOVE:
			{
				if (!st) break;
				if (!st->tracking)
				{
					TRACKMOUSEEVENT tme{};
					tme.cbSize = sizeof(tme);
					tme.dwFlags = TME_LEAVE;
					tme.hwndTrack = hwnd;
					TrackMouseEvent(&tme);
					st->tracking = true;
				}

				POINT pt{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
				const int hit = SegTabsHitIndex(hwnd, st, pt);
				if (hit != st->hover)
				{
					st->hover = hit;
					SegTabsInvalidate(hwnd);
				}
				return 0;
			}
			case WM_MOUSELEAVE:
			{
				if (!st) break;
				st->tracking = false;
				st->pressed = -1;
				if (st->hover != -1)
				{
					st->hover = -1;
					SegTabsInvalidate(hwnd);
				}
				return 0;
			}

			case WM_ERASEBKGND:
				return 1;

			case WM_PAINT:
			{
				PAINTSTRUCT ps{};
				HDC hdc = BeginPaint(hwnd, &ps);

				RECT rcClient{};
				GetClientRect(hwnd, &rcClient);

				HDC memDC = CreateCompatibleDC(hdc);
				HBITMAP memBmp = CreateCompatibleBitmap(hdc, std::max(1l, rcClient.right - rcClient.left), std::max(1l, rcClient.bottom - rcClient.top));
				HGDIOBJ oldBmp = SelectObject(memDC, memBmp);


				{
					HBRUSH bg = CreateSolidBrush(kDarkBkColor);
					FillRect(memDC, &rcClient, bg);
					DeleteObject(bg);
				}

				const SegTabsLayout lo = SegTabsComputeLayout(hwnd, st);


				{
					HPEN pen = CreatePen(PS_SOLID, 1, kTabsPillBorder);
					HBRUSH brush = CreateSolidBrush(kTabsPillFill);
					HGDIOBJ oldPen = SelectObject(memDC, pen);
					HGDIOBJ oldBrush = SelectObject(memDC, brush);
					RoundRect(memDC, lo.pill.left, lo.pill.top, lo.pill.right, lo.pill.bottom, kTabsRadius * 2, kTabsRadius * 2);
					SelectObject(memDC, oldBrush);
					SelectObject(memDC, oldPen);
					DeleteObject(brush);
					DeleteObject(pen);
				}


				if (st)
				{
					for (int i = 0; i < kSegTabCount; ++i)
					{
						RECT seg = lo.seg[i];
						InflateRect(&seg, -1, -1);
						if (seg.right <= seg.left) continue;

						const bool isSel = (i == st->sel);
						const bool isHover = (!isSel && i == st->hover);
						const bool isPressed = (i == st->pressed);

						bool draw = isSel || isHover || isPressed;
						if (!draw) continue;

						COLORREF fill = isSel ? kTabsSelectedFill : (isPressed ? kTabsPressedFill : kTabsHoverFill);
						COLORREF border = isSel ? kTabsSelectedBorder : kTabsPillBorder;
						if (isPressed && !isSel) border = kTabsSelectedBorder;

						HPEN pen = CreatePen(PS_SOLID, 1, border);
						HBRUSH brush = CreateSolidBrush(fill);
						HGDIOBJ oldPen = SelectObject(memDC, pen);
						HGDIOBJ oldBrush = SelectObject(memDC, brush);
						RoundRect(memDC, seg.left, seg.top, seg.right, seg.bottom, kTabsRadius * 2, kTabsRadius * 2);
						SelectObject(memDC, oldBrush);
						SelectObject(memDC, oldPen);
						DeleteObject(brush);
						DeleteObject(pen);
					}
				}


				{
					const HFONT font = st && st->font ? st->font : (HFONT)GetStockObject(DEFAULT_GUI_FONT);
					HGDIOBJ oldFont = SelectObject(memDC, font);
					SetBkMode(memDC, TRANSPARENT);

					for (int i = 0; i < kSegTabCount; ++i)
					{
						const bool isSel = st && (i == st->sel);
						SetTextColor(memDC, isSel ? kTabsTextSelected : kTabsText);
						RECT tr = lo.seg[i];
						DrawTextW(memDC, kSegTabLabels[i], -1, &tr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
					}
					SelectObject(memDC, oldFont);
				}


				if (GetFocus() == hwnd)
				{
					RECT fr = lo.pill;
					InflateRect(&fr, -1, -1);
					HPEN pen = CreatePen(PS_SOLID, 1, RGB(120, 120, 120));
					HGDIOBJ oldPen = SelectObject(memDC, pen);
					HGDIOBJ oldBrush = SelectObject(memDC, GetStockObject(NULL_BRUSH));
					RoundRect(memDC, fr.left, fr.top, fr.right, fr.bottom, kTabsRadius * 2, kTabsRadius * 2);
					SelectObject(memDC, oldBrush);
					SelectObject(memDC, oldPen);
					DeleteObject(pen);
				}

				BitBlt(hdc, 0, 0, rcClient.right - rcClient.left, rcClient.bottom - rcClient.top, memDC, 0, 0, SRCCOPY);

				SelectObject(memDC, oldBmp);
				DeleteObject(memBmp);
				DeleteDC(memDC);

				EndPaint(hwnd, &ps);
				return 0;
			}

			default:
				break;
		}

		return DefWindowProcW(hwnd, msg, wParam, lParam);
	}

}

SettingsWindow::SettingsWindow(App& app, HINSTANCE hInst) : m_app(app), m_hInst(hInst)
{
	INITCOMMONCONTROLSEX icex{};
	icex.dwSize = sizeof(icex);
	icex.dwICC = ICC_STANDARD_CLASSES | ICC_BAR_CLASSES;
	InitCommonControlsEx(&icex);
	Win32UiUtil::InitializeDarkModeSupport();

	m_darkBrush = CreateSolidBrush(kDarkBkColor);

	WNDCLASSEXW wc{};
	wc.cbSize = sizeof(wc);
	wc.lpfnWndProc = WndProcThunk;
	wc.hInstance = m_hInst;
	wc.lpszClassName = kClassName;
	wc.hbrBackground = m_darkBrush;
	wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
	wc.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
	Win32UiUtil::RegisterWindowClassOrThrow(wc, "SettingsWindow", true);

	WNDCLASSEXW wcc{};
	wcc.cbSize = sizeof(wcc);
	wcc.lpfnWndProc = ContentProcThunk;
	wcc.hInstance = m_hInst;
	wcc.lpszClassName = kContentClassName;
	wcc.hbrBackground = m_darkBrush;
	wcc.hCursor = LoadCursor(nullptr, IDC_ARROW);
	Win32UiUtil::RegisterWindowClassOrThrow(wcc, "SettingsContent", true);

	WNDCLASSEXW wct{};
	wct.cbSize = sizeof(wct);
	wct.lpfnWndProc = SegTabsProc;
	wct.hInstance = m_hInst;
	wct.lpszClassName = kSegTabsClassName;
	wct.hCursor = LoadCursor(nullptr, IDC_ARROW);
	wct.hbrBackground = m_darkBrush;
	Win32UiUtil::RegisterWindowClassOrThrow(wct, "SettingsSegTabs", true);

	for (int i = 0; i < 16; ++i) m_customColors[i] = RGB(255, 255, 255);

	m_hFont = Win32UiUtil::CreateSegoeUiFont(-12, FW_NORMAL);
	m_hHeaderFont = Win32UiUtil::CreateSegoeUiFont(-13, FW_SEMIBOLD);
}

SettingsWindow::~SettingsWindow()
{
	if (m_hwnd) DestroyWindow(m_hwnd);
	UnregisterClassW(kClassName, m_hInst);
	UnregisterClassW(kContentClassName, m_hInst);
	UnregisterClassW(kSegTabsClassName, m_hInst);
	if (m_hFont) DeleteObject(m_hFont);
	if (m_hHeaderFont) DeleteObject(m_hHeaderFont);
	if (m_darkBrush) DeleteObject(m_darkBrush);
	if (m_tooltip) DestroyWindow(m_tooltip);
}

void SettingsWindow::Show()
{
	if (!m_hwnd)
	{
		const RECT workArea = Win32UiUtil::GetWorkArea();

		const int workW = std::max(1l, workArea.right - workArea.left);
		const int workH = std::max(1l, workArea.bottom - workArea.top);
		const int insetX = std::min(24, std::max(8, workW / 20));
		const int insetY = std::min(32, std::max(8, workH / 20));
		const int maxW = std::max(360, workW - insetX * 2);
		const int maxH = std::max(420, workH - insetY * 2);
		const int windowW = (maxW < 640) ? maxW : 640;
		const int windowH = (maxH < 920) ? maxH : 920;
		const int x = workArea.left + std::max(0, (workW - windowW) / 2);
		const int y = workArea.top + std::max(0, (workH - windowH) / 2);

		m_hwnd = CreateWindowExW(
			WS_EX_DLGMODALFRAME,
			kClassName, L"設定",
			WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_SIZEBOX | WS_CLIPCHILDREN,
			x, y, windowW, windowH,
			nullptr, nullptr, m_hInst, this);
		if (!m_hwnd)
		{
			throw std::runtime_error("CreateWindowExW (SettingsWindow) failed.");
		}


		RECT wr{};
		GetWindowRect(m_hwnd, &wr);
		m_fixedWindowWidth = wr.right - wr.left;
		Win32UiUtil::EnableImmersiveDarkMode(m_hwnd);
	}

	if (!m_created)
	{
		CreateControls();
		m_created = true;
	}

	ScrollTo(0);

	m_backupSettings = m_app.Settings();
	LoadCurrentSettings();
	ShowWindow(m_hwnd, SW_SHOW);
	SetForegroundWindow(m_hwnd);
}

void SettingsWindow::Hide()
{
	if (m_hwnd) ShowWindow(m_hwnd, SW_HIDE);
}

void SettingsWindow::SetModernFont(HWND hChild)
{
	Win32UiUtil::ApplyControlFont(hChild, m_hFont);
}

void SettingsWindow::SetHeaderFont(HWND hChild)
{
	Win32UiUtil::ApplyControlFont(hChild, m_hHeaderFont);
}

void SettingsWindow::SetDarkTheme(HWND hChild)
{
	Win32UiUtil::ApplyDarkControlTheme(hChild);
}

void SettingsWindow::CreateControls()
{

	RECT rcMain{};
	GetClientRect(m_hwnd, &rcMain);
	const int clientW = rcMain.right - rcMain.left;
	const int clientH = rcMain.bottom - rcMain.top;
	const SettingsFrameLayout frame = ComputeSettingsFrameLayout(clientW, clientH, m_footerHeight);
	m_headerHeight = frame.headerHeight;

	m_tabs = CreateWindowExW(
		0,
		kSegTabsClassName,
		nullptr,
		WS_CHILD | WS_VISIBLE | WS_TABSTOP,
		frame.outerPadX,
		frame.outerPadY,
		std::max(0, clientW - frame.outerPadX * 2),
		frame.tabHeight,
		m_hwnd,
		ControlIdToMenuHandle(ID_NAV_TABS),
		m_hInst,
		this);
	SetModernFont(m_tabs);
	SendMessageW(m_tabs, SEGMSG_SETSEL, 0, 0);

	m_footerDivider = CreateWindowExW(0, L"STATIC", nullptr, WS_CHILD | WS_VISIBLE | SS_ETCHEDHORZ,
									  0, frame.footerTop, clientW, 1, m_hwnd, nullptr, m_hInst, nullptr);

	m_okBtn = CreateWindowExW(0, L"BUTTON", L"OK", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
							  frame.xOk, frame.footerY, frame.buttonWidth, frame.buttonHeight, m_hwnd, ControlIdToMenuHandle(ID_OK), m_hInst, nullptr);
	SetModernFont(m_okBtn);
	SetDarkTheme(m_okBtn);

	m_cancelBtn = CreateWindowExW(0, L"BUTTON", L"キャンセル", WS_CHILD | WS_VISIBLE,
								  frame.xCancel, frame.footerY, frame.buttonWidth, frame.buttonHeight, m_hwnd, ControlIdToMenuHandle(ID_CANCEL), m_hInst, nullptr);
	SetModernFont(m_cancelBtn);
	SetDarkTheme(m_cancelBtn);

	m_applyBtn = CreateWindowExW(0, L"BUTTON", L"適用", WS_CHILD | WS_VISIBLE,
								 frame.xApply, frame.footerY, frame.buttonWidth, frame.buttonHeight, m_hwnd, ControlIdToMenuHandle(ID_APPLY), m_hInst, nullptr);
	SetModernFont(m_applyBtn);
	SetDarkTheme(m_applyBtn);


	m_content = CreateWindowExW(
		0,
		kContentClassName,
		nullptr,
		WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_VSCROLL,
		0,
		m_headerHeight,
		clientW,
		frame.contentHeight,
		m_hwnd,
		nullptr,
		m_hInst,
		this);
	Win32UiUtil::ApplyDarkControlTheme(m_content);


	HWND parent = m_content;
	RECT rcContent{};
	GetClientRect(parent, &rcContent);
	const int contentW = rcContent.right - rcContent.left;

	int y = 14;
	const int rowH = 32;
	const int xPadding = 20;
	const int labelW = 170;
	const int valueW = 56;
	const int browseBtnW = 86;
	const int usableW = std::max(0, contentW - xPadding * 2);
	const int editW = std::max(220, usableW - labelW - browseBtnW - 14);
	const int sliderW = std::max(200, usableW - labelW - valueW - 14);
	const int sectionW = std::max(0, usableW);

	auto CreateLabel = [&](const wchar_t* text, int x, int y, int w) {
		HWND h = CreateWindowExW(0, L"STATIC", text, WS_CHILD | WS_VISIBLE | SS_CENTERIMAGE, x, y, w, 24, parent, nullptr, m_hInst, nullptr);
		SetModernFont(h);
		return h;
		};

	auto CreateSectionHeader = [&](const wchar_t* text, int x, int y, int w) {
		HWND h = CreateWindowExW(0, L"STATIC", text, WS_CHILD | WS_VISIBLE | SS_LEFT | SS_CENTERIMAGE, x, y, w, 24, parent, nullptr, m_hInst, nullptr);
		SetHeaderFont(h);
		return h;
		};

	auto CreateDivider = [&](int x, int y, int w) {
		HWND h = CreateWindowExW(0, L"STATIC", nullptr, WS_CHILD | WS_VISIBLE | SS_ETCHEDHORZ, x, y, w, 1, parent, nullptr, m_hInst, nullptr);
		return h;
		};

	auto CreateSlider = [&](int id, int x, int y, int w) {
		HWND h = CreateWindowExW(0, TRACKBAR_CLASSW, L"", WS_CHILD | WS_VISIBLE | TBS_HORZ | TBS_NOTICKS, x, y, w, 24, parent, ControlIdToMenuHandle(id), m_hInst, nullptr);
		SetModernFont(h);
		SetDarkTheme(h);
		return h;
		};

	auto CreateEdit = [&](int id, int x, int y, int w, bool numeric = false) {
		DWORD style = WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL;
		if (numeric) style |= ES_NUMBER;
		HWND h = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", style, x, y, w, 24, parent, ControlIdToMenuHandle(id), m_hInst, nullptr);
		SetModernFont(h);
		SetDarkTheme(h);
		return h;
		};

	auto CreateCheck = [&](int id, const wchar_t* text, int x, int y, int w) {
		HWND h = CreateWindowExW(0, L"BUTTON", text, WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, x, y, w, 24, parent, ControlIdToMenuHandle(id), m_hInst, nullptr);
		SetModernFont(h);
		SetDarkTheme(h);
		return h;
		};

	m_tooltip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_ALWAYSTIP | TTS_BALLOON,
								CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
								m_hwnd, nullptr, m_hInst, nullptr);
	SendMessageW(m_tooltip, TTM_SETMAXTIPWIDTH, 0, 340);
	SendMessageW(m_tooltip, TTM_SETDELAYTIME, TTDT_INITIAL, 400);
	SendMessageW(m_tooltip, TTM_SETDELAYTIME, TTDT_AUTOPOP, 12000);

	auto AddTooltip = [&](HWND target, const wchar_t* text) {
		if (!m_tooltip || !target || !text) return;
		TTTOOLINFOW tti{};
		tti.cbSize = sizeof(tti);
		tti.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
		tti.hwnd = m_hwnd;
		tti.uId = reinterpret_cast<UINT_PTR>(target);
		tti.lpszText = const_cast<LPWSTR>(text);
		SendMessageW(m_tooltip, TTM_ADDTOOL, 0, reinterpret_cast<LPARAM>(&tti));
		};


	m_sectionYBasic = y;
	CreateSectionHeader(L"基本設定", xPadding, y, sectionW);
	CreateDivider(xPadding, y + 24, sectionW);
	y += 30;


	CreateLabel(L"モデルパス:", xPadding, y, labelW);
	m_modelPathEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
									  xPadding + labelW + 6, y, editW, 24, parent, ControlIdToMenuHandle(ID_MODEL_PATH), m_hInst, nullptr);
	SetModernFont(m_modelPathEdit);
	SetDarkTheme(m_modelPathEdit);
	m_browseBtn = CreateWindowExW(0, L"BUTTON", L"参照...", WS_CHILD | WS_VISIBLE,
								  xPadding + labelW + 6 + editW + 8, y, browseBtnW, 24, parent, ControlIdToMenuHandle(ID_BROWSE), m_hInst, nullptr);
	SetModernFont(m_browseBtn);
	SetDarkTheme(m_browseBtn);
	y += rowH + 8;

	m_topmostCheck = CreateCheck(ID_TOPMOST, L"常に最前面に表示", xPadding, y, 220);
	y += rowH + 10;

	CreateLabel(L"最大FPS:", xPadding, y, labelW);
	m_fpsEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_NUMBER,
								xPadding + labelW + 6, y, 76, 24, parent, ControlIdToMenuHandle(ID_FPS_EDIT), m_hInst, nullptr);
	SetModernFont(m_fpsEdit);
	SetDarkTheme(m_fpsEdit);
	m_fpsSpin = CreateWindowExW(0, UPDOWN_CLASSW, nullptr,
								WS_CHILD | WS_VISIBLE | UDS_ARROWKEYS | UDS_SETBUDDYINT | UDS_ALIGNRIGHT,
								xPadding + labelW + 6 + 76, y, 20, 24, parent, ControlIdToMenuHandle(ID_FPS_SPIN), m_hInst, nullptr);
	SendMessageW(m_fpsSpin, UDM_SETRANGE32, 1, 240);
	SendMessageW(m_fpsSpin, UDM_SETBUDDY, reinterpret_cast<WPARAM>(m_fpsEdit), 0);
	m_unlimitedFpsCheck = CreateCheck(ID_FPS_UNLIMITED, L"無制限", xPadding + labelW + 6 + 104, y, 120);
	y += rowH;
	m_limitMonitorRefreshRateCheck = CreateCheck(
		ID_FPS_LIMIT_TO_REFRESH,
		L"モニターのリフレッシュレートで上限",
		xPadding + labelW + 6,
		y,
		280);
	AddTooltip(m_limitMonitorRefreshRateCheck, L"現在表示中モニターのリフレッシュレートを上限FPSとして適用します。");
	y += rowH;
	m_vsyncCheck = CreateCheck(ID_FPS_VSYNC, L"V-Sync", xPadding + labelW + 6, y, 120);
	AddTooltip(m_vsyncCheck, L"有効にすると垂直同期でティアリングを抑えます。");
	y += rowH;

	CreateLabel(L"プリセット読み込み:", xPadding, y, labelW);
	m_presetModeCombo = CreateWindowExW(0, WC_COMBOBOXW, L"", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST,
										xPadding + labelW + 6, y, 240, 200, parent, ControlIdToMenuHandle(ID_PRESET_MODE_COMBO), m_hInst, nullptr);
	SetModernFont(m_presetModeCombo);
	SetDarkTheme(m_presetModeCombo);
	SendMessageW(m_presetModeCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"モデル読み込み時に確認"));
	SendMessageW(m_presetModeCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"常に読み込む"));
	SendMessageW(m_presetModeCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"読み込まない"));
	y += rowH + 20;


	m_sectionYLight = y;
	CreateSectionHeader(L"表示・ライト", xPadding, y, sectionW);
	CreateDivider(xPadding, y + 24, sectionW);
	y += 30;
	CreateLabel(L"モデルサイズ:", xPadding, y, labelW);
	m_scaleSlider = CreateSlider(ID_SCALE_SLIDER, xPadding + labelW, y, sliderW);
	SendMessageW(m_scaleSlider, TBM_SETRANGE, TRUE, MAKELONG(10, 875));
	m_scaleLabel = CreateLabel(L"1.00", xPadding + labelW + sliderW + 10, y, 50);
	y += rowH;

	CreateLabel(L"明るさ:", xPadding, y, labelW);
	m_brightnessSlider = CreateSlider(ID_BRIGHTNESS_SLIDER, xPadding + labelW, y, sliderW);
	SendMessageW(m_brightnessSlider, TBM_SETRANGE, TRUE, MAKELONG(10, 300));
	m_brightnessLabel = CreateLabel(L"1.30", xPadding + labelW + sliderW + 10, y, 50);
	y += rowH;

	CreateLabel(L"環境光:", xPadding, y, labelW);
	m_ambientSlider = CreateSlider(ID_AMBIENT_SLIDER, xPadding + labelW, y, sliderW);
	SendMessageW(m_ambientSlider, TBM_SETRANGE, TRUE, MAKELONG(0, 100));
	m_ambientLabel = CreateLabel(L"0.45", xPadding + labelW + sliderW + 10, y, 50);
	y += rowH;

	CreateLabel(L"全体の彩度:", xPadding, y, labelW);
	m_globalSatSlider = CreateSlider(ID_GLOBAL_SAT_SLIDER, xPadding + labelW, y, sliderW);
	SendMessageW(m_globalSatSlider, TBM_SETRANGE, TRUE, MAKELONG(0, 200));
	m_globalSatLabel = CreateLabel(L"1.10", xPadding + labelW + sliderW + 10, y, 50);
	y += rowH;

	CreateLabel(L"主光源強度/色:", xPadding, y, labelW);
	m_keyIntensitySlider = CreateSlider(ID_KEY_INTENSITY_SLIDER, xPadding + labelW, y, sliderW - 80);
	SendMessageW(m_keyIntensitySlider, TBM_SETRANGE, TRUE, MAKELONG(0, 300));
	m_keyIntensityLabel = CreateLabel(L"1.40", xPadding + labelW + (sliderW - 80) + 5, y, 40);
	m_keyColorBtn = CreateWindowExW(0, L"BUTTON", L"色", WS_CHILD | WS_VISIBLE, xPadding + labelW + (sliderW - 80) + 50, y, 40, 24, parent, ControlIdToMenuHandle(ID_KEY_COLOR_BTN), m_hInst, nullptr);
	SetModernFont(m_keyColorBtn); SetDarkTheme(m_keyColorBtn);
	m_keyColorPreview = CreateWindowExW(WS_EX_STATICEDGE, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_OWNERDRAW, xPadding + labelW + (sliderW - 80) + 95, y + 2, 20, 20, parent, nullptr, m_hInst, nullptr);
	y += rowH;

	CreateLabel(L"補助光源強度/色:", xPadding, y, labelW);
	m_fillIntensitySlider = CreateSlider(ID_FILL_INTENSITY_SLIDER, xPadding + labelW, y, sliderW - 80);
	SendMessageW(m_fillIntensitySlider, TBM_SETRANGE, TRUE, MAKELONG(0, 200));
	m_fillIntensityLabel = CreateLabel(L"0.50", xPadding + labelW + (sliderW - 80) + 5, y, 40);
	m_fillColorBtn = CreateWindowExW(0, L"BUTTON", L"色", WS_CHILD | WS_VISIBLE, xPadding + labelW + (sliderW - 80) + 50, y, 40, 24, parent, ControlIdToMenuHandle(ID_FILL_COLOR_BTN), m_hInst, nullptr);
	SetModernFont(m_fillColorBtn); SetDarkTheme(m_fillColorBtn);
	m_fillColorPreview = CreateWindowExW(WS_EX_STATICEDGE, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_OWNERDRAW, xPadding + labelW + (sliderW - 80) + 95, y + 2, 20, 20, parent, nullptr, m_hInst, nullptr);
	y += rowH + 20;


	m_sectionYToon = y;
	CreateSectionHeader(L"トゥーンシェーディング", xPadding, y, sectionW);
	CreateDivider(xPadding, y + 24, sectionW);
	y += 30;
	m_toonEnableCheck = CreateWindowExW(0, L"BUTTON", L"トゥーンシェーディング有効", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, xPadding, y, 220, 24, parent, ControlIdToMenuHandle(ID_TOON_ENABLE), m_hInst, nullptr);
	SetModernFont(m_toonEnableCheck); SetDarkTheme(m_toonEnableCheck);
	y += rowH;

	m_selfShadowCheck = CreateCheck(ID_SELF_SHADOW_ENABLE, L"自己影を有効", xPadding, y, 180);
	AddTooltip(m_selfShadowCheck, L"主光源方向の自己影を使います。前髪や衣装の落ち影が強く出ます。");
	y += rowH;

	m_outlineCheck = CreateCheck(ID_OUTLINE_ENABLE, L"輪郭線を有効", xPadding, y, 180);
	AddTooltip(m_outlineCheck, L"PMX のエッジ描画を有効にします。重い場合はここを切ると効果が出やすいです。");
	y += rowH;

	CreateLabel(L"輪郭線の強さ:", xPadding, y, labelW);
	m_outlineStrengthSlider = CreateSlider(ID_OUTLINE_STRENGTH_SLIDER, xPadding + labelW, y, sliderW);
	SendMessageW(m_outlineStrengthSlider, TBM_SETRANGE, TRUE, MAKELONG(0, 300));
	m_outlineStrengthLabel = CreateLabel(L"1.00", xPadding + labelW + sliderW + 10, y, 50);
	AddTooltip(m_outlineStrengthSlider, L"PMX 輪郭線の太さ倍率です。100% が既定値です。");
	y += rowH;

	CreateLabel(L"輪郭線の濃さ:", xPadding, y, labelW);
	m_outlineOpacitySlider = CreateSlider(ID_OUTLINE_OPACITY_SLIDER, xPadding + labelW, y, sliderW);
	SendMessageW(m_outlineOpacitySlider, TBM_SETRANGE, TRUE, MAKELONG(0, 300));
	m_outlineOpacityLabel = CreateLabel(L"1.00", xPadding + labelW + sliderW + 10, y, 50);
	AddTooltip(m_outlineOpacitySlider, L"PMX 輪郭線の不透明度倍率です。前向き面の薄い輪郭も強められます。");
	y += rowH;

	CreateLabel(L"アンチエイリアス:", xPadding, y, labelW);
	m_aaModeCombo = CreateWindowExW(0, WC_COMBOBOXW, L"", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST,
									xPadding + labelW, y, 220, 200, parent, ControlIdToMenuHandle(ID_AA_MODE_COMBO), m_hInst, nullptr);
	SetModernFont(m_aaModeCombo);
	SetDarkTheme(m_aaModeCombo);
	SendMessageW(m_aaModeCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"オフ"));
	SendMessageW(m_aaModeCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"FXAA"));
	SendMessageW(m_aaModeCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"MSAA"));
	SendMessageW(m_aaModeCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"MSAA + FXAA"));
	AddTooltip(m_aaModeCombo, L"輪郭の滑らかさ。MSAA は cutout 境界にも効き、FXAA は最終画像を整えます。");
	y += rowH;

	CreateLabel(L"MSAA サンプル数:", xPadding, y, labelW);
	m_msaaSamplesCombo = CreateWindowExW(0, WC_COMBOBOXW, L"", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST,
										 xPadding + labelW, y, 220, 200, parent, ControlIdToMenuHandle(ID_MSAA_SAMPLES_COMBO), m_hInst, nullptr);
	SetModernFont(m_msaaSamplesCombo);
	SetDarkTheme(m_msaaSamplesCombo);
	{
		const UINT maxMsaa = m_app.GetMaximumSupportedMsaaSampleCount();
		const int noneIndex = static_cast<int>(SendMessageW(m_msaaSamplesCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"なし")));
		SendMessageW(m_msaaSamplesCombo, CB_SETITEMDATA, static_cast<WPARAM>(noneIndex), 1);
		for (UINT samples = 2; samples <= maxMsaa; samples *= 2)
		{
			std::wstring label = std::to_wstring(samples) + L"x";
			const int index = static_cast<int>(SendMessageW(m_msaaSamplesCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str())));
			SendMessageW(m_msaaSamplesCombo, CB_SETITEMDATA, static_cast<WPARAM>(index), samples);
		}
	}
	AddTooltip(m_msaaSamplesCombo, L"MSAA を使う場合のサンプル数です。大きいほど重くなります。");
	y += rowH;

	CreateLabel(L"影解像度:", xPadding, y, labelW);
	m_shadowResolutionCombo = CreateWindowExW(0, WC_COMBOBOXW, L"", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST,
											  xPadding + labelW, y, 220, 200, parent, ControlIdToMenuHandle(ID_SHADOW_RESOLUTION_COMBO), m_hInst, nullptr);
	SetModernFont(m_shadowResolutionCombo);
	SetDarkTheme(m_shadowResolutionCombo);
	{
		const struct
		{
			const wchar_t* label;
			int size;
		} shadowResolutions[] = {
			{ L"256", 256 },
			{ L"512", 512 },
			{ L"1024", 1024 },
			{ L"2048", 2048 },
		};
		for (const auto& entry : shadowResolutions)
		{
			const int index = static_cast<int>(SendMessageW(m_shadowResolutionCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(entry.label)));
			SendMessageW(m_shadowResolutionCombo, CB_SETITEMDATA, static_cast<WPARAM>(index), entry.size);
		}
	}
	AddTooltip(m_shadowResolutionCombo, L"自己影シャドウマップの解像度です。大きいほど輪郭は安定しますが重くなります。");
	y += rowH;

	CreateLabel(L"コントラスト:", xPadding, y, labelW);
	m_toonContrastSlider = CreateSlider(ID_TOON_CONTRAST_SLIDER, xPadding + labelW, y, sliderW);
	SendMessageW(m_toonContrastSlider, TBM_SETRANGE, TRUE, MAKELONG(50, 250));
	m_toonContrastLabel = CreateLabel(L"", xPadding + labelW + sliderW + 10, y, 50);
	y += rowH;

	CreateLabel(L"影の色味(度):", xPadding, y, labelW);
	m_shadowHueSlider = CreateSlider(ID_SHADOW_HUE_SLIDER, xPadding + labelW, y, sliderW);
	SendMessageW(m_shadowHueSlider, TBM_SETRANGE, TRUE, MAKELONG(0, 90));
	m_shadowHueLabel = CreateLabel(L"", xPadding + labelW + sliderW + 10, y, 50);
	y += rowH;

	CreateLabel(L"影の彩度:", xPadding, y, labelW);
	m_shadowSatSlider = CreateSlider(ID_SHADOW_SAT_SLIDER, xPadding + labelW, y, sliderW);
	SendMessageW(m_shadowSatSlider, TBM_SETRANGE, TRUE, MAKELONG(0, 100));
	m_shadowSatLabel = CreateLabel(L"", xPadding + labelW + sliderW + 10, y, 50);
	y += rowH;

	CreateLabel(L"影の範囲:", xPadding, y, labelW);
	m_shadowRampSlider = CreateSlider(ID_SHADOW_RAMP_SLIDER, xPadding + labelW, y, sliderW);
	SendMessageW(m_shadowRampSlider, TBM_SETRANGE, TRUE, MAKELONG(0, 100));
	m_shadowRampLabel = CreateLabel(L"", xPadding + labelW + sliderW + 10, y, 50);
	y += rowH;


	CreateLabel(L"濃い影の閾値:", xPadding, y, labelW);
	m_shadowDeepThresholdSlider = CreateSlider(ID_SHADOW_DEEP_THRESH_SLIDER, xPadding + labelW, y, sliderW);
	SendMessageW(m_shadowDeepThresholdSlider, TBM_SETRANGE, TRUE, MAKELONG(0, 100));
	m_shadowDeepThresholdLabel = CreateLabel(L"", xPadding + labelW + sliderW + 10, y, 50);
	y += rowH;

	CreateLabel(L"濃い影の境界:", xPadding, y, labelW);
	m_shadowDeepSoftSlider = CreateSlider(ID_SHADOW_DEEP_SOFT_SLIDER, xPadding + labelW, y, sliderW);
	SendMessageW(m_shadowDeepSoftSlider, TBM_SETRANGE, TRUE, MAKELONG(0, 50));
	m_shadowDeepSoftLabel = CreateLabel(L"", xPadding + labelW + sliderW + 10, y, 50);
	y += rowH;

	CreateLabel(L"濃い影の強度:", xPadding, y, labelW);
	m_shadowDeepMulSlider = CreateSlider(ID_SHADOW_DEEP_MUL_SLIDER, xPadding + labelW, y, sliderW);
	SendMessageW(m_shadowDeepMulSlider, TBM_SETRANGE, TRUE, MAKELONG(0, 100));
	m_shadowDeepMulLabel = CreateLabel(L"", xPadding + labelW + sliderW + 10, y, 50);
	y += rowH;

	CreateLabel(L"リム幅/強度:", xPadding, y, labelW);
	m_rimWidthSlider = CreateSlider(ID_RIM_WIDTH_SLIDER, xPadding + labelW, y, sliderW / 2 - 5);
	SendMessageW(m_rimWidthSlider, TBM_SETRANGE, TRUE, MAKELONG(10, 100));
	m_rimIntensitySlider = CreateSlider(ID_RIM_INTENSITY_SLIDER, xPadding + labelW + sliderW / 2 + 5, y, sliderW / 2 - 5);
	SendMessageW(m_rimIntensitySlider, TBM_SETRANGE, TRUE, MAKELONG(0, 100));
	m_rimWidthLabel = CreateLabel(L"", xPadding + labelW + sliderW + 10, y, 50);
	y += rowH;

	CreateLabel(L"スペキュラ:", xPadding, y, labelW);
	m_specularStepSlider = CreateSlider(ID_SPECULAR_STEP_SLIDER, xPadding + labelW, y, sliderW);
	SendMessageW(m_specularStepSlider, TBM_SETRANGE, TRUE, MAKELONG(0, 100));
	m_specularStepLabel = CreateLabel(L"", xPadding + labelW + sliderW + 10, y, 50);
	y += rowH + 20;


	CreateSectionHeader(L"顔マテリアル", xPadding, y, sectionW);
	CreateDivider(xPadding, y + 24, sectionW);
	y += 30;
	m_faceMaterialOverridesCheck = CreateCheck(ID_FACE_OVERRIDE_ENABLE, L"顔マテリアルの別処理を有効", xPadding, y, 260);
	AddTooltip(m_faceMaterialOverridesCheck, L"顔と判定したマテリアルだけ影の濃さとトゥーンコントラストを別設定で処理します。");
	y += rowH;
	CreateLabel(L"顔の影の濃さ:", xPadding, y, labelW);
	m_faceShadowMulSlider = CreateSlider(ID_FACE_SHADOW_MUL_SLIDER, xPadding + labelW, y, sliderW);
	SendMessageW(m_faceShadowMulSlider, TBM_SETRANGE, TRUE, MAKELONG(0, 100));
	m_faceShadowMulLabel = CreateLabel(L"", xPadding + labelW + sliderW + 10, y, 50);
	y += rowH;

	CreateLabel(L"顔のコントラスト:", xPadding, y, labelW);
	m_faceContrastMulSlider = CreateSlider(ID_FACE_CONTRAST_MUL_SLIDER, xPadding + labelW, y, sliderW);
	SendMessageW(m_faceContrastMulSlider, TBM_SETRANGE, TRUE, MAKELONG(0, 200));
	m_faceContrastMulLabel = CreateLabel(L"", xPadding + labelW + sliderW + 10, y, 50);
	y += rowH + 20;

	CreateSectionHeader(L"光源方向", xPadding, y, sectionW);
	CreateDivider(xPadding, y + 24, sectionW);
	y += 30;
	CreateLabel(L"主光源方向 (X/Y/Z):", xPadding, y, 200); y += 24;
	int slider3W = 145;
	m_keyDirXSlider = CreateSlider(ID_KEY_DIR_X_SLIDER, xPadding, y, slider3W);
	SendMessageW(m_keyDirXSlider, TBM_SETRANGE, TRUE, MAKELONG(0, 200));
	m_keyDirYSlider = CreateSlider(ID_KEY_DIR_Y_SLIDER, xPadding + 150, y, slider3W);
	SendMessageW(m_keyDirYSlider, TBM_SETRANGE, TRUE, MAKELONG(0, 200));
	m_keyDirZSlider = CreateSlider(ID_KEY_DIR_Z_SLIDER, xPadding + 300, y, slider3W);
	SendMessageW(m_keyDirZSlider, TBM_SETRANGE, TRUE, MAKELONG(0, 200));
	y += rowH + 5;

	CreateLabel(L"補助光源方向 (X/Y/Z):", xPadding, y, 200); y += 24;
	m_fillDirXSlider = CreateSlider(ID_FILL_DIR_X_SLIDER, xPadding, y, slider3W);
	SendMessageW(m_fillDirXSlider, TBM_SETRANGE, TRUE, MAKELONG(0, 200));
	m_fillDirYSlider = CreateSlider(ID_FILL_DIR_Y_SLIDER, xPadding + 150, y, slider3W);
	SendMessageW(m_fillDirYSlider, TBM_SETRANGE, TRUE, MAKELONG(0, 200));
	m_fillDirZSlider = CreateSlider(ID_FILL_DIR_Z_SLIDER, xPadding + 300, y, slider3W);
	SendMessageW(m_fillDirZSlider, TBM_SETRANGE, TRUE, MAKELONG(0, 200));
	y += rowH + 20;


	const int physicsLabelW = 230;
	const int physicsEditW = 80;

	m_sectionYPhysics = y;
	CreateSectionHeader(L"物理演算", xPadding, y, sectionW);
	CreateDivider(xPadding, y + 24, sectionW);
	y += 30;

	auto label = CreateLabel(L"固定タイムステップ:", xPadding, y, physicsLabelW);
	AddTooltip(label, L"1ステップの時間(秒)。小さいほど精度↑/負荷↑。標準: 0.016前後。");
	m_physicsFixedTimeStepEdit = CreateEdit(ID_PHYS_FIXED_TIMESTEP, xPadding + physicsLabelW, y, physicsEditW);
	AddTooltip(m_physicsFixedTimeStepEdit, L"1ステップの時間(秒)。小さいほど精度↑/負荷↑。標準: 0.016前後。");
	y += rowH;

	label = CreateLabel(L"サブステップ数:", xPadding, y, physicsLabelW);
	AddTooltip(label, L"1フレームを分割する回数。増やすと安定性↑/負荷↑。標準: 1〜4。");
	m_physicsMaxSubStepsEdit = CreateEdit(ID_PHYS_MAX_SUBSTEPS, xPadding + physicsLabelW, y, physicsEditW, true);
	AddTooltip(m_physicsMaxSubStepsEdit, L"1フレームを分割する回数。増やすと安定性↑/負荷↑。soft body ありなら標準: 4〜8。");
	y += rowH;

	label = CreateLabel(L"ウォームアップ:", xPadding, y, physicsLabelW);
	AddTooltip(label, L"モデル読込直後に先行実行するステップ数です。初期姿勢の安定化に使います。");
	m_physicsWarmupStepsEdit = CreateEdit(ID_PHYS_WARMUP_STEPS, xPadding + physicsLabelW, y, physicsEditW, true);
	AddTooltip(m_physicsWarmupStepsEdit, L"モデル読込直後に先行実行するステップ数です。初期姿勢の安定化に使います。");
	y += rowH;

	label = CreateLabel(L"重力 (X/Y/Z):", xPadding, y, physicsLabelW);
	AddTooltip(label, L"重力加速度。Yが下方向。標準: (0, -98, 0)。");
	m_physicsGravityXEdit = CreateEdit(ID_PHYS_GRAVITY_X, xPadding + physicsLabelW, y, physicsEditW);
	m_physicsGravityYEdit = CreateEdit(ID_PHYS_GRAVITY_Y, xPadding + physicsLabelW + physicsEditW + 5, y, physicsEditW);
	m_physicsGravityZEdit = CreateEdit(ID_PHYS_GRAVITY_Z, xPadding + physicsLabelW + (physicsEditW + 5) * 2, y, physicsEditW);
	AddTooltip(m_physicsGravityXEdit, L"重力加速度。Yが下方向。標準: (0, -98, 0)。");
	AddTooltip(m_physicsGravityYEdit, L"重力加速度。Yが下方向。標準: (0, -98, 0)。");
	AddTooltip(m_physicsGravityZEdit, L"重力加速度。Yが下方向。標準: (0, -98, 0)。");
	y += rowH + 20;

	CreateSectionHeader(L"物理演算(高度な設定)", xPadding, y, sectionW);
	CreateDivider(xPadding, y + 24, sectionW);
	y += 30;

	label = CreateLabel(L"位置変化閾値:", xPadding, y, physicsLabelW);
	AddTooltip(label, L"キネマティック剛体の位置変化を無視する閾値(距離の2乗)。小さいほど敏感。標準: 1e-12。");
	m_physicsKinematicPosThresholdEdit = CreateEdit(ID_PHYS_KIN_POS_THRESHOLD, xPadding + physicsLabelW, y, physicsEditW);
	AddTooltip(m_physicsKinematicPosThresholdEdit, L"キネマティック剛体の位置変化を無視する閾値(距離の2乗)。小さいほど敏感。標準: 1e-12。");
	y += rowH;

	label = CreateLabel(L"回転変化閾値:", xPadding, y, physicsLabelW);
	AddTooltip(label, L"キネマティック剛体の回転変化を無視する閾値(1 - dot)。小さいほど敏感。標準: 1e-8。");
	m_physicsKinematicRotThresholdEdit = CreateEdit(ID_PHYS_KIN_ROT_THRESHOLD, xPadding + physicsLabelW, y, physicsEditW);
	AddTooltip(m_physicsKinematicRotThresholdEdit, L"キネマティック剛体の回転変化を無視する閾値(1 - dot)。小さいほど敏感。標準: 1e-8。");
	y += rowH;

	label = CreateLabel(L"最小速度クリップ:", xPadding, y, physicsLabelW);
	AddTooltip(label, L"DynamicAndPositionAdjust への速度注入を無視する最小速度(m/s)。標準: 1e-4。");
	m_physicsMinVelocityClipEdit = CreateEdit(ID_PHYS_MIN_VELOCITY_CLIP, xPadding + physicsLabelW, y, physicsEditW);
	AddTooltip(m_physicsMinVelocityClipEdit, L"DynamicAndPositionAdjust への速度注入を無視する最小速度(m/s)。標準: 1e-4。");
	y += rowH;

	label = CreateLabel(L"ジョイントERP:", xPadding, y, physicsLabelW);
	AddTooltip(label, L"6DOFスプリングジョイントのSTOP_ERP。低いほど振動しにくいが緩くなる。標準: 0.475。");
	m_physicsJointStopErpEdit = CreateEdit(ID_PHYS_JOINT_STOP_ERP, xPadding + physicsLabelW, y, physicsEditW);
	AddTooltip(m_physicsJointStopErpEdit, L"6DOFスプリングジョイントのSTOP_ERP。低いほど振動しにくいが緩くなる。標準: 0.475。");
	y += rowH;

	label = CreateLabel(L"CCD閾値倍率:", xPadding, y, physicsLabelW);
	AddTooltip(label, L"CCD motionThreshold に対する倍率。大きいほど微小変位でCCDが発動しにくい。標準: 1.0。");
	m_physicsCcdThresholdScaleEdit = CreateEdit(ID_PHYS_CCD_THRESHOLD_SCALE, xPadding + physicsLabelW, y, physicsEditW);
	AddTooltip(m_physicsCcdThresholdScaleEdit, L"CCD motionThreshold に対する倍率。大きいほど微小変位でCCDが発動しにくい。標準: 1.0。");
	y += rowH;

	label = CreateLabel(L"Sleep線形閾値:", xPadding, y, physicsLabelW);
	AddTooltip(label, L" Sleeping判定の線形速度閾値(m/s)。標準: 0.1。");
	m_physicsSleepLinearThresholdEdit = CreateEdit(ID_PHYS_SLEEP_LINEAR_THRESHOLD, xPadding + physicsLabelW, y, physicsEditW);
	AddTooltip(m_physicsSleepLinearThresholdEdit, L" Sleeping判定の線形速度閾値(m/s)。標準: 0.1。");
	y += rowH;

	label = CreateLabel(L"Sleep角速度閾値:", xPadding, y, physicsLabelW);
	AddTooltip(label, L" Sleeping判定の角速度閾値(rad/s)。標準: 0.1。");
	m_physicsSleepAngularThresholdEdit = CreateEdit(ID_PHYS_SLEEP_ANGULAR_THRESHOLD, xPadding + physicsLabelW, y, physicsEditW);
	AddTooltip(m_physicsSleepAngularThresholdEdit, L" Sleeping判定の角速度閾値(rad/s)。標準: 0.1。");
	y += rowH;

	label = CreateLabel(L"書戻し角度閾値(度):", xPadding, y, physicsLabelW);
	AddTooltip(label, L"ボーン書き戻し時に無視する最小角度差(度)。0で無効。標準: 0.0。");
	m_physicsWritebackAngleThresholdEdit = CreateEdit(ID_PHYS_WRITEBACK_ANGLE_THRESHOLD, xPadding + physicsLabelW, y, physicsEditW);
	AddTooltip(m_physicsWritebackAngleThresholdEdit, L"ボーン書き戻し時に無視する最小角度差(度)。0で無効。標準: 0.0。");
	y += rowH + 20;

	m_physicsAdvancedStartY = y;
	m_physicsAdvancedEndY = y;
	m_physicsAdvancedHeight = 0;




	{
		const int actionGap = 10;
		const int actionH = 32;

		int resetW = 170;
		int loadW = 170;
		int saveW = sectionW - resetW - loadW - actionGap * 2;


		if (saveW < 200)
		{
			const int each = std::max(0, (sectionW - actionGap * 2) / 3);
			resetW = each;
			loadW = each;
			saveW = sectionW - resetW - loadW - actionGap * 2;
		}

		const int xReset = xPadding;
		const int xLoad = xReset + resetW + actionGap;
		const int xSave = xLoad + loadW + actionGap;
		m_physicsActionsExpandedY = y;

		m_resetLightBtn = CreateWindowExW(0, L"BUTTON", L"ライト設定をリセット", WS_CHILD | WS_VISIBLE,
										  xReset, y, resetW, actionH, parent, ControlIdToMenuHandle(ID_RESET_LIGHT), m_hInst, nullptr);
		SetModernFont(m_resetLightBtn);
		SetDarkTheme(m_resetLightBtn);

		m_loadPresetBtn = CreateWindowExW(0, L"BUTTON", L"プリセットを読み込む", WS_CHILD | WS_VISIBLE,
										  xLoad, y, loadW, actionH, parent, ControlIdToMenuHandle(ID_LOAD_PRESET), m_hInst, nullptr);
		SetModernFont(m_loadPresetBtn);
		SetDarkTheme(m_loadPresetBtn);

		m_savePresetBtn = CreateWindowExW(0, L"BUTTON", L"このモデルの設定を保存", WS_CHILD | WS_VISIBLE,
										  xSave, y, saveW, actionH, parent, ControlIdToMenuHandle(ID_SAVE_PRESET), m_hInst, nullptr);
		SetModernFont(m_savePresetBtn);
		SetDarkTheme(m_savePresetBtn);

		y += actionH + 24;
	}

	m_physicsExpandedContentHeight = y + 18;
	m_physicsCollapsedContentHeight = std::max(0, m_physicsExpandedContentHeight - m_physicsAdvancedHeight);
	m_totalContentHeight = m_physicsExpandedContentHeight;
	UpdateScrollInfo();
}

void SettingsWindow::UpdatePhysicsAdvancedVisibility()
{
	if (!m_content)
	{
		return;
	}

	m_totalContentHeight = m_physicsExpandedContentHeight;
	ScrollTo(m_scrollY);
}

void SettingsWindow::UpdateScrollInfo()
{
	if (!m_content) return;

	RECT rc;
	GetClientRect(m_content, &rc);
	const int clientHeight = rc.bottom - rc.top;

	SCROLLINFO si{};
	si.cbSize = sizeof(si);
	si.fMask = SIF_ALL;
	si.nMin = 0;
	si.nMax = m_totalContentHeight;
	si.nPage = static_cast<UINT>(clientHeight);
	si.nPos = m_scrollY;

	SetScrollInfo(m_content, SB_VERT, &si, TRUE);
}

void SettingsWindow::OnVScroll(WPARAM wParam)
{
	if (!m_content) return;

	const int action = LOWORD(wParam);
	int newY = m_scrollY;

	RECT rc;
	GetClientRect(m_content, &rc);
	const int page = rc.bottom - rc.top;
	const int maxPos = std::max(0, m_totalContentHeight - page);

	switch (action)
	{
		case SB_TOP:      newY = 0; break;
		case SB_BOTTOM:   newY = maxPos; break;
		case SB_LINEUP:   newY -= 40; break;
		case SB_LINEDOWN: newY += 40; break;
		case SB_PAGEUP:   newY -= (page - 40); break;
		case SB_PAGEDOWN: newY += (page - 40); break;
		case SB_THUMBTRACK:
		case SB_THUMBPOSITION:
		{
			SCROLLINFO si{};
			si.cbSize = sizeof(si);
			si.fMask = SIF_TRACKPOS;
			GetScrollInfo(m_content, SB_VERT, &si);
			newY = si.nTrackPos;
			break;
		}
		default: return;
	}

	ScrollTo(newY);
}

void SettingsWindow::OnMouseWheel(int delta)
{
	if (!m_content) return;
	const int scrollAmount = -(delta / WHEEL_DELTA) * 60;
	ScrollTo(m_scrollY + scrollAmount);
}

void SettingsWindow::ScrollTo(int targetY)
{
	if (!m_content) return;

	RECT rc;
	GetClientRect(m_content, &rc);
	const int page = rc.bottom - rc.top;
	const int maxPos = std::max(0, m_totalContentHeight - page);

	int newY = targetY;
	if (newY < 0) newY = 0;
	if (newY > maxPos) newY = maxPos;

	if (newY != m_scrollY)
	{
		ScrollWindowEx(m_content, 0, m_scrollY - newY, nullptr, nullptr, nullptr, nullptr,
					   SW_INVALIDATE | SW_SCROLLCHILDREN | SW_ERASE);
		m_scrollY = newY;
	}
	UpdateScrollInfo();
	UpdateNavHighlightFromScroll();
	UpdateWindow(m_content);
}


void SettingsWindow::UpdateNavHighlightFromScroll()
{
	if (!m_tabs || !m_content) return;



	RECT rc{};
	GetClientRect(m_content, &rc);
	const int page = (rc.bottom - rc.top);
	const int focusOffset = std::min(120, std::max(24, page / 4));
	const int focusY = m_scrollY + focusOffset;

	int idx = 0;
	if (focusY >= m_sectionYPhysics) idx = 3;
	else if (focusY >= m_sectionYToon) idx = 2;
	else if (focusY >= m_sectionYLight) idx = 1;
	else idx = 0;

	if (idx != m_lastAutoNavIndex)
	{
		m_lastAutoNavIndex = idx;
		SendMessageW(m_tabs, SEGMSG_SETSEL, static_cast<WPARAM>(idx), 0);
	}
}

void SettingsWindow::LoadGeneralSettings(const AppSettings& settings)
{
	SetWindowTextW(m_modelPathEdit, settings.modelPath.wstring().c_str());
	SetCheckState(m_topmostCheck, settings.alwaysOnTop);
	SetCheckState(m_unlimitedFpsCheck, settings.unlimitedFps);
	SetCheckState(m_limitMonitorRefreshRateCheck, settings.limitFpsToMonitorRefreshRate);
	SetCheckState(m_vsyncCheck, settings.vsyncEnabled);
	SetEditInt(m_fpsEdit, settings.targetFps);
	SendMessageW(m_fpsSpin, UDM_SETPOS32, 0, settings.targetFps);
	UpdateFpsControlState();
	SendMessageW(m_presetModeCombo, CB_SETCURSEL, static_cast<WPARAM>(settings.globalPresetMode), 0);
}

void SettingsWindow::LoadLightSettings(const LightSettings& light)
{
	LoadLightScalarControls(light);

	const struct
	{
		HWND check;
		bool checked;
	} checkBindings[] = {
		{ m_toonEnableCheck, light.toonEnabled },
		{ m_selfShadowCheck, light.selfShadowEnabled },
		{ m_outlineCheck, light.outlineEnabled },
		{ m_faceMaterialOverridesCheck, light.faceMaterialOverridesEnabled },
	};
	for (const auto& binding : checkBindings)
	{
		SetCheckState(binding.check, binding.checked);
	}

	SendMessageW(m_aaModeCombo, CB_SETCURSEL, static_cast<WPARAM>(std::clamp(light.antiAliasingMode, 0, 3)), 0);
	SetComboSelectionByItemData(m_msaaSamplesCombo, light.msaaSampleCount);
	SetComboSelectionByItemData(m_shadowResolutionCombo, light.shadowMapSize);
	UpdateLightDependentControlState(light);
}

void SettingsWindow::LoadPhysicsSettings(const PhysicsSettings& physics)
{
	SetEditFloat(m_physicsFixedTimeStepEdit, physics.fixedTimeStep, 5);
	SetEditInt(m_physicsMaxSubStepsEdit, physics.maxSubSteps);
	SetEditInt(m_physicsWarmupStepsEdit, physics.warmupSteps);
	SetEditFloat(m_physicsGravityXEdit, physics.gravity.x, 4);
	SetEditFloat(m_physicsGravityYEdit, physics.gravity.y, 4);
	SetEditFloat(m_physicsGravityZEdit, physics.gravity.z, 4);
	SetEditFloat(m_physicsKinematicPosThresholdEdit, physics.kinematicPositionThreshold, 12);
	SetEditFloat(m_physicsKinematicRotThresholdEdit, physics.kinematicRotationThreshold, 12);
	SetEditFloat(m_physicsMinVelocityClipEdit, physics.minKinematicVelocityClip, 6);
	SetEditFloat(m_physicsJointStopErpEdit, physics.jointStopErp, 3);
	SetEditFloat(m_physicsCcdThresholdScaleEdit, physics.ccdThresholdScale, 2);
	SetEditFloat(m_physicsSleepLinearThresholdEdit, physics.sleepLinearThreshold, 3);
	SetEditFloat(m_physicsSleepAngularThresholdEdit, physics.sleepAngularThreshold, 3);
	SetEditFloat(m_physicsWritebackAngleThresholdEdit, physics.writebackAngleThresholdDeg, 2);
}

void SettingsWindow::LoadLightScalarControls(const LightSettings& light)
{
	const struct
	{
		HWND slider;
		float value;
		float scale;
	} scaledSliders[] = {
		{ m_scaleSlider, light.modelScale, 100.0f },
		{ m_brightnessSlider, light.brightness, 100.0f },
		{ m_ambientSlider, light.ambientStrength, 100.0f },
		{ m_globalSatSlider, light.globalSaturation, 100.0f },
		{ m_keyIntensitySlider, light.keyLightIntensity, 100.0f },
		{ m_fillIntensitySlider, light.fillLightIntensity, 100.0f },
		{ m_outlineStrengthSlider, light.outlineWidthScale, 100.0f },
		{ m_outlineOpacitySlider, light.outlineOpacityScale, 100.0f },
		{ m_toonContrastSlider, light.toonContrast, 100.0f },
		{ m_shadowSatSlider, light.shadowSaturationBoost, 100.0f },
		{ m_shadowDeepThresholdSlider, light.shadowDeepThreshold, 100.0f },
		{ m_shadowDeepSoftSlider, light.shadowDeepSoftness, 100.0f },
		{ m_shadowDeepMulSlider, light.shadowDeepMul, 100.0f },
		{ m_faceShadowMulSlider, light.faceShadowMul, 100.0f },
		{ m_faceContrastMulSlider, light.faceToonContrastMul, 100.0f },
		{ m_rimWidthSlider, light.rimWidth, 100.0f },
		{ m_rimIntensitySlider, light.rimIntensity, 100.0f },
		{ m_specularStepSlider, light.specularStep, 100.0f },
	};
	for (const auto& binding : scaledSliders)
	{
		SetSliderPositionScaled(binding.slider, binding.value, binding.scale);
	}

	const struct
	{
		HWND slider;
		float value;
		float offset;
		float scale;
	} offsetSliders[] = {
		{ m_shadowHueSlider, light.shadowHueShiftDeg, 45.0f, 1.0f },
		{ m_keyDirXSlider, light.keyLightDirX, 1.0f, 100.0f },
		{ m_keyDirYSlider, light.keyLightDirY, 1.0f, 100.0f },
		{ m_keyDirZSlider, light.keyLightDirZ, 1.0f, 100.0f },
		{ m_fillDirXSlider, light.fillLightDirX, 1.0f, 100.0f },
		{ m_fillDirYSlider, light.fillLightDirY, 1.0f, 100.0f },
		{ m_fillDirZSlider, light.fillLightDirZ, 1.0f, 100.0f },
	};
	for (const auto& binding : offsetSliders)
	{
		SetSliderPositionOffsetScaled(binding.slider, binding.value, binding.offset, binding.scale);
	}

	SetSliderPosition(m_shadowRampSlider, static_cast<int>((light.shadowRampShift + 0.5f) * 100.0f));
}

void SettingsWindow::ReadLightScalarControls(LightSettings& light) const
{
	auto getSliderValue = [](HWND slider)
	{
		return static_cast<float>(SendMessageW(slider, TBM_GETPOS, 0, 0));
	};

	const struct
	{
		float* target;
		HWND slider;
		float scale;
	} scaledSliders[] = {
		{ &light.modelScale, m_scaleSlider, 100.0f },
		{ &light.brightness, m_brightnessSlider, 100.0f },
		{ &light.ambientStrength, m_ambientSlider, 100.0f },
		{ &light.globalSaturation, m_globalSatSlider, 100.0f },
		{ &light.keyLightIntensity, m_keyIntensitySlider, 100.0f },
		{ &light.fillLightIntensity, m_fillIntensitySlider, 100.0f },
		{ &light.outlineWidthScale, m_outlineStrengthSlider, 100.0f },
		{ &light.outlineOpacityScale, m_outlineOpacitySlider, 100.0f },
		{ &light.toonContrast, m_toonContrastSlider, 100.0f },
		{ &light.shadowSaturationBoost, m_shadowSatSlider, 100.0f },
		{ &light.shadowDeepThreshold, m_shadowDeepThresholdSlider, 100.0f },
		{ &light.shadowDeepSoftness, m_shadowDeepSoftSlider, 100.0f },
		{ &light.shadowDeepMul, m_shadowDeepMulSlider, 100.0f },
		{ &light.faceShadowMul, m_faceShadowMulSlider, 100.0f },
		{ &light.faceToonContrastMul, m_faceContrastMulSlider, 100.0f },
		{ &light.rimWidth, m_rimWidthSlider, 100.0f },
		{ &light.rimIntensity, m_rimIntensitySlider, 100.0f },
		{ &light.specularStep, m_specularStepSlider, 100.0f },
	};
	for (const auto& binding : scaledSliders)
	{
		*binding.target = getSliderValue(binding.slider) / binding.scale;
	}

	const struct
	{
		float* target;
		HWND slider;
		float offset;
		float scale;
	} offsetSliders[] = {
		{ &light.shadowHueShiftDeg, m_shadowHueSlider, 45.0f, 1.0f },
		{ &light.keyLightDirX, m_keyDirXSlider, 1.0f, 100.0f },
		{ &light.keyLightDirY, m_keyDirYSlider, 1.0f, 100.0f },
		{ &light.keyLightDirZ, m_keyDirZSlider, 1.0f, 100.0f },
		{ &light.fillLightDirX, m_fillDirXSlider, 1.0f, 100.0f },
		{ &light.fillLightDirY, m_fillDirYSlider, 1.0f, 100.0f },
		{ &light.fillLightDirZ, m_fillDirZSlider, 1.0f, 100.0f },
	};
	for (const auto& binding : offsetSliders)
	{
		*binding.target = getSliderValue(binding.slider) / binding.scale - binding.offset;
	}

	light.shadowRampShift = (getSliderValue(m_shadowRampSlider) / 100.0f) - 0.5f;
}

void SettingsWindow::UpdateLightValueLabels(const LightSettings& light)
{
	const struct
	{
		HWND label;
		float value;
	} labelBindings[] = {
		{ m_scaleLabel, light.modelScale },
		{ m_brightnessLabel, light.brightness },
		{ m_ambientLabel, light.ambientStrength },
		{ m_globalSatLabel, light.globalSaturation },
		{ m_keyIntensityLabel, light.keyLightIntensity },
		{ m_fillIntensityLabel, light.fillLightIntensity },
		{ m_outlineStrengthLabel, light.outlineWidthScale },
		{ m_outlineOpacityLabel, light.outlineOpacityScale },
		{ m_toonContrastLabel, light.toonContrast },
		{ m_shadowHueLabel, light.shadowHueShiftDeg },
		{ m_shadowSatLabel, light.shadowSaturationBoost },
		{ m_shadowRampLabel, light.shadowRampShift },
		{ m_rimWidthLabel, light.rimWidth },
		{ m_rimIntensityLabel, light.rimIntensity },
		{ m_specularStepLabel, light.specularStep },
		{ m_shadowDeepThresholdLabel, light.shadowDeepThreshold },
		{ m_shadowDeepSoftLabel, light.shadowDeepSoftness },
		{ m_shadowDeepMulLabel, light.shadowDeepMul },
		{ m_faceShadowMulLabel, light.faceShadowMul },
		{ m_faceContrastMulLabel, light.faceToonContrastMul },
	};
	for (const auto& binding : labelBindings)
	{
		SetWindowTextW(binding.label, FormatFloat(binding.value).c_str());
	}
}

void SettingsWindow::UpdateLightDependentControlState(const LightSettings& light)
{
	const bool usesMsaa =
		light.antiAliasingMode == static_cast<int>(AntiAliasingMode::Msaa) ||
		light.antiAliasingMode == static_cast<int>(AntiAliasingMode::MsaaFxaa);
	SetControlsEnabled({ m_msaaSamplesCombo }, usesMsaa);
	SetControlsEnabled({ m_shadowResolutionCombo }, light.selfShadowEnabled);
	SetControlsEnabled(
		{ m_outlineStrengthSlider, m_outlineStrengthLabel, m_outlineOpacitySlider, m_outlineOpacityLabel },
		light.outlineEnabled);
	SetControlsEnabled(
		{ m_faceShadowMulSlider, m_faceShadowMulLabel, m_faceContrastMulSlider, m_faceContrastMulLabel },
		light.faceMaterialOverridesEnabled);
}

void SettingsWindow::LoadCurrentSettings()
{
	const auto& settings = m_app.Settings();
	LoadGeneralSettings(settings);
	LoadLightSettings(settings.light);
	LoadPhysicsSettings(settings.physics);

	UpdatePhysicsAdvancedVisibility();
	UpdateLightPreview();
	InvalidateRect(m_hwnd, nullptr, TRUE);
}

void SettingsWindow::UpdateLightPreview()
{
	LightSettings light = m_app.GetLightSettings();
	BuildLightSettingsFromUi(light);
	UpdateLightDependentControlState(light);
	UpdateLightValueLabels(light);
	m_app.UpdateLiveLightSettings(light);
}

bool SettingsWindow::PickColor(float& r, float& g, float& b, HWND)
{
	CHOOSECOLORW cc{};
	cc.lStructSize = sizeof(cc);
	cc.hwndOwner = m_hwnd;
	cc.lpCustColors = m_customColors;
	cc.rgbResult = FloatToColorRef(r, g, b);
	cc.Flags = CC_FULLOPEN | CC_RGBINIT;

	if (ChooseColorW(&cc))
	{
		r = GetRValue(cc.rgbResult) / 255.0f;
		g = GetGValue(cc.rgbResult) / 255.0f;
		b = GetBValue(cc.rgbResult) / 255.0f;
		return true;
	}
	return false;
}

void SettingsWindow::BuildGeneralSettingsFromUi(AppSettings& out) const
{
	wchar_t buf[MAX_PATH]{};
	GetWindowTextW(m_modelPathEdit, buf, MAX_PATH);
	out.modelPath = buf;

	out.alwaysOnTop = IsChecked(m_topmostCheck);
	out.unlimitedFps = IsChecked(m_unlimitedFpsCheck);
	out.limitFpsToMonitorRefreshRate = IsChecked(m_limitMonitorRefreshRateCheck);
	out.vsyncEnabled = IsChecked(m_vsyncCheck);
	if (out.limitFpsToMonitorRefreshRate || out.vsyncEnabled)
	{
		out.unlimitedFps = false;
	}

	int fps = GetEditBoxInt(m_fpsEdit, out.targetFps);
	out.targetFps = std::clamp(fps, 1, 240);

	int sel = (int)SendMessageW(m_presetModeCombo, CB_GETCURSEL, 0, 0);
	if (sel >= 0) out.globalPresetMode = static_cast<PresetMode>(sel);
}

void SettingsWindow::BuildLightSettingsFromUi(LightSettings& light) const
{
	ReadLightScalarControls(light);
	light.toonEnabled = IsChecked(m_toonEnableCheck);
	light.selfShadowEnabled = IsChecked(m_selfShadowCheck);
	light.outlineEnabled = IsChecked(m_outlineCheck);
	light.antiAliasingMode = std::clamp(static_cast<int>(SendMessageW(m_aaModeCombo, CB_GETCURSEL, 0, 0)), 0, 3);
	light.msaaSampleCount = GetComboSelectionItemData(m_msaaSamplesCombo, light.msaaSampleCount);
	light.shadowMapSize = GetComboSelectionItemData(m_shadowResolutionCombo, light.shadowMapSize);
	light.faceMaterialOverridesEnabled = IsChecked(m_faceMaterialOverridesCheck);
}

void SettingsWindow::BuildPhysicsSettingsFromUi(PhysicsSettings& physics) const
{
	physics.fixedTimeStep = std::max(0.0001f, GetEditBoxFloat(m_physicsFixedTimeStepEdit, physics.fixedTimeStep));
	physics.maxSubSteps = std::max(1, GetEditBoxInt(m_physicsMaxSubStepsEdit, physics.maxSubSteps));
	physics.warmupSteps = std::max(0, GetEditBoxInt(m_physicsWarmupStepsEdit, physics.warmupSteps));
	physics.gravity.x = GetEditBoxFloat(m_physicsGravityXEdit, physics.gravity.x);
	physics.gravity.y = GetEditBoxFloat(m_physicsGravityYEdit, physics.gravity.y);
	physics.gravity.z = GetEditBoxFloat(m_physicsGravityZEdit, physics.gravity.z);
	physics.kinematicPositionThreshold = std::max(0.0f, GetEditBoxFloat(m_physicsKinematicPosThresholdEdit, physics.kinematicPositionThreshold));
	physics.kinematicRotationThreshold = std::max(0.0f, GetEditBoxFloat(m_physicsKinematicRotThresholdEdit, physics.kinematicRotationThreshold));
	physics.minKinematicVelocityClip = std::max(0.0f, GetEditBoxFloat(m_physicsMinVelocityClipEdit, physics.minKinematicVelocityClip));
	physics.jointStopErp = std::clamp(GetEditBoxFloat(m_physicsJointStopErpEdit, physics.jointStopErp), 0.0f, 1.0f);
	physics.ccdThresholdScale = std::max(0.0f, GetEditBoxFloat(m_physicsCcdThresholdScaleEdit, physics.ccdThresholdScale));
	physics.sleepLinearThreshold = std::max(0.0f, GetEditBoxFloat(m_physicsSleepLinearThresholdEdit, physics.sleepLinearThreshold));
	physics.sleepAngularThreshold = std::max(0.0f, GetEditBoxFloat(m_physicsSleepAngularThresholdEdit, physics.sleepAngularThreshold));
	physics.writebackAngleThresholdDeg = std::max(0.0f, GetEditBoxFloat(m_physicsWritebackAngleThresholdEdit, physics.writebackAngleThresholdDeg));
}

void SettingsWindow::BuildSettingsFromUi(AppSettings& out) const
{
	BuildGeneralSettingsFromUi(out);
	out.light = m_app.GetLightSettings();
	BuildLightSettingsFromUi(out.light);
	BuildPhysicsSettingsFromUi(out.physics);
}

void SettingsWindow::ApplyAndSave()
{
	AppSettings newSettings = m_app.Settings();
	BuildSettingsFromUi(newSettings);
	m_app.ApplySettings(newSettings, true);
	m_backupSettings = m_app.Settings();
	LoadCurrentSettings();
}

void SettingsWindow::UpdateFpsControlState()
{
	const bool unlimited = (SendMessageW(m_unlimitedFpsCheck, BM_GETCHECK, 0, 0) == BST_CHECKED);
	const bool limitToRefreshRate = (SendMessageW(m_limitMonitorRefreshRateCheck, BM_GETCHECK, 0, 0) == BST_CHECKED);
	const bool vsyncEnabled = (SendMessageW(m_vsyncCheck, BM_GETCHECK, 0, 0) == BST_CHECKED);
	const bool hasLimiter = limitToRefreshRate || vsyncEnabled;
	const bool disableFpsInput = unlimited || limitToRefreshRate || vsyncEnabled;
	EnableWindow(m_fpsEdit, !disableFpsInput);
	EnableWindow(m_fpsSpin, !disableFpsInput);
	EnableWindow(m_unlimitedFpsCheck, !hasLimiter);
	EnableWindow(m_limitMonitorRefreshRateCheck, !unlimited);
	EnableWindow(m_vsyncCheck, !unlimited);
}

void SettingsWindow::UpdateFaceMaterialControlState()
{
	SetControlsEnabled(
		{ m_faceShadowMulSlider, m_faceShadowMulLabel, m_faceContrastMulSlider, m_faceContrastMulLabel },
		IsChecked(m_faceMaterialOverridesCheck));
}

bool SettingsWindow::HasUnsavedChanges() const
{
	if (!m_hwnd) return false;

	AppSettings current = m_backupSettings;
	BuildSettingsFromUi(current);
	return current != m_backupSettings;
}

LRESULT CALLBACK SettingsWindow::WndProcThunk(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	SettingsWindow* self = Win32UiUtil::InitializeWindowUserData<SettingsWindow>(hWnd, msg, lParam);
	if (self) return self->WndProc(hWnd, msg, wParam, lParam);
	return DefWindowProcW(hWnd, msg, wParam, lParam);
}

LRESULT CALLBACK SettingsWindow::ContentProcThunk(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	SettingsWindow* self = Win32UiUtil::InitializeWindowUserData<SettingsWindow>(hWnd, msg, lParam);
	if (self) return self->ContentProc(hWnd, msg, wParam, lParam);
	return DefWindowProcW(hWnd, msg, wParam, lParam);
}

LRESULT SettingsWindow::ContentProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	switch (msg)
	{
		case WM_VSCROLL:
			OnVScroll(wParam);
			return 0;
		case WM_MOUSEWHEEL:
			OnMouseWheel(GET_WHEEL_DELTA_WPARAM(wParam));
			return 0;
		case WM_COMMAND:
		case WM_NOTIFY:
		case WM_HSCROLL:
		case WM_DRAWITEM:
		case WM_CTLCOLORSTATIC:
		case WM_CTLCOLORBTN:
		case WM_CTLCOLOREDIT:
		case WM_CTLCOLORLISTBOX:
			return SendMessageW(m_hwnd, msg, wParam, lParam);
		default:
			break;
	}
	return DefWindowProcW(hWnd, msg, wParam, lParam);
}

LRESULT SettingsWindow::WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	switch (msg)
	{
		case WM_GETMINMAXINFO:
		{

			auto* mmi = reinterpret_cast<MINMAXINFO*>(lParam);
			if (mmi && m_fixedWindowWidth > 0)
			{
				mmi->ptMinTrackSize.x = m_fixedWindowWidth;
				mmi->ptMaxTrackSize.x = m_fixedWindowWidth;
			}
			return 0;
		}

		case WM_NCHITTEST:
		{

			LRESULT hit = DefWindowProcW(hWnd, msg, wParam, lParam);
			switch (hit)
			{
				case HTLEFT:
				case HTRIGHT:
					return HTCLIENT;
				case HTTOPLEFT:
				case HTTOPRIGHT:
					return HTTOP;
				case HTBOTTOMLEFT:
				case HTBOTTOMRIGHT:
					return HTBOTTOM;
				default:
					return hit;
			}
		}

		case WM_APP_NAV_CHANGED:
		{
			const int idx = static_cast<int>(wParam);
			const int pad = 6;
			switch (idx)
			{
				case 0: ScrollTo(m_sectionYBasic - pad); break;
				case 1: ScrollTo(m_sectionYLight - pad); break;
				case 2: ScrollTo(m_sectionYToon - pad); break;
				case 3: ScrollTo(m_sectionYPhysics - pad); break;
				default: break;
			}
			return 0;
		}

		case WM_NOTIFY:
			break;

		case WM_CTLCOLORSTATIC:
		case WM_CTLCOLORBTN:
		{
			HDC hdc = reinterpret_cast<HDC>(wParam);
			SetTextColor(hdc, kTextColor);
			SetBkMode(hdc, TRANSPARENT);
			return (LRESULT)m_darkBrush;
		}
		case WM_CTLCOLOREDIT:
		case WM_CTLCOLORLISTBOX:
		{
			HDC hdc = reinterpret_cast<HDC>(wParam);
			SetTextColor(hdc, kTextColor);
			SetBkColor(hdc, kDarkBkColor);
			return (LRESULT)m_darkBrush;
		}

		case WM_VSCROLL:
			OnVScroll(wParam);
			return 0;

		case WM_MOUSEWHEEL:
			OnMouseWheel(GET_WHEEL_DELTA_WPARAM(wParam));
			return 0;

		case WM_SIZE:
		{
			RECT rc;
			GetClientRect(hWnd, &rc);
			const int clientW = rc.right - rc.left;
			const int clientH = rc.bottom - rc.top;
			const SettingsFrameLayout frame = ComputeSettingsFrameLayout(clientW, clientH, m_footerHeight);
			m_headerHeight = frame.headerHeight;

			if (m_tabs)
			{
				SetWindowPos(
					m_tabs,
					nullptr,
					frame.outerPadX,
					frame.outerPadY,
					std::max(0, clientW - frame.outerPadX * 2),
					frame.tabHeight,
					SWP_NOZORDER | SWP_NOACTIVATE);
			}
			if (m_footerDivider)
			{
				SetWindowPos(m_footerDivider, nullptr, 0, frame.footerTop, clientW, 1, SWP_NOZORDER | SWP_NOACTIVATE);
			}
			if (m_okBtn) SetWindowPos(m_okBtn, nullptr, frame.xOk, frame.footerY, frame.buttonWidth, frame.buttonHeight, SWP_NOZORDER | SWP_NOACTIVATE);
			if (m_cancelBtn) SetWindowPos(m_cancelBtn, nullptr, frame.xCancel, frame.footerY, frame.buttonWidth, frame.buttonHeight, SWP_NOZORDER | SWP_NOACTIVATE);
			if (m_applyBtn) SetWindowPos(m_applyBtn, nullptr, frame.xApply, frame.footerY, frame.buttonWidth, frame.buttonHeight, SWP_NOZORDER | SWP_NOACTIVATE);

			if (m_content)
			{
				SetWindowPos(m_content, nullptr, 0, frame.contentY, clientW, frame.contentHeight, SWP_NOZORDER | SWP_NOACTIVATE);
			}

			ScrollTo(m_scrollY);
			return 0;
		}

		case WM_COMMAND:
			switch (LOWORD(wParam))
			{
				case ID_BROWSE: {
					wchar_t path[MAX_PATH]{};
					OPENFILENAMEW ofn{};
					ofn.lStructSize = sizeof(ofn);
					ofn.hwndOwner = hWnd;
					ofn.lpstrFilter = L"PMXモデル (*.pmx)\0*.pmx\0すべてのファイル\0*.*\0";
					ofn.lpstrFile = path;
					ofn.nMaxFile = MAX_PATH;
					ofn.Flags = OFN_FILEMUSTEXIST;
					if (GetOpenFileNameW(&ofn)) SetWindowTextW(m_modelPathEdit, path);
					return 0;
				}
				case ID_KEY_COLOR_BTN: {
					LightSettings light = m_app.GetLightSettings();
					if (PickColor(light.keyLightColorR, light.keyLightColorG, light.keyLightColorB, m_keyColorBtn))
					{
						m_app.UpdateLiveLightSettings(light);
						InvalidateRect(m_hwnd, nullptr, TRUE);
					}
					return 0;
				}
				case ID_FILL_COLOR_BTN: {
					LightSettings light = m_app.GetLightSettings();
					if (PickColor(light.fillLightColorR, light.fillLightColorG, light.fillLightColorB, m_fillColorBtn))
					{
						m_app.UpdateLiveLightSettings(light);
						InvalidateRect(m_hwnd, nullptr, TRUE);
					}
					return 0;
				}
				case ID_TOON_ENABLE: UpdateLightPreview(); return 0;
				case ID_SELF_SHADOW_ENABLE: UpdateLightPreview(); return 0;
				case ID_OUTLINE_ENABLE: UpdateLightPreview(); return 0;
				case ID_FACE_OVERRIDE_ENABLE: UpdateLightPreview(); return 0;
				case ID_AA_MODE_COMBO:
				case ID_MSAA_SAMPLES_COMBO:
				case ID_SHADOW_RESOLUTION_COMBO:
					if (HIWORD(wParam) == CBN_SELCHANGE)
					{
						UpdateLightPreview();
					}
					return 0;
				case ID_FPS_UNLIMITED:
				case ID_FPS_LIMIT_TO_REFRESH:
				case ID_FPS_VSYNC:
					UpdateFpsControlState();
					return 0;
				case ID_RESET_LIGHT: {
					LightSettings defaultLight;
					m_app.UpdateLiveLightSettings(defaultLight);
					LoadCurrentSettings();
					return 0;
				}
				case ID_SAVE_PRESET: {
					AppSettings currentSettings = m_app.Settings();
					BuildSettingsFromUi(currentSettings);
					if (currentSettings.modelPath.empty())
					{
						MessageBoxW(m_hwnd, L"モデルが読み込まれていません。", L"エラー", MB_ICONWARNING);
					}
					else
					{
						SettingsManager::SavePreset(
							m_app.BaseDir(),
							currentSettings.modelPath,
							currentSettings.light,
							currentSettings.physics);
						MessageBoxW(m_hwnd, L"現在のライト/物理設定をモデル用プリセットとして保存しました。", L"保存完了", MB_ICONINFORMATION);
					}
					return 0;
				}
				case ID_LOAD_PRESET: {
					const auto& settings = m_app.Settings();
					if (settings.modelPath.empty())
					{
						MessageBoxW(m_hwnd, L"モデルが読み込まれていません。", L"エラー", MB_ICONWARNING);
					}
					else
					{
						LightSettings light = m_app.GetLightSettings();
						PhysicsSettings physics = m_app.GetPhysicsSettings();
						if (SettingsManager::LoadPreset(m_app.BaseDir(), settings.modelPath, light, physics))
						{
							m_app.UpdateLiveLightSettings(light);
							m_app.UpdateLivePhysicsSettings(physics);
							LoadCurrentSettings();
							MessageBoxW(m_hwnd, L"プリセットを読み込みました。", L"完了", MB_ICONINFORMATION);
						}
						else
						{
							MessageBoxW(m_hwnd, L"このモデル用のプリセットが見つかりません。", L"エラー", MB_ICONWARNING);
						}
					}
					return 0;
				}
				case ID_OK: ApplyAndSave(); Hide(); return 0;
				case ID_CANCEL: m_app.ApplySettings(m_backupSettings, false); LoadCurrentSettings(); Hide(); return 0;
				case ID_APPLY: ApplyAndSave(); return 0;
			}
			break;

		case WM_HSCROLL:
			UpdateLightPreview();
			return 0;

		case WM_DRAWITEM: {
			auto* dis = reinterpret_cast<DRAWITEMSTRUCT*>(lParam);
			if (dis->hwndItem == m_keyColorPreview || dis->hwndItem == m_fillColorPreview)
			{
				FillRect(dis->hDC, &dis->rcItem, m_darkBrush);
				RECT rc = dis->rcItem; InflateRect(&rc, -2, -2);
				HBRUSH brush = nullptr;
				const auto& light = m_app.GetLightSettings();
				if (dis->hwndItem == m_keyColorPreview) brush = CreateSolidBrush(FloatToColorRef(light.keyLightColorR, light.keyLightColorG, light.keyLightColorB));
				else brush = CreateSolidBrush(FloatToColorRef(light.fillLightColorR, light.fillLightColorG, light.fillLightColorB));
				FillRect(dis->hDC, &rc, brush);
				DeleteObject(brush);
				HBRUSH frameBrush = CreateSolidBrush(RGB(100, 100, 100));
				FrameRect(dis->hDC, &rc, frameBrush);
				DeleteObject(frameBrush);
				return TRUE;
			}
			break;
		}

		case WM_CLOSE:
		{
			if (!HasUnsavedChanges())
			{
				Hide();
				return 0;
			}

			const int result = MessageBoxW(
				m_hwnd,
				L"設定を適用して閉じますか？",
				L"確認",
				MB_YESNOCANCEL | MB_ICONQUESTION);

			if (result == IDYES)
			{
				ApplyAndSave();
				Hide();
				return 0;
			}
			if (result == IDNO)
			{
				m_app.ApplySettings(m_backupSettings, false);
				LoadCurrentSettings();
				Hide();
				return 0;
			}
			return 0;
		}


		case WM_THEMECHANGED:
		case WM_SETTINGCHANGE:
		{
			if (Win32UiUtil::ConsumeThemeChange(hWnd))
			{
				return DefWindowProcW(hWnd, msg, wParam, lParam);
			}

			const LRESULT r = DefWindowProcW(hWnd, msg, wParam, lParam);

			Win32UiUtil::ResetDarkControlTheme(m_content);
			Win32UiUtil::ScheduleDarkScrollbarsRefresh(hWnd);
			RedrawWindow(m_hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
			return r;
		}

		case Win32UiUtil::kDarkScrollbarsRefreshMessage:
		{
			Win32UiUtil::CompleteDarkScrollbarsRefresh(hWnd, m_content);
			RedrawWindow(m_hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
			return 0;
		}

		default: break;
	}

	return DefWindowProcW(hWnd, msg, wParam, lParam);
}
