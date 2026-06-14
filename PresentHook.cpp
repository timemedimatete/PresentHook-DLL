// ============================================================================
// PresentHook.cpp
// DLL de hook para IDXGISwapChain::Present / Present1 en procesos Unity D3D11.
// Objetivo: que las instancias secundarias no presenten frames reales, reduciendo
// el uso de GPU casi a 0, manteniendo la logica del juego, input y macros.
//
// Compilar: DLL x64 Release (Visual Studio Community 2022).
// Inyeccion: Sandboxie InjectDll64, o inyector manual por PID.
//
// Funcionamiento:
//   - Si la linea de comandos NO contiene "-aoo-sec", la DLL no hace nada.
//   - Si contiene "-aoo-sec", intercepta Present/Present1 y devuelve S_OK
//     sin llamar al driver grafico.
// ============================================================================

#include <windows.h>
#include <dxgi.h>
#include <dxgi1_2.h>
#include <d3d11.h>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

// Indices en las vtables COM (confirmados contra headers de Windows SDK)
constexpr size_t PRESENT_INDEX  = 8;  // IDXGISwapChain::Present
constexpr size_t PRESENT1_INDEX = 13; // IDXGISwapChain1::Present1

using Present_t  = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags);
using Present1_t = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain1* pSwapChain, UINT SyncInterval, UINT Flags, const DXGI_PRESENT_PARAMETERS* pPresentParameters);

static Present_t  g_pfnPresentOriginal  = nullptr;
static Present1_t g_pfnPresent1Original = nullptr;
static bool       g_blockPresent        = false;
static bool       g_installed           = false;
static HMODULE    g_hModule             = nullptr;

// ---------------------------------------------------------------------------
// Determina si este proceso debe bloquear Present (instancia secundaria).
// El script de lanzamiento anade "-aoo-sec" a las secundarias y "-aoo-main"
// a la principal.
// ---------------------------------------------------------------------------
static bool IsSecondaryInstance()
{
    LPWSTR pszCmd = GetCommandLineW();
    return (pszCmd != nullptr) && (wcsstr(pszCmd, L"-aoo-sec") != nullptr);
}

HRESULT STDMETHODCALLTYPE Present_Hook(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags)
{
    if (g_blockPresent)
        return S_OK;

    return g_pfnPresentOriginal(pSwapChain, SyncInterval, Flags);
}

HRESULT STDMETHODCALLTYPE Present1_Hook(IDXGISwapChain1* pSwapChain, UINT SyncInterval, UINT Flags, const DXGI_PRESENT_PARAMETERS* pPresentParameters)
{
    if (g_blockPresent)
        return S_OK;

    return g_pfnPresent1Original(pSwapChain, SyncInterval, Flags, pPresentParameters);
}

static bool HookVTableEntry(void** vtable, size_t index, void* hookFn, void** originalFn)
{
    *originalFn = vtable[index];

    DWORD oldProtect = 0;
    if (!VirtualProtect(&vtable[index], sizeof(void*), PAGE_READWRITE, &oldProtect))
        return false;

    vtable[index] = hookFn;

    VirtualProtect(&vtable[index], sizeof(void*), oldProtect, &oldProtect);
    return true;
}

// ---------------------------------------------------------------------------
// Crea un swap chain dummy para obtener la vtable global de IDXGISwapChain
// (y IDXGISwapChain1) y sobrescribe Present/Present1.
// ---------------------------------------------------------------------------
static bool InstallPresentHook()
{
    WNDCLASSEXW wc = {};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = DefWindowProcW;
    wc.hInstance     = g_hModule;
    wc.lpszClassName = L"AooPresentHookDummy";

    if (!RegisterClassExW(&wc))
    {
        if (GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
            return false;
    }

    HWND hWnd = CreateWindowExW(
        0,
        wc.lpszClassName,
        L"",
        WS_OVERLAPPEDWINDOW,
        0, 0, 1, 1,
        nullptr,
        nullptr,
        g_hModule,
        nullptr);

    if (!hWnd)
        return false;

    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount                         = 1;
    sd.BufferDesc.Width                    = 1;
    sd.BufferDesc.Height                   = 1;
    sd.BufferDesc.Format                   = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator    = 60;
    sd.BufferDesc.RefreshRate.Denominator  = 1;
    sd.BufferUsage                         = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow                        = hWnd;
    sd.SampleDesc.Count                    = 1;
    sd.SampleDesc.Quality                  = 0;
    sd.Windowed                            = TRUE;
    sd.SwapEffect                          = DXGI_SWAP_EFFECT_DISCARD;

    ID3D11Device*        pDevice     = nullptr;
    ID3D11DeviceContext* pContext    = nullptr;
    IDXGISwapChain*      pSwapChain  = nullptr;
    D3D_FEATURE_LEVEL    featureLevel = {};

    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr,
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        0,
        nullptr,
        0,
        D3D11_SDK_VERSION,
        &sd,
        &pSwapChain,
        &pDevice,
        &featureLevel,
        &pContext);

    // Fallback por si no hay GPU disponible (no deberia ocurrir en tu caso,
    // pero evita un fallo de carga).
    if (FAILED(hr))
    {
        hr = D3D11CreateDeviceAndSwapChain(
            nullptr,
            D3D_DRIVER_TYPE_WARP,
            nullptr,
            0,
            nullptr,
            0,
            D3D11_SDK_VERSION,
            &sd,
            &pSwapChain,
            &pDevice,
            &featureLevel,
            &pContext);
    }

    if (FAILED(hr) || pSwapChain == nullptr)
    {
        DestroyWindow(hWnd);
        UnregisterClassW(wc.lpszClassName, g_hModule);
        return false;
    }

    // Hook de Present en IDXGISwapChain
    void** vtable = *(void***)pSwapChain;
    HookVTableEntry(vtable, PRESENT_INDEX, Present_Hook, (void**)&g_pfnPresentOriginal);

    // Hook de Present1 en IDXGISwapChain1 (algunas versiones de Unity lo usan)
    IDXGISwapChain1* pSwapChain1 = nullptr;
    if (SUCCEEDED(pSwapChain->QueryInterface(__uuidof(IDXGISwapChain1), (void**)&pSwapChain1)))
    {
        void** vtable1 = *(void***)pSwapChain1;
        HookVTableEntry(vtable1, PRESENT1_INDEX, Present1_Hook, (void**)&g_pfnPresent1Original);
        pSwapChain1->Release();
    }

    if (pContext)
        pContext->Release();
    if (pDevice)
        pDevice->Release();
    pSwapChain->Release();

    DestroyWindow(hWnd);
    UnregisterClassW(wc.lpszClassName, g_hModule);

    g_installed = (g_pfnPresentOriginal != nullptr || g_pfnPresent1Original != nullptr);
    return g_installed;
}

static void WINAPI InitThread(LPVOID)
{
    if (!IsSecondaryInstance())
    {
        OutputDebugStringA("[PresentHook] Instancia principal (-aoo-main); no se instala hook.");
        return;
    }

    g_blockPresent = true;

    if (InstallPresentHook())
    {
        OutputDebugStringA("[PresentHook] Hook instalado; Present bloqueado para esta instancia secundaria.");
    }
    else
    {
        OutputDebugStringA("[PresentHook] ERROR: no se pudo instalar el hook de Present.");
    }
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved)
{
    switch (ul_reason_for_call)
    {
    case DLL_PROCESS_ATTACH:
        g_hModule = hModule;
        DisableThreadLibraryCalls(hModule);
        CreateThread(nullptr, 0, InitThread, nullptr, 0, nullptr);
        break;
    }
    return TRUE;
}
// Commit para activar GitHub Actions - 2026
