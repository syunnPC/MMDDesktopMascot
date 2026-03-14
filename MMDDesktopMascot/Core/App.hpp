#pragma once
#include <windows.h>
#include <memory>
#include <filesystem>
#include <string>
#include <vector>
#include <atomic>
#include <thread>
#include <chrono>
#include "ProgressWindow.hpp"
#include "TrayIcon.hpp"
#include "DcompRenderer.hpp"
#include "MmdAnimator.hpp"
#include "Settings.hpp"
#include "InputManager.hpp"
#include "WindowManager.hpp"
#include "TrayMenuWindow.hpp"

constexpr UINT kDefaultTimerMs = 16;

class SettingsWindow;

class App
{
public:
	explicit App(HINSTANCE hInst);
	~App();

	App(const App&) = delete;
	App& operator=(const App&) = delete;

	int Run();

	const AppSettings& Settings() const
	{
		return m_settingsData;
	}
	void ApplySettings(const AppSettings& settings, bool persist);
	const LightSettings& GetLightSettings() const
	{
		return m_settingsData.light;
	}
	UINT GetMaximumSupportedMsaaSampleCount() const;
	const PhysicsSettings& GetPhysicsSettings() const
	{
		return m_settingsData.physics;
	}
	void UpdateLiveLightSettings(const LightSettings& light);
	void UpdateLivePhysicsSettings(const PhysicsSettings& physics);
	void ApplyLightSettings();
	void ApplyPhysicsSettings();
	void SaveSettings();

	const std::filesystem::path& ModelsDir() const
	{
		return m_modelsDir;
	}
	const std::filesystem::path& BaseDir() const
	{
		return m_baseDir;
	}

	void ToggleGizmoWindow();
	void TogglePhysics();
	void ToggleWindowManipulation();
	void MoveRenderWindowBy(int dx, int dy);
	void AddCameraRotation(float dx, float dy);
	void AdjustScale(float delta);
	void AdjustBrightness(float delta);
	void RenderGizmo();

private:
	void InitRenderer();
	void InitTray();
	void InitAnimator();
	void BuildTrayMenu();
	void RefreshMotionList();
	void UpdateTimerInterval();
	void ApplyPresentationSettings();
	int GetRenderMonitorRefreshRateHz() const;
	std::chrono::steady_clock::duration ComputeTimerInterval() const;
	void LoadModelFromSettings();
	std::filesystem::path NormalizeModelPath(const std::filesystem::path& path) const;
	ProgressWindow& EnsureProgressWindow();
	SettingsWindow& EnsureSettingsWindow();
	bool ShowPresetLoadDialog(const std::filesystem::path& modelPath);
	void ApplyPresetSettings(const std::filesystem::path& path);
	bool ShouldPersistPresetFor(const std::filesystem::path& modelPath) const;
	void PersistPresetIfNeeded();
	void SyncSettingsFromRuntime();
	void ApplyAnimatorRuntimeSettings();
	void UpdateLookAtTracking();
	bool ApplyThemeCommand(UINT id);
	bool ApplyMotionCommand(UINT id);
	void PostLoadProgress(float progress, const wchar_t* message);
	void ToggleLookAt();
	void ToggleAutoBlink();
	void ToggleBreathing();

	void OnTrayCommand(UINT id);
	void ShowTrayMenu(const POINT& anchor);
	void OnTimer();
	void Update();
	void Render();
	void OnLoadProgress(WPARAM wParam, LPARAM lParam);

	HINSTANCE m_hInst{};

	std::unique_ptr<TrayIcon> m_tray;
	std::unique_ptr<TrayMenuWindow> m_trayMenu;
	std::unique_ptr<SettingsWindow> m_settings;

	std::unique_ptr<DcompRenderer> m_renderer;
	std::unique_ptr<MmdAnimator> m_animator;

	InputManager m_input;
	WindowManager m_windowManager;

	AppSettings m_settingsData;

	std::filesystem::path m_baseDir;
	std::filesystem::path m_modelsDir;
	std::filesystem::path m_motionsDir;

	std::vector<std::filesystem::path> m_motionFiles;

	ULONG_PTR m_gdiplusToken{};
	bool m_gdiplusInitialized{ false };
	bool m_timerPeriodEnabled{ false };

	// ローディング関連
	std::unique_ptr<ProgressWindow> m_progress;
	std::atomic<bool> m_isLoading{ false };
	std::jthread m_loadThread;

	void StartLoadingModel(const std::filesystem::path& path);
	void OnLoadComplete(WPARAM wParam, LPARAM lParam);

	void CancelLoadingThread();

	std::chrono::steady_clock::duration m_frameInterval{ std::chrono::milliseconds(kDefaultTimerMs) };
	std::filesystem::path m_loadedModelPath;
	std::filesystem::path m_loadingModelPath;
	mutable int m_cachedMonitorRefreshRateHz{ 0 };
	mutable std::chrono::steady_clock::time_point m_lastMonitorRefreshRateQuery{};

	bool m_lookAtEnabled{ false };
};
