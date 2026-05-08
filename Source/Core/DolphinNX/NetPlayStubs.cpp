// Copyright 2026 Dolphin Emulator Project
// Copyright 2026 Dan | ticoverse.com
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/HW/EXI/EXI_DeviceIPL.h"
#include "Core/HW/SI/SI_DeviceAMBaseboard.h"
#include "Core/HW/SI/SI_DeviceGCController.h"
#include "InputCommon/GCPadStatus.h"

bool SerialInterface::CSIDevice_GCController::NetPlay_GetInput(int pad_num, GCPadStatus* status)
{
  return false;
}

bool SerialInterface::CSIDevice_AMBaseboard::NetPlay_GetInput(int pad_num, GCPadStatus* status)
{
  return false;
}


int SerialInterface::CSIDevice_GCController::NetPlay_InGamePadToLocalPad(int numPAD)
{
  return numPAD;
}

int SerialInterface::CSIDevice_AMBaseboard::NetPlay_InGamePadToLocalPad(int numPAD)
{
  return numPAD;
}

u64 ExpansionInterface::CEXIIPL::NetPlay_GetEmulatedTime()
{
  return 0;
}
