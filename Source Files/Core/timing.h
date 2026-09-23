#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>

namespace vectorclick::core {

class IntervalRandomGenerator final {
public:
    explicit constexpr IntervalRandomGenerator(const std::uint64_t seed = 0) noexcept
        : state_(seed) {}

    constexpr void Reseed(const std::uint64_t seed) noexcept {
        state_ = seed;
        drift_initialized_ = false;
        drift_region_ = 0;
        drift_region_minimum_ = 0;
        drift_region_maximum_ = 0;
        drift_budget_remaining_ = 0;
        natural_pace_q20_ = 0;
        natural_slow_pace_q20_ = 0;
    }

    [[nodiscard]] constexpr std::uint64_t NextInclusive(
        const std::uint64_t minimum,
        const std::uint64_t maximum) noexcept {
        if (maximum <= minimum) {
            return minimum;
        }

        const std::uint64_t range = maximum - minimum + 1U;
        if (range == 0U) {
            return NextRaw();
        }

        const std::uint64_t threshold = (0U - range) % range;
        std::uint64_t value = 0;
        do {
            value = NextRaw();
        } while (value < threshold);
        return minimum + (value % range);
    }

    // Produces bounded fast, middle, and slow timing periods. Each period is
    // held for an irregular time budget instead of only a few actions, so
    // short runs do not immediately average back toward the range midpoint.
    // Every interval remains inside the configured inclusive limits, and equal
    // seeds remain deterministic so all input positions in one action consume
    // the same interval sequence.
    [[nodiscard]] constexpr std::uint64_t NextDriftingInclusive(
        const std::uint64_t minimum,
        const std::uint64_t maximum) noexcept {
        if (maximum <= minimum) {
            return minimum;
        }

        if (!drift_initialized_ || drift_budget_remaining_ == 0) {
            BeginDriftRegime(minimum, maximum);
        }

        const std::uint64_t result =
            NextInclusive(drift_region_minimum_, drift_region_maximum_);
        drift_budget_remaining_ = result >= drift_budget_remaining_
            ? 0
            : drift_budget_remaining_ - result;
        return result;
    }

    // Natural Variation timing model derived from recorded human mouse-click
    // sessions. Unlike Independent, this does not sample every delay
    // from the full range independently. Unlike Drifting, it does not jump
    // between discrete fast / middle / slow regions. A normal local pace drift is
    // gradually joined by a slower pace component as configured tempo slows,
    // while asymmetric local variation rides on top of the combined pace.
    //
    // The model is deterministic for a given seed and always honors the
    // inclusive configured bounds. It intentionally reproduces statistical
    // properties (pace persistence, right-skewed local noise, occasional
    // heavier tails), not any recorded human sequence.
    [[nodiscard]] std::uint64_t NextNaturalInclusive(
        const std::uint64_t minimum,
        const std::uint64_t maximum) noexcept {
        if (maximum <= minimum) {
            return minimum;
        }

        constexpr std::int64_t q20_one = 1LL << 20;
        constexpr std::int64_t pace_rho_q20 = 996'147;       // 0.95
        constexpr std::int64_t pace_innovation_fast_q20 = 29'468; // v0.1
        constexpr std::int64_t pace_innovation_slow_target_q20 = 22'183;
        constexpr std::int64_t slow_pace_rho_q20 = 1'043'333; // ~0.995
        constexpr std::int64_t slow_pace_innovation_target_q20 = 5'793;
        constexpr std::int64_t local_core_q20 = 95'420;      // 0.091
        constexpr std::int64_t positive_tail_q20 = 734'003;  // 0.70
        constexpr std::int64_t negative_tail_q20 = 373'293;  // 0.356
        constexpr std::int64_t local_center_offset_q20 = 25'740; // 0.024548
        constexpr std::uint64_t positive_tail_threshold =
            142'452; // 13.5853% of 2^20
        constexpr std::uint64_t negative_tail_threshold =
            63'026; // 6.0107% of 2^20
        constexpr std::uint32_t maximum_attempts = 8;

        const std::uint64_t span = maximum - minimum;
        const std::uint64_t center = minimum + span / 2U;
        const std::uint64_t half_span = span / 2U;

        // Full measured variability is used once either side of the configured
        // range is at least 25% of the center interval. Narrower user ranges
        // proportionally reduce the model's pace / noise amplitudes instead of
        // piling large numbers of samples onto the hard limits.
        const std::uint64_t quarter_center =
            center / 4U + (center % 4U == 0U ? 0U : 1U);
        const std::int64_t variation_scale_q20 =
            half_span >= quarter_center
                ? q20_one
                : static_cast<std::int64_t>(FractionToQ20(half_span * 4U, center));

        // Preserve the accepted mouse pace / noise model through a 250 ms
        // configured center. From there to 600 ms, smoothly introduce a
        // longer-memory pace component and measured slow-tempo variability.
        // The user's hard Minimum / Maximum range remains authoritative.
        constexpr std::uint64_t slow_transition_start = 250'000U;
        constexpr std::uint64_t slow_transition_full = 600'000U;
        constexpr std::int64_t local_boost_max_q20 = 419'430; // 0.40
        constexpr std::int64_t pace_boost_max_q20 = 943'718; // 0.90
        std::int64_t slow_progress_q20 = 0;
        if (center > slow_transition_start) {
            const std::uint64_t slow_delta = std::min<std::uint64_t>(
                center - slow_transition_start,
                slow_transition_full - slow_transition_start);
            slow_progress_q20 = static_cast<std::int64_t>(FractionToQ20(
                slow_delta, slow_transition_full - slow_transition_start));
        }

        const std::int64_t local_boost_q20 = MultiplyQ20(
            slow_progress_q20, local_boost_max_q20);
        const std::int64_t pace_boost_q20 = MultiplyQ20(
            slow_progress_q20, pace_boost_max_q20);
        const std::int64_t effective_local_scale_q20 = MultiplyQ20(
            variation_scale_q20,
            q20_one + MultiplyQ20(local_boost_q20, variation_scale_q20));
        const std::int64_t effective_pace_scale_q20 = MultiplyQ20(
            variation_scale_q20,
            q20_one + MultiplyQ20(pace_boost_q20, variation_scale_q20));

        const std::int64_t fast_innovation_q20 =
            pace_innovation_fast_q20 + MultiplyQ20(
                pace_innovation_slow_target_q20 - pace_innovation_fast_q20,
                slow_progress_q20);
        const std::int64_t slow_innovation_base_q20 = MultiplyQ20(
            slow_pace_innovation_target_q20, slow_progress_q20);

        const std::int64_t relative_half_q20 = static_cast<std::int64_t>(
            FractionToQ20(std::min(half_span, center), center));
        const std::int64_t pace_limit_q20 = std::min<std::int64_t>(
            (q20_one * 30) / 100,
            (relative_half_q20 * 60) / 100);

        const std::int64_t scaled_innovation_q20 =
            MultiplyQ20(fast_innovation_q20, effective_pace_scale_q20);
        const std::int64_t scaled_slow_innovation_q20 =
            MultiplyQ20(slow_innovation_base_q20, effective_pace_scale_q20);
        const std::int64_t scaled_core_q20 =
            MultiplyQ20(local_core_q20, effective_local_scale_q20);
        const std::int64_t scaled_positive_tail_q20 =
            MultiplyQ20(positive_tail_q20, effective_local_scale_q20);
        const std::int64_t scaled_negative_tail_q20 =
            MultiplyQ20(negative_tail_q20, effective_local_scale_q20);
        const std::int64_t scaled_center_offset_q20 =
            MultiplyQ20(local_center_offset_q20, effective_local_scale_q20);

        const std::int64_t pace_innovation = MultiplyQ20(
            ApproximateNormalQ20(), scaled_innovation_q20);
        std::int64_t slow_pace_innovation = 0;
        if (slow_progress_q20 != 0) {
            slow_pace_innovation = MultiplyQ20(
                ApproximateNormalQ20(), scaled_slow_innovation_q20);
        }
        natural_pace_q20_ =
            MultiplyQ20(natural_pace_q20_, pace_rho_q20) + pace_innovation;
        natural_slow_pace_q20_ =
            MultiplyQ20(natural_slow_pace_q20_, slow_pace_rho_q20) +
            slow_pace_innovation;
        natural_pace_q20_ = std::clamp(
            natural_pace_q20_, -pace_limit_q20, pace_limit_q20);
        natural_slow_pace_q20_ = std::clamp(
            natural_slow_pace_q20_, -pace_limit_q20, pace_limit_q20);

        std::uint64_t last_candidate = center;
        for (std::uint32_t attempt = 0; attempt < maximum_attempts; ++attempt) {
            std::int64_t local_noise_q20 = MultiplyQ20(
                ApproximateNormalQ20(), scaled_core_q20);
            local_noise_q20 -= scaled_center_offset_q20;

            const std::uint64_t tail_bits = NextRaw();
            if ((tail_bits & ((1ULL << 20) - 1ULL)) < positive_tail_threshold) {
                const std::uint64_t unit = (tail_bits >> 20U) & 0xFFFFULL;
                const std::uint64_t squared = unit * unit;
                local_noise_q20 += static_cast<std::int64_t>(
                    (static_cast<std::uint64_t>(scaled_positive_tail_q20) * squared) /
                    (0xFFFFULL * 0xFFFFULL));
            }
            if (((tail_bits >> 44U) & ((1ULL << 20) - 1ULL)) <
                negative_tail_threshold) {
                const std::uint64_t unit = (tail_bits >> 28U) & 0xFFFFULL;
                const std::uint64_t squared = unit * unit;
                local_noise_q20 -= static_cast<std::int64_t>(
                    (static_cast<std::uint64_t>(scaled_negative_tail_q20) * squared) /
                    (0xFFFFULL * 0xFFFFULL));
            }

            const std::int64_t active_slow_pace_q20 = MultiplyQ20(
                natural_slow_pace_q20_, slow_progress_q20);
            const std::int64_t combined_pace_q20 = std::clamp(
                natural_pace_q20_ + active_slow_pace_q20,
                -pace_limit_q20,
                pace_limit_q20);
            const std::int64_t pace_factor_q20 = q20_one + combined_pace_q20;
            const std::int64_t local_factor_q20 = std::max<std::int64_t>(
                q20_one / 8, q20_one + local_noise_q20);
            const std::int64_t combined_factor_q20 = MultiplyQ20(
                pace_factor_q20, local_factor_q20);

            last_candidate = ScaleUnsignedQ20(
                center, static_cast<std::uint64_t>(std::max<std::int64_t>(
                            0, combined_factor_q20)));
            if (last_candidate >= minimum && last_candidate <= maximum) {
                return last_candidate;
            }
        }

        // Extremely narrow ranges can reject most of the measured-shape model.
        // Fall back to a bounded ordinary sample rather than pinning repeatedly
        // to Minimum or Maximum.
        return NextInclusive(minimum, maximum);
    }


    // Keyboard calibration of the same Natural timing architecture. Recorded
    // single-key tapping showed stronger short / medium pace persistence and
    // less immediate interval noise than mouse clicking, while deliberately
    // slow tapping regained some variability. It remains bounded by the same
    // user-configured Minimum / Maximum contract and does not replay data.
    [[nodiscard]] std::uint64_t NextNaturalKeyboardInclusive(
        const std::uint64_t minimum,
        const std::uint64_t maximum) noexcept {
        if (maximum <= minimum) {
            return minimum;
        }

        constexpr std::int64_t q20_one = 1LL << 20;
        constexpr std::int64_t pace_rho_fast_q20 = 1'032'847; // 0.985 through ordinary keyboard tempos
        constexpr std::int64_t pace_rho_slow_q20 = 1'017'119; // 0.970 by 900 ms
        constexpr std::int64_t pace_innovation_fast_q20 = 21'496; // about 0.0205
        constexpr std::int64_t pace_innovation_slow_target_q20 = 16'777;
        constexpr std::int64_t slow_pace_rho_q20 = 1'043'333; // ~0.995
        constexpr std::int64_t slow_pace_innovation_target_q20 = 2'097;
        constexpr std::int64_t local_core_q20 = 57'672;      // about 0.055
        constexpr std::int64_t positive_tail_q20 = 471'859;  // 0.45
        constexpr std::int64_t negative_tail_q20 = 235'930;  // 0.225
        constexpr std::int64_t local_center_offset_q20 = 8'258; // about 0.007875
        constexpr std::uint64_t positive_tail_threshold =
            73'400; // about 7.0% of 2^20
        constexpr std::uint64_t negative_tail_threshold =
            36'700; // about 3.5% of 2^20
        constexpr std::uint32_t maximum_attempts = 8;

        const std::uint64_t span = maximum - minimum;
        const std::uint64_t center = minimum + span / 2U;
        const std::uint64_t half_span = span / 2U;

        std::int64_t pace_rho_q20 = pace_rho_fast_q20;
        if (center > 600'000U) {
            const std::uint64_t rho_delta = std::min<std::uint64_t>(
                center - 600'000U, 300'000U);
            pace_rho_q20 = pace_rho_fast_q20 - static_cast<std::int64_t>(
                ((pace_rho_fast_q20 - pace_rho_slow_q20) *
                 static_cast<std::int64_t>(rho_delta)) / 300'000LL);
        }

        // Full measured variability is used once either side of the configured
        // range is at least 25% of the center interval. Narrower user ranges
        // proportionally reduce the model's pace / noise amplitudes instead of
        // piling large numbers of samples onto the hard limits.
        const std::uint64_t quarter_center =
            center / 4U + (center % 4U == 0U ? 0U : 1U);
        const std::int64_t variation_scale_q20 =
            half_span >= quarter_center
                ? q20_one
                : static_cast<std::int64_t>(FractionToQ20(half_span * 4U, center));

        // Keep the ordinary keyboard pace / noise profile through a 250 ms
        // configured center. From there to 600 ms, smoothly introduce the
        // same longer-memory component with a smaller initial variability boost.
        // The user's hard Minimum / Maximum range remains authoritative.
        constexpr std::uint64_t slow_transition_start = 250'000U;
        constexpr std::uint64_t slow_transition_full = 600'000U;
        constexpr std::int64_t local_boost_max_q20 = 209'715; // 0.20
        constexpr std::int64_t pace_boost_max_q20 = 157'286; // 0.15
        std::int64_t slow_progress_q20 = 0;
        if (center > slow_transition_start) {
            const std::uint64_t slow_delta = std::min<std::uint64_t>(
                center - slow_transition_start,
                slow_transition_full - slow_transition_start);
            slow_progress_q20 = static_cast<std::int64_t>(FractionToQ20(
                slow_delta, slow_transition_full - slow_transition_start));
        }

        const std::int64_t local_boost_q20 = MultiplyQ20(
            slow_progress_q20, local_boost_max_q20);
        const std::int64_t pace_boost_q20 = MultiplyQ20(
            slow_progress_q20, pace_boost_max_q20);
        std::int64_t effective_local_scale_q20 = MultiplyQ20(
            variation_scale_q20,
            q20_one + MultiplyQ20(local_boost_q20, variation_scale_q20));
        std::int64_t effective_pace_scale_q20 = MultiplyQ20(
            variation_scale_q20,
            q20_one + MultiplyQ20(pace_boost_q20, variation_scale_q20));

        // Deliberately slow keyboard tapping becomes more variable again. Keep
        // the calmer ordinary-keypress profile through 600 ms, then restore
        // part of the measured slow-tempo spread by 900 ms.
        if (center > 600'000U) {
            const std::uint64_t extra_delta = std::min<std::uint64_t>(
                center - 600'000U, 300'000U);
            const std::int64_t extra_progress_q20 = static_cast<std::int64_t>(
                FractionToQ20(extra_delta, 300'000U));
            const std::int64_t extra_local_boost_q20 = MultiplyQ20(
                extra_progress_q20, 838'861); // up to +80%
            const std::int64_t extra_pace_boost_q20 = MultiplyQ20(
                extra_progress_q20, 524'288); // up to +50%
            effective_local_scale_q20 = MultiplyQ20(
                effective_local_scale_q20, q20_one + extra_local_boost_q20);
            effective_pace_scale_q20 = MultiplyQ20(
                effective_pace_scale_q20, q20_one + extra_pace_boost_q20);
        }

        const std::int64_t fast_innovation_q20 =
            pace_innovation_fast_q20 + MultiplyQ20(
                pace_innovation_slow_target_q20 - pace_innovation_fast_q20,
                slow_progress_q20);
        const std::int64_t slow_innovation_base_q20 = MultiplyQ20(
            slow_pace_innovation_target_q20, slow_progress_q20);

        const std::int64_t relative_half_q20 = static_cast<std::int64_t>(
            FractionToQ20(std::min(half_span, center), center));
        const std::int64_t pace_limit_q20 = std::min<std::int64_t>(
            (q20_one * 30) / 100,
            (relative_half_q20 * 60) / 100);

        const std::int64_t scaled_innovation_q20 =
            MultiplyQ20(fast_innovation_q20, effective_pace_scale_q20);
        const std::int64_t scaled_slow_innovation_q20 =
            MultiplyQ20(slow_innovation_base_q20, effective_pace_scale_q20);
        const std::int64_t scaled_core_q20 =
            MultiplyQ20(local_core_q20, effective_local_scale_q20);
        const std::int64_t scaled_positive_tail_q20 =
            MultiplyQ20(positive_tail_q20, effective_local_scale_q20);
        const std::int64_t scaled_negative_tail_q20 =
            MultiplyQ20(negative_tail_q20, effective_local_scale_q20);
        const std::int64_t scaled_center_offset_q20 =
            MultiplyQ20(local_center_offset_q20, effective_local_scale_q20);

        const std::int64_t pace_innovation = MultiplyQ20(
            ApproximateNormalQ20(), scaled_innovation_q20);
        std::int64_t slow_pace_innovation = 0;
        if (slow_progress_q20 != 0) {
            slow_pace_innovation = MultiplyQ20(
                ApproximateNormalQ20(), scaled_slow_innovation_q20);
        }
        natural_pace_q20_ =
            MultiplyQ20(natural_pace_q20_, pace_rho_q20) + pace_innovation;
        natural_slow_pace_q20_ =
            MultiplyQ20(natural_slow_pace_q20_, slow_pace_rho_q20) +
            slow_pace_innovation;
        natural_pace_q20_ = std::clamp(
            natural_pace_q20_, -pace_limit_q20, pace_limit_q20);
        natural_slow_pace_q20_ = std::clamp(
            natural_slow_pace_q20_, -pace_limit_q20, pace_limit_q20);

        std::uint64_t last_candidate = center;
        for (std::uint32_t attempt = 0; attempt < maximum_attempts; ++attempt) {
            std::int64_t local_noise_q20 = MultiplyQ20(
                ApproximateNormalQ20(), scaled_core_q20);
            local_noise_q20 -= scaled_center_offset_q20;

            const std::uint64_t tail_bits = NextRaw();
            if ((tail_bits & ((1ULL << 20) - 1ULL)) < positive_tail_threshold) {
                const std::uint64_t unit = (tail_bits >> 20U) & 0xFFFFULL;
                const std::uint64_t squared = unit * unit;
                local_noise_q20 += static_cast<std::int64_t>(
                    (static_cast<std::uint64_t>(scaled_positive_tail_q20) * squared) /
                    (0xFFFFULL * 0xFFFFULL));
            }
            if (((tail_bits >> 44U) & ((1ULL << 20) - 1ULL)) <
                negative_tail_threshold) {
                const std::uint64_t unit = (tail_bits >> 28U) & 0xFFFFULL;
                const std::uint64_t squared = unit * unit;
                local_noise_q20 -= static_cast<std::int64_t>(
                    (static_cast<std::uint64_t>(scaled_negative_tail_q20) * squared) /
                    (0xFFFFULL * 0xFFFFULL));
            }

            const std::int64_t active_slow_pace_q20 = MultiplyQ20(
                natural_slow_pace_q20_, slow_progress_q20);
            const std::int64_t combined_pace_q20 = std::clamp(
                natural_pace_q20_ + active_slow_pace_q20,
                -pace_limit_q20,
                pace_limit_q20);
            const std::int64_t pace_factor_q20 = q20_one + combined_pace_q20;
            const std::int64_t local_factor_q20 = std::max<std::int64_t>(
                q20_one / 8, q20_one + local_noise_q20);
            const std::int64_t combined_factor_q20 = MultiplyQ20(
                pace_factor_q20, local_factor_q20);

            last_candidate = ScaleUnsignedQ20(
                center, static_cast<std::uint64_t>(std::max<std::int64_t>(
                            0, combined_factor_q20)));
            if (last_candidate >= minimum && last_candidate <= maximum) {
                return last_candidate;
            }
        }

        // Extremely narrow ranges can reject most of the measured-shape model.
        // Fall back to a bounded ordinary sample rather than pinning repeatedly
        // to Minimum or Maximum.
        return NextInclusive(minimum, maximum);
    }

private:
    [[nodiscard]] static constexpr std::int64_t MultiplyQ20(
        const std::int64_t left,
        const std::int64_t right) noexcept {
        return (left * right) >> 20;
    }

    // Converts numerator / denominator to unsigned Q20 without multiplying the
    // numerator by 2^20, avoiding overflow for large validated durations.
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

    // Six 8-bit uniforms from one SplitMix64 output form a compact normal-like
    // variate (mean 0, standard deviation approximately 1) without adding a
    // floating-point distribution dependency to the scheduler.
    [[nodiscard]] std::int64_t ApproximateNormalQ20() noexcept {
        constexpr std::int64_t q20_one = 1LL << 20;
        const std::uint64_t bits = NextRaw();
        std::int64_t sum = 0;
        for (unsigned shift = 0; shift < 48U; shift += 8U) {
            sum += static_cast<std::int64_t>((bits >> shift) & 0xFFULL);
        }
        return ((sum - 765LL) * q20_one) / 181LL;
    }

    [[nodiscard]] static constexpr std::uint64_t SaturatingMultiplyUnsigned(
        const std::uint64_t value,
        const std::uint64_t multiplier) noexcept {
        if (value == 0 || multiplier == 0) {
            return 0;
        }
        if (value > std::numeric_limits<std::uint64_t>::max() / multiplier) {
            return std::numeric_limits<std::uint64_t>::max();
        }
        return value * multiplier;
    }

    constexpr void BeginDriftRegime(const std::uint64_t minimum,
                                    const std::uint64_t maximum) noexcept {
        const std::uint64_t span = maximum - minimum;

        // Fast periods are intentionally less common than middle and slow
        // periods because the same interval reduction produces a much larger
        // increase in actions per second. The weighting keeps the long-run
        // throughput near the independent mode while allowing pronounced
        // short-run differences.
        const std::uint64_t roll = NextInclusive(0U, 19U);
        std::uint64_t region = roll < 4U ? 0U : roll < 12U ? 1U : 2U;

        // Avoid needlessly extending an identical period half of the time.
        // A repeated region is still allowed, so the sequence does not become
        // a predictable fast-middle-slow cycle.
        if (drift_initialized_ && region == drift_region_ &&
            NextInclusive(0U, 1U) == 0U) {
            region = (region + 1U + NextInclusive(0U, 1U)) % 3U;
        }
        drift_region_ = region;

        if (region == 0U) {
            drift_region_minimum_ = minimum;
            drift_region_maximum_ = minimum + (span * 38U) / 100U;
        } else if (region == 1U) {
            drift_region_minimum_ = minimum + (span * 30U) / 100U;
            drift_region_maximum_ = minimum + (span * 70U) / 100U;
        } else {
            drift_region_minimum_ = minimum + (span * 62U) / 100U;
            drift_region_maximum_ = maximum;
        }

        const std::uint64_t midpoint = minimum + span / 2U;
        const std::uint64_t minimum_budget = std::max<std::uint64_t>(
            750'000U, SaturatingMultiplyUnsigned(midpoint, 18U));
        const std::uint64_t maximum_budget = std::max<std::uint64_t>(
            1'500'000U, SaturatingMultiplyUnsigned(midpoint, 60U));
        drift_budget_remaining_ = NextInclusive(
            minimum_budget,
            std::max(minimum_budget, maximum_budget));
        drift_initialized_ = true;
    }

    [[nodiscard]] constexpr std::uint64_t NextRaw() noexcept {
        state_ += 0x9E3779B97F4A7C15ULL;
        std::uint64_t value = state_;
        value = (value ^ (value >> 30U)) * 0xBF58476D1CE4E5B9ULL;
        value = (value ^ (value >> 27U)) * 0x94D049BB133111EBULL;
        return value ^ (value >> 31U);
    }

    std::uint64_t state_{};
    bool drift_initialized_{};
    std::uint64_t drift_region_{};
    std::uint64_t drift_region_minimum_{};
    std::uint64_t drift_region_maximum_{};
    std::uint64_t drift_budget_remaining_{};
    std::int64_t natural_pace_q20_{};
    std::int64_t natural_slow_pace_q20_{};
};

[[nodiscard]] constexpr std::int64_t SaturatingAddTime(
    const std::int64_t left,
    const std::int64_t right) noexcept {
    if (right > 0 && left > std::numeric_limits<std::int64_t>::max() - right) {
        return std::numeric_limits<std::int64_t>::max();
    }
    if (right < 0 && left < std::numeric_limits<std::int64_t>::min() - right) {
        return std::numeric_limits<std::int64_t>::min();
    }
    return left + right;
}

[[nodiscard]] constexpr std::int64_t SaturatingMultiplyTime(
    const std::int64_t duration,
    const std::uint64_t multiplier) noexcept {
    if (duration <= 0 || multiplier == 0) {
        return 0;
    }
    const auto maximum = static_cast<std::uint64_t>(
        std::numeric_limits<std::int64_t>::max());
    const auto positive_duration = static_cast<std::uint64_t>(duration);
    if (multiplier > maximum / positive_duration) {
        return std::numeric_limits<std::int64_t>::max();
    }
    return static_cast<std::int64_t>(positive_duration * multiplier);
}

// Returns the absolute deadline for one input position in one action. The same
// helper is used with QPC ticks at runtime and integer units in tests.
[[nodiscard]] constexpr std::int64_t ScheduledInputDeadline(
    const std::int64_t session_start,
    const std::int64_t action_interval,
    const std::int64_t action_spacing,
    const std::uint64_t action_index,
    const std::uint32_t input_index) noexcept {
    const auto action_offset = SaturatingMultiplyTime(
        action_interval, action_index);
    const auto input_offset = SaturatingMultiplyTime(
        action_spacing, static_cast<std::uint64_t>(input_index));
    return SaturatingAddTime(
        SaturatingAddTime(session_start, action_offset), input_offset);
}

// Keeps the release phase tied to the accepted schedule instead of starting a
// fresh duration after the press call returns. This prevents normal timer and
// input-submission overhead from becoming permanent cadence drift.
[[nodiscard]] constexpr std::int64_t ScheduledReleaseDeadline(
    const std::int64_t press_deadline,
    const std::int64_t down_duration) noexcept {
    return SaturatingAddTime(press_deadline, down_duration);
}

// Unlimited runs may discard only whole actions whose final input is already
// older than the permitted lag. This preserves bounded memory and prevents a
// long replay flood after a stall or an intentionally excessive configuration.
[[nodiscard]] constexpr std::uint64_t FirstUnlimitedActionToKeep(
    const std::int64_t now,
    const std::int64_t session_start,
    const std::int64_t action_interval,
    const std::int64_t final_input_offset,
    const std::int64_t maximum_lag) noexcept {
    if (action_interval <= 0 || maximum_lag <= 0 || now <= maximum_lag) {
        return 0;
    }

    const auto first_action_end = SaturatingAddTime(
        session_start, final_input_offset);
    const auto oldest_allowed_deadline = now - maximum_lag;
    if (oldest_allowed_deadline <= first_action_end) {
        return 0;
    }

    const auto overdue = oldest_allowed_deadline - first_action_end;
    const auto whole_intervals = overdue / action_interval;
    const bool partial_interval = overdue % action_interval != 0;
    return static_cast<std::uint64_t>(whole_intervals) +
           static_cast<std::uint64_t>(partial_interval ? 1 : 0);
}

} // namespace vectorclick::core
