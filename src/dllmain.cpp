#include <Windows.h>
#include "../src/hook/hook.h"

static DWORD WINAPI MainThread(LPVOID)
{
    Hook::Init();
    return 0;
}

BOOL APIENTRY DllMain(HMODULE mod, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(mod);
        CreateThread(nullptr, 0, MainThread, nullptr, 0, nullptr);
    }
    return TRUE;
}