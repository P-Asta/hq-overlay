#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

namespace hq::overlay {

DWORD WINAPI BootstrapThread(void* module_parameter);
void NotifyProcessDetach() noexcept;
bool RequestSoftDisable(const char* reason) noexcept;
[[nodiscard]] bool IsReady() noexcept;

}  // namespace hq::overlay
