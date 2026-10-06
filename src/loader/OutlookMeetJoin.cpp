#include <windows.h>
#include <tlhelp32.h>
#include <wrl.h>
#include <string>
#include "WebView2.h"

using namespace Microsoft::WRL;

static const wchar_t* kDataDir = L"\\OutlookMeetJoin\\";

static bool DataPath(const wchar_t* name, wchar_t* out, DWORD cap) {
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", out, cap);
    if (!n || n >= cap) return false;
    return lstrlenW(out) + lstrlenW(kDataDir) + lstrlenW(name) < (int)cap
        && lstrcatW(out, kDataDir) && lstrcatW(out, name);
}

static void LogLine(const wchar_t* text) {
    wchar_t path[MAX_PATH] = {0};
    if (!DataPath(L"loader.log", path, MAX_PATH)) return;
    HANDLE h = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t line[512];
    _snwprintf_s(line, 512, _TRUNCATE, L"[%02d:%02d:%02d.%03d pid=%lu] %s\r\n",
               st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, GetCurrentProcessId(), text);
    char utf8[1024];
    int len = WideCharToMultiByte(CP_UTF8, 0, line, -1, utf8, sizeof(utf8), NULL, NULL);
    DWORD written = 0;
    if (len > 1) WriteFile(h, utf8, (DWORD)(len - 1), &written, NULL);
    CloseHandle(h);
}

static bool DataFileExists(const wchar_t* name) {
    wchar_t path[MAX_PATH] = {0};
    if (!DataPath(name, path, MAX_PATH)) return false;
    return GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES;
}

static std::wstring LoadPayload(void) {
    wchar_t path[MAX_PATH] = {0};
    if (!DataPath(L"inject.js", path, MAX_PATH)) return L"";
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) { LogLine(L"payload: inject.js not found"); return L""; }
    DWORD size = GetFileSize(h, NULL);
    if (size == INVALID_FILE_SIZE || size == 0 || size > 512 * 1024) { CloseHandle(h); LogLine(L"payload: empty or too large"); return L""; }
    std::string buf(size, '\0');
    DWORD read = 0;
    BOOL ok = ReadFile(h, &buf[0], size, &read, NULL);
    CloseHandle(h);
    if (!ok || read == 0) return L"";
    buf.resize(read);
    int wlen = MultiByteToWideChar(CP_UTF8, 0, buf.data(), (int)buf.size(), NULL, 0);
    if (wlen <= 0) return L"";
    std::wstring out(wlen, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, buf.data(), (int)buf.size(), &out[0], wlen);
    return out;
}

static IMAGE_NT_HEADERS* NtHeaders(BYTE* base) {
    IMAGE_DOS_HEADER* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return NULL;
    IMAGE_NT_HEADERS* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    return nt->Signature == IMAGE_NT_SIGNATURE ? nt : NULL;
}

static SRWLOCK g_patchLock = SRWLOCK_INIT;

struct PatchGuard {
    PatchGuard() { AcquireSRWLockExclusive(&g_patchLock); }
    ~PatchGuard() { ReleaseSRWLockExclusive(&g_patchLock); }
};

static bool WriteProtected(void* addr, const void* value, SIZE_T size) {
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(addr, &mbi, sizeof(mbi))) return false;
    const DWORD exec = PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    DWORD oldProt = 0;
    if (!VirtualProtect(addr, size, (mbi.Protect & exec) ? PAGE_EXECUTE_READWRITE : PAGE_READWRITE, &oldProt)) return false;
    memcpy(addr, value, size);
    if (!VirtualProtect(addr, size, oldProt, &oldProt)) LogLine(L"patch: could not restore page protection");
    return true;
}

static bool SwapPointer(ULONG_PTR* slot, void* newFunc, void** oldFunc) {
    PatchGuard guard;
    if ((void*)*slot == newFunc) return false;
    void* prev = (void*)*slot;
    if (!WriteProtected(slot, &newFunc, sizeof(void*))) return false;
    if (oldFunc && !*oldFunc) *oldFunc = prev;
    return true;
}

static bool PatchThunks(BYTE* base, IMAGE_THUNK_DATA* names, IMAGE_THUNK_DATA* addrs, const char* funcName, void* newFunc, void** oldFunc) {
    for (; names->u1.Function; ++names, ++addrs) {
        if (names->u1.Ordinal & IMAGE_ORDINAL_FLAG) continue;
        IMAGE_IMPORT_BY_NAME* ibn = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
        if (strcmp((const char*)ibn->Name, funcName) != 0) continue;
        return SwapPointer(&addrs->u1.Function, newFunc, oldFunc);
    }
    return false;
}

static bool PatchIAT(HMODULE hMod, const char* libName, const char* funcName, void* newFunc, void** oldFunc) {
    BYTE* base = (BYTE*)hMod;
    IMAGE_NT_HEADERS* nt = hMod ? NtHeaders(base) : NULL;
    if (!nt) return false;
    DWORD rva = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
    if (!rva) return false;
    for (IMAGE_IMPORT_DESCRIPTOR* imp = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + rva); imp->Name; ++imp) {
        if (_stricmp((const char*)(base + imp->Name), libName) != 0 || !imp->OriginalFirstThunk) continue;
        if (PatchThunks(base, reinterpret_cast<IMAGE_THUNK_DATA*>(base + imp->OriginalFirstThunk),
                        reinterpret_cast<IMAGE_THUNK_DATA*>(base + imp->FirstThunk), funcName, newFunc, oldFunc)) return true;
    }
    return false;
}

static bool PatchDelayIAT(HMODULE hMod, const char* libName, const char* funcName, void* newFunc, void** oldFunc) {
    BYTE* base = (BYTE*)hMod;
    IMAGE_NT_HEADERS* nt = hMod ? NtHeaders(base) : NULL;
    if (!nt) return false;
    DWORD rva = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT].VirtualAddress;
    if (!rva) return false;
    for (IMAGE_DELAYLOAD_DESCRIPTOR* d = reinterpret_cast<IMAGE_DELAYLOAD_DESCRIPTOR*>(base + rva); d->DllNameRVA; ++d) {
        if (!d->Attributes.RvaBased || _stricmp((const char*)(base + d->DllNameRVA), libName) != 0) continue;
        if (PatchThunks(base, reinterpret_cast<IMAGE_THUNK_DATA*>(base + d->ImportNameTableRVA),
                        reinterpret_cast<IMAGE_THUNK_DATA*>(base + d->ImportAddressTableRVA), funcName, newFunc, oldFunc)) return true;
    }
    return false;
}

static BYTE* g_eatBase = NULL;
static BYTE* g_eatTramp = NULL;

static BYTE* MakeTrampoline(BYTE* base, DWORD imageSize, void* target) {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    ULONG_PTR gran = si.dwAllocationGranularity;
    ULONG_PTR limit = (ULONG_PTR)base + 0x7FFF0000;
    for (ULONG_PTR p = ((ULONG_PTR)base + imageSize + gran - 1) & ~(gran - 1); p < limit; p += gran) {
        BYTE* m = (BYTE*)VirtualAlloc((void*)p, si.dwPageSize, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (!m) continue;
        m[0] = 0xFF;
        m[1] = 0x25;
        *(DWORD*)(m + 2) = 0;
        *(void**)(m + 6) = target;
        DWORD oldProt = 0;
        VirtualProtect(m, si.dwPageSize, PAGE_EXECUTE_READ, &oldProt);
        FlushInstructionCache(GetCurrentProcess(), m, 14);
        return m;
    }
    return NULL;
}

static bool PatchEAT(HMODULE hMod, const char* funcName, void* newFunc) {
    BYTE* base = (BYTE*)hMod;
    IMAGE_NT_HEADERS* nt = hMod ? NtHeaders(base) : NULL;
    if (!nt) return false;
    if (g_eatBase && g_eatBase != base) return false;
    DWORD rva = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].VirtualAddress;
    if (!rva) return false;
    IMAGE_EXPORT_DIRECTORY* exp = reinterpret_cast<IMAGE_EXPORT_DIRECTORY*>(base + rva);
    DWORD* names = reinterpret_cast<DWORD*>(base + exp->AddressOfNames);
    WORD* ords = reinterpret_cast<WORD*>(base + exp->AddressOfNameOrdinals);
    DWORD* funcs = reinterpret_cast<DWORD*>(base + exp->AddressOfFunctions);
    for (DWORD i = 0; i < exp->NumberOfNames; i++) {
        if (strcmp((const char*)(base + names[i]), funcName) != 0) continue;
        DWORD* entry = &funcs[ords[i]];
        PatchGuard guard;
        if (g_eatTramp && (ULONG_PTR)base + *entry == (ULONG_PTR)g_eatTramp) return false;
        if (!g_eatTramp) {
            g_eatTramp = MakeTrampoline(base, nt->OptionalHeader.SizeOfImage, newFunc);
            if (!g_eatTramp) return false;
            g_eatBase = base;
        }
        DWORD rvaTramp = (DWORD)((ULONG_PTR)g_eatTramp - (ULONG_PTR)base);
        return WriteProtected(entry, &rvaTramp, sizeof(DWORD));
    }
    return false;
}

static bool PatchVtbl(void** vtbl, int index, void* newFunc, void** oldFunc) {
    if (!vtbl) return false;
    PatchGuard guard;
    if (vtbl[index] == newFunc) return false;
    if (oldFunc && *oldFunc && vtbl[index] != *oldFunc) return false;
    void* prev = vtbl[index];
    if (!WriteProtected(&vtbl[index], &newFunc, sizeof(void*))) return false;
    if (oldFunc && !*oldFunc) *oldFunc = prev;
    return true;
}

typedef HRESULT(STDMETHODCALLTYPE* PFN_InvokeEnv)(ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler*, HRESULT, ICoreWebView2Environment*);
typedef HRESULT(STDMETHODCALLTYPE* PFN_CreateCtrl)(ICoreWebView2Environment*, HWND, ICoreWebView2CreateCoreWebView2ControllerCompletedHandler*);
typedef HRESULT(STDMETHODCALLTYPE* PFN_InvokeCtrl)(ICoreWebView2CreateCoreWebView2ControllerCompletedHandler*, HRESULT, ICoreWebView2Controller*);
typedef HRESULT(STDAPICALLTYPE* PFN_CreateEnv)(PCWSTR, PCWSTR, ICoreWebView2EnvironmentOptions*, ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler*);

static PFN_InvokeEnv  g_origInvokeEnv = NULL;
static PFN_CreateCtrl g_origCreateCtrl = NULL;
static PFN_InvokeCtrl g_origInvokeCtrl = NULL;
static PFN_CreateEnv  g_origCreateEnv = NULL;

static void Inject(ICoreWebView2* webview) {
    if (DataFileExists(L"debug")) {
        ComPtr<ICoreWebView2Settings> settings;
        if (SUCCEEDED(webview->get_Settings(&settings)) && settings) {
            settings->put_AreDevToolsEnabled(TRUE);
            webview->OpenDevToolsWindow();
            LogLine(L"debug: devtools enabled");
        }
    }
    std::wstring js = LoadPayload();
    if (js.empty()) return;
    HRESULT a = webview->AddScriptToExecuteOnDocumentCreated(js.c_str(), NULL);
    HRESULT b = webview->ExecuteScript(js.c_str(), NULL);
    LogLine(SUCCEEDED(a) ? L"inject: AddScriptToExecuteOnDocumentCreated OK" : L"inject: AddScriptToExecuteOnDocumentCreated FAILED");
    LogLine(SUCCEEDED(b) ? L"inject: ExecuteScript OK" : L"inject: ExecuteScript FAILED");
}

static HRESULT STDMETHODCALLTYPE Hook_InvokeCtrl(ICoreWebView2CreateCoreWebView2ControllerCompletedHandler* self, HRESULT hr, ICoreWebView2Controller* controller) {
    if (SUCCEEDED(hr) && controller) {
        ComPtr<ICoreWebView2> webview;
        if (SUCCEEDED(controller->get_CoreWebView2(&webview)) && webview) Inject(webview.Get());
    }
    return g_origInvokeCtrl ? g_origInvokeCtrl(self, hr, controller) : S_OK;
}

static HRESULT STDMETHODCALLTYPE Hook_CreateCtrl(ICoreWebView2Environment* env, HWND hwnd, ICoreWebView2CreateCoreWebView2ControllerCompletedHandler* handler) {
    if (handler && PatchVtbl(*(void***)handler, 3, (void*)Hook_InvokeCtrl, (void**)&g_origInvokeCtrl)) LogLine(L"hook: controller handler");
    return g_origCreateCtrl ? g_origCreateCtrl(env, hwnd, handler) : E_FAIL;
}

static HRESULT STDMETHODCALLTYPE Hook_InvokeEnv(ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler* self, HRESULT hr, ICoreWebView2Environment* env) {
    if (SUCCEEDED(hr) && env && PatchVtbl(*(void***)env, 3, (void*)Hook_CreateCtrl, (void**)&g_origCreateCtrl)) LogLine(L"hook: environment");
    return g_origInvokeEnv ? g_origInvokeEnv(self, hr, env) : S_OK;
}

static HRESULT STDAPICALLTYPE Hook_CreateEnv(PCWSTR browserFolder, PCWSTR userDataFolder, ICoreWebView2EnvironmentOptions* options,
                                             ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler* handler) {
    if (handler && PatchVtbl(*(void***)handler, 3, (void*)Hook_InvokeEnv, (void**)&g_origInvokeEnv)) LogLine(L"hook: environment handler");
    if (!g_origCreateEnv) { LogLine(L"hook: original function missing"); return E_FAIL; }
    return g_origCreateEnv(browserFolder, userDataFolder, options, handler);
}

static bool EnsureOriginal(void) {
    if (g_origCreateEnv) return true;
    HMODULE h = GetModuleHandleW(L"WebView2Loader.dll");
    if (!h) h = LoadLibraryW(L"WebView2Loader.dll");
    if (!h) return false;
    g_origCreateEnv = (PFN_CreateEnv)GetProcAddress(h, "CreateCoreWebView2EnvironmentWithOptions");
    if (g_origCreateEnv) LogLine(L"original function resolved");
    return g_origCreateEnv != NULL;
}

static void PatchAllModules(void) {
    if (!EnsureOriginal()) return;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
    if (snap == INVALID_HANDLE_VALUE) return;
    MODULEENTRY32W me;
    ZeroMemory(&me, sizeof(me));
    me.dwSize = sizeof(me);
    wchar_t msg[300];
    if (Module32FirstW(snap, &me)) {
        do {
            if (PatchIAT(me.hModule, "WebView2Loader.dll", "CreateCoreWebView2EnvironmentWithOptions", (void*)Hook_CreateEnv, NULL)) {
                _snwprintf_s(msg, 300, _TRUNCATE, L"IAT hooked in %s", me.szModule);
                LogLine(msg);
            }
            if (PatchDelayIAT(me.hModule, "WebView2Loader.dll", "CreateCoreWebView2EnvironmentWithOptions", (void*)Hook_CreateEnv, NULL)) {
                _snwprintf_s(msg, 300, _TRUNCATE, L"delay-IAT hooked in %s", me.szModule);
                LogLine(msg);
            }
            if (_wcsicmp(me.szModule, L"WebView2Loader.dll") == 0 &&
                PatchEAT(me.hModule, "CreateCoreWebView2EnvironmentWithOptions", (void*)Hook_CreateEnv)) {
                LogLine(L"EAT hooked in WebView2Loader.dll");
            }
        } while (Module32NextW(snap, &me));
    }
    CloseHandle(snap);
}

static DWORD WINAPI Worker(LPVOID) {
    for (int i = 0; i < 400; i++) { PatchAllModules(); Sleep(1); }
    for (int i = 0; i < 700; i++) { PatchAllModules(); Sleep(20); }
    for (int i = 0; i < 1200; i++) { PatchAllModules(); Sleep(500); }
    LogLine(L"module watch finished");
    return 0;
}

typedef struct _RTL_VERIFIER_THUNK_DESCRIPTOR {
    PCHAR ThunkName;
    PVOID ThunkOldAddress;
    PVOID ThunkNewAddress;
} RTL_VERIFIER_THUNK_DESCRIPTOR, *PRTL_VERIFIER_THUNK_DESCRIPTOR;

typedef struct _RTL_VERIFIER_DLL_DESCRIPTOR {
    PWCHAR DllName;
    ULONG  DllFlags;
    PVOID  DllAddress;
    PRTL_VERIFIER_THUNK_DESCRIPTOR DllThunks;
} RTL_VERIFIER_DLL_DESCRIPTOR, *PRTL_VERIFIER_DLL_DESCRIPTOR;

typedef void (NTAPI* RTL_VERIFIER_DLL_LOAD_CALLBACK)(PWSTR, PVOID, SIZE_T, PVOID);
typedef void (NTAPI* RTL_VERIFIER_DLL_UNLOAD_CALLBACK)(PWSTR, PVOID, SIZE_T, PVOID);
typedef void (NTAPI* RTL_VERIFIER_NTDLLHEAPFREE_CALLBACK)(PVOID, SIZE_T);

typedef struct _RTL_VERIFIER_PROVIDER_DESCRIPTOR {
    ULONG Length;
    PRTL_VERIFIER_DLL_DESCRIPTOR ProviderDlls;
    RTL_VERIFIER_DLL_LOAD_CALLBACK ProviderDllLoadCallback;
    RTL_VERIFIER_DLL_UNLOAD_CALLBACK ProviderDllUnloadCallback;
    PWSTR VerifierImage;
    ULONG VerifierFlags;
    ULONG VerifierDebug;
    PVOID RtlpGetStackTraceAddress;
    PVOID RtlpDebugPageHeapCreate;
    PVOID RtlpDebugPageHeapDestroy;
    RTL_VERIFIER_NTDLLHEAPFREE_CALLBACK ProviderNtdllHeapFreeCallback;
} RTL_VERIFIER_PROVIDER_DESCRIPTOR, *PRTL_VERIFIER_PROVIDER_DESCRIPTOR;

#ifndef DLL_PROCESS_VERIFIER
#define DLL_PROCESS_VERIFIER 4
#endif

static RTL_VERIFIER_DLL_DESCRIPTOR g_noHooks = {0};
static RTL_VERIFIER_PROVIDER_DESCRIPTOR g_desc = {
    sizeof(RTL_VERIFIER_PROVIDER_DESCRIPTOR),
    &g_noHooks,
    [](PWSTR, PVOID, SIZE_T, PVOID) {},
    [](PWSTR, PVOID, SIZE_T, PVOID) {},
    NULL, 0, 0,
    NULL, NULL, NULL,
    [](PVOID, SIZE_T) {}
};

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved) {
    if (fdwReason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hinstDLL);
        LogLine(L"=== OutlookMeetJoin loaded ===");
        HANDLE t = CreateThread(NULL, 0, Worker, NULL, 0, NULL);
        if (t) CloseHandle(t);
    } else if (fdwReason == DLL_PROCESS_VERIFIER && lpvReserved) {
        *(PVOID*)lpvReserved = &g_desc;
    }
    return TRUE;
}
