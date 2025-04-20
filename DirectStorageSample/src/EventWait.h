#pragma once
#include <wrl/client.h>
#include <wrl/event.h>

class EventWait
{
  Microsoft::WRL::Wrappers::Event m_event;
  TP_WAIT* m_wait = nullptr;
public:
  EventWait() = default;
  ~EventWait()
  {
    Close();
  }

  template<typename T, void (T::*FN)()>
  void Init(T* target)
  {
    auto callback = [](TP_CALLBACK_INSTANCE*, void* context, TP_WAIT*, TP_WAIT_RESULT)
      {
        T* target = reinterpret_cast<T*>(context);
        (target->*FN)();
      };
    
    m_wait = CreateThreadpoolWait(callback, target, nullptr);
    constexpr BOOL manualReset = TRUE;
    constexpr BOOL initialState = FALSE;
    m_event.Attach(CreateEventW(nullptr, manualReset, initialState, nullptr));
  }

  void SetThraedpoolWait()
  {
    ResetEvent(m_event.Get());
    ::SetThreadpoolWait(m_wait, m_event.Get(), nullptr);
  }
  bool IsSet() const
  {
    return WaitForSingleObject(m_event.Get(), 0) == WAIT_OBJECT_0;
  }
  void Close()
  {
    if (m_wait)
    {
      WaitForThreadpoolWaitCallbacks(m_wait, TRUE);
      CloseThreadpoolWait(m_wait);
      m_wait = nullptr;
    }
  }
  operator HANDLE()
  {
    return m_event.Get();
  }
};