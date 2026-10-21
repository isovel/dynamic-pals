#define NOMINMAX 
#include <Windows.h>

#include <Mod/CppUserModBase.hpp>
#include <DynamicOutput/DynamicOutput.hpp>
#include <Unreal/UObjectArray.hpp>

#include "ConfigManager.hpp"
#include "SaveManager.hpp"
#include "HooksManager.hpp"
#include "Utils.hpp"
#include "VFXManager.hpp"
#include "FileWatcher.hpp"
#include "NativeAsyncLoader.hpp"

#include "UI/Views/UIManager.hpp" 
#include "PalProcessor.hpp"

#include <PalCore/Runtime/GameThread.hpp>
#include <PalUI/Client.hpp>

#include <optional>

using namespace RC;
using namespace RC::Unreal;

class FDynamicPalsGCListener : public FUObjectDeleteListener {
public:
    bool bIsRegistered = false;

    void Register() {
        if (!bIsRegistered) {
            UObjectArray::AddUObjectDeleteListener(this);
            bIsRegistered = true;
        }
    }

    void Unregister() {
        if (bIsRegistered) {
            bIsRegistered = false;
            UObjectArray::RemoveUObjectDeleteListener(this);
        }
    }

    void NotifyUObjectDeleted(const UObjectBase* Object, int32 Index) override {
        UObject* Obj = const_cast<UObject*>(static_cast<const UObject*>(Object));
        if (!Obj) return;

        bool bWasTracked = false;
        std::vector<const wchar_t*> sources;

        if (DynPals::PalProcessor::Get().OnUObjectDeleted(Obj)) {
            bWasTracked = true;
            sources.push_back(STR("PalProcessor"));
        }
        if (DynPals::NativeAsyncLoader::OnUObjectDeleted(Obj)) {
            bWasTracked = true;
            sources.push_back(STR("NativeAsyncLoader"));
        }
        if (DynPals::UIManager::Get().OnUObjectDeleted(Obj)) {
            bWasTracked = true;
            sources.push_back(STR("UI"));
        }
        if (DynPals::HooksManager::OnUObjectDeleted(Obj)) {
            bWasTracked = true;
            sources.push_back(STR("HooksManager"));
        }
        if (DynPals::VFXManager::Get().OnUObjectDeleted(Obj)) {
            bWasTracked = true;
            sources.push_back(STR("VFXManager"));
        }

        if (bWasTracked) {
            std::wstring ObjName = Obj->GetName();
            UClass* Cls = Obj->GetClassPrivate();
            std::wstring ClassName = Cls ? Cls->GetName() : L"None";

            std::wstring sourcesStr = L"";
            for (size_t i = 0; i < sources.size(); ++i) {
                sourcesStr += sources[i];
                if (i + 1 < sources.size()) sourcesStr += L", ";
            }

            RC::Output::send<RC::LogLevel::Default>(
                STR("[GC] Purged Tracked Object: '{}' (Class: '{}', Source: [{}], Index: {})\n"), 
                ObjName, ClassName, sourcesStr, Index
            );
        }
    }
    
    // FIX: When Unreal Engine shuts down the object array, unregister immediately!
    void OnUObjectArrayShutdown() override {
        Unregister();
    } 
};
static FDynamicPalsGCListener GGCListener;


class DynPalsMod : public CppUserModBase
{
public:
    DynPalsMod() : CppUserModBase()
    {
        ModName = STR("DynamicPals");
        ModVersion = STR("1.0.0");
        ModDescription = STR("Native C++ run-time mesh and material swapper.");
        ModAuthors = STR("Modder");
    }

    ~DynPalsMod() override 
    {
        GGCListener.Unregister();
        DynPals::UIManager::Get().Detach();
        UI.reset();
        PalCore::GameThread::Tasks().RemoveFrameCallback(ConfigPoll);
        if (bStarted) PalCore::GameThread::Stop();
    }

    auto on_update() -> void override {}
    
    auto on_unreal_init() -> void override
    {
        bStarted = PalCore::GameThread::Start(STR("DynamicPals"));
        if (!bStarted) DP_LOG(Error, "pal-core's game-thread pump did not start");

        UI.emplace("DynamicPals", [](std::uint8_t, std::string_view Context, std::string_view Message) {
            DP_LOG(Warning, "[PalUI] {}: {}", DynPals::Utils::StringToWString(std::string(Context)), DynPals::Utils::StringToWString(std::string(Message)));
        });
        if (!UI->IsConnected()) {
            DP_LOG(Error, "PalUI.dll did not accept the connection; the Alt+N menu is off. Install the PalUI mod beside DynamicPals.");
            UI.reset();
        }
        DynPals::UIManager::Get().Attach(UI ? &*UI : nullptr);


        // Register the listener safely
        GGCListener.Register();

        UObject* KismetLib = UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, STR("/Script/Engine.Default__KismetSystemLibrary"));
        if (KismetLib) {
            FString ContentDir;
            DynPals::Utils::CallFunction(KismetLib, STR("GetProjectContentDirectory"), &ContentDir);
            std::wstring BasePath = DynPals::Utils::FStringToWString(ContentDir);

            DynPals::SaveManager::Get().Initialize(BasePath);
            // After Initialize: ConfigFile is driven from one thread, and this runs on UE4SS's.
            ConfigPoll = PalCore::GameThread::Tasks().AddFrameCallback([](float) { DynPals::SaveManager::Get().TickConfig(); });
            DynPals::ConfigManager::Get().Initialize(BasePath);
            DynPals::FileWatcher::Start(BasePath + L"Paks/~mods/");
            DynPals::VFXManager::Get().Initialize(); 
            DynPals::HooksManager::RegisterHooks();

            DynPals::UIManager::Get();
        }
    }

private:
    bool bStarted = false;
    std::uint64_t ConfigPoll = 0;
    std::optional<PalUI::Client::Connection> UI;
};

#define DYNPALS_MOD_API __declspec(dllexport)
extern "C"
{
    DYNPALS_MOD_API CppUserModBase* start_mod() {
        return new DynPalsMod();
    }

    DYNPALS_MOD_API void uninstall_mod(CppUserModBase* mod) {
        delete mod;
    }
}