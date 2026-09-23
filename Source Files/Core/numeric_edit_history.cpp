#include "Core/numeric_edit_history.h"

#include <utility>

namespace vectorclick::core {

void NumericEditHistory::Begin(std::wstring baseline) {
    active_ = true;
    baseline_ = std::move(baseline);
    redo_.reset();
}

void NumericEditHistory::End() noexcept {
    active_ = false;
    baseline_.clear();
    redo_.reset();
}

bool NumericEditHistory::IsActive() const noexcept {
    return active_;
}

bool NumericEditHistory::HasUncommittedChange(
    const std::wstring_view current) const noexcept {
    return active_ && current != baseline_;
}

void NumericEditHistory::NoteUserEdit() {
    if (active_) {
        redo_.reset();
    }
}

std::optional<std::wstring> NumericEditHistory::UndoTarget(
    const std::wstring_view current) {
    if (!HasUncommittedChange(current)) {
        return std::nullopt;
    }

    redo_ = std::wstring(current);
    return baseline_;
}

std::optional<std::wstring> NumericEditHistory::RedoTarget(
    const std::wstring_view current) const {
    if (!active_ || !redo_.has_value() || current != baseline_) {
        return std::nullopt;
    }
    return redo_;
}

void NumericEditHistory::Synchronize(std::wstring baseline) {
    if (!active_) {
        return;
    }
    baseline_ = std::move(baseline);
    redo_.reset();
}

} // namespace vectorclick::core
