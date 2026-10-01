#include <Windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>

#include "../lib/MinHook/MinHook.h"
#include "../lib/ImGui/imgui.h"
#include "../lib/ImGui/imgui_impl_win32.h"
#include "../lib/ImGui/imgui_impl_dx12.h"

#include "../hook/hook.h"
#include "../menu/menu.h"

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

using PresentFn = HRESULT(__stdcall*)(IDXGISwapChain3*, UINT, UINT);
using ResizeFn = HRESULT(__stdcall*)(IDXGISwapChain3*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
using ExecFn = void(__stdcall*)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);

static PresentFn oPresent = nullptr;
static ResizeFn  oResize = nullptr;
static ExecFn    oExec = nullptr;
static WNDPROC   oWndProc = nullptr;

struct Frame { ID3D12CommandAllocator* alloc; ID3D12Resource* res; D3D12_CPU_DESCRIPTOR_HANDLE rtv; };

static const int MAX_FRAMES = 8;
static Frame                      g_frames[MAX_FRAMES] = {};
static UINT                       g_bufCount = 0;
static bool                       g_init = false;
static HWND                       g_hwnd = nullptr;
static ID3D12Device* g_dev = nullptr;
static ID3D12CommandQueue* g_queue = nullptr;
static ID3D12DescriptorHeap* g_rtvHeap = nullptr;
static ID3D12DescriptorHeap* g_srvHeap = nullptr;
static ID3D12GraphicsCommandList* g_cmd = nullptr;
static ID3D12Fence* g_fence = nullptr;
static HANDLE       g_fenceEvent = nullptr;
static UINT64       g_fenceCounter = 0;
static UINT64       g_fenceVals[MAX_FRAMES] = {};

static void WaitFor(UINT64 v)
{
    if (g_fence->GetCompletedValue() < v) {
        g_fence->SetEventOnCompletion(v, g_fenceEvent);
        WaitForSingleObject(g_fenceEvent, INFINITE);
    }
}
static void CreateRTVs(IDXGISwapChain3* sc)
{
    UINT inc = g_dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    D3D12_CPU_DESCRIPTOR_HANDLE h = g_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    for (UINT i = 0; i < g_bufCount; i++) {
        sc->GetBuffer(i, IID_PPV_ARGS(&g_frames[i].res));
        g_dev->CreateRenderTargetView(g_frames[i].res, nullptr, h);
        g_frames[i].rtv = h;
        h.ptr += inc;
    }
}

static void ReleaseRTVs()
{
    for (UINT i = 0; i < g_bufCount; i++)
        if (g_frames[i].res) { g_frames[i].res->Release(); g_frames[i].res = nullptr; }
}

static LRESULT CALLBACK hkWndProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_KEYDOWN && w == VK_INSERT && !(l & (1 << 30))) Menu::Toggle();

    if (Menu::IsOpen()) {
        ImGui_ImplWin32_WndProcHandler(h, m, w, l);
        ImGuiIO& io = ImGui::GetIO();

        bool mouse = (m >= WM_MOUSEFIRST && m <= WM_MOUSELAST) || m == WM_INPUT;
        bool key = (m >= WM_KEYFIRST && m <= WM_KEYLAST);

        if (m == WM_INPUT) return DefWindowProcW(h, m, w, l);
        if (mouse) return TRUE;
        if (key && io.WantCaptureKeyboard) return TRUE;
    }
    return CallWindowProcW(oWndProc, h, m, w, l);
}

static void InitImGui(IDXGISwapChain3* sc)
{
    sc->GetDevice(IID_PPV_ARGS(&g_dev));
    g_dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_fence));
    g_fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    DXGI_SWAP_CHAIN_DESC desc;
    sc->GetDesc(&desc);
    g_hwnd = desc.OutputWindow;
    g_bufCount = desc.BufferCount > MAX_FRAMES ? MAX_FRAMES : desc.BufferCount;

    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    hd.NumDescriptors = g_bufCount;
    g_dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_rtvHeap));

    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = 1;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    g_dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_srvHeap));

    for (UINT i = 0; i < g_bufCount; i++)
        g_dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g_frames[i].alloc));
    g_dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_frames[0].alloc, nullptr, IID_PPV_ARGS(&g_cmd));
    g_cmd->Close();

    CreateRTVs(sc);

    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    ImGui_ImplWin32_Init(g_hwnd);
    ImGui_ImplDX12_InitInfo ii = {};
    ii.Device = g_dev;
    ii.CommandQueue = g_queue;
    ii.NumFramesInFlight = (int)g_bufCount;
    ii.RTVFormat = desc.BufferDesc.Format;
    ii.DSVFormat = DXGI_FORMAT_UNKNOWN;
    ii.SrvDescriptorHeap = g_srvHeap;
    ii.SrvDescriptorAllocFn = [](ImGui_ImplDX12_InitInfo*, D3D12_CPU_DESCRIPTOR_HANDLE* cpu, D3D12_GPU_DESCRIPTOR_HANDLE* gpu)
        {
            *cpu = g_srvHeap->GetCPUDescriptorHandleForHeapStart();
            *gpu = g_srvHeap->GetGPUDescriptorHandleForHeapStart();
        };
    ii.SrvDescriptorFreeFn = [](ImGui_ImplDX12_InitInfo*, D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_GPU_DESCRIPTOR_HANDLE) {};
    ImGui_ImplDX12_Init(&ii);

    oWndProc = (WNDPROC)SetWindowLongPtrW(g_hwnd, GWLP_WNDPROC, (LONG_PTR)hkWndProc);
    g_init = true;
}

static void __stdcall hkExec(ID3D12CommandQueue* q, UINT n, ID3D12CommandList* const* lists)
{
    if (!g_queue && q->GetDesc().Type == D3D12_COMMAND_LIST_TYPE_DIRECT) g_queue = q;
    oExec(q, n, lists);
}

static HRESULT __stdcall hkPresent(IDXGISwapChain3* sc, UINT sync, UINT flags)
{
    if (flags & DXGI_PRESENT_TEST) return oPresent(sc, sync, flags);
    if (!g_queue) return oPresent(sc, sync, flags);
    if (!g_init) InitImGui(sc);

    ImGui_ImplDX12_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    
    Menu::Render();

    ImGui::Render();

    UINT idx = sc->GetCurrentBackBufferIndex();
    Frame& f = g_frames[idx];
    WaitFor(g_fenceVals[idx]);
    f.alloc->Reset();

    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = f.res;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;

    g_cmd->Reset(f.alloc, nullptr);
    g_cmd->ResourceBarrier(1, &b);
    g_cmd->OMSetRenderTargets(1, &f.rtv, FALSE, nullptr);
    g_cmd->SetDescriptorHeaps(1, &g_srvHeap);
    ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), g_cmd);
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    g_cmd->ResourceBarrier(1, &b);
    g_cmd->Close();

    ID3D12CommandList* lists[] = { g_cmd };
    oExec(g_queue, 1, lists);
    g_queue->Signal(g_fence, ++g_fenceCounter);
    g_fenceVals[idx] = g_fenceCounter;

    return oPresent(sc, sync, flags);
}

static HRESULT __stdcall hkResize(IDXGISwapChain3* sc, UINT count, UINT w, UINT h, DXGI_FORMAT fmt, UINT flags)
{
    if (!g_init) return oResize(sc, count, w, h, fmt, flags);

    g_queue->Signal(g_fence, ++g_fenceCounter);
    WaitFor(g_fenceCounter);

    ReleaseRTVs();
    ImGui_ImplDX12_InvalidateDeviceObjects();
    HRESULT hr = oResize(sc, count, w, h, fmt, flags);
    ImGui_ImplDX12_CreateDeviceObjects();
    CreateRTVs(sc);
    return hr;
}

static bool GetVTableAddrs(void** present, void** resize, void** exec)
{
    WNDCLASSEXA wc = { sizeof(wc), CS_HREDRAW | CS_VREDRAW, DefWindowProcA, 0, 0,
                       GetModuleHandleA(nullptr), 0, 0, 0, 0, "dx12dummy", 0 };
    RegisterClassExA(&wc);
    HWND hw = CreateWindowA(wc.lpszClassName, "", WS_OVERLAPPEDWINDOW, 0, 0, 100, 100, 0, 0, wc.hInstance, 0);

    IDXGIFactory4* fac = nullptr;
    ID3D12Device* dev = nullptr;
    ID3D12CommandQueue* q = nullptr;
    IDXGISwapChain1* sc = nullptr;
    bool ok = false;

    if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&fac))) &&
        SUCCEEDED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dev)))) {
        D3D12_COMMAND_QUEUE_DESC qd{};
        qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&q));

        DXGI_SWAP_CHAIN_DESC1 sd{};
        sd.Width = 100; sd.Height = 100;
        sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        sd.SampleDesc.Count = 1;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.BufferCount = 2;
        sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

        if (SUCCEEDED(fac->CreateSwapChainForHwnd(q, hw, &sd, nullptr, nullptr, &sc))) {
            void** sv = *(void***)sc;
            void** qv = *(void***)q;
            *present = sv[8];
            *resize = sv[13];
            *exec = qv[10];
            ok = true;
        }
    }

    if (sc) sc->Release();
    if (q) q->Release();
    if (dev) dev->Release();
    if (fac) fac->Release();
    DestroyWindow(hw);
    UnregisterClassA(wc.lpszClassName, wc.hInstance);
    return ok;
}

bool Hook::Init()
{
    void* pPresent, * pResize, * pExec;
    if (!GetVTableAddrs(&pPresent, &pResize, &pExec)) return false;

    if (MH_Initialize() != MH_OK) return false;
    MH_CreateHook(pPresent, (void*)hkPresent, (void**)&oPresent);
    MH_CreateHook(pResize, (void*)hkResize, (void**)&oResize);
    MH_CreateHook(pExec, (void*)hkExec, (void**)&oExec);
    return MH_EnableHook(MH_ALL_HOOKS) == MH_OK;
}