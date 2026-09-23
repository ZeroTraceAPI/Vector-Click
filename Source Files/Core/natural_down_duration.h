#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>

namespace vectorclick::core {

// Press-duration timing model derived from recorded physical mouse-button and
// keyboard key-dwell measurements. Mouse and keyboard share one deterministic
// architecture with small input-specific calibration differences. The generator
// has its own state so enabling it cannot consume or perturb the established
// input-interval random sequence.
class NaturalDownDurationGenerator final {
public:
    explicit constexpr NaturalDownDurationGenerator(
        const std::uint64_t seed = 0) noexcept
        : state_(seed) {}

    constexpr void Reseed(const std::uint64_t seed) noexcept {
        state_ = seed;
        pace_q20_ = 0;
        slow_pace_q20_ = 0;
    }

    [[nodiscard]] static constexpr std::uint64_t AutomaticBaselineCenter(
        const std::uint64_t tempo_reference_microseconds) noexcept {
        if (tempo_reference_microseconds == 0U) {
            return 0U;
        }

        // Compact integer approximation of the robust saturating relationship
        // measured across the balanced person / hand / preset groups. The result is
        // a generic population center, not a prediction of one person's exact
        // physical press duration.
        struct Anchor final {
            std::uint64_t interval;
            std::uint64_t down;
        };
        constexpr std::array<Anchor, 7> anchors{{
            {50'000U, 41'000U},
            {100'000U, 46'000U},
            {150'000U, 66'000U},
            {200'000U, 97'000U},
            {250'000U, 124'000U},
            {300'000U, 141'000U},
            {400'000U, 154'000U},
        }};

        if (tempo_reference_microseconds <= anchors.front().interval) {
            return anchors.front().down;
        }
        if (tempo_reference_microseconds >= 600'000U) {
            return 160'000U;
        }
        if (tempo_reference_microseconds >= anchors.back().interval) {
            return anchors.back().down +
                   ((tempo_reference_microseconds - anchors.back().interval) *
                    6'000U) /
                       200'000U;
        }

        for (std::size_t index = 1; index < anchors.size(); ++index) {
            if (tempo_reference_microseconds <= anchors[index].interval) {
                const std::uint64_t interval_span =
                    anchors[index].interval - anchors[index - 1U].interval;
                const std::uint64_t interval_offset =
                    tempo_reference_microseconds - anchors[index - 1U].interval;
                const std::uint64_t down_span =
                    anchors[index].down - anchors[index - 1U].down;
                return anchors[index - 1U].down +
                       (down_span * interval_offset) / interval_span;
            }
        }
        return anchors.back().down;
    }

    [[nodiscard]] static constexpr std::uint64_t KeyboardAutomaticBaselineCenter(
        const std::uint64_t tempo_reference_microseconds) noexcept {
        if (tempo_reference_microseconds == 0U) {
            return 0U;
        }

        struct Anchor final {
            std::uint64_t interval;
            std::uint64_t down;
        };
        constexpr std::array<Anchor, 7> anchors{{
            {50'000U, 39'000U},
            {100'000U, 45'000U},
            {150'000U, 62'000U},
            {200'000U, 87'000U},
            {250'000U, 108'000U},
            {300'000U, 122'000U},
            {400'000U, 130'000U},
        }};

        if (tempo_reference_microseconds <= anchors.front().interval) {
            return anchors.front().down;
        }
        if (tempo_reference_microseconds >= anchors.back().interval) {
            return anchors.back().down;
        }
        for (std::size_t index = 1; index < anchors.size(); ++index) {
            if (tempo_reference_microseconds <= anchors[index].interval) {
                const std::uint64_t interval_span =
                    anchors[index].interval - anchors[index - 1U].interval;
                const std::uint64_t interval_offset =
                    tempo_reference_microseconds - anchors[index - 1U].interval;
                const std::uint64_t down_span =
                    anchors[index].down - anchors[index - 1U].down;
                return anchors[index - 1U].down +
                       (down_span * interval_offset) / interval_span;
            }
        }
        return anchors.back().down;
    }

    [[nodiscard]] std::uint64_t NextConfigured(
        const std::uint64_t configured_center_microseconds,
        const std::uint64_t timing_limit_microseconds,
        const std::uint64_t tempo_reference_microseconds,
        const std::uint64_t upcoming_interval_microseconds) noexcept {
        return Next(
            configured_center_microseconds,
            timing_limit_microseconds,
            tempo_reference_microseconds,
            upcoming_interval_microseconds,
            false,
            Q20One);
    }

    [[nodiscard]] std::uint64_t NextAutomatic(
        const std::uint64_t timing_limit_microseconds,
        const std::uint64_t tempo_reference_microseconds,
        const std::uint64_t upcoming_interval_microseconds) noexcept {
        return Next(
            AutomaticBaselineCenter(tempo_reference_microseconds),
            timing_limit_microseconds,
            tempo_reference_microseconds,
            upcoming_interval_microseconds,
            true,
            Q20One);
    }

    [[nodiscard]] std::uint64_t NextConfiguredKeyboard(
        const std::uint64_t configured_center_microseconds,
        const std::uint64_t timing_limit_microseconds,
        const std::uint64_t tempo_reference_microseconds,
        const std::uint64_t upcoming_interval_microseconds) noexcept {
        return Next(
            configured_center_microseconds,
            timing_limit_microseconds,
            tempo_reference_microseconds,
            upcoming_interval_microseconds,
            false,
            KeyboardVariationScaleQ20);
    }

    [[nodiscard]] std::uint64_t NextAutomaticKeyboard(
        const std::uint64_t timing_limit_microseconds,
        const std::uint64_t tempo_reference_microseconds,
        const std::uint64_t upcoming_interval_microseconds) noexcept {
        return Next(
            KeyboardAutomaticBaselineCenter(tempo_reference_microseconds),
            timing_limit_microseconds,
            tempo_reference_microseconds,
            upcoming_interval_microseconds,
            true,
            KeyboardVariationScaleQ20);
    }

private:
    static constexpr std::int64_t Q20One = 1LL << 20;
    static constexpr std::int64_t KeyboardVariationScaleQ20 = 891'290; // 0.85
    static constexpr std::int64_t FastRhoQ20 = 922'747;   // about 0.88
    static constexpr std::int64_t SlowRhoQ20 = 1'034'945; // about 0.987

    [[nodiscard]] static constexpr std::int64_t MultiplyQ20(
        const std::int64_t left,
        const std::int64_t right) noexcept {
        return (left * right) >> 20;
    }

    [[nodiscard]] static constexpr std::uint64_t FractionToQ20(
        const std::uint64_t numerator,
        const std::uint64_t denominator) noexcept {
        constexpr std::uint64_t q20_one = 1ULL << 20;
        if (denominator == 0U || numerator >= denominator) {
            return q20_one;
        }

        std::uint64_t remainder = numerator;
        std::uint64_t result = 0;
        for (int bit = 0; bit < 20; ++bit) {
            result <<= 1U;
            const std::uint64_t complement = denominator - remainder;
            if (remainder >= complement) {
                remainder -= complement;
                result |= 1U;
            } else {
                remainder += remainder;
            }
        }
        return result;
    }

    [[nodiscard]] static constexpr std::uint64_t ScaleUnsignedQ20(
        const std::uint64_t value,
        const std::uint64_t factor_q20) noexcept {
        constexpr std::uint64_t q20_one = 1ULL << 20;
        constexpr std::uint64_t q20_mask = q20_one - 1U;
        const std::uint64_t whole = value >> 20U;
        if (factor_q20 != 0U &&
            whole > std::numeric_limits<std::uint64_t>::max() / factor_q20) {
            return std::numeric_limits<std::uint64_t>::max();
        }
        const std::uint64_t high = whole * factor_q20;
        const std::uint64_t low =
            ((value & q20_mask) * factor_q20) >> 20U;
        if (high > std::numeric_limits<std::uint64_t>::max() - low) {
            return std::numeric_limits<std::uint64_t>::max();
        }
        return high + low;
    }

    [[nodiscard]] static constexpr std::uint64_t ConstrainAutomaticCenter(
        const std::uint64_t baseline_center_microseconds,
        const std::uint64_t timing_limit_microseconds) noexcept {
        if (baseline_center_microseconds == 0U ||
            timing_limit_microseconds == 0U) {
            return 0U;
        }

        // Automatic mode may move its inferred center so roughly one-third of
        // an unusually tight local envelope remains released. Configured-center
        // mode does not use this rule: it preserves the user's requested center
        // and attenuates variation near hard boundaries instead.
        const std::uint64_t envelope_center =
            (timing_limit_microseconds * 2U) / 3U;
        return std::max<std::uint64_t>(
            1U,
            std::min(baseline_center_microseconds, envelope_center));
    }

    [[nodiscard]] static constexpr std::uint64_t ConditionCenterForUpcomingInterval(
        const std::uint64_t base_center_microseconds,
        const std::uint64_t tempo_reference_microseconds,
        const std::uint64_t upcoming_interval_microseconds) noexcept {
        if (base_center_microseconds == 0U ||
            tempo_reference_microseconds == 0U ||
            upcoming_interval_microseconds == 0U) {
            return base_center_microseconds;
        }

        // Recorded press durations couple more strongly to the upcoming interval
        // during genuinely fast input, while ordinary and slow input show
        // much weaker coupling. Transition smoothly from 0.60 through 220 ms to
        // 0.30 at 400 ms and above instead of applying one fixed percentage at
        // every tempo.
        std::int64_t coupling_q20 = 314'573; // 0.30
        if (tempo_reference_microseconds <= 220'000U) {
            coupling_q20 = 629'146; // 0.60
        } else if (tempo_reference_microseconds < 400'000U) {
            coupling_q20 = 629'146 - static_cast<std::int64_t>(
                ((tempo_reference_microseconds - 220'000U) * 314'573U) /
                180'000U);
        }
        constexpr std::int64_t relative_limit_q20 = 786'432; // 0.75

        std::int64_t relative_q20 = 0;
        if (upcoming_interval_microseconds >= tempo_reference_microseconds) {
            const std::uint64_t delta =
                upcoming_interval_microseconds - tempo_reference_microseconds;
            relative_q20 = static_cast<std::int64_t>(FractionToQ20(
                std::min(delta, tempo_reference_microseconds),
                tempo_reference_microseconds));
        } else {
            const std::uint64_t delta =
                tempo_reference_microseconds - upcoming_interval_microseconds;
            relative_q20 = -static_cast<std::int64_t>(FractionToQ20(
                std::min(delta, tempo_reference_microseconds),
                tempo_reference_microseconds));
        }

        relative_q20 = std::clamp(
            relative_q20, -relative_limit_q20, relative_limit_q20);
        const std::int64_t factor_q20 =
            Q20One + MultiplyQ20(relative_q20, coupling_q20);
        // A positive configured center must remain positive after fixed-point
        // scaling. Extremely small centers (notably 1 microsecond) can otherwise
        // round down to zero when the upcoming interval is much shorter than the
        // tempo reference.
        return std::max<std::uint64_t>(
            1U,
            ScaleUnsignedQ20(
                base_center_microseconds,
                static_cast<std::uint64_t>(
                    std::max<std::int64_t>(1, factor_q20))));
    }

    [[nodiscard]] std::uint64_t Next(
        std::uint64_t center_microseconds,
        const std::uint64_t timing_limit_microseconds,
        const std::uint64_t tempo_reference_microseconds,
        const std::uint64_t upcoming_interval_microseconds,
        const bool automatic_center,
        const std::int64_t input_variation_scale_q20) noexcept {
        if (center_microseconds == 0U || timing_limit_microseconds == 0U) {
            return 0U;
        }

        center_microseconds = ConditionCenterForUpcomingInterval(
            center_microseconds,
            tempo_reference_microseconds,
            upcoming_interval_microseconds);
        center_microseconds = automatic_center
                                  ? ConstrainAutomaticCenter(
                                        center_microseconds,
                                        timing_limit_microseconds)
                                  : std::min(
                                        center_microseconds,
                                        timing_limit_microseconds);
        if (center_microseconds == 0U) {
            return 0U;
        }

        // Slow physical input showed broader press-duration variation without
        // enough evidence for a separate persistence regime. Introduce only a
        // gradual scale increase from 450 ms through 800 ms.
        std::int64_t slow_progress_q20 = 0;
        if (tempo_reference_microseconds > 450'000U) {
            const std::uint64_t delta = std::min<std::uint64_t>(
                tempo_reference_microseconds - 450'000U, 350'000U);
            slow_progress_q20 = static_cast<std::int64_t>(
                (delta << 20U) / 350'000U);
        }
        const std::int64_t pace_boost_q20 =
            Q20One + MultiplyQ20(slow_progress_q20, 104'858); // up to +10%
        const std::int64_t local_boost_q20 =
            Q20One + MultiplyQ20(slow_progress_q20, 367'002); // up to +35%

        std::int64_t fast_innovation_scale_q20 =
            MultiplyQ20(62'584, pace_boost_q20);
        std::int64_t slow_innovation_scale_q20 =
            MultiplyQ20(11'797, pace_boost_q20);
        std::int64_t core_scale_q20 =
            MultiplyQ20(110'100, local_boost_q20);
        std::int64_t tail_scale_q20 =
            MultiplyQ20(754'975, local_boost_q20);
        std::int64_t center_offset_q20 =
            MultiplyQ20(6'291, local_boost_q20);

        // Near a hard timing boundary, reduce every source of variation instead
        // of repeatedly clipping or shifting a user-configured center. This lets
        // configured-center mode remain faithful to the requested typical value.
        const std::uint64_t upper_room =
            timing_limit_microseconds - center_microseconds;
        const std::uint64_t symmetric_room =
            std::min(center_microseconds, upper_room);
        std::int64_t room_scale_q20 = Q20One;
        if (symmetric_room < center_microseconds / 2U) {
            const std::uint64_t room_fraction_q20 =
                FractionToQ20(symmetric_room, center_microseconds);
            room_scale_q20 = static_cast<std::int64_t>(
                std::min<std::uint64_t>(
                    room_fraction_q20 * 2U,
                    static_cast<std::uint64_t>(Q20One)));
        }
        fast_innovation_scale_q20 =
            MultiplyQ20(fast_innovation_scale_q20, room_scale_q20);
        slow_innovation_scale_q20 =
            MultiplyQ20(slow_innovation_scale_q20, room_scale_q20);
        core_scale_q20 = MultiplyQ20(core_scale_q20, room_scale_q20);
        tail_scale_q20 = MultiplyQ20(tail_scale_q20, room_scale_q20);
        center_offset_q20 = MultiplyQ20(center_offset_q20, room_scale_q20);

        fast_innovation_scale_q20 = MultiplyQ20(
            fast_innovation_scale_q20, input_variation_scale_q20);
        slow_innovation_scale_q20 = MultiplyQ20(
            slow_innovation_scale_q20, input_variation_scale_q20);
        core_scale_q20 = MultiplyQ20(
            core_scale_q20, input_variation_scale_q20);
        tail_scale_q20 = MultiplyQ20(
            tail_scale_q20, input_variation_scale_q20);
        center_offset_q20 = MultiplyQ20(
            center_offset_q20, input_variation_scale_q20);

        pace_q20_ = MultiplyQ20(pace_q20_, FastRhoQ20) +
                    MultiplyQ20(
                        ApproximateNormalQ20(),
                        fast_innovation_scale_q20);
        slow_pace_q20_ = MultiplyQ20(slow_pace_q20_, SlowRhoQ20) +
                         MultiplyQ20(
                             ApproximateNormalQ20(),
                             slow_innovation_scale_q20);
        pace_q20_ = std::clamp<std::int64_t>(
            pace_q20_, -367'002, 367'002); // +/-35%
        slow_pace_q20_ = std::clamp<std::int64_t>(
            slow_pace_q20_, -262'144, 262'144); // +/-25%

        constexpr std::uint32_t attempts = 8;
        for (std::uint32_t attempt = 0; attempt < attempts; ++attempt) {
            std::int64_t local_noise_q20 = MultiplyQ20(
                ApproximateNormalQ20(), core_scale_q20);
            local_noise_q20 -= center_offset_q20;

            const std::uint64_t tail_bits = NextRaw();
            if ((tail_bits & ((1ULL << 20U) - 1ULL)) < 52'429ULL) {
                const std::uint64_t unit = (tail_bits >> 20U) & 0xFFFFULL;
                const std::uint64_t squared = unit * unit;
                const std::int64_t magnitude_q20 =
                    static_cast<std::int64_t>(
                        (static_cast<std::uint64_t>(
                             std::max<std::int64_t>(0, tail_scale_q20)) *
                         squared) /
                        (0xFFFFULL * 0xFFFFULL));
                if (((tail_bits >> 36U) & 0x3ULL) != 0U) {
                    local_noise_q20 += magnitude_q20;
                } else {
                    local_noise_q20 -= magnitude_q20;
                }
            }

            const std::int64_t combined_pace_q20 =
                std::clamp<std::int64_t>(
                    pace_q20_ + slow_pace_q20_,
                    -524'288,
                    524'288); // +/-50%
            const std::int64_t active_pace_q20 =
                MultiplyQ20(combined_pace_q20, room_scale_q20);
            const std::int64_t factor_q20 = std::max<std::int64_t>(
                Q20One / 8,
                Q20One + active_pace_q20 + local_noise_q20);
            const std::uint64_t candidate = ScaleUnsignedQ20(
                center_microseconds,
                static_cast<std::uint64_t>(factor_q20));
            if (candidate >= 1U && candidate <= timing_limit_microseconds) {
                return candidate;
            }
        }

        // A pathological / narrow envelope can leave no useful natural range.
        // Preserve the safe center instead of pinning to arbitrary boundaries.
        return center_microseconds;
    }

    [[nodiscard]] std::int64_t ApproximateNormalQ20() noexcept {
        const std::uint64_t bits = NextRaw();
        std::int64_t sum = 0;
        for (unsigned shift = 0; shift < 48U; shift += 8U) {
            sum += static_cast<std::int64_t>((bits >> shift) & 0xFFULL);
        }
        return ((sum - 765LL) * Q20One) / 181LL;
    }

    [[nodiscard]] std::uint64_t NextRaw() noexcept {
        state_ += 0x9E3779B97F4A7C15ULL;
        std::uint64_t value = state_;
        value = (value ^ (value >> 30U)) * 0xBF58476D1CE4E5B9ULL;
        value = (value ^ (value >> 27U)) * 0x94D049BB133111EBULL;
        return value ^ (value >> 31U);
    }

    std::uint64_t state_{};
    std::int64_t pace_q20_{};
    std::int64_t slow_pace_q20_{};
};

} // namespace vectorclick::core
