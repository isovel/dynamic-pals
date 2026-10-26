#include <cmath>
#include <numbers>
#include "SettingsStore.hpp"

#include "json.hpp"

#include <PalCfg/Document.hpp>
#include <PalCfg/Load.hpp>

namespace DynPals {

    DynPalsSettings LayerWorld(const DynPalsSettings& Global, std::string_view WorldSettingsJson, PalCfg::IDiagnosticSink& Sink) {
        DynPalsSettings Layered = Global;

        PalCfg::Document Doc;
        if (!Doc.Parse(std::string(WorldSettingsJson))) {
            Sink.Report({PalCfg::Severity::Error, {}, Doc.Error().message});
            return Layered;
        }
        if (!Doc.Root().IsObject()) {
            Sink.Report({PalCfg::Severity::Error, {}, "the world's settings are not an object"});
            return Layered;
        }

        // LoadFields writes only the keys present, so the rest stay global.
        PalCfg::LoadFields(kWorldFields, Doc.Root(), Layered, Sink);
        return Layered;
    }

    std::string WorldOverride(const DynPalsSettings& Global, const DynPalsSettings& Effective) {
        std::string Out = "{";
        for (const auto& Field : kWorldFields) {
            std::string Was, Now;
            Field.ops->format(&Global, Field.boundMemberPtr, Was);
            Field.ops->format(&Effective, Field.boundMemberPtr, Now);
            if (Was == Now) continue;
            if (Out.size() > 1) Out += ",";
            Out += "\"" + std::string(Field.meta.key) + "\":" + Now;
        }
        return Out + "}";
    }

    std::string SettingsObjectOf(std::string_view SaveJson) {
        const auto Save = nlohmann::json::parse(SaveJson, nullptr, false);
        if (Save.is_discarded() || !Save.is_object()) return "{}";
        const auto Found = Save.find("Settings");
        if (Found == Save.end() || !Found->is_object()) return "{}";
        return Found->dump();
    }

    std::wstring NewestWorldSave(const std::vector<SaveCandidate>& Files, std::wstring_view Prefix) {
        const SaveCandidate* Newest = nullptr;
        for (const SaveCandidate& File : Files) {
            const std::wstring Name = std::filesystem::path(File.Path).filename().wstring();
            if (!Name.starts_with(Prefix) || !Name.ends_with(L".json")) continue;
            if (!Newest || File.Modified > Newest->Modified) Newest = &File;
        }
        return Newest ? Newest->Path : std::wstring();
    }

    double CentredSideOffset(double ClearFrom, double Distance, double FovDegrees) {
        if (!(ClearFrom > 0.0 && ClearFrom < 1.0) || !(FovDegrees > 0.0 && FovDegrees < 180.0)) return 0.0;
        // The clear part's middle sits ClearFrom of the half-width right of centre; the camera moves left that far.
        const double HalfWidth = Distance * std::tan(FovDegrees * std::numbers::pi / 360.0);
        return -ClearFrom * HalfWidth;
    }

    double TurnedRotation(double Degrees, double DeltaUnits) {
        const double Turned = std::fmod(Degrees + DeltaUnits * 0.5, 360.0);
        const double Wrapped = Turned < 0.0 ? Turned + 360.0 : Turned;
        // A tiny negative remainder rounds up to 360 when wrapped.
        return Wrapped >= 360.0 ? 0.0 : Wrapped;
    }
}
