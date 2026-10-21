#pragma once
#include <string>

#include <PalCfg/Schema.hpp>

namespace DynPals {

    struct DynPalsSettings {
    bool bFocusPal = true;
    double CameraRotation = 180.0;
    bool bRelativeCamera = true;
    
    std::wstring MenuKey = L"N";
    std::wstring MenuModifier = L"LeftAlt"; 
    std::wstring TestMenuKey = L"G";
    std::wstring TestMenuModifier = L"LeftAlt";

    // --- ADD REROLL KEYBIND (Unset by default) ---
    std::wstring RerollKey = L"None";
    std::wstring RerollModifier = L"None";

    // Pre-cached hotkey data
    int MenuKeyVK = 'N';
    int MenuModVK = 0x12; // VK_MENU
    bool bMenuIsGamepad = false;

    int TestKeyVK = 'G';
    int TestModVK = 0x12;
    bool bTestIsGamepad = false;

    // --- ADD PRE-CACHED VKs ---
    int RerollKeyVK = 0;
    int RerollModVK = 0;
    bool bRerollIsGamepad = false;

    void CacheKeybinds();
};

    // config.json, beside the DLL. The keys are the ones world saves have always
    // used, so one schema reads both.
    inline constexpr auto kSchema = PalCfg::Schema<DynPalsSettings>("DynamicPals")
        .Field("FocusPal", &DynPalsSettings::bFocusPal)
            .Label("Focus pal camera")
            .Help("Turn the camera to the pal while the Alt+N menu is open.\nA world can override this; see its save under Paks/~mods.")
        .Field("CameraRotation", &DynPalsSettings::CameraRotation)
            .Label("Camera rotation")
            .Help("Degrees around the pal the focus camera sits at.")
            .Range(0.0, 360.0)
        .Field("RelativeCamera", &DynPalsSettings::bRelativeCamera)
            .Label("Relative camera")
            .Help("Measure the rotation from the pal's facing instead of the world's.")
        .Field("MenuKey", &DynPalsSettings::MenuKey)
            .Label("Menu key")
            .Help("Key names as Unreal spells them: N, F5, Gamepad_FaceButton_Top. None unbinds.")
        .Field("MenuModifier", &DynPalsSettings::MenuModifier)
            .Label("Menu modifier")
        .Field("RerollKey", &DynPalsSettings::RerollKey)
            .Label("Reroll key")
            .Help("Rerolls the pal in the crosshair.")
        .Field("RerollModifier", &DynPalsSettings::RerollModifier)
            .Label("Reroll modifier");

    inline constexpr auto kFields = kSchema.Flatten();

    // The settings a world's save may override.
    inline constexpr auto kWorldSchema = PalCfg::Schema<DynPalsSettings>("DynamicPals")
        .Field("FocusPal", &DynPalsSettings::bFocusPal)
        .Field("CameraRotation", &DynPalsSettings::CameraRotation)
            .Range(0.0, 360.0)
        .Field("RelativeCamera", &DynPalsSettings::bRelativeCamera);

    inline constexpr auto kWorldFields = kWorldSchema.Flatten();
}
