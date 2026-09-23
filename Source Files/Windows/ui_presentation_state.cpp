#include "Windows/ui_presentation_state.h"

#include <algorithm>
#include <bit>
#include <charconv>
#include <limits>
#include <utility>

namespace vectorclick::win {
namespace {

template <typename T>
void AppendInteger(std::wstring& output, const T value) {
    char buffer[32]{};
    const auto result = std::to_chars(buffer, buffer + sizeof(buffer), value);
    if (result.ec == std::errc{}) {
        output.append(buffer, result.ptr);
    }
}

void AppendFixed(std::wstring& output, const double value, const int precision) {
    // The presentation layer only requests one or two fixed decimal places.
    // Keep that narrow contract locale-independent without pulling libc++'s
    // generic floating-point charconv tables into the static Windows image.
    static_assert(sizeof(double) == sizeof(std::uint64_t));
    static_assert(std::numeric_limits<double>::is_iec559);

    if (precision != 1 && precision != 2) {
        return;
    }

    constexpr std::uint64_t fraction_mask = (std::uint64_t{1} << 52U) - 1U;
    const std::uint64_t bits = std::bit_cast<std::uint64_t>(value);
    const bool negative = (bits >> 63U) != 0U;
    const std::uint64_t exponent_bits = (bits >> 52U) & 0x7ffU;
    const std::uint64_t fraction_bits = bits & fraction_mask;

    if (exponent_bits == 0x7ffU) {
        if (negative) {
            output.push_back(L'-');
        }
        output += fraction_bits == 0U ? L"inf" : L"nan";
        return;
    }

    std::uint64_t significand = fraction_bits;
    int exponent = -1074;
    if (exponent_bits != 0U) {
        significand |= std::uint64_t{1} << 52U;
        exponent = static_cast<int>(exponent_bits) - 1023 - 52;
    }

    const std::uint64_t scale = precision == 1 ? 10U : 100U;
    const std::uint64_t scaled_significand = significand * scale;
    std::uint64_t rounded = 0U;

    if (exponent >= 0) {
        if (exponent >= 64 ||
            scaled_significand >
                (std::numeric_limits<std::uint64_t>::max() >> exponent)) {
            return;
        }
        rounded = scaled_significand << exponent;
    } else {
        const int shift = -exponent;
        if (shift < 64) {
            rounded = scaled_significand >> shift;
            const std::uint64_t mask = (std::uint64_t{1} << shift) - 1U;
            const std::uint64_t remainder = scaled_significand & mask;
            const std::uint64_t halfway = std::uint64_t{1} << (shift - 1);
            if (remainder > halfway ||
                (remainder == halfway && (rounded & 1U) != 0U)) {
                ++rounded;
            }
        }
    }

    if (negative) {
        output.push_back(L'-');
    }
    AppendInteger(output, rounded / scale);
    output.push_back(L'.');
    const std::uint64_t fraction = rounded % scale;
    if (precision == 2) {
        output.push_back(static_cast<wchar_t>(L'0' + (fraction / 10U)));
    }
    output.push_back(static_cast<wchar_t>(L'0' + (fraction % 10U)));
}

std::wstring FormatUnitsPerSecondFromDuration(
    const std::uint64_t duration_microseconds,
    const std::uint32_t units_per_duration) {
    if (duration_microseconds == 0U || units_per_duration == 0U) {
        return L"0.00";
    }

    // Preserve the established display precision exactly: high rates
    // use one decimal place, while slower rates use two.
    const std::size_t precision = duration_microseconds <= 10'000U ? 1U : 2U;
    const std::uint64_t scale = precision == 1U ? 10U : 100U;

    const std::uint64_t numerator =
        1'000'000U * scale * static_cast<std::uint64_t>(units_per_duration);
    std::uint64_t scaled_rate = numerator / duration_microseconds;
    const std::uint64_t remainder = numerator % duration_microseconds;
    if (remainder != 0U && remainder >= duration_microseconds - remainder) {
        ++scaled_rate;
    }

    const std::uint64_t whole = scaled_rate / scale;
    const std::uint64_t fractional = scaled_rate % scale;
    std::wstring result = std::to_wstring(whole);
    result.push_back(L'.');
    const std::wstring fractional_text = std::to_wstring(fractional);
    result.append(precision - fractional_text.size(), L'0');
    result.append(fractional_text);
    return result;
}

std::wstring GeneratedInputText(const bool keyboard) {
    return keyboard ? L"Generated key presses" : L"Generated clicks";
}

} // namespace

RatePresentation BuildRatePresentation(const RatePresentationInput& input) {
    if (input.action_pattern == core::ActionPattern::Hold) {
        if (!input.run_time_limit_valid) {
            return {
                L"Check time limit values",
                L"Run time limit must use valid Hours, Minutes, and Seconds values.",
            };
        }
        if (input.run_time_limit_microseconds > 0U) {
            return {
                L"Held until time limit / Stop",
                L"Hold keeps the selected mouse button or keyboard key down until the configured run time limit, Stop, Emergency Stop, shutdown, or a target failure releases it.",
            };
        }
        return {
            L"Held until Stop / Emergency Stop",
            L"Hold keeps the selected mouse button or keyboard key down until Stop, Emergency Stop, shutdown, or a target failure releases it.",
        };
    }

    std::uint64_t minimum_interval_microseconds = input.interval_microseconds;
    std::uint64_t maximum_interval_microseconds = input.interval_microseconds;
    if (input.randomize_interval) {
        if (!input.minimum_interval_valid || !input.maximum_interval_valid) {
            return {
                L"Invalid random interval range",
                L"Minimum interval and Maximum interval must be valid non-negative millisecond values.",
            };
        }
        if (input.minimum_interval_microseconds == 0U) {
            return {
                L"Minimum interval must be greater than 0",
                L"Enter a Minimum interval greater than 0 milliseconds before starting.",
            };
        }
        if (input.maximum_interval_microseconds <
            input.minimum_interval_microseconds) {
            return {
                L"Maximum interval is below minimum",
                L"Maximum interval cannot be lower than Minimum interval.",
            };
        }
        minimum_interval_microseconds = input.minimum_interval_microseconds;
        maximum_interval_microseconds = input.maximum_interval_microseconds;
    } else {
        if (!input.interval_valid) {
            return {
                L"Check interval values",
                L"Input interval must use valid Minutes, Seconds, and Milliseconds values.",
            };
        }
        if (input.interval_microseconds == 0U) {
            return {
                L"Interval must be greater than 0",
                L"Enter an Input interval greater than 0 milliseconds before starting.",
            };
        }
    }

    if (!input.down_duration_valid) {
        return {
            L"Invalid down duration",
            L"Down duration must be a valid non-negative number of milliseconds.",
        };
    }
    if (input.down_duration_microseconds > minimum_interval_microseconds) {
        return {
            input.randomize_interval
                ? L"Down duration exceeds minimum interval"
                : L"Down duration exceeds interval",
            input.randomize_interval
                ? L"Down duration cannot be longer than Minimum interval. Shorten Down duration or increase Minimum interval."
                : L"Down duration cannot be longer than Input interval. Shorten Down duration or increase Input interval.",
        };
    }

    std::uint32_t inputs_per_action = 1U;
    switch (input.action_pattern) {
    case core::ActionPattern::Single:
        break;
    case core::ActionPattern::Double:
        inputs_per_action = 2U;
        break;
    case core::ActionPattern::Triple:
        inputs_per_action = 3U;
        break;
    case core::ActionPattern::Burst:
        if (!input.burst_count_valid ||
            input.burst_count < core::MinimumBurstCount ||
            input.burst_count > core::MaximumBurstCount) {
            return {
                L"Burst count must be 2 through 100",
                L"Enter a Burst count from 2 through 100.",
            };
        }
        inputs_per_action = input.burst_count;
        break;
    case core::ActionPattern::Hold:
        break;
    }

    if (inputs_per_action > 1U) {
        if (!input.action_spacing_valid || input.action_spacing_microseconds == 0U) {
            return {
                L"Action spacing must be greater than 0",
                L"Enter an Action spacing greater than 0 milliseconds for Double, Triple, or Burst.",
            };
        }
        if (input.down_duration_microseconds > input.action_spacing_microseconds) {
            return {
                L"Down duration exceeds action spacing",
                L"Down duration cannot be longer than Action spacing. Shorten Down duration or increase Action spacing.",
            };
        }
    }

    if (!input.run_time_limit_valid) {
        return {
            L"Check time limit values",
            L"Run time limit must use valid Hours, Minutes, and Seconds values.",
        };
    }

    const std::wstring maximum_configured_rate =
        FormatUnitsPerSecondFromDuration(
            minimum_interval_microseconds, inputs_per_action);
    const std::wstring minimum_configured_rate =
        FormatUnitsPerSecondFromDuration(
            maximum_interval_microseconds, inputs_per_action);
    const std::wstring rate_prefix = input.keyboard ? L"Key rate: " : L"Click rate: ";
    const std::wstring rate_suffix = input.keyboard ? L" per second" : L" CPS";
    const std::wstring configured_rate =
        input.randomize_interval &&
                minimum_interval_microseconds != maximum_interval_microseconds
            ? minimum_configured_rate + L" to " + maximum_configured_rate
            : maximum_configured_rate;
    const std::wstring display_rate = rate_prefix + configured_rate + rate_suffix;

    const long double requested_rate =
        (1'000'000.0L * static_cast<long double>(inputs_per_action)) /
        static_cast<long double>(minimum_interval_microseconds);
    const long double timing_limit = input.down_duration_microseconds == 0U
                                         ? std::numeric_limits<long double>::infinity()
                                         : 1'000'000.0L /
                                               static_cast<long double>(input.down_duration_microseconds);
    const long double safety_limit =
        static_cast<long double>(core::MaximumGeneratedInputsPerSecond);
    const long double effective_limit = std::min(timing_limit, safety_limit);

    if (requested_rate <= effective_limit) {
        return {
            display_rate,
            input.randomize_interval
                ? (input.keyboard
                       ? L"Configured individual key-press rate range. Vector Click chooses a new inclusive interval between Minimum and Maximum before each action after the first. Double, Triple, and Burst can send several key presses per action. The receiving application may process fewer presses than Vector Click sends."
                       : L"Configured individual click-rate range. Vector Click chooses a new inclusive interval between Minimum and Maximum before each action after the first. Double, Triple, and Burst can send several clicks per action. The receiving application may process fewer clicks than Vector Click sends.")
                : (input.keyboard
                       ? L"Configured individual key presses per second. Double, Triple, and Burst can send several key presses per action. The receiving application may process fewer presses than Vector Click sends."
                       : L"Configured individual mouse clicks per second. Double, Triple, and Burst can send several clicks per action. The receiving application may process fewer clicks than Vector Click sends."),
        };
    }

    const bool safety_is_limit = safety_limit <= timing_limit;
    const std::uint64_t limit_duration_microseconds = safety_is_limit
        ? (1'000'000ULL + core::MaximumGeneratedInputsPerSecond - 1ULL) /
              core::MaximumGeneratedInputsPerSecond
        : input.down_duration_microseconds;
    const std::wstring maximum_rate =
        FormatUnitsPerSecondFromDuration(limit_duration_microseconds, 1U);
    const std::wstring limit_label = safety_is_limit ? L"Safety limit: " : L"Timing limit: ";
    const std::wstring limit_suffix = input.keyboard ? L" per second" : L" CPS";

    return {
        display_rate + L"\n" + limit_label + maximum_rate + limit_suffix,
        safety_is_limit
            ? (input.keyboard
                   ? L"The first line is the configured key-press rate. The second line is Vector Click's 10,000-input-per-second safety limit. The receiving application may process fewer presses than Vector Click sends."
                   : L"The first line is the configured click rate. The second line is Vector Click's 10,000-input-per-second safety limit. The receiving application may process fewer clicks than Vector Click sends.")
            : (input.keyboard
                   ? L"The first line is the configured key-press rate. The second line is the fastest rate allowed by the current Down duration. The receiving application may process fewer presses than Vector Click sends."
                   : L"The first line is the configured click rate. The second line is the fastest rate allowed by the current Down duration. The receiving application may process fewer clicks than Vector Click sends."),
    };
}

StartAvailabilityPresentation EvaluateStartAvailability(const StartAvailabilityInput& input) {
    const bool running = input.engine_state == PresentationEngineState::Running ||
                         input.engine_state == PresentationEngineState::Stopping;
    const bool start_enabled = input.safety_hotkeys_ready && input.settings_valid &&
                               input.target_ready && !input.cleanup_required &&
                               input.engine_state != PresentationEngineState::Faulted &&
                               !input.emergency_active;
    // Cleanup ownership is persistent safety state, not a transient visual
    // state. It must override a previously cached Ready snapshot even while
    // Emergency Stop is still latched or its shield is open.
    const bool update_cached_availability =
        input.cleanup_required ||
        (!running && input.engine_state != PresentationEngineState::Faulted &&
         !input.emergency_active);

    if (!update_cached_availability) {
        return {
            start_enabled,
            false,
            input.previous_initialized,
            input.previous_available,
            input.previous_unavailable_reason,
            false,
            input.cleanup_required,
        };
    }

    std::wstring unavailable_reason;
    if (input.cleanup_required) {
        unavailable_reason = L"Cleanup required before another run can start";
    } else if (!input.safety_hotkeys_ready) {
        unavailable_reason = input.safety_hotkey_reason.empty()
                                 ? L"Emergency Stop hotkey is unavailable"
                                 : input.safety_hotkey_reason;
    } else if (!input.settings_valid) {
        unavailable_reason = input.validation_error.empty()
                                 ? L"Check the highlighted settings"
                                 : input.validation_error;
    } else if (!input.target_ready) {
        unavailable_reason = L"The selected target window is unavailable";
    }

    const bool availability_changed =
        !input.previous_initialized || input.previous_available != start_enabled ||
        input.previous_unavailable_reason != unavailable_reason;

    return {
        start_enabled,
        true,
        true,
        start_enabled,
        std::move(unavailable_reason),
        availability_changed,
        input.cleanup_required,
    };
}

bool RequiresCleanupRecovery(const PresentationEngineState state,
                             const bool has_tracked_input) noexcept {
    // Tracked input is expected while a run is active or its ordinary cleanup
    // is still transitioning. It becomes a user-facing recovery condition only
    // after the engine reaches a terminal or idle state and ownership remains.
    return has_tracked_input &&
           state != PresentationEngineState::Running &&
           state != PresentationEngineState::Stopping;
}

StatusCategory StatusCategoryForEngineState(const PresentationEngineState state) noexcept {
    switch (state) {
    case PresentationEngineState::Ready:
        return StatusCategory::Ready;
    case PresentationEngineState::Running:
        return StatusCategory::Running;
    case PresentationEngineState::Stopping:
    case PresentationEngineState::Disarmed:
        return StatusCategory::Transition;
    case PresentationEngineState::Faulted:
        return StatusCategory::Attention;
    }
    return StatusCategory::Attention;
}

std::wstring StateText(const PresentationEngineState state) {
    switch (state) {
    case PresentationEngineState::Ready:
        return L"Ready";
    case PresentationEngineState::Running:
        return L"Running";
    case PresentationEngineState::Stopping:
        return L"Stopping...";
    case PresentationEngineState::Disarmed:
        return L"Emergency stop complete";
    case PresentationEngineState::Faulted:
        return L"Input stopped with an error";
    }
    return L"Unknown";
}

std::wstring ActionUnitText(const core::ActionPattern pattern, const bool keyboard) {
    switch (pattern) {
    case core::ActionPattern::Single:
        return keyboard ? L"key presses" : L"clicks";
    case core::ActionPattern::Double:
    case core::ActionPattern::Triple:
    case core::ActionPattern::Burst:
        return L"actions";
    case core::ActionPattern::Hold:
        return L"holds";
    }
    return L"actions";
}

StatusPresentation BuildStatusPresentation(const StatusPresentationInput& input) {
    const bool idle = input.engine_state == PresentationEngineState::Ready ||
                      input.engine_state == PresentationEngineState::Disarmed;
    if (idle && input.availability.attention_required) {
        std::wstring text = L"Status: Attention required";
        if (!input.availability.unavailable_reason.empty()) {
            text += L" | " + input.availability.unavailable_reason;
        }
        return {
            StatusCategory::Attention,
            std::move(text),
            L"Vector Click still tracks an input that did not receive a confirmed release. Resume or unblock the target, then use Retry cleanup. Press Emergency Stop to open the recovery Safety Shield and access Force Stop and Exit.",
        };
    }
    if (idle && input.availability.initialized && !input.availability.available) {
        std::wstring text = L"Status: Not ready";
        if (!input.availability.unavailable_reason.empty()) {
            text += L" | " + input.availability.unavailable_reason;
        }
        return {
            StatusCategory::NotReady,
            std::move(text),
            L"Vector Click cannot start until the listed setting or target issue is corrected.",
        };
    }

    std::wstring text = L"Status: " + StateText(input.engine_state);
    if (idle && !input.completion_note.empty()) {
        text += L" | ";
        text += input.completion_note;
    }
    if (idle && !input.diagnostics_enabled) {
        text += L" | Completed " + ActionUnitText(input.action_pattern, input.keyboard) + L": ";
        text += std::to_wstring(input.completed_actions);
    }

    return {
        StatusCategoryForEngineState(input.engine_state),
        std::move(text),
        L"Shows the current run state. Multi-input actions count as complete only after every input finishes. Hold counts after its release finishes.",
    };
}

DiagnosticsPresentation BuildDiagnosticsPresentation(const DiagnosticsPresentationInput& input) {
    const bool idle = input.engine_state == PresentationEngineState::Ready ||
                      input.engine_state == PresentationEngineState::Disarmed;
    std::wstring output;
    output.reserve(192);
    StatusCategory category = StatusCategoryForEngineState(input.engine_state);

    if (idle && input.availability.attention_required) {
        category = StatusCategory::Attention;
        output = L"Attention required";
        if (!input.availability.unavailable_reason.empty()) {
            output += L" | ";
            output += input.availability.unavailable_reason;
        }
        output += L"\nLast run: ";
        AppendInteger(output, input.completed_actions);
        output += L" actions | ";
        AppendInteger(output, input.generated_inputs);
        output += input.keyboard ? L" key presses | " : L" clicks | ";
        AppendFixed(output, input.actual_inputs_per_second, 1);
        output += input.keyboard ? L" per second" : L" CPS";
    } else if (idle && input.availability.initialized && !input.availability.available) {
        category = StatusCategory::NotReady;
        output = L"Not ready";
        if (!input.availability.unavailable_reason.empty()) {
            output += L" | ";
            output += input.availability.unavailable_reason;
        }
        output += L"\nLast run: ";
        AppendInteger(output, input.completed_actions);
        output += L" actions | ";
        AppendInteger(output, input.generated_inputs);
        output += input.keyboard ? L" key presses | " : L" clicks | ";
        AppendFixed(output, input.actual_inputs_per_second, 1);
        output += input.keyboard ? L" per second" : L" CPS";
    } else if (input.action_pattern == core::ActionPattern::Hold) {
        const bool active = input.engine_state == PresentationEngineState::Running &&
                            input.generated_inputs > 0U;
        output = StateText(input.engine_state);
        if (idle && !input.completion_note.empty()) {
            output += L" | ";
            output += input.completion_note;
        }
        output += L" | Hold active: ";
        output += active ? L"Yes" : L"No";
        output += L" | ";
        output += GeneratedInputText(input.keyboard);
        output += L": ";
        AppendInteger(output, input.generated_inputs);
        output += L"\nCompleted holds: ";
        AppendInteger(output, input.completed_actions);
        output += L" | Elapsed: ";
        AppendFixed(output, input.elapsed_seconds, 2);
        output += L" s";
    } else {
        output = StateText(input.engine_state);
        if (idle && !input.completion_note.empty()) {
            output += L" | ";
            output += input.completion_note;
        }
        output += L" | Completed actions: ";
        AppendInteger(output, input.completed_actions);
        output += L" | ";
        output += GeneratedInputText(input.keyboard);
        output += L": ";
        AppendInteger(output, input.generated_inputs);
        output += L"\nAction rate: ";
        AppendFixed(output, input.actual_actions_per_second, 1);
        output += L"/s | ";
        output += input.keyboard ? L"Key rate: " : L"Click rate: ";
        AppendFixed(output, input.actual_inputs_per_second, 1);
        output += input.keyboard ? L"/s" : L" CPS";
        output += L" | Elapsed: ";
        AppendFixed(output, input.elapsed_seconds, 2);
        output += L" s";
    }

    return {category, std::move(output)};
}

} // namespace vectorclick::win
