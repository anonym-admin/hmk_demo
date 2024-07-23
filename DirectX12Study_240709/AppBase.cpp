#include "pch.h"

#include "AppBase.h"
#include "Camera.h"
#include "ColorBuffer.h"
#include "DescriptorHeap.h"
#include "GeometryGenerator.h"
#include "GraphicsCommon.h"
#include "Input.h"
#include "Model.h"
#include "Timer.h"

AppBase *g_appBase = nullptr;

namespace Display
{
uint32_t g_screenWidth     = 1200;
uint32_t g_screenHeight    = 800;
extern float g_imguiWidth  = 0.0f;
extern float g_imguiHeight = 0.0f;
} // namespace Display

namespace Graphics
{
ID3D12Device *g_Device = nullptr;
ColorBuffer g_DisplayPlane[2];

DescriptorHeap s_Texture;
DescriptorAllocator g_DescriptorAllocator[] = {D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
                                               D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, D3D12_DESCRIPTOR_HEAP_TYPE_RTV,
                                               D3D12_DESCRIPTOR_HEAP_TYPE_DSV};
} // namespace Graphics

HWND g_hwnd = nullptr;

LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam);
IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

AppBase::AppBase()
{
    g_appBase = this;
}

AppBase::~AppBase()
{
    WaitForPreviousFrame();
    CloseHandle(m_fenceEvent);

    Graphics::DestroyGraphicsCommon();

    // Cleanup
    ImGui_ImplDX12_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    SAFE_VECTOR_CLEAR(m_lightSpheres);
    SAFE_VECTOR_CLEAR(m_opaqueList);
    SAFE_DELETE(m_depthMap);
    SAFE_DELETE(m_skybox);

    SAFE_RELEASE(m_envTexture);
    SAFE_DELETE(m_camera);
    //SAFE_RELEASE(m_rootSignature)
    SAFE_DELETE(m_timer);
    SAFE_RELEASE(m_fence);
    SAFE_RELEASE(m_commandList);
    SAFE_RELEASE(m_commandAllocator);
    SAFE_RELEASE(m_swapChain);
    SAFE_RELEASE(m_commandQueue);
    SAFE_RELEASE(m_device);
}

bool AppBase::Initialize()
{
    if (!InitWindow())
    {
        return false;
    }
    if (!InitD3D())
    {
        return false;
    }
    if (!InitGui())
    {
        return false;
    }

    // Init Timer
    CREATE_OBJ(m_timer, Timer);
    m_timer->Initialize();

    // Mouse & Keyboard input initialize.
    GameInput::Initialize();

    this->BuildSRVDesriptorHeap();
    this->BuildGlobalConsts();

    // Init graphics common.
    Graphics::InitGraphicsCommon(m_device);

    // Init Light
    InitLights();

    // Create sky box.
    CREATE_OBJ(m_skybox, Model);
    {
        MeshData cube = GeometryGenerator::MakeCube(50.0f, 50.0f, 50.0f);
        m_skybox->Initialize(m_device, m_commandList, {cube});
    }

    // Create depth map square
    CREATE_OBJ(m_depthMap, Model)
    {
        MeshData square = GeometryGenerator::MakeSquare(2.0f, 2.0f);
        m_depthMap->Initialize(m_device, m_commandList, {square});
    }

    m_postEffects.Initialize();

    return true;
}
void AppBase::Update(const float dt)
{
    m_timer->Update();

    GameInput::Update(dt);

    UpdateGlobalConsts(dt);

    for (auto &e : m_opaqueList)
    {
        e->Update();
    }

    m_skybox->Update();
}

void AppBase::Render()
{
    ThrowIfFailed(m_commandAllocator->Reset());
    ThrowIfFailed(m_commandList->Reset(m_commandAllocator, nullptr));
    // Depth only pass.
    DepthOnlyPass();
    // Render Object.
    RenderOpaqueObject();
    // Render Depth viewport
    RenderDepthMapViewport();
}

int32_t AppBase::Run()
{
    // Main sample loop.
    MSG msg = {};
    while (msg.message != WM_QUIT)
    {
        // Process any messages in the queue.
        if (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
        else
        {
            if (m_isKeyDown[VK_ESCAPE])
                PostQuitMessage(0);

            // Start the Dear ImGui frame
            ImGui_ImplDX12_NewFrame();
            ImGui_ImplWin32_NewFrame();
            ImGui::NewFrame();

            ImGuiIO &io = ImGui::GetIO();
            (void)io;

            this->UpdateGui(io.Framerate);

            ImGui::Render();

            this->Update(io.Framerate);

            this->Render();

            ID3D12DescriptorHeap *descHeaps[] = {m_imguiInitHeap.Get()};
            m_commandList->SetDescriptorHeaps(_countof(descHeaps), descHeaps);
            ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), m_commandList);

            // Indicate that the back buffer will now be used to present.
            m_commandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(
                                                  Graphics::g_DisplayPlane[m_frameIndex].GetResource(),
                                                  D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT));

            ThrowIfFailed(m_commandList->Close());
            // Execute the command list.
            ID3D12CommandList *ppCommandLists[] = {m_commandList};
            m_commandQueue->ExecuteCommandLists(_countof(ppCommandLists), ppCommandLists);

            // Present the frame.
            ThrowIfFailed(m_swapChain->Present(1, 0));

            WaitForPreviousFrame();

            m_frameIndex = (m_frameIndex + 1) % s_frameCount;
        }
    }
    // Return this part of the WM_QUIT message to Windows.
    return static_cast<int32_t>(msg.wParam);
}

bool AppBase::InitWindow()
{
    // Initialize the window class.
    WNDCLASSEX windowClass    = {};
    windowClass.cbSize        = sizeof(WNDCLASSEX);
    windowClass.style         = CS_HREDRAW | CS_VREDRAW;
    windowClass.lpfnWndProc   = WndProc;
    windowClass.hInstance     = GetModuleHandle(nullptr);
    windowClass.hCursor       = LoadCursor(NULL, IDC_ARROW);
    windowClass.lpszClassName = L"DX12Study";
    RegisterClassEx(&windowClass);

    RECT windowRect = {0, 0, static_cast<LONG>(Display::g_screenWidth), static_cast<LONG>(Display::g_screenHeight)};
    AdjustWindowRect(&windowRect, WS_OVERLAPPEDWINDOW, FALSE);

    // Create the window and store a handle to it.
    g_hwnd = m_hwnd =
        CreateWindow(windowClass.lpszClassName, windowClass.lpszClassName, WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
                     CW_USEDEFAULT, windowRect.right - windowRect.left, windowRect.bottom - windowRect.top,
                     nullptr, // We have no parent window.
                     nullptr, // We aren't using menus.
                     windowClass.hInstance, nullptr);
    ShowWindow(m_hwnd, SW_SHOW);

    return true;
}

bool AppBase::InitD3D()
{
    UINT dxgiFactoryFlags = 0;

#if defined(_DEBUG)
    // Enable the debug layer (requires the Graphics Tools "optional feature").
    // NOTE: Enabling the debug layer after device creation will invalidate the active device.
    {
        ID3D12Debug *debugController = nullptr;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController))))
        {
            debugController->EnableDebugLayer();

            // Enable additional debug layers.
            dxgiFactoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
            SAFE_RELEASE(debugController);
        }
    }
#endif
    IDXGIFactory6 *factory = nullptr;
    ThrowIfFailed(CreateDXGIFactory2(dxgiFactoryFlags, IID_PPV_ARGS(&factory)));

    if (m_useWarpDevice)
    {
        IDXGIAdapter *warpAdapter = nullptr;
        ThrowIfFailed(factory->EnumWarpAdapter(IID_PPV_ARGS(&warpAdapter)));

        ThrowIfFailed(D3D12CreateDevice(warpAdapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&m_device)));
        SAFE_RELEASE(warpAdapter);
    }
    else
    {
        IDXGIAdapter1 *hardwareAdapter = nullptr;
        GetHardwareAdapter(factory, &hardwareAdapter);

        ThrowIfFailed(D3D12CreateDevice(hardwareAdapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&m_device)));
        SAFE_RELEASE(hardwareAdapter);
    }

    Graphics::g_Device = m_device;

    // Describe and create the command queue.
    D3D12_COMMAND_QUEUE_DESC queueDesc = {};
    queueDesc.Flags                    = D3D12_COMMAND_QUEUE_FLAG_NONE;
    queueDesc.Type                     = D3D12_COMMAND_LIST_TYPE_DIRECT;

    ThrowIfFailed(m_device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&m_commandQueue)));

    DXGI_SWAP_CHAIN_DESC1 swapChainDesc = {};
    swapChainDesc.BufferCount           = s_frameCount;
    swapChainDesc.Width                 = Display::g_screenWidth;
    swapChainDesc.Height                = Display::g_screenHeight;
    swapChainDesc.Format                = DXGI_FORMAT_R8G8B8A8_UNORM;
    swapChainDesc.BufferUsage           = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapChainDesc.SwapEffect            = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    swapChainDesc.SampleDesc.Count      = 1;
    swapChainDesc.Flags                 = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;

    IDXGISwapChain1 *swapChain = nullptr;
    ThrowIfFailed(
        factory->CreateSwapChainForHwnd(m_commandQueue, m_hwnd, &swapChainDesc, nullptr, nullptr, &swapChain));

    // This sample does not support fullscreen transitions.
    ThrowIfFailed(factory->MakeWindowAssociation(m_hwnd, DXGI_MWA_NO_ALT_ENTER));
    m_swapChain = swapChain;
    swapChain   = nullptr;

    ThrowIfFailed(m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&m_commandAllocator)));

    // Create the command list.
    ThrowIfFailed(m_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, m_commandAllocator, nullptr,
                                              IID_PPV_ARGS(&m_commandList)));

    // Command lists are created in the recording state, but there is nothing
    // to record yet. The main loop expects it to be closed, so close it now.
    ThrowIfFailed(m_commandList->Close());

    // Create synchronization objects.
    {
        ThrowIfFailed(m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence)));
        m_fenceValue = 1;

        // Create an event handle to use for frame synchronization.
        m_fenceEvent = CreateEvent(nullptr, false, false, nullptr);
        if (m_fenceEvent == nullptr)
        {
            ThrowIfFailed(HRESULT_FROM_WIN32(GetLastError()));
        }
    }

    this->Resize();

    SAFE_RELEASE(factory);

    return true;
}

bool AppBase::InitGui()
{
    // Setup Dear ImGui context
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    (void)io;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard; // Enable Keyboard Controls
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;  // Enable Gamepad Controls

    // Setup Dear ImGui style
    ImGui::StyleColorsDark();
    // ImGui::StyleColorsLight();

    m_imguiInitHeap.Create(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1);

    // Setup Platform/Renderer backends
    ImGui_ImplWin32_Init(m_hwnd);
    ImGui_ImplDX12_Init(m_device, 3, DXGI_FORMAT_R8G8B8A8_UNORM, m_imguiInitHeap.Get(),
                        D3D12_CPU_DESCRIPTOR_HANDLE(m_imguiInitHeap[0]),
                        D3D12_GPU_DESCRIPTOR_HANDLE(m_imguiInitHeap[0]));
    return true;
}

void AppBase::GetHardwareAdapter(IDXGIFactory1 *pFactory, IDXGIAdapter1 **ppAdapter, bool requestHighPerformanceAdapter)
{
    *ppAdapter = nullptr;

    IDXGIAdapter1 *adapter = nullptr;

    IDXGIFactory6 *factory6 = nullptr;
    if (SUCCEEDED(pFactory->QueryInterface(IID_PPV_ARGS(&factory6))))
    {
        for (UINT adapterIndex = 0; SUCCEEDED(factory6->EnumAdapterByGpuPreference(
                 adapterIndex,
                 requestHighPerformanceAdapter == true ? DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE
                                                       : DXGI_GPU_PREFERENCE_UNSPECIFIED,
                 IID_PPV_ARGS(&adapter)));
             ++adapterIndex)
        {
            DXGI_ADAPTER_DESC1 desc;
            adapter->GetDesc1(&desc);

            if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)
            {
                // Don't select the Basic Render Driver adapter.
                // If you want a software adapter, pass in "/warp" on the command line.
                continue;
            }

            // Check to see whether the adapter supports Direct3D 12, but don't create the
            // actual device yet.
            if (SUCCEEDED(D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0, _uuidof(ID3D12Device), nullptr)))
            {
                break;
            }
        }
    }

    if (adapter == nullptr)
    {
        for (UINT adapterIndex = 0; SUCCEEDED(pFactory->EnumAdapters1(adapterIndex, &adapter)); ++adapterIndex)
        {
            DXGI_ADAPTER_DESC1 desc;
            adapter->GetDesc1(&desc);

            if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)
            {
                // Don't select the Basic Render Driver adapter.
                // If you want a software adapter, pass in "/warp" on the command line.
                continue;
            }

            // Check to see whether the adapter supports Direct3D 12, but don't create the
            // actual device yet.
            if (SUCCEEDED(D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0, _uuidof(ID3D12Device), nullptr)))
            {
                break;
            }
        }
    }

    *ppAdapter = adapter;
    adapter    = nullptr;

    SAFE_RELEASE(factory6);
}

void AppBase::BuildGlobalConsts()
{
    m_globalConstsBuffer.Initialize(m_device, 1);
}

void AppBase::UpdateGlobalConsts(const float dt)
{
    auto eyePos  = m_camera->GetPosition();
    auto viewRow = m_camera->GetViewMatrix();
    auto projRow = m_camera->GetProjectionMatrix();

    m_globalConstData.eyeWorld    = eyePos;
    m_globalConstData.view        = viewRow.Transpose();
    m_globalConstData.viewInv     = m_globalConstData.view.Invert();
    m_globalConstData.proj        = projRow.Transpose();
    m_globalConstData.projInv     = m_globalConstData.proj.Invert();
    m_globalConstData.viewProjInv = (viewRow * projRow).Invert().Transpose();

    m_globalConstsBuffer.Upload(0, &m_globalConstData);
}

void AppBase::UpdateCamera(const float dt)
{
    if (GameInput::IsPressed(GameInput::kKey_w))
    {
        m_camera->MoveFront(dt);
    }
    if (GameInput::IsPressed(GameInput::kKey_s))
    {
        m_camera->MoveBack(dt);
    }
    if (GameInput::IsPressed(GameInput::kKey_d))
    {
        m_camera->MoveRight(dt);
    }
    if (GameInput::IsPressed(GameInput::kKey_a))
    {
        m_camera->MoveLeft(dt);
    }
    if (GameInput::IsPressed(GameInput::kKey_q))
    {
        m_camera->MoveUp(dt);
    }
    if (GameInput::IsPressed(GameInput::kKey_e))
    {
        m_camera->MoveDown(dt);
    }
}

void AppBase::UpdateGui(const float frameRate)
{
}

void AppBase::DepthOnlyPass()
{
    m_commandList->RSSetViewports(1, &Graphics::mainViewport);
    m_commandList->RSSetScissorRects(1, &Graphics::mainSissorRect);

    m_commandList->SetGraphicsRootSignature(
        Graphics::defaultRootSignature); // Root signature 이후에 변경 .... 방법 찾기
    m_commandList->SetGraphicsRootConstantBufferView(0, m_globalConstsBuffer.GetResource()->GetGPUVirtualAddress());

    ID3D12DescriptorHeap *descHeaps[] = {Graphics::s_Texture.Get()};
    m_commandList->SetDescriptorHeaps(_countof(descHeaps), descHeaps);
    m_commandList->SetGraphicsRootDescriptorTable(3, D3D12_GPU_DESCRIPTOR_HANDLE(m_handle));

    m_commandList->OMSetRenderTargets(0, nullptr, false, &m_depthOnlyBuffer.GetDSV());
    m_commandList->ClearDepthStencilView(m_depthOnlyBuffer.GetDSV(), D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
    // render object.
    m_commandList->SetPipelineState(Graphics::depthOnlyPSO);
    for (auto &e : m_opaqueList)
    {
        e->Render(m_commandList);
    }
    // render skybox
    m_skybox->Render(m_commandList);
}

void AppBase::RenderOpaqueObject()
{
    m_commandList->RSSetViewports(1, &Graphics::mainViewport);
    m_commandList->RSSetScissorRects(1, &Graphics::mainSissorRect);

    // Indicate that the back buffer will be used as a render target.
    m_commandList->ResourceBarrier(
        1, &CD3DX12_RESOURCE_BARRIER::Transition(Graphics::g_DisplayPlane[m_frameIndex].GetResource(),
                                                 D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET));

    m_commandList->OMSetRenderTargets(1, &Graphics::g_DisplayPlane[m_frameIndex].GetRTV(), false,
                                      &m_depthBuffer.GetDSV());
    // Record commands.
    const float clearColor[] = {0.0f, 0.2f, 0.4f, 1.0f};
    m_commandList->ClearRenderTargetView(Graphics::g_DisplayPlane[m_frameIndex].GetRTV(), clearColor, 0, nullptr);
    m_commandList->ClearDepthStencilView(m_depthBuffer.GetDSV(), D3D12_CLEAR_FLAG_STENCIL | D3D12_CLEAR_FLAG_DEPTH,
                                         1.0f, 0, 0, nullptr);

    // render object.
    for (auto &e : m_opaqueList)
    {
        m_commandList->SetPipelineState(e->GetPSO(m_isWireFrame));
        e->Render(m_commandList);

        if (m_drawAsNormal)
        {
            m_commandList->SetPipelineState(Graphics::normalPSO);
            e->RenderNormal(m_commandList);
        }
    }

    // render skybox
    m_commandList->SetPipelineState(Graphics::skyboxPSO);
    m_skybox->Render(m_commandList);
}

void AppBase::RenderDepthMapViewport()
{
    m_commandList->RSSetViewports(1, &Graphics::depthMapViewport);
    m_commandList->RSSetScissorRects(1, &Graphics::mainSissorRect);
    m_commandList->SetPipelineState(Graphics::depthViewportPSO);
    m_commandList->SetGraphicsRootSignature(Graphics::depthOnlyRootSignature);
    m_commandList->SetGraphicsRootShaderResourceView(0, m_depthBuffer.GetResource()->GetGPUVirtualAddress());

    m_postEffects.Render(m_commandList);
}

void AppBase::DestroyPSO()
{
    SAFE_RELEASE(Graphics::defaultWirePSO);
    SAFE_RELEASE(Graphics::defaultSolidPSO);
}

void AppBase::CreateBuffers()
{
    using namespace Display;
    using namespace Graphics;
    // Create a RTV for each frame.
    for (UINT n = 0; n < s_frameCount; n++)
    {
        ID3D12Resource *backBuffer = nullptr;
        ThrowIfFailed(m_swapChain->GetBuffer(n, IID_PPV_ARGS(&backBuffer)));
        g_DisplayPlane[n].CreateFromSwapChain(backBuffer);
    }

    // Create depth stencil buffer.
    m_depthBuffer.Create(g_screenWidth, g_screenHeight, DXGI_FORMAT_R24G8_TYPELESS);

    // Create depth only buffer
    m_depthOnlyBuffer.Create(g_screenWidth, g_screenHeight, DXGI_FORMAT_R32_TYPELESS, true);
}

void AppBase::OnMouse(const float x, const float y)
{
    using namespace Display;

    auto newScreenWidth = float(g_screenWidth - g_imguiWidth);
    auto newSreenHeight = float(g_screenHeight);

    m_mouseX = std::clamp(x, 0.0f, newScreenWidth);
    m_mouseY = std::clamp(y, 0.0f, newSreenHeight);

    m_ndcX = m_mouseX / newScreenWidth * 2.0f - 1.0f;
    m_ndcY = -(m_mouseY / newSreenHeight * 2.0f - 1.0f);

    m_ndcX = std::clamp(m_ndcX, -1.0f, 1.0f);
    m_ndcY = std::clamp(m_ndcY, -1.0f, 1.0f);

    if (m_isFPV)
    {
        m_camera->MouseUpdate(m_ndcX, m_ndcY);
    }
}

void AppBase::BuildSRVDesriptorHeap()
{
    Graphics::s_Texture.Create(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 4098);
}

LRESULT AppBase::MemberWndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (ImGui_ImplWin32_WndProcHandler(hWnd, message, wParam, lParam))
        return true;

    switch (message)
    {
    case WM_CREATE:
        return 0;

    case WM_SIZE: {
        using namespace Display;

        RECT rect = {};
        GetClientRect(hWnd, &rect);
        g_screenWidth  = uint32_t(rect.right - rect.left);
        g_screenHeight = uint32_t(rect.bottom - rect.top);

        if (m_swapChain)
        {
            if (g_screenWidth && g_screenHeight)
            {
                this->Resize();

                Graphics::mainViewport   = D3DUtils::CreateViewport(0.0f, 0.0f, g_screenWidth, g_screenHeight);
                Graphics::mainSissorRect = D3DUtils::CreateScissorRect(0.0f, 0.0f, g_screenWidth, g_screenHeight);
            }
        }
    }
    break;
    case WM_KEYDOWN:
        if (wParam == 70)
            m_isFPV = !m_isFPV;
        m_isKeyDown[wParam] = true;
        break;
    case WM_KEYUP:
        m_isKeyDown[wParam] = false;
        break;
    case WM_MOUSEMOVE:
        // std::cout << HIWORD(lParam) << " " << LOWORD(lParam) << std::endl;
        OnMouse(LOWORD(lParam), HIWORD(lParam));
        break;
    case WM_LBUTTONDOWN:
        m_leftButtonDown      = true;
        m_leftButtonDragStart = true;
        break;
    case WM_LBUTTONUP:
        m_leftButtonDown = false;
        break;
    case WM_RBUTTONDOWN:
        m_rightButtonDown      = true;
        m_rightButtonDragStart = true;
        break;
    case WM_RBUTTONUP:
        m_rightButtonDown = false;
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        break;
    }

    // Handle any messages the switch statement didn't.
    return DefWindowProc(hWnd, message, wParam, lParam);
}

void AppBase::WaitForPreviousFrame()
{
    // WAITING FOR THE FRAME TO COMPLETE BEFORE CONTINUING IS NOT BEST PRACTICE.
    // This is code implemented as such for simplicity. The D3D12HelloFrameBuffering
    // sample illustrates how to use fences for efficient resource usage and to
    // maximize GPU utilization.

    // Signal and increment the fence value.
    const UINT64 fence = m_fenceValue;
    ThrowIfFailed(m_commandQueue->Signal(m_fence, fence));
    m_fenceValue++;

    // Wait until the previous frame is finished.
    if (m_fence->GetCompletedValue() < fence)
    {
        ThrowIfFailed(m_fence->SetEventOnCompletion(fence, m_fenceEvent));
        WaitForSingleObject(m_fenceEvent, INFINITE);
    }
}

void AppBase::InitCubemap(std::wstring basePath, std::wstring envFilename)
{
    ResourceUploadBatch resourceUpload(m_device);

    resourceUpload.Begin();

    bool isCubemap = false;
    ThrowIfFailed(CreateDDSTextureFromFile(m_device, resourceUpload, (basePath + envFilename).c_str(), &m_envTexture,
                                           false, 0, nullptr, &isCubemap));

    // Upload the resources to the GPU.
    auto uploadResourcesFinished = resourceUpload.End(m_commandQueue);

    // Wait for the upload thread to terminate
    uploadResourcesFinished.wait();

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Shader4ComponentMapping         = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.ViewDimension                   = D3D12_SRV_DIMENSION_TEXTURECUBE;
    srvDesc.TextureCube.MostDetailedMip     = 0;
    srvDesc.TextureCube.MipLevels           = m_envTexture->GetDesc().MipLevels;
    srvDesc.TextureCube.ResourceMinLODClamp = 0.0f;
    srvDesc.Format                          = m_envTexture->GetDesc().Format;

    m_handle = Graphics::s_Texture.Alloc(1);
    m_device->CreateShaderResourceView(m_envTexture, &srvDesc, D3D12_CPU_DESCRIPTOR_HANDLE(m_handle));
}

void AppBase::InitLights()
{
    // directional light
    {
        m_light[0].type |= DIRECTIONAL_LIGHT;
    }
    // point light
    {
        m_light[1].type |= POINT_LIGHT;
        m_light[1].position = Vector3(0.0f, 1.0f, -5.0f);

        MeshData sphere    = GeometryGenerator::MakeSphere(0.025f, 10, 10);
        Model *lightSphere = new Model;
        lightSphere->Initialize(m_device, m_commandList, {sphere});
        lightSphere->GetMaterialConstCPU().ambient = Vector3(1.0f, 0.0f, 0.0f);
        lightSphere->UpdateWorldMatrix(Matrix::CreateTranslation(m_light[1].position));
        m_lightSpheres.push_back(lightSphere);
    }
    // spot light
    {
        m_light[2].type |= SPOT_LIGHT;
        m_light[2].position = Vector3(1.0f, 2.0f, -3.0f);

        MeshData sphere    = GeometryGenerator::MakeSphere(0.025f, 10, 10);
        Model *lightSphere = new Model;
        lightSphere->Initialize(m_device, m_commandList, {sphere});
        lightSphere->GetMaterialConstCPU().ambient = Vector3(1.0f, 0.0f, 0.0f);
        lightSphere->UpdateWorldMatrix(Matrix::CreateTranslation(m_light[2].position));
        m_lightSpheres.push_back(lightSphere);
    }
}

void AppBase::UpdateLights()
{
}

LRESULT WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    return g_appBase->MemberWndProc(hWnd, message, wParam, lParam);
}

void AppBase::Resize()
{
    using namespace Display;
    using namespace Graphics;

    m_commandList->Reset(m_commandAllocator, nullptr);

    // Reset a RTV for each frame.
    if (g_DisplayPlane[0].GetResource())
    {
        for (UINT n = 0; n < s_frameCount; n++)
        {
            g_DisplayPlane[n].GetResource()->Release();
        }
    }
    if (m_depthBuffer.GetResource())
    {
        m_depthBuffer.GetResource()->Release();
    }
    // swap chain resize.
    if (m_swapChain)
    {
        ThrowIfFailed(m_swapChain->ResizeBuffers(s_frameCount, g_screenWidth, g_screenHeight,
                                                 DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH));
    }

    // Create frame resources.
    {
        // 추상화
        // Describe and create a render target view (SRV) descriptor heap.
        // D3DUtils::CreateDscriptor(m_device, 1, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
        //                          D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, &m_srvHeap);
        m_frameIndex = 0;

        this->CreateBuffers();

        // Transition the resource from its initial state to be used as a depth buffer.
        m_commandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(m_depthBuffer.GetResource(),
                                                                                D3D12_RESOURCE_STATE_COMMON,
                                                                                D3D12_RESOURCE_STATE_DEPTH_WRITE));
        // Transition the resource from its initial state to be used as a depth buffer.
        m_commandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(m_depthOnlyBuffer.GetResource(),
                                                                                D3D12_RESOURCE_STATE_COMMON,
                                                                                D3D12_RESOURCE_STATE_DEPTH_WRITE));
    }

    // For texture loading.
    ThrowIfFailed(m_commandList->Close());
    ID3D12CommandList *ppCommandLists[] = {m_commandList};
    m_commandQueue->ExecuteCommandLists(_countof(ppCommandLists), ppCommandLists);

    WaitForPreviousFrame();
}
