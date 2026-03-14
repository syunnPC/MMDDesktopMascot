#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "App.hpp"
#include "FileUtil.hpp"
#include "ModelLoadWorker.hpp"
#include "SettingsWindow.hpp"
#include "Settings.hpp"
#include "ProgressWindow.hpp"
#include <algorithm>
#include <thread>
#include <stdexcept>
#include <string>
#include <cmath>
#include <utility>
#include <shellapi.h>
#include <ShObjIdl_core.h>
#include <commctrl.h>
#include <mmsystem.h>
#include <gdiplus.h>
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "winmm.lib")

namespace
{
	struct ModelLoadProgress
	{
		float progress{};
		std::wstring message;
	};

	struct ThemeCommandEntry
	{
		UINT commandId;
		const wchar_t* title;
		TrayMenuThemeId theme;
	};

	class ScopedComApartment
	{
	public:
		explicit ScopedComApartment(DWORD coInitFlags) noexcept
			: m_result(CoInitializeEx(nullptr, coInitFlags))
		{
		}

		~ScopedComApartment()
		{
			if (SUCCEEDED(m_result))
			{
				CoUninitialize();
			}
		}

	private:
		HRESULT m_result{};
	};

	template <typename T>
	bool PostOwnedWindowMessage(HWND hwnd, UINT message, std::unique_ptr<T> payload)
	{
		if (!hwnd || !payload)
		{
			return false;
		}

		T* rawPayload = payload.get();
		if (!PostMessageW(hwnd, message, 0, reinterpret_cast<LPARAM>(rawPayload)))
		{
			return false;
		}

		payload.release();
		return true;
	}

	TrayMenuItem MakeActionItem(UINT commandId, std::wstring title, std::wstring subtitle, bool destructive = false)
	{
		TrayMenuItem item;
		item.kind = TrayMenuItem::Kind::Action;
		item.commandId = commandId;
		item.title = std::move(title);
		item.subtitle = std::move(subtitle);
		item.destructive = destructive;
		return item;
	}

	TrayMenuItem MakeToggleItem(UINT commandId, std::wstring title, std::wstring subtitle, bool toggled)
	{
		TrayMenuItem item;
		item.kind = TrayMenuItem::Kind::Toggle;
		item.commandId = commandId;
		item.title = std::move(title);
		item.subtitle = std::move(subtitle);
		item.toggled = toggled;
		return item;
	}

	TrayMenuItem MakeHeaderItem(std::wstring title)
	{
		TrayMenuItem item;
		item.kind = TrayMenuItem::Kind::Header;
		item.title = std::move(title);
		return item;
	}

	TrayMenuItem MakeSeparatorItem()
	{
		TrayMenuItem item;
		item.kind = TrayMenuItem::Kind::Separator;
		return item;
	}

	static TrayMenuThemeId ClampTrayMenuThemeId(int v)
	{
		// Persisted values: 0..5 (Custom is not persisted)
		switch (v)
		{
			case 0: return TrayMenuThemeId::DarkDefault;
			case 1: return TrayMenuThemeId::Light;
			case 2: return TrayMenuThemeId::Midnight;
			case 3: return TrayMenuThemeId::Sakura;
			case 4: return TrayMenuThemeId::SolarizedDark;
			case 5: return TrayMenuThemeId::HighContrast;
			default: return TrayMenuThemeId::DarkDefault;
		}
	}

	enum TrayCmd : UINT
	{
		CMD_OPEN_SETTINGS = 100,
		CMD_RELOAD_MOTIONS = 101,
		CMD_STOP_MOTION = 102,
		CMD_TOGGLE_PAUSE = 103,
		CMD_TOGGLE_PHYSICS = 104,
		CMD_TOGGLE_WINDOW_MANIP = 105,
		CMD_EXIT = 199,
		CMD_TOGGLE_LOOKAT = 106,
		CMD_TOGGLE_AUTOBLINK = 107,
		CMD_TOGGLE_BREATH = 108,

		CMD_THEME_DARK = 200,
		CMD_THEME_LIGHT = 201,
		CMD_THEME_MIDNIGHT = 202,
		CMD_THEME_SAKURA = 203,
		CMD_THEME_SOLARIZED = 204,
		CMD_THEME_HIGHCONTRAST = 205,
		CMD_MOTION_BASE = 1000
	};

	constexpr ThemeCommandEntry kThemeCommands[] = {
		{ CMD_THEME_DARK, L"ダーク (既定)", TrayMenuThemeId::DarkDefault },
		{ CMD_THEME_LIGHT, L"ライト", TrayMenuThemeId::Light },
		{ CMD_THEME_MIDNIGHT, L"ミッドナイト", TrayMenuThemeId::Midnight },
		{ CMD_THEME_SAKURA, L"桜 (Sakura)", TrayMenuThemeId::Sakura },
		{ CMD_THEME_SOLARIZED, L"ソーラー", TrayMenuThemeId::SolarizedDark },
		{ CMD_THEME_HIGHCONTRAST, L"ハイコントラスト", TrayMenuThemeId::HighContrast },
	};

	TrayMenuItem BuildThemeMenuItem(TrayMenuThemeId currentTheme)
	{
		TrayMenuItem themeMenu;
		themeMenu.kind = TrayMenuItem::Kind::Action;
		themeMenu.title = L"テーマ";

		for (const auto& entry : kThemeCommands)
		{
			TrayMenuItem item = MakeActionItem(entry.commandId, entry.title, L"");
			item.toggled = (currentTheme == entry.theme);
			themeMenu.children.push_back(std::move(item));
		}

		return themeMenu;
	}

	void AppendMotionMenuItems(std::vector<TrayMenuItem>& items, const std::vector<std::filesystem::path>& motionFiles)
	{
		items.push_back(MakeHeaderItem(L"モーション"));

		if (motionFiles.empty())
		{
			items.push_back(MakeActionItem(
				0,
				L"モーションファイルが見つかりません",
				L"\"Motions\" フォルダーに .vmd を追加してください"));
			return;
		}

		for (size_t i = 0; i < motionFiles.size(); ++i)
		{
			items.push_back(MakeActionItem(
				CMD_MOTION_BASE + static_cast<UINT>(i),
				motionFiles[i].stem().wstring(),
				L"クリックして再生を開始"));
		}
	}
}

App::App(HINSTANCE hInst)
	: m_hInst(hInst)
	, m_input(*this)
	, m_windowManager(
		hInst,
		m_input,
		m_settingsData,
		WindowManager::Callbacks{
			[this](const POINT& pt) { ShowTrayMenu(pt); },
			[this](UINT id) { OnTrayCommand(id); },
			[this](WPARAM wParam, LPARAM lParam) { OnLoadProgress(wParam, lParam); },
			[this](WPARAM wParam, LPARAM lParam) { OnLoadComplete(wParam, lParam); },
			[this]() { SaveSettings(); }
		})
{
	Gdiplus::GdiplusStartupInput gdiplusStartupInput;
	if (Gdiplus::GdiplusStartup(&m_gdiplusToken, &gdiplusStartupInput, nullptr) == Gdiplus::Ok)
	{
		m_gdiplusInitialized = true;
	}
	else
	{
		throw std::runtime_error("GdiplusStartup failed.");
	}

	SetCurrentProcessExplicitAppUserModelID(L"MMDDesktopMascot");
	if (timeBeginPeriod(1) == TIMERR_NOERROR)
	{
		m_timerPeriodEnabled = true;
	}

	m_baseDir = FileUtil::GetExecutableDir();
	m_modelsDir = m_baseDir / L"Models";
	m_motionsDir = m_baseDir / L"Motions";

	const auto defaultModel = m_modelsDir / L"default.pmx";
	m_settingsData = SettingsManager::Load(m_baseDir, defaultModel);

	if (m_settingsData.modelPath.empty())
	{
		m_settingsData.modelPath = defaultModel;
	}
	m_settingsData.modelPath = NormalizeModelPath(m_settingsData.modelPath);
	if (!m_settingsData.modelPath.empty() && !std::filesystem::exists(m_settingsData.modelPath))
	{
		m_settingsData.modelPath = std::filesystem::exists(defaultModel)
			? defaultModel
			: std::filesystem::path{};
	}

	m_windowManager.Initialize();
	m_input.SetWindows(m_windowManager.GizmoWindow());
	m_input.RegisterHotkeys(m_windowManager.RenderWindow());

	m_windowManager.ApplyTopmost(m_settingsData.alwaysOnTop);

	InitRenderer();
	InitAnimator();

	m_trayMenu = std::make_unique<TrayMenuWindow>(m_hInst, [this](UINT id) { OnTrayCommand(id); });

	// 起動時に設定ファイルのテーマを適用
	{
		const TrayMenuThemeId theme = ClampTrayMenuThemeId(m_settingsData.trayMenuThemeId);
		m_trayMenu->SetTheme(theme);
		m_settingsData.trayMenuThemeId = static_cast<int>(theme);
	}

	BuildTrayMenu();
	InitTray();

	ApplyPresentationSettings();
}

App::~App()
{
	CancelLoadingThread();
	SaveSettings();

	m_input.UnregisterHotkeys(m_windowManager.RenderWindow());

	if (m_gdiplusInitialized)
	{
		Gdiplus::GdiplusShutdown(m_gdiplusToken);
	}

	if (m_timerPeriodEnabled)
	{
		timeEndPeriod(1);
	}
}

int App::Run()
{
	using clock = std::chrono::steady_clock;
	MSG msg{};
	m_frameInterval = ComputeTimerInterval();
	auto nextTick = clock::now();

	while (true)
	{
		while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
		{
			if (msg.message == WM_QUIT)
			{
				return static_cast<int>(msg.wParam);
			}
			TranslateMessage(&msg);
			DispatchMessageW(&msg);
		}

		const auto now = clock::now();
		if (m_frameInterval <= clock::duration::zero() || now >= nextTick)
		{
			OnTimer();
			m_frameInterval = ComputeTimerInterval();
			const auto afterTick = clock::now();
			if (m_frameInterval <= clock::duration::zero())
			{
				nextTick = afterTick;
			}
			else
			{
				nextTick += m_frameInterval;
				if (nextTick <= afterTick)
				{
					nextTick = afterTick + m_frameInterval;
				}
			}
			continue;
		}

		const auto sleepFor = nextTick - now;
		if (sleepFor > std::chrono::milliseconds(2))
		{
			// Sleep a little less than the remaining budget to reduce overshoot.
			std::this_thread::sleep_for(sleepFor - std::chrono::milliseconds(1));
		}
		else
		{
			std::this_thread::yield();
		}
	}
}

void App::LoadModelFromSettings()
{
	if (!m_animator) return;
	if (m_settingsData.modelPath.empty()) return;

	// 既に読み込み中なら無視
	if (m_isLoading) return;

	StartLoadingModel(m_settingsData.modelPath);
}

std::filesystem::path App::NormalizeModelPath(const std::filesystem::path& path) const
{
	if (path.empty())
	{
		return {};
	}

	if (path.is_absolute())
	{
		return path.lexically_normal();
	}

	return (m_baseDir / path).lexically_normal();
}

ProgressWindow& App::EnsureProgressWindow()
{
	if (!m_progress)
	{
		m_progress = std::make_unique<ProgressWindow>(m_hInst, m_windowManager.RenderWindow());
	}

	return *m_progress;
}

SettingsWindow& App::EnsureSettingsWindow()
{
	if (!m_settings)
	{
		m_settings = std::make_unique<SettingsWindow>(*this, m_hInst);
	}

	return *m_settings;
}

bool App::ShowPresetLoadDialog(const std::filesystem::path& modelPath)
{
	TASKDIALOGCONFIG config{};
	config.cbSize = sizeof(config);
	config.hwndParent = m_windowManager.RenderWindow();
	config.hInstance = m_hInst;
	config.dwFlags = TDF_ENABLE_HYPERLINKS | TDF_ALLOW_DIALOG_CANCELLATION | TDF_USE_COMMAND_LINKS_NO_ICON;
	config.pszWindowTitle = L"設定の読み込み";
	config.pszMainInstruction = L"このモデル用の設定プリセットが見つかりました。";
	config.pszContent = L"保存された表示・ライト・物理設定を適用しますか？";

	TASKDIALOG_BUTTON buttons[] = {
		{ IDYES, L"読み込む" },
		{ IDNO, L"読み込まない" }
	};
	config.pButtons = buttons;
	config.cButtons = _countof(buttons);
	config.nDefaultButton = IDYES;

	TASKDIALOG_BUTTON radios[] = {
		{ 100, L"次回も確認する" },
		{ 101, L"このモデルは次回から同じ選択をする" },
		{ 102, L"すべてのモデルで次回から同じ選択をする" }
	};
	config.pRadioButtons = radios;
	config.cRadioButtons = _countof(radios);
	config.nDefaultRadioButton = 100;

	int button = 0;
	int radio = 0;
	const HRESULT hr = TaskDialogIndirect(&config, &button, &radio, nullptr);
	if (FAILED(hr))
	{
		throw std::runtime_error("TaskDialogIndirect failed.");
	}

	const bool shouldLoad = (button == IDYES);
	if (radio == 101)
	{
		const std::wstring key = SettingsManager::MakeModelPresetKey(m_baseDir, modelPath);
		if (!key.empty())
		{
			m_settingsData.perModelPresetSettings[key] = shouldLoad ? PresetMode::AlwaysLoad : PresetMode::NeverLoad;
		}
		SettingsManager::Save(m_baseDir, m_settingsData);
	}
	else if (radio == 102)
	{
		m_settingsData.globalPresetMode = shouldLoad ? PresetMode::AlwaysLoad : PresetMode::NeverLoad;
		SettingsManager::Save(m_baseDir, m_settingsData);
	}

	return shouldLoad;
}

void App::ApplyPresetSettings(const std::filesystem::path& path)
{
	if (!SettingsManager::LoadPreset(m_baseDir, path, m_settingsData.light, m_settingsData.physics))
	{
		return;
	}

	ApplyLightSettings();
	m_animator->SetPhysicsSettings(m_settingsData.physics);
	if (m_settings)
	{
		m_settings->Refresh();
	}
}

void App::PostLoadProgress(float progress, const wchar_t* message)
{
	HWND msgWnd = m_windowManager.MessageWindow();
	if (!msgWnd) return;

	auto payload = std::make_unique<ModelLoadProgress>();
	payload->progress = std::clamp(progress, 0.0f, 1.0f);
	if (message) payload->message = message;
	(void)PostOwnedWindowMessage(msgWnd, WindowManager::kLoadProgressMessage, std::move(payload));
}

void App::StartLoadingModel(const std::filesystem::path& path)
{
	const std::filesystem::path normalizedPath = NormalizeModelPath(path);
	if (normalizedPath.empty() || !std::filesystem::exists(normalizedPath)) return;

	if (SettingsManager::HasPreset(m_baseDir, normalizedPath))
	{
		bool shouldLoadPreset = false;
		switch (SettingsManager::ResolvePresetMode(m_settingsData, m_baseDir, normalizedPath))
		{
			case PresetMode::AlwaysLoad:
				shouldLoadPreset = true;
				break;
			case PresetMode::NeverLoad:
				shouldLoadPreset = false;
				break;
			default:
				shouldLoadPreset = ShowPresetLoadDialog(normalizedPath);
				break;
		}

		if (shouldLoadPreset) ApplyPresetSettings(normalizedPath);
	}

	ProgressWindow& progress = EnsureProgressWindow();
	progress.Show();
	progress.SetMessage(L"読み込み開始...");
	progress.SetProgress(0.0f);

	CancelLoadingThread();
	m_isLoading = true;
	m_loadingModelPath = normalizedPath;

	// ワーカースレッド起動
	m_loadThread = std::jthread([this, normalizedPath](std::stop_token stopToken) {
		ScopedComApartment comApartment(COINIT_APARTMENTTHREADED);

		auto result = std::make_unique<mmd::app::ModelLoadResult>(
			mmd::app::LoadModelWithTextures(
				normalizedPath,
				*m_renderer,
				[this](float progress, const wchar_t* message)
				{
					PostLoadProgress(progress, message);
				},
				stopToken));

		// 完了通知 (常に結果構造体を渡し、成否は内容で判定)
		if (stopToken.stop_requested())
		{
			return;
		}

		if (HWND msgWnd = m_windowManager.MessageWindow())
		{
			(void)PostOwnedWindowMessage(msgWnd, WindowManager::kLoadCompleteMessage, std::move(result));
		}

	});
}

void App::CancelLoadingThread()
{
	if (m_loadThread.joinable())
	{
		m_loadThread.request_stop();
		m_loadThread.join();
	}
	m_isLoading = false;
}

void App::OnTimer()
{
	if (m_isLoading) return;

	Update();
	Render();
}

void App::Update()
{
	if (m_isLoading) return;

	UpdateLookAtTracking();

	if (m_animator)
	{
		m_animator->Update();
	}
}

void App::Render()
{
	if (m_isLoading) return;

	if (m_renderer && m_animator)
	{
		m_renderer->Render(*m_animator);
	}

	if (m_windowManager.IsGizmoVisible() && m_windowManager.GizmoWindow())
	{
		m_windowManager.PositionGizmoWindow();
		InvalidateRect(m_windowManager.GizmoWindow(), nullptr, FALSE);
	}
}

void App::ToggleGizmoWindow()
{
	m_windowManager.ToggleGizmoWindow();
}

void App::TogglePhysics()
{
	if (m_animator)
	{
		m_animator->TogglePhysics();
		BuildTrayMenu();
	}
}

void App::ToggleWindowManipulation()
{
	m_windowManager.ToggleWindowManipulationMode();
	BuildTrayMenu();
}

void App::MoveRenderWindowBy(int dx, int dy)
{
	HWND renderWnd = m_windowManager.RenderWindow();
	if (!renderWnd) return;

	RECT rc{};
	GetWindowRect(renderWnd, &rc);
	const int newX = rc.left + dx;
	const int newY = rc.top + dy;
	SetWindowPos(renderWnd, nullptr, newX, newY, 0, 0,
				 SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);

	if (m_windowManager.IsGizmoVisible() && m_windowManager.GizmoWindow())
	{
		m_windowManager.PositionGizmoWindow();
	}
}

void App::AddCameraRotation(float dx, float dy)
{
	if (m_renderer)
	{
		m_renderer->AddCameraRotation(dx, dy);
	}
}

void App::AdjustScale(float delta)
{
	if (m_renderer)
	{
		m_renderer->AdjustScale(delta);
		m_settingsData.light = m_renderer->GetLightSettings();
		SaveSettings();
	}
}

void App::AdjustBrightness(float delta)
{
	if (m_renderer)
	{
		m_renderer->AdjustBrightness(delta);
		m_settingsData.light = m_renderer->GetLightSettings();
		SaveSettings();
	}
}

void App::RenderGizmo()
{
	m_windowManager.RenderGizmo();
}

void App::InitRenderer()
{
	ProgressWindow& progress = EnsureProgressWindow();
	progress.Show();
	progress.SetProgress(0.0f);
	progress.SetMessage(L"レンダラーを初期化しています...");

	auto onProgress = [&progress](float p, const wchar_t* msg) {
		progress.SetProgress(p);
		if (msg && msg[0] != L'\0')
		{
			progress.SetMessage(msg);
		}
		};

	m_renderer = std::make_unique<DcompRenderer>();
	m_renderer->Initialize(m_windowManager.RenderWindow(), onProgress);
	m_windowManager.SetRenderer(m_renderer.get());
	m_windowManager.InstallRenderClickThrough();
	m_windowManager.ForceRenderTreeClickThrough();

	// 保存されたライト設定を適用
	m_renderer->SetLightSettings(m_settingsData.light);
	m_renderer->SetVSyncEnabled(m_settingsData.vsyncEnabled);

	progress.Hide();
}

void App::InitAnimator()
{
	m_animator = std::make_unique<MmdAnimator>();
	ApplyPhysicsSettings();
	ApplyAnimatorRuntimeSettings();
	LoadModelFromSettings();
}

UINT App::GetMaximumSupportedMsaaSampleCount() const
{
	return m_renderer ? m_renderer->GetMaximumSupportedMsaaSampleCount() : 1u;
}

void App::InitTray()
{
	m_tray = std::make_unique<TrayIcon>(m_windowManager.MessageWindow(), 1);
	m_tray->Show(L"MMDDesktopMascot");
	m_windowManager.SetTray(m_tray.get());
}

void App::BuildTrayMenu()
{
	if (!m_trayMenu) return;

	RefreshMotionList();

	TrayMenuModel model{};
	model.title = L"MMD Desktop Mascot";
	if (!m_settingsData.modelPath.empty())
	{
		model.subtitle = m_settingsData.modelPath.filename().wstring();
	}
	else
	{
		model.subtitle = L"モデル未読み込み";
	}

	const bool paused = (m_animator && m_animator->IsPaused());
	const bool physicsEnabled = (m_animator && m_animator->PhysicsEnabled());
	const bool autoBlink = (m_animator && m_animator->AutoBlinkEnabled());
	const bool breathing = (m_animator && m_animator->BreathingEnabled());

	auto& items = model.items;
	items.push_back(MakeActionItem(CMD_OPEN_SETTINGS, L"設定", L"描画・ライティング・プリセットを編集"));
	items.push_back(BuildThemeMenuItem(m_trayMenu->GetThemeId()));
	items.push_back(MakeToggleItem(
		CMD_TOGGLE_WINDOW_MANIP,
		L"ウィンドウ操作モード",
		L"Ctrl+Alt+R で切り替え",
		m_windowManager.IsWindowManipulationMode()));

	items.push_back(MakeSeparatorItem());
	items.push_back(MakeHeaderItem(L"再生コントロール"));
	items.push_back(MakeToggleItem(
		CMD_TOGGLE_PAUSE,
		paused ? L"再生を再開" : L"一時停止",
		L"モーションを一時停止 / 再開",
		paused));
	items.push_back(MakeToggleItem(
		CMD_TOGGLE_PHYSICS,
		L"物理シミュレーション",
		physicsEnabled ? L"有効" : L"無効",
		physicsEnabled));
	items.push_back(MakeToggleItem(
		CMD_TOGGLE_LOOKAT,
		L"視線追従",
		L"視線を注視点へ向けます",
		m_lookAtEnabled));
	items.push_back(MakeToggleItem(
		CMD_TOGGLE_AUTOBLINK,
		L"自動まばたき",
		L"自然なまばたきを付与",
		autoBlink));
	items.push_back(MakeToggleItem(
		CMD_TOGGLE_BREATH,
		L"呼吸モーション (待機時)",
		L"待機中の呼吸モーションを制御",
		breathing));
	items.push_back(MakeActionItem(
		CMD_STOP_MOTION,
		L"停止 (リセット)",
		L"再生を止めてポーズをリセット",
		true));

	items.push_back(MakeSeparatorItem());
	AppendMotionMenuItems(items, m_motionFiles);
	items.push_back(MakeActionItem(
		CMD_RELOAD_MOTIONS,
		L"モーション一覧を更新",
		L"フォルダーを再スキャンします"));
	items.push_back(MakeSeparatorItem());
	items.push_back(MakeActionItem(
		CMD_EXIT,
		L"終了",
		L"アプリケーションを終了します",
		true));

	m_trayMenu->SetModel(model);
}

void App::ShowTrayMenu(const POINT& anchor)
{
	if (!m_trayMenu) return;
	BuildTrayMenu();
	m_trayMenu->ShowAt(anchor);
}

void App::RefreshMotionList()
{
	m_motionFiles.clear();

	std::error_code ec;
	if (!std::filesystem::is_directory(m_motionsDir, ec) || ec)
	{
		return;
	}

	const auto options = std::filesystem::directory_options::skip_permission_denied;
	for (std::filesystem::directory_iterator it(m_motionsDir, options, ec), end; it != end; it.increment(ec))
	{
		if (ec)
		{
			break;
		}

		const auto& entry = *it;
		if (!entry.is_regular_file(ec))
		{
			ec.clear();
			continue;
		}

		auto ext = entry.path().extension().wstring();
		for (auto& c : ext) c = static_cast<wchar_t>(towlower(c));

		if (ext == L".vmd")
		{
			m_motionFiles.push_back(entry.path());
		}
	}

	std::sort(m_motionFiles.begin(), m_motionFiles.end());
}

std::chrono::steady_clock::duration App::ComputeTimerInterval() const
{
	if (m_settingsData.unlimitedFps &&
		!m_settingsData.limitFpsToMonitorRefreshRate)
	{
		return std::chrono::steady_clock::duration::zero();
	}

	if (m_settingsData.targetFps <= 0)
	{
		return std::chrono::milliseconds(kDefaultTimerMs);
	}

	int clampedFps = std::max(1, m_settingsData.targetFps);
	if (m_settingsData.limitFpsToMonitorRefreshRate)
	{
		const int refreshRateHz = GetRenderMonitorRefreshRateHz();
		if (refreshRateHz > 0)
		{
			clampedFps = m_settingsData.unlimitedFps
				? refreshRateHz
				: std::min(clampedFps, refreshRateHz);
		}
		else if (m_settingsData.unlimitedFps)
		{
			return std::chrono::steady_clock::duration::zero();
		}
	}

	const auto preciseInterval = std::chrono::duration<double>(1.0 / static_cast<double>(clampedFps));
	return std::chrono::duration_cast<std::chrono::steady_clock::duration>(preciseInterval);
}

void App::UpdateTimerInterval()
{
	m_frameInterval = ComputeTimerInterval();
}

void App::ApplyPresentationSettings()
{
	UpdateTimerInterval();
	if (m_renderer)
	{
		m_renderer->SetVSyncEnabled(m_settingsData.vsyncEnabled);
	}
}

int App::GetRenderMonitorRefreshRateHz() const
{
	const auto now = std::chrono::steady_clock::now();
	if ((now - m_lastMonitorRefreshRateQuery) < std::chrono::seconds(1))
	{
		return m_cachedMonitorRefreshRateHz;
	}

	m_lastMonitorRefreshRateQuery = now;
	m_cachedMonitorRefreshRateHz = 0;

	const HWND renderWnd = m_windowManager.RenderWindow();
	if (!renderWnd)
	{
		return 0;
	}

	const HMONITOR monitor = MonitorFromWindow(renderWnd, MONITOR_DEFAULTTONEAREST);
	if (!monitor)
	{
		return 0;
	}

	MONITORINFOEXW monitorInfo{};
	monitorInfo.cbSize = sizeof(monitorInfo);
	if (!GetMonitorInfoW(monitor, &monitorInfo))
	{
		return 0;
	}

	DEVMODEW displayMode{};
	displayMode.dmSize = sizeof(displayMode);
	if (!EnumDisplaySettingsW(monitorInfo.szDevice, ENUM_CURRENT_SETTINGS, &displayMode))
	{
		return 0;
	}

	const DWORD refreshRate = displayMode.dmDisplayFrequency;
	if (refreshRate >= 2 && refreshRate <= 1000)
	{
		m_cachedMonitorRefreshRateHz = static_cast<int>(refreshRate);
	}

	return m_cachedMonitorRefreshRateHz;
}

bool App::ShouldPersistPresetFor(const std::filesystem::path& modelPath) const
{
	if (modelPath.empty()) return false;

	bool shouldSavePreset = SettingsManager::HasPreset(m_baseDir, modelPath);
	const PresetMode mode = SettingsManager::ResolvePresetMode(m_settingsData, m_baseDir, modelPath);
	if (mode == PresetMode::AlwaysLoad)
	{
		shouldSavePreset = true;
	}
	return shouldSavePreset;
}

void App::PersistPresetIfNeeded()
{
	if (!ShouldPersistPresetFor(m_settingsData.modelPath))
	{
		return;
	}

	SettingsManager::SavePreset(
		m_baseDir,
		m_settingsData.modelPath,
		m_settingsData.light,
		m_settingsData.physics);
}

void App::SyncSettingsFromRuntime()
{
	m_windowManager.UpdateSettingsForRenderSize();

	if (m_renderer)
	{
		m_settingsData.light = m_renderer->GetLightSettings();
		m_settingsData.vsyncEnabled = m_renderer->IsVSyncEnabled();
	}

	if (m_trayMenu)
	{
		const TrayMenuThemeId theme = ClampTrayMenuThemeId(static_cast<int>(m_trayMenu->GetThemeId()));
		m_settingsData.trayMenuThemeId = static_cast<int>(theme);
	}

	m_settingsData.lookAtEnabled = m_lookAtEnabled;
	if (m_animator)
	{
		m_settingsData.autoBlinkEnabled = m_animator->AutoBlinkEnabled();
		m_settingsData.breathingEnabled = m_animator->BreathingEnabled();
	}
}

void App::ApplyAnimatorRuntimeSettings()
{
	m_lookAtEnabled = m_settingsData.lookAtEnabled;
	if (!m_animator)
	{
		return;
	}

	m_animator->SetLookAtState(m_lookAtEnabled, 0.0f, 0.0f);
	m_animator->SetAutoBlinkEnabled(m_settingsData.autoBlinkEnabled);
	m_animator->SetBreathingEnabled(m_settingsData.breathingEnabled);
}

void App::UpdateLookAtTracking()
{
	if (!m_lookAtEnabled || !m_animator || !m_renderer)
	{
		return;
	}

	using namespace DirectX;

	const XMFLOAT4X4 headMatrix = m_animator->GetBoneGlobalMatrix(L"頭");
	const XMVECTOR position = XMVectorSet(headMatrix._41, headMatrix._42, headMatrix._43, 1.0f);
	const XMVECTOR up = XMVector3Normalize(XMVectorSet(headMatrix._21, headMatrix._22, headMatrix._23, 0.0f));
	const XMFLOAT3 headScreen = m_renderer->ProjectToScreen(
		XMFLOAT3(XMVectorGetX(position), XMVectorGetY(position), XMVectorGetZ(position)));

	constexpr float kAxisLength = 5.0f;
	const XMVECTOR upProbe = XMVectorAdd(position, XMVectorScale(up, kAxisLength));
	const XMFLOAT3 upScreen = m_renderer->ProjectToScreen(
		XMFLOAT3(XMVectorGetX(upProbe), XMVectorGetY(upProbe), XMVectorGetZ(upProbe)));

	float upX = upScreen.x - headScreen.x;
	float upY = upScreen.y - headScreen.y;
	float rightX = 1.0f;
	float rightY = 0.0f;

	const float upLength = std::sqrt(upX * upX + upY * upY);
	if (upLength > 1.0e-4f)
	{
		upX /= upLength;
		upY /= upLength;
		rightX = -upY;
		rightY = upX;
	}

	POINT cursor{};
	GetCursorPos(&cursor);
	ScreenToClient(m_windowManager.RenderWindow(), &cursor);

	const float dx = static_cast<float>(cursor.x) - headScreen.x;
	const float dy = static_cast<float>(cursor.y) - headScreen.y;
	const float localX = dx * rightX + dy * rightY;
	const float localY = dx * upX + dy * upY;
	const float distance = std::max(upLength * 3.0f, 150.0f);

	const float targetYaw = -std::atan2(localX, distance);
	const float targetPitch = std::atan2(localY, distance);

	bool currentEnabled = false;
	float currentYaw = 0.0f;
	float currentPitch = 0.0f;
	m_animator->GetLookAtState(currentEnabled, currentYaw, currentPitch);

	constexpr float kSmoothing = 0.2f;
	const float nextYaw = currentYaw + (targetYaw - currentYaw) * kSmoothing;
	const float nextPitch = currentPitch + (targetPitch - currentPitch) * kSmoothing;
	m_animator->SetLookAtState(true, nextYaw, nextPitch);
}

void App::ApplySettings(const AppSettings& settings, bool persist)
{
	AppSettings nextSettings = settings;
	nextSettings.modelPath = NormalizeModelPath(nextSettings.modelPath);

	const std::filesystem::path previousModelPath = m_settingsData.modelPath;
	if (nextSettings.modelPath.empty())
	{
		nextSettings.modelPath = previousModelPath;
	}
	if (!nextSettings.modelPath.empty() &&
		nextSettings.modelPath != previousModelPath &&
		!std::filesystem::exists(nextSettings.modelPath))
	{
		MessageBoxW(
			m_windowManager.RenderWindow(),
			L"指定されたモデルファイルが見つかりません。モデル変更は適用しません。",
			L"エラー",
			MB_ICONERROR);
		nextSettings.modelPath = previousModelPath;
	}

	const bool modelChanged = (m_settingsData.modelPath != nextSettings.modelPath);
	const bool topmostChanged = (m_settingsData.alwaysOnTop != nextSettings.alwaysOnTop);
	const bool fpsChanged =
		(m_settingsData.targetFps != nextSettings.targetFps) ||
		(m_settingsData.unlimitedFps != nextSettings.unlimitedFps) ||
		(m_settingsData.limitFpsToMonitorRefreshRate != nextSettings.limitFpsToMonitorRefreshRate) ||
		(m_settingsData.vsyncEnabled != nextSettings.vsyncEnabled);
	const bool physicsChanged = (m_settingsData.physics != nextSettings.physics);
	const bool runtimeFeatureChanged =
		(m_settingsData.lookAtEnabled != nextSettings.lookAtEnabled) ||
		(m_settingsData.autoBlinkEnabled != nextSettings.autoBlinkEnabled) ||
		(m_settingsData.breathingEnabled != nextSettings.breathingEnabled);

	m_settingsData = nextSettings;

	if (modelChanged)
	{
		LoadModelFromSettings();
	}

	if (topmostChanged)
	{
		m_windowManager.ApplyTopmost(m_settingsData.alwaysOnTop);
	}

	if (fpsChanged)
	{
		ApplyPresentationSettings();
	}

	ApplyLightSettings();
	if (physicsChanged)
	{
		ApplyPhysicsSettings();
	}
	ApplyAnimatorRuntimeSettings();
	if (runtimeFeatureChanged)
	{
		BuildTrayMenu();
	}

	if (persist)
	{
		SaveSettings();
	}
}

void App::UpdateLiveLightSettings(const LightSettings& light)
{
	m_settingsData.light = light;
	ApplyLightSettings();
}

void App::UpdateLivePhysicsSettings(const PhysicsSettings& physics)
{
	if (m_settingsData.physics == physics)
	{
		return;
	}

	m_settingsData.physics = physics;
	ApplyPhysicsSettings();
}

void App::ApplyLightSettings()
{
	if (m_renderer)
	{
		m_renderer->SetLightSettings(m_settingsData.light);
	}
}

void App::ApplyPhysicsSettings()
{
	if (m_animator)
	{
		m_animator->SetPhysicsSettings(m_settingsData.physics);
	}
}

void App::SaveSettings()
{
	SyncSettingsFromRuntime();
	SettingsManager::Save(m_baseDir, m_settingsData);
	PersistPresetIfNeeded();
}

bool App::ApplyThemeCommand(UINT id)
{
	if (!m_trayMenu) return false;

	for (const auto& entry : kThemeCommands)
	{
		if (entry.commandId != id) continue;

		m_trayMenu->SetTheme(entry.theme);
		m_settingsData.trayMenuThemeId = static_cast<int>(entry.theme);
		SaveSettings();
		return true;
	}

	return false;
}

bool App::ApplyMotionCommand(UINT id)
{
	if (id < CMD_MOTION_BASE) return false;

	const size_t idx = id - CMD_MOTION_BASE;
	if (idx >= m_motionFiles.size() || !m_animator) return true;

	m_animator->LoadMotion(m_motionFiles[idx]);
	BuildTrayMenu();
	return true;
}

void App::ToggleLookAt()
{
	m_lookAtEnabled = !m_lookAtEnabled;
	if (m_animator)
	{
		m_animator->SetLookAtState(m_lookAtEnabled, 0.0f, 0.0f);
	}

	BuildTrayMenu();
	SaveSettings();
}

void App::ToggleAutoBlink()
{
	if (!m_animator)
	{
		return;
	}

	m_animator->SetAutoBlinkEnabled(!m_animator->AutoBlinkEnabled());
	BuildTrayMenu();
	SaveSettings();
}

void App::ToggleBreathing()
{
	if (!m_animator)
	{
		return;
	}

	m_animator->SetBreathingEnabled(!m_animator->BreathingEnabled());
	BuildTrayMenu();
	SaveSettings();
}

void App::OnTrayCommand(UINT id)
{
	if (m_trayMenu)
	{
		m_trayMenu->Hide();
	}

	const auto rebuildTrayMenu = [this]()
	{
		BuildTrayMenu();
	};
	const auto withAnimator = [this]<typename TFunc>(TFunc&& func) -> bool
	{
		if (!m_animator)
		{
			return false;
		}

		func(*m_animator);
		return true;
	};
	const auto closeApplication = [this]()
	{
		if (m_windowManager.RenderWindow())
		{
			PostMessageW(m_windowManager.RenderWindow(), WM_CLOSE, 0, 0);
		}
		else
		{
			PostQuitMessage(0);
		}
	};

	switch (id)
	{
		case CMD_OPEN_SETTINGS:
			EnsureSettingsWindow().Show();
			break;

		case CMD_RELOAD_MOTIONS:
			rebuildTrayMenu();
			break;

		case CMD_STOP_MOTION:
			(void)withAnimator([](MmdAnimator& animator) { animator.StopMotion(); });
			break;

		case CMD_TOGGLE_PAUSE:
			if (withAnimator([](MmdAnimator& animator) { animator.TogglePause(); }))
			{
				rebuildTrayMenu();
			}
			break;

		case CMD_TOGGLE_PHYSICS:
			TogglePhysics();
			break;

		case CMD_TOGGLE_LOOKAT:
			ToggleLookAt();
			break;

		case CMD_TOGGLE_AUTOBLINK:
			ToggleAutoBlink();
			break;

		case CMD_TOGGLE_BREATH:
			ToggleBreathing();
			break;

		case CMD_TOGGLE_WINDOW_MANIP:
			ToggleWindowManipulation();
			break;

		case CMD_THEME_DARK:
		case CMD_THEME_LIGHT:
		case CMD_THEME_MIDNIGHT:
		case CMD_THEME_SAKURA:
		case CMD_THEME_SOLARIZED:
		case CMD_THEME_HIGHCONTRAST:
			(void)ApplyThemeCommand(id);
			break;

		case CMD_EXIT:
			closeApplication();
			break;

		default:
			(void)ApplyMotionCommand(id);
			break;
	}
}

void App::OnLoadProgress(WPARAM, LPARAM lParam)
{
	std::unique_ptr<ModelLoadProgress> progress(reinterpret_cast<ModelLoadProgress*>(lParam));
	if (!progress || !m_progress) return;

	if (!progress->message.empty())
	{
		m_progress->SetMessage(progress->message);
	}
	m_progress->SetProgress(progress->progress);
}

void App::OnLoadComplete(WPARAM, LPARAM lParam)
{
	std::unique_ptr<mmd::app::ModelLoadResult> result(reinterpret_cast<mmd::app::ModelLoadResult*>(lParam));

	if (result && result->model)
	{
		// アニメーターにセット
		if (m_animator)
		{
			m_animator->SetModel(std::move(result->model));
			m_animator->Update();
		}
		m_loadedModelPath = m_loadingModelPath.empty() ? m_settingsData.modelPath : m_loadingModelPath;
	}
	else
	{
		const std::wstring message = (result && !result->errorMessage.empty())
			? result->errorMessage
			: std::wstring(L"モデルの読み込みに失敗しました。");

		MessageBoxW(m_windowManager.RenderWindow(), message.c_str(), L"エラー", MB_ICONERROR);

		m_settingsData.modelPath = m_loadedModelPath;
		SettingsManager::Save(m_baseDir, m_settingsData);
		if (m_settings)
		{
			m_settings->Refresh();
		}
	}

	if (m_progress)
	{
		m_progress->Hide();
	}
	m_loadingModelPath.clear();
	m_isLoading = false; // 描画再開

	InvalidateRect(m_windowManager.RenderWindow(), nullptr, FALSE);
}
