#include <windows.h>

#include <iostream>
#include <iomanip>
#include <sstream>
#include <string>
#include <map>

struct KeyboardTimingConfig
{
    int delayMs;
    int repeatPeriodMs;
    int rawDelay;
    int rawSpeed;
};

struct HeldKeyState
{
    bool isDown;
    unsigned int repeatCount;
    ULONGLONG firstDownTick;
    ULONGLONG lastDownTick;
    ULONGLONG firstRepeatDelayMs;

    HeldKeyState()
        : isDown(false),
        repeatCount(0),
        firstDownTick(0),
        lastDownTick(0),
        firstRepeatDelayMs(0)
    {
    }
};

static HHOOK g_keyboardHook = NULL;
static DWORD g_mainThreadId = 0;
static UINT_PTR g_timerId = 0;

static KeyboardTimingConfig g_timing = { 530, 92, 1, 31 };
static std::map<DWORD, HeldKeyState> g_keyStates;

static bool g_hasInlineLog = false;
static size_t g_inlineLogLength = 0;

enum ConsoleLogColor
{
    LogColorDefault,
    LogColorBlue,
    LogColorGreen,
    LogColorRed,
    LogColorWhite
};

static HANDLE g_consoleHandle = NULL;
static WORD g_defaultConsoleAttr = 0;

static bool ReadKeyboardRegistryValue(const wchar_t* valueName, int defaultValue, int* outValue)
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
        *outValue = static_cast<int>(*reinterpret_cast<DWORD*>(valueBuffer));
        return true;
    }

    if (valueType == REG_SZ || valueType == REG_EXPAND_SZ)
    {
        const wchar_t* textValue = reinterpret_cast<const wchar_t*>(valueBuffer);
        *outValue = _wtoi(textValue);
        return true;
    }

    *outValue = defaultValue;
    return false;
}

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

static int CalculateDelayMs(int keyboardDelay)
{
    int safeDelay = ClampInt(keyboardDelay, 0, 3);
    return (safeDelay + 1) * 250;
}

static int CalculateRepeatPeriodMs(int keyboardSpeed)
{
    int safeSpeed = ClampInt(keyboardSpeed, 0, 31);

    int hwValue = 31 - safeSpeed;
    int exponent = hwValue >> 3;
    int mantissa = hwValue & 7;
    int baseDelay = (8 + mantissa) << exponent;
    int periodMs = (baseDelay * 25 + 3) / 6;

    return periodMs;
}

static KeyboardTimingConfig LoadKeyboardTimingConfig()
{
    KeyboardTimingConfig config = {};

    int keyboardDelay = 1;
    int keyboardSpeed = 31;

    ReadKeyboardRegistryValue(L"KeyboardDelay", 1, &keyboardDelay);
    ReadKeyboardRegistryValue(L"KeyboardSpeed", 31, &keyboardSpeed);

    config.rawDelay = keyboardDelay;
    config.rawSpeed = keyboardSpeed;
    config.delayMs = CalculateDelayMs(keyboardDelay);
    config.repeatPeriodMs = CalculateRepeatPeriodMs(keyboardSpeed);

    return config;
}

static std::string FormatVkCode(DWORD vkCode)
{
    std::ostringstream oss;
    oss << "VK_0x"
        << std::uppercase
        << std::hex
        << std::setw(2)
        << std::setfill('0')
        << vkCode;

    return oss.str();
}

static std::string GetKeyLabel(DWORD vkCode)
{
    if (vkCode >= 'A' && vkCode <= 'Z')
    {
        return std::string(1, static_cast<char>(vkCode));
    }

    if (vkCode >= '0' && vkCode <= '9')
    {
        return std::string(1, static_cast<char>(vkCode));
    }

    switch (vkCode)
    {
    case VK_ESCAPE:
        return "ESC";
    case VK_SPACE:
        return "SPACE";
    case VK_RETURN:
        return "ENTER";
    case VK_TAB:
        return "TAB";
    case VK_BACK:
        return "BACKSPACE";
    case VK_SHIFT:
        return "SHIFT";
    case VK_LSHIFT:
        return "LSHIFT";
    case VK_RSHIFT:
        return "RSHIFT";
    case VK_CONTROL:
        return "CTRL";
    case VK_LCONTROL:
        return "LCTRL";
    case VK_RCONTROL:
        return "RCTRL";
    case VK_MENU:
        return "ALT";
    case VK_LMENU:
        return "LALT";
    case VK_RMENU:
        return "RALT";
    case VK_LEFT:
        return "LEFT";
    case VK_RIGHT:
        return "RIGHT";
    case VK_UP:
        return "UP";
    case VK_DOWN:
        return "DOWN";
    case VK_HOME:
        return "HOME";
    case VK_END:
        return "END";
    case VK_PRIOR:
        return "PAGEUP";
    case VK_NEXT:
        return "PAGEDOWN";
    case VK_INSERT:
        return "INSERT";
    case VK_DELETE:
        return "DELETE";
    case VK_CAPITAL:
        return "CAPSLOCK";
    case VK_F1:
        return "F1";
    case VK_F2:
        return "F2";
    case VK_F3:
        return "F3";
    case VK_F4:
        return "F4";
    case VK_F5:
        return "F5";
    case VK_F6:
        return "F6";
    case VK_F7:
        return "F7";
    case VK_F8:
        return "F8";
    case VK_F9:
        return "F9";
    case VK_F10:
        return "F10";
    case VK_F11:
        return "F11";
    case VK_F12:
        return "F12";
    default:
        break;
    }

    return FormatVkCode(vkCode);
}

static void FinishInlineLog()
{
    if (g_hasInlineLog)
    {
        std::cout << std::endl;
        g_hasInlineLog = false;
        g_inlineLogLength = 0;
    }
}

static WORD GetConsoleAttribute(ConsoleLogColor color)
{
    switch (color)
    {
    case LogColorBlue:
        return FOREGROUND_BLUE | FOREGROUND_INTENSITY;

    case LogColorGreen:
        return FOREGROUND_GREEN | FOREGROUND_INTENSITY;

    case LogColorRed:
        return FOREGROUND_RED | FOREGROUND_INTENSITY;

    case LogColorWhite:
        return FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY;

    case LogColorDefault:
    default:
        return g_defaultConsoleAttr;
    }
}

static void SetConsoleLogColor(ConsoleLogColor color)
{
    if (g_consoleHandle == NULL)
    {
        return;
    }

    SetConsoleTextAttribute(g_consoleHandle, GetConsoleAttribute(color));
}

static void RestoreConsoleLogColor()
{
    if (g_consoleHandle == NULL)
    {
        return;
    }

    SetConsoleTextAttribute(g_consoleHandle, g_defaultConsoleAttr);
}

static void WriteLogLine(const std::string& text, ConsoleLogColor color)
{
    FinishInlineLog();

    SetConsoleLogColor(color);
    std::cout << text << std::endl;
    RestoreConsoleLogColor();
}

static void WriteInlineLog(const std::string& text, ConsoleLogColor color)
{
    SetConsoleLogColor(color);

    std::cout << '\r' << text;

    if (g_inlineLogLength > text.length())
    {
        std::cout << std::string(g_inlineLogLength - text.length(), ' ');
        std::cout << '\r' << text;
    }

    std::cout.flush();
    RestoreConsoleLogColor();

    g_hasInlineLog = true;
    g_inlineLogLength = text.length();
}

static void OnKeyDown(DWORD vkCode)
{
    ULONGLONG nowTick = GetTickCount64();
    HeldKeyState& state = g_keyStates[vkCode];

    if (!state.isDown)
    {
        state.isDown = true;
        state.repeatCount = 0;
        state.firstDownTick = nowTick;
        state.lastDownTick = nowTick;
        state.firstRepeatDelayMs = 0;

        std::ostringstream oss;
        oss << "[KEY DOWN] " << GetKeyLabel(vkCode) << " was pressed.";
        WriteLogLine(oss.str(), LogColorWhite);
        return;
    }

    ULONGLONG lastIntervalMs = nowTick - state.lastDownTick;

    state.lastDownTick = nowTick;
    ++state.repeatCount;

    if (state.repeatCount == 1)
    {
        state.firstRepeatDelayMs = nowTick - state.firstDownTick;

        std::ostringstream oss;
        oss << "[KEY REPEAT] "
            << GetKeyLabel(vkCode)
            << " first repeat after: "
            << std::setw(5)
            << std::setfill('0')
            << state.firstRepeatDelayMs
            << " ms, repeat count: "
            << std::setw(2)
            << std::setfill('0')
            << state.repeatCount;

        WriteInlineLog(oss.str(), LogColorGreen);
        return;
    }

    std::ostringstream oss;
    oss << "[KEY REPEAT] "
        << GetKeyLabel(vkCode)
        << " repeat count: "
        << std::setw(2)
        << std::setfill('0')
        << state.repeatCount
        << ", first repeat: "
        << std::setw(5)
        << std::setfill('0')
        << state.firstRepeatDelayMs
        << " ms, last interval: "
        << std::setw(5)
        << std::setfill('0')
        << lastIntervalMs
        << " ms";

    WriteInlineLog(oss.str(), LogColorGreen);
}

static void OnKeyUp(DWORD vkCode)
{
    std::map<DWORD, HeldKeyState>::iterator it = g_keyStates.find(vkCode);

    if (it == g_keyStates.end() || !it->second.isDown)
    {
        return;
    }

    it->second = HeldKeyState();

    std::ostringstream oss;
    oss << "[KEY UP] " << GetKeyLabel(vkCode) << " was released.";
    WriteLogLine(oss.str(), LogColorWhite);
}

static void CheckHeldKeyTimeouts()
{
    ULONGLONG nowTick = GetTickCount64();

    for (std::map<DWORD, HeldKeyState>::iterator it = g_keyStates.begin(); it != g_keyStates.end(); ++it)
    {
        DWORD vkCode = it->first;
        HeldKeyState& state = it->second;

        if (!state.isDown)
        {
            continue;
        }

        if (state.repeatCount != 0)
        {
            continue;
        }

        ULONGLONG elapsedMs = nowTick - state.firstDownTick;

        std::ostringstream oss;
        ConsoleLogColor logColor;

        if (elapsedMs < static_cast<ULONGLONG>(g_timing.delayMs))
        {
            oss << "[WAIT REPEAT] "
                << GetKeyLabel(vkCode)
                << " wait elapsed: "
                << std::setw(5)
                << std::setfill('0')
                << elapsedMs
                << " ms / "
                << std::setw(5)
                << std::setfill('0')
                << g_timing.delayMs
                << " ms";

            logColor = LogColorBlue;
        }
        else
        {
            oss << "[NO REPEAT] "
                << GetKeyLabel(vkCode)
                << " still down, wait elapsed: "
                << std::setw(5)
                << std::setfill('0')
                << elapsedMs
                << " ms / "
                << std::setw(5)
                << std::setfill('0')
                << g_timing.delayMs
                << " ms";

            logColor = LogColorRed;
        }

        WriteInlineLog(oss.str(), logColor);
    }
}

static LRESULT CALLBACK KeyboardHookProc(int code, WPARAM wParam, LPARAM lParam)
{
    if (code == HC_ACTION)
    {
        const KBDLLHOOKSTRUCT* keyInfo = reinterpret_cast<const KBDLLHOOKSTRUCT*>(lParam);
        DWORD vkCode = keyInfo->vkCode;

        if (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN)
        {
            OnKeyDown(vkCode);

            /*
            if (vkCode == VK_ESCAPE)
            {
                WriteLogLine("[EXIT] ESC was received. Closing program.", LogColorWhite);
                PostThreadMessage(g_mainThreadId, WM_QUIT, 0, 0);
            }
            */
        }
        else if (wParam == WM_KEYUP || wParam == WM_SYSKEYUP)
        {
            OnKeyUp(vkCode);
        }
    }

    return CallNextHookEx(g_keyboardHook, code, wParam, lParam);
}

int main()
{
    g_consoleHandle = GetStdHandle(STD_OUTPUT_HANDLE);

    CONSOLE_SCREEN_BUFFER_INFO consoleInfo = {};
    if (g_consoleHandle != INVALID_HANDLE_VALUE &&
        GetConsoleScreenBufferInfo(g_consoleHandle, &consoleInfo))
    {
        g_defaultConsoleAttr = consoleInfo.wAttributes;
    }
    else
    {
        g_consoleHandle = NULL;
        g_defaultConsoleAttr = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE;
    }

    g_mainThreadId = GetCurrentThreadId();
    g_timing = LoadKeyboardTimingConfig();

    std::cout << "Low-level keyboard hook PoC started." << std::endl;
    std::cout << "KeyboardDelay raw value: " << g_timing.rawDelay << std::endl;
    std::cout << "KeyboardSpeed raw value: " << g_timing.rawSpeed << std::endl;
    std::cout << "Initial repeat wait: " << g_timing.delayMs << " ms" << std::endl;
    std::cout << "Repeat period: " << g_timing.repeatPeriodMs << " ms" << std::endl;
    std::cout << "Press CTRL+C to exit." << std::endl;

    g_keyboardHook = SetWindowsHookExW(
        WH_KEYBOARD_LL,
        KeyboardHookProc,
        GetModuleHandleW(NULL),
        0);

    if (g_keyboardHook == NULL)
    {
        std::cout << "Failed to install keyboard hook." << std::endl;
        return 1;
    }

    g_timerId = SetTimer(NULL, 1, 20, NULL);

    MSG message = {};
    while (GetMessageW(&message, NULL, 0, 0) > 0)
    {
        if (message.message == WM_TIMER)
        {
            CheckHeldKeyTimeouts();
            continue;
        }

        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    FinishInlineLog();

    if (g_timerId != 0)
    {
        KillTimer(NULL, g_timerId);
        g_timerId = 0;
    }

    if (g_keyboardHook != NULL)
    {
        UnhookWindowsHookEx(g_keyboardHook);
        g_keyboardHook = NULL;
    }

    std::cout << "Program closed." << std::endl;
    return 0;
}
