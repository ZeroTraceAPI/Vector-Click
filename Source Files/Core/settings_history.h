#pragma once

#include "Core/settings.h"

#include <cstddef>
#include <vector>

namespace vectorclick::core {

// Settings Undo / Redo is intentionally session-only. The current state plus
// the 24 previous committed states are retained, which provides up to 24 undo
// steps while bounding memory and keeping history independent of portable
// settings persistence.
inline constexpr std::size_t MaximumSettingsHistoryEntries = 24;

class SettingsHistory final {
public:
    void Reset(const RunSettings& state);
    [[nodiscard]] bool IsInitialized() const noexcept;
    [[nodiscard]] bool CanUndo() const noexcept;
    [[nodiscard]] bool CanRedo() const noexcept;
    [[nodiscard]] const RunSettings* Current() const noexcept;
    [[nodiscard]] const RunSettings* UndoTarget() const noexcept;
    [[nodiscard]] const RunSettings* RedoTarget() const noexcept;

    // Record a newly committed semantic settings state. Recording after an
    // undo discards the redo branch, matching conventional Undo / Redo rules.
    [[nodiscard]] bool Record(const RunSettings& state);

    // Replace the current snapshot without creating a user-visible history
    // step. This is used when an excluded external operation necessarily
    // adjusts an otherwise undoable dependent setting. If the state actually
    // changes while a redo branch exists, that stale redo branch is discarded.
    void ReplaceCurrent(const RunSettings& state);

    // Cursor movement is committed only after the caller has successfully
    // applied the target state. This allows asynchronous hotkey registration
    // to fail without corrupting the history position.
    [[nodiscard]] bool CommitUndo() noexcept;
    [[nodiscard]] bool CommitRedo() noexcept;

    [[nodiscard]] std::size_t UndoDepth() const noexcept;
    [[nodiscard]] std::size_t RedoDepth() const noexcept;

private:
    static constexpr std::size_t MaximumSnapshots =
        MaximumSettingsHistoryEntries + 1U;

    std::vector<RunSettings> snapshots_;
    std::size_t cursor_{};
};

} // namespace vectorclick::core
