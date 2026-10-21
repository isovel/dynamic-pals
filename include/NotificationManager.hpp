#pragma once
#include <string>
#include <vector>
#include <functional>
#include <Unreal/UObjectGlobals.hpp>
#include "DataTypes.hpp"

namespace DynPals {

    class NotificationManager {
    public:
        static NotificationManager& Get() {
            static NotificationManager instance;
            return instance;
        }

        void EnqueueToast(const std::wstring& Message, EPalLogPriority Priority = EPalLogPriority::Normal, EPalLogContentToneType Tone = EPalLogContentToneType::Normal);
        void ClearInGameLogs();
        void ShowModalDialog(const std::wstring& Message);

        // Native 2-button hijacked modal dialog (Yes/No style with custom labels & actions)
        void ShowTwoButtonModal(
            const std::wstring& Message,
            const std::wstring& RightBtnText, std::function<void()> OnRightClick,
            const std::wstring& LeftBtnText = L"Cancel", std::function<void()> OnLeftClick = nullptr
        );

    private:
        NotificationManager() = default;
        NotificationManager(const NotificationManager&) = delete;
        NotificationManager& operator=(const NotificationManager&) = delete;
    };
}