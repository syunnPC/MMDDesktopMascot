#pragma once
#include <windows.h>
#include <commctrl.h>
#include <string>

class ProgressWindow
{
public:
	ProgressWindow(HINSTANCE hInst, HWND parent);
	~ProgressWindow();

	void Show();
	void Hide();
	void SetProgress(float percentage); // 0.0 - 1.0
	void SetMessage(const std::wstring& msg);

	bool IsVisible() const
	{
		return m_hwnd != nullptr;
	}
	HWND Handle() const
	{
		return m_hwnd;
	}

private:
	static LRESULT CALLBACK WndProcThunk(HWND, UINT, WPARAM, LPARAM);
	LRESULT WndProc(HWND, UINT, WPARAM, LPARAM);

	RECT ComputeWindowBounds() const;
	void CreateControls();
	void SetModernFont(HWND hChild);
	void ConfigureProgressBar();

	HINSTANCE m_hInst;
	HWND m_parent;
	HWND m_hwnd{ nullptr };
	HWND m_progressBar{ nullptr };
	HWND m_statusLabel{ nullptr };

	HFONT m_hFont{ nullptr };
	HBRUSH m_darkBrush{ nullptr };
};
