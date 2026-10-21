#pragma once
#include <filesystem>
#include <string>
#include <vector>
#include <string_view>

#include <PalCfg/Diagnostics.hpp>

#include "Settings.hpp"

namespace DynPals {

    // Global with every camera key present in WorldSettingsJson laid over it.
    // Keybinds stay global. An unreadable document leaves Global as it is.
    DynPalsSettings LayerWorld(const DynPalsSettings& Global, std::string_view WorldSettingsJson, PalCfg::IDiagnosticSink& Sink);

    // A JSON object of the camera keys whose values differ between the two.
    std::string WorldOverride(const DynPalsSettings& Global, const DynPalsSettings& Effective);

    // The save's Settings object as text, or "{}".
    std::string SettingsObjectOf(std::string_view SaveJson);

    struct SaveCandidate {
        std::wstring Path;
        std::filesystem::file_time_type Modified;
    };

    // The most recently modified Prefix*.json among Files, or empty.
    std::wstring NewestWorldSave(const std::vector<SaveCandidate>& Files, std::wstring_view Prefix);
}
