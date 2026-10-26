#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "Settings.hpp"
#include "SettingsStore.hpp"
#include "json.hpp"

#include <PalCfg/Diagnostics.hpp>
#include <PalCfg/Load.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <vector>
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

TEST_CASE("a world override laid back over the global settings gives the effective ones")
{
    DynPalsSettings Global, Effective;
    Effective.CameraRotation = 75.0;
    Effective.bRelativeCamera = false;
    PalCfg::CollectingSink Sink;
    const DynPalsSettings Again = LayerWorld(Global, WorldOverride(Global, Effective), Sink);
    CHECK(Again.CameraRotation == 75.0);
    CHECK_FALSE(Again.bRelativeCamera);
    CHECK(Again.bFocusPal == Global.bFocusPal);
    CHECK(Sink.All().empty());
}

TEST_CASE("a save without a Settings object imports the defaults")
{
    DynPalsSettings Imported;
    Imported.MenuKey = L"K";
    PalCfg::CollectingSink Sink;
    PalCfg::LoadFromText(kFields, SettingsObjectOf(R"({"Pals": {}})"), Imported, Sink);
    CHECK(Imported.MenuKey == L"N");
    CHECK(Imported.CameraRotation == 180.0);
}

TEST_CASE("the newest world save is the one the import reads")
{
    using Clock = std::filesystem::file_time_type::clock;
    const auto Now = Clock::now();
    const std::vector<SaveCandidate> Files = {
        {L"/m/_DynPals_Save_A.json", Now - std::chrono::hours(2)},
        {L"/m/_DynPals_Save_B.json", Now - std::chrono::hours(1)},
        {L"/m/_DynPals_Save_C.json.bak", Now},
        {L"/m/other.json", Now},
    };
    CHECK(NewestWorldSave(Files, L"_DynPals_Save_") == L"/m/_DynPals_Save_B.json");
    CHECK(NewestWorldSave({}, L"_DynPals_Save_").empty());
}

TEST_CASE("the side offset centres a pal right of the menu")
{
    CHECK(DynPals::CentredSideOffset(0.5, 100.0, 90.0) == Catch::Approx(-50.0));
    CHECK(DynPals::CentredSideOffset(0.4, 200.0, 90.0) == Catch::Approx(-80.0));
    CHECK(DynPals::CentredSideOffset(0.0, 100.0, 90.0) == 0.0);
    CHECK(DynPals::CentredSideOffset(1.0, 100.0, 90.0) == 0.0);
    CHECK(DynPals::CentredSideOffset(0.5, 100.0, 0.0) == 0.0);
    CHECK(DynPals::CentredSideOffset(0.5, 100.0, 180.0) == 0.0);
}

TEST_CASE("a dragged rotation wraps into one turn")
{
    CHECK(DynPals::TurnedRotation(359.0, 4.0) == 1.0);
    CHECK(DynPals::TurnedRotation(1.0, -4.0) == 359.0);
    CHECK(DynPals::TurnedRotation(0.0, 720.0) == 0.0);
    CHECK(DynPals::TurnedRotation(90.0, 0.0) == 90.0);
    CHECK(DynPals::TurnedRotation(0.0, -1e-14) < 360.0);
}
