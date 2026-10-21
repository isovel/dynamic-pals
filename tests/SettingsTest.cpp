#include <catch2/catch_test_macros.hpp>

#include "Settings.hpp"
#include "SettingsStore.hpp"
#include "json.hpp"

#include <PalCfg/Diagnostics.hpp>
#include <PalCfg/Load.hpp>

#include <algorithm>
#include <string>

using namespace DynPals;

TEST_CASE("a world's camera settings override the global ones key by key")
{
    DynPalsSettings Global;
    PalCfg::CollectingSink Sink;
    const DynPalsSettings Effective = LayerWorld(Global, R"({"CameraRotation": 90})", Sink);
    CHECK(Effective.CameraRotation == 90.0);
    CHECK(Effective.bFocusPal == Global.bFocusPal);
}

TEST_CASE("a world save's keybinds are ignored")
{
    DynPalsSettings Global;
    PalCfg::CollectingSink Sink;
    CHECK(LayerWorld(Global, R"({"MenuKey": "K"})", Sink).MenuKey == L"N");
}

TEST_CASE("an unreadable world save keeps the global settings and reports one error")
{
    DynPalsSettings Global;
    Global.CameraRotation = 45.0;
    PalCfg::CollectingSink Sink;
    CHECK(LayerWorld(Global, "{not json", Sink).CameraRotation == 45.0);
    CHECK(std::ranges::count(Sink.All(), PalCfg::Severity::Error, &PalCfg::Diagnostic::severity) == 1);
}

TEST_CASE("a world override holds only the camera keys that differ")
{
    DynPalsSettings Global, Effective;
    Effective.CameraRotation = 90.0;
    Effective.MenuKey = L"K";
    const auto Object = nlohmann::json::parse(WorldOverride(Global, Effective));
    CHECK(Object.size() == 1);
    CHECK(Object.at("CameraRotation") == 90.0);
}

TEST_CASE("an old world save's Settings object imports every setting")
{
    const std::string Save = R"({"System": {}, "Settings": {"FocusPal": false, "CameraRotation": 30,
        "RelativeCamera": true, "MenuKey": "K", "MenuModifier": "LeftCtrl", "RerollKey": "R", "RerollModifier": "None"}})";
    DynPalsSettings Imported;
    PalCfg::CollectingSink Sink;
    PalCfg::LoadFromText(kFields, SettingsObjectOf(Save), Imported, Sink);
    CHECK_FALSE(Imported.bFocusPal);
    CHECK(Imported.MenuKey == L"K");
    CHECK(Imported.RerollKey == L"R");
}

TEST_CASE("a save without a Settings object yields an empty one")
{
    CHECK(SettingsObjectOf(R"({"Pals": {}})") == "{}");
}
