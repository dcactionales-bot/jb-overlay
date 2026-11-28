#include "spoofer.h"
#include <cstdio>
#include <cstdlib>
#include <windows.h>

int main() {
    SetConsoleTitleA("JB HWID Spoofer");
    printf("=== JB HWID SPOOFER ===\n\n");

    // check admin
    BOOL isAdmin = FALSE;
    PSID adminGroup = nullptr;
    SID_IDENTIFIER_AUTHORITY ntAuth = SECURITY_NT_AUTHORITY;
    if (AllocateAndInitializeSid(&ntAuth, 2,
            SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_ADMINS,
            0, 0, 0, 0, 0, 0, &adminGroup)) {
        CheckTokenMembership(nullptr, adminGroup, &isAdmin);
        FreeSid(adminGroup);
    }
    if (!isAdmin) {
        printf("!!! ADMIN OLARAK CALISTIR !!!\n");
        printf("Sag tik -> Yonetici olarak calistir\n\n");
        system("pause");
        return 1;
    }

    // backup
    printf("[1] Yedekleniyor...\n");
    if (spoof::BackupHwids("hwid_backup.txt"))
        printf("    hwid_backup.txt kaydedildi\n");
    else
        printf("    UYARI: yedek yazilamadi (devam ediyor)\n");

    // show current
    printf("\n--- MEVCUT KIMLIKLER ---\n");
    printf("MachineGuid:   %s\n", spoof::ReadReg("SOFTWARE\\Microsoft\\Cryptography", "MachineGuid").c_str());
    printf("HwProfileGuid: %s\n", spoof::ReadReg("SYSTEM\\CurrentControlSet\\Control\\IDConfigDB\\Hardware Profiles\\0001", "HwProfileGuid").c_str());
    printf("ProductId:     %s\n", spoof::ReadReg("SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", "ProductId").c_str());
    printf("Bilgisayar:    %s\n", spoof::ReadComputerName().c_str());
    printf("Volume Serial: %s\n", spoof::ReadVolumeSerial(L'C').c_str());
    printf("Disk Serial:   %s\n", spoof::ReadDiskSerial(0).c_str());

    // spoof
    printf("\n[2] Registry spoofing...\n");
    int ok = 0, fail = 0;
    auto run = [&](const char* name, bool (*fn)(std::string&)) {
        std::string v;
        if (fn(v)) { printf("    OK   %-18s -> %s\n", name, v.c_str()); ++ok; }
        else       { printf("    FAIL %-18s\n", name); ++fail; }
    };

    run("MachineGuid",   spoof::SpoofMachineGuid);
    run("HwProfileGuid", spoof::SpoofHwProfileGuid);
    run("ProductId",     spoof::SpoofProductId);
    run("BuildGUID",     spoof::SpoofBuildGuid);
    run("InstallDate",   spoof::SpoofInstallDate);
    run("SQM MachineId", spoof::SpoofSqmId);
    run("ComputerName",  spoof::SpoofComputerName);

    printf("\n[3] Volume serial...\n");
    std::string volResult;
    if (spoof::SpoofVolumeSerial(L'C', volResult)) {
        printf("    OK   VolumeSerial      -> %s\n", volResult.c_str()); ++ok;
    } else {
        printf("    FAIL VolumeSerial (normal — bazi disklerde calismaz)\n"); ++fail;
    }

    // MAC
    printf("\n[4] MAC adresleri...\n");
    int macs = spoof::SpoofMacAddresses();
    printf("    %d adapter degistirildi\n", macs);
    if (macs > 0) {
        printf("    Adapter reset yapiliyor (internet 2 sn kesilir)...\n");
        spoof::ResetAdapters();
        printf("    Tamam\n");
    }

    // cleanup
    printf("\n[5] Temizlik...\n");
    int eac = spoof::CleanEac();
    int traces = spoof::CleanTraces();
    printf("    EAC: %d temizlendi\n", eac);
    printf("    Traces: %d temizlendi\n", traces);

    // summary
    printf("\n========================================\n");
    printf("  SONUC\n");
    printf("========================================\n");
    printf("  Registry : %d/%d basarili\n", ok, ok + fail);
    printf("  MAC      : %d adapter\n", macs);
    printf("  EAC + iz : %d + %d temizlendi\n", eac, traces);
    printf("========================================\n");
    printf("\n  *** BILGISAYARI YENIDEN BASLAT ***\n");
    printf("  Reboot sonrasi oyunu ac, sonra overlay'i ac.\n\n");

    system("pause");
    return 0;
}
