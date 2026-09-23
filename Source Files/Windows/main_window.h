#pragma once

#include "Core/diagnostic_report.h"
#include "Core/numeric_edit_history.h"
#include "Core/settings.h"
#include "Core/settings_history.h"
#include "Windows/controller.h"
#include "Windows/click_position_indicator.h"
#include "Windows/hotkey_thread.h"
#include "Windows/process_priority_manager.h"
#include "Windows/profile_manager.h"
#include "Windows/profile_store.h"
#include "Windows/run_feedback_presenter.h"
#include "Windows/safety_shield.h"
#include "Windows/settings_store.h"
#include "Windows/target_window.h"
#include "Windows/target_window_picker.h"
#include "Windows/tooltip_manager.h"
#include "Windows/ui_layout.h"
#include "Windows/ui_presentation_state.h"
#include "Windows/ui_theme.h"
#include "Windows/win32_raii.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace vectorclick::win {

class MainWindow {
public:
    explicit MainWindow(
        HINSTANCE instance,
        std::optional<TargetWindowIdentity> startup_target_restore = std::nullopt,
        std::optional<int> startup_page_restore = std::nullopt,
        std::optional<core::RunSettings> startup_settings_restore = std::nullopt,
        std::optional<std::wstring> startup_profile_restore = std::nullopt,
        bool startup_settings_restore_invalid = false);
    ~MainWindow();

    MainWindow(const MainWindow&) = delete;
    MainWindow& operator=(const MainWindow&) = delete;

    [[nodiscard]] bool Create();
    [[nodiscard]] HWND Handle() const noexcept { return window_; }
    [[nodiscard]] bool PreTranslateMessage(const MSG& message);

private:
    enum ControlId : int {
        BasicPageTab = 90,
        AdvancedPageTab,
        AboutPageTab,
        ActionTypeCombo = 100,
        ActionPatternCombo,
        BurstCountEdit,
        ActionSpacingEdit,
        BackendCombo,
        RandomIntervalCheck,
        RandomIntervalStyleCombo,
        DownDurationBehaviorCombo,
        MinimumIntervalEdit,
        MaximumIntervalEdit,
        IntervalMinutesEdit,
        IntervalSecondsEdit,
        ButtonDownMinutesEdit,
        ButtonDownSecondsEdit,
        ActionSpacingMinutesEdit,
        ActionSpacingSecondsEdit,
        MinimumIntervalMinutesEdit,
        MinimumIntervalSecondsEdit,
        MaximumIntervalMinutesEdit,
        MaximumIntervalSecondsEdit,
        SelectTargetButton,
        ClearTargetButton,
        BackgroundInputCheck,
        MouseButtonCombo,
        GeneratedKeyCombo,
        IntervalEdit,
        ButtonDownEdit,
        CurrentCursorRadio,
        FixedPositionRadio,
        FixedXEdit,
        FixedYEdit,
        CapturePositionButton,
        ClickPositionIndicatorCheck,
        UnlimitedRadio,
        LimitedRadio,
        RepeatCountEdit,
        RunTimeHoursEdit,
        RunTimeMinutesEdit,
        RunTimeSecondsEdit,
        StartHotkeyCombo,
        EmergencyHotkeyCombo,
        DiagnosticsCheck,
        SafetyShieldCheck,
        ForceExitOnEmergencyStopCheck,
        CaptureExclusionCheck,
        RememberSettingsCheck,
        ProcessPriorityCombo,
        StartButton,
        StopButton,
        EmergencyButton,
        AdminButton,
        StatusText,
        RateText,
        DiagnosticsText,
        OfficialDownloadsButton,
        SourceCodeButton,
        ReportBugButton,
        CopySupportEmailButton,
        ViewLicenseButton,
        CopyDiagnosticReportButton,
        // Scheduling and feedback controls use a separate ID range so they can
        // never collide with the established RateText / DiagnosticsText IDs.
        TimingWorkerPriorityCombo = 200,
        HotkeyControlPriorityCombo,
        TimingWorkerQosCombo,
        WindowsNotificationCombo,
        SystemSoundCombo,
        RunningIndicatorCheck,
        KeepOnTopCheck,
        ImportSettingsButton,
        ProfileCombo,
        ManageProfilesButton,
    };
    static_assert(TimingWorkerPriorityCombo > CopyDiagnosticReportButton);
    static_assert(HotkeyControlPriorityCombo != TimingWorkerPriorityCombo);
    static_assert(TimingWorkerQosCombo != HotkeyControlPriorityCombo);
    static_assert(WindowsNotificationCombo != TimingWorkerQosCombo);
    static_assert(SystemSoundCombo != WindowsNotificationCombo);
    static_assert(RunningIndicatorCheck != SystemSoundCombo);
    static_assert(KeepOnTopCheck != RunningIndicatorCheck);
    static_assert(ImportSettingsButton != KeepOnTopCheck);
    static_assert(ProfileCombo != ImportSettingsButton);
    static_assert(ManageProfilesButton != ProfileCombo);

    enum class SettingsHistoryOperation : unsigned char {
        None,
        Undo,
        Redo,
    };

    enum class HotkeyIndicatorState : unsigned char {
        Unknown,
        Registering,
        Active,
        Unavailable,
    };

    struct DurationEditSet final {
        HWND minutes{};
        HWND seconds{};
        HWND milliseconds{};
    };

    struct RunTimeLimitEditSet final {
        HWND hours{};
        HWND minutes{};
        HWND seconds{};
    };

    // Native message routing and subclass entry points.
    static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM w_param, LPARAM l_param);
    static LRESULT CALLBACK ContentHostProc(HWND window, UINT message, WPARAM w_param, LPARAM l_param);
    static LRESULT CALLBACK ResizeOverlayProc(HWND window, UINT message, WPARAM w_param, LPARAM l_param);
    static LRESULT CALLBACK AdvancedScrollSnapshotProc(HWND window, UINT message, WPARAM w_param, LPARAM l_param);
    static LRESULT CALLBACK AdvancedScrollbarProc(HWND window, UINT message, WPARAM w_param, LPARAM l_param);
    static LRESULT CALLBACK ComboPopupProc(HWND window, UINT message, WPARAM w_param, LPARAM l_param);
    static LRESULT CALLBACK ChoiceControlProc(HWND window, UINT message, WPARAM w_param, LPARAM l_param);
    static LRESULT CALLBACK ComboSubclassProc(HWND window,
                                              UINT message,
                                              WPARAM w_param,
                                              LPARAM l_param,
                                              UINT_PTR subclass_id,
                                              DWORD_PTR reference_data);
    static LRESULT CALLBACK AdvancedFocusSubclassProc(HWND window,
                                                       UINT message,
                                                       WPARAM w_param,
                                                       LPARAM l_param,
                                                       UINT_PTR subclass_id,
                                                       DWORD_PTR reference_data);
    static LRESULT CALLBACK EditSubclassProc(HWND window,
                                             UINT message,
                                             WPARAM w_param,
                                             LPARAM l_param,
                                             UINT_PTR subclass_id,
                                             DWORD_PTR reference_data);
    LRESULT HandleMessage(UINT message, WPARAM w_param, LPARAM l_param);

    // Window lifecycle, layout, retained surfaces, scrolling, and footer
    // presentation.
    bool OnCreate();
    void PresentAlreadyRunningNotice();
    void OnDestroy();
    void CreateControls();
    void LayoutControls(bool redraw = true);
    [[nodiscard]] layout::LayoutMetrics BuildLayoutMetrics() const;
    void SelectPage(int index);
    void PositionContentHost(bool clean_repaint);
    [[nodiscard]] int CurrentAdvancedViewportHeightLogical() const noexcept;
    [[nodiscard]] int AdvancedMaximumScrollLogical() const noexcept;
    [[nodiscard]] RECT AdvancedScrollbarTrackRect() const noexcept;
    [[nodiscard]] RECT AdvancedScrollbarThumbRect(
        bool stable_snapshot = false) const noexcept;
    [[nodiscard]] bool SetAdvancedScrollOffset(int offset_logical,
                                               bool refresh_move_cover = true);
    [[nodiscard]] bool BeginAdvancedScrollSnapshot();
    void UpdateAdvancedScrollSnapshot() noexcept;
    void FinishAdvancedScrollSnapshot(bool refresh_move_cover = true) noexcept;
    void HideAdvancedScrollSnapshot() noexcept;
    void ScrollAdvancedBy(int delta_logical);
    [[nodiscard]] bool ScrollAdvancedWheel(WPARAM w_param, LPARAM l_param);
    void EnsureAdvancedControlVisible(HWND control);
    void UpdateAdvancedScrollbar(bool redraw = true) noexcept;
    [[nodiscard]] bool BeginResizeOverlay(const RECT* proposed_outer = nullptr,
                                          WPARAM sizing_edge = 0,
                                          bool allow_focus_only_patch = false);
    [[nodiscard]] bool UpdateResizeOverlayForOuterRect(const RECT& outer,
                                                       WPARAM sizing_edge = 0) noexcept;
    [[nodiscard]] bool UpdateResizeOverlayForCurrentClient() noexcept;
    [[nodiscard]] bool PresentResizeOverlay(const RECT& outer,
                                            const RECT& client) noexcept;
    [[nodiscard]] bool EnsureResizeOverlayGuard() noexcept;
    void HideResizeOverlayGuard() noexcept;
    [[nodiscard]] bool PrepareAdvancedResizePreview();
    [[nodiscard]] bool PaintAdvancedResizePreview(HDC target,
                                                   int overlay_width,
                                                   int overlay_height,
                                                   int client_screen_top) noexcept;
    void ClearResizePreviewState() noexcept;
    void FinishResizeOverlay() noexcept;
    void HideResizeOverlay() noexcept;
    void Relayout(bool redraw = true);
    [[nodiscard]] layout::Page CurrentLayoutPage() const noexcept;
    [[nodiscard]] int CurrentContentHeightLogical() const noexcept;
    [[nodiscard]] int MeasureStatusHeightLogical() const;
    void UpdateStatusAreaLayout(bool resize_window = true);
    [[nodiscard]] UINT_PTR StartGeneratedTimer(UINT_PTR& active_timer_id,
                                                UINT milliseconds) noexcept;
    void StopGeneratedTimer(UINT_PTR& active_timer_id) noexcept;
    void QueueStatusAreaLayout() noexcept;
    void SetStatusPresentation(const StatusPresentation& presentation,
                               bool force_full_repaint = false);
    void UpdateVisibleStatusTooltip();
    void ShowInputMethodTargetRoutingStatus();
    void SetDiagnosticsText(const std::wstring& text);

    // Owner-draw, control construction, custom combo, and key-capture helpers.
    [[nodiscard]] bool IsCompactTextControl(HWND control) const;
    [[nodiscard]] bool IsGroupControl(HWND control) const;
    [[nodiscard]] bool IsCheckControl(HWND control) const;
    [[nodiscard]] bool IsRadioControl(HWND control) const;
    [[nodiscard]] bool IsPushButtonControl(HWND control) const;
    void DrawCompactText(const DRAWITEMSTRUCT& draw) const;
    void DrawGroupControl(const DRAWITEMSTRUCT& draw) const;
    void DrawChoiceControl(const DRAWITEMSTRUCT& draw, bool radio) const;
    void DrawPushButtonControl(const DRAWITEMSTRUCT& draw) const;
    void DrawTabControl(const DRAWITEMSTRUCT& draw) const;
    void DrawComboItem(const DRAWITEMSTRUCT& draw) const;
    void DrawPrintedCombo(HWND combo, HDC destination) const;
    void DrawPrintedEdit(HWND edit, HDC destination) const;
    void UpdateKeySelectorTooltip(HWND combo);
    void UpdateKeySelectorTooltips();
    [[nodiscard]] std::wstring SelectedComboText(HWND combo) const;
    [[nodiscard]] bool IsComboSelectionClipped(HWND combo,
                                               std::wstring_view text) const;
    [[nodiscard]] ui::Glyph GlyphForControl(HWND control) const noexcept;
    void InvalidateControl(HWND control) const noexcept;
    [[nodiscard]] int PreferredControlWidth(HWND control, int extra_logical_width, int minimum_logical_width) const;
    void ApplyFont(HWND control) const;
    void RecreateFontForDpi();
    HWND MakeControl(const wchar_t* class_name,
                     const wchar_t* text,
                     DWORD style,
                     DWORD extended_style,
                     int id);
    HWND MakeChoice(const wchar_t* text, bool radio, DWORD style, int id);
    HWND MakeLabel(const wchar_t* text);
    HWND MakeGroup(const wchar_t* text);
    [[nodiscard]] bool OpenComboPopup(HWND combo);
    void CloseComboPopup(bool commit_selection);
    void CommitComboPopupSelection();
    void ApplyInputTypeSelectionCovered(int selection);
    void MoveComboPopupHighlight(int index);
    void ScrollComboPopup(int row_delta);
    [[nodiscard]] bool RenderComboPopup();
    [[nodiscard]] int ComboPopupItemAtPoint(POINT point) const noexcept;
    [[nodiscard]] RECT ComboPopupScrollbarGutter() const noexcept;
    [[nodiscard]] RECT ComboPopupScrollbarTrack() const noexcept;
    [[nodiscard]] RECT ComboPopupScrollbarThumb() const noexcept;
    [[nodiscard]] bool IsComboPopupOpenFor(HWND combo) const noexcept;
    void NotifyCombo(HWND combo, int notification) const noexcept;
    void FinishStartupPresentation();
    void BeginKeyCapture(HWND combo);
    void EndKeyCapture();
    [[nodiscard]] bool CaptureKeyForCombo(HWND combo, UINT virtual_key, std::uint16_t modifiers = 0);
    [[nodiscard]] bool IsKeyCaptureCombo(HWND combo) const noexcept;

    // Numeric field editing and local edit-history behavior.
    void StepNumericEdit(HWND edit, int direction);
    void UpdateNumericEditFormatting(HWND edit) const noexcept;
    [[nodiscard]] bool IsDurationMinutesEdit(HWND edit) const noexcept;
    [[nodiscard]] bool IsDurationSecondsEdit(HWND edit) const noexcept;
    [[nodiscard]] bool IsDurationMillisecondsEdit(HWND edit) const noexcept;
    [[nodiscard]] bool IsDurationEdit(HWND edit) const noexcept;
    [[nodiscard]] bool IsRunTimeLimitEdit(HWND edit) const noexcept;
    [[nodiscard]] bool TryReadDurationEdits(
        const DurationEditSet& edits,
        core::DurationComponents& components,
        std::uint64_t& total_microseconds) const noexcept;
    void SetDurationEdits(const DurationEditSet& edits,
                          const core::DurationComponents& components);
    [[nodiscard]] bool TryReadRunTimeLimitEdits(
        core::RunTimeLimitComponents& components,
        std::uint64_t& total_microseconds) const noexcept;
    void SetRunTimeLimitEdits(const core::RunTimeLimitComponents& components);
    [[nodiscard]] RunTimeLimitEditSet RunTimeLimitEdits() const noexcept;
    [[nodiscard]] DurationEditSet IntervalDurationEdits() const noexcept;
    [[nodiscard]] DurationEditSet ButtonDownDurationEdits() const noexcept;
    [[nodiscard]] DurationEditSet ActionSpacingDurationEdits() const noexcept;
    [[nodiscard]] DurationEditSet MinimumIntervalDurationEdits() const noexcept;
    [[nodiscard]] DurationEditSet MaximumIntervalDurationEdits() const noexcept;
    void QueueNumericPresentationRefresh(bool update_rate);
    void FlushNumericPresentationRefresh();
    void BeginNumericEditHistorySession(HWND control);
    void EndNumericEditHistorySession(HWND control) noexcept;
    void NoteNumericEditHistoryTextChanged(HWND control);
    void SynchronizeNumericEditHistorySession();
    [[nodiscard]] bool HandleNumericEditHistoryShortcut(HWND control, bool redo);
    void ApplyNumericEditHistoryText(HWND control, const std::wstring& text);

    // Settings selection, scheduling controls, run feedback, and Settings
    // Undo / Redo.
    [[nodiscard]] bool ReadSettings(core::RunSettings& settings, std::wstring& error) const;
    void ApplySettings(const core::RunSettings& settings);
    [[nodiscard]] core::ProcessPriorityMode SelectedProcessPriorityMode() const noexcept;
    void SelectProcessPriorityMode(core::ProcessPriorityMode mode) noexcept;
    void HandleProcessPrioritySelection();
    [[nodiscard]] core::TimingWorkerPriorityMode SelectedTimingWorkerPriorityMode() const noexcept;
    void SelectTimingWorkerPriorityMode(core::TimingWorkerPriorityMode mode) noexcept;
    void HandleTimingWorkerPrioritySelection();
    [[nodiscard]] core::HotkeyControlPriorityMode SelectedHotkeyControlPriorityMode() const noexcept;
    void SelectHotkeyControlPriorityMode(core::HotkeyControlPriorityMode mode) noexcept;
    void HandleHotkeyControlPrioritySelection();
    [[nodiscard]] core::TimingWorkerQosMode SelectedTimingWorkerQosMode() const noexcept;
    void SelectTimingWorkerQosMode(core::TimingWorkerQosMode mode) noexcept;
    void HandleTimingWorkerQosSelection();
    [[nodiscard]] core::RunFeedbackMode SelectedRunFeedbackMode(HWND combo) const noexcept;
    void SelectRunFeedbackMode(HWND combo, core::RunFeedbackMode mode) noexcept;
    void BeginRunFeedback();
    void HandleRunFeedbackTerminal(EngineState state,
                                   bool emergency_context,
                                   bool release_warning = false);
    void PresentRunFeedbackNotification(std::wstring_view title,
                                        std::wstring_view body);
    [[nodiscard]] bool BeginProcessPriorityActive();
    void EndProcessPriorityActive(bool show_error = false);
    void DisableActiveRunHotkeyGuard() noexcept;
    [[nodiscard]] bool IsNumericEditControl(HWND control) const noexcept;
    [[nodiscard]] core::RunSettings NormalizeSettingsHistoryState(
        core::RunSettings settings) const noexcept;
    [[nodiscard]] bool CaptureSettingsHistoryState(
        core::RunSettings& settings) const;
    void CommitSettingsHistoryFromControls();
    void SynchronizeSettingsHistoryFromControls();
    [[nodiscard]] bool CanUseSettingsHistory() const noexcept;
    [[nodiscard]] bool HandleSettingsHistoryShortcut(bool redo);
    [[nodiscard]] bool RestoreCurrentSettingsHistoryState();
    [[nodiscard]] bool ApplySettingsHistoryState(
        const core::RunSettings& settings);
    [[nodiscard]] bool BeginSettingsHistoryHotkeyTransaction(
        const core::RunSettings& target,
        SettingsHistoryOperation operation);

    // Input-dependent presentation, retained movement frames, and live status
    // state.
    void UpdateActionControls(bool redraw = true);
    void RedrawBasicTopHost();
    void QueueInputTypeUpdate() noexcept;
    void ApplyInputTypeUpdate();
    [[nodiscard]] bool CaptureBasicTopSnapshot();
    [[nodiscard]] bool ShowBasicTopSnapshot();
    void HideBasicTopSnapshot() noexcept;
    [[nodiscard]] bool CanCaptureMoveCoverFromScreen() const noexcept;
    [[nodiscard]] bool CaptureMoveCoverFromScreen();
    [[nodiscard]] bool CaptureMoveCoverFromWindow();
    [[nodiscard]] HWND CurrentMoveCoverPointerControl() const noexcept;
    [[nodiscard]] bool TryPatchMoveCoverForFocusOnlyTabSwitch() noexcept;
    [[nodiscard]] bool RefreshMoveCoverForRestoredCaptionDrag();
    [[nodiscard]] bool HasReusableMoveCover() const noexcept;
    [[nodiscard]] bool CopyMoveCoverRegion(HWND source, UniqueGdiObject& destination) const noexcept;
    void MarkMoveCoverPresentationDirty(bool queue_refresh = true) noexcept;
    void QueueMoveCoverRefresh() noexcept;
    void RefreshMoveCoverCache();
    [[nodiscard]] bool ShowMoveCover();
    void RetireMoveCoverAfterResize() noexcept;
    void RetireMoveCoverAfterMove() noexcept;
    void HideMoveCover() noexcept;
    void UpdateActionPatternControls();
    [[nodiscard]] RatePresentationInput CaptureRatePresentationInput() const;
    [[nodiscard]] PresentationSnapshot BuildPresentationSnapshot(bool evaluate_availability) const;
    void ApplyPresentationSnapshot(const PresentationSnapshot& snapshot,
                                   bool update_rate,
                                   bool update_availability,
                                   bool force_status,
                                   bool update_diagnostics);
    void RefreshPresentation(bool update_rate,
                             bool update_availability,
                             bool force_status,
                             bool update_diagnostics);
    void UpdateRateLabel();
    void RefreshEnabledState();
    void RefreshStartAvailability();
    void UpdateEnabledState(EngineState state);
    void QueueSurfaceRepair() noexcept;
    void RepairVisibleSurfaces();
    void UpdateStatus(EngineState state, std::uint64_t completed_cycles);

    // Hotkeys, privacy / window options, profiles, imports, and settings
    // persistence.
    void ConfigureHotkeysFromControls(bool show_errors);
    void HandleHotkeyRegistrationResult(HotkeyRegistrationResult result);
    void RestoreConfirmedHotkeyControls();
    void UpdateSafetyHotkeyIndicator();
    [[nodiscard]] bool SafetyHotkeysReady() const noexcept;
    [[nodiscard]] bool CleanupRequired() const noexcept;
    [[nodiscard]] std::wstring SafetyHotkeyUnavailableReason() const;
    void ValidateGeneratedKeySelection(bool show_errors);
    void HandleCaptureExclusionToggle();
    void HandleKeepOnTopToggle();
    [[nodiscard]] bool SetMainWindowTopmost(bool topmost) noexcept;
    void HandleRememberSettingsToggle();
    [[nodiscard]] bool CanImportSettings() const noexcept;
    void ImportSettingsFromFile();
    [[nodiscard]] bool CanManageProfiles() const noexcept;
    void RefreshProfileSelector();
    void UpdateProfileSelectorPresentation();
    void UpdateProfileSelectorDroppedWidth();
    void UpdateProfileSelectorTooltip();
    void HandleProfileSelection();
    void OpenProfileManager();
    [[nodiscard]] bool BeginProfileLoad(const ProfileInfo& profile);
    [[nodiscard]] bool CaptureCurrentProfileSettings(core::RunSettings& settings,
                                                     std::wstring& error) const;
    void OnProfilesChanged();
    [[nodiscard]] bool BeginSettingsImportHotkeyTransaction(
        const core::RunSettings& target);
    [[nodiscard]] bool ApplyImportedSettings(core::RunSettings settings);
    void FinishSettingsImport(bool imported);
    void ScheduleSettingsSave();
    void CancelScheduledSettingsSave() noexcept;
    void SaveSettingsIfEnabled(bool show_errors = true);

    // Diagnostics, run control, emergency recovery, shutdown, elevation, and
    // targets.
    [[nodiscard]] std::wstring BuildDiagnosticReport() const;
    void CopyDiagnosticReport();
    void StartFromControls();
    void StopNormally();
    void EmergencyStop();
    void RetryEmergencyCleanup();
    void BeginForceStopAndExit(bool watchdog_already_armed = false) noexcept;
    void QueueEmergencyUi() noexcept;
    void AdvanceEmergencyCompletion();
    void TryClearEmergencyLatch();
    void RemoveQueuedStartRequests() noexcept;
    void ResetClickPositionIndicator() noexcept;
    void ShowSafetyShield();
    [[nodiscard]] bool PrepareSafeShutdown(bool save_settings,
                                           bool show_settings_errors);
    void PresentShutdownRecovery();
    void CancelPendingSessionEnd() noexcept;
    void RestartElevated();
    [[nodiscard]] bool ConfirmAdministratorTargetAccess();
    void RestoreStartupTarget();
    void BeginPositionCapture();
    void SetCapturePositionButtonText(const std::wstring& text);
    void FinishPositionCapture();
    void CancelPositionCapture();
    void ChooseTargetWindow();
    void ClearTargetWindow();
    [[nodiscard]] bool RecoverTargetWindowIfAvailable(bool present_status);
    void UpdateTargetDisplay();

    // Small control-value adapters shared by the higher-level handlers above.
    [[nodiscard]] core::ActionPattern SelectedActionPattern() const noexcept;
    [[nodiscard]] core::HotkeyBinding SelectedHotkey(HWND combo) const;
    bool SelectHotkey(HWND combo, const core::HotkeyBinding& binding);
    [[nodiscard]] core::HotkeyBinding SelectedGeneratedKey() const;
    void SelectGeneratedKey(const core::HotkeyBinding& binding);
    [[nodiscard]] static bool ParseSignedCoordinate(HWND edit, std::int32_t& value);
    [[nodiscard]] static bool IsChecked(HWND control) noexcept;
    void SetChecked(HWND control, bool checked) noexcept;
    void SetRadioPair(HWND first, HWND second, HWND selected) noexcept;
    [[nodiscard]] static bool IsPointOnVirtualDesktop(std::int32_t x, std::int32_t y) noexcept;
    [[nodiscard]] static std::wstring GetControlText(HWND control);
    void SetControlText(HWND control, const std::wstring& text);

    // Window identity, shared presentation services, and long-lived helpers.
    HINSTANCE instance_{};
    bool running_as_administrator_{};
    std::optional<TargetWindowIdentity> startup_target_restore_;
    std::optional<core::RunSettings> startup_settings_restore_;
    std::optional<std::wstring> startup_profile_restore_;
    bool startup_settings_restore_invalid_{};
    HWND window_{};
    UINT dpi_{96};
    bool single_instance_notice_pending_{};
    bool single_instance_notice_presenting_{};

    UniqueGdiObject window_background_brush_;
    UniqueGdiObject content_background_brush_;
    UniqueGdiObject font_;
    UniqueGdiObject bold_font_;
    UniqueGdiObject action_font_;
    TooltipManager tooltips_;
    SafetyShield safety_shield_;
    ClickPositionIndicator click_position_indicator_;
    SettingsStore settings_store_{L"VectorClick"};
    ProfileStore profile_store_{L"VectorClick"};
    std::unique_ptr<ProfileManagerWindow> profile_manager_;
    bool profile_manager_open_{};
    ProcessPriorityManager process_priority_manager_{};
    RunFeedbackPresenter run_feedback_presenter_{};

    // Input engine and dedicated hotkey-thread coordination.
    std::unique_ptr<Controller> controller_;
    std::unique_ptr<HotkeyThread> hotkey_thread_;
    std::mutex hotkey_registration_result_mutex_;
    std::optional<HotkeyRegistrationResult> pending_hotkey_registration_result_;

    // Settings, profiles, diagnostics, and edit-history state.
    core::RunSettings settings_cache_{};
    core::SettingsHistory settings_history_;
    SettingsHistoryOperation settings_history_operation_{SettingsHistoryOperation::None};
    std::optional<core::RunSettings> pending_settings_history_target_;
    bool settings_history_transaction_pending_{};
    bool settings_import_picker_open_{};
    bool settings_import_transaction_pending_{};
    bool settings_import_source_is_canonical_{};
    bool settings_import_is_profile_load_{};
    std::optional<core::RunSettings> pending_settings_import_target_;
    std::optional<ProfileInfo> pending_profile_load_info_;
    std::optional<core::RunSettings> pending_profile_load_saved_settings_;
    std::optional<ProfileInfo> active_profile_info_;
    std::optional<core::RunSettings> active_profile_saved_settings_;
    std::vector<ProfileInfo> profile_selector_profiles_;
    bool refreshing_profile_selector_{};
    std::optional<core::RunSettings> last_saved_settings_;
    std::optional<std::wstring> last_saved_profile_id_;
    core::DiagnosticRun last_run_diagnostic_{};
    std::vector<std::pair<std::wstring, core::HotkeyBinding>> hotkey_choices_;
    std::vector<std::pair<std::wstring, core::HotkeyBinding>> generated_key_choices_;
    std::vector<HWND> labels_;
    bool suppress_control_events_{false};
    bool suppress_advanced_focus_scroll_{false};
    bool numeric_text_update_in_progress_{false};
    core::NumericEditHistory numeric_edit_history_;
    HWND numeric_edit_history_control_{};
    bool numeric_edit_history_update_in_progress_{};
    bool numeric_presentation_refresh_posted_{};
    bool numeric_presentation_refresh_pending_{};
    bool numeric_rate_refresh_pending_{};

    // Timers, target state, resize / movement lifecycle, and queued
    // presentation work.
    UINT_PTR next_generated_timer_id_{0x1000};
    UINT_PTR position_capture_timer_id_{};
    UINT_PTR settings_save_timer_id_{};
    UINT_PTR status_layout_timer_id_{};
    UINT_PTR support_email_copied_timer_id_{};
    UINT_PTR diagnostic_report_copied_timer_id_{};
    UINT_PTR run_notification_cleanup_timer_id_{};
    UINT_PTR advanced_scroll_settle_timer_id_{};
    int position_capture_seconds_remaining_{};
    TargetWindowInfo target_window_{};
    std::optional<TargetWindowIdentity> elevated_target_continue_;
    bool startup_target_restored_{};
    std::wstring startup_target_restore_error_;
    bool target_picker_open_{};
    bool interactive_resize_{};
    bool interactive_sizing_{};
    bool programmatic_resize_overlay_{};
    bool resize_overlay_finish_posted_{};
    int status_height_logical_{40};
    int automatic_status_window_growth_logical_{};
    bool status_layout_update_in_progress_{};
    int size_move_client_width_{};
    int size_move_client_height_{};
    bool settings_save_pending_{};
    bool run_feedback_active_{};
    bool click_position_indicator_available_{};
    bool surface_repair_posted_{};
    bool startup_presentation_complete_{};
    bool move_cover_pending_exact_screen_refresh_{};
    bool move_cover_exact_refresh_deferred_{};
    bool maximized_caption_drag_pending_{};
    bool restored_caption_drag_active_{};
    bool caption_move_active_{};
    bool window_was_maximized_{};
    bool restored_down_exact_move_pending_{};
    bool restored_down_caption_drag_pending_{};
    std::wstring last_target_summary_;
    std::wstring last_target_details_;
    std::wstring last_target_recovery_error_;
    bool target_validity_initialized_{};
    bool last_target_valid_{};

    // Cross-thread emergency, safety-hotkey, and input-release coordination.
    std::atomic<bool> emergency_latched_{false};
    std::atomic<bool> safety_shield_requested_{false};
    std::atomic<bool> force_exit_on_emergency_stop_requested_{false};
    std::atomic<bool> force_exit_in_progress_{false};
    std::atomic<bool> shutdown_in_progress_{false};
    std::atomic<bool> emergency_ui_pending_{false};
    std::atomic<bool> emergency_ui_presented_{false};
    std::atomic<bool> emergency_cleanup_complete_{false};
    std::atomic<bool> emergency_release_failed_{false};
    std::atomic<std::uint64_t> latest_click_indicator_point_{0};
    std::atomic<bool> click_indicator_update_pending_{false};
    std::atomic<bool> key_capture_active_{false};
    std::atomic<std::uint32_t> hotkey_toggle_generation_{0};
    std::atomic<core::DiagnosticRunOutcome> run_stop_intent_{
        core::DiagnosticRunOutcome::EndedUnclassified};
    std::atomic<std::uint16_t> active_start_hotkey_key_{0};
    std::atomic<std::uint16_t> active_start_hotkey_modifiers_{0};
    std::atomic<std::uint16_t> active_emergency_hotkey_key_{0};
    std::atomic<std::uint16_t> active_emergency_hotkey_modifiers_{0};
    bool session_end_query_pending_{};
    bool session_end_cleanup_succeeded_{true};
    bool hotkey_registration_pending_{true};
    bool safety_hotkeys_confirmed_{};
    bool active_run_hotkey_guard_active_{};
    HotkeyIndicatorState hotkey_indicator_state_{HotkeyIndicatorState::Unknown};
    std::uint64_t pending_hotkey_request_id_{HotkeyThread::InitialRequestId};
    core::HotkeyBinding confirmed_start_hotkey_{};
    core::HotkeyBinding confirmed_emergency_hotkey_{};
    std::optional<std::wstring> deferred_hotkey_notice_;

    // Retained presentation surfaces, custom combo state, tabs, pages, and
    // scrolling.
    HWND content_host_{};
    HWND resize_overlay_{};
    HWND resize_overlay_guard_{};
    bool resize_overlay_child_clipped_{};
    int resize_overlay_content_x_{};
    int resize_overlay_content_y_{};
    RECT resize_overlay_screen_rect_{};
    bool resize_overlay_geometry_valid_{};
    int resize_overlay_advanced_viewport_logical_{layout::BasicPageHeight};
    int resize_overlay_source_advanced_viewport_logical_{layout::BasicPageHeight};
    bool resize_advanced_physical_bottom_anchor_active_{};
    int resize_advanced_physical_bottom_anchor_screen_y_{};
    UniqueGdiObject resize_advanced_preview_bitmap_;
    int resize_advanced_preview_width_{};
    int resize_advanced_preview_height_{};
    HWND startup_cover_{};
    HWND combo_popup_{};
    HWND combo_popup_owner_{};
    HWND key_capture_combo_{};
    int combo_popup_highlight_{-1};
    int combo_popup_hover_{-1};
    int combo_popup_top_index_{};
    int combo_popup_item_count_{};
    int combo_popup_visible_rows_{};
    int combo_popup_item_height_{};
    int combo_popup_x_{};
    int combo_popup_y_{};
    int combo_popup_width_{};
    int combo_popup_height_{};
    bool combo_popup_scroll_hovered_{};
    bool combo_popup_scroll_dragging_{};
    int combo_popup_scroll_drag_offset_{};
    HWND control_parent_{};
    HWND basic_tab_button_{};
    HWND advanced_tab_button_{};
    HWND about_tab_button_{};
    int selected_page_index_{};
    HWND basic_page_host_{};
    HWND advanced_page_host_{};
    HWND advanced_scroll_content_host_{};
    HWND advanced_scrollbar_{};
    HWND advanced_scroll_snapshot_{};
    UniqueGdiObject advanced_scroll_snapshot_bitmap_;
    int advanced_scroll_snapshot_width_{};
    int advanced_scroll_snapshot_height_{};
    bool advanced_scroll_snapshot_active_{};
    HWND about_page_host_{};
    HWND basic_top_host_{};
    HWND basic_top_snapshot_{};
    UniqueGdiObject basic_top_snapshot_bitmap_;
    HWND move_cover_{};
    UniqueGdiObject move_cover_bitmap_;
    int move_cover_width_{};
    int move_cover_height_{};
    int move_cover_page_index_{-1};
    int move_cover_action_type_index_{-1};
    int move_cover_advanced_scroll_offset_{-1};
    HWND move_cover_focus_{};
    HWND move_cover_pointer_control_{};
    std::uint64_t move_cover_presentation_revision_{1U};
    std::uint64_t move_cover_bitmap_revision_{};
    bool input_type_update_pending_{};
    bool input_type_update_posted_{};
    bool move_cover_refresh_posted_{};
    int advanced_viewport_height_logical_{layout::BasicPageHeight};
    int advanced_scroll_offset_logical_{};
    int advanced_scroll_wheel_remainder_{};
    bool advanced_scroll_hovered_{};
    bool advanced_scroll_dragging_{};
    int advanced_scroll_drag_offset_{};

    // Pointer state for custom numeric-field hover and press presentation.
    HWND numeric_hot_edit_{};
    int numeric_hot_part_{};
    HWND numeric_pressed_edit_{};
    int numeric_pressed_part_{};

    // Group containers for the Basic, Advanced, and About page layouts.
    HWND input_group_{};
    HWND advanced_timing_group_{};
    HWND target_group_{};
    HWND position_group_{};
    HWND repeat_group_{};
    HWND hotkeys_group_{};
    HWND performance_group_{};
    HWND notifications_group_{};
    HWND options_group_{};
    HWND about_identity_group_{};
    HWND about_links_group_{};

    // Native child controls. Keep handles grouped by their visual / functional
    // order.
    HWND action_type_label_{};
    HWND action_type_combo_{};
    HWND action_pattern_label_{};
    HWND action_pattern_combo_{};
    HWND burst_count_label_{};
    HWND burst_count_edit_{};
    HWND action_spacing_label_{};
    HWND action_spacing_minutes_edit_{};
    HWND action_spacing_seconds_edit_{};
    HWND action_spacing_edit_{};
    HWND backend_label_{};
    HWND backend_combo_{};
    HWND random_interval_check_{};
    HWND random_interval_style_label_{};
    HWND random_interval_style_combo_{};
    HWND advanced_minutes_header_{};
    HWND advanced_seconds_header_{};
    HWND advanced_milliseconds_header_{};
    HWND minimum_interval_label_{};
    HWND minimum_interval_minutes_edit_{};
    HWND minimum_interval_seconds_edit_{};
    HWND minimum_interval_edit_{};
    HWND maximum_interval_label_{};
    HWND maximum_interval_minutes_edit_{};
    HWND maximum_interval_seconds_edit_{};
    HWND maximum_interval_edit_{};
    HWND mouse_button_label_{};
    HWND mouse_button_combo_{};
    HWND generated_key_label_{};
    HWND generated_key_combo_{};
    HWND target_label_{};
    HWND target_status_text_{};
    HWND select_target_button_{};
    HWND clear_target_button_{};
    HWND background_input_check_{};
    HWND interval_label_{};
    HWND basic_interval_minutes_header_{};
    HWND basic_interval_seconds_header_{};
    HWND basic_interval_milliseconds_header_{};
    HWND interval_minutes_edit_{};
    HWND interval_seconds_edit_{};
    HWND interval_edit_{};
    HWND rate_text_{};
    HWND button_down_label_{};
    HWND button_down_minutes_edit_{};
    HWND button_down_seconds_edit_{};
    HWND button_down_edit_{};
    HWND down_duration_behavior_label_{};
    HWND down_duration_behavior_combo_{};
    HWND current_cursor_radio_{};
    HWND fixed_position_radio_{};
    HWND fixed_x_label_{};
    HWND fixed_x_edit_{};
    HWND fixed_y_label_{};
    HWND fixed_y_edit_{};
    HWND capture_position_button_{};
    HWND click_position_indicator_check_{};
    HWND position_unavailable_text_{};
    HWND unlimited_radio_{};
    HWND limited_radio_{};
    HWND repeat_count_edit_{};
    HWND repeat_unit_label_{};
    HWND run_time_hours_header_{};
    HWND run_time_minutes_header_{};
    HWND run_time_seconds_header_{};
    HWND run_time_hours_edit_{};
    HWND run_time_minutes_edit_{};
    HWND run_time_seconds_edit_{};
    HWND start_hotkey_label_{};
    HWND start_hotkey_combo_{};
    HWND emergency_hotkey_label_{};
    HWND emergency_hotkey_combo_{};
    HWND diagnostics_check_{};
    HWND safety_shield_check_{};
    HWND force_exit_on_emergency_stop_check_{};
    HWND capture_exclusion_check_{};
    HWND keep_on_top_check_{};
    HWND profile_label_{};
    HWND profile_combo_{};
    HWND manage_profiles_button_{};
    HWND remember_settings_check_{};
    HWND import_settings_button_{};
    HWND performance_guidance_text_{};
    HWND process_priority_label_{};
    HWND process_priority_combo_{};
    HWND timing_worker_priority_label_{};
    HWND timing_worker_priority_combo_{};
    HWND hotkey_control_priority_label_{};
    HWND hotkey_control_priority_combo_{};
    HWND timing_worker_qos_label_{};
    HWND timing_worker_qos_combo_{};
    HWND windows_notification_label_{};
    HWND windows_notification_combo_{};
    HWND system_sound_label_{};
    HWND system_sound_combo_{};
    HWND running_indicator_check_{};
    HWND about_name_text_{};
    HWND about_version_text_{};
    HWND about_description_text_{};
    HWND about_details_text_{};
    HWND about_links_description_text_{};
    HWND about_support_text_{};
    HWND official_downloads_button_{};
    HWND source_code_button_{};
    HWND report_bug_button_{};
    HWND copy_support_email_button_{};
    HWND view_license_button_{};
    HWND copy_diagnostic_report_button_{};
    HWND start_button_{};
    HWND stop_button_{};
    HWND emergency_button_{};
    HWND admin_button_{};
    HWND status_text_{};
    HWND diagnostics_text_{};
    std::wstring last_diagnostics_text_;
    std::wstring ordinary_status_tooltip_;
    StatusCategory status_category_{StatusCategory::Ready};
    StatusCategory diagnostics_category_{StatusCategory::Ready};
    std::optional<PresentationSnapshot> last_presentation_snapshot_;
    bool start_availability_initialized_{};
    bool start_available_{};
    bool start_attention_required_{};
    std::wstring start_unavailable_reason_;
};

} // namespace vectorclick::win
