#include <bx/bx.h>
#include <bx/file.h>
#include <bx/sort.h>
#include "bgfx\bgfx.h"
#include "bgfx\platform.h"

#include "pch.h"
#include "common.h"
#include "geometry.h"
#include "graphicsplugin.h"

#ifdef XR_USE_GRAPHICS_API_D3D11

#include <common/xr_linear.h>
#include <DirectXColors.h>
#include <D3Dcompiler.h>

using namespace Microsoft::WRL;
using namespace DirectX;

namespace {
bx::AllocatorI* getDefaultAllocator() {
    BX_PRAGMA_DIAGNOSTIC_PUSH();
    BX_PRAGMA_DIAGNOSTIC_IGNORED_MSVC(4459);  // warning C4459: declaration of 's_allocator' hides global declaration
    BX_PRAGMA_DIAGNOSTIC_IGNORED_CLANG_GCC("-Wshadow");
    static bx::DefaultAllocator s_allocator;
    return &s_allocator;
    BX_PRAGMA_DIAGNOSTIC_POP();
}
bx::AllocatorI* g_allocator = getDefaultAllocator();

typedef bx::StringT<&g_allocator> String;

class FileReader : public bx::FileReader {
    typedef bx::FileReader super;

   public:
    virtual bool open(const bx::FilePath& _filePath, bx::Error* _err) override {
        String filePath("");
        filePath.append(_filePath);
        return super::open(filePath.getPtr(), _err);
    }
};
struct PosColorVertex {
    float m_x;
    float m_y;
    float m_z;
    uint32_t m_abgr;

    static void init() {
        ms_decl.begin()
            .add(bgfx::Attrib::Position, 3, bgfx::AttribType::Float)
            .add(bgfx::Attrib::Color0, 4, bgfx::AttribType::Uint8, true)
            .end();
    };

    static bgfx::VertexDecl ms_decl;
};

bgfx::VertexDecl PosColorVertex::ms_decl;

static PosColorVertex s_cubeVertices[] = {
    {-1.0f, 1.0f, 1.0f, 0xff000000},   {1.0f, 1.0f, 1.0f, 0xff0000ff},   {-1.0f, -1.0f, 1.0f, 0xff00ff00},
    {1.0f, -1.0f, 1.0f, 0xff00ffff},   {-1.0f, 1.0f, -1.0f, 0xffff0000}, {1.0f, 1.0f, -1.0f, 0xffff00ff},
    {-1.0f, -1.0f, -1.0f, 0xffffff00}, {1.0f, -1.0f, -1.0f, 0xffffffff},
};

static const uint16_t s_cubeTriList[] = {
    0, 1, 2,           // 0
    1, 3, 2, 4, 6, 5,  // 2
    5, 6, 7, 0, 2, 4,  // 4
    4, 2, 6, 1, 5, 3,  // 6
    5, 7, 3, 0, 4, 1,  // 8
    4, 5, 1, 2, 3, 6,  // 10
    6, 3, 7,
};

static const uint16_t s_cubeTriStrip[] = {
    0, 1, 2, 3, 7, 1, 5, 0, 4, 2, 6, 7, 4, 5,
};

static const uint16_t s_cubeLineList[] = {
    0, 1, 0, 2, 0, 4, 1, 3, 1, 5, 2, 3, 2, 6, 3, 7, 4, 5, 4, 6, 5, 7, 6, 7,
};

static const uint16_t s_cubeLineStrip[] = {
    0, 2, 3, 1, 5, 7, 6, 4, 0, 2, 6, 4, 5, 7, 3, 1, 0,
};

static const uint16_t s_cubePoints[] = {0, 1, 2, 3, 4, 5, 6, 7};

static const char* s_ptNames[]{
    "Triangle List", "Triangle Strip", "Lines", "Line Strip", "Points",
};

static const uint64_t s_ptState[]{
    UINT64_C(0), BGFX_STATE_PT_TRISTRIP, BGFX_STATE_PT_LINES, BGFX_STATE_PT_LINESTRIP, BGFX_STATE_PT_POINTS,
};
BX_STATIC_ASSERT(BX_COUNTOF(s_ptState) == BX_COUNTOF(s_ptNames));

struct ModelConstantBuffer {
    XMFLOAT4X4 Model;
};
struct ViewProjectionConstantBuffer {
    XMFLOAT4X4 ViewProjection;
};

// Separate entrypoints for the vertex and pixel shader functions.
constexpr char ShaderHlsl[] = R"_(
    struct PSVertex {
        float4 Pos : SV_POSITION;
        float3 Color : COLOR0;
    };
    struct Vertex {
        float3 Pos : POSITION;
        float3 Color : COLOR0;
    };
    cbuffer ModelConstantBuffer : register(b0) {
        float4x4 Model;
    };
    cbuffer ViewProjectionConstantBuffer : register(b1) {
        float4x4 ViewProjection;
    };

    PSVertex MainVS(Vertex input) {
       PSVertex output;
       output.Pos = mul(mul(float4(input.Pos, 1), Model), ViewProjection);
       output.Color = input.Color;
       return output;
    }

    float4 MainPS(PSVertex input) : SV_TARGET {
        return float4(input.Color, 1);
    }
    )_";

XMMATRIX XM_CALLCONV LoadXrPose(const XrPosef& pose) {
    return XMMatrixAffineTransformation(DirectX::g_XMOne, DirectX::g_XMZero,
                                        XMLoadFloat4(reinterpret_cast<const XMFLOAT4*>(&pose.orientation)),
                                        XMLoadFloat3(reinterpret_cast<const XMFLOAT3*>(&pose.position)));
}

XMMATRIX XM_CALLCONV LoadXrMatrix(const XrMatrix4x4f& matrix) {
    // XrMatrix4x4f has same memory layout as DirectX Math (Row-major,post-multiplied = column-major,pre-multiplied)
    return XMLoadFloat4x4(reinterpret_cast<const XMFLOAT4X4*>(&matrix));
}

ComPtr<ID3DBlob> CompileShader(const char* hlsl, const char* entrypoint, const char* shaderTarget) {
    ComPtr<ID3DBlob> compiled;
    ComPtr<ID3DBlob> errMsgs;
    DWORD flags = D3DCOMPILE_PACK_MATRIX_COLUMN_MAJOR | D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_WARNINGS_ARE_ERRORS;

#ifdef _DEBUG
    flags |= D3DCOMPILE_SKIP_OPTIMIZATION | D3DCOMPILE_DEBUG;
#else
    flags |= D3DCOMPILE_OPTIMIZATION_LEVEL3;
#endif

    HRESULT hr = D3DCompile(hlsl, strlen(hlsl), nullptr, nullptr, nullptr, entrypoint, shaderTarget, flags, 0,
                            compiled.GetAddressOf(), errMsgs.GetAddressOf());
    if (FAILED(hr)) {
        std::string errMsg((const char*)errMsgs->GetBufferPointer(), errMsgs->GetBufferSize());
        Log::Write(Log::Level::Error, Fmt("D3DCompile failed %X: %s", hr, errMsg.c_str()));
        THROW_HR(hr, "D3DCompile");
    }

    return compiled;
}

ComPtr<IDXGIAdapter1> GetAdapter(LUID adapterId) {
    // Create the DXGI factory.
    ComPtr<IDXGIFactory1> dxgiFactory;
    CHECK_HRCMD(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(dxgiFactory.ReleaseAndGetAddressOf())));

    for (UINT adapterIndex = 0;; adapterIndex++) {
        // EnumAdapters1 will fail with DXGI_ERROR_NOT_FOUND when there are no more adapters to enumerate.
        ComPtr<IDXGIAdapter1> dxgiAdapter;
        CHECK_HRCMD(dxgiFactory->EnumAdapters1(adapterIndex, dxgiAdapter.ReleaseAndGetAddressOf()));

        DXGI_ADAPTER_DESC1 adapterDesc;
        CHECK_HRCMD(dxgiAdapter->GetDesc1(&adapterDesc));
        if (memcmp(&adapterDesc.AdapterLuid, &adapterId, sizeof(adapterId)) == 0) {
            Log::Write(Log::Level::Verbose, Fmt("Using graphics adapter %ws", adapterDesc.Description));
            return dxgiAdapter;
        }
    }
}

void InitializeD3D11DeviceForAdapter(IDXGIAdapter1* adapter, const std::vector<D3D_FEATURE_LEVEL>& featureLevels,
                                     ID3D11Device** device, ID3D11DeviceContext** deviceContext) {
    UINT creationFlags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;

#ifdef _DEBUG
    creationFlags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

    // Create the Direct3D 11 API device object and a corresponding context.
    const D3D_DRIVER_TYPE driverType = adapter == nullptr ? D3D_DRIVER_TYPE_HARDWARE : D3D_DRIVER_TYPE_UNKNOWN;
    const HRESULT hr = D3D11CreateDevice(adapter, driverType, 0, creationFlags, featureLevels.data(), (UINT)featureLevels.size(),
                                         D3D11_SDK_VERSION, device, nullptr, deviceContext);
    if (FAILED(hr)) {
        // If the initialization fails, fall back to the WARP device.
        // For more information on WARP, see: http://go.microsoft.com/fwlink/?LinkId=286690
        CHECK_HRCMD(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, 0, creationFlags, featureLevels.data(),
                                      (UINT)featureLevels.size(), D3D11_SDK_VERSION, device, nullptr, deviceContext));
    }
}

struct D3D11GraphicsPlugin : public IGraphicsPlugin {
    static const bgfx::Memory* loadMem(bx::FileReaderI* _reader, const char* _filePath) {
        if (bx::open(_reader, _filePath)) {
            uint32_t size = (uint32_t)bx::getSize(_reader);
            const bgfx::Memory* mem = bgfx::alloc(size + 1);
            bx::read(_reader, mem->data, size);
            bx::close(_reader);
            mem->data[mem->size - 1] = '\0';
            return mem;
        }

        std::cout << "Failed to load " << _filePath << "\n";
        return NULL;
    }

    static bgfx::ShaderHandle loadShader(bx::FileReaderI* _reader, const char* _name) {
        char filePath[512];

        const char* shaderPath = "???";

        switch (bgfx::getRendererType()) {
            case bgfx::RendererType::Noop:
            case bgfx::RendererType::Direct3D9:
                shaderPath = "shaders/dx9/";
                break;
            case bgfx::RendererType::Direct3D11:
            case bgfx::RendererType::Direct3D12:
                shaderPath = "shaders/dx11/";
                break;
            case bgfx::RendererType::Gnm:
                shaderPath = "shaders/pssl/";
                break;
            case bgfx::RendererType::Metal:
                shaderPath = "shaders/metal/";
                break;
            case bgfx::RendererType::Nvn:
                shaderPath = "shaders/nvn/";
                break;
            case bgfx::RendererType::OpenGL:
                shaderPath = "shaders/glsl/";
                break;
            case bgfx::RendererType::OpenGLES:
                shaderPath = "shaders/essl/";
                break;
            case bgfx::RendererType::Vulkan:
                shaderPath = "shaders/spirv/";
                break;

            case bgfx::RendererType::Count:
                BX_CHECK(false, "You should not be here!");
                break;
        }

        bx::strCopy(filePath, BX_COUNTOF(filePath),
                    "C:/Users/trbaron/workspace/OpenXR-SDK/build/win64/src/tests/hello_xr/Release/dx11/");
        bx::strCat(filePath, BX_COUNTOF(filePath), _name);
        bx::strCat(filePath, BX_COUNTOF(filePath), ".bin");

        bgfx::ShaderHandle handle = bgfx::createShader(loadMem(_reader, filePath));
        bgfx::setName(handle, _name);

        return handle;
    }

    D3D11GraphicsPlugin(const std::shared_ptr<Options>&, std::shared_ptr<IPlatformPlugin>){};

    std::vector<std::string> GetInstanceExtensions() const override { return {XR_KHR_D3D11_ENABLE_EXTENSION_NAME}; }

    void InitializeDevice(XrInstance instance, XrSystemId systemId) override {
        // Create the D3D11 device for the adapter associated with the system.
        XrGraphicsRequirementsD3D11KHR graphicsRequirements{XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR};
        CHECK_XRCMD(xrGetD3D11GraphicsRequirementsKHR(instance, systemId, &graphicsRequirements));

        // BGFX
        bgfx::renderFrame();
        bgfx::Init bgfxInit;
        bgfxInit.type = bgfx::RendererType::Direct3D11;
        //bgfxInit.vendorId = BGFX_PCI_ID_NONE;    // Auto select
        bgfxInit.vendorId = BGFX_PCI_ID_NVIDIA; 
        bgfxInit.deviceId = (uint16_t) graphicsRequirements.adapterLuid.LowPart;
        bgfxInit.resolution.width = 1000;
        bgfxInit.resolution.height = 1000;
        bgfxInit.resolution.reset = BGFX_RESET_VSYNC;
        bgfx::init(bgfxInit);

        std::cout << "BGFX INIT!!!"
                  << "\n";
        auto caps = bgfx::getCaps();
        std::cout << "DeviceID: " << caps->deviceId << "\n";

        // Enable debug text.
        bgfx::setDebug(true);

        // Set view 0 clear state.
        bgfx::setViewClear(0, BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH, 0x303030ff, 1.0f, 0);

        // DX
        //const ComPtr<IDXGIAdapter1> adapter = GetAdapter(graphicsRequirements.adapterLuid);

        //// Create a list of feature levels which are both supported by the OpenXR runtime and this application.
        //std::vector<D3D_FEATURE_LEVEL> featureLevels = {D3D_FEATURE_LEVEL_12_1, D3D_FEATURE_LEVEL_12_0, D3D_FEATURE_LEVEL_11_1,
        //                                                D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
        //featureLevels.erase(std::remove_if(featureLevels.begin(), featureLevels.end(),
        //                                   [&](D3D_FEATURE_LEVEL fl) { return fl < graphicsRequirements.minFeatureLevel; }),
        //                    featureLevels.end());
        //CHECK_MSG(featureLevels.size() != 0, "Unsupported minimum feature level!");

        //InitializeD3D11DeviceForAdapter(adapter.Get(), featureLevels, m_device.ReleaseAndGetAddressOf(),
        //                                m_deviceContext.ReleaseAndGetAddressOf());

        InitializeResources();

		auto intern = bgfx::getInternalData();
        
		m_graphicsBinding.device = (ID3D11Device*)intern->context;

        //m_graphicsBinding.device = m_device.Get();
    }

    void InitializeResources() {
        // BGFX
        // Create vertex stream declaration.
        PosColorVertex::init();

        // Create static vertex buffer.
        m_vbh = bgfx::createVertexBuffer(
            // Static data can be passed with bgfx::makeRef
            bgfx::makeRef(s_cubeVertices, sizeof(s_cubeVertices)), PosColorVertex::ms_decl);

        // Create static index buffer for triangle list rendering.
        m_ibh[0] = bgfx::createIndexBuffer(
            // Static data can be passed with bgfx::makeRef
            bgfx::makeRef(s_cubeTriList, sizeof(s_cubeTriList)));

        // Create static index buffer for triangle strip rendering.
        m_ibh[1] = bgfx::createIndexBuffer(
            // Static data can be passed with bgfx::makeRef
            bgfx::makeRef(s_cubeTriStrip, sizeof(s_cubeTriStrip)));

        // Create static index buffer for line list rendering.
        m_ibh[2] = bgfx::createIndexBuffer(
            // Static data can be passed with bgfx::makeRef
            bgfx::makeRef(s_cubeLineList, sizeof(s_cubeLineList)));

        // Create static index buffer for line strip rendering.
        m_ibh[3] = bgfx::createIndexBuffer(
            // Static data can be passed with bgfx::makeRef
            bgfx::makeRef(s_cubeLineStrip, sizeof(s_cubeLineStrip)));

        // Create static index buffer for point list rendering.
        m_ibh[4] = bgfx::createIndexBuffer(
            // Static data can be passed with bgfx::makeRef
            bgfx::makeRef(s_cubePoints, sizeof(s_cubePoints)));

        // Create program from shaders.
        s_fileReader = BX_NEW(getDefaultAllocator(), FileReader);
        auto _fsName = "fs_cubes";
        auto _vsName = "vs_cubes";
        bgfx::ShaderHandle vsh = loadShader(s_fileReader, _vsName);
        bgfx::ShaderHandle fsh = BGFX_INVALID_HANDLE;
        if (NULL != _fsName) {
            fsh = loadShader(s_fileReader, _fsName);
        }

        bgfx::createProgram(vsh, fsh, true /* destroy shaders when program is destroyed */);
        // m_program = loadProgram("vs_cubes", "fs_cubes");

        // m_timeOffset = bx::getHPCounter();

        // DX
     /*   const ComPtr<ID3DBlob> vertexShaderBytes = CompileShader(ShaderHlsl, "MainVS", "vs_5_0");
        CHECK_HRCMD(m_device->CreateVertexShader(vertexShaderBytes->GetBufferPointer(), vertexShaderBytes->GetBufferSize(), nullptr,
                                                 m_vertexShader.ReleaseAndGetAddressOf()));

        const ComPtr<ID3DBlob> pixelShaderBytes = CompileShader(ShaderHlsl, "MainPS", "ps_5_0");
        CHECK_HRCMD(m_device->CreatePixelShader(pixelShaderBytes->GetBufferPointer(), pixelShaderBytes->GetBufferSize(), nullptr,
                                                m_pixelShader.ReleaseAndGetAddressOf()));

        const D3D11_INPUT_ELEMENT_DESC vertexDesc[] = {
            {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0},
            {"COLOR", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0},
        };

        CHECK_HRCMD(m_device->CreateInputLayout(vertexDesc, (UINT)ArraySize(vertexDesc), vertexShaderBytes->GetBufferPointer(),
                                                vertexShaderBytes->GetBufferSize(), &m_inputLayout));

        const CD3D11_BUFFER_DESC modelConstantBufferDesc(sizeof(ModelConstantBuffer), D3D11_BIND_CONSTANT_BUFFER);
        CHECK_HRCMD(m_device->CreateBuffer(&modelConstantBufferDesc, nullptr, m_modelCBuffer.ReleaseAndGetAddressOf()));

        const CD3D11_BUFFER_DESC viewProjectionConstantBufferDesc(sizeof(ViewProjectionConstantBuffer), D3D11_BIND_CONSTANT_BUFFER);
        CHECK_HRCMD(
            m_device->CreateBuffer(&viewProjectionConstantBufferDesc, nullptr, m_viewProjectionCBuffer.ReleaseAndGetAddressOf()));

        const D3D11_SUBRESOURCE_DATA vertexBufferData{Geometry::c_cubeVertices};
        const CD3D11_BUFFER_DESC vertexBufferDesc(sizeof(Geometry::c_cubeVertices), D3D11_BIND_VERTEX_BUFFER);
        CHECK_HRCMD(m_device->CreateBuffer(&vertexBufferDesc, &vertexBufferData, m_cubeVertexBuffer.ReleaseAndGetAddressOf()));

        const D3D11_SUBRESOURCE_DATA indexBufferData{Geometry::c_cubeIndices};
        const CD3D11_BUFFER_DESC indexBufferDesc(sizeof(Geometry::c_cubeIndices), D3D11_BIND_INDEX_BUFFER);
        CHECK_HRCMD(m_device->CreateBuffer(&indexBufferDesc, &indexBufferData, m_cubeIndexBuffer.ReleaseAndGetAddressOf()));*/
    }

    int64_t SelectColorSwapchainFormat(const std::vector<int64_t>& runtimeFormats) const override {
        // List of supported color swapchain formats, in priority order.
        constexpr DXGI_FORMAT SupportedColorSwapchainFormats[] = {
            DXGI_FORMAT_R8G8B8A8_UNORM,
            DXGI_FORMAT_B8G8R8A8_UNORM,
            DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,
            DXGI_FORMAT_B8G8R8A8_UNORM_SRGB,
        };

        auto swapchainFormatIt =
            std::find_first_of(std::begin(SupportedColorSwapchainFormats), std::end(SupportedColorSwapchainFormats),
                               runtimeFormats.begin(), runtimeFormats.end());
        if (swapchainFormatIt == std::end(SupportedColorSwapchainFormats)) {
            THROW("No runtime swapchain format supported for color swapchain");
        }
        std::cout << (*swapchainFormatIt) << "IS THE \n";
        return *swapchainFormatIt;
    }

    const XrBaseInStructure* GetGraphicsBinding() const override {
        return reinterpret_cast<const XrBaseInStructure*>(&m_graphicsBinding);
    }

    std::vector<XrSwapchainImageBaseHeader*> AllocateSwapchainImageStructs(
        uint32_t capacity, const XrSwapchainCreateInfo& /*swapchainCreateInfo*/) override {
        // Allocate and initialize the buffer of image structs (must be sequential in memory for xrEnumerateSwapchainImages).
        // Return back an array of pointers to each swapchain image struct so the consumer doesn't need to know the type/size.
        std::vector<XrSwapchainImageD3D11KHR> swapchainImageBuffer(capacity);
        std::vector<XrSwapchainImageBaseHeader*> swapchainImageBase;

        for (XrSwapchainImageD3D11KHR& image : swapchainImageBuffer) {
            image.type = XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR;
            swapchainImageBase.push_back(reinterpret_cast<XrSwapchainImageBaseHeader*>(&image));
        }

        // Keep the buffer alive by moving it into the list of buffers.
        m_swapchainImageBuffers.push_back(std::move(swapchainImageBuffer));

        return swapchainImageBase;
    }

    //ComPtr<ID3D11DepthStencilView> GetDepthStencilView(ID3D11Texture2D* colorTexture) {
    //    // If a depth-stencil view has already been created for this back-buffer, use it.
    //    auto depthBufferIt = m_colorToDepthMap.find(colorTexture);
    //    if (depthBufferIt != m_colorToDepthMap.end()) {
    //        return depthBufferIt->second;
    //    }

    //    // This back-buffer has no cooresponding depth-stencil texture, so create one with matching dimensions.
    //    D3D11_TEXTURE2D_DESC colorDesc;
    //    colorTexture->GetDesc(&colorDesc);

    //    D3D11_TEXTURE2D_DESC depthDesc{};
    //    depthDesc.Width = colorDesc.Width;
    //    depthDesc.Height = colorDesc.Height;
    //    depthDesc.ArraySize = colorDesc.ArraySize;
    //    depthDesc.MipLevels = 1;
    //    depthDesc.Format = DXGI_FORMAT_R32_TYPELESS;
    //    depthDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_DEPTH_STENCIL;
    //    depthDesc.SampleDesc.Count = 1;
    //    ComPtr<ID3D11Texture2D> depthTexture;
    //    CHECK_HRCMD(m_device->CreateTexture2D(&depthDesc, nullptr, depthTexture.ReleaseAndGetAddressOf()));

    //    // Create and cache the depth stencil view.
    //    ComPtr<ID3D11DepthStencilView> depthStencilView;
    //    CD3D11_DEPTH_STENCIL_VIEW_DESC depthStencilViewDesc(D3D11_DSV_DIMENSION_TEXTURE2D, DXGI_FORMAT_D32_FLOAT);
    //    CHECK_HRCMD(m_device->CreateDepthStencilView(depthTexture.Get(), &depthStencilViewDesc, depthStencilView.GetAddressOf()));
    //    depthBufferIt = m_colorToDepthMap.insert(std::make_pair(colorTexture, depthStencilView)).first;

    //    return depthStencilView;
    //}

    void RenderView(const XrCompositionLayerProjectionView& layerView, const XrSwapchainImageBaseHeader* swapchainImage,
                    int64_t swapchainFormat, const std::vector<Cube>& cubes) override {
        auto x = swapchainFormat || cubes.size();
        if (x) {
            std::cout << "tmp\n";
		}

        // Shared
        CHECK(layerView.subImage.imageArrayIndex == 0);  // Texture arrays not supported.
        ID3D11Texture2D* const colorTexture = reinterpret_cast<const XrSwapchainImageD3D11KHR*>(swapchainImage)->texture;

        // BGFX
        // bgfx::TextureHandle textures[2] =
        // bgfx::createTexture2D()
        // bgfx::Attachment at;
        bgfx::ViewId view = 0;
        bgfx::setViewName(view, "standard view");
        bgfx::setViewRect(view, (uint16_t)layerView.subImage.imageRect.offset.x, (uint16_t)layerView.subImage.imageRect.offset.y,
                          (uint16_t)layerView.subImage.imageRect.extent.width,
                          (uint16_t)layerView.subImage.imageRect.extent.height);
        bgfx::setViewClear(view, BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH, 0xff3030ff, 1.0f, 0);

        // auto t = bgfx::createTexture2D(1, 1, false, 1, bgfx::TextureFormat::BGRA8);
        // bgfx::TextureHandle textureHandle;

        // bgfx::gl::
        // bgfx::setViewFrameBuffer(view, frameBuffer);

        // DX
        //auto i = false;
        //if (i) {
        //    CD3D11_VIEWPORT viewport((float)layerView.subImage.imageRect.offset.x, (float)layerView.subImage.imageRect.offset.y,
        //                             (float)layerView.subImage.imageRect.extent.width,
        //                             (float)layerView.subImage.imageRect.extent.height);
        //    m_deviceContext->RSSetViewports(1, &viewport);

        //    // Create RenderTargetView with original swapchain format (swapchain is typeless).
        //    ComPtr<ID3D11RenderTargetView> renderTargetView;
        //    const CD3D11_RENDER_TARGET_VIEW_DESC renderTargetViewDesc(D3D11_RTV_DIMENSION_TEXTURE2D, (DXGI_FORMAT)swapchainFormat);
        //    CHECK_HRCMD(
        //        m_device->CreateRenderTargetView(colorTexture, &renderTargetViewDesc, renderTargetView.ReleaseAndGetAddressOf()));

        //    const ComPtr<ID3D11DepthStencilView> depthStencilView = GetDepthStencilView(colorTexture);

        //    // Clear swapchain and depth buffer. NOTE: This will clear the entire render target view, not just the specified
        //    // view.
        //    // TODO: Do not clear to a color when using a pass-through view configuration.
        //    m_deviceContext->ClearRenderTargetView(renderTargetView.Get(), DirectX::Colors::DarkSlateGray);
        //    m_deviceContext->ClearDepthStencilView(depthStencilView.Get(), D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);

        //    ID3D11RenderTargetView* renderTargets[] = {renderTargetView.Get()};
        //    m_deviceContext->OMSetRenderTargets((UINT)ArraySize(renderTargets), renderTargets, depthStencilView.Get());

        //    const XMMATRIX spaceToView = XMMatrixInverse(nullptr, LoadXrPose(layerView.pose));
        //    XrMatrix4x4f projectionMatrix;
        //    XrMatrix4x4f_CreateProjectionFov(&projectionMatrix, GRAPHICS_D3D, layerView.fov, 0.05f, 100.0f);

        //    // Set shaders and constant buffers.
        //    ViewProjectionConstantBuffer viewProjection;
        //    XMStoreFloat4x4(&viewProjection.ViewProjection, XMMatrixTranspose(spaceToView * LoadXrMatrix(projectionMatrix)));
        //    m_deviceContext->UpdateSubresource(m_viewProjectionCBuffer.Get(), 0, nullptr, &viewProjection, 0, 0);

        //    ID3D11Buffer* const constantBuffers[] = {m_modelCBuffer.Get(), m_viewProjectionCBuffer.Get()};
        //    m_deviceContext->VSSetConstantBuffers(0, (UINT)ArraySize(constantBuffers), constantBuffers);
        //    m_deviceContext->VSSetShader(m_vertexShader.Get(), nullptr, 0);
        //    m_deviceContext->PSSetShader(m_pixelShader.Get(), nullptr, 0);

        //    // Set cube primitive data.
        //    const UINT strides[] = {sizeof(Geometry::Vertex)};
        //    const UINT offsets[] = {0};
        //    ID3D11Buffer* vertexBuffers[] = {m_cubeVertexBuffer.Get()};
        //    m_deviceContext->IASetVertexBuffers(0, (UINT)ArraySize(vertexBuffers), vertexBuffers, strides, offsets);
        //    m_deviceContext->IASetIndexBuffer(m_cubeIndexBuffer.Get(), DXGI_FORMAT_R16_UINT, 0);
        //    m_deviceContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        //    m_deviceContext->IASetInputLayout(m_inputLayout.Get());

        //    // Render each cube
        //    for (const Cube& cube : cubes) {
        //        // Compute and update the model transform.
        //        ModelConstantBuffer model;
        //        XMStoreFloat4x4(&model.Model, XMMatrixTranspose(XMMatrixScaling(cube.Scale.x, cube.Scale.y, cube.Scale.z) *
        //                                                        LoadXrPose(cube.Pose)));
        //        m_deviceContext->UpdateSubresource(m_modelCBuffer.Get(), 0, nullptr, &model, 0, 0);

        //        // Draw the cube.
        //        m_deviceContext->DrawIndexed((UINT)ArraySize(Geometry::c_cubeIndices), 0, 0);
        //    }
        //}

        std::cout << "Render\n";
        if (!started){
            D3D11_TEXTURE2D_DESC colorDesc;
            colorTexture->GetDesc(&colorDesc);

            _textures[0] =
                bgfx::createTexture2D((uint16_t)colorDesc.Width, (uint16_t)colorDesc.Height, false, 1, bgfx::TextureFormat::RGBA8, BGFX_TEXTURE_RT);
           
            started = true;
		}
		 bgfx::overrideInternal(_textures[0], (uintptr_t)colorTexture);
            bgfx::FrameBufferHandle frameBuffer = bgfx::createFrameBuffer(1, _textures);
            bgfx::setViewFrameBuffer(view, frameBuffer);
            bgfx::touch(view);
            bgfx::frame();
		
        
    }

    bgfx::TextureHandle _textures[1];
    bool started = false;

   private:
    // BGFX
    bx::FileReaderI* s_fileReader = NULL;

    bgfx::VertexBufferHandle m_vbh;
    bgfx::IndexBufferHandle m_ibh[BX_COUNTOF(s_ptState)];
    bgfx::ProgramHandle m_program;
    int64_t m_timeOffset;
    int32_t m_pt;

    bool m_r;
    bool m_g;
    bool m_b;
    bool m_a;

    // Shared
    std::list<std::vector<XrSwapchainImageD3D11KHR>> m_swapchainImageBuffers;

    // DX
    ComPtr<ID3D11Device> m_device;
    ComPtr<ID3D11DeviceContext> m_deviceContext;
    XrGraphicsBindingD3D11KHR m_graphicsBinding{XR_TYPE_GRAPHICS_BINDING_D3D11_KHR};
    ComPtr<ID3D11VertexShader> m_vertexShader;
    ComPtr<ID3D11PixelShader> m_pixelShader;
    ComPtr<ID3D11InputLayout> m_inputLayout;
    ComPtr<ID3D11Buffer> m_modelCBuffer;
    ComPtr<ID3D11Buffer> m_viewProjectionCBuffer;
    ComPtr<ID3D11Buffer> m_cubeVertexBuffer;
    ComPtr<ID3D11Buffer> m_cubeIndexBuffer;

    // Map color buffer to associated depth buffer. This map is populated on demand.
    std::map<ID3D11Texture2D*, ComPtr<ID3D11DepthStencilView>> m_colorToDepthMap;
};
}  // namespace

std::shared_ptr<IGraphicsPlugin> CreateGraphicsPlugin_D3D11(const std::shared_ptr<Options>& options,
                                                            std::shared_ptr<IPlatformPlugin> platformPlugin) {
    return std::make_shared<D3D11GraphicsPlugin>(options, platformPlugin);
}

#endif
