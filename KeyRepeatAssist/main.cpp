#include <windows.h>
#include "KeyRepeatAssist.h"

int APIENTRY wWinMain(
    HINSTANCE instanceHandle,
    HINSTANCE previousInstanceHandle,
    LPWSTR commandLine,
    int commandShow)
{
    UNREFERENCED_PARAMETER(previousInstanceHandle);
    UNREFERENCED_PARAMETER(commandLine);
    UNREFERENCED_PARAMETER(commandShow);

    return KeyRepeatAssist::Run(instanceHandle);
}
