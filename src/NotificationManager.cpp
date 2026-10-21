#define NOMINMAX
#include <Windows.h>
#include <shellapi.h>

#include "NotificationManager.hpp"
#include "UI/Views/UIManager.hpp"
#include <PalCore/Runtime/GameThread.hpp>
#include <PalCore/Runtime/Pal.hpp>
#include <PalCore/Runtime/Toast.hpp>
#include "Utils.hpp"
#include <Unreal/FString.hpp>

using namespace RC::Unreal;

namespace DynPals {
    // Owner of this file's queued game-thread work.
    static const char QueueTag = 0;


    void EnqueueUIToast(const std::wstring& Message, uint8_t PriorityType, uint8_t ToneType) {
        NotificationManager::Get().EnqueueToast(
            Message, 
            static_cast<EPalLogPriority>(PriorityType), 
            static_cast<EPalLogContentToneType>(ToneType)
        );
    }

    void NotificationManager::EnqueueToast(const std::wstring& Message, EPalLogPriority Priority, EPalLogContentToneType Tone) {
        const PalCore::Priority Rank = Priority == EPalLogPriority::None ? PalCore::Priority::Normal : static_cast<PalCore::Priority>(Priority);
        PalCore::Toast(Message, static_cast<PalCore::Tone>(Tone), Rank);
    }

    void NotificationManager::ClearInGameLogs() {
    // 2. Remove all VeryImportant logs from the native PalLogManager subsystem
    UObject* LogManager = PalCore::LogManager();
    if (LogManager && Utils::IsObjectValid(LogManager)) {
        UFunction* RemoveVeryImportantFunc = LogManager->GetFunctionByNameInChain(STR("RemoveVeryImportantLog"));
        
        // Read veryImportantLogMap from UPalLogManager (TMap<FGuid, FPalLogDataSet> at offset 0x0090)
        FProperty* VILogMapProp = Utils::GetProperty(LogManager, STR("veryImportantLogMap"));
        if (VILogMapProp && RemoveVeryImportantFunc) {
            // Collect GUIDs to remove
            TArray<FGuid> LogIdsToRemove;
            auto* MapHelper = VILogMapProp->ContainerPtrToValuePtr<void>(LogManager);
            
            // Or safely pull GUIDs via reflection if direct layout isn't bound:
            // Iterate over widgets to get the IDs currently displayed
        }
    }

    // 3. Clear the active view widgets in WBP_PalLogWidget_C
    std::vector<UObject*> logWidgets;
    UObjectGlobals::FindAllOf(STR("WBP_PalLogWidget_C"), logWidgets);

    for (UObject* LogWidget : logWidgets) {
        if (!LogWidget || !Utils::IsObjectValid(LogWidget)) continue;

        // Collect all active Very Important GUIDs from the widget
        TArray<FGuid> ActiveVIGuids;
        FProperty* VeryImpIDArrayProp = Utils::GetProperty(LogWidget, STR("veryImportantLogIDArray"));
        if (VeryImpIDArrayProp) {
            TArray<FGuid>* IDArray = VeryImpIDArrayProp->ContainerPtrToValuePtr<TArray<FGuid>>(LogWidget);
            if (IDArray) {
                ActiveVIGuids = *IDArray;
                IDArray->Empty();
            }
        }

        // Also check nowDisplayVeryImportantLog
        FGuid CurrentDisplayedGuid{};
        if (Utils::GetPropertyValue<FGuid>(LogWidget, STR("nowDisplayVeryImportantLog"), CurrentDisplayedGuid)) {
            if (CurrentDisplayedGuid.A != 0 || CurrentDisplayedGuid.B != 0) {
                ActiveVIGuids.Add(CurrentDisplayedGuid);
                FGuid ZeroGuid{};
                Utils::SetPropertyValue<FGuid>(LogWidget, STR("nowDisplayVeryImportantLog"), ZeroGuid);
            }
        }

        // Notify UPalLogManager to remove each active very important log
        if (LogManager && Utils::IsObjectValid(LogManager)) {
            UFunction* RemoveVIFunc = LogManager->GetFunctionByNameInChain(STR("RemoveVeryImportantLog"));
            if (RemoveVIFunc) {
                for (const FGuid& id : ActiveVIGuids) {
                    struct { FGuid targetLogId; bool ReturnValue; } Params{ id, false };
                    Utils::SafeProcessEvent(LogManager, RemoveVIFunc, &Params);
                }
            }
        }

        // Call widget's native removal function if present
        UFunction* OnRemovedVIFunc = LogWidget->GetFunctionByNameInChain(STR("OnRemovedVeryImportantLog"));
        if (OnRemovedVIFunc) {
            for (const FGuid& id : ActiveVIGuids) {
                struct { FGuid logId; } Params{ id };
                Utils::SafeProcessEvent(LogWidget, OnRemovedVIFunc, &Params);
            }
        }

        // Clear display containers
        UObject* NormalScrollBox = nullptr;
        if (Utils::GetPropertyValue<UObject*>(LogWidget, STR("ScrollBox_NormalLog"), NormalScrollBox) && NormalScrollBox) {
            Utils::CallFunction(NormalScrollBox, STR("ClearChildren"));
        }

        UObject* ImportantBorder = nullptr;
        if (Utils::GetPropertyValue<UObject*>(LogWidget, STR("ImportantBorder"), ImportantBorder) && ImportantBorder) {
            Utils::CallFunction(ImportantBorder, STR("ClearChildren"));
        }

        UObject* VeryImportantBorder = nullptr;
        if (Utils::GetPropertyValue<UObject*>(LogWidget, STR("VeryImportantBorder"), VeryImportantBorder) && VeryImportantBorder) {
            Utils::CallFunction(VeryImportantBorder, STR("ClearChildren"));
        }

        // Clear tracking lists
        FProperty* NormalListProp = Utils::GetProperty(LogWidget, STR("NormalLogList"));
        if (NormalListProp) {
            TArray<UObject*>* NormalList = NormalListProp->ContainerPtrToValuePtr<TArray<UObject*>>(LogWidget);
            if (NormalList) NormalList->Empty();
        }

        FProperty* ImportantListProp = Utils::GetProperty(LogWidget, STR("ImportantLogList"));
        if (ImportantListProp) {
            TArray<UObject*>* ImportantList = ImportantListProp->ContainerPtrToValuePtr<TArray<UObject*>>(LogWidget);
            if (ImportantList) ImportantList->Empty();
        }
    }
}

    void NotificationManager::ShowModalDialog(const std::wstring& Message) {
        PalCore::GameThread::Tasks().Enqueue(&QueueTag, [Message]() {
            UObject* LogManager = PalCore::LogManager(); 
            UObject* WorldContext = LogManager ? LogManager->GetOuterPrivate() : nullptr;
            UObject* PlayerController = nullptr;

            if (WorldContext && Utils::IsObjectValid(WorldContext)) {
                UObject* GameplayStatics = UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, STR("/Script/Engine.Default__GameplayStatics"));
                if (GameplayStatics) {
                    struct { UObject* WorldContextObject; int32_t PlayerIndex; UObject* ReturnValue; } GSParams{WorldContext, 0, nullptr};
                    Utils::CallFunction(GameplayStatics, STR("GetPlayerController"), &GSParams);
                    PlayerController = GSParams.ReturnValue;
                }
            }
            if (!PlayerController) PlayerController = UObjectGlobals::FindFirstOf(STR("PalPlayerController"));

            if (!PlayerController || !Utils::IsObjectValid(PlayerController)) return;

            UObject* PalUtil = UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, STR("/Script/Pal.Default__PalUtility"));
            if (!PalUtil) return;

            UFunction* AlertFunc = PalUtil->GetFunctionByNameInChain(STR("Alert"));
            if (AlertFunc) {
                alignas(8) uint8_t AlertParams[256] = {0};
                
                // Initialize ALL parameters
                for (FProperty* Prop = (FProperty*)AlertFunc->GetChildProperties(); Prop; Prop = (FProperty*)Utils::GetNextField(Prop)) {
                    Prop->InitializeValue_InContainer(AlertParams);
                }

                FProperty* WCProp = AlertFunc->GetPropertyByNameInChain(STR("WorldContextObject"));
                if (!WCProp) WCProp = AlertFunc->GetPropertyByNameInChain(STR("WorldContext"));
                if (WCProp) *WCProp->ContainerPtrToValuePtr<UObject*>(AlertParams) = PlayerController;

                FProperty* MsgProp = AlertFunc->GetPropertyByNameInChain(STR("Message"));
                if (!MsgProp) MsgProp = AlertFunc->GetPropertyByNameInChain(STR("Msg"));
                if (MsgProp) Utils::AssignStringToTextProperty(Message, AlertParams, MsgProp);

                Utils::SafeProcessEvent(PalUtil, AlertFunc, AlertParams);

                // Destroy ALL parameters
                for (FProperty* Prop = (FProperty*)AlertFunc->GetChildProperties(); Prop; Prop = (FProperty*)Utils::GetNextField(Prop)) {
                    Prop->DestroyValue_InContainer(AlertParams);
                }
                DP_LOG(Default, "[NotificationManager] Displayed native modal alert dialog.");


            }
        });
    }

    // 2-Button Hijacked Modal Controller

    void NotificationManager::ShowTwoButtonModal(
        const std::wstring& Message,
        const std::wstring& RightBtnText, std::function<void()> OnRightClick,
        const std::wstring& LeftBtnText, std::function<void()> OnLeftClick
    ) {
        PalCore::GameThread::Tasks().Enqueue(&QueueTag, [Message, RightBtnText, OnRightClick, LeftBtnText, OnLeftClick]() {
            // The game's own dialog labels its buttons OK and Cancel, so the message names what each does.
            const std::wstring Text = Message + L"\n\nOK: " + RightBtnText + L"    Cancel: " + LeftBtnText;
            const bool bShown = UIManager::Get().ShowChoice(Text, [OnRightClick, OnLeftClick](bool bAccepted) {
                if (bAccepted && OnRightClick) OnRightClick();
                if (!bAccepted && OnLeftClick) OnLeftClick();
            });
            if (!bShown) NotificationManager::Get().ShowModalDialog(Message);
        });
    }
}