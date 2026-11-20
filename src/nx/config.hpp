#pragma once

#include "platform.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <iosfwd>

namespace hp2::nx {
struct UserSettings {
    int frameLimit{defaultFrameLimit}; // 0 = uncapped, otherwise 1..maximumFrameLimit.
    bool showFps{};
};
inline UserSettings userSettings;
inline int selectedSessionLanguage{-1};
inline void SetSessionLanguage(int language) noexcept {
    if(language>=0 && language<6)selectedSessionLanguage=language;
}
inline int LanguageForSession(int profileLanguage) noexcept {
    return selectedSessionLanguage>=0 ? selectedSessionLanguage:profileLanguage;
}
inline bool gameplayPresentation{};
void LoadUserSettings(const std::filesystem::path& directory,std::ostream& log);
bool SaveUserSettings() noexcept;
void CycleFrameLimit(int direction) noexcept;

}
