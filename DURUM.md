# DURUM

Build: `cmake --build build --config Release` (proje kökünden). Son build temiz, /W4 uyarısız. Oyunda canlı test YOK.

## Biten
- `src/esp.cpp` — `Esp::Update()`, `ResolveBattleModel()` (heap taraması), `ReadViewMatrix()` (heap taraması + yerel oyuncu ekran-merkezi doğrulaması), `ScanHeap()` yardımcısı
- `src/esp.h` — Config'e `fovCircle` / `fovRadius`, cache üyeleri
- `src/main.cpp` — Process attach (`ROB RUN.exe`, 1 sn aralıkla), kendi döngüde Update + Render, menü: ESP, FOV circle, FOV slider
- `.gitignore` + ilk commit (`03f6952`)

## Açık FILL / doğrulanmamış
- `kPrimValue = 0x18` — **tahmin**. Dump generic'lerde 0x0 gösteriyor; canlı nesnede doğrula
- il2cpp Dictionary düzeni (`0x18/0x20/0x20/0x18/0x10`) — standart x64 değerleri, bu build'de doğrulanmadı
- `kBattleModelInstance = 0` — kullanılmıyor, heap taraması onun yerini aldı. Silinebilir
- `kBattleModelTypeInfo` / `kPlayerEntityTypeInfo` — script.json RVA'ları, oyun güncellenince değişir
- `kMetersPerUnit = 0.01905f` — eski CS2 sabiti, Unity'de büyük ihtimalle yanlış (mesafe)
- `kHeadHeight = 1.75f` — sabit; kemik okuması yok
- Rol: yerel dışındaki herkes Enemy. Takım alanı eşlenmedi
- İsim boş (`PlayerClientModel` içinde alan aranmadı)
- Matris doğrulaması: yerel oyuncu ekran merkezine yakın varsayımı (`kCenterTol = 0.35`). Birinci şahıs kamerada tüm adaylar elenebilir

## Sıradaki adımlar
1. Oyunu aç, overlay'i çalıştır; "attached" görünüyor mu bak
2. `ResolveBattleModel()` bulunuyor mu, `Update()` true dönüyor mu logla
3. `kPrimValue` ve Dictionary düzenini canlı nesnede doğrula
4. Matris adayının doğru kameraya kilitlendiğini kutuların hizasından kontrol et; gerekirse `kCenterTol` / `PlausibleMatrix`
5. `kMetersPerUnit` düzelt
6. Takım alanını bul, `Role::Teammate` ayır
7. İsim alanını bul (`PlayerClientModel`)
8. Oyun yamasında: TypeInfo RVA'larını script.json'dan yeniden çıkar
