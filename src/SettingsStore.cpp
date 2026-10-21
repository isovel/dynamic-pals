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
}
