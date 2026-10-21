#include "SaveManager.hpp"
#include "ConfigManager.hpp"
#include "PalProcessor.hpp"
#include "InputManager.hpp"
#include "Utils.hpp"
#include "NotificationManager.hpp"
#include "SettingsStore.hpp"
#include <DynamicOutput/DynamicOutput.hpp>
#include <filesystem>
#include <fstream>

#include <PalCfg/Load.hpp>
#include <PalCfg/Paths.hpp>
#include <PalCfg/Win32.hpp>
#include "json.hpp"

#include <Unreal/UObject.hpp>
#include <Unreal/FString.hpp>

using namespace RC;
using namespace RC::Unreal;

namespace DynPals {

    namespace {
        // config.json's diagnostics: notes to the log, warnings and errors on screen as well.
        class ConfigSink final : public PalCfg::IDiagnosticSink {
        public:
            void Report(const PalCfg::Diagnostic& Diagnostic) override {
                const std::wstring Line = Utils::StringToWString(Diagnostic.field.empty() ? Diagnostic.message : Diagnostic.field + ": " + Diagnostic.message);
                switch (Diagnostic.severity) {
                case PalCfg::Severity::Note:
                    DP_LOG(Verbose, "[Config] {}", Line);
                    break;
                case PalCfg::Severity::Warning:
                    DP_LOG(Warning, "[Config] {}", Line);
                    NotificationManager::Get().EnqueueToast(L"[DynPals] config.json: " + Line, EPalLogPriority::Important, EPalLogContentToneType::Negative);
                    break;
                case PalCfg::Severity::Error:
                    DP_LOG(Error, "[Config] {}", Line);
                    break;
                }
            }
        };

        ConfigSink& Sink() {
            static ConfigSink Instance;
            return Instance;
        }

        // The newest world save, whose Settings seed a first config.json.
        std::wstring NewestWorldSave(const std::wstring& Folder, const std::wstring& Prefix) {
            std::error_code Error;
            std::wstring Newest;
            std::filesystem::file_time_type NewestTime{};
            for (const auto& Entry : std::filesystem::directory_iterator(Folder, Error)) {
                const std::wstring Name = Entry.path().filename().wstring();
                if (!Entry.is_regular_file(Error) || !Name.starts_with(Prefix) || !Name.ends_with(L".json")) continue;
                const auto Time = Entry.last_write_time(Error);
                if (Newest.empty() || Time > NewestTime) {
                    Newest = Entry.path().wstring();
                    NewestTime = Time;
                }
            }
            return Newest;
        }
    }

    void DynPalsSettings::CacheKeybinds() {
    bMenuIsGamepad = (MenuKey.rfind(L"Gamepad_", 0) == 0);
    MenuKeyVK = InputManager::Get().KeyNameToVK(MenuKey);
    MenuModVK = InputManager::Get().KeyNameToVK(MenuModifier);

    // --- CACHE REROLL KEYBIND ---
    bRerollIsGamepad = (RerollKey.rfind(L"Gamepad_", 0) == 0);
    RerollKeyVK = InputManager::Get().KeyNameToVK(RerollKey);
    RerollModVK = InputManager::Get().KeyNameToVK(RerollModifier);
}

    void SaveManager::Initialize(const std::wstring& BasePath) {
        ConfigPath = BasePath + L"Paks/~mods/"; 
        LoadGlobal();
        Relayer();
    }

    void SaveManager::LoadGlobal() {
        static const int Anchor = 0;
        std::string Folder;
        if (!PalCfg::ModuleDirectory(&Anchor, Folder)) {
            DP_LOG(Error, "Could not find the mod's folder; settings stay at their defaults for this session.");
            return;
        }

        Config = std::make_unique<ConfigFile>(kFields, PalCfg::ResolveAgainst(Folder, "../config.json"), kSchema.ModId());
        Config->SetSink(Sink());
        Config->SetOnReload([this](const DynPalsSettings&) {
            Relayer();
            DP_LOG(Default, "[Config] config.json reloaded.");
        });

        std::error_code Error;
        if (std::filesystem::exists(Utils::StringToWString(Config->Path()), Error)) {
            Config->Load();
            return;
        }

        DynPalsSettings Imported;
        const std::wstring Source = NewestWorldSave(ConfigPath, PersistFileName);
        if (!Source.empty()) {
            PalCfg::LoadFromText(kFields, SettingsObjectOf(Utils::ReadFileToString(Source)), Imported, Sink());
        }
        if (Config->Save(Imported)) {
            if (Source.empty()) DP_LOG(Default, "[Config] Wrote config.json with the defaults.");
            else DP_LOG(Default, "[Config] Wrote config.json from the settings in '{}'.", std::filesystem::path(Source).filename().wstring());
        }
    }

    void SaveManager::Relayer() {
        const DynPalsSettings Global = Config ? *Config->Get() : DynPalsSettings{};
        Settings = LayerWorld(Global, WorldSettingsText, Sink());
        Settings.CacheKeybinds();
    }

    void SaveManager::TickConfig() {
        if (Config) Config->Tick(ConfigFile::Clock::now());
    }

    void SaveManager::SaveGlobal() {
        if (!Config) return;
        DynPalsSettings Next = *Config->Get();
        Next.MenuKey = Settings.MenuKey;
        Next.MenuModifier = Settings.MenuModifier;
        Next.RerollKey = Settings.RerollKey;
        Next.RerollModifier = Settings.RerollModifier;
        Config->Save(Next);
    }

    void SaveManager::Reset() {
        CurrentWorldSaveID = L"";
        PersistedSwaps.clear();
        AccessOrder.clear(); 
        WorldSettingsText = "{}";
        Relayer();
    }

    void SaveManager::MarkAccessed(const std::wstring& InstanceID) {
        AccessOrder.remove(InstanceID);
        AccessOrder.push_front(InstanceID);
        
        while (AccessOrder.size() > MaxSaveEntries) {
            std::wstring oldestID = AccessOrder.back();
            AccessOrder.pop_back();
            PersistedSwaps.erase(oldestID);
        }
    }

    void SaveManager::LoadWorldData(UObject* World) {
        if (!World) return;

        UObject* GI = nullptr;
        Utils::GetPropertyValue<UObject*>(World, STR("OwningGameInstance"), GI, true);
        if (!GI) return;

        FString SaveDir;
        Utils::GetPropertyValue<FString>(GI, STR("SelectedWorldSaveDirectoryName"), SaveDir, true);
        std::wstring WorldSaveID = Utils::FStringToWString(SaveDir);

        if (WorldSaveID.empty()) {
            WorldSaveID = L"Multiplayer_Shared";
        }

        if (WorldSaveID == CurrentWorldSaveID) return;

        CurrentWorldSaveID = WorldSaveID;
        PersistedSwaps.clear();
        AccessOrder.clear(); 
        WorldSettingsText = "{}";
        Relayer();

        std::wstring persistPath = ConfigPath + PersistFileName + CurrentWorldSaveID + L".json";
        std::string content = Utils::ReadFileToString(persistPath);

        if (content.empty()) return;

        try {
            auto data = nlohmann::ordered_json::parse(content);
            
            if (data.contains("Settings") && data.at("Settings").is_object()) {
                WorldSettingsText = data.at("Settings").dump();
                Relayer();
            }

            if (data.contains("PersistencePals") && data.at("PersistencePals").is_object()) {
                for (auto& [instanceIdStr, palNode] : data.at("PersistencePals").items()) {
                    PalPersistData pd;
                    pd.InstanceID = Utils::StringToWString(instanceIdStr);
                    pd.PackName = Utils::StringToWString(palNode.value("PackName", ""));
                    pd.SkinName = Utils::StringToWString(palNode.value("SkinName", ""));
                    pd.SwapLabel = Utils::StringToWString(palNode.value("SwapLabel", palNode.value("SkinLabel", ""))); 
                    pd.SkelMeshPath = Utils::StringToWString(palNode.value("SkelMeshPath", ""));
                    pd.OriginalSkinName = Utils::StringToWString(palNode.value("OriginalSkinName", ""));
                    pd.bIsManuallyLocked = palNode.value("IsLocked", false);
                    pd.SizeMultiplier = palNode.value("SizeMultiplier", -1.0);

                    if (palNode.contains("Morphs") && palNode.at("Morphs").is_object()) {
                        for (auto& [morphName, morphVal] : palNode.at("Morphs").items()) {
                            pd.MorphSet[Utils::StringToWString(morphName)] = morphVal.get<double>();
                        }
                    }
                    if (palNode.contains("Mats") && palNode.at("Mats").is_object()) {
                        for (auto& [matIndex, matPath] : palNode.at("Mats").items()) {
                            pd.MatSet[matIndex] = Utils::StringToWString(matPath.get<std::string>());
                        }
                    }
                    if (palNode.contains("MatColors") && palNode.at("MatColors").is_object()) {
                        for (auto& [matIndex, colorArr] : palNode.at("MatColors").items()) {
                            if (colorArr.is_array() && colorArr.size() == 4) {
                                pd.MatColorSet[matIndex] = { colorArr[0].get<float>(), colorArr[1].get<float>(), colorArr[2].get<float>(), colorArr[3].get<float>() };
                            }
                        }
                    }
                    PersistedSwaps[pd.InstanceID] = pd;
                    AccessOrder.push_back(pd.InstanceID); 
                }
            }
        } catch (...) {
            DP_LOG(Error, "Failed to parse world persistence data. File might be corrupted.\n");
        }
    }

    void SaveManager::SaveWorldData() {
        if (CurrentWorldSaveID.empty()) return;
        
        std::wstring persistPath = ConfigPath + PersistFileName + CurrentWorldSaveID + L".json";
        
        // Fast snapshot on Game Thread (<0.05ms)
        auto palsCopy = PersistedSwaps;
        auto accessOrderCopy = AccessOrder;
        WorldSettingsText = Config ? WorldOverride(*Config->Get(), Settings) : "{}";
        std::string settingsCopy = WorldSettingsText;
        std::wstring worldIdCopy = CurrentWorldSaveID;

        // Offload JSON stringification and disk I/O to background thread
        std::thread([persistPath, palsCopy = std::move(palsCopy), accessOrderCopy = std::move(accessOrderCopy), 
                     settingsCopy = std::move(settingsCopy), worldIdCopy = std::move(worldIdCopy)]() {
            static std::mutex fileWriteMutex;
            std::lock_guard<std::mutex> lock(fileWriteMutex);

            nlohmann::ordered_json out;
            
            nlohmann::ordered_json systemObj;
            systemObj["ModVersion"] = "1.1.0";
            systemObj["WorldID"] = Utils::WStringToString(worldIdCopy);
            out["System"] = systemObj;
            
            out["Settings"] = nlohmann::ordered_json::parse(settingsCopy);
            
            nlohmann::ordered_json palsObj;
            for (const auto& id : accessOrderCopy) {
                auto it = palsCopy.find(id);
                if (it != palsCopy.end()) {
                    auto& data = it->second;
                    if (!data.ShouldSave()) continue;

                    nlohmann::ordered_json palNode;
                    palNode["PackName"] = Utils::WStringToString(data.PackName);
                    palNode["SkinName"] = Utils::WStringToString(data.SkinName);
                    palNode["SwapLabel"] = Utils::WStringToString(data.SwapLabel); 
                    palNode["SkelMeshPath"] = Utils::WStringToString(data.SkelMeshPath);
                    if (!data.OriginalSkinName.empty()) palNode["OriginalSkinName"] = Utils::WStringToString(data.OriginalSkinName);
                    palNode["IsLocked"] = data.bIsManuallyLocked; 
                    if (data.SizeMultiplier > 0.0) palNode["SizeMultiplier"] = data.SizeMultiplier;
                    
                    nlohmann::ordered_json morphsObj;
                    for (const auto& [mName, mVal] : data.MorphSet) {
                        morphsObj[Utils::WStringToString(mName)] = mVal;
                    }
                    palNode["Morphs"] = morphsObj;
                    
                    nlohmann::ordered_json matsObj;
                    for (const auto& [mIndex, mPath] : data.MatSet) {
                        matsObj[mIndex] = Utils::WStringToString(mPath);
                    }
                    if (!matsObj.empty()) palNode["Mats"] = matsObj;
                    
                    nlohmann::ordered_json matColorsObj;
                    for (const auto& [mIndex, mColor] : data.MatColorSet) {
                        matColorsObj[mIndex] = { mColor.R, mColor.G, mColor.B, mColor.A };
                    }
                    if (!matColorsObj.empty()) palNode["MatColors"] = matColorsObj;
                    
                    palsObj[Utils::WStringToString(id)] = palNode;
                }
            }
            
            out["PersistencePals"] = palsObj;

            std::ofstream file(persistPath);
            if (file.is_open()) {
                file << out.dump(4);
                RC::Output::send<RC::LogLevel::Default>(STR("[DynPals] Saved world persistence data cleanly to disk (async).\n"));
            }
        }).detach();
    }

    PalPersistData* SaveManager::GetPersistData(const std::wstring& InstanceID) {
        auto it = PersistedSwaps.find(InstanceID);
        if (it != PersistedSwaps.end()) {
            MarkAccessed(InstanceID);
            return &it->second;
        }
        return nullptr;
    }

    void SaveManager::SetPersistData(const std::wstring& InstanceID, const PalPersistData& Data, bool bWriteToDisk) {
        PersistedSwaps[InstanceID] = Data;
        MarkAccessed(InstanceID);
        
        if (bWriteToDisk) {
            SaveWorldData(); 
        }
    }
}