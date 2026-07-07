#include "KeyRepeatAssist.h"

#include <windows.h>
#include <atomic>
#include <cstdlib>

namespace KeyRepeatAssist
{
    enum AssistGateState
    {
        AssistGateRunning = 0,
        AssistGatePaused = 1
    };

    struct KeyboardTimingConfig
    {
        DWORD delayMs;
        DWORD repeatPeriodMs;
        int rawDelay;
        int rawSpeed;
    };

    struct RepeatKeyProfile
    {
        DWORD vkCode;
        WORD scanCode;
        DWORD inputFlags;
    };

    struct RepeatSnapshot
    {
        ULONGLONG keyEpoch;
        ULONGLONG inputEpoch;
        ULONGLONG gateEpoch;
        HWND startWindow;
    };

    static const DWORD kVirtualUpDelayMs = 10;
    static const DWORD kVirtualUpRetryMs = 10;

    static const wchar_t* kSingleInstanceMutexName =
        L"Local\\KeyRepeatAssist_SingleInstance";

    static std::atomic<LONG> g_gateState(AssistGateRunning);
    static std::atomic<ULONGLONG> g_gateEpoch(0);
    static std::atomic<ULONGLONG> g_inputEpoch(0);

    static const ULONG_PTR kAssistExtraInfo = 0x53485250UL;

    static HHOOK g_keyboardHook = NULL;
    static HANDLE g_singleInstanceMutex = NULL;

    static KeyboardTimingConfig g_timing = {};

    class RepeatKeyWorker
    {
    public:
        RepeatKeyWorker()
            : m_threadHandle(NULL),
            m_wakeEvent(NULL),
            m_vkCode(0),
            m_scanCode(0),
            m_inputFlags(0),
            m_delayMs(500),
            m_periodMs(90),
            m_stopRequested(false),
            m_physicalDown(false),
            m_virtualDownActive(false),
            m_virtualUpPending(false),
            m_virtualUpDueTick(0),
            m_keyEpoch(0),
            m_startWindowValue(0)
        {
        }

        bool Start(const RepeatKeyProfile& profile, DWORD delayMs, DWORD periodMs)
        {
            m_vkCode = profile.vkCode;
            m_scanCode = profile.scanCode;
            m_inputFlags = profile.inputFlags;

            m_delayMs.store(delayMs, std::memory_order_release);
            m_periodMs.store(periodMs, std::memory_order_release);
            m_stopRequested.store(false, std::memory_order_release);
            m_physicalDown.store(false, std::memory_order_release);
            m_virtualDownActive.store(false, std::memory_order_release);
            m_virtualUpPending.store(false, std::memory_order_release);
            m_virtualUpDueTick.store(0, std::memory_order_release);
            m_keyEpoch.store(0, std::memory_order_release);
            m_startWindowValue.store(0, std::memory_order_release);

            m_wakeEvent = CreateEventW(NULL, FALSE, FALSE, NULL);
            if (m_wakeEvent == NULL)
            {
                return false;
            }

            m_threadHandle = CreateThread(
                NULL,
                0,
                &RepeatKeyWorker::ThreadProc,
                this,
                0,
                NULL);

            if (m_threadHandle == NULL)
            {
                CloseHandle(m_wakeEvent);
                m_wakeEvent = NULL;
                return false;
            }

            return true;
        }

        void Stop()
        {
            m_stopRequested.store(true, std::memory_order_release);
            Wake();

            if (m_threadHandle != NULL)
            {
                WaitForSingleObject(m_threadHandle, INFINITE);
                CloseHandle(m_threadHandle);
                m_threadHandle = NULL;
            }

            if (m_wakeEvent != NULL)
            {
                CloseHandle(m_wakeEvent);
                m_wakeEvent = NULL;
            }
        }

        DWORD GetVkCode() const
        {
            return m_vkCode;
        }

        void Wake()
        {
            if (m_wakeEvent != NULL)
            {
                SetEvent(m_wakeEvent);
            }
        }

        void NotifyPhysicalDown(HWND foregroundWindow)
        {
            m_startWindowValue.store(
                reinterpret_cast<ULONG_PTR>(foregroundWindow),
                std::memory_order_release);

            m_physicalDown.store(true, std::memory_order_release);
            m_keyEpoch.fetch_add(1, std::memory_order_acq_rel);

            Wake();
        }

        void NotifyPhysicalUp()
        {
            m_physicalDown.store(false, std::memory_order_release);
            m_startWindowValue.store(0, std::memory_order_release);
            m_keyEpoch.fetch_add(1, std::memory_order_acq_rel);

            if (m_virtualDownActive.load(std::memory_order_acquire))
            {
                ScheduleVirtualUp(kVirtualUpDelayMs);
            }

            Wake();
        }

    private:
        static DWORD WINAPI ThreadProc(LPVOID parameter)
        {
            RepeatKeyWorker* self = static_cast<RepeatKeyWorker*>(parameter);
            self->ThreadMain();
            return 0;
        }

        void ThreadMain()
        {
            while (!m_stopRequested.load(std::memory_order_acquire))
            {
                FlushVirtualUpIfDue();

                DWORD waitMs = GetThreadWaitMs();
                WaitForSingleObject(m_wakeEvent, waitMs);

                if (m_stopRequested.load(std::memory_order_acquire))
                {
                    break;
                }

                FlushVirtualUpIfDue();
                ProcessCurrentState();
            }

            if (m_virtualDownActive.load(std::memory_order_acquire))
            {
                SendSyntheticUp();
                m_virtualDownActive.store(false, std::memory_order_release);
                m_virtualUpPending.store(false, std::memory_order_release);
                m_virtualUpDueTick.store(0, std::memory_order_release);
            }
        }

        DWORD GetThreadWaitMs() const
        {
            if (!m_virtualUpPending.load(std::memory_order_acquire))
            {
                return INFINITE;
            }

            ULONGLONG nowTick = GetTickCount64();
            ULONGLONG dueTick =
                m_virtualUpDueTick.load(std::memory_order_acquire);

            if (nowTick >= dueTick)
            {
                return 0;
            }

            ULONGLONG delta = dueTick - nowTick;
            if (delta > 0x7FFFFFFFUL)
            {
                return 0x7FFFFFFF;
            }

            return static_cast<DWORD>(delta);
        }

        void ProcessCurrentState()
        {
            while (!m_stopRequested.load(std::memory_order_acquire))
            {
                FlushVirtualUpIfDue();

                if (m_virtualUpPending.load(std::memory_order_acquire))
                {
                    return;
                }

                if (!m_physicalDown.load(std::memory_order_acquire))
                {
                    return;
                }

                if (g_gateState.load(std::memory_order_acquire) != AssistGateRunning)
                {
                    WaitForSingleObject(m_wakeEvent, INFINITE);
                    continue;
                }

                RepeatSnapshot snapshot = CaptureSnapshot();
                DWORD delayMs = m_delayMs.load(std::memory_order_acquire);

                DWORD waitResult = WaitForSingleObject(m_wakeEvent, delayMs);

                if (m_stopRequested.load(std::memory_order_acquire))
                {
                    return;
                }

                FlushVirtualUpIfDue();

                if (m_virtualUpPending.load(std::memory_order_acquire))
                {
                    return;
                }

                if (waitResult == WAIT_OBJECT_0)
                {
                    continue;
                }

                if (!IsSnapshotValid(snapshot))
                {
                    CancelHoldIfForegroundChanged(snapshot.startWindow);
                    continue;
                }

                RepeatUntilInterrupted(snapshot);
            }
        }

        void RepeatUntilInterrupted(const RepeatSnapshot& snapshot)
        {
            while (!m_stopRequested.load(std::memory_order_acquire))
            {
                FlushVirtualUpIfDue();

                if (m_virtualUpPending.load(std::memory_order_acquire))
                {
                    return;
                }

                if (!IsSnapshotValid(snapshot))
                {
                    CancelHoldIfForegroundChanged(snapshot.startWindow);
                    return;
                }

                if (!SendSyntheticDownIfSafe(snapshot))
                {
                    return;
                }

                DWORD periodMs = m_periodMs.load(std::memory_order_acquire);
                DWORD waitResult = WaitForSingleObject(m_wakeEvent, periodMs);

                if (m_stopRequested.load(std::memory_order_acquire))
                {
                    return;
                }

                FlushVirtualUpIfDue();

                if (m_virtualUpPending.load(std::memory_order_acquire))
                {
                    return;
                }

                if (waitResult == WAIT_OBJECT_0)
                {
                    return;
                }
            }
        }

        RepeatSnapshot CaptureSnapshot() const
        {
            RepeatSnapshot snapshot = {};
            snapshot.keyEpoch = m_keyEpoch.load(std::memory_order_acquire);
            snapshot.inputEpoch = g_inputEpoch.load(std::memory_order_acquire);
            snapshot.gateEpoch = g_gateEpoch.load(std::memory_order_acquire);
            snapshot.startWindow = GetStartWindow();

            return snapshot;
        }

        bool IsSnapshotValid(const RepeatSnapshot& snapshot) const
        {
            if (g_gateState.load(std::memory_order_acquire) != AssistGateRunning)
            {
                return false;
            }

            if (g_gateEpoch.load(std::memory_order_acquire) != snapshot.gateEpoch)
            {
                return false;
            }

            if (g_inputEpoch.load(std::memory_order_acquire) != snapshot.inputEpoch)
            {
                return false;
            }

            if (!m_physicalDown.load(std::memory_order_acquire))
            {
                return false;
            }

            if (m_virtualUpPending.load(std::memory_order_acquire))
            {
                return false;
            }

            if (m_keyEpoch.load(std::memory_order_acquire) != snapshot.keyEpoch)
            {
                return false;
            }

            if (snapshot.startWindow == NULL)
            {
                return false;
            }

            if (GetForegroundWindow() != snapshot.startWindow)
            {
                return false;
            }

            if ((GetAsyncKeyState(static_cast<int>(m_vkCode)) & 0x8000) == 0)
            {
                return false;
            }

            return true;
        }

        bool SendSyntheticDownIfSafe(const RepeatSnapshot& snapshot)
        {
            if (!IsSnapshotValid(snapshot))
            {
                return false;
            }

            ULONGLONG beforeGateEpoch =
                g_gateEpoch.load(std::memory_order_acquire);

            ULONGLONG beforeInputEpoch =
                g_inputEpoch.load(std::memory_order_acquire);

            ULONGLONG beforeKeyEpoch =
                m_keyEpoch.load(std::memory_order_acquire);

            if (beforeGateEpoch != snapshot.gateEpoch ||
                beforeInputEpoch != snapshot.inputEpoch ||
                beforeKeyEpoch != snapshot.keyEpoch)
            {
                return false;
            }

            if (g_gateState.load(std::memory_order_acquire) != AssistGateRunning)
            {
                return false;
            }

            if (!IsSnapshotValid(snapshot))
            {
                return false;
            }

            INPUT input = {};
            input.type = INPUT_KEYBOARD;
            input.ki.wVk = 0;
            input.ki.wScan = m_scanCode;
            input.ki.dwFlags = KEYEVENTF_SCANCODE | m_inputFlags;
            input.ki.time = 0;
            input.ki.dwExtraInfo = kAssistExtraInfo;

            UINT sentCount = SendInput(1, &input, sizeof(INPUT));
            if (sentCount != 1)
            {
                return false;
            }

            m_virtualDownActive.store(true, std::memory_order_release);

            ULONGLONG afterGateEpoch =
                g_gateEpoch.load(std::memory_order_acquire);

            ULONGLONG afterInputEpoch =
                g_inputEpoch.load(std::memory_order_acquire);

            ULONGLONG afterKeyEpoch =
                m_keyEpoch.load(std::memory_order_acquire);

            bool changedDuringSend =
                afterGateEpoch != beforeGateEpoch ||
                afterInputEpoch != beforeInputEpoch ||
                afterKeyEpoch != beforeKeyEpoch;

            if (!m_physicalDown.load(std::memory_order_acquire))
            {
                ScheduleVirtualUp(kVirtualUpDelayMs);
                return false;
            }

            if ((GetAsyncKeyState(static_cast<int>(m_vkCode)) & 0x8000) == 0)
            {
                ScheduleVirtualUp(kVirtualUpDelayMs);
                return false;
            }

            if (changedDuringSend)
            {
                return false;
            }

            if (!IsSnapshotValid(snapshot))
            {
                return false;
            }

            return true;
        }

        void ScheduleVirtualUp(DWORD delayMs)
        {
            if (!m_virtualDownActive.load(std::memory_order_acquire))
            {
                return;
            }

            ULONGLONG dueTick = GetTickCount64() + delayMs;

            m_virtualUpDueTick.store(dueTick, std::memory_order_release);
            m_virtualUpPending.store(true, std::memory_order_release);

            Wake();
        }

        bool FlushVirtualUpIfDue()
        {
            if (!m_virtualUpPending.load(std::memory_order_acquire))
            {
                return false;
            }

            ULONGLONG nowTick = GetTickCount64();
            ULONGLONG dueTick =
                m_virtualUpDueTick.load(std::memory_order_acquire);

            if (nowTick < dueTick)
            {
                return false;
            }

            if (SendSyntheticUp())
            {
                m_virtualDownActive.store(false, std::memory_order_release);
                m_virtualUpPending.store(false, std::memory_order_release);
                m_virtualUpDueTick.store(0, std::memory_order_release);
                return true;
            }

            m_virtualUpDueTick.store(
                GetTickCount64() + kVirtualUpRetryMs,
                std::memory_order_release);

            Wake();
            return false;
        }

        bool SendSyntheticUp()
        {
            INPUT input = {};
            input.type = INPUT_KEYBOARD;
            input.ki.wVk = 0;
            input.ki.wScan = m_scanCode;
            input.ki.dwFlags =
                KEYEVENTF_SCANCODE |
                KEYEVENTF_KEYUP |
                m_inputFlags;

            input.ki.time = 0;
            input.ki.dwExtraInfo = kAssistExtraInfo;

            UINT sentCount = SendInput(1, &input, sizeof(INPUT));
            return sentCount == 1;
        }

        void CancelHoldIfForegroundChanged(HWND startWindow)
        {
            if (startWindow == NULL)
            {
                return;
            }

            if (GetForegroundWindow() == startWindow)
            {
                return;
            }

            m_physicalDown.store(false, std::memory_order_release);
            m_keyEpoch.fetch_add(1, std::memory_order_acq_rel);
            m_startWindowValue.store(0, std::memory_order_release);

            if (m_virtualDownActive.load(std::memory_order_acquire))
            {
                ScheduleVirtualUp(kVirtualUpDelayMs);
            }
        }

        HWND GetStartWindow() const
        {
            ULONG_PTR value =
                m_startWindowValue.load(std::memory_order_acquire);

            return reinterpret_cast<HWND>(value);
        }

    private:
        HANDLE m_threadHandle;
        HANDLE m_wakeEvent;

        DWORD m_vkCode;
        WORD m_scanCode;
        DWORD m_inputFlags;

        std::atomic<DWORD> m_delayMs;
        std::atomic<DWORD> m_periodMs;

        std::atomic<bool> m_stopRequested;
        std::atomic<bool> m_physicalDown;
        std::atomic<bool> m_virtualDownActive;
        std::atomic<bool> m_virtualUpPending;

        std::atomic<ULONGLONG> m_virtualUpDueTick;
        std::atomic<ULONGLONG> m_keyEpoch;
        std::atomic<ULONG_PTR> m_startWindowValue;
    };

    static RepeatKeyWorker g_workers[4];
    static bool g_workerStarted[4] = {};

    static int ClampInt(int value, int minValue, int maxValue)
    {
        if (value < minValue)
        {
            return minValue;
        }

        if (value > maxValue)
        {
            return maxValue;
        }

        return value;
    }

    static bool ReadKeyboardRegistryValue(
        const wchar_t* valueName,
        int defaultValue,
        int* outValue)
    {
        HKEY keyHandle = NULL;

        LONG openResult = RegOpenKeyExW(
            HKEY_CURRENT_USER,
            L"Control Panel\\Keyboard",
            0,
            KEY_QUERY_VALUE,
            &keyHandle);

        if (openResult != ERROR_SUCCESS)
        {
            *outValue = defaultValue;
            return false;
        }

        DWORD valueType = 0;
        BYTE valueBuffer[128] = {};
        DWORD valueSize = sizeof(valueBuffer);

        LONG queryResult = RegQueryValueExW(
            keyHandle,
            valueName,
            NULL,
            &valueType,
            valueBuffer,
            &valueSize);

        RegCloseKey(keyHandle);

        if (queryResult != ERROR_SUCCESS)
        {
            *outValue = defaultValue;
            return false;
        }

        if (valueType == REG_DWORD && valueSize >= sizeof(DWORD))
        {
            *outValue =
                static_cast<int>(*reinterpret_cast<DWORD*>(valueBuffer));
            return true;
        }

        if (valueType == REG_SZ || valueType == REG_EXPAND_SZ)
        {
            const wchar_t* textValue =
                reinterpret_cast<const wchar_t*>(valueBuffer);

            *outValue = _wtoi(textValue);
            return true;
        }

        *outValue = defaultValue;
        return false;
    }

    static DWORD CalculateDelayMs(int keyboardDelay)
    {
        int safeDelay = ClampInt(keyboardDelay, 0, 3);
        return static_cast<DWORD>((safeDelay + 1) * 250);
    }

    static DWORD CalculateRepeatPeriodMs(int keyboardSpeed)
    {
        int safeSpeed = ClampInt(keyboardSpeed, 0, 31);

        int hwValue = 31 - safeSpeed;
        int exponent = hwValue >> 3;
        int mantissa = hwValue & 7;
        int baseDelay = (8 + mantissa) << exponent;
        int periodMs = (baseDelay * 25 + 3) / 6;

        return static_cast<DWORD>(periodMs);
    }

    static KeyboardTimingConfig LoadKeyboardTimingConfig()
    {
        KeyboardTimingConfig config = {};

        int keyboardSpeed = 31;
        int keyboardDelay = 1;

        ReadKeyboardRegistryValue(L"KeyboardSpeed", 31, &keyboardSpeed);
        ReadKeyboardRegistryValue(L"KeyboardDelay", 1, &keyboardDelay);

        config.rawSpeed = keyboardSpeed;
        config.rawDelay = keyboardDelay;
        config.repeatPeriodMs = CalculateRepeatPeriodMs(keyboardSpeed);
        config.delayMs = CalculateDelayMs(keyboardDelay);

        return config;
    }

    static RepeatKeyWorker* FindWorkerByVkCode(DWORD vkCode)
    {
        for (int index = 0; index < 4; ++index)
        {
            if (g_workers[index].GetVkCode() == vkCode)
            {
                return &g_workers[index];
            }
        }

        return NULL;
    }

    static bool IsSelfInjectedKeyboardEvent(const KBDLLHOOKSTRUCT* keyInfo)
    {
        return keyInfo->dwExtraInfo == kAssistExtraInfo;
    }

    static bool IsKeyDownMessage(WPARAM messageValue)
    {
        return messageValue == WM_KEYDOWN ||
            messageValue == WM_SYSKEYDOWN;
    }

    static bool IsKeyUpMessage(WPARAM messageValue)
    {
        return messageValue == WM_KEYUP ||
            messageValue == WM_SYSKEYUP;
    }

    static void WakeAllWorkers()
    {
        for (int index = 0; index < 4; ++index)
        {
            g_workers[index].Wake();
        }
    }

    static void PauseAssistForPhysicalInput()
    {
        g_gateState.store(AssistGatePaused, std::memory_order_release);
        g_gateEpoch.fetch_add(1, std::memory_order_acq_rel);
        g_inputEpoch.fetch_add(1, std::memory_order_acq_rel);

        WakeAllWorkers();
    }

    static void ResumeAssistAfterPhysicalInput()
    {
        g_gateEpoch.fetch_add(1, std::memory_order_acq_rel);
        g_gateState.store(AssistGateRunning, std::memory_order_release);

        WakeAllWorkers();
    }

    static LRESULT CALLBACK KeyboardHookProc(
        int code,
        WPARAM wParam,
        LPARAM lParam)
    {
        if (code == HC_ACTION)
        {
            const KBDLLHOOKSTRUCT* keyInfo =
                reinterpret_cast<const KBDLLHOOKSTRUCT*>(lParam);

            if (!IsSelfInjectedKeyboardEvent(keyInfo))
            {
                PauseAssistForPhysicalInput();

                RepeatKeyWorker* worker = FindWorkerByVkCode(keyInfo->vkCode);

                if (worker != NULL)
                {
                    if (IsKeyDownMessage(wParam))
                    {
                        worker->NotifyPhysicalDown(GetForegroundWindow());
                    }
                    else if (IsKeyUpMessage(wParam))
                    {
                        worker->NotifyPhysicalUp();
                    }
                }

                ResumeAssistAfterPhysicalInput();
            }
        }

        return CallNextHookEx(g_keyboardHook, code, wParam, lParam);
    }

    static bool CreateSingleInstanceMutex()
    {
        g_singleInstanceMutex = CreateMutexW(
            NULL,
            TRUE,
            kSingleInstanceMutexName);

        if (g_singleInstanceMutex == NULL)
        {
            return false;
        }

        if (GetLastError() == ERROR_ALREADY_EXISTS)
        {
            CloseHandle(g_singleInstanceMutex);
            g_singleInstanceMutex = NULL;
            return false;
        }

        return true;
    }

    static bool StartWorkers()
    {
        const RepeatKeyProfile profiles[4] =
        {
            { VK_LSHIFT,   0x2A, 0 },
            { VK_LCONTROL, 0x1D, 0 },
            { VK_LMENU,    0x38, 0 },
            { VK_RSHIFT,   0x36, 0 }
        };

        for (int index = 0; index < 4; ++index)
        {
            g_workerStarted[index] = false;
        }

        for (int index = 0; index < 4; ++index)
        {
            bool started = g_workers[index].Start(
                profiles[index],
                g_timing.delayMs,
                g_timing.repeatPeriodMs);

            if (!started)
            {
                return false;
            }

            g_workerStarted[index] = true;
        }

        return true;
    }

    static void StopWorkers()
    {
        for (int index = 0; index < 4; ++index)
        {
            if (g_workerStarted[index])
            {
                g_workers[index].Stop();
                g_workerStarted[index] = false;
            }
        }
    }

    static bool InstallKeyboardHook(HINSTANCE instanceHandle)
    {
        g_keyboardHook = SetWindowsHookExW(
            WH_KEYBOARD_LL,
            KeyboardHookProc,
            instanceHandle,
            0);

        return g_keyboardHook != NULL;
    }

    static void RemoveKeyboardHook()
    {
        if (g_keyboardHook != NULL)
        {
            UnhookWindowsHookEx(g_keyboardHook);
            g_keyboardHook = NULL;
        }
    }

    static void ReleaseSingleInstanceMutex()
    {
        if (g_singleInstanceMutex != NULL)
        {
            ReleaseMutex(g_singleInstanceMutex);
            CloseHandle(g_singleInstanceMutex);
            g_singleInstanceMutex = NULL;
        }
    }

    static void Cleanup()
    {
        RemoveKeyboardHook();
        StopWorkers();
        ReleaseSingleInstanceMutex();
    }

    int Run(HINSTANCE instanceHandle)
    {
        if (!CreateSingleInstanceMutex())
        {
            return 0;
        }

        g_timing = LoadKeyboardTimingConfig();

        if (!StartWorkers())
        {
            Cleanup();
            return 1;
        }

        if (!InstallKeyboardHook(instanceHandle))
        {
            Cleanup();
            return 1;
        }

        MSG message = {};
        while (GetMessageW(&message, NULL, 0, 0) > 0)
        {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }

        Cleanup();
        return 0;
    }
}
