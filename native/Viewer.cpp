// Native asset/animation development viewer. No emulator and no gameplay simulation.
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include "Geometry.h"
#include <chrono>
#include <thread>
#include <iostream>
#include <iomanip>
#include <sstream>
#pragma comment(lib,"d3d11.lib")
#pragma comment(lib,"dxgi.lib")
#pragma comment(lib,"d3dcompiler.lib")
using Microsoft::WRL::ComPtr;
using namespace NativeMelee;
static void Check(HRESULT hr) { if (FAILED(hr)) throw std::runtime_error("D3D failure " + std::to_string(hr)); }
static LRESULT CALLBACK WindowProc(HWND h,UINT m,WPARAM w,LPARAM l) {
  if (m==WM_CLOSE) { DestroyWindow(h); return 0; }
  if (m==WM_DESTROY) { PostQuitMessage(0); return 0; }
  return DefWindowProcW(h,m,w,l);
}
struct DrawVertex { Vec position,color; std::array<float,2> uv; Vec normal; };
static void Capture(ID3D11Device* device,ID3D11DeviceContext* context,ID3D11Texture2D* image,const std::string& path) {
  D3D11_TEXTURE2D_DESC desc; image->GetDesc(&desc);
  desc.Usage=D3D11_USAGE_STAGING; desc.BindFlags=0; desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ; desc.MiscFlags=0;
  ComPtr<ID3D11Texture2D> staging; Check(device->CreateTexture2D(&desc,nullptr,&staging));
  context->CopyResource(staging.Get(),image);
  D3D11_MAPPED_SUBRESOURCE map; Check(context->Map(staging.Get(),0,D3D11_MAP_READ,0,&map));
  std::ofstream file(path,std::ios::binary);
  if (!file) { context->Unmap(staging.Get(),0); throw std::runtime_error("Cannot write capture"); }
  file << "P6\n" << desc.Width << ' ' << desc.Height << "\n255\n";
  for (UINT y=0;y<desc.Height;++y) for (UINT x=0;x<desc.Width;++x)
    file.write(static_cast<const char*>(map.pData)+y*map.RowPitch+x*4,3);
  context->Unmap(staging.Get(),0);
  if (!file) throw std::runtime_error("Capture write failed");
}
int main(int argc,char** argv) {
  try {
    std::string assets="runtime/native-assets",capture;
    double fps=240,seconds=0,absolute_frame=-1;
    size_t animation_index=0;
    for (int i=1;i<argc;++i) {
      std::string argument=argv[i];
      if (i+1>=argc) throw std::runtime_error("Missing argument value");
      std::string value=argv[++i];
      if (argument=="--assets") assets=value;
      else if (argument=="--fps") fps=std::stod(value);
      else if (argument=="--seconds") seconds=std::stod(value);
      else if (argument=="--frame") absolute_frame=std::stod(value);
      else if (argument=="--animation") animation_index=std::stoul(value);
      else if (argument=="--capture") capture=value;
      else throw std::runtime_error("Unknown argument " + argument);
    }
    if (!std::isfinite(fps) || fps<0 || !std::isfinite(seconds) || seconds<0 || !std::isfinite(absolute_frame))
      throw std::runtime_error("Invalid rate/time");
    Archive model(assets+"/PlFcNr.dat"),bundle(assets+"/PlFcAJ.dat");
    Rig rig(model); Mesh mesh=LoadMesh(model,rig); auto animations=LoadAnimations(bundle);
    const auto& animation=animations.at(animation_index);
    if (animation.frames<=0 || animation.nodes.size()!=rig.joints.size()) throw std::runtime_error("Animation/rig mismatch");
    std::cout << "NATIVE DEVELOPMENT VIEWER: " << mesh.triangles.size()/3 << " triangles, "
              << rig.joints.size() << " joints. No gameplay, rollback, RTX or DLSS yet.\n"
              << animation.name << "\n";
    const auto initial=Pose(rig,&animation,0);
    Vec low{{1e9f,1e9f,1e9f}},high{{-1e9f,-1e9f,-1e9f}};
    for(const auto& vertex:mesh.triangles) {
      const auto p=Skin(vertex,rig,initial);
      for(int k=0;k<3;++k) {low[k]=std::min(low[k],p[k]);high[k]=std::max(high[k],p[k]);}
    }
    std::cout << "Pose bounds: " << low[0] << ',' << low[1] << ',' << low[2] << " to " << high[0] << ',' << high[1] << ',' << high[2] << '\n';
    HINSTANCE instance=GetModuleHandleW(nullptr);
    WNDCLASSW wc{}; wc.hInstance=instance; wc.lpfnWndProc=WindowProc; wc.lpszClassName=L"MeleeNativeAssetViewer"; wc.hCursor=LoadCursor(nullptr,IDC_ARROW);
    if(!RegisterClassW(&wc) && GetLastError()!=ERROR_CLASS_ALREADY_EXISTS) throw std::runtime_error("Window class failed");
    RECT rect{0,0,960,720}; AdjustWindowRect(&rect,WS_OVERLAPPEDWINDOW,FALSE);
    HWND window=CreateWindowExW(WS_EX_NOACTIVATE,wc.lpszClassName,L"Melee native asset viewer — development only",WS_OVERLAPPEDWINDOW,
      CW_USEDEFAULT,CW_USEDEFAULT,rect.right-rect.left,rect.bottom-rect.top,nullptr,nullptr,instance,nullptr);
    if(!window) throw std::runtime_error("Window creation failed");
    struct WindowGuard { HWND h; ~WindowGuard(){if(IsWindow(h))DestroyWindow(h);} } window_guard{window};
    DXGI_SWAP_CHAIN_DESC sd{}; sd.BufferDesc.Width=960;sd.BufferDesc.Height=720;sd.BufferDesc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.SampleDesc.Count=1;sd.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;sd.BufferCount=2;sd.OutputWindow=window;sd.Windowed=TRUE;
    ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;ComPtr<IDXGISwapChain> swap;
    Check(D3D11CreateDeviceAndSwapChain(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&sd,&swap,&device,nullptr,&context));
    ComPtr<ID3D11Texture2D> backbuffer;Check(swap->GetBuffer(0,IID_PPV_ARGS(&backbuffer)));
    ComPtr<ID3D11RenderTargetView> target;Check(device->CreateRenderTargetView(backbuffer.Get(),nullptr,&target));
    D3D11_TEXTURE2D_DESC depth_desc{};depth_desc.Width=960;depth_desc.Height=720;depth_desc.MipLevels=1;depth_desc.ArraySize=1;
    depth_desc.Format=DXGI_FORMAT_D24_UNORM_S8_UINT;depth_desc.SampleDesc.Count=1;depth_desc.BindFlags=D3D11_BIND_DEPTH_STENCIL;
    ComPtr<ID3D11Texture2D> depth;Check(device->CreateTexture2D(&depth_desc,nullptr,&depth));
    ComPtr<ID3D11DepthStencilView> depth_view;Check(device->CreateDepthStencilView(depth.Get(),nullptr,&depth_view));
    const char* shader=R"(
cbuffer Camera : register(b0) { float4 centerScale; };
Texture2D Albedo:register(t0);SamplerState LinearSampler:register(s0);
struct Output { float4 position:SV_POSITION;float3 world:TEXCOORD0;float3 color:COLOR0;float2 uv:TEXCOORD1;float3 normal:TEXCOORD2; };
Output VS(float3 p:POSITION,float3 color:COLOR0,float2 uv:TEXCOORD0,float3 normal:NORMAL0) {
  Output o;float3 q=(p-centerScale.xyz)*centerScale.w;
  float3 view=float3(.8*q.x+.6*q.z,q.y,.6*q.x-.8*q.z);
  o.position=float4(view.x*.75,view.y,view.z*.1+.5,1);o.world=p;o.color=color;o.uv=uv;o.normal=normal;return o;
}
float4 PS(Output i):SV_TARGET {
  float3 n=normalize(i.normal);
  float light=.3+.7*abs(dot(n,normalize(float3(.3,.8,.5))));
  float4 texel=Albedo.Sample(LinearSampler,i.uv);clip(texel.a-.4);
  return float4(i.color*texel.rgb*light,1);
})";
    ComPtr<ID3DBlob> vs_blob,ps_blob,error;
    Check(D3DCompile(shader,strlen(shader),nullptr,nullptr,nullptr,"VS","vs_5_0",0,0,&vs_blob,&error));
    Check(D3DCompile(shader,strlen(shader),nullptr,nullptr,nullptr,"PS","ps_5_0",0,0,&ps_blob,&error));
    ComPtr<ID3D11VertexShader> vs;ComPtr<ID3D11PixelShader> ps;
    Check(device->CreateVertexShader(vs_blob->GetBufferPointer(),vs_blob->GetBufferSize(),nullptr,&vs));
    Check(device->CreatePixelShader(ps_blob->GetBufferPointer(),ps_blob->GetBufferSize(),nullptr,&ps));
    D3D11_INPUT_ELEMENT_DESC elements[]={{"POSITION",0,DXGI_FORMAT_R32G32B32_FLOAT,0,0,D3D11_INPUT_PER_VERTEX_DATA,0},
                                     {"COLOR",0,DXGI_FORMAT_R32G32B32_FLOAT,0,12,D3D11_INPUT_PER_VERTEX_DATA,0},
                                     {"TEXCOORD",0,DXGI_FORMAT_R32G32_FLOAT,0,24,D3D11_INPUT_PER_VERTEX_DATA,0},
                                     {"NORMAL",0,DXGI_FORMAT_R32G32B32_FLOAT,0,32,D3D11_INPUT_PER_VERTEX_DATA,0}};
    ComPtr<ID3D11InputLayout> layout;Check(device->CreateInputLayout(elements,4,vs_blob->GetBufferPointer(),vs_blob->GetBufferSize(),&layout));
    std::vector<ComPtr<ID3D11ShaderResourceView>> texture_views;
    for(const auto& texture:mesh.textures) {
      D3D11_TEXTURE2D_DESC desc{};desc.Width=texture.width;desc.Height=texture.height;
      desc.MipLevels=1;desc.ArraySize=1;desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;desc.SampleDesc.Count=1;
      desc.Usage=D3D11_USAGE_IMMUTABLE;desc.BindFlags=D3D11_BIND_SHADER_RESOURCE;
      D3D11_SUBRESOURCE_DATA data{texture.pixels.data(),texture.width*4,0};
      ComPtr<ID3D11Texture2D> image;Check(device->CreateTexture2D(&desc,&data,&image));
      ComPtr<ID3D11ShaderResourceView> view;Check(device->CreateShaderResourceView(image.Get(),nullptr,&view));
      texture_views.push_back(view);
    }
    D3D11_SAMPLER_DESC sampler_desc{};sampler_desc.Filter=D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampler_desc.AddressU=sampler_desc.AddressV=sampler_desc.AddressW=D3D11_TEXTURE_ADDRESS_WRAP;sampler_desc.MaxLOD=D3D11_FLOAT32_MAX;
    ComPtr<ID3D11SamplerState> sampler;Check(device->CreateSamplerState(&sampler_desc,&sampler));
    ID3D11SamplerState* sampler_ptr=sampler.Get();context->PSSetSamplers(0,1,&sampler_ptr);
    D3D11_BUFFER_DESC bd{};bd.ByteWidth=static_cast<UINT>(mesh.triangles.size()*sizeof(DrawVertex));bd.Usage=D3D11_USAGE_DYNAMIC;
    bd.BindFlags=D3D11_BIND_VERTEX_BUFFER;bd.CPUAccessFlags=D3D11_CPU_ACCESS_WRITE;
    ComPtr<ID3D11Buffer> vertices;Check(device->CreateBuffer(&bd,nullptr,&vertices));
    float camera[4]={(low[0]+high[0])*.5f,(low[1]+high[1])*.5f,(low[2]+high[2])*.5f,1.65f/std::max(high[1]-low[1],1.f)};
    bd={};bd.ByteWidth=16;bd.Usage=D3D11_USAGE_IMMUTABLE;bd.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
    D3D11_SUBRESOURCE_DATA initial_camera{camera,0,0};ComPtr<ID3D11Buffer> constants;Check(device->CreateBuffer(&bd,&initial_camera,&constants));
    D3D11_RASTERIZER_DESC rd{};rd.FillMode=D3D11_FILL_SOLID;rd.CullMode=D3D11_CULL_NONE;rd.DepthClipEnable=TRUE;
    ComPtr<ID3D11RasterizerState> raster;Check(device->CreateRasterizerState(&rd,&raster));context->RSSetState(raster.Get());
    D3D11_VIEWPORT viewport{0,0,960,720,0,1};context->RSSetViewports(1,&viewport);
    context->IASetInputLayout(layout.Get());context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    UINT stride=sizeof(DrawVertex),offset=0;ID3D11Buffer* vb=vertices.Get();context->IASetVertexBuffers(0,1,&vb,&stride,&offset);
    context->VSSetShader(vs.Get(),nullptr,0);context->PSSetShader(ps.Get(),nullptr,0);
    ID3D11Buffer* cb=constants.Get();context->VSSetConstantBuffers(0,1,&cb);
    ID3D11RenderTargetView* rtv=target.Get();context->OMSetRenderTargets(1,&rtv,depth_view.Get());
    ShowWindow(window,SW_SHOWNOACTIVATE);
    const auto start=std::chrono::steady_clock::now();auto deadline=start;
    uint64_t draws=0;bool running=true,captured=false;double last_report=0;uint64_t reported_draws=0;
    while(running) {
      MSG message;while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)) {if(message.message==WM_QUIT)running=false;TranslateMessage(&message);DispatchMessageW(&message);}
      if(!running)break;
      const auto now=std::chrono::steady_clock::now();const double elapsed=std::chrono::duration<double>(now-start).count();
      if(seconds>0 && elapsed>=seconds)break;
      if(fps>0 && now<deadline) {std::this_thread::yield();continue;}
      if(fps>0) {
        const double late=std::chrono::duration<double>(now-deadline).count();
        deadline+=std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>((std::floor(late*fps)+1)/fps));
      }
      const float frame=static_cast<float>(absolute_frame>=0?absolute_frame:std::fmod(elapsed*60,animation.frames));
      const auto pose=Pose(rig,&animation,frame);
      const auto bound=SkinMatrices(rig,pose);
      D3D11_MAPPED_SUBRESOURCE mapped;Check(context->Map(vertices.Get(),0,D3D11_MAP_WRITE_DISCARD,0,&mapped));
      auto* output=static_cast<DrawVertex*>(mapped.pData);
      for(size_t i=0;i<mesh.triangles.size();++i) {
        // Mapped upload memory can be write-combined: never read/accumulate in it.
        DrawVertex vertex;
        SkinVertex(mesh.triangles[i],pose,bound,vertex.position,vertex.normal);
        vertex.color=mesh.triangles[i].color;vertex.uv=mesh.triangles[i].uv;
        output[i]=vertex;
      }
      context->Unmap(vertices.Get(),0);
      const float background[]={.035f,.045f,.065f,1};context->ClearRenderTargetView(target.Get(),background);
      context->ClearDepthStencilView(depth_view.Get(),D3D11_CLEAR_DEPTH,1,0);
      for(const auto& batch:mesh.batches) {
        ID3D11ShaderResourceView* view=texture_views[batch.texture].Get();context->PSSetShaderResources(0,1,&view);
        context->Draw(static_cast<UINT>(batch.count),static_cast<UINT>(batch.start));
      }
      if(!capture.empty() && !captured) {Capture(device.Get(),context.Get(),backbuffer.Get(),capture);captured=true;}
      Check(swap->Present(0,0));++draws;
      if(elapsed-last_report>=1) {
        double measured=(draws-reported_draws)/(elapsed-last_report);
        std::wostringstream title;title<<L"Melee native asset viewer — "<<std::fixed<<std::setprecision(1)<<measured<<L" geometry draws/s — development only";
        SetWindowTextW(window,title.str().c_str());
        std::cout<<"geometry_draws_per_second="<<measured<<" animation_frame="<<frame<<'\n';
        last_report=elapsed;reported_draws=draws;
      }
    }
    context->ClearState();context->Flush();
    return 0;
  } catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
}
