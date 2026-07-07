#include "AcadShiftPulseAssist.h"

#include <windows.h>
#include <atomic>
#include <cstdlib>

namespace AcadShiftPulseAssist
{
    struct KeyboardTimingConfig
    {
        DWORD delayMs;
        int rawDelay;
    };

    struct PulseKeyProfile
    {
        DWORD vkCode;
        WORD scanCode;
        DWORD inputFlags;
    };

    struct PulseSnapshot
    {
        ULONGLONG keyEpoch;
        HWND startWindow;
    };

    static const wchar_t* kSingleInstanceMutexName =
        L"Local\\AcadShiftPulseAssist_SingleInstance";

    static const ULONG_PTR kAssistExtraInfo = 0x53485250UL;
    static const DWORD kVirtualUpDelayMs = 10;
    static const DWORD kVirtualUpRetryMs = 10;

    static HHOOK g_keyboardHook = NULL;
    static HANDLE g_singleInstanceMutex = NULL;

    static KeyboardTimingConfig g_timing = {};

    static bool IsAllowedForegroundWindow(HWND windowHandle);

    class PulseKeyWorker
    {
    public:
        PulseKeyWorker()
            : m_threadHandle(NULL),
            m_wakeEvent(NULL),
            m_vkCode(0),
            m_scanCode(0),
            m_inputFlags(0),
            m_delayMs(500),
            m_stopRequested(false),
            m_physicalDown(false),
            m_nativeRepeatSeen(false),
            m_pulseSent(false),
            m_virtualDownActive(false),
            m_virtualUpPending(false),
            m_virtualUpDueTick(0),
            m_keyEpoch(0),
            m_startWindowValue(0)
        {
        }

        bool Start(const PulseKeyProfile& profile, DWORD delayMs)
        {
            m_vkCode = profile.vkCode;
            m_scanCode = profile.scanCode;
            m_inputFlags = profile.inputFlags;

            m_delayMs.store(delayMs, std::memory_order_release);
            m_stopRequested.store(false, std::memory_order_release);
            m_physicalDown.store(false, std::memory_order_release);
            m_nativeRepeatSeen.store(false, std::memory_order_release);
            m_pulseSent.store(false, std::memory_order_release);
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
                &PulseKeyWorker::ThreadProc,
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

        void NotifyPhysicalDown(HWND foregroundWindow, bool allowedWindow)
        {
            if (!m_physicalDown.load(std::memory_order_acquire))
            {
                if (!allowedWindow)
                {
                    return;
                }

                m_startWindowValue.store(
                    reinterpret_cast<ULONG_PTR>(foregroundWindow),
                    std::memory_order_release);

                if (!m_virtualDownActive.load(std::memory_order_acquire) &&
                    !m_virtualUpPending.load(std::memory_order_acquire))
                {
                    m_nativeRepeatSeen.store(false, std::memory_order_release);
                    m_pulseSent.store(false, std::memory_order_release);
                }
                else
                {
                    m_nativeRepeatSeen.store(true, std::memory_order_release);
                    m_pulseSent.store(true, std::memory_order_release);
                }

                m_physicalDown.store(true, std::memory_order_release);
                m_keyEpoch.fetch_add(1, std::memory_order_acq_rel);

                Wake();
                return;
            }

            m_nativeRepeatSeen.store(true, std::memory_order_release);
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
            PulseKeyWorker* self = static_cast<PulseKeyWorker*>(parameter);
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

                if (m_nativeRepeatSeen.load(std::memory_order_acquire))
                {
                    return;
                }

                if (m_pulseSent.load(std::memory_order_acquire))
                {
                    return;
                }

                PulseSnapshot snapshot = CaptureSnapshot();
                DWORD delayMs = m_delayMs.load(std::memory_order_acquire);

                DWORD waitResult = WaitForSingleObject(m_wakeEvent, delayMs);

                if (m_stopRequested.load(std::memory_order_acquire))
                {
                    return;
                }

                FlushVirtualUpIfDue();

                if (waitResult == WAIT_OBJECT_0)
                {
                    continue;
                }

                if (!IsSnapshotValid(snapshot))
                {
                    CancelHoldIfForegroundChanged(snapshot.startWindow);
                    continue;
                }

                SendPulseIfSafe(snapshot);
                return;
            }
        }

        PulseSnapshot CaptureSnapshot() const
        {
            PulseSnapshot snapshot = {};
            snapshot.keyEpoch = m_keyEpoch.load(std::memory_order_acquire);
            snapshot.startWindow = GetStartWindow();

            return snapshot;
        }

        bool IsSnapshotValid(const PulseSnapshot& snapshot) const
        {
            if (!m_physicalDown.load(std::memory_order_acquire))
            {
                return false;
            }

            if (m_nativeRepeatSeen.load(std::memory_order_acquire))
            {
                return false;
            }

            if (m_pulseSent.load(std::memory_order_acquire))
            {
                return false;
            }

            if (m_virtualDownActive.load(std::memory_order_acquire))
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

            if (!IsAllowedForegroundWindow(snapshot.startWindow))
            {
                return false;
            }

            if ((GetAsyncKeyState(static_cast<int>(m_vkCode)) & 0x8000) == 0)
            {
                return false;
            }

            return true;
        }

        bool SendPulseIfSafe(const PulseSnapshot& snapshot)
        {
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
            m_pulseSent.store(true, std::memory_order_release);

            if (!m_physicalDown.load(std::memory_order_acquire))
            {
                ScheduleVirtualUp(kVirtualUpDelayMs);
                return true;
            }

            if ((GetAsyncKeyState(static_cast<int>(m_vkCode)) & 0x8000) == 0)
            {
                ScheduleVirtualUp(kVirtualUpDelayMs);
                return true;
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

        std::atomic<bool> m_stopRequested;
        std::atomic<bool> m_physicalDown;
        std::atomic<bool> m_nativeRepeatSeen;
        std::atomic<bool> m_pulseSent;
        std::atomic<bool> m_virtualDownActive;
        std::atomic<bool> m_virtualUpPending;

        std::atomic<ULONGLONG> m_virtualUpDueTick;
        std::atomic<ULONGLONG> m_keyEpoch;
        std::atomic<ULONG_PTR> m_startWindowValue;
    };

    static PulseKeyWorker g_workers[2];
    static bool g_workerStarted[2] = {};

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

    static const wchar_t* FindFileNamePart(const wchar_t* path)
    {
        const wchar_t* fileName = path;
        const wchar_t* current = path;

        while (*current != L'\0')
        {
            if (*current == L'\\' || *current == L'/')
            {
                fileName = current + 1;
            }

            ++current;
        }

        return fileName;
    }

    static bool IsAllowedProcessName(const wchar_t* fileName)
    {
        if (fileName == NULL)
        {
            return false;
        }

        if (_wcsnicmp(fileName, L"acadlt.exe", 11) == 0)
        {
            return true;
        }

        if (int i = fileName[0])
        {
            if (_wcsnicmp(
                &fileName[(((i & 0xDF) == 0x5A) ? 2 : 1)],
                L"cad.exe",
                8) == 0)
            {
                return true;
            }
        }

        return false;
    }

    static bool IsAllowedForegroundWindow(HWND windowHandle)
    {
        if (windowHandle == NULL)
        {
            return false;
        }

        DWORD processId = 0;
        GetWindowThreadProcessId(windowHandle, &processId);

        if (processId == 0)
        {
            return false;
        }

        HANDLE processHandle = OpenProcess(
            PROCESS_QUERY_LIMITED_INFORMATION,
            FALSE,
            processId);

        if (processHandle == NULL)
        {
            return false;
        }

        wchar_t imagePath[MAX_PATH] = {};
        DWORD imagePathSize = MAX_PATH;

        BOOL queryResult = QueryFullProcessImageNameW(
            processHandle,
            0,
            imagePath,
            &imagePathSize);

        CloseHandle(processHandle);

        if (!queryResult)
        {
            return false;
        }

        const wchar_t* fileName = FindFileNamePart(imagePath);
        return IsAllowedProcessName(fileName);
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

    static KeyboardTimingConfig LoadKeyboardTimingConfig()
    {
        KeyboardTimingConfig config = {};

        int keyboardDelay = 1;

        ReadKeyboardRegistryValue(L"KeyboardDelay", 1, &keyboardDelay);

        config.rawDelay = keyboardDelay;
        config.delayMs = CalculateDelayMs(keyboardDelay);

        return config;
    }

    static PulseKeyWorker* FindWorkerByVkCode(DWORD vkCode)
    {
        for (int index = 0; index < 2; ++index)
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

    static bool StartWorkers()
    {
        const PulseKeyProfile profiles[2] =
        {
            { VK_LSHIFT, 0x2A, 0 },
            { VK_RSHIFT, 0x36, 0 }
        };

        for (int index = 0; index < 2; ++index)
        {
            g_workerStarted[index] = false;
        }

        for (int index = 0; index < 2; ++index)
        {
            bool started = g_workers[index].Start(
                profiles[index],
                g_timing.delayMs);

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
        for (int index = 0; index < 2; ++index)
        {
            if (g_workerStarted[index])
            {
                g_workers[index].Stop();
                g_workerStarted[index] = false;
            }
        }
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
                PulseKeyWorker* worker = FindWorkerByVkCode(keyInfo->vkCode);

                if (worker != NULL)
                {
                    if (IsKeyDownMessage(wParam))
                    {
                        HWND foregroundWindow = GetForegroundWindow();
                        bool allowedWindow =
                            IsAllowedForegroundWindow(foregroundWindow);

                        worker->NotifyPhysicalDown(
                            foregroundWindow,
                            allowedWindow);
                    }
                    else if (IsKeyUpMessage(wParam))
                    {
                        worker->NotifyPhysicalUp();
                    }
                }
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
