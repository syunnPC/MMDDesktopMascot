#pragma once
#include <windows.h>
#include <string>

#include "Settings.hpp"

class App;

class SettingsWindow
{
public:
	SettingsWindow(App& app, HINSTANCE hInst);
	~SettingsWindow();

	SettingsWindow(const SettingsWindow&) = delete;
	SettingsWindow& operator=(const SettingsWindow&) = delete;

	void Show();
	void Hide();
	void Refresh()
	{
		LoadCurrentSettings();
	}

private:
	static LRESULT CALLBACK WndProcThunk(HWND, UINT, WPARAM, LPARAM);
	static LRESULT CALLBACK ContentProcThunk(HWND, UINT, WPARAM, LPARAM);
	LRESULT WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
	LRESULT ContentProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

	void CreateControls();
	void LoadCurrentSettings();
	void LoadGeneralSettings(const AppSettings& settings);
	void LoadLightSettings(const LightSettings& light);
	void LoadPhysicsSettings(const PhysicsSettings& physics);
	void LoadLightScalarControls(const LightSettings& light);
	void ReadLightScalarControls(LightSettings& light) const;
	void UpdateLightValueLabels(const LightSettings& light);
	void UpdateLightDependentControlState(const LightSettings& light);
	void BuildSettingsFromUi(AppSettings& out) const;
	void BuildGeneralSettingsFromUi(AppSettings& out) const;
	void BuildLightSettingsFromUi(LightSettings& light) const;
	void BuildPhysicsSettingsFromUi(PhysicsSettings& physics) const;
	void ApplyAndSave();
	void UpdateLightPreview();
	void UpdateFaceMaterialControlState();
	bool PickColor(float& r, float& g, float& b, HWND btnHwnd);
	void UpdateFpsControlState();
	void UpdatePhysicsAdvancedVisibility();

	void SetHeaderFont(HWND hChild);
	void SetModernFont(HWND hChild);
	void SetDarkTheme(HWND hChild);

	void UpdateScrollInfo();
	void OnVScroll(WPARAM wParam);
	void OnMouseWheel(int delta);
	void ScrollTo(int targetY);
	void UpdateNavHighlightFromScroll();
	bool HasUnsavedChanges() const;

	App& m_app;
	HINSTANCE m_hInst;
	HWND m_hwnd{};
	HWND m_tabs{};
	HWND m_content{};
	HWND m_footerDivider{};

	HFONT m_hFont{ nullptr };
	HFONT m_hHeaderFont{ nullptr };
	HBRUSH m_darkBrush{ nullptr };
	HWND m_tooltip{};

	// サイズ変更(縦のみ許可)
	int m_fixedWindowWidth{ 0 };

	int m_totalContentHeight{ 0 };
	int m_scrollY{ 0 };
	int m_lastAutoNavIndex{ -1 };
	int m_sectionYBasic{ 0 };
	int m_sectionYLight{ 0 };
	int m_sectionYToon{ 0 };
	int m_sectionYPhysics{ 0 };

	int m_headerHeight{ 52 };
	int m_footerHeight{ 58 };
	int m_anchorBasicY{ 0 };
	int m_anchorLightY{ 0 };
	int m_anchorToonY{ 0 };
	int m_anchorPhysicsY{ 0 };

	// 基本設定
	HWND m_modelPathEdit{};
	HWND m_browseBtn{};
	HWND m_topmostCheck{};
	HWND m_fpsEdit{};
	HWND m_fpsSpin{};
	HWND m_unlimitedFpsCheck{};
	HWND m_limitMonitorRefreshRateCheck{};
	HWND m_vsyncCheck{};

	// プリセット設定
	HWND m_presetModeCombo{};

	// スケール
	HWND m_scaleSlider{};
	HWND m_scaleLabel{};

	// ライト設定
	HWND m_brightnessSlider{};
	HWND m_brightnessLabel{};
	HWND m_ambientSlider{};
	HWND m_ambientLabel{};
	HWND m_globalSatSlider{};
	HWND m_globalSatLabel{};
	HWND m_keyIntensitySlider{};
	HWND m_keyIntensityLabel{};
	HWND m_keyColorBtn{};
	HWND m_keyColorPreview{};
	HWND m_fillIntensitySlider{};
	HWND m_fillIntensityLabel{};
	HWND m_fillColorBtn{};
	HWND m_fillColorPreview{};

	// Toon Control
	HWND m_toonEnableCheck{};
	HWND m_selfShadowCheck{};
	HWND m_outlineCheck{};
	HWND m_outlineStrengthSlider{};
	HWND m_outlineStrengthLabel{};
	HWND m_outlineOpacitySlider{};
	HWND m_outlineOpacityLabel{};
	HWND m_aaModeCombo{};
	HWND m_msaaSamplesCombo{};
	HWND m_shadowResolutionCombo{};
	HWND m_toonContrastSlider{};
	HWND m_toonContrastLabel{};
	HWND m_shadowHueSlider{};
	HWND m_shadowHueLabel{};
	HWND m_shadowSatSlider{};
	HWND m_shadowSatLabel{};
	HWND m_shadowRampSlider{};
	HWND m_shadowRampLabel{};

	// Deep Shadow
	HWND m_shadowDeepThresholdSlider{};
	HWND m_shadowDeepThresholdLabel{};
	HWND m_shadowDeepSoftSlider{};
	HWND m_shadowDeepSoftLabel{};
	HWND m_shadowDeepMulSlider{};
	HWND m_shadowDeepMulLabel{};
	HWND m_rimWidthSlider{};
	HWND m_rimWidthLabel{};
	HWND m_rimIntensitySlider{};
	HWND m_rimIntensityLabel{};
	HWND m_specularStepSlider{};
	HWND m_specularStepLabel{};

	// Face Control
	HWND m_faceMaterialOverridesCheck{};
	HWND m_faceShadowMulSlider{};
	HWND m_faceShadowMulLabel{};
	HWND m_faceContrastMulSlider{};
	HWND m_faceContrastMulLabel{};

	// Direction
	HWND m_keyDirXSlider{};
	HWND m_keyDirYSlider{};
	HWND m_keyDirZSlider{};
	HWND m_fillDirXSlider{};
	HWND m_fillDirYSlider{};
	HWND m_fillDirZSlider{};

	// Physics settings
	HWND m_physicsFixedTimeStepEdit{};
	HWND m_physicsMaxSubStepsEdit{};
	HWND m_physicsWarmupStepsEdit{};
	HWND m_physicsGravityXEdit{};
	HWND m_physicsGravityYEdit{};
	HWND m_physicsGravityZEdit{};
	int m_physicsAdvancedHeight{ 0 };
	int m_physicsAdvancedStartY{ 0 };
	int m_physicsAdvancedEndY{ 0 };
	int m_physicsActionsExpandedY{ 0 };
	int m_physicsExpandedContentHeight{ 0 };
	int m_physicsCollapsedContentHeight{ 0 };

	HWND m_resetLightBtn{};
	HWND m_savePresetBtn{};
	HWND m_loadPresetBtn{};

	HWND m_okBtn{};
	HWND m_cancelBtn{};
	HWND m_applyBtn{};

	bool m_created{ false };
	COLORREF m_customColors[16]{};
	AppSettings m_backupSettings{};
};
