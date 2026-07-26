// Copyright 2017 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoCommon/AsyncShaderCompiler.h"

#include <memory>
#include <thread>

#include "Common/Assert.h"
#include "Common/Logging/Log.h"
#include "Common/Thread.h"

#include "Core/Config/GraphicsSettings.h"
#include "Core/Core.h"
#include "Core/System.h"

#include "VideoCommon/VideoConfig.h"

namespace VideoCommon
{
static bool ShouldStartWorkerThreads()
{
  if (!g_backend_info.bSupportsBackgroundCompiling)
    return false;

#if defined(__SWITCH__)
  switch (Config::Get(Config::GFX_SHADER_COMPILATION_MODE))
  {
  case ShaderCompilationMode::Synchronous:
  case ShaderCompilationMode::SynchronousUberShaders:
    return false;
  case ShaderCompilationMode::AsynchronousUberShaders:
  case ShaderCompilationMode::AsynchronousSkipRendering:
    break;
  }
#endif

  return true;
}

AsyncShaderCompiler::AsyncShaderCompiler()
{
}

AsyncShaderCompiler::~AsyncShaderCompiler()
{
  // Pending work can be left at shutdown.
  // The work item classes are expected to clean up after themselves.
  ASSERT(!HasWorkerThreads());
}

void AsyncShaderCompiler::QueueWorkItem(WorkItemPtr item, u32 priority)
{
  // If no worker threads are available, compile synchronously.
  if (!HasWorkerThreads())
  {
    item->Compile();
    m_completed_work.push_back(std::move(item));
  }
  else
  {
    std::lock_guard guard(m_pending_work_lock);
    m_pending_work.emplace(priority, std::move(item));
    m_worker_thread_wake.notify_one();
  }
}

void AsyncShaderCompiler::RetrieveWorkItems()
{
  std::deque<WorkItemPtr> completed_work;
  {
    std::lock_guard guard(m_completed_work_lock);
    m_completed_work.swap(completed_work);
  }

  while (!completed_work.empty())
  {
    completed_work.front()->Retrieve();
    completed_work.pop_front();
  }
}

bool AsyncShaderCompiler::HasPendingWork()
{
  std::lock_guard guard(m_pending_work_lock);
  return !m_pending_work.empty() || m_busy_workers.load() != 0;
}

bool AsyncShaderCompiler::HasCompletedWork()
{
  std::lock_guard guard(m_completed_work_lock);
  return !m_completed_work.empty();
}

void AsyncShaderCompiler::ClearAllWork()
{
  {
    std::lock_guard guard(m_pending_work_lock);
    m_pending_work.clear();
  }

  {
    std::lock_guard guard(m_completed_work_lock);
    m_completed_work.clear();
  }
}

bool AsyncShaderCompiler::WaitUntilCompletion(
    const std::function<void(size_t, size_t)>& progress_callback)
{
  if (!HasPendingWork())
    return true;

  // Wait a second before opening a progress dialog.
  // This way, if the operation completes quickly, we don't annoy the user.
  constexpr u32 CHECK_INTERVAL_MS = 1000 / 30;
  constexpr auto CHECK_INTERVAL = std::chrono::milliseconds(CHECK_INTERVAL_MS);
  for (u32 i = 0; i < (1000 / CHECK_INTERVAL_MS); i++)
  {
    std::this_thread::sleep_for(std::chrono::milliseconds(CHECK_INTERVAL));
    if (!HasPendingWork())
      return true;
  }

  // Grab the number of pending items. We use this to work out how many are left.
  size_t total_items;
  {
    // Safe to hold both locks here, since nowhere else does.
    std::lock_guard pending_guard(m_pending_work_lock);
    std::lock_guard completed_guard(m_completed_work_lock);
    total_items = m_completed_work.size() + m_pending_work.size() + m_busy_workers.load() + 1;
  }

  // Update progress while the compiles complete.
  while (Core::GetState(Core::System::GetInstance()) != Core::State::Stopping)
  {
    size_t remaining_items;
    {
      std::lock_guard pending_guard(m_pending_work_lock);
      if (m_pending_work.empty() && !m_busy_workers.load())
        return true;
      remaining_items = m_pending_work.size();
    }

    progress_callback(total_items - remaining_items, total_items);
    std::this_thread::sleep_for(CHECK_INTERVAL);
  }
  return false;
}

bool AsyncShaderCompiler::StartWorkerThreads(u32 num_worker_threads)
{
  if (num_worker_threads == 0)
    return true;

  if (!ShouldStartWorkerThreads())
    return true;

  for (u32 i = 0; i < num_worker_threads; i++)
  {
    void* thread_param = nullptr;
    if (!WorkerThreadInitMainThread(&thread_param))
    {
      WARN_LOG_FMT(VIDEO, "Failed to initialize shader compiler worker thread.");
      break;
    }

    m_worker_thread_start_result.store(false);

#ifdef __SWITCH__
    constexpr size_t SHADER_COMPILER_STACK_SIZE = 2 * 1024 * 1024;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, SHADER_COMPILER_STACK_SIZE);

    pthread_t thread;
    auto start = std::make_unique<WorkerThreadStart>(WorkerThreadStart{this, thread_param});
    const int create_result =
        pthread_create(&thread, &attr, &AsyncShaderCompiler::WorkerThreadTrampoline, start.get());
    pthread_attr_destroy(&attr);
    if (create_result != 0)
    {
      WARN_LOG_FMT(VIDEO, "Failed to create shader compiler worker thread: {}", create_result);
      break;
    }
    start.release();
#else
    std::thread thr(&AsyncShaderCompiler::WorkerThreadEntryPoint, this, thread_param);
#endif
    m_init_event.Wait();

    if (!m_worker_thread_start_result.load())
    {
      WARN_LOG_FMT(VIDEO, "Failed to start shader compiler worker thread.");
#ifdef __SWITCH__
      pthread_join(thread, nullptr);
#else
      thr.join();
#endif
      break;
    }

#ifdef __SWITCH__
    m_worker_threads.push_back(thread);
#else
    m_worker_threads.push_back(std::move(thr));
#endif
  }

  return HasWorkerThreads();
}

bool AsyncShaderCompiler::ResizeWorkerThreads(u32 num_worker_threads)
{
  if (m_worker_threads.size() == num_worker_threads)
    return true;

  StopWorkerThreads();
  return StartWorkerThreads(num_worker_threads);
}

bool AsyncShaderCompiler::HasWorkerThreads() const
{
  return !m_worker_threads.empty();
}

void AsyncShaderCompiler::StopWorkerThreads()
{
  if (!HasWorkerThreads())
    return;

  // Signal worker threads to stop, and wake all of them.
  {
    std::lock_guard guard(m_pending_work_lock);
    m_exit_flag.Set();
    m_worker_thread_wake.notify_all();
  }

  // Wait for worker threads to exit.
#ifdef __SWITCH__
  for (pthread_t thread : m_worker_threads)
    pthread_join(thread, nullptr);
#else
  for (std::thread& thr : m_worker_threads)
    thr.join();
#endif
  m_worker_threads.clear();
  m_exit_flag.Clear();
}

bool AsyncShaderCompiler::WorkerThreadInitMainThread(void** param)
{
  return true;
}

bool AsyncShaderCompiler::WorkerThreadInitWorkerThread(void* param)
{
  return true;
}

void AsyncShaderCompiler::WorkerThreadExit(void* param)
{
}

#ifdef __SWITCH__
void* AsyncShaderCompiler::WorkerThreadTrampoline(void* start_ptr)
{
  const std::unique_ptr<WorkerThreadStart> start(static_cast<WorkerThreadStart*>(start_ptr));
  start->compiler->WorkerThreadEntryPoint(start->param);
  return nullptr;
}
#endif

void AsyncShaderCompiler::WorkerThreadEntryPoint(void* param)
{
  Common::SetCurrentThreadName("AsyncShaderCompiler Worker");
#ifdef __SWITCH__
  Common::SetCurrentThreadAffinity(2);
  // Keep shader compilation below the audio callback priority.
  Common::AdjustCurrentThreadPriority(2);
#endif

  // Initialize worker thread with backend-specific method.
  if (!WorkerThreadInitWorkerThread(param))
  {
    WARN_LOG_FMT(VIDEO, "Failed to initialize shader compiler worker.");
    m_worker_thread_start_result.store(false);
    m_init_event.Set();
    return;
  }

  m_worker_thread_start_result.store(true);
  m_init_event.Set();

  WorkerThreadRun();

  WorkerThreadExit(param);
}

void AsyncShaderCompiler::WorkerThreadRun()
{
  std::unique_lock pending_lock(m_pending_work_lock);
  while (!m_exit_flag.IsSet())
  {
    m_worker_thread_wake.wait(pending_lock);

    while (!m_pending_work.empty() && !m_exit_flag.IsSet())
    {
      m_busy_workers++;
      auto iter = m_pending_work.begin();
      WorkItemPtr item(std::move(iter->second));
      m_pending_work.erase(iter);
      pending_lock.unlock();

      if (item->Compile())
      {
        std::lock_guard completed_guard(m_completed_work_lock);
        m_completed_work.push_back(std::move(item));
      }

      pending_lock.lock();
      m_busy_workers--;
    }
  }
}

}  // namespace VideoCommon
