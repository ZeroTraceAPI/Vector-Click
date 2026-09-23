#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace vectorclick::win::layout {

inline constexpr int InitialWindowWidth = 720;
inline constexpr int CompactWindowHeight = 631;
inline constexpr int InitialWindowHeight = CompactWindowHeight;
inline constexpr int MinimumWindowWidth = 720;
inline constexpr int FixedContentWidth = 680;
inline constexpr int BasicPageHeight = 430;
inline constexpr int AdvancedPageHeight = 1108;
inline constexpr int AboutPageHeight = 430;
inline constexpr int FooterBottomPadding = 5;
inline constexpr int CompactContentHeight = 580 + FooterBottomPadding;
inline constexpr int ExpandedContentHeight = 800 + FooterBottomPadding;
inline constexpr int ContentChromeHeight = CompactContentHeight - BasicPageHeight;
// The content host has an eight-logical-pixel gutter to the right of each
// page. The Advanced scrollbar occupies that gutter instead of overlaying the
// card's rounded right edge.
inline constexpr int AdvancedScrollbarWidth = 8;
inline constexpr int AdvancedScrollbarRightInset = 0;
inline constexpr int AdvancedScrollbarTopInset = 4;
inline constexpr int AdvancedScrollbarBottomInset = 4;
inline constexpr int MinimumStatusHeight = 40;
inline constexpr int MinimumContentMargin = 4;
// The outer window provides compact, nearly balanced top and bottom margins.
// Retain only a subtle upward optical bias.
inline constexpr int ContentVerticalBias = 1;
inline constexpr int StatusPanelHorizontalInset = 12;
inline constexpr int StatusPanelWidth = FixedContentWidth - (StatusPanelHorizontalInset * 2);
inline constexpr int TabTop = 2;
inline constexpr int TabHeight = 36;
inline constexpr int TabDockTop = TabTop + TabHeight - 1;
inline constexpr int TabCornerRadius = 10;
inline constexpr int TabDockJunctionRadius = 5;


enum class Page : std::uint8_t {
    Basic,
    Advanced,
    About,
};

struct Rect final {
    int x{};
    int y{};
    int width{};
    int height{};

    [[nodiscard]] friend constexpr bool operator==(const Rect&, const Rect&) = default;
};

struct LayoutMetrics final {
    int action_type_label_width{118};
    int mouse_button_label_width{118};
    int generated_key_label_width{118};
    int action_pattern_label_width{118};
    int interval_label_width{128};
    int current_cursor_width{142};
    int fixed_position_width{184};
    int unlimited_width{126};
    int limited_width{116};
    int start_hotkey_label_width{132};
    int emergency_hotkey_label_width{142};
    int background_input_width{232};
    int random_interval_width{228};
    int diagnostics_width{206};
    int click_position_indicator_width{238};
    int safety_shield_width{288};
    int force_exit_on_emergency_stop_width{278};
    int capture_exclusion_width{306};
    int keep_on_top_width{220};
    int remember_settings_width{194};
    int running_indicator_width{286};
};

enum class Control : std::size_t {
    BasicTab,
    AdvancedTab,
    AboutTab,
    BasicPageHost,
    AdvancedPageHost,
    AboutPageHost,
    BasicTopHost,
    InputGroup,
    PositionGroup,
    RepeatGroup,
    HotkeysGroup,
    ActionTypeLabel,
    ActionTypeCombo,
    MouseButtonLabel,
    GeneratedKeyLabel,
    MouseButtonCombo,
    GeneratedKeyCombo,
    ActionPatternLabel,
    ActionPatternCombo,
    IntervalLabel,
    BasicIntervalMinutesHeader,
    BasicIntervalSecondsHeader,
    BasicIntervalMillisecondsHeader,
    IntervalMinutesEdit,
    IntervalSecondsEdit,
    IntervalEdit,
    RateText,
    CurrentCursorRadio,
    FixedPositionRadio,
    FixedXLabel,
    FixedXEdit,
    FixedYLabel,
    FixedYEdit,
    CapturePositionButton,
    PositionUnavailableText,
    UnlimitedRadio,
    LimitedRadio,
    RepeatCountEdit,
    RepeatUnitLabel,
    RunTimeHoursHeader,
    RunTimeMinutesHeader,
    RunTimeSecondsHeader,
    RunTimeHoursEdit,
    RunTimeMinutesEdit,
    RunTimeSecondsEdit,
    StartHotkeyLabel,
    StartHotkeyCombo,
    EmergencyHotkeyLabel,
    EmergencyHotkeyCombo,
    AdvancedTimingGroup,
    AdvancedMinutesHeader,
    AdvancedSecondsHeader,
    AdvancedMillisecondsHeader,
    ButtonDownLabel,
    ButtonDownMinutesEdit,
    ButtonDownSecondsEdit,
    ButtonDownEdit,
    DownDurationBehaviorLabel,
    DownDurationBehaviorCombo,
    ActionSpacingLabel,
    ActionSpacingMinutesEdit,
    ActionSpacingSecondsEdit,
    ActionSpacingEdit,
    BurstCountLabel,
    BurstCountEdit,
    BackendLabel,
    BackendCombo,
    RandomIntervalCheck,
    RandomIntervalStyleLabel,
    RandomIntervalStyleCombo,
    MinimumIntervalLabel,
    MinimumIntervalMinutesEdit,
    MinimumIntervalSecondsEdit,
    MinimumIntervalEdit,
    MaximumIntervalLabel,
    MaximumIntervalMinutesEdit,
    MaximumIntervalSecondsEdit,
    MaximumIntervalEdit,
    TargetGroup,
    TargetLabel,
    TargetStatusText,
    SelectTargetButton,
    ClearTargetButton,
    BackgroundInputCheck,
    AdminButton,
    PerformanceGroup,
    PerformanceGuidanceText,
    NotificationsGroup,
    WindowsNotificationLabel,
    WindowsNotificationCombo,
    SystemSoundLabel,
    SystemSoundCombo,
    RunningIndicatorCheck,
    OptionsGroup,
    DiagnosticsCheck,
    ClickPositionIndicatorCheck,
    SafetyShieldCheck,
    ForceExitOnEmergencyStopCheck,
    CaptureExclusionCheck,
    KeepOnTopCheck,
    ProfileLabel,
    ProfileCombo,
    ManageProfilesButton,
    RememberSettingsCheck,
    ProcessPriorityLabel,
    ProcessPriorityCombo,
    TimingWorkerPriorityLabel,
    TimingWorkerPriorityCombo,
    HotkeyControlPriorityLabel,
    HotkeyControlPriorityCombo,
    TimingWorkerQosLabel,
    TimingWorkerQosCombo,
    AboutIdentityGroup,
    AboutNameText,
    AboutVersionText,
    AboutDescriptionText,
    AboutDetailsText,
    AboutLinksGroup,
    AboutLinksDescriptionText,
    AboutSupportText,
    OfficialDownloadsButton,
    SourceCodeButton,
    ReportBugButton,
    CopySupportEmailButton,
    ViewLicenseButton,
    CopyDiagnosticReportButton,
    ImportSettingsButton,
    StatusText,
    DiagnosticsText,
    StartButton,
    StopButton,
    EmergencyButton,
    Count,
};

inline constexpr std::size_t ControlCount = static_cast<std::size_t>(Control::Count);

struct MainLayout final {
    std::array<Rect, ControlCount> controls{};
    int content_width{FixedContentWidth};
    int content_height{ExpandedContentHeight};

    [[nodiscard]] constexpr const Rect& operator[](const Control control) const noexcept {
        return controls[static_cast<std::size_t>(control)];
    }
};

[[nodiscard]] constexpr int NormalizeStatusHeight(const int status_height) noexcept {
    return status_height < MinimumStatusHeight ? MinimumStatusHeight : status_height;
}

[[nodiscard]] constexpr int PageHeight(const Page page) noexcept {
    switch (page) {
    case Page::Advanced:
        return AdvancedPageHeight;
    case Page::Basic:
        return BasicPageHeight;
    case Page::About:
        return AboutPageHeight;
    }
    return AdvancedPageHeight;
}

[[nodiscard]] constexpr int ClampAdvancedViewportHeight(const int height) noexcept {
    return height < BasicPageHeight
               ? BasicPageHeight
               : height > AdvancedPageHeight ? AdvancedPageHeight : height;
}

[[nodiscard]] constexpr int VisiblePageHeight(
    const Page page,
    const int advanced_viewport_height = BasicPageHeight) noexcept {
    return page == Page::Advanced
               ? ClampAdvancedViewportHeight(advanced_viewport_height)
               : PageHeight(page);
}

[[nodiscard]] constexpr int ContentHeightForViewport(
    const int status_height,
    const int page_viewport_height) noexcept {
    return ContentChromeHeight + page_viewport_height +
           (NormalizeStatusHeight(status_height) - MinimumStatusHeight);
}

[[nodiscard]] constexpr int BaseOuterHeight(const Page) noexcept {
    return CompactWindowHeight;
}

[[nodiscard]] constexpr int MinimumOuterHeight(const int status_height,
                                               const Page) noexcept {
    return CompactWindowHeight +
           (NormalizeStatusHeight(status_height) - MinimumStatusHeight);
}

[[nodiscard]] int ScaleForDpi(int value, unsigned int dpi) noexcept;
[[nodiscard]] int CalculateAdvancedViewportHeight(int client_height_pixels,
                                                   unsigned int dpi,
                                                   int status_height) noexcept;
[[nodiscard]] MainLayout CalculateMainLayout(
    const LayoutMetrics& metrics,
    int status_height,
    Page selected_page,
    int advanced_viewport_height = BasicPageHeight) noexcept;
[[nodiscard]] Rect CalculateCenteredContentHost(
    int client_width_pixels,
    int client_height_pixels,
    unsigned int dpi,
    int status_height,
    Page selected_page,
    int advanced_viewport_height = BasicPageHeight) noexcept;
[[nodiscard]] Rect CalculateClampedContentHost(
    int client_width_pixels,
    int client_height_pixels,
    unsigned int dpi,
    int status_height,
    Page selected_page,
    int preferred_x_pixels,
    int preferred_y_pixels,
    int advanced_viewport_height = BasicPageHeight) noexcept;

} // namespace vectorclick::win::layout
