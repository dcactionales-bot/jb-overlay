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

// ---- read current values ----
std::string ReadReg(const char* path, const char* name);
std::string ReadDiskSerial(int drive = 0);
std::string ReadVolumeSerial(wchar_t letter = L'C');
std::string ReadComputerName();

// ---- spoof registry identifiers (each fills newVal) ----
bool SpoofMachineGuid(std::string& nv);
bool SpoofHwProfileGuid(std::string& nv);
bool SpoofProductId(std::string& nv);
bool SpoofBuildGuid(std::string& nv);
bool SpoofInstallDate(std::string& nv);
bool SpoofSqmId(std::string& nv);
bool SpoofComputerName(std::string& nv);

// ---- volume serial (NTFS VBR write, needs admin) ----
bool SpoofVolumeSerial(wchar_t letter, std::string& nv);

// ---- MAC addresses ----
int  SpoofMacAddresses();     // registry change, returns adapters changed
void ResetAdapters();          // SetupDi disable/enable cycle (immediate apply)

// ---- cleanup ----
int  CleanEac();               // EAC cache + registry
int  CleanTraces();            // prefetch, temp, game traces

// ---- backup original values to file ----
bool BackupHwids(const char* path);

}  // namespace spoof
