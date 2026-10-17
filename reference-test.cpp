// Compare native NV12 presentation with CPU-generated final-size RGB.
// CPU reference modes perform no GPU YUV conversion or image resizing.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11_1.h>
#include <dxgi1_4.h>
#include <dcomp.h>
#include <wrl/client.h>
#include <filesystem>
#include <fstream>
#include <vector>
#include <string>
#include <sstream>
#include <stdexcept>
#include <algorithm>
#include <cstring>
#include <cwchar>
using Microsoft::WRL::ComPtr;
constexpr int W=640,H=360;
std::filesystem::path folder,assetFolder;
std::ofstream logFile;
HWND windowHandle=nullptr;
bool ready=false,inTimer=false,selfTest=false,crop=false,captureCycle=false,failed=false;
int selection=1,outputWidth=639,lastMode=-99;
ULONGLONG lastHud=0;
ULONGLONG cycleStart=0;
int cycleStep=0;
HANDLE surfaceHandle=nullptr;
ComPtr<ID3D11Device> device;
ComPtr<ID3D11DeviceContext> context;
ComPtr<IDXGIFactory2> factory;
ComPtr<IDXGIFactoryMedia> mediaFactory;
ComPtr<IDCompositionDevice> composition;
ComPtr<IDCompositionTarget> target;
ComPtr<IDCompositionVisual> rootVisual,imageVisual;
ComPtr<IDXGISwapChain1> background,imageChain;
ComPtr<IDXGISwapChainMedia> mediaStats;
ComPtr<ID3D11Texture2D> sourceTexture;

void check(HRESULT hr,const char* operation){
  if(FAILED(hr)){std::ostringstream s;s<<operation<<" failed: 0x"<<std::hex<<UINT(hr);throw std::runtime_error(s.str());}
}
const char* modeLabel(){
  switch(selection){case 1:return "native-NV12";case 2:return "zimg-linear";case 3:return "zimg-cubic";case 4:return "cpu-rgb-first";default:return "cpu-linear";}
}
const wchar_t* wideLabel(){
  switch(selection){case 1:return L"1: Native NV12 hardware path";case 2:return L"2: CPU zimg BILINEAR reference";case 3:return L"3: CPU zimg BICUBIC reference";case 4:return L"4: CPU RGB-first then bilinear resize";default:return L"5: Independent CPU bilinear reference";}
}
const wchar_t* presentationName(){
  switch(lastMode){case 0:return L"COMPOSED";case 1:return L"HARDWARE OVERLAY";case 2:return L"NONE";default:return L"pending";}
}
int bufferWidth(){return selection==1?W:outputWidth;}
void record(const char* event){
  RECT rect={};GetWindowRect(windowHandle,&rect);
  logFile<<GetTickCount64()<<" "<<event<<" path="<<modeLabel()<<" buffer="<<bufferWidth()<<"x"<<H
    <<" output="<<outputWidth<<"x"<<H<<" geometry="<<(crop?"crop-right-1":"full-source-resize")
    <<" GPU_resize="<<(selection==1&&!crop&&outputWidth!=W)
    <<" offset=24,120 window="<<rect.left<<","<<rect.top<<" foreground="<<(GetForegroundWindow()==windowHandle)<<" composition_mode="<<lastMode
    <<" window_dpi="<<GetDpiForWindow(windowHandle)<<" per_monitor_v2="<<AreDpiAwarenessContextsEqual(GetWindowDpiAwarenessContext(windowHandle),DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)<<"\n";logFile.flush();
}
std::vector<unsigned char> readBytes(const std::filesystem::path& path,size_t count){
  std::ifstream file(path,std::ios::binary|std::ios::ate);
  if(!file||file.tellg()!=std::streamoff(count))throw std::runtime_error("Missing or incorrectly sized reference asset");
  std::vector<unsigned char> bytes(count);file.seekg(0);file.read(reinterpret_cast<char*>(bytes.data()),std::streamsize(count));
  if(!file)throw std::runtime_error("Cannot read reference asset");return bytes;
}
void prepareTexture(){
  bool nv12=selection==1;int width=bufferWidth();UINT pitch=UINT(width*(nv12?1:4));
  size_t rows=nv12?H*3/2:H;
  std::filesystem::path path;
  if(nv12)path=assetFolder/L"graph-640x360.nv12";
  else path=assetFolder/((crop?std::string("crop-"):std::string("scale-"))+std::to_string(outputWidth)+"-"+modeLabel()+".bgra");
  auto bytes=readBytes(path,size_t(pitch)*rows);
  D3D11_TEXTURE2D_DESC d={};d.Width=UINT(width);d.Height=H;d.MipLevels=1;d.ArraySize=1;
  d.Format=nv12?DXGI_FORMAT_NV12:DXGI_FORMAT_B8G8R8A8_UNORM;d.SampleDesc.Count=1;
  d.Usage=D3D11_USAGE_DEFAULT;d.BindFlags=D3D11_BIND_RENDER_TARGET;
  D3D11_SUBRESOURCE_DATA initial={};initial.pSysMem=bytes.data();initial.SysMemPitch=pitch;
  ComPtr<ID3D11Texture2D> next;check(device->CreateTexture2D(&d,&initial,&next),"Upload texture");
  d.Usage=D3D11_USAGE_STAGING;d.BindFlags=0;d.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
  ComPtr<ID3D11Texture2D> readback;check(device->CreateTexture2D(&d,nullptr,&readback),"Create readback");
  context->CopyResource(readback.Get(),next.Get());D3D11_MAPPED_SUBRESOURCE mapped={};
  check(context->Map(readback.Get(),0,D3D11_MAP_READ,0,&mapped),"Map readback");
  bool exact=true;
  for(size_t row=0;row<rows;row++)if(std::memcmp(static_cast<unsigned char*>(mapped.pData)+row*mapped.RowPitch,bytes.data()+row*pitch,pitch)!=0){exact=false;break;}
  context->Unmap(readback.Get(),0);
  if(!exact)throw std::runtime_error("GPU upload differs from CPU reference bytes");
  sourceTexture=std::move(next);
  logFile<<GetTickCount64()<<" VERIFIED byte-exact upload path="<<modeLabel()<<" bytes="<<bytes.size()<<"\n";logFile.flush();
}
void drawHud(){
  if(!background)return;RECT client={};GetClientRect(windowHandle,&client);
  UINT width=UINT(std::max<LONG>(client.right,1)),height=UINT(std::max<LONG>(client.bottom,1));
  DXGI_SWAP_CHAIN_DESC1 desc={};background->GetDesc1(&desc);
  if(desc.Width!=width||desc.Height!=height)check(background->ResizeBuffers(2,width,height,DXGI_FORMAT_UNKNOWN,0),"Resize background");
  BITMAPINFO bi={};bi.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);bi.bmiHeader.biWidth=LONG(width);bi.bmiHeader.biHeight=-LONG(height);
  bi.bmiHeader.biPlanes=1;bi.bmiHeader.biBitCount=32;bi.bmiHeader.biCompression=BI_RGB;
  void* pixels=nullptr;HDC dc=CreateCompatibleDC(nullptr);HBITMAP bitmap=CreateDIBSection(dc,&bi,DIB_RGB_COLORS,&pixels,nullptr,0);
  if(!bitmap){DeleteDC(dc);throw std::runtime_error("Cannot draw status text");}
  HGDIOBJ old=SelectObject(dc,bitmap);HBRUSH brush=CreateSolidBrush(RGB(23,23,23));FillRect(dc,&client,brush);DeleteObject(brush);
  SetBkMode(dc,TRANSPARENT);SetTextColor(dc,RGB(235,235,235));
  HFONT font=CreateFontW(-17,0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");
  HGDIOBJ oldFont=SelectObject(dc,font);
  std::wostringstream s;s<<wideLabel()<<L" | "<<presentationName()<<L"\n"
    <<(crop?L"Geometry: hide rightmost source pixel; NO RESIZE":L"Geometry: full source displayed at selected width; NO CROP")<<L" | visible "<<outputWidth<<L" x 360\n"
    <<(captureCycle?L"AUTOMATIC CAPTURE: NV12 crop -> CPU crop -> NV12 full -> NV12 crop\n":L"1 NV12 | 2 CPU bilinear | 3 CPU bicubic | 4 CPU RGB-first | 5 independent CPU bilinear\n")
    <<(captureCycle?L"Keep this window in front. It closes automatically after about 32 seconds. Esc stops early.\n":L"T: widths 640..636 | 0: width 640 | X: toggle right crop 1 | A/C optional notes | Esc close\n")
    <<(selection==1?L"Native path: source 640x360 NV12, display hardware reconstructs color":L"CPU reference: final-size BGRA, uploaded exactly, displayed 1:1; NO GPU YUV conversion");
  auto text=s.str();RECT area={24,8,LONG(width)-12,116};DrawTextW(dc,text.c_str(),-1,&area,DT_LEFT|DT_TOP|DT_NOPREFIX);
  ComPtr<ID3D11Texture2D> buffer;check(background->GetBuffer(0,IID_PPV_ARGS(&buffer)),"Get background buffer");
  context->UpdateSubresource(buffer.Get(),0,nullptr,pixels,width*4,0);
  SelectObject(dc,oldFont);SelectObject(dc,old);DeleteObject(font);DeleteObject(bitmap);DeleteDC(dc);
  check(background->Present(1,0),"Present background");
}
void makeChain(){
  prepareTexture();
  check(imageVisual->SetContent(nullptr),"Clear content");check(composition->Commit(),"Commit clear");
  mediaStats.Reset();imageChain.Reset();if(surfaceHandle){CloseHandle(surfaceHandle);surfaceHandle=nullptr;}
  check(DCompositionCreateSurfaceHandle(COMPOSITIONOBJECT_ALL_ACCESS,nullptr,&surfaceHandle),"Create composition handle");
  DXGI_SWAP_CHAIN_DESC1 d={};d.Width=UINT(bufferWidth());d.Height=H;d.SampleDesc.Count=1;d.BufferCount=2;
  d.Format=selection==1?DXGI_FORMAT_NV12:DXGI_FORMAT_B8G8R8A8_UNORM;d.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;
  d.Scaling=DXGI_SCALING_STRETCH;d.SwapEffect=DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;d.AlphaMode=DXGI_ALPHA_MODE_IGNORE;
  d.Flags=DXGI_SWAP_CHAIN_FLAG_FULLSCREEN_VIDEO|(selection==1?DXGI_SWAP_CHAIN_FLAG_YUV_VIDEO:0);
  check(mediaFactory->CreateSwapChainForCompositionSurfaceHandle(device.Get(),surfaceHandle,&d,nullptr,&imageChain),"Create image chain");
  ComPtr<IDXGISwapChain3> chain3;check(imageChain.As(&chain3),"Get chain3");
  check(chain3->SetColorSpace1(selection==1?DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709:DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709),"Set color space");
  check(imageChain.As(&mediaStats),"Get presentation statistics");check(imageVisual->SetContent(imageChain.Get()),"Set content");
  D2D_MATRIX_3X2_F matrix={selection==1&&!crop?float(outputWidth)/W:1.0f,0,0,1,0,0};
  check(imageVisual->SetTransform(matrix),"Set transform");
  if(selection==1&&crop){D2D_RECT_F clip={0,0,float(outputWidth),H};check(imageVisual->SetClip(clip),"Set clip");}
  else check(imageVisual->SetClip(static_cast<IDCompositionClip*>(nullptr)),"Clear clip");
  check(imageVisual->SetOffsetX(24),"Set X");check(imageVisual->SetOffsetY(120),"Set Y");
  check(composition->Commit(),"Commit image");lastMode=-99;record("selection-change");drawHud();
}
void present(){
  ComPtr<ID3D11Texture2D> buffer;check(imageChain->GetBuffer(0,IID_PPV_ARGS(&buffer)),"Get image buffer");
  context->CopyResource(buffer.Get(),sourceTexture.Get());check(imageChain->Present(1,0),"Present image");
  DXGI_FRAME_STATISTICS_MEDIA stats={};HRESULT hr=mediaStats->GetFrameStatisticsMedia(&stats);
  int mode=SUCCEEDED(hr)?int(stats.CompositionMode):-1;if(mode!=lastMode){lastMode=mode;record("presentation-mode");}
  if(GetTickCount64()-lastHud>=1000){lastHud=GetTickCount64();drawHud();
    std::wstring title=std::wstring(L"NV12 vs CPU reference | ")+wideLabel()+L" | width "+std::to_wstring(outputWidth)+L" | "+presentationName();SetWindowTextW(windowHandle,title.c_str());}
}
void initialize(){
  check(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,D3D11_CREATE_DEVICE_BGRA_SUPPORT|D3D11_CREATE_DEVICE_VIDEO_SUPPORT,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&context),"Create device");
  ComPtr<IDXGIDevice> dxgi;ComPtr<IDXGIAdapter> adapter;check(device.As(&dxgi),"Get DXGI device");check(dxgi->GetAdapter(&adapter),"Get adapter");
  check(adapter->GetParent(IID_PPV_ARGS(&factory)),"Get factory");check(factory.As(&mediaFactory),"Get media factory");
  check(DCompositionCreateDevice(dxgi.Get(),IID_PPV_ARGS(&composition)),"Create composition device");
  check(composition->CreateTargetForHwnd(windowHandle,TRUE,&target),"Create target");check(composition->CreateVisual(&rootVisual),"Create root");check(composition->CreateVisual(&imageVisual),"Create image visual");
  RECT client={};GetClientRect(windowHandle,&client);DXGI_SWAP_CHAIN_DESC1 d={};d.Width=UINT(client.right);d.Height=UINT(client.bottom);
  d.Format=DXGI_FORMAT_B8G8R8A8_UNORM;d.SampleDesc.Count=1;d.BufferCount=2;d.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;
  d.Scaling=DXGI_SCALING_STRETCH;d.SwapEffect=DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;d.AlphaMode=DXGI_ALPHA_MODE_IGNORE;
  check(factory->CreateSwapChainForComposition(device.Get(),&d,nullptr,&background),"Create background");
  check(rootVisual->SetContent(background.Get()),"Set background");check(rootVisual->AddVisual(imageVisual.Get(),TRUE,nullptr),"Add image");check(target->SetRoot(rootVisual.Get()),"Set root");
  makeChain();ready=true;SetTimer(windowHandle,1,40,nullptr);present();
}
LRESULT CALLBACK windowProc(HWND hwnd,UINT message,WPARAM key,LPARAM data){
  try{
    if(message==WM_TIMER&&ready&&!inTimer){
      inTimer=true;present();
      if(captureCycle&&GetTickCount64()-cycleStart>=ULONGLONG(cycleStep+1)*8000){
        ++cycleStep;
        if(cycleStep==4){DestroyWindow(hwnd);inTimer=false;return 0;}
        selection=cycleStep==1?2:1;crop=cycleStep!=2;outputWidth=crop?639:640;
        makeChain();record("automatic-capture-step");
      }
      inTimer=false;return 0;
    }
    if(message==WM_SIZE&&ready){drawHud();return 0;}
    if(message==WM_KEYDOWN&&ready){
      if(captureCycle){if(key==VK_ESCAPE)DestroyWindow(hwnd);return 0;}
      bool rebuild=false;
      if(key>='1'&&key<='5'){selection=int(key-'0');rebuild=true;}
      else if(key=='0'){outputWidth=W;crop=false;rebuild=true;}
      else if(key=='T'){outputWidth=outputWidth>W-4?outputWidth-1:W;crop=false;rebuild=true;}
      else if(key=='X'){crop=!crop;outputWidth=639;rebuild=true;}
      else if(key=='A')record("USER-AFFECTED");else if(key=='C')record("USER-CLEAN");
      else if(key==VK_ESCAPE){DestroyWindow(hwnd);return 0;}
      if(rebuild)makeChain();return 0;
    }
    if(message==WM_DESTROY){ready=false;KillTimer(hwnd,1);PostQuitMessage(0);return 0;}
  }catch(const std::exception& e){failed=true;ready=false;logFile<<"ERROR "<<e.what()<<"\n";logFile.flush();KillTimer(hwnd,1);if(!selfTest&&!captureCycle)MessageBoxA(hwnd,e.what(),"Reference test error",MB_OK|MB_ICONERROR);DestroyWindow(hwnd);return 0;}
  return DefWindowProcW(hwnd,message,key,data);
}
int WINAPI wWinMain(HINSTANCE instance,HINSTANCE,PWSTR arguments,int show){
  try{
    selfTest=arguments&&std::wcsstr(arguments,L"--self-test");
    captureCycle=arguments&&std::wcsstr(arguments,L"--capture-cycle");
    if(captureCycle)crop=true;
    int startX=220,startY=80,startWidth=1020,startHeight=700;
    if(arguments){const wchar_t* position=std::wcsstr(arguments,L"--position=");if(position&&swscanf_s(position,L"--position=%d,%d",&startX,&startY)!=2)throw std::runtime_error("Expected --position=x,y");}
    if(arguments){const wchar_t* size=std::wcsstr(arguments,L"--size=");if(size&&(swscanf_s(size,L"--size=%d,%d",&startWidth,&startHeight)!=2||startWidth<64||startHeight<64))throw std::runtime_error("Expected --size=width,height (at least64)");}
    bool topmost=captureCycle||(arguments&&std::wcsstr(arguments,L"--topmost"));
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    wchar_t path[32768]={};GetModuleFileNameW(nullptr,path,32768);folder=std::filesystem::path(path).parent_path();assetFolder=folder/L"assets";
    logFile.open(folder/(selfTest?L"self-test.log":L"run.log"),std::ios::app);logFile<<"START CPU reference comparison; no browser or decoder\n";
    WNDCLASSW wc={};wc.lpfnWndProc=windowProc;wc.hInstance=instance;wc.lpszClassName=L"Nv12CpuReferenceDiagnostic";wc.hCursor=LoadCursorW(nullptr,IDC_ARROW);
    if(!RegisterClassW(&wc))throw std::runtime_error("Cannot register window");
    windowHandle=CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP|(topmost?WS_EX_TOPMOST:0),wc.lpszClassName,L"NV12 vs CPU reference",WS_OVERLAPPEDWINDOW,startX,startY,startWidth,startHeight,nullptr,nullptr,instance,nullptr);
    if(!windowHandle)throw std::runtime_error("Cannot create window");if(!selfTest)ShowWindow(windowHandle,show);initialize();cycleStart=GetTickCount64();
    if(selfTest){
      for(int width:{640,639,638,637,636})for(int mode=1;mode<=5;mode++){outputWidth=width;selection=mode;crop=false;makeChain();present();}
      for(int mode=1;mode<=5;mode++){outputWidth=639;selection=mode;crop=true;makeChain();present();}
      logFile<<"SELFTEST PASS: all CPU reference uploads byte-exact; all widths, modes and crop geometry present successfully\n";logFile.flush();DestroyWindow(windowHandle);
    }else{MSG message={};while(GetMessageW(&message,nullptr,0,0)>0){TranslateMessage(&message);DispatchMessageW(&message);}}
    if(surfaceHandle)CloseHandle(surfaceHandle);return failed?1:0;
  }catch(const std::exception& e){if(logFile){logFile<<"FATAL "<<e.what()<<"\n";logFile.flush();}if(!selfTest&&!captureCycle)MessageBoxA(nullptr,e.what(),"Reference test error",MB_OK|MB_ICONERROR);return 1;}
}
