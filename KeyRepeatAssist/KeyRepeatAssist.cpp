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

    static const bool kCorrectOverlappedSend = false;

    static const wchar_t* kSingleInstanceMutexName =
        L"Local\\KeyRepeatAssist_SingleInstance";

    static std::atomic<LONG> g_gateState(AssistGateRunning);
    static std::atomic<ULONGLONG> g_gateEpoch(0);
    static std::atomic<ULONGLONG> g_inputEpoch(0);

    static HHOOK g_keyboardHook = NULL;
    static HANDLE g_singleInstanceMutex = NULL;

    static KeyboardTimingConfig g_timing = {};

    static ULONGLONG ReadCounter()
    {
        LARGE_INTEGER value = {};
        QueryPerformanceCounter(&value);
        return static_cast<ULONGLONG>(value.QuadPart);
    }

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
            m_correctionUpPending(false),
            m_keyEpoch(0),
            m_startWindowValue(0),
            m_lastPhysicalUpCounter(0)
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
            m_correctionUpPending.store(false, std::memory_order_release);
            m_keyEpoch.store(0, std::memory_order_release);
            m_startWindowValue.store(0, std::memory_order_release);
            m_lastPhysicalUpCounter.store(0, std::memory_order_release);

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

            m_lastPhysicalUpCounter.store(0, std::memory_order_release);
            m_correctionUpPending.store(false, std::memory_order_release);
            m_physicalDown.store(true, std::memory_order_release);
            m_keyEpoch.fetch_add(1, std::memory_order_acq_rel);

            Wake();
        }

        void NotifyPhysicalUp(ULONGLONG hookCounter)
        {
            m_lastPhysicalUpCounter.store(hookCounter, std::memory_order_release);
            m_physicalDown.store(false, std::memory_order_release);
            m_startWindowValue.store(0, std::memory_order_release);
            m_keyEpoch.fetch_add(1, std::memory_order_acq_rel);

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
                WaitForSingleObject(m_wakeEvent, INFINITE);

                if (m_stopRequested.load(std::memory_order_acquire))
                {
                    break;
                }

                ProcessCurrentState();
            }

            FlushCorrectionUpIfNeeded();
        }

        void ProcessCurrentState()
        {
            while (!m_stopRequested.load(std::memory_order_acquire))
            {
                FlushCorrectionUpIfNeeded();

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

                FlushCorrectionUpIfNeeded();

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
                FlushCorrectionUpIfNeeded();

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

                FlushCorrectionUpIfNeeded();

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

            ULONGLONG sendBeginCounter = ReadCounter();

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
            input.ki.dwExtraInfo = GetMessageExtraInfo();

            UINT sentCount = SendInput(1, &input, sizeof(INPUT));
            ULONGLONG sendEndCounter = ReadCounter();

            if (sentCount != 1)
            {
                return false;
            }

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

            if (changedDuringSend)
            {
                RequestCorrectionIfSendAfterPhysicalUp(
                    sendBeginCounter,
                    sendEndCounter);
                return false;
            }

            if (!IsSnapshotValid(snapshot))
            {
                RequestCorrectionIfSendAfterPhysicalUp(
                    sendBeginCounter,
                    sendEndCounter);
                return false;
            }

            return true;
        }

        void RequestCorrectionIfSendAfterPhysicalUp(
            ULONGLONG sendBeginCounter,
            ULONGLONG sendEndCounter)
        {
            if (m_physicalDown.load(std::memory_order_acquire))
            {
                return;
            }

            ULONGLONG physicalUpCounter =
                m_lastPhysicalUpCounter.load(std::memory_order_acquire);

            if (physicalUpCounter == 0)
            {
                return;
            }

            bool clearlyAfterUp = sendBeginCounter >= physicalUpCounter;
            bool overlappedWithUp =
                sendBeginCounter < physicalUpCounter &&
                sendEndCounter >= physicalUpCounter;

            if (clearlyAfterUp || (kCorrectOverlappedSend && overlappedWithUp))
            {
                m_correctionUpPending.store(true, std::memory_order_release);
                Wake();
            }
        }

        void FlushCorrectionUpIfNeeded()
        {
            if (m_physicalDown.load(std::memory_order_acquire))
            {
                m_correctionUpPending.store(false, std::memory_order_release);
                return;
            }

            bool needCorrection =
                m_correctionUpPending.exchange(false, std::memory_order_acq_rel);

            if (!needCorrection)
            {
                return;
            }

            INPUT input = {};
            input.type = INPUT_KEYBOARD;
            input.ki.wVk = 0;
            input.ki.wScan = m_scanCode;
            input.ki.dwFlags =
                KEYEVENTF_SCANCODE |
                KEYEVENTF_KEYUP |
                m_inputFlags;

            input.ki.time = 0;
            input.ki.dwExtraInfo = GetMessageExtraInfo();

            SendInput(1, &input, sizeof(INPUT));
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
        std::atomic<bool> m_correctionUpPending;

        std::atomic<ULONGLONG> m_keyEpoch;
        std::atomic<ULONG_PTR> m_startWindowValue;
        std::atomic<ULONGLONG> m_lastPhysicalUpCounter;
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
        config.delayMs = CalculateDelayMs(keyboardDelay) + config.repeatPeriodMs;

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

    static bool IsInjectedKeyboardEvent(const KBDLLHOOKSTRUCT* keyInfo)
    {
        return (keyInfo->flags & LLKHF_INJECTED) != 0;
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

            if (!IsInjectedKeyboardEvent(keyInfo))
            {
                ULONGLONG hookCounter = ReadCounter();

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
                        worker->NotifyPhysicalUp(hookCounter);
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
