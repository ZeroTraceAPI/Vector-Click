#include "Windows/ui_layout.h"

#include <algorithm>
#include <cstdint>

namespace vectorclick::win::layout {
namespace {

void Set(MainLayout& layout,
         const Control control,
         const int x,
         const int y,
         const int width,
         const int height = 30) noexcept {
    layout.controls[static_cast<std::size_t>(control)] = {x, y, width, height};
}

} // namespace

int ScaleForDpi(const int value, const unsigned int dpi) noexcept {
    const std::int64_t effective_dpi = dpi == 0U ? 96 : static_cast<std::int64_t>(dpi);
    const std::int64_t product = static_cast<std::int64_t>(value) * effective_dpi;
    if (product >= 0) {
        return static_cast<int>((product + 48) / 96);
    }
    return static_cast<int>((product - 48) / 96);
}

int CalculateAdvancedViewportHeight(const int client_height_pixels,
                                    const unsigned int dpi,
                                    const int status_height) noexcept {
    const int effective_dpi = dpi == 0U ? 96 : static_cast<int>(dpi);
    const int client_height_logical =
        std::max(0, (client_height_pixels * 96 + (effective_dpi / 2)) /
                        effective_dpi);
    const int available_content_height =
        client_height_logical - (MinimumContentMargin * 2);
    const int status_growth =
        NormalizeStatusHeight(status_height) - MinimumStatusHeight;
    const int viewport_height =
        available_content_height - ContentChromeHeight - status_growth;
    return ClampAdvancedViewportHeight(viewport_height);
}

MainLayout CalculateMainLayout(const LayoutMetrics& metrics,
                               const int status_height,
                               const Page selected_page,
                               const int advanced_viewport_height) noexcept {
    const int normalized_status_height = NormalizeStatusHeight(status_height);
    const int clamped_advanced_viewport =
        ClampAdvancedViewportHeight(advanced_viewport_height);
    const int selected_page_height =
        VisiblePageHeight(selected_page, clamped_advanced_viewport);
    MainLayout result{};
    result.content_height =
        ContentHeightForViewport(normalized_status_height, selected_page_height);

    constexpr int basic_tab_width = 104;
    constexpr int advanced_tab_width = 124;
    constexpr int about_tab_width = 104;
    constexpr int page_x = 8;
    constexpr int page_y = 42;
    constexpr int page_width = 664;
    constexpr int basic_page_height = BasicPageHeight;
    constexpr int advanced_page_height = AdvancedPageHeight;
    constexpr int about_page_height = AboutPageHeight;

    Set(result, Control::BasicTab, page_x + 8, TabTop, basic_tab_width, TabHeight);
    Set(result,
        Control::AdvancedTab,
        page_x + 8 + basic_tab_width + 6,
        TabTop,
        advanced_tab_width,
        TabHeight);
    Set(result,
        Control::AboutTab,
        page_x + 8 + basic_tab_width + 6 + advanced_tab_width + 6,
        TabTop,
        about_tab_width,
        TabHeight);
    Set(result, Control::BasicPageHost, page_x, page_y, page_width, basic_page_height);
    Set(result,
        Control::AdvancedPageHost,
        page_x,
        page_y,
        page_width,
        clamped_advanced_viewport);
    Set(result, Control::AboutPageHost, page_x, page_y, page_width, about_page_height);

    constexpr int card_gap = 10;
    constexpr int column_width = 326;
    constexpr int right_column_x = column_width + card_gap;
    // Basic establishes the shared visual card envelope. The page hosts remain
    // two logical pixels wider so their existing clipping and scrollbar
    // geometry stay unchanged.
    constexpr int card_envelope_width = right_column_x + column_width;
    constexpr int top_card_height = 272;
    constexpr int bottom_card_y = top_card_height + card_gap;
    constexpr int bottom_card_height = basic_page_height - bottom_card_y - 2;
    constexpr int row_height = 28;
    constexpr int card_left_padding = 16;
    constexpr int field_width = 138;

    Set(result, Control::BasicTopHost, 0, 0, page_width, top_card_height);
    Set(result, Control::InputGroup, 0, 0, column_width, top_card_height);
    Set(result, Control::PositionGroup, right_column_x, 0, column_width, top_card_height);
    Set(result, Control::RepeatGroup, 0, bottom_card_y, column_width, bottom_card_height);
    Set(result,
        Control::HotkeysGroup,
        right_column_x,
        bottom_card_y,
        column_width,
        bottom_card_height);

    const int input_field_x = column_width - card_left_padding - field_width;
    const int input_label_width = std::min(
        input_field_x - card_left_padding - 6,
        std::max({metrics.action_type_label_width,
                  metrics.mouse_button_label_width,
                  metrics.generated_key_label_width,
                  metrics.action_pattern_label_width,
                  metrics.interval_label_width}));
    int row_y = 44;
    constexpr int compact_row_gap = 34;
    Set(result, Control::ActionTypeLabel, card_left_padding, row_y, input_label_width, row_height);
    Set(result, Control::ActionTypeCombo, input_field_x, row_y, field_width, 280);
    row_y += compact_row_gap;
    Set(result, Control::MouseButtonLabel, card_left_padding, row_y, input_label_width, row_height);
    Set(result, Control::GeneratedKeyLabel, card_left_padding, row_y, input_label_width, row_height);
    Set(result, Control::MouseButtonCombo, input_field_x, row_y, field_width, 280);
    Set(result, Control::GeneratedKeyCombo, input_field_x, row_y, field_width, 320);
    row_y += compact_row_gap;
    Set(result, Control::ActionPatternLabel, card_left_padding, row_y, input_label_width, row_height);
    Set(result, Control::ActionPatternCombo, input_field_x, row_y, field_width, 260);

    constexpr int duration_field_width = 92;
    constexpr int duration_field_gap = 7;
    constexpr int duration_first_x = card_left_padding;
    constexpr int duration_second_x =
        duration_first_x + duration_field_width + duration_field_gap;
    constexpr int duration_third_x =
        duration_second_x + duration_field_width + duration_field_gap;
    Set(result,
        Control::IntervalLabel,
        card_left_padding,
        145,
        column_width - (card_left_padding * 2),
        22);
    Set(result, Control::BasicIntervalMinutesHeader,
        duration_first_x, 171, duration_field_width, 18);
    Set(result, Control::BasicIntervalSecondsHeader,
        duration_second_x, 171, duration_field_width, 18);
    Set(result, Control::BasicIntervalMillisecondsHeader,
        duration_third_x, 171, duration_field_width, 18);
    Set(result, Control::IntervalMinutesEdit,
        duration_first_x, 190, duration_field_width, row_height);
    Set(result, Control::IntervalSecondsEdit,
        duration_second_x, 190, duration_field_width, row_height);
    Set(result, Control::IntervalEdit,
        duration_third_x, 190, duration_field_width, row_height);
    Set(result,
        Control::RateText,
        card_left_padding,
        225,
        column_width - (card_left_padding * 2),
        40);

    const int position_left = right_column_x + card_left_padding;
    Set(result, Control::CurrentCursorRadio, position_left, 48, metrics.current_cursor_width, row_height);
    Set(result, Control::FixedPositionRadio, position_left, 84, metrics.fixed_position_width, row_height);

    constexpr int coordinate_label_width = 24;
    constexpr int coordinate_edit_width = 82;
    constexpr int coordinate_gap = 10;
    const int x_label_x = position_left;
    const int x_edit_x = x_label_x + coordinate_label_width;
    const int y_label_x = x_edit_x + coordinate_edit_width + coordinate_gap;
    const int y_edit_x = y_label_x + coordinate_label_width;
    Set(result, Control::FixedXLabel, x_label_x, 120, coordinate_label_width, row_height);
    Set(result, Control::FixedXEdit, x_edit_x, 120, coordinate_edit_width, row_height);
    Set(result, Control::FixedYLabel, y_label_x, 120, coordinate_label_width, row_height);
    Set(result, Control::FixedYEdit, y_edit_x, 120, coordinate_edit_width, row_height);
    Set(result,
        Control::CapturePositionButton,
        position_left,
        174,
        column_width - (card_left_padding * 2),
        46);
    Set(result,
        Control::PositionUnavailableText,
        position_left,
        68,
        column_width - (card_left_padding * 2),
        90);

    const int repeat_left = card_left_padding;
    constexpr int repeat_row_height = 26;
    Set(result, Control::UnlimitedRadio, repeat_left, bottom_card_y + 42,
        metrics.unlimited_width, repeat_row_height);
    const int limited_width = metrics.limited_width;
    Set(result, Control::LimitedRadio, repeat_left, bottom_card_y + 70,
        limited_width, repeat_row_height);
    const int repeat_edit_x = repeat_left + limited_width + 8;
    // The repeat count accepts at most 1,000,000, so it can be slightly
    // narrower than the duration fields while still leaving enough text room
    // for all seven digits plus the spin-button gutter. Tighten only the gap
    // after it so the complete single-action unit label, including
    // "key presses", remains visible at high DPI without ellipsis.
    constexpr int repeat_edit_width = 88;
    Set(result, Control::RepeatCountEdit, repeat_edit_x, bottom_card_y + 70,
        repeat_edit_width, repeat_row_height);
    constexpr int repeat_unit_offset = repeat_edit_width + 4;
    Set(result,
        Control::RepeatUnitLabel,
        repeat_edit_x + repeat_unit_offset,
        bottom_card_y + 70,
        column_width - (repeat_edit_x + repeat_unit_offset) - card_left_padding,
        repeat_row_height);

    // The run time limit uses the same three-field width as Basic Input
    // interval. All-zero means off, so the Repeat card needs no additional
    // enable checkbox and can retain the established Basic page height.
    Set(result, Control::RunTimeHoursHeader,
        duration_first_x, bottom_card_y + 97, duration_field_width, 16);
    Set(result, Control::RunTimeMinutesHeader,
        duration_second_x, bottom_card_y + 97, duration_field_width, 16);
    Set(result, Control::RunTimeSecondsHeader,
        duration_third_x, bottom_card_y + 97, duration_field_width, 16);
    Set(result, Control::RunTimeHoursEdit,
        duration_first_x, bottom_card_y + 115, duration_field_width, repeat_row_height);
    Set(result, Control::RunTimeMinutesEdit,
        duration_second_x, bottom_card_y + 115, duration_field_width, repeat_row_height);
    Set(result, Control::RunTimeSecondsEdit,
        duration_third_x, bottom_card_y + 115, duration_field_width, repeat_row_height);

    const int hotkey_left = right_column_x + card_left_padding;
    const int hotkey_label_width = std::max(metrics.start_hotkey_label_width,
                                            metrics.emergency_hotkey_label_width);
    // Use the same compact field width as the Input card. This is wide enough
    // for the shared "Press a key..." capture prompt while preserving the
    // complete Start / Stop and Emergency Stop labels and their card spacing.
    constexpr int hotkey_combo_width = field_width;
    const int hotkey_combo_x = right_column_x + column_width - card_left_padding - hotkey_combo_width;
    Set(result, Control::StartHotkeyLabel, hotkey_left, bottom_card_y + 50, hotkey_label_width, row_height);
    Set(result, Control::StartHotkeyCombo, hotkey_combo_x, bottom_card_y + 50, hotkey_combo_width, 280);
    Set(result,
        Control::EmergencyHotkeyLabel,
        hotkey_left,
        bottom_card_y + 94,
        hotkey_label_width,
        row_height);
    Set(result,
        Control::EmergencyHotkeyCombo,
        hotkey_combo_x,
        bottom_card_y + 94,
        hotkey_combo_width,
        280);

    constexpr int advanced_label_x = 18;
    constexpr int advanced_card_width = card_envelope_width;
    constexpr int advanced_combo_width = 214;
    constexpr int advanced_combo_x =
        advanced_card_width - advanced_label_x - advanced_combo_width;
    constexpr int timing_height = 358;
    Set(result, Control::AdvancedTimingGroup, 0, 0, advanced_card_width, timing_height);

    constexpr int timing_label_width = 300;
    constexpr int advanced_duration_field_width = 98;
    constexpr int advanced_duration_field_gap = 6;
    constexpr int duration_minutes_x = 338;
    constexpr int duration_seconds_x =
        duration_minutes_x + advanced_duration_field_width + advanced_duration_field_gap;
    constexpr int duration_milliseconds_x =
        duration_seconds_x + advanced_duration_field_width + advanced_duration_field_gap;
    Set(result, Control::AdvancedMinutesHeader,
        duration_minutes_x, 40, advanced_duration_field_width, 18);
    Set(result, Control::AdvancedSecondsHeader,
        duration_seconds_x, 40, advanced_duration_field_width, 18);
    Set(result, Control::AdvancedMillisecondsHeader,
        duration_milliseconds_x, 40, advanced_duration_field_width, 18);

    Set(result, Control::ButtonDownLabel, advanced_label_x, 60, timing_label_width, 26);
    Set(result, Control::ButtonDownMinutesEdit, duration_minutes_x, 60, advanced_duration_field_width, 26);
    Set(result, Control::ButtonDownSecondsEdit, duration_seconds_x, 60, advanced_duration_field_width, 26);
    Set(result, Control::ButtonDownEdit, duration_milliseconds_x, 60, advanced_duration_field_width, 26);
    Set(result, Control::DownDurationBehaviorLabel, advanced_label_x, 90, timing_label_width, 26);
    Set(result, Control::DownDurationBehaviorCombo, advanced_combo_x, 90, advanced_combo_width, 180);
    Set(result, Control::ActionSpacingLabel, advanced_label_x, 122, timing_label_width, 26);
    Set(result, Control::ActionSpacingMinutesEdit, duration_minutes_x, 122, advanced_duration_field_width, 26);
    Set(result, Control::ActionSpacingSecondsEdit, duration_seconds_x, 122, advanced_duration_field_width, 26);
    Set(result, Control::ActionSpacingEdit, duration_milliseconds_x, 122, advanced_duration_field_width, 26);

    constexpr int advanced_numeric_width = 184;
    constexpr int advanced_numeric_x =
        advanced_card_width - advanced_label_x - advanced_numeric_width;
    Set(result, Control::BurstCountLabel, advanced_label_x, 156, timing_label_width, 26);
    Set(result, Control::BurstCountEdit, advanced_numeric_x, 156, advanced_numeric_width, 26);
    Set(result, Control::BackendLabel, advanced_label_x, 186, timing_label_width, 26);
    Set(result, Control::BackendCombo, advanced_combo_x, 186, advanced_combo_width, 280);
    Set(result,
        Control::RandomIntervalCheck,
        advanced_label_x,
        222,
        std::min(300, metrics.random_interval_width),
        18);
    Set(result, Control::RandomIntervalStyleLabel, advanced_label_x, 250, timing_label_width, 26);
    Set(result, Control::RandomIntervalStyleCombo, advanced_combo_x, 250, advanced_combo_width, 180);
    Set(result, Control::MinimumIntervalLabel, advanced_label_x, 286, timing_label_width, 26);
    Set(result, Control::MinimumIntervalMinutesEdit, duration_minutes_x, 286, advanced_duration_field_width, 26);
    Set(result, Control::MinimumIntervalSecondsEdit, duration_seconds_x, 286, advanced_duration_field_width, 26);
    Set(result, Control::MinimumIntervalEdit, duration_milliseconds_x, 286, advanced_duration_field_width, 26);
    Set(result, Control::MaximumIntervalLabel, advanced_label_x, 318, timing_label_width, 26);
    Set(result, Control::MaximumIntervalMinutesEdit, duration_minutes_x, 318, advanced_duration_field_width, 26);
    Set(result, Control::MaximumIntervalSecondsEdit, duration_seconds_x, 318, advanced_duration_field_width, 26);
    Set(result, Control::MaximumIntervalEdit, duration_milliseconds_x, 318, advanced_duration_field_width, 26);

    constexpr int target_y = timing_height + card_gap;
    constexpr int target_height = 142;
    Set(result, Control::TargetGroup, 0, target_y, advanced_card_width, target_height);
    Set(result, Control::TargetLabel, advanced_label_x, target_y + 44, 130, 26);
    Set(result, Control::TargetStatusText, 152, target_y + 42, 202, 32);
    Set(result, Control::SelectTargetButton, 362, target_y + 42, 180, 34);
    constexpr int clear_target_width = 96;
    Set(result,
        Control::ClearTargetButton,
        advanced_card_width - advanced_label_x - clear_target_width,
        target_y + 42,
        clear_target_width,
        34);
    // Keep the two compatibility actions on one balanced lower row instead of
    // squeezing Allow background input between the target row and administrator
    // restart button. Their vertical centers match while the card height remains
    // unchanged.
    Set(result,
        Control::BackgroundInputCheck,
        advanced_label_x,
        target_y + 99,
        metrics.background_input_width,
        24);
    Set(result, Control::AdminButton, 338, target_y + 96, 232, 30);

    // Scheduling controls live in their own full-width card. Keep the card on
    // the same 662-logical-pixel envelope as every other Advanced section so
    // the page retains the corrected shared left / right edges.
    constexpr int performance_y = target_y + target_height + card_gap;
    constexpr int performance_height = 206;
    Set(result,
        Control::PerformanceGroup,
        0,
        performance_y,
        advanced_card_width,
        performance_height);

    constexpr int performance_left_x = advanced_label_x;
    constexpr int performance_right_x = 338;
    constexpr int performance_combo_width = 236;
    constexpr int performance_label_width = 250;
    constexpr int performance_label_height = 20;
    constexpr int performance_combo_top_y = performance_y + 68;
    constexpr int performance_combo_bottom_y = performance_y + 122;

    Set(result,
        Control::ProcessPriorityLabel,
        performance_left_x,
        performance_y + 46,
        performance_label_width,
        performance_label_height);
    Set(result,
        Control::ProcessPriorityCombo,
        performance_left_x,
        performance_combo_top_y,
        performance_combo_width,
        180);
    Set(result,
        Control::TimingWorkerQosLabel,
        performance_right_x,
        performance_y + 46,
        performance_label_width,
        performance_label_height);
    Set(result,
        Control::TimingWorkerQosCombo,
        performance_right_x,
        performance_combo_top_y,
        performance_combo_width,
        160);
    Set(result,
        Control::TimingWorkerPriorityLabel,
        performance_left_x,
        performance_y + 100,
        performance_label_width,
        performance_label_height);
    Set(result,
        Control::TimingWorkerPriorityCombo,
        performance_left_x,
        performance_combo_bottom_y,
        performance_combo_width,
        160);
    Set(result,
        Control::HotkeyControlPriorityLabel,
        performance_right_x,
        performance_y + 100,
        performance_label_width,
        performance_label_height);
    Set(result,
        Control::HotkeyControlPriorityCombo,
        performance_right_x,
        performance_combo_bottom_y,
        performance_combo_width,
        140);
    Set(result,
        Control::PerformanceGuidanceText,
        advanced_label_x,
        performance_y + 154,
        advanced_card_width - (advanced_label_x * 2),
        40);

    constexpr int notifications_y = performance_y + performance_height + card_gap;
    constexpr int notifications_height = 136;
    Set(result,
        Control::NotificationsGroup,
        0,
        notifications_y,
        advanced_card_width,
        notifications_height);
    // Match the Scheduling & Performance column grid. Notifications and sound
    // are parallel event-output choices, so presenting them side by side makes
    // the relationship clearer and avoids unnecessarily wide selectors.
    Set(result,
        Control::WindowsNotificationLabel,
        performance_left_x,
        notifications_y + 46,
        performance_label_width,
        performance_label_height);
    Set(result,
        Control::WindowsNotificationCombo,
        performance_left_x,
        notifications_y + 68,
        performance_combo_width,
        150);
    Set(result,
        Control::SystemSoundLabel,
        performance_right_x,
        notifications_y + 46,
        performance_label_width,
        performance_label_height);
    Set(result,
        Control::SystemSoundCombo,
        performance_right_x,
        notifications_y + 68,
        performance_combo_width,
        150);
    Set(result,
        Control::RunningIndicatorCheck,
        advanced_label_x,
        notifications_y + 108,
        std::min(advanced_card_width - (advanced_label_x * 2),
                 metrics.running_indicator_width),
        18);

    constexpr int options_y = notifications_y + notifications_height + card_gap;
    constexpr int options_height = advanced_page_height - options_y - 2;
    Set(result, Control::OptionsGroup, 0, options_y, advanced_card_width, options_height);
    constexpr int option_row_height = 18;
    Set(result,
        Control::DiagnosticsCheck,
        advanced_label_x,
        options_y + 50,
        std::min(292, metrics.diagnostics_width),
        option_row_height);
    Set(result,
        Control::ClickPositionIndicatorCheck,
        advanced_label_x,
        options_y + 72,
        std::min(292, metrics.click_position_indicator_width),
        option_row_height);
    Set(result,
        Control::SafetyShieldCheck,
        338,
        options_y + 50,
        std::min(296, metrics.safety_shield_width),
        option_row_height);
    Set(result,
        Control::ForceExitOnEmergencyStopCheck,
        338,
        options_y + 72,
        std::min(296, metrics.force_exit_on_emergency_stop_width),
        option_row_height);
    Set(result,
        Control::CaptureExclusionCheck,
        advanced_label_x,
        options_y + 94,
        std::min(advanced_card_width - (advanced_label_x * 2),
                 metrics.capture_exclusion_width),
        option_row_height);
    Set(result,
        Control::KeepOnTopCheck,
        338,
        options_y + 94,
        std::min(296, metrics.keep_on_top_width),
        option_row_height);
    Set(result,
        Control::ProfileLabel,
        advanced_label_x,
        options_y + 120,
        132,
        20);
    Set(result,
        Control::ProfileCombo,
        advanced_label_x,
        options_y + 142,
        performance_combo_width,
        180);
    constexpr int import_settings_width = 236;
    Set(result,
        Control::ManageProfilesButton,
        338,
        options_y + 138,
        import_settings_width,
        34);
    Set(result,
        Control::RememberSettingsCheck,
        advanced_label_x,
        options_y + 188,
        std::min(240, metrics.remember_settings_width),
        option_row_height);
    Set(result,
        Control::ImportSettingsButton,
        338,
        options_y + 180,
        import_settings_width,
        34);

    // Match the About cards to the same visual right edge as the Basic and
    // Advanced cards instead of extending into the page host's two-pixel gutter.
    constexpr int about_card_width = card_envelope_width;
    constexpr int about_card_height = 174;
    Set(result, Control::AboutIdentityGroup, 0, 0, about_card_width, about_card_height);
    Set(result, Control::AboutNameText, 20, 48, 390, 32);
    Set(result, Control::AboutVersionText, 20, 80, 390, 24);
    Set(result, Control::AboutDescriptionText, 20, 108, 392, 50);
    Set(result, Control::AboutDetailsText, about_card_width - 26 - 208, 50, 208, 90);

    constexpr int about_links_y = about_card_height + card_gap;
    constexpr int about_links_height = about_page_height - about_links_y - 2;
    Set(result, Control::AboutLinksGroup, 0, about_links_y, about_card_width, about_links_height);
    constexpr int about_links_padding = 18;
    Set(result,
        Control::AboutLinksDescriptionText,
        about_links_padding,
        about_links_y + 44,
        about_card_width - (about_links_padding * 2),
        34);
    Set(result,
        Control::AboutSupportText,
        about_links_padding,
        about_links_y + 80,
        about_card_width - (about_links_padding * 2),
        24);
    constexpr int about_button_width = 304;
    constexpr int about_button_height = 34;
    constexpr int about_right_button_x =
        about_card_width - about_links_padding - about_button_width;
    Set(result,
        Control::OfficialDownloadsButton,
        about_links_padding,
        about_links_y + 112,
        about_button_width,
        about_button_height);
    Set(result,
        Control::SourceCodeButton,
        about_right_button_x,
        about_links_y + 112,
        about_button_width,
        about_button_height);
    Set(result,
        Control::ViewLicenseButton,
        about_links_padding,
        about_links_y + 152,
        about_button_width,
        about_button_height);
    Set(result,
        Control::CopySupportEmailButton,
        about_right_button_x,
        about_links_y + 152,
        about_button_width,
        about_button_height);
    Set(result,
        Control::CopyDiagnosticReportButton,
        about_links_padding,
        about_links_y + 192,
        about_button_width,
        about_button_height);
    Set(result,
        Control::ReportBugButton,
        about_right_button_x,
        about_links_y + 192,
        about_button_width,
        about_button_height);

    const int status_y = page_y + selected_page_height + 10;
    Set(result,
        Control::StatusText,
        StatusPanelHorizontalInset,
        status_y,
        StatusPanelWidth,
        normalized_status_height);
    Set(result,
        Control::DiagnosticsText,
        StatusPanelHorizontalInset,
        status_y,
        StatusPanelWidth,
        normalized_status_height);

    const int button_y = status_y + normalized_status_height + 10;
    constexpr int button_height = 48;
    constexpr int button_gap = 10;
    constexpr int normal_button_width = 196;
    constexpr int emergency_button_width = 240;
    int button_x = 12;
    Set(result, Control::StartButton, button_x, button_y, normal_button_width, button_height);
    button_x += normal_button_width + button_gap;
    Set(result, Control::StopButton, button_x, button_y, normal_button_width, button_height);
    button_x += normal_button_width + button_gap;
    Set(result, Control::EmergencyButton, button_x, button_y, emergency_button_width, button_height);

    return result;
}

Rect CalculateCenteredContentHost(const int client_width_pixels,
                                  const int client_height_pixels,
                                  const unsigned int dpi,
                                  const int status_height,
                                  const Page selected_page,
                                  const int advanced_viewport_height) noexcept {
    const int host_width = ScaleForDpi(FixedContentWidth, dpi);
    const int host_height = ScaleForDpi(
        ContentHeightForViewport(
            status_height,
            VisiblePageHeight(selected_page, advanced_viewport_height)),
        dpi);
    const int minimum_margin = ScaleForDpi(MinimumContentMargin, dpi);
    const int vertical_bias = ScaleForDpi(ContentVerticalBias, dpi);
    const int centered_y = (client_height_pixels - host_height) / 2;
    return {
        std::max(minimum_margin, (client_width_pixels - host_width) / 2),
        std::max(minimum_margin, centered_y - vertical_bias),
        host_width,
        host_height,
    };
}

Rect CalculateClampedContentHost(const int client_width_pixels,
                                 const int client_height_pixels,
                                 const unsigned int dpi,
                                 const int status_height,
                                 const Page selected_page,
                                 const int preferred_x_pixels,
                                 const int preferred_y_pixels,
                                 const int advanced_viewport_height) noexcept {
    const int host_width = ScaleForDpi(FixedContentWidth, dpi);
    const int host_height = ScaleForDpi(
        ContentHeightForViewport(
            status_height,
            VisiblePageHeight(selected_page, advanced_viewport_height)),
        dpi);
    const int minimum_margin = ScaleForDpi(MinimumContentMargin, dpi);
    return {
        std::clamp(preferred_x_pixels,
                   minimum_margin,
                   std::max(minimum_margin,
                            client_width_pixels - host_width - minimum_margin)),
        std::clamp(preferred_y_pixels,
                   minimum_margin,
                   std::max(minimum_margin,
                            client_height_pixels - host_height - minimum_margin)),
        host_width,
        host_height,
    };
}

} // namespace vectorclick::win::layout
