#include "Win32Application.h"

#include <combaseapi.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <thread>
#include <random>
#include <vector>
void WorkerThread()
{
  std::default_random_engine rng;
  while (true)
  {
    std::vector<float> dataList;
    std::uniform_real_distribution<float> d(-1.0f, 1.0f);
    for (int i = 0; i < 10 * 10000; ++i)
    {
      dataList.push_back(d(rng));
    }
    dataList.clear();
  }
}

int __stdcall wWinMain(_In_ HINSTANCE hInstance,
  _In_opt_ HINSTANCE hPrevInstance,
  _In_ LPWSTR lpCmdLine,
  _In_ int nCmdShow)
{
  UNREFERENCED_PARAMETER(hPrevInstance);
  UNREFERENCED_PARAMETER(lpCmdLine);
  
  ::CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);

  Win32Application::WindowInitParams initParams{
    .width = 1280,
    .height = 720,
    .cmdShow = nCmdShow,
    .hInstance = hInstance,
  };

  //std::vector<std::thread> threads(10);
  //for (auto& t : threads)
  //{
  //  t = std::thread([] { WorkerThread(); });
  //}

  return Win32Application::Run(initParams);
}
