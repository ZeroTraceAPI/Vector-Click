#pragma once

#include "Core/settings.h"
#include "Windows/profile_store.h"
#include "Windows/win32_raii.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <functional>
#include <string>
#include <vector>

namespace vectorclick::win {

class ProfileManagerWindow {
public:
    struct Callbacks final {
        std::function<bool(core::RunSettings&, std::wstring&)> capture_current_settings;
        std::function<bool()> operations_allowed;
        std::function<void()> profiles_changed;
    };

    ProfileManagerWindow(HINSTANCE instance,
                         HWND owner,
                         ProfileStore& store,
                         Callbacks callbacks);
    ~ProfileManagerWindow();

    ProfileManagerWindow(const ProfileManagerWindow&) = delete;
    ProfileManagerWindow& operator=(const ProfileManagerWindow&) = delete;

    [[nodiscard]] bool Show();
    void Close() noexcept;
    [[nodiscard]] bool IsOpen() const noexcept {
        return window_ != nullptr && IsWindow(window_) != FALSE;
    }
    void Refresh();
    void RefreshAvailability();
    void SynchronizeOwnerPresentation();

private:
    enum ControlId : int {
        ProfileList = 1,
        NameEdit,
        NewButton,
        SaveButton,
        RenameButton,
        DuplicateButton,
        ImportButton,
        ExportButton,
        DeleteButton,
        OpenFolderButton,
        CloseButton,
    };

    static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM w_param, LPARAM l_param);
    static LRESULT CALLBACK ProfileListSubclassProc(HWND window,
                                                     UINT message,
                                                     WPARAM w_param,
                                                     LPARAM l_param,
                                                     UINT_PTR subclass_id,
                                                     DWORD_PTR reference_data);
    LRESULT HandleMessage(UINT message, WPARAM w_param, LPARAM l_param);
    [[nodiscard]] bool RegisterWindowClass() const;
    [[nodiscard]] bool CreateControls();
    void Layout();
    void LayoutProfileList();
    void RecreateFont();
    void RefreshList(std::wstring_view preferred_id = {});
    [[nodiscard]] int ProfileListVisibleRows() const noexcept;
    [[nodiscard]] int ProfileListMaximumTopIndex() const noexcept;
    [[nodiscard]] RECT ProfileScrollbarGutterRect() const noexcept;
    [[nodiscard]] RECT ProfileScrollbarHoverRect() const noexcept;
    [[nodiscard]] RECT ProfileScrollbarTrackRect() const noexcept;
    [[nodiscard]] RECT ProfileScrollbarThumbRect() const noexcept;
    [[nodiscard]] bool ScrollProfileListWheel(WPARAM w_param) noexcept;
    void SetProfileListTopIndex(int top_index) noexcept;
    void ScrollProfileListBy(int row_delta) noexcept;
    void InvalidateProfileScrollbar() noexcept;
    void UpdateSelection();
    void UpdateEnabledState();
    [[nodiscard]] int SelectedIndex() const noexcept;
    [[nodiscard]] ProfileInfo* SelectedProfile() noexcept;
    [[nodiscard]] const ProfileInfo* SelectedProfile() const noexcept;
    [[nodiscard]] std::wstring NameText() const;
    void SetNameText(std::wstring_view text);
    [[nodiscard]] bool CaptureCurrent(core::RunSettings& settings, std::wstring& error) const;
    [[nodiscard]] bool OperationsAllowed() const;

    void CreateFromCurrent();
    void SaveCurrent();
    void RenameSelected();
    void DuplicateSelected();
    void ImportProfile();
    void ExportProfile();
    void DeleteSelected();
    void OpenProfilesFolder();
    void ShowOperationError(std::wstring_view title, const std::wstring& error) const;
    void PaintButton(const DRAWITEMSTRUCT& draw, bool danger = false) const;

    HINSTANCE instance_{};
    HWND owner_{};
    ProfileStore& store_;
    Callbacks callbacks_;

    HWND window_{};
    HWND list_label_{};
    HWND list_{};
    HWND name_label_{};
    HWND name_edit_{};
    HWND hint_text_{};
    HWND status_text_{};
    HWND new_button_{};
    HWND save_button_{};
    HWND rename_button_{};
    HWND duplicate_button_{};
    HWND import_button_{};
    HWND export_button_{};
    HWND delete_button_{};
    HWND open_folder_button_{};
    HWND close_button_{};
    UINT dpi_{96};
    UniqueGdiObject font_;
    UniqueGdiObject bold_font_;
    RECT list_frame_{};
    bool profile_scrollbar_visible_{};
    bool profile_scrollbar_hovered_{};
    bool profile_scrollbar_dragging_{};
    int profile_scrollbar_drag_offset_{};
    int profile_scroll_wheel_remainder_{};
    std::vector<ProfileInfo> profiles_;
};

} // namespace vectorclick::win
