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

    // Degrees turned by a drag of DeltaUnits slate units, at half a degree each, wrapped into [0, 360).
    double TurnedRotation(double Degrees, double DeltaUnits);

    // The camera's sideways shift (a spring arm's socket Y) that centres a subject Distance ahead in the screen's part
    // right of ClearFrom (0-1 of its width), at a horizontal field of view of FovDegrees; 0 when either is out of range.
    double CentredSideOffset(double ClearFrom, double Distance, double FovDegrees);

    // The most recently modified Prefix*.json among Files, or empty.
    std::wstring NewestWorldSave(const std::vector<SaveCandidate>& Files, std::wstring_view Prefix);
}
