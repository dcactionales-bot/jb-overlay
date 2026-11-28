#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <string>

namespace spoof {

// read a REG_SZ from HKLM
std::string ReadReg(const char* path, const char* name);

// each returns true on success, fills newVal with the written value
bool SpoofMachineGuid(std::string& newVal);
bool SpoofHwProfileGuid(std::string& newVal);
bool SpoofProductId(std::string& newVal);
bool SpoofBuildGuid(std::string& newVal);
bool SpoofInstallDate(std::string& newVal);
bool SpoofSqmId(std::string& newVal);

// returns number of adapters changed
int  SpoofMacAddresses();
// returns number of items cleaned
int  CleanEacCache();

}  // namespace spoof
