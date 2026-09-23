#include "Windows/shared_image_loader.h"

namespace vectorclick::win {

HICON LoadSharedDefaultIcon(const HINSTANCE instance,
                            const LPCWSTR resource) noexcept {
    return reinterpret_cast<HICON>(LoadImageW(
        instance, resource, IMAGE_ICON, 0, 0, LR_DEFAULTSIZE | LR_SHARED));
}

HCURSOR LoadSharedDefaultCursor(const HINSTANCE instance,
                                const LPCWSTR resource) noexcept {
    return reinterpret_cast<HCURSOR>(LoadImageW(
        instance, resource, IMAGE_CURSOR, 0, 0, LR_DEFAULTSIZE | LR_SHARED));
}

} // namespace vectorclick::win
