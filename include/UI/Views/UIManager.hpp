#pragma once
#include <chrono>
#include <functional>
#include <memory>
#include <set>
#include <string>
#include <vector>
#include <Unreal/UObjectGlobals.hpp>

namespace PalUI::Client { class Connection; class Row; }

namespace DynPals {

    // The Alt+N menu: a PalUI window over the pal in the crosshair. The PalUI
    // state lives in the source file, so this header stays free of Windows.h.
    class UIManager {
    public:
        static UIManager& Get() {
            static UIManager instance;
            return instance;
        }

        ~UIManager();

        // Null when the PalUI mod is absent; the menu then only says so.
        void Attach(PalUI::Client::Connection* InUI);
        PalUI::Client::Connection* Connection() const { return UI; }

        void Toggle(RC::Unreal::UObject* PlayerController);
        bool IsMenuOpen() const;
        // Rebuilds the open menu on the next tick, as after a config reload.
        void RequestRebuild() { bNeedsRefresh = true; }
        void Tick();
        void CloseForUnload();
        // Drops pointers to an object the engine has deleted; true when one was held.
        bool OnUObjectDeleted(RC::Unreal::UObject* Obj);
        // Drops the window before the connection it was made on.
        void Detach();

        // The game's OK/Cancel dialog, queued behind one already showing; false without PalUI.
        bool ShowChoice(const std::wstring& Message, std::function<void(bool)> OnResult);

        // Saves a debounced menu edit now.
        void FlushPendingSave();
        // config.json changed: the camera follows it and the open menu is filled again.
        void OnSettingsReloaded();

        void PreloadUI(RC::Unreal::UObject* PC);

    private:
        UIManager();

        struct State;
        std::unique_ptr<State> S;
        PalUI::Client::Connection* UI = nullptr;
        bool bWarnedMissing = false;
        std::set<std::string> LoggedKinds;

        void WarnMissing();
        // Row, logging once per kind when PalUI could not build it.
        PalUI::Client::Row Built(PalUI::Client::Row Row, const char* Kind);

        bool Open();
        void Close();
        void OnClosed();
        void Fill();
        bool IsTargetLocked();
        void FillSettings();
        void FillPreferences();
        void StartKeyCapture(int Target);
        std::wstring FormatKeybindText(const std::wstring& Modifier, const std::wstring& Key);

        void UpdateTarget();
        void ApplySize(double NewValue);
        void ApplyMorph(const std::wstring& MorphName, double NewValue);

        // --- Consolidated Helpers ---
        RC::Unreal::UObject* GetCameraBoom(RC::Unreal::UObject* Pal);

        // --- Debounced Save System ---
        bool bPendingDiskSave = false;
        std::chrono::steady_clock::time_point LastSliderChangeTime;

        void TriggerSaveDebounced();
        void ShowNextChoice();

        // --- Camera Helper Methods ---
        void EnablePalCamera();
        void DisablePalCamera();
        void UpdatePalCameraRotation(double Yaw, bool bTeleport = false);

        // State Tracking
        bool bHideInvalidSwaps = true;
        bool bNeedsRefresh = false;
        bool bWasOpen = false;
        bool bPrefsStale = false;
        // When the last key capture ended, so the Escape that ended it does not also close the menu.
        std::chrono::steady_clock::time_point CaptureEndedAt;

        // Target Context
        RC::Unreal::UObject* CurrentPlayerController = nullptr;
        RC::Unreal::UObject* TargetPal = nullptr;
        std::wstring TargetInstanceID = L"";
        std::wstring TargetCharID = L"";

        // Auto-refresh state trackers
        double LastObservedSize = -999.0;                      // <--- Size Tracker
        std::wstring LastObservedLabel = L"";                  // <--- Swap Tracker

        // Camera Members
        RC::Unreal::UObject* OriginalViewTarget = nullptr;
        bool bIsPalCameraActive = false;
    };
}
