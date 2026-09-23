#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace vectorclick::core {

// One focused numeric field is treated as one uncommitted edit transaction.
// This avoids relying on the native EDIT control's character-level undo stack,
// which can expose intermediate text such as an empty field when the user
// expects the previously committed numeric value.
class NumericEditHistory final {
public:
    void Begin(std::wstring baseline);
    void End() noexcept;

    [[nodiscard]] bool IsActive() const noexcept;
    [[nodiscard]] bool HasUncommittedChange(
        std::wstring_view current) const noexcept;

    // Any direct user edit after an Undo creates a new local branch and
    // discards the local Redo value. The committed baseline remains unchanged
    // until the field loses focus and the settings layer accepts the value.
    void NoteUserEdit();

    // Undo returns the baseline and remembers the current text as the one local
    // Redo target. No application-level settings-history cursor is moved.
    [[nodiscard]] std::optional<std::wstring> UndoTarget(
        std::wstring_view current);

    // Redo is available only while the field still shows the baseline restored
    // by local Undo. Reapplying it keeps the same Redo target so Ctrl+Z and
    // Ctrl+Y can alternate while the field remains focused.
    [[nodiscard]] std::optional<std::wstring> RedoTarget(
        std::wstring_view current) const;

    // Application-level settings Undo / Redo can replace the focused field
    // programmatically. Synchronize that resulting text as the new local
    // baseline so stale in-progress Redo text cannot cross semantic history
    // steps.
    void Synchronize(std::wstring baseline);

private:
    bool active_{};
    std::wstring baseline_;
    std::optional<std::wstring> redo_;
};

} // namespace vectorclick::core
