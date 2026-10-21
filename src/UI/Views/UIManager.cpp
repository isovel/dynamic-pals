#define NOMINMAX 
#include <Windows.h>

#include "UI/Views/UIManager.hpp"
#include "ConfigManager.hpp"
#include "NotificationManager.hpp"
#include "SaveManager.hpp"
#include "PalProcessor.hpp"
#include "InputManager.hpp"
#include "Utils.hpp"
#include "DataTypes.hpp"
#include <PalUI/Client.hpp>
#include <cmath>
#include <map>
#include <algorithm>
#include <chrono>
#include <deque>
#include <optional>
#include <utility>

using namespace RC;
using namespace RC::Unreal;

namespace DynPals {

    struct UIManager::State {
        std::optional<PalUI::Client::Window> Window;
        PalUI::Client::Page Settings;
        PalUI::Client::Page Prefs;
        // One choice row per pack, and the config each of its choices swaps to.
        std::vector<PalUI::Client::Row> PackRows;
        std::vector<std::vector<int>> PackConfigs;
        PalUI::Client::Row CaptureRow;
        std::optional<PalUI::Client::Dialog> Choice;
        std::deque<std::pair<std::wstring, std::function<void(bool)>>> Choices;
        bool bCloseRequested = false;
        // PalUI takes one title bar per build, so a changed lock state rebuilds the window.
        bool bShownLocked = false;
    };

    UIManager::UIManager() : S(std::make_unique<State>()) {}
    UIManager::~UIManager() = default;

    // Set to true for smooth camera interpolation; set to false for instant snapping
    static constexpr bool bEnableCameraSmoothing = false;

    // Helper: Strips any 8-character hexadecimal fallback hash like " (41BCA68A)" from labels
    static std::wstring StripFallbackHash(std::wstring str) {
        size_t openPos = 0;
        while ((openPos = str.find(L" (", openPos)) != std::wstring::npos) {
            if (openPos + 10 < str.length() && str[openPos + 10] == L')') {
                bool isHex = true;
                for (size_t k = openPos + 2; k < openPos + 10; ++k) {
                    if (!std::iswxdigit(str[k])) {
                        isHex = false;
                        break;
                    }
                }
                if (isHex) {
                    str.erase(openPos, 11);
                    continue;
                }
            }
            openPos += 2;
        }
        return str;
    }

    // Helper: Safely updates USceneComponent rotation using native reflection functions
    static void SetComponentRotationDirect(RC::Unreal::UObject* Comp, const FRotator_UE5& Rot, bool bWorldSpace, bool bTeleport = false) {
        if (!Comp || !Utils::IsObjectValid(Comp)) return;

        const wchar_t* FuncName = bWorldSpace ? STR("K2_SetWorldRotation") : STR("K2_SetRelativeRotation");
        RC::Unreal::UFunction* Func = Comp->GetFunctionByNameInChain(FuncName);
        if (!Func) return;

        alignas(8) uint8_t Buffer[512] = {0};
        for (RC::Unreal::FProperty* Prop = (RC::Unreal::FProperty*)Func->GetChildProperties(); Prop; Prop = (RC::Unreal::FProperty*)Utils::GetNextField(Prop)) {
            Prop->InitializeValue_InContainer(Buffer);
        }

        RC::Unreal::FProperty* RotProp = Func->GetPropertyByNameInChain(STR("NewRotation"));
        if (RotProp) {
            *RotProp->ContainerPtrToValuePtr<FRotator_UE5>(Buffer) = Rot;
        }

        RC::Unreal::FProperty* TeleportProp = Func->GetPropertyByNameInChain(STR("bTeleport"));
        if (TeleportProp && TeleportProp->GetClass().GetName() == STR("BoolProperty")) {
            static_cast<RC::Unreal::FBoolProperty*>(TeleportProp)->SetPropertyValue(TeleportProp->ContainerPtrToValuePtr<void>(Buffer), bTeleport);
        }

        Utils::SafeProcessEvent(Comp, Func, Buffer);

        for (RC::Unreal::FProperty* Prop = (RC::Unreal::FProperty*)Func->GetChildProperties(); Prop; Prop = (RC::Unreal::FProperty*)Utils::GetNextField(Prop)) {
            Prop->DestroyValue_InContainer(Buffer);
        }
    }

    // --- CONSOLIDATED CAMERA BOOM RESOLVER ---
    RC::Unreal::UObject* UIManager::GetCameraBoom(RC::Unreal::UObject* Pal) {
        if (!Pal || !Utils::IsObjectValid(Pal)) return nullptr;

        UObject* CameraBoomObj = nullptr;
        if (Utils::GetPropertyValue<UObject*>(Pal, STR("CameraBoom"), CameraBoomObj, true) && CameraBoomObj) {
            return CameraBoomObj;
        }

        UClass* SpringArmClass = UObjectGlobals::StaticFindObject<UClass*>(nullptr, nullptr, STR("/Script/Engine.SpringArmComponent"));
        if (SpringArmClass) {
            struct { UClass* ComponentClass; UObject* ReturnValue; } GetCompParams{SpringArmClass, nullptr};
            Utils::CallFunction(Pal, STR("GetComponentByClass"), &GetCompParams, true);
            return GetCompParams.ReturnValue;
        }
        return nullptr;
    }

    // --- CONSOLIDATED SCROLL OFFSET CACHER ---

    // --- DEBOUNCED DISK SAVE METHODS ---
    void UIManager::TriggerSaveDebounced() {
        bPendingDiskSave = true;
        LastSliderChangeTime = std::chrono::steady_clock::now();
    }

    void UIManager::FlushPendingSave() {
        if (bPendingDiskSave) {
            bPendingDiskSave = false;
            SaveManager::Get().SaveWorldData();
        }
    }

    // Struct to remember vanilla camera settings when closing the menu
    struct FCachedCameraState {
        float WalkArmLength = 300.0f;
        float TargetArmLength = 300.0f;
        FVector WalkOffset{ 0.0, 0.0, 0.0 };
        FVector SocketOffset{ 0.0, 0.0, 0.0 };
        FVector TargetOffset{ 0.0, 0.0, 0.0 };
        bool bCollisionTest = true;
        float ProbeSize = 12.0f;
        bool bValid = false;
    };
    static FCachedCameraState GCachedCamState;

    void UIManager::EnablePalCamera() {
        if (!CurrentPlayerController || !TargetPal || bIsPalCameraActive) return;

        if (OriginalViewTarget && !Utils::IsObjectValid(OriginalViewTarget)) {
            OriginalViewTarget = nullptr;
        }

        // 1. Force-Activate FollowCamera
        UObject* FollowCameraObj = nullptr;
        if (!Utils::GetPropertyValue<UObject*>(TargetPal, STR("FollowCamera"), FollowCameraObj, true) || !FollowCameraObj) {
            UClass* CameraClass = UObjectGlobals::StaticFindObject<UClass*>(nullptr, nullptr, STR("/Script/Engine.CameraComponent"));
            if (CameraClass) {
                struct { UClass* ComponentClass; UObject* ReturnValue; } GetCompParams{CameraClass, nullptr};
                Utils::CallFunction(TargetPal, STR("GetComponentByClass"), &GetCompParams, true);
                FollowCameraObj = GetCompParams.ReturnValue;
            }
        }

        if (FollowCameraObj) {
            Utils::SetPropertyValue<bool>(FollowCameraObj, STR("bIsActive"), true);
            struct { bool bReset; } ActParams{ false };
            Utils::CallFunction(FollowCameraObj, STR("Activate"), &ActParams, true);
        }

        // 2. Configure CameraBoom
        UObject* CameraBoomObj = GetCameraBoom(TargetPal);
        if (CameraBoomObj) {
            // A. Cache existing values so we can restore them cleanly on close
            Utils::GetPropertyValue<float>(CameraBoomObj, STR("WalkCameraArmLength"), GCachedCamState.WalkArmLength);
            Utils::GetPropertyValue<float>(CameraBoomObj, STR("TargetArmLength"), GCachedCamState.TargetArmLength);
            Utils::GetPropertyValue<FVector>(CameraBoomObj, STR("WalkCameraOffset"), GCachedCamState.WalkOffset);
            Utils::GetPropertyValue<FVector>(CameraBoomObj, STR("SocketOffset"), GCachedCamState.SocketOffset);
            Utils::GetPropertyValue<FVector>(CameraBoomObj, STR("TargetOffset"), GCachedCamState.TargetOffset);
            Utils::GetPropertyValue<bool>(CameraBoomObj, STR("bDoCollisionTest"), GCachedCamState.bCollisionTest);
            Utils::GetPropertyValue<float>(CameraBoomObj, STR("ProbeSize"), GCachedCamState.ProbeSize);
            GCachedCamState.bValid = true;

            // B. Calculate optimal camera distance based on physical Pal dimensions
            struct { bool bOnlyCollidingComponents; uint8_t Pad[7]; FVector_UE5 Origin; FVector_UE5 BoxExtent; } BoundsParams{true, {0}, {0.0, 0.0, 0.0}, {0.0, 0.0, 0.0}};
            Utils::CallFunction(TargetPal, STR("GetActorBounds"), &BoundsParams, true);
            
            double maxExtent = std::max({ BoundsParams.BoxExtent.X, BoundsParams.BoxExtent.Y, BoundsParams.BoxExtent.Z });
            float optimalDistance = static_cast<float>(std::clamp(maxExtent * 2.8, 350.0, 3500.0));

            // C. Disable all collision checks
            Utils::SetPropertyValue<bool>(CameraBoomObj, STR("bDoCollisionTest"), false);
            Utils::SetPropertyValue<float>(CameraBoomObj, STR("ProbeSize"), 0.0f);

            // D. Zero out sideways shoulder offsets so Pal is perfectly centered
            FVector ZeroVec{ 0.0, 0.0, 0.0 };
            Utils::SetPropertyValue<FVector>(CameraBoomObj, STR("WalkCameraOffset"), ZeroVec);
            Utils::SetPropertyValue<FVector>(CameraBoomObj, STR("SocketOffset"), ZeroVec);
            Utils::SetPropertyValue<FVector>(CameraBoomObj, STR("TargetOffset"), ZeroVec);

            // E. Override BOTH arm lengths so PalShooterSpringArmComponent's tick doesn't pull it in
            Utils::SetPropertyValue<float>(CameraBoomObj, STR("WalkCameraArmLength"), optimalDistance);
            Utils::SetPropertyValue<float>(CameraBoomObj, STR("TargetArmLength"), optimalDistance);

            Utils::SetPropertyValue<bool>(CameraBoomObj, STR("bUsePawnControlRotation"), false);
            Utils::SetPropertyValue<bool>(CameraBoomObj, STR("bEnableCameraLag"), bEnableCameraSmoothing);
            Utils::SetPropertyValue<bool>(CameraBoomObj, STR("bEnableCameraRotationLag"), bEnableCameraSmoothing);

            UpdatePalCameraRotation(SaveManager::Get().Settings.CameraRotation, true);
        }

        // 3. Blend view target
        UFunction* SetViewTargetFunc = CurrentPlayerController->GetFunctionByNameInChain(STR("SetViewTargetWithBlend"));
        if (SetViewTargetFunc) {
            alignas(8) uint8_t SetViewParams[128] = {0};
            FProperty* TargetProp = SetViewTargetFunc->GetPropertyByNameInChain(STR("NewViewTarget"));
            if (TargetProp) *TargetProp->ContainerPtrToValuePtr<UObject*>(SetViewParams) = TargetPal;
            Utils::SafeProcessEvent(CurrentPlayerController, SetViewTargetFunc, SetViewParams);
            bIsPalCameraActive = true;
        }
    }

    void UIManager::DisablePalCamera() {
        if (!bIsPalCameraActive || !CurrentPlayerController) return;

        if (OriginalViewTarget && !Utils::IsObjectValid(OriginalViewTarget)) {
            OriginalViewTarget = nullptr;
        }

        // Restore CameraBoom settings back to vanilla defaults
        UObject* CameraBoomObj = GetCameraBoom(TargetPal);
        if (CameraBoomObj && GCachedCamState.bValid) {
            Utils::SetPropertyValue<float>(CameraBoomObj, STR("WalkCameraArmLength"), GCachedCamState.WalkArmLength);
            Utils::SetPropertyValue<float>(CameraBoomObj, STR("TargetArmLength"), GCachedCamState.TargetArmLength);
            Utils::SetPropertyValue<FVector>(CameraBoomObj, STR("WalkCameraOffset"), GCachedCamState.WalkOffset);
            Utils::SetPropertyValue<FVector>(CameraBoomObj, STR("SocketOffset"), GCachedCamState.SocketOffset);
            Utils::SetPropertyValue<FVector>(CameraBoomObj, STR("TargetOffset"), GCachedCamState.TargetOffset);
            Utils::SetPropertyValue<bool>(CameraBoomObj, STR("bDoCollisionTest"), GCachedCamState.bCollisionTest);
            Utils::SetPropertyValue<float>(CameraBoomObj, STR("ProbeSize"), GCachedCamState.ProbeSize);

            UFunction* SetAbsFunc = CameraBoomObj->GetFunctionByNameInChain(STR("SetAbsolute"));
            if (SetAbsFunc) {
                struct { bool bNewAbsoluteLocation; bool bNewAbsoluteRotation; bool bNewAbsoluteScale; } AbsParams{ false, false, false };
                Utils::SafeProcessEvent(CameraBoomObj, SetAbsFunc, &AbsParams);
            }
            Utils::SetPropertyValue<bool>(CameraBoomObj, STR("bInheritYaw"), true);
            GCachedCamState.bValid = false;
        }

        UFunction* SetViewTargetFunc = CurrentPlayerController->GetFunctionByNameInChain(STR("SetViewTargetWithBlend"));
        if (SetViewTargetFunc) {
            alignas(8) uint8_t Params[128] = {0};
            FProperty* TargetProp = SetViewTargetFunc->GetPropertyByNameInChain(STR("NewViewTarget"));
            if (TargetProp) {
                *TargetProp->ContainerPtrToValuePtr<UObject*>(Params) = OriginalViewTarget ? OriginalViewTarget : CurrentPlayerController;
            }
            Utils::SafeProcessEvent(CurrentPlayerController, SetViewTargetFunc, Params);
        }

        bIsPalCameraActive = false;
    }

    void UIManager::UpdatePalCameraRotation(double Yaw, bool bTeleport) {
        UObject* CameraBoomObj = GetCameraBoom(TargetPal);
        if (CameraBoomObj && Utils::IsObjectValid(CameraBoomObj)) {
            bool bRelative = SaveManager::Get().Settings.bRelativeCamera;

            Utils::SetPropertyValue<bool>(CameraBoomObj, STR("bInheritYaw"), bRelative);

            UFunction* SetAbsFunc = CameraBoomObj->GetFunctionByNameInChain(STR("SetAbsolute"));
            if (SetAbsFunc) {
                struct { bool bNewAbsoluteLocation; bool bNewAbsoluteRotation; bool bNewAbsoluteScale; } AbsParams{ false, !bRelative, false };
                Utils::SafeProcessEvent(CameraBoomObj, SetAbsFunc, &AbsParams);
            }

            bool bEffectiveTeleport = bTeleport || !bEnableCameraSmoothing;
            FRotator_UE5 NewRot{ 0.0, Yaw, 0.0 };
            SetComponentRotationDirect(CameraBoomObj, NewRot, !bRelative, bEffectiveTeleport);
        }
    }

    void UIManager::UpdateTarget() {
        TargetPal = nullptr;
        TargetInstanceID = L"";
        TargetCharID = L"";

        if (!CurrentPlayerController) return;

        UObject* PlayerPawn = nullptr;
        Utils::CallFunction(CurrentPlayerController, STR("K2_GetPawn"), &PlayerPawn);
        if (!PlayerPawn) return;

        struct { FVector_UE5 Location; FRotator_UE5 Rotation; } ViewPointParams;
        Utils::CallFunction(CurrentPlayerController, STR("GetPlayerViewPoint"), &ViewPointParams);
        
        FVector_UE5 CameraLoc = ViewPointParams.Location;
        FRotator_UE5 CameraRot = ViewPointParams.Rotation;

        double PitchRad = CameraRot.Pitch * 0.01745329251; 
        double YawRad = CameraRot.Yaw * 0.01745329251;
        double CosPitch = std::cos(PitchRad);

        FVector_UE5 CameraForward{
            std::cos(YawRad) * CosPitch,
            std::sin(YawRad) * CosPitch,
            std::sin(PitchRad)
        };

        std::vector<UObject*> AllPals;
        UObjectGlobals::FindAllOf(STR("PalCharacter"), AllPals);

        UObject* aimedPal = nullptr;
        double highestDot = -1.0;
        UObject* closestPal = nullptr;
        double closestDistSq = 999999999.0;

        for (UObject* Pal : AllPals) {
            if (Pal == PlayerPawn || !Utils::IsObjectValid(Pal)) continue;

            bool bHidden = false;
            Utils::GetPropertyValue<bool>(Pal, STR("bHidden"), bHidden, true);
            bool bBeingDestroyed = false;
            Utils::GetPropertyValue<bool>(Pal, STR("bActorIsBeingDestroyed"), bBeingDestroyed, true);

            if (bHidden || bBeingDestroyed) continue;

            struct { FVector_UE5 RetVal; } PalLocParams;
            Utils::CallFunction(Pal, STR("K2_GetActorLocation"), &PalLocParams);
            FVector_UE5 PalLoc = PalLocParams.RetVal;

            FVector_UE5 Dir{ PalLoc.X - CameraLoc.X, PalLoc.Y - CameraLoc.Y, PalLoc.Z - CameraLoc.Z };
            double distSq = (Dir.X * Dir.X) + (Dir.Y * Dir.Y) + (Dir.Z * Dir.Z);
            double dist = std::sqrt(distSq);

            if (dist > 5000.0) continue;

            FVector_UE5 DirNorm{ Dir.X / dist, Dir.Y / dist, Dir.Z / dist };
            double dot = CameraForward.X * DirNorm.X + CameraForward.Y * DirNorm.Y + CameraForward.Z * DirNorm.Z;

            if (dot >= 0.80 && dot > highestDot) {
                highestDot = dot;
                aimedPal = Pal;
            }

            if (distSq < closestDistSq) {
                closestDistSq = distSq;
                closestPal = Pal;
            }
        }

        TargetPal = aimedPal ? aimedPal : closestPal;

        if (TargetPal) {
            UObject* ParamComp = nullptr;
            Utils::GetPropertyValue(TargetPal, STR("CharacterParameterComponent"), ParamComp);
            if (!ParamComp) return;

            UObject* IndivParam = nullptr;
            Utils::GetPropertyValue(ParamComp, STR("IndividualParameter"), IndivParam);
            if (!IndivParam) return;

            FPalInstanceID IDStruct;
            if (Utils::GetPropertyValue(IndivParam, STR("IndividualId"), IDStruct)) {
                TargetInstanceID = Utils::GuidToWString(IDStruct.InstanceId);
            }

            UObject* PalUtil = UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, STR("/Script/Pal.Default__PalUtility"));
            struct { UObject* Char; FName RetVal; } CharIDParams{TargetPal, FName()};
            if (PalUtil) Utils::CallFunction(PalUtil, STR("GetCharacterIDFromCharacter"), &CharIDParams);
            TargetCharID = PalProcessor::Get().StripCharacterPrefix(CharIDParams.RetVal.ToString());
        }
    }


    void UIManager::ApplySize(double NewValue) {
        LastObservedSize = NewValue;

        PalPersistData* p = SaveManager::Get().GetPersistData(TargetInstanceID);
        if (p) {
            p->SizeMultiplier = NewValue;
            SaveManager::Get().SetPersistData(TargetInstanceID, *p, false); // <--- Pass false (memory only)

            if (TargetPal && Utils::IsObjectValid(TargetPal)) {
                UObject* MeshComp = nullptr;
                Utils::CallFunction(TargetPal, STR("GetMainMesh"), &MeshComp);
                if (MeshComp && Utils::IsObjectValid(MeshComp)) {
                    UClass* CharClass = TargetPal->GetClassPrivate();
                    if (CharClass) {
                        UObject* CDO = CharClass->GetClassDefaultObject();
                        UObject* VanillaMesh = nullptr;
                        if (CDO) Utils::GetPropertyValue<UObject*>(CDO, STR("Mesh"), VanillaMesh);
                        
                        FVector_UE5 BaseScale{ 1.0, 1.0, 1.0 };
                        if (VanillaMesh) {
                            FVector_UE5 CDOScale{ 1.0, 1.0, 1.0 };
                            if (Utils::GetPropertyValue<FVector_UE5>(VanillaMesh, STR("RelativeScale3D"), CDOScale)) {
                                if (CDOScale.X > 0.001 && CDOScale.Y > 0.001 && CDOScale.Z > 0.001) BaseScale = CDOScale;
                            }
                        }

                        if (NewValue <= 0.001) NewValue = 1.0;
                        FVector_UE5 FinalMeshScale{ BaseScale.X * NewValue, BaseScale.Y * NewValue, BaseScale.Z * NewValue };

                        Utils::SetPropertyValue<FVector_UE5>(MeshComp, STR("DefaultScale3D"), FinalMeshScale);
                        struct { FVector_UE5 NewScale3D; } ScaleParams{ FinalMeshScale };
                        Utils::CallFunction(MeshComp, STR("SetRelativeScale3D"), &ScaleParams);
                    }
                }
            }
        }
    }

    void UIManager::ApplyMorph(const std::wstring& MorphName, double NewValue) {
        PalPersistData* p = SaveManager::Get().GetPersistData(TargetInstanceID);
        if (p) {
            p->MorphSet[MorphName] = NewValue;
            SaveManager::Get().SetPersistData(TargetInstanceID, *p, false); // <--- Pass false (memory only)
            
            UObject* MeshComp = nullptr;
            Utils::CallFunction(TargetPal, STR("GetMainMesh"), &MeshComp);
            if (MeshComp) {
                struct { FName MorphTargetName; float Value; bool bRemoveZeroWeight; } MorphParams{
                    FName(MorphName.c_str(), FNAME_Add), static_cast<float>(NewValue), false
                };
                Utils::CallFunction(MeshComp, STR("SetMorphTarget"), &MorphParams);
            }
        }
    }

    void UIManager::Attach(PalUI::Client::Connection* InUI) {
        UI = InUI;
    }

    void UIManager::Detach() {
        if (S->Window) Close();
        S->Choice.reset();
        S->Choices.clear();
        UI = nullptr;
    }

    bool UIManager::ShowChoice(const std::wstring& Message, std::function<void(bool)> OnResult) {
        if (!UI) return false;
        S->Choices.emplace_back(Message, std::move(OnResult));
        if (!S->Choice || !S->Choice->IsPending()) ShowNextChoice();
        return true;
    }

    // The next dialog waits for Tick, since a dialog cannot go while its own handler runs.
    void UIManager::ShowNextChoice() {
        if (!UI || S->Choices.empty()) return;
        auto [Message, OnResult] = std::move(S->Choices.front());
        S->Choices.pop_front();
        S->Choice.emplace(*UI);
        S->Choice->Show(Utils::WStringToString(Message), PalUI::Client::DialogKind::OkCancel, std::move(OnResult));
    }

    void UIManager::OnSettingsReloaded() {
        if (!S->Window) return;
        const auto& Settings = SaveManager::Get().Settings;
        if (Settings.bFocusPal && !bIsPalCameraActive) EnablePalCamera();
        else if (!Settings.bFocusPal && bIsPalCameraActive) DisablePalCamera();
        else if (bIsPalCameraActive) UpdatePalCameraRotation(Settings.CameraRotation);
        bNeedsRefresh = true;
        bPrefsStale = true;
    }

    bool UIManager::IsMenuOpen() const {
        return S->Window && S->Window->IsOpen();
    }

    void UIManager::WarnMissing() {
        if (bWarnedMissing) return;
        bWarnedMissing = true;
        NotificationManager::Get().EnqueueToast(L"The DynamicPals menu needs the PalUI mod", EPalLogPriority::Important, EPalLogContentToneType::Negative);
    }

    PalUI::Client::Row UIManager::Built(PalUI::Client::Row Row, const char* Kind) {
        if (!Row.IsValid() && LoggedKinds.insert(Kind).second) {
            DP_LOG(Warning, "PalUI could not build a {} row; the installed PalUI may be older than this build expects.", Utils::StringToWString(Kind));
        }
        return Row;
    }

    void UIManager::Toggle(RC::Unreal::UObject* PlayerController) {
        if (!UI) {
            WarnMissing();
            return;
        }
        if (IsMenuOpen()) {
            Close();
            return;
        }
        CurrentPlayerController = PlayerController;
        Open();
    }

    bool UIManager::Open() {
        UpdateTarget();
        
        if (!CurrentPlayerController) {
            EnqueueUIToast(L"Player controller not found. Cannot switch camera.", 2, 1);
            return false;
        }

        if (!TargetPal) {
            EnqueueUIToast(L"No valid Pal found in range!", 2, 1);
            return false; 
        }

        S->Window.emplace(*UI);
        S->Window->SetSize(650.0, 760.0);
        if (!S->Window->Open()) {
            S->Window.reset();
            DP_LOG(Warning, "PalUI did not open the menu window; is the PalUI mod enabled?");
            WarnMissing();
            return false;
        }

        RC::Unreal::UFunction* GetViewTargetFunc = CurrentPlayerController->GetFunctionByNameInChain(STR("GetViewTarget")); 
        if (GetViewTargetFunc) {
            alignas(8) uint8_t Params[32] = {0};
            Utils::SafeProcessEvent(CurrentPlayerController, GetViewTargetFunc, Params);
            FProperty* RetProp = GetViewTargetFunc->GetPropertyByNameInChain(STR("ReturnValue"));
            if (RetProp) OriginalViewTarget = *RetProp->ContainerPtrToValuePtr<UObject*>(Params);
        }

        if (!OriginalViewTarget) {
            struct { RC::Unreal::UObject* ReturnValue; } GetPawnParams{nullptr};
            Utils::CallFunction(CurrentPlayerController, STR("K2_GetPawn"), &GetPawnParams, true);
            OriginalViewTarget = GetPawnParams.ReturnValue;
        }

        if (SaveManager::Get().Settings.bFocusPal) {
            EnablePalCamera();
        }

        Fill();
        bWasOpen = true;
        return true;
    }

    void UIManager::Close() {
        if (S->Window) S->Window->Close();
        OnClosed();
    }

    void UIManager::OnClosed() {
        if (InputManager::Get().IsCapturing()) {
            InputManager::Get().CancelCapture();
        }
        FlushPendingSave();

        DisablePalCamera();
        OriginalViewTarget = nullptr;
        bIsPalCameraActive = false;

        TargetPal = nullptr;
        TargetInstanceID = L"";
        TargetCharID = L"";
        bNeedsRefresh = false;
        bWasOpen = false;
        LastObservedSize = -999.0;
        LastObservedLabel = L"";

        S->CaptureRow = {};
        S->PackRows.clear();
        S->PackConfigs.clear();
        S->Settings = {};
        S->Prefs = {};
        S->bCloseRequested = false;
        S->Window.reset();
    }

    bool UIManager::OnUObjectDeleted(RC::Unreal::UObject* Obj) {
        bool bFound = false;
        if (TargetPal == Obj) { TargetPal = nullptr; S->bCloseRequested = true; bFound = true; }
        if (OriginalViewTarget == Obj) { OriginalViewTarget = nullptr; bFound = true; }
        if (CurrentPlayerController == Obj) { CurrentPlayerController = nullptr; bFound = true; }
        return bFound;
    }

    void UIManager::CloseForUnload() {
        if (S->Window) Close();
        CurrentPlayerController = nullptr;
    }

    bool UIManager::IsTargetLocked() {
        PalPersistData* Persist = SaveManager::Get().GetPersistData(TargetInstanceID);
        return Persist && Persist->bIsManuallyLocked;
    }

    void UIManager::Fill() {
        S->Settings = S->Window->AddTab("Settings");
        S->Prefs = S->Window->AddTab("Preferences");
        FillSettings();
        FillPreferences();

        // Escape cancels a key capture first; the close itself waits for Tick,
        // since the window cannot go while its own handler runs.
        S->Window->SetCancel([this]() {
            if (InputManager::Get().IsCapturing()) return;
            if (std::chrono::steady_clock::now() - CaptureEndedAt < std::chrono::milliseconds(250)) return;
            S->bCloseRequested = true;
        });

        S->bShownLocked = IsTargetLocked();
        S->Window->SetTitleBar(Utils::WStringToString(L"DYN PALS: " + TargetCharID), S->bShownLocked ? "LOCKED" : "");
        S->Window->SelectTab(0);
    }

    std::wstring UIManager::FormatKeybindText(const std::wstring& Modifier, const std::wstring& Key) {
        if (Key.empty() || Key == L"None") {
            return L"[ Unset ]";
        }
        std::wstring result = L"[ ";
        if (!Modifier.empty() && Modifier != L"None") result += Modifier + L" + ";
        result += Key + L" ]";
        return result;
    }

    void UIManager::FillPreferences() {
        auto& Settings = SaveManager::Get().Settings;
        Built(S->Prefs.AddHeader("Global Keybinds"), "Header");
        Built(S->Prefs.AddLabelWithButton("Main Menu Toggle", Utils::WStringToString(FormatKeybindText(Settings.MenuModifier, Settings.MenuKey)),
                                    [this]() { StartKeyCapture(0); }), "LabelWithButton");
        Built(S->Prefs.AddLabelWithButton("Direct Reroll Pal", Utils::WStringToString(FormatKeybindText(Settings.RerollModifier, Settings.RerollKey)),
                                    [this]() { StartKeyCapture(2); }), "LabelWithButton");
        S->CaptureRow = Built(S->Prefs.AddLabel(""), "Label");
    }

    void UIManager::StartKeyCapture(int Target) {
        if (InputManager::Get().IsCapturing()) return;
        S->CaptureRow.SetText("Press any key or button... (Esc cancels)");

        InputManager::Get().StartCapture(
            [this, Target](const std::wstring& Key, const std::wstring& Mod) {
                auto& Settings = SaveManager::Get().Settings;
                if (Target == 0) {
                    Settings.MenuKey = Key;
                    Settings.MenuModifier = Mod;
                } else if (Target == 2) {
                    Settings.RerollKey = Key;
                    Settings.RerollModifier = Mod;
                }
                Settings.CacheKeybinds();
                SaveManager::Get().SaveGlobal();
                CaptureEndedAt = std::chrono::steady_clock::now();

                // The bound key shows on the row's button, which takes no new text, so the page is filled again.
                if (S->Prefs.IsValid() && S->Prefs.Clear()) FillPreferences();
            },
            [this]() {
                CaptureEndedAt = std::chrono::steady_clock::now();
                S->CaptureRow.SetText("");
            }
        );
    }

    void UIManager::PreloadUI(RC::Unreal::UObject* PC) {
        CurrentPlayerController = PC;
        Utils::LoadAssetSafely(L"/Game/Pal/Effect/Common/LevelUp/NS_LevelUp_Pal");
    }

    void UIManager::FillSettings() {
        S->PackRows.clear();
        S->PackConfigs.clear();
        if (!S->Settings.IsValid() || !TargetPal) return;

        PalPersistData* currentPersist = SaveManager::Get().GetPersistData(TargetInstanceID);

        // 2. Fetch Pal Statistics
        bool IsRare = false, IsWild = false;
        std::wstring GenderStr = L"None", SkinName = L"";
        int LevelNum = 1, RankNum = 0, FriendshipNum = 0;
        std::vector<std::wstring> Traits;

        UObject* ParamComp = nullptr;
        Utils::GetPropertyValue(TargetPal, STR("CharacterParameterComponent"), ParamComp);
        if (ParamComp) {
            UObject* IndivParam = nullptr;
            Utils::GetPropertyValue(ParamComp, STR("IndividualParameter"), IndivParam);
            if (IndivParam) {
                struct { bool RetVal; } RareParams{false};
                Utils::CallFunction(IndivParam, STR("IsRarePal"), &RareParams);
                IsRare = RareParams.RetVal;

                struct { uint8_t RetVal; } GenderParams{0};
                Utils::CallFunction(IndivParam, STR("GetGenderType"), &GenderParams);
                GenderStr = (GenderParams.RetVal == 1) ? L"Male" : ((GenderParams.RetVal == 2) ? L"Female" : L"None");

                struct { int32_t RetVal = -1; } LevelParams;
                Utils::CallFunction(IndivParam, STR("GetLevel"), &LevelParams);
                LevelNum = LevelParams.RetVal == -1 ? 1 : LevelParams.RetVal;

                struct { int32_t RetVal = -1; } RankParams;
                Utils::CallFunction(IndivParam, STR("GetRank"), &RankParams);
                RankNum = RankParams.RetVal == -1 ? 0 : RankParams.RetVal;

                struct { int32_t RetVal = -1; } FriendshipParams;
                Utils::CallFunction(IndivParam, STR("GetFriendshipRank"), &FriendshipParams);
                FriendshipNum = FriendshipParams.RetVal;
                if (FriendshipNum == -1) {
                    struct { int32_t RetVal = -1; } LegacyFriendshipParams;
                    Utils::CallFunction(IndivParam, STR("GetFriendshipPoint"), &LegacyFriendshipParams);
                    FriendshipNum = LegacyFriendshipParams.RetVal == -1 ? 0 : LegacyFriendshipParams.RetVal;
                }

                struct { FName RetVal; } SkinParams{FName()};
                Utils::CallFunction(IndivParam, STR("GetSkinName"), &SkinParams);
                SkinName = SkinParams.RetVal.ToString();
                if (SkinName == L"None") SkinName = L"";

                // Strip synthetic dynamic ID so the UI matchmaker evaluates against the real skin
                if (SwapIdentity::IsDynamicSkin(SkinName)) {
                    SkinName = (currentPersist && !currentPersist->OriginalSkinName.empty()) ? currentPersist->OriginalSkinName : L"";
                }

                struct { TArray<FName> RetVal; } TraitsParams;
                Utils::CallFunction(IndivParam, STR("GetPassiveSkillList"), &TraitsParams);
                for (int32_t i = 0; i < TraitsParams.RetVal.Num(); ++i) {
                    Traits.push_back(TraitsParams.RetVal[i].ToString());
                }
            }
        }

        UObject* PalUtil = UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, STR("/Script/Pal.Default__PalUtility"));
        if (PalUtil) {
            struct { UObject* Actor; bool RetVal; } WildParams{TargetPal, false};
            Utils::CallFunction(PalUtil, STR("IsWildNPC"), &WildParams);
            IsWild = WildParams.RetVal;
        }

        std::wstring CurrentSwapLabel = currentPersist ? currentPersist->SwapLabel : L"";
        auto evaluations = ConfigManager::Get().EvaluateAllSwaps(TargetCharID, IsRare, GenderStr, Traits, LevelNum, SkinName, RankNum, FriendshipNum, IsWild, CurrentSwapLabel);

        int bestScore = 999999;
        for (const auto& eval : evaluations) {
            if (eval.IsValid && eval.Score < bestScore) bestScore = eval.Score;
        }

        double totalTiedWeight = 0.0;
        for (const auto& eval : evaluations) {
            if (eval.IsValid && eval.Score == bestScore) {
                double w = ConfigManager::Get().GetConfigs()[eval.ConfigIndex].SpawnWeight;
                if (w > 0.0) totalTiedWeight += w;
            }
        }

        int persistConfigIndex = currentPersist ? ConfigManager::Get().FindConfigIndex(currentPersist->PackName, currentPersist->SkinName, currentPersist->SwapLabel, currentPersist->SkelMeshPath, TargetCharID) : -1;

        std::map<std::wstring, std::vector<SwapEvaluation>> rawPacks;
        for (const auto& eval : evaluations) {
            if (bHideInvalidSwaps && !eval.IsValid && eval.ConfigIndex != persistConfigIndex) {
                continue;
            }
            rawPacks[ConfigManager::Get().GetConfigs()[eval.ConfigIndex].PackName].push_back(eval);
        }

        // 3. Consolidated Dropdown Reconstruction (Single Pre-Parse Pass)
        std::vector<std::wstring> newDropdownOptions;
        std::vector<int> newDropdownConfigIndices;

        struct FParsedDropdownItem {
            int ConfigIndex;
            std::wstring CleanLabel;
            std::vector<std::wstring> Tokens;
        };

        for (auto& [packName, evals] : rawPacks) {
            newDropdownOptions.push_back(L"[ " + packName + L" ]");
            newDropdownConfigIndices.push_back(-1); 

            std::map<std::wstring, int> prefixCounts;
            std::vector<FParsedDropdownItem> parsedItems;
            parsedItems.reserve(evals.size());

            // Single Pre-parsing Pass: Extracts label, strips prefixes, tokenizes once
            for (auto& eval : evals) {
                auto& cfg = ConfigManager::Get().GetConfigs()[eval.ConfigIndex];
                std::wstring label = StripFallbackHash(cfg.SwapLabel);
                if (label.empty()) label = cfg.SkinName;
                if (label.empty()) {
                    label = cfg.SkelMeshPath;
                    size_t slash = label.find_last_of(L'/');
                    if (slash != std::wstring::npos) label = label.substr(slash + 1);
                    size_t dot = label.find(L'.');
                    if (dot != std::wstring::npos) label = label.substr(0, dot);
                }

                FParsedDropdownItem item{ eval.ConfigIndex, label, {} };

                if (label.rfind(L"SK_", 0) == 0 || label.rfind(L"sk_", 0) == 0 || label.find(L'_') != std::wstring::npos) {
                    std::wstring clean = label;
                    if (clean.rfind(L"SK_", 0) == 0 || clean.rfind(L"sk_", 0) == 0) clean = clean.substr(3);

                    std::wstring lowerClean = clean;
                    std::transform(lowerClean.begin(), lowerClean.end(), lowerClean.begin(), ::towlower);
                    std::wstring lowerCharID = TargetCharID;
                    std::transform(lowerCharID.begin(), lowerCharID.end(), lowerCharID.begin(), ::towlower);

                    if (lowerClean.rfind(lowerCharID + L"_", 0) == 0 && clean.length() >= TargetCharID.length() + 1) {
                        clean = clean.substr(TargetCharID.length() + 1);
                    } else if (lowerClean.rfind(lowerCharID, 0) == 0 && clean.length() >= TargetCharID.length()) {
                        clean = clean.substr(TargetCharID.length());
                        if (!clean.empty() && clean[0] == L'_') clean = clean.substr(1);
                    }
                    if (clean.empty()) clean = L"(Vanilla Mesh)";

                    size_t start = 0, end = 0;
                    while ((end = clean.find(L'_', start)) != std::wstring::npos) {
                        if (end != start) item.Tokens.push_back(clean.substr(start, end - start));
                        start = end + 1;
                    }
                    if (start < clean.length()) item.Tokens.push_back(clean.substr(start));

                    item.CleanLabel = clean;
                    if (item.Tokens.size() >= 3) prefixCounts[item.Tokens[0] + L"_" + item.Tokens[1]]++;
                    if (item.Tokens.size() >= 2) prefixCounts[item.Tokens[0]]++;
                }

                parsedItems.push_back(std::move(item));
            }

            // Assembly Pass: Generates clean display strings from cached tokens
            for (const auto& item : parsedItems) {
                std::wstring display = item.CleanLabel;

                if (!item.Tokens.empty()) {
                    std::wstring bestPrefix = L"";
                    int prefixTokens = 0;
                    if (item.Tokens.size() >= 3 && prefixCounts[item.Tokens[0] + L"_" + item.Tokens[1]] >= 2) {
                        bestPrefix = item.Tokens[0] + L"_" + item.Tokens[1];
                        prefixTokens = 2;
                    } else if (item.Tokens.size() >= 2 && prefixCounts[item.Tokens[0]] >= 2) {
                        bestPrefix = item.Tokens[0];
                        prefixTokens = 1;
                    }

                    if (!bestPrefix.empty()) {
                        display = L"";
                        for (size_t i = prefixTokens; i < item.Tokens.size(); ++i) {
                            display += item.Tokens[i] + (i < item.Tokens.size() - 1 ? L" " : L"");
                        }
                    } else {
                        std::replace(display.begin(), display.end(), L'_', L' ');
                    }

                    // CamelCase spacing
                    std::wstring splitDisplay = L"";
                    if (!display.empty()) {
                        splitDisplay.push_back(display[0]);
                        for (size_t i = 1; i < display.size(); ++i) {
                            if ((display[i - 1] >= L'a' && display[i - 1] <= L'z') && (display[i] >= L'A' && display[i] <= L'Z')) {
                                splitDisplay.push_back(L' ');
                            }
                            splitDisplay.push_back(display[i]);
                        }
                        display = splitDisplay;
                    }
                }

                newDropdownOptions.push_back(L"   " + display);
                newDropdownConfigIndices.push_back(item.ConfigIndex);
            }
        }

        if (newDropdownOptions.empty()) {
            newDropdownOptions.push_back(L"   (No Skins Available)");
            newDropdownConfigIndices.push_back(-1);
        }


        // 3. Swap picker: one choice row per pack.
        Built(S->Settings.AddHeader("Current Swap"), "Header");
        std::vector<std::string> Labels;
        std::vector<int> Configs;
        std::wstring PackName;
        const auto EndPack = [&]() {
            if (Labels.empty()) return;
            std::size_t Selected = PALUI_NO_INDEX;
            for (std::size_t i = 0; i < Configs.size(); ++i) {
                if (Configs[i] == persistConfigIndex) Selected = i;
            }
            const std::size_t Pack = S->PackRows.size();
            Built(S->Settings.AddHeader(Utils::WStringToString(PackName)), "Header");
            S->PackRows.push_back(Built(S->Settings.AddChoices(Labels, Selected, [this, Pack](std::size_t Index) {
                if (Pack >= S->PackConfigs.size() || Index >= S->PackConfigs[Pack].size()) return;
                PalProcessor::Get().ForceSwap(TargetPal, S->PackConfigs[Pack][Index]);
                for (std::size_t Other = 0; Other < S->PackRows.size(); ++Other) {
                    if (Other != Pack) S->PackRows[Other].SetIndex(std::nullopt);
                }
                bNeedsRefresh = true;
            }), "Choices"));
            S->PackConfigs.push_back(Configs);
            Labels.clear();
            Configs.clear();
        };
        for (std::size_t i = 0; i < newDropdownOptions.size(); ++i) {
            const std::wstring& Option = newDropdownOptions[i];
            if (newDropdownConfigIndices[i] == -1) {
                EndPack();
                if (Option.rfind(L"[ ", 0) == 0) {
                    PackName = Option.substr(2, Option.size() >= 4 ? Option.size() - 4 : 0);
                } else {
                    Built(S->Settings.AddLabel(Utils::WStringToString(Option.substr(Option.find_first_not_of(L' ')))), "Label");
                }
                continue;
            }
            Labels.push_back(Utils::WStringToString(Option.substr(Option.find_first_not_of(L' '))));
            Configs.push_back(newDropdownConfigIndices[i]);
        }
        EndPack();

        Built(S->Settings.AddButton("Reroll Pal", [this]() { PalProcessor::Get().ProcessPal(TargetPal, true); bNeedsRefresh = true; }), "Button");
        Built(S->Settings.AddButton("Reset Pal", [this]() { if (TargetPal && Utils::IsObjectValid(TargetPal)) { PalProcessor::Get().ResetPal(TargetPal); bNeedsRefresh = true; } }), "Button");
        Built(S->Settings.AddSwitch("Hide Invalid", bHideInvalidSwaps, [this](bool bState) { bHideInvalidSwaps = bState; bNeedsRefresh = true; }), "Switch");
        Built(S->Settings.AddSwitch("Focus Pal Camera", SaveManager::Get().Settings.bFocusPal, [this](bool bState) {
            SaveManager::Get().Settings.bFocusPal = bState; SaveManager::Get().SaveWorldData();
            if (bState) EnablePalCamera(); else DisablePalCamera();
            bNeedsRefresh = true; 
        }), "Switch");

        // 5. Camera Controls (Relative Switch + Rotation Slider)
        if (SaveManager::Get().Settings.bFocusPal) {
            Built(S->Settings.AddSwitch("Relative Camera", SaveManager::Get().Settings.bRelativeCamera, [this](bool bState) {
                SaveManager::Get().Settings.bRelativeCamera = bState;
                SaveManager::Get().SaveWorldData();
                UpdatePalCameraRotation(SaveManager::Get().Settings.CameraRotation);
            }), "Switch");
            Built(S->Settings.AddSlider("Camera Rotation", SaveManager::Get().Settings.CameraRotation, 0.0, 360.0, [this](double NewValue) {
                SaveManager::Get().Settings.CameraRotation = NewValue;
                UpdatePalCameraRotation(NewValue);
                TriggerSaveDebounced(); // <--- Debounce disk write!
            }), "Slider");
        }

        // 6. Dynamic Size Slider
        if (currentPersist && persistConfigIndex != -1) {
            auto& activeCfg = ConfigManager::Get().GetConfigs()[persistConfigIndex];
            if (activeCfg.MinSizeMultiplier < activeCfg.MaxSizeMultiplier) {
                double currentVal = currentPersist->SizeMultiplier > 0.0 ? currentPersist->SizeMultiplier : 1.0;
                currentVal = std::clamp(currentVal, activeCfg.MinSizeMultiplier, activeCfg.MaxSizeMultiplier);
                Built(S->Settings.AddSlider("Size Adjustment", currentVal, activeCfg.MinSizeMultiplier, activeCfg.MaxSizeMultiplier, [this](double NewValue) {
                    ApplySize(NewValue);
                    TriggerSaveDebounced(); // <--- Debounce disk write!
                }), "Slider");
            }
        }

        // 7. Shape Keys / Morph Sliders
        if (currentPersist && persistConfigIndex != -1) {
            auto& activeCfg = ConfigManager::Get().GetConfigs()[persistConfigIndex];
            if (!activeCfg.MorphTargetList.empty()) {
                Built(S->Settings.AddHeader("Morph Targets"), "Header");
                for (auto& morph : activeCfg.MorphTargetList) {
                    if (morph.type != L"Restrict" && morph.minVal < morph.maxVal) {
                        const double currentVal = currentPersist->MorphSet[morph.target];
                        Built(S->Settings.AddSlider(Utils::WStringToString(morph.target), currentVal, morph.minVal, morph.maxVal, [this, MorphName = morph.target](double NewValue) {
                            ApplyMorph(MorphName, NewValue);
                            TriggerSaveDebounced(); // <--- Debounce disk write!
                        }), "Slider");
                    }
                }
            }
        }

        // 8. Dynamic Log Output
        if (currentPersist && currentPersist->SizeMultiplier > 0.0 && std::abs(currentPersist->SizeMultiplier - 1.0) > 0.005) {
            wchar_t sizeBuf[32];
            swprintf(sizeBuf, 32, L"Size Modifier: %.2fx", currentPersist->SizeMultiplier);
            Built(S->Settings.AddLabel(Utils::WStringToString(sizeBuf)), "Label");
        }
        
        Built(S->Settings.AddHeader("Matchmaker Log"), "Header");

        if (evaluations.empty()) {
            Built(S->Settings.AddLabel(Utils::WStringToString(L"No swaps configured for this Pal.")), "Label");
        } else {
            for (const auto& eval : evaluations) {
                if (bHideInvalidSwaps && !eval.IsValid) continue;

                auto& cfg = ConfigManager::Get().GetConfigs()[eval.ConfigIndex];
                Built(S->Settings.AddLabel(Utils::WStringToString(cfg.PackName)), "Label");

                // --- 1. Prioritize SwapLabel ---
                std::wstring displayLabel = StripFallbackHash(cfg.SwapLabel);
                
                // --- 2. Fallback to SkinName ---
                if (displayLabel.empty()) {
                    displayLabel = cfg.SkinName;
                }
                
                // --- 3. Fallback to Skeletal Mesh Filename ---
                if (displayLabel.empty() && !cfg.SkelMeshPath.empty()) {
                    std::wstring meshName = cfg.SkelMeshPath;
                    size_t slash = meshName.find_last_of(L'/');
                    if (slash != std::wstring::npos) meshName = meshName.substr(slash + 1);
                    size_t dot = meshName.find(L'.');
                    if (dot != std::wstring::npos) meshName = meshName.substr(0, dot);
                    
                    if (meshName.rfind(L"SK_", 0) == 0 || meshName.rfind(L"sk_", 0) == 0) meshName = meshName.substr(3);
                    for (wchar_t& c : meshName) { if (c == L'_') c = L' '; }
                    displayLabel = meshName;
                }
                
                // --- 4. Final Absolute Fallback ---
                if (displayLabel.empty()) {
                    displayLabel = L"(Unlabeled)";
                }

                double pct = 0.0;
                if (eval.IsValid && eval.Score == bestScore && totalTiedWeight > 0.0 && cfg.SpawnWeight > 0.0) {
                    pct = (cfg.SpawnWeight * 100.0) / totalTiedWeight;
                }

                std::wstring logStr;
                if (eval.IsValid) {
                    wchar_t pctBuf[16];
                    swprintf(pctBuf, 16, L"%.1f", pct);
                    logStr = L"    " + std::wstring(pctBuf) + L"% : " + displayLabel;
                } else {
                    logStr = L"    [X] " + displayLabel;
                    if (!eval.RejectionReason.empty()) {
                        logStr += L" (" + eval.RejectionReason + L")";
                    }
                }
                Built(S->Settings.AddLabel(Utils::WStringToString(logStr)), "Label");
            }
        }

    }

    void UIManager::Tick() {
        if (!S->Choices.empty() && (!S->Choice || !S->Choice->IsPending())) ShowNextChoice();
        if (!S->Window) return;

        if (bWasOpen && !S->Window->IsOpen()) {
            OnClosed();
            return;
        }
        if (S->bCloseRequested || !TargetPal || !Utils::IsObjectValid(TargetPal)) {
            Close();
            return;
        }

        // 1. Tick the standalone InputManager if in capture mode
        if (InputManager::Get().IsCapturing()) {
            InputManager::Get().Tick(CurrentPlayerController);
            return;
        }

        // 2. Debounced save
        if (bPendingDiskSave) {
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - LastSliderChangeTime).count();
            if (elapsed >= 500) { 
                bPendingDiskSave = false;
                SaveManager::Get().SaveWorldData();
            }
        }

        // 3. Auto-detect background size updates on Pal
        PalPersistData* p = SaveManager::Get().GetPersistData(TargetInstanceID);
        if (p) {
            if (p->SizeMultiplier != LastObservedSize || p->SwapLabel != LastObservedLabel) {
                LastObservedSize = p->SizeMultiplier;
                LastObservedLabel = p->SwapLabel;
                bNeedsRefresh = true;
            }
        }

        if (bPrefsStale) {
            bPrefsStale = false;
            if (S->Prefs.IsValid() && S->Prefs.Clear()) FillPreferences();
        }

        if (bNeedsRefresh) {
            bNeedsRefresh = false;
            if (IsTargetLocked() == S->bShownLocked && S->Settings.Clear()) {
                FillSettings();
            } else if (S->Window->Reset()) {
                Fill();
            } else {
                DP_LOG(Warning, "PalUI could not clear the menu for a refresh; reopen it to see the change.");
            }
        }
    }
}
