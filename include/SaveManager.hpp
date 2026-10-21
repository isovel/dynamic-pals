#pragma once
#include <string>
#include <map>
#include <list>
#include <Unreal/UObjectGlobals.hpp>
#include "DataTypes.hpp"
#include "Settings.hpp"

#include <PalCfg/ConfigFile.hpp>

#include <memory>
#include <optional>

namespace DynPals {

    class SaveManager {
    public:
        static SaveManager& Get() {
            static SaveManager instance;
            return instance;
        }

        void Initialize(const std::wstring& BasePath);
        void LoadWorldData(RC::Unreal::UObject* World);
        void SaveWorldData();

        PalPersistData* GetPersistData(const std::wstring& InstanceID);
        void SetPersistData(const std::wstring& InstanceID, const PalPersistData& Data, bool bWriteToDisk = false);

        void Reset();

        // config.json: polled for hand edits, and written by SaveGlobal.
        void TickConfig();
        void SaveGlobal();

        // config.json with the loaded world's camera overrides laid over it.
        DynPalsSettings Settings;

    private:
        SaveManager() = default;
        SaveManager(const SaveManager&) = delete;
        SaveManager& operator=(const SaveManager&) = delete;

        void MarkAccessed(const std::wstring& InstanceID);
        void LoadGlobal();
        void Relayer();
        // config.json's settings, or those it could not be written with.
        DynPalsSettings Global() const;

        using ConfigFile = PalCfg::ConfigFile<DynPalsSettings, kFields.size()>;
        std::unique_ptr<ConfigFile> Config;
        // The loaded world's Settings object, kept so a reload of config.json can lay it over again.
        std::string WorldSettingsText = "{}";
        // The global settings Settings was last laid over; a world override is measured from these.
        DynPalsSettings LayeredGlobal;
        std::optional<DynPalsSettings> Unsaved;

        std::wstring ConfigPath;
        std::wstring PersistFileName = L"_DynPals_Save_";
        std::wstring CurrentWorldSaveID = L"";
        
        std::map<std::wstring, PalPersistData> PersistedSwaps;
        std::list<std::wstring> AccessOrder;
        const size_t MaxSaveEntries = 1000;
    };
}