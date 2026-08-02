#pragma once

#include <filesystem>
#include <string_view>

namespace hq::logging {

enum class Level { Info, Warning, Error };

void Initialize();
void Write(Level level, std::string_view message);
[[nodiscard]] std::filesystem::path CurrentPath();

}  // namespace hq::logging
