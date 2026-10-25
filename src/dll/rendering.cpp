#include "rendering.h"

#include <Windows.h>
#include <atlbase.h>
#include <atomic>
#include <cstdio>
#include <d3d11.h>

#include "MinHook.h"
#include "glm/glm.hpp"
#include "tracy/Tracy.hpp"
#include "tracy/TracyD3D11.hpp"

#include "alienResources.h"
#include "common.h"
#include "fnTypes.h"
#include "frameConstants.h"
#include "hookedFns.h"
#include "menu.h"
#include "methodHook.h"
#include "post.h"
#include "profiler.h"
#include "reshadeBridge.h"
#include "resourceUtil.h"
#include "settings.h"
#include "shaderHooks.h"
#include "shaderRegistry.h"
#include "taa.h"
#include "vtables.h"

ID3D11Device *g_device = nullptr;
ID3D11DeviceContext *g_deferred_context = nullptr;

TracyD3D11Ctx g_tracyD3D11Ctx;
FrameConstants g_frameConstants;
AlienResources g_alienResources;

extern ShaderHandle g_sharpenPsHandle;

// ----------------------------------------------------------------------------------------------------------------

bool g_shaderHooksEnabled = false;

// Draw is also hooked with the mod off while the ReShade bridge waits for a loading screen to start DLSS 5 behind;
// Draw_hook then only notes whether the 3D scene was drawn.
static bool s_drawHooked = false;

static void updateDrawHook()
{
    const bool wanted = g_shaderHooksEnabled || ReShadeBridge::isWatchingForLoadingScreen();
    if (wanted == s_drawHooked)
    {
        return;
    }

    if (wanted)
    {
        enableMethodHook(g_d3dHooks.Draw);
    }
    else
    {
        disableMethodHook(g_d3dHooks.Draw);
    }
    s_drawHooked = wanted;
}

void enableShaderHooks()
{
    ZoneScopedN(__FUNCTION__);

    if (!g_shaderHooksEnabled)
    {
        enableMethodHook(g_d3dHooks.PSSetShader);
        enableMethodHook(g_d3dHooks.VSSetShader);

        taaEnableApiHooks();
        g_shaderHooksEnabled = true;
        updateDrawHook();
    }
}

void disableShaderHooks()
{
    ZoneScopedN(__FUNCTION__);

    if (g_shaderHooksEnabled)
    {
        disableMethodHook(g_d3dHooks.PSSetShader);
        disableMethodHook(g_d3dHooks.VSSetShader);

        taaDisableApiHooks();

        g_shaderHooksEnabled = false;
        updateDrawHook();
    }
}

// The hotkeys are read on a background thread, but flipping the hooks there can land between a Map and its Unmap
// or between the passes of one frame and leave the TAA state half-applied (#70). The thread only records the
// request, and Present applies it on the render thread between frames.
static std::atomic<int> g_requestedShaderHooks{-1};

void requestShaderHooks(bool enable)
{
    g_requestedShaderHooks = enable ? 1 : 0;
}

unsigned g_settingsWarnings = 0;
static unsigned s_framesWithoutMotionPass = 0;
static unsigned s_framesWithoutSharpenPass = 0;

// The camera motion pass only runs with Motion Blur on, and the SMAA pass the sharpen filter replaces only with SMAA
// T1x. Without them TAA has no velocity (smearing) or the frame is never finished. Only frames that rendered the 3D
// scene count, and a pass has to be missing for a couple of seconds, so menus and loading screens don't warn.
static void checkVideoSettings()
{
    constexpr unsigned threshold = 120;

    if (g_frameConstants.scenePassThisFrame)
    {
        s_framesWithoutMotionPass = g_frameConstants.motionPassThisFrame ? 0 : s_framesWithoutMotionPass + 1;
        s_framesWithoutSharpenPass = g_frameConstants.sharpenPassThisFrame ? 0 : s_framesWithoutSharpenPass + 1;
    }

    g_settingsWarnings = 0;
    if (g_shaderHooksEnabled && s_framesWithoutMotionPass > threshold)
    {
        g_settingsWarnings |= SettingsWarning_MotionBlur;
    }
    if (g_shaderHooksEnabled && s_framesWithoutSharpenPass > threshold)
    {
        g_settingsWarnings |= SettingsWarning_Smaa;
    }

    g_frameConstants.scenePassThisFrame = false;
    g_frameConstants.motionPassThisFrame = false;
    g_frameConstants.sharpenPassThisFrame = false;
}

static void applyRequestedShaderHooks()
{
    const int requested = g_requestedShaderHooks.exchange(-1);
    if (requested < 0 || (requested == 1) == g_shaderHooksEnabled)
    {
        return;
    }

    if (requested == 1)
    {
        enableShaderHooks();
    }
    else
    {
        disableShaderHooks();
    }

    g_alienResources.mappedCbDefaultXSC = nullptr;
    g_alienResources.mappedCbDefaultVSC = nullptr;
    g_alienResources.mappedCbDefaultPSC = nullptr;
    g_frameConstants.taaRanThisFrame = false;
    g_frameConstants.prevViewProjNoJitter = g_frameConstants.currViewProjNoJitter;
    g_frameConstants.resetTaaHistory = requested == 1;
    s_framesWithoutMotionPass = 0;
    s_framesWithoutSharpenPass = 0;
}

// ----------------------------------------------------------------------------------------------------------------

static void finishFrame()
{
    ZoneScopedN(__FUNCTION__);

    g_frameConstants.taaRanThisFrame = false;
    ShaderRegistry::releaseUnused();
    g_frameConstants.prevViewProjNoJitter = g_frameConstants.currViewProjNoJitter;

    FrameMark;
}

// ----------------------------------------------------------------------------------------------------------------

HRESULT WINAPI CreateSamplerState_hook(ID3D11Device *device, const D3D11_SAMPLER_DESC *pSamplerDesc,
                                       ID3D11SamplerState **ppSamplerState)
{
    ZoneScopedN(__FUNCTION__);

    D3D11_SAMPLER_DESC desc = *pSamplerDesc;

    // Only g-buffer pass objects have anisotropy, and those are also the ones that need the bias.
    if (desc.MaxAnisotropy > 1.0f)
    {
        desc.MipLODBias -= 0.5f;
    }

    return g_d3dHookOrig.CreateSamplerState(device, &desc, ppSamplerState);
}

// ----------------------------------------------------------------------------------------------------------------

//const char *const s_overallString = "Overall Draw";

HRESULT WINAPI Draw_hook(ID3D11DeviceContext *context, UINT VertexCount, UINT StartVertexLocation)
{
    ZoneScopedN(__FUNCTION__);
    //TracyD3D11Zone(g_tracyD3D11Ctx, "Overall Draw");

    HRESULT result = NO_ERROR;
    auto origDrawFn = [&]() { result = g_d3dHookOrig.Draw(context, VertexCount, StartVertexLocation); };
#if !defined(ALIASISOLATION_NO_TAA_PASS) && !defined(ALIASISOLATION_NO_CA_PASS)
    CComPtr<ID3D11VertexShader> currentVs = nullptr;
    CComPtr<ID3D11PixelShader> currentPs = nullptr;
    context->VSGetShader(&currentVs.p, nullptr, nullptr);
    context->PSGetShader(&currentPs.p, nullptr, nullptr);

    // With the mod off, Draw is only hooked to watch for the 3D scene (see updateDrawHook).
    if (!g_shaderHooksEnabled)
    {
        if (isScenePassDraw(currentVs, currentPs))
        {
            g_frameConstants.scenePassThisFrame = true;
        }
        origDrawFn();
        return result;
    }

    if (currentPs == g_alienResources.cameraMotionPs && g_alienResources.cameraMotionPs != nullptr)
    {
        CComPtr<ID3D11ShaderResourceView> depthSrv = nullptr;
        context->PSGetShaderResources(8, 1, &depthSrv);
        g_alienResources.depthSrv = depthSrv;
        g_frameConstants.motionPassThisFrame = true;
    }
#endif
#ifndef ALIASISOLATION_NO_TAA_PASS
    taaOnDraw(context, currentVs, currentPs);
#endif
#ifndef ALIASISOLATION_NO_CA_PASS
    if (caOnDraw(context, currentPs, origDrawFn))
    {
        g_frameConstants.sharpenPassThisFrame = true;
        finishFrame();

        return result;
    }
#endif
    origDrawFn();

    return result;
}

// ----------------------------------------------------------------------------------------------------------------

void STDMETHODCALLTYPE PSSetShader_hook(ID3D11DeviceContext *context,
                                        /* [annotation] */
                                        _In_opt_ ID3D11PixelShader *pPixelShader,
                                        /* [annotation] */
                                        _In_reads_opt_(NumClassInstances) ID3D11ClassInstance *const *ppClassInstances,
                                        UINT NumClassInstances)
{
    ZoneScopedN(__FUNCTION__);

    ID3D11PixelShader *shader = pPixelShader;
#ifndef ALIASISOLATION_NO_PS_OVERRIDES
    auto replaced = getReplacedShader(pPixelShader);
    if (replaced == g_sharpenPsHandle && g_sharpenPsHandle.isValid())
    {
        modifySharpenPass(context);
        shader = ShaderRegistry::getPs(replaced);
    }
#endif

    g_d3dHookOrig.PSSetShader(context, shader, ppClassInstances, NumClassInstances);

#ifndef ALIASISOLATION_NO_PS_OVERRIDES
    if (g_alienResources.cameraMotionPs == pPixelShader && g_alienResources.cameraMotionPs != nullptr)
    {
        g_alienResources.cbDefaultPSC = nullptr;
        context->PSGetConstantBuffers(2, 1, &g_alienResources.cbDefaultPSC);

        CComPtr<ID3D11RenderTargetView> rtv;
        context->OMGetRenderTargets(1, &rtv.p, nullptr);
        g_alienResources.velocityRtv = rtv;

        CComPtr<ID3D11Resource> rtRes = nullptr;
        if (rtv)
        {
            rtv->GetResource(&rtRes.p);
        }
        else
        {
            LOG_MSG("[aliasIsolation::rendering] FATAL ERROR - rtv->GetResource(&rtRes.p) - Failed to get resource "
                    "from render target view!\n",
                    "");
            DebugBreak();
        }

        g_alienResources.velocitySrv = srvFromTex((ID3D11Texture2D *)rtRes.p);
    }
#endif
}

// ----------------------------------------------------------------------------------------------------------------

void STDMETHODCALLTYPE VSSetShader_hook(ID3D11DeviceContext *context,
                                        /* [annotation] */
                                        _In_opt_ ID3D11VertexShader *pVertexShader,
                                        /* [annotation] */
                                        _In_reads_opt_(NumClassInstances) ID3D11ClassInstance *const *ppClassInstances,
                                        UINT NumClassInstances)
{
    ZoneScopedN(__FUNCTION__);

    ID3D11VertexShader *shader = pVertexShader;
#ifndef ALIASISOLATION_NO_VS_OVERRIDES
    auto replaced = getReplacedShader(pVertexShader);
    if (replaced.isValid())
    {
        shader = ShaderRegistry::getVs(replaced);
    }
#endif

    g_d3dHookOrig.VSSetShader(context, shader, ppClassInstances, NumClassInstances);

#ifndef ALIASISOLATION_NO_VS_OVERRIDES
    if (g_alienResources.smaaVertexShader == pVertexShader && g_alienResources.smaaVertexShader != nullptr)
    {
        g_alienResources.cbDefaultXSC = nullptr;
        context->VSGetConstantBuffers(0, 1, &g_alienResources.cbDefaultXSC);
    }
    else if (g_alienResources.rgbmEncodeVs == pVertexShader && g_alienResources.rgbmEncodeVs != nullptr)
    {
        g_alienResources.cbDefaultVSC = nullptr;
        context->VSGetConstantBuffers(1, 1, &g_alienResources.cbDefaultVSC);
    }
#endif
}

// ----------------------------------------------------------------------------------------------------------------

HRESULT WINAPI ResizeBuffers_hook(IDXGISwapChain *swapChain, UINT BufferCount, UINT Width, UINT Height,
                                  DXGI_FORMAT NewFormat, UINT SwapChainFlags)
{
    ZoneScopedN(__FUNCTION__);

    g_alienResources.cbDefaultXSC = nullptr;
    g_alienResources.cbDefaultVSC = nullptr;
    g_alienResources.cbDefaultPSC = nullptr;
    g_alienResources.mappedCbDefaultXSC = nullptr;
    g_alienResources.mappedCbDefaultVSC = nullptr;
    g_alienResources.mappedCbDefaultPSC = nullptr;
    g_alienResources.velocitySrv = nullptr;
    g_alienResources.mainTexView = nullptr;

    g_frameConstants.screenWidth = Width;
    g_frameConstants.screenHeight = Height;

    releaseResourceViews();

    Menu::CleanupRenderTarget();

    HRESULT hr = g_d3dHookOrig.ResizeBuffers(swapChain, BufferCount, Width, Height, NewFormat, SwapChainFlags);

    Menu::CreateRenderTarget();

    return hr;
}

// ----------------------------------------------------------------------------------------------------------------

HRESULT WINAPI Present_hook(IDXGISwapChain *swapChain, UINT SyncInterval, UINT Flags)
{
    ZoneScopedN(__FUNCTION__);

    {
        ZoneScopedN("Present_hook Menu::DrawMenu");
        ReShadeBridge::onGameFrame(g_frameConstants.scenePassThisFrame, g_shaderHooksEnabled);
        checkVideoSettings();
        ReShadeBridge::update();
        Menu::DrawMenu();
    }

    const HRESULT ret = g_d3dHookOrig.Present(swapChain, SyncInterval, Flags);

    TracyD3D11Collect(g_tracyD3D11Ctx);

    applyRequestedShaderHooks();
    updateDrawHook();

    return ret;
}

// ----------------------------------------------------------------------------------------------------------------

D3D11CreateDeviceAndSwapChain_t D3D11CreateDeviceAndSwapChain_orig = nullptr;

HRESULT WINAPI D3D11CreateDeviceAndSwapChain_hook(void *pAdapter, D3D_DRIVER_TYPE DriverType, HMODULE Software,
                                                  UINT Flags, const void *pFeatureLevels, UINT FeatureLevels,
                                                  UINT SDKVersion, const DXGI_SWAP_CHAIN_DESC *pSwapChainDesc,
                                                  IDXGISwapChain **ppSwapChain, ID3D11Device **ppDevice,
                                                  void *pFeatureLevel, ID3D11DeviceContext **ppImmediateContext)
{
    ZoneScopedN(__FUNCTION__);

    HRESULT res = D3D11CreateDeviceAndSwapChain_orig(pAdapter, DriverType, Software, Flags, pFeatureLevels,
                                                     FeatureLevels, SDKVersion, pSwapChainDesc, ppSwapChain, ppDevice,
                                                     pFeatureLevel, ppImmediateContext);

    // We only care about the first call to this function. Other hook-based mods will sometimes create their own
    // devices, but we don't care about those, and don't want to inject into those.
    if (g_device)
    {
        return res;
    }

    g_frameConstants.screenWidth = pSwapChainDesc->BufferDesc.Width;
    g_frameConstants.screenHeight = pSwapChainDesc->BufferDesc.Height;

    g_device = *ppDevice;

    DX_CHECK(g_device->CreateDeferredContext(0, &g_deferred_context));

    g_tracyD3D11Ctx = TracyD3D11Context(g_device, *ppImmediateContext);

#define HOOK_DEVICE_METHOD(NAME)                                                                                       \
    g_d3dHooks.NAME =                                                                                                  \
        hookMethod(*(void ***)*ppDevice, int(D3D11DeviceVTbl::NAME), &NAME##_hook, (void **)&g_d3dHookOrig.NAME)
#define HOOK_CONTEXT_METHOD(NAME)                                                                                      \
    g_d3dHooks.NAME = hookMethod(*(void ***)*ppImmediateContext, int(D3D11DeviceContextVTbl::NAME), &NAME##_hook,      \
                                 (void **)&g_d3dHookOrig.NAME)
#define HOOK_SWAPCHAIN_METHOD(NAME)                                                                                    \
    g_d3dHooks.NAME =                                                                                                  \
        hookMethod(*(void ***)*ppSwapChain, int(IDXGISwapChainVtbl::NAME), &NAME##_hook, (void **)&g_d3dHookOrig.NAME)

    HOOK_DEVICE_METHOD(CreatePixelShader);
    HOOK_DEVICE_METHOD(CreateVertexShader);
    HOOK_DEVICE_METHOD(CreateSamplerState);

    HOOK_CONTEXT_METHOD(PSSetShader);
    HOOK_CONTEXT_METHOD(VSSetShader);
    HOOK_CONTEXT_METHOD(Draw);

    HOOK_SWAPCHAIN_METHOD(ResizeBuffers);
    HOOK_SWAPCHAIN_METHOD(Present);

    taaHookApi(*ppImmediateContext);

#undef HOOK_DEVICE_METHOD
#undef HOOK_CONTEXT_METHOD
#undef HOOK_SWAPCHAIN_METHOD

    enableShaderHooks();

    enableMethodHook(g_d3dHooks.CreatePixelShader);
    enableMethodHook(g_d3dHooks.CreateVertexShader);
    enableMethodHook(g_d3dHooks.CreateSamplerState);
    enableMethodHook(g_d3dHooks.ResizeBuffers);
    enableMethodHook(g_d3dHooks.Present);

#if _DEBUG
    if (AllocConsole())
    {
        freopen("CONIN$", "r", stdin);
        freopen("CONOUT$", "w", stdout);
        freopen("CONOUT$", "w", stderr);
    }
#endif

    // Initialise the Alias Isolation menu.
    Menu::InitMenu(**ppSwapChain);

    return res;
}

// ----------------------------------------------------------------------------------------------------------------

void hookRendering()
{
    ZoneScopedN(__FUNCTION__);

    int minhookRet;

    HMODULE hModule = GetModuleHandleA("d3d11");
    if (hModule)
    {
        g_d3dHooks.D3D11CreateDeviceAndSwapChain = (LPVOID)GetProcAddress(hModule, "D3D11CreateDeviceAndSwapChain");
        if (g_d3dHooks.D3D11CreateDeviceAndSwapChain)
        {
            minhookRet = MH_CreateHook(g_d3dHooks.D3D11CreateDeviceAndSwapChain, &D3D11CreateDeviceAndSwapChain_hook,
                                       (LPVOID *)&D3D11CreateDeviceAndSwapChain_orig);
        }
        else
        {
            minhookRet = MH_ERROR_FUNCTION_NOT_FOUND;
        }
    }
    else
    {
        // ret = MH_ERROR_MODULE_NOT_FOUND;
        LOG_MSG("[aliasIsolation::rendering] GetModuleHandleA(\"d3d11\") failed: MH_ERROR_MODULE_NOT_FOUND\n", "");
        return;
    }

    if (minhookRet != MH_OK)
    {
        char buf[256];
        sprintf_s(buf, "MH_CreateHookApi() failed: %d\n", minhookRet);
        MessageBoxA(NULL, buf, NULL, NULL);
        return;
    }

    MH_CHECK(MH_EnableHook(g_d3dHooks.D3D11CreateDeviceAndSwapChain));
    LOG_MSG("[aliasIsolation::rendering] MH_EnableHook(g_d3dHooks.D3d11CreateDeviceAndSwapChain)\n", "");

    ShaderRegistry::startCompilerThread();
    LOG_MSG("[aliasIsolation::rendering] ShaderRegistry::startCompilerThread()\n", "");
}

void unhookRendering()
{
    ShaderRegistry::stopCompilerThread();
    disableShaderHooks();
    disableMethodHook(g_d3dHooks.Draw);
    MH_DisableHook(g_d3dHooks.D3D11CreateDeviceAndSwapChain);
    disableMethodHook(g_d3dHooks.CreatePixelShader);
    disableMethodHook(g_d3dHooks.CreateVertexShader);
    disableMethodHook(g_d3dHooks.CreateSamplerState);
    disableMethodHook(g_d3dHooks.ResizeBuffers);

    TracyD3D11Destroy(g_tracyD3D11Ctx);
}
