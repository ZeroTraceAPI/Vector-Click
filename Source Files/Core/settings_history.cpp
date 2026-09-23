#include "Core/settings_history.h"

#include <algorithm>

namespace vectorclick::core {

void SettingsHistory::Reset(const RunSettings& state) {
    snapshots_.clear();
    snapshots_.push_back(state);
    cursor_ = 0;
}

bool SettingsHistory::IsInitialized() const noexcept {
    return !snapshots_.empty();
}

bool SettingsHistory::CanUndo() const noexcept {
    return IsInitialized() && cursor_ > 0U;
}

bool SettingsHistory::CanRedo() const noexcept {
    return IsInitialized() && cursor_ + 1U < snapshots_.size();
}

const RunSettings* SettingsHistory::Current() const noexcept {
    return IsInitialized() ? &snapshots_[cursor_] : nullptr;
}

const RunSettings* SettingsHistory::UndoTarget() const noexcept {
    return CanUndo() ? &snapshots_[cursor_ - 1U] : nullptr;
}

const RunSettings* SettingsHistory::RedoTarget() const noexcept {
    return CanRedo() ? &snapshots_[cursor_ + 1U] : nullptr;
}

bool SettingsHistory::Record(const RunSettings& state) {
    if (!IsInitialized()) {
        Reset(state);
        return false;
    }
    if (snapshots_[cursor_] == state) {
        return false;
    }

    if (cursor_ + 1U < snapshots_.size()) {
        snapshots_.erase(snapshots_.begin() +
                             static_cast<std::ptrdiff_t>(cursor_ + 1U),
                         snapshots_.end());
    }

    snapshots_.push_back(state);
    cursor_ = snapshots_.size() - 1U;

    if (snapshots_.size() > MaximumSnapshots) {
        const std::size_t excess = snapshots_.size() - MaximumSnapshots;
        snapshots_.erase(snapshots_.begin(),
                         snapshots_.begin() +
                             static_cast<std::ptrdiff_t>(excess));
        cursor_ -= std::min(cursor_, excess);
    }
    return true;
}

void SettingsHistory::ReplaceCurrent(const RunSettings& state) {
    if (!IsInitialized()) {
        Reset(state);
        return;
    }
    if (snapshots_[cursor_] == state) {
        return;
    }

    snapshots_[cursor_] = state;
    if (cursor_ + 1U < snapshots_.size()) {
        snapshots_.erase(snapshots_.begin() +
                             static_cast<std::ptrdiff_t>(cursor_ + 1U),
                         snapshots_.end());
    }
}

bool SettingsHistory::CommitUndo() noexcept {
    if (!CanUndo()) {
        return false;
    }
    --cursor_;
    return true;
}

bool SettingsHistory::CommitRedo() noexcept {
    if (!CanRedo()) {
        return false;
    }
    ++cursor_;
    return true;
}

std::size_t SettingsHistory::UndoDepth() const noexcept {
    return IsInitialized() ? cursor_ : 0U;
}

std::size_t SettingsHistory::RedoDepth() const noexcept {
    return IsInitialized() ? snapshots_.size() - cursor_ - 1U : 0U;
}

} // namespace vectorclick::core
