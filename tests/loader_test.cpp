#include <windows.h>
#include <psapi.h>
#include <stdio.h>

static int Fail(const char* msg) {
    printf("FAIL: %s\n", msg);
    return 1;
}

int wmain(int argc, wchar_t** argv) {
    if (argc < 3) return Fail("usage: loader_test <WebView2Loader.dll> <OutlookMeetJoin.dll>");

    HMODULE wv = LoadLibraryW(argv[1]);
    if (!wv) return Fail("cannot load WebView2Loader.dll");
    void* before = (void*)GetProcAddress(wv, "CreateCoreWebView2EnvironmentWithOptions");
    if (!before) return Fail("export not found");

    HMODULE omj = LoadLibraryW(argv[2]);
    if (!omj) return Fail("cannot load OutlookMeetJoin.dll");
    MODULEINFO mi = {};
    if (!GetModuleInformation(GetCurrentProcess(), omj, &mi, sizeof(mi))) return Fail("no module info");

    BYTE* after = NULL;
    for (int i = 0; i < 300 && (!after || (void*)after == before); i++) {
        Sleep(10);
        after = (BYTE*)GetProcAddress(wv, "CreateCoreWebView2EnvironmentWithOptions");
    }
    if ((void*)after == before) return Fail("export was not patched");
    if (after[0] != 0xFF || after[1] != 0x25 || *(DWORD*)(after + 2) != 0) return Fail("export does not point to a jmp trampoline");

    BYTE* target = *(BYTE**)(after + 6);
    BYTE* lo = (BYTE*)mi.lpBaseOfDll;
    if (target < lo || target >= lo + mi.SizeOfImage) return Fail("trampoline does not jump into OutlookMeetJoin.dll");

    Sleep(100);
    if ((BYTE*)GetProcAddress(wv, "CreateCoreWebView2EnvironmentWithOptions") != after) return Fail("export re-patched with a new trampoline");

    printf("ok: EAT -> trampoline %p -> hook %p (OutlookMeetJoin.dll %p..%p)\n", after, target, lo, lo + mi.SizeOfImage);
    return 0;
}
