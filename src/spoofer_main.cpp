#include "spoofer.h"
#include <cstdio>

int main() {
    printf("=== JB HWID SPOOFER ===\n\n");

    // backup
    printf("[1] Yedekleniyor...\n");
    if (spoof::BackupHwids("hwid_backup.txt"))
        printf("    hwid_backup.txt kaydedildi\n");
    else
        printf("    HATA: yedek yazilamadi\n");

    // show current
    printf("\n--- MEVCUT KIMLIKLER ---\n");
    printf("MachineGuid:   %s\n", spoof::ReadReg("SOFTWARE\\Microsoft\\Cryptography", "MachineGuid").c_str());
    printf("HwProfileGuid: %s\n", spoof::ReadReg("SYSTEM\\CurrentControlSet\\Control\\IDConfigDB\\Hardware Profiles\\0001", "HwProfileGuid").c_str());
    printf("ProductId:     %s\n", spoof::ReadReg("SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", "ProductId").c_str());
    printf("Bilgisayar:    %s\n", spoof::ReadComputerName().c_str());
    printf("Volume Serial: %s\n", spoof::ReadVolumeSerial(L'C').c_str());
    printf("Disk Serial:   %s\n", spoof::ReadDiskSerial(0).c_str());

    // spoof
    printf("\n[2] Spoofing...\n");
    int ok = 0, fail = 0;
    auto run = [&](const char* name, bool (*fn)(std::string&)) {
        std::string v;
        if (fn(v)) { printf("    OK  %-18s -> %s\n", name, v.c_str()); ++ok; }
        else       { printf("    FAIL %-18s\n", name); ++fail; }
    };

    run("MachineGuid",   spoof::SpoofMachineGuid);
    run("HwProfileGuid", spoof::SpoofHwProfileGuid);
    run("ProductId",     spoof::SpoofProductId);
    run("BuildGUID",     spoof::SpoofBuildGuid);
    run("InstallDate",   spoof::SpoofInstallDate);
    run("SQM MachineId", spoof::SpoofSqmId);
    run("ComputerName",  spoof::SpoofComputerName);

    std::string volResult;
    if (spoof::SpoofVolumeSerial(L'C', volResult)) {
        printf("    OK  %-18s -> %s\n", "VolumeSerial", volResult.c_str()); ++ok;
    } else {
        printf("    FAIL %-18s\n", "VolumeSerial"); ++fail;
    }

    // MAC
    printf("\n[3] MAC adresleri...\n");
    int macs = spoof::SpoofMacAddresses();
    printf("    %d adapter degistirildi\n", macs);
    printf("    Adapter reset yapiliyor...\n");
    spoof::ResetAdapters();
    printf("    Tamam\n");

    // cleanup
    printf("\n[4] Temizlik...\n");
    int eac = spoof::CleanEac();
    int traces = spoof::CleanTraces();
    printf("    EAC: %d temizlendi\n", eac);
    printf("    Traces: %d temizlendi\n", traces);

    // summary
    printf("\n=== SONUC ===\n");
    printf("Registry: %d/%d basarili\n", ok, ok + fail);
    printf("MAC: %d adapter\n", macs);
    printf("EAC + iz: %d + %d temizlendi\n", eac, traces);
    printf("\n*** BILGISAYARI YENIDEN BASLAT ***\n");
    printf("Reboot sonrasi oyunu ac, sonra overlay'i ac.\n\n");

    printf("Kapatmak icin bir tusa bas...\n");
    getchar();
    return 0;
}
