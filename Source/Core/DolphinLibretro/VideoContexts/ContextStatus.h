#pragma once

enum class ContextState
{
  Unknown,
  Reset,
  Destroyed
};

struct ContextStatus
{
  bool initialized = false;
  ContextState state = ContextState::Unknown;

  void MarkReset() { state = ContextState::Reset; }
  void MarkDestroyed() { state = ContextState::Destroyed; }
  void MarkUnknown() { state = ContextState::Unknown; }

  bool IsReady() const { return state == ContextState::Reset; }
  bool IsDestroyed() const { return state == ContextState::Destroyed; }

  void MarkInitialized() { initialized = true; }
  void MarkUnitialized() { initialized = false; }

  bool IsInitialized() const { return initialized; }
};

extern ContextStatus g_context_status;
