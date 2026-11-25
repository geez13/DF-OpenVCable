# Changelog: DF-OpenVCable (DimzFrantic Open Virtual Audio Cable)

## [1.0.0-macos] - 2026-11-25

### Added
- **Port native macOS (`macos/`)**:
  - **All-in-One CoreAudio HAL `AudioServerPlugIn`** (`macos/driver/DFOpenVCableDriver.cpp`, `macos/driver/ring_buffer.h`, `macos/driver/dfovc_protocol.h`, `macos/driver/Info.plist`): satu bundle `DF-OpenVCable.driver` universal (`arm64` + `x86_64`) yang langsung menyediakan 5 pasang Virtual Audio Cable independen (`DF-OpenVCable A` s/d `DF-OpenVCable E`) dengan kontrol Volume & Mute native, dukungan dinamis 44.1/48/88.2/96/192 kHz, buffer 50–2000 ms, clock zero-drift berbasis `mach_absolute_time()`, dan penyimpanan persisten via `coreaudiod` host storage.
  - **Native macOS Control Panel** (`macos/control_panel/DFOpenVCableControlPanel.swift`): aplikasi SwiftUI (`DF-OpenVCable Control Panel.app`) dengan 5 tab kabel, kontrol konfigurasi live, meter peak Playback/Capture, bar utilisasi ring buffer, dan penghitung overrun/underrun.
  - **CLI & Live Audio Loopback Tester** (`macos/cli/dfovc_ctl.swift`): utilitas `dfovc-ctl` (`list`, `status [--json]`, `config`, `reset-stats`, `loopback-test`) yang menguji aliran audio 1 kHz live melalui CoreAudio HAL dengan analisis RMS & Goertzel.
  - **Build, Packaging, & Deployment** (`macos/Makefile`, `macos/scripts/install.sh`, `uninstall.sh`, `reload.sh`, `selftest.sh`): kompilasi mandiri cukup dengan Xcode Command Line Tools (tanpa Xcode.app), otomatis ad-hoc code-signed untuk Apple Silicon.
  - **Automated Tests & CI** (`macos/tests/test_ring_buffer.cpp`, `macos/tests/test_driver_plugin.cpp`, `.github/workflows/build-macos.yml`, `docs/VALIDATION_MACOS.md`): unit test ring buffer + integration test `dlopen` `AudioServerPlugIn` + workflow GitHub Actions macOS.

## [1.0.0.14] - 2026-11-25

### Fixed
- **Bug inti: `m_StartTime` diisi frekuensi, bukan waktu sekarang** (`src/minwavert.cpp`).

  `KeQueryPerformanceCounter(PLARGE_INTEGER)` menulis **frekuensi** ke pointer yang diberikan; nilai **waktu sekarang** adalah *return value*-nya. Kode memanggil:

  ```cpp
  KeQueryPerformanceCounter(&m_StartTime);   // m_StartTime = 10.000.000 (frekuensi)
  ```

  sehingga `elapsed = now - m_StartTime = now - 10.000.000` ≈ sebesar **uptime sistem**. `totalBytesStreamed` meledak, `delta` selalu lebih besar dari DMA buffer, dan setiap panggilan `GetPosition` menulis satu DMA buffer penuh ke ring — terus-menerus, ~48×/detik.

  Bukti terukur di mesin (kabel A, 3 detik):

  | Metrik | Nilai | Arti |
  |---|---|---|
  | format | R `6/288000`, C `6/288000` | normalisasi container sudah benar |
  | `fullB` | **0** | ring tidak pernah penuh |
  | `fill` | stabil 36.102 (Δ+6) | tidak ada drift |
  | `trim#` | **+143** | 48 pemotongan/detik |
  | `trimB` | **+6.615.894** | 2,2 MB/detik dibuang, padahal ring hanya menerima 288 kB/detik |

  Mustahil kecuali sisi render menulis ~8× real time. Setelah diperbaiki, laju tulis = laju baca = 288.000 B/s sehingga trim tidak perlu jalan sama sekali.

  Perbaikan: `m_StartTime = KeQueryPerformanceCounter(NULL);` — plus jaring pengaman di `GetPosition` (kalau `m_StartTime` masih 0 atau di masa depan, mulai hitung dari sekarang) supaya `elapsed` tidak pernah sebesar uptime.

- `src/ringbuffer.cpp`: `m_LastWriteTime` mengalami bug yang sama; sekarang memakai return value.

### Changed
- **DriverVer**: `1.0.0.13` → `1.0.0.14`.

## [1.0.0.13] - 2026-11-25

### Fixed
- **Akar sebenarnya suara "alien / aliran listrik": container sample beda antara sisi Render dan Capture.**

  Diagnostik baru dari 1.0.0.12 membuktikan langsung di mesin (bukan dugaan):

  | Kabel | Render | Capture |
  |---|---|---|
  | A–D | `blockAlign=8`, **384.000 B/s** (24-in-32) | `blockAlign=6`, **288.000 B/s** (24-bit packed) |
  | E | `blockAlign=6`, 288.000 B/s | `blockAlign=6`, 288.000 B/s |

  Selisih 96.000 B/s pada A–D = tepat angka yang menjelaskan ~50 pemotongan/detik (dengung 50 Hz). Pin hanya mengunci **jumlah bit** (24), bukan **lebar container**, sehingga Windows bebas memilih 3 atau 4 byte per sample per endpoint — dan pilihannya berbeda antara Render dan Capture.

  Perbaikan: ring buffer kini punya **format kanonik tetap, PCM 24-bit packed (3 byte/sample)**, dan driver menormalkan container saat transfer:
  - Render `24-in-32` → packed 24 (buang byte padding terendah, data 24-bit ada di byte 1..3)
  - Capture packed 24 → `24-in-32` (sisipkan padding nol di byte terendah)

  Jadi berapa pun container yang dinegosiasikan engine, laju byte kedua sisi selalu sama dan tidak ada lagi backlog yang dipotong. Laju acuan ring (`m_BytesPerSec`), alignment, preroll, dan latency limit sekarang dihitung dari format kanonik — bukan dari `nAvgBytesPerSec` stream.

### Changed
- Buffer kerja non-paged per stream (`m_pConvertA`/`m_pConvertB`) untuk normalisasi; dialokasikan di `AllocateAudioBuffer` dan dilepas di `FreeAudioBuffer`/destruktor.
- Path pass-through (container sudah sama) tetap tanpa salinan tambahan.
- **DriverVer**: `1.0.0.12` → `1.0.0.13`.

## [1.0.0.12] - 2026-11-25

### Fixed
- **Suara "alien / seperti aliran listrik"** (`src/minwavert.cpp`): `DataRangeIntersection` mengabaikan `ClientDataRange` dan **selalu** membalas 24-bit/48 kHz. Aplikasi yang meminta format lain tetap mendapat `STATUS_SUCCESS` dengan format yang tidak dimintanya, lalu menafsirkan byte dengan format berbeda. Karena loopback memindahkan PCM mentah per-byte, laju byte sisi Capture jadi tidak sama dengan Render → backlog menumpuk, dipotong terus-menerus, dan suara terdengar robotik/dengung.

  Gejala terukur sebelum perbaikan (30 detik, kabel A, sedang memutar):
  - `fill` berayun 11.262 → 58.014 byte
  - **overrun +1.494 (≈ 50/detik)** — 50 kali pemotongan per detik inilah yang terdengar seperti dengung 50 Hz ("aliran listrik")
  - `underrun +0` (pre-roll cushion dari 1.0.0.10 sudah bekerja)

  Perbaikan: `DataRangeIntersection` sekarang memvalidasi `ClientDataRange` (MajorFormat/SubFormat PCM + batas channels/bits/sample rate) dan mengembalikan `STATUS_NO_MATCH` kalau permintaan klien tidak cocok dengan format terkunci 48 kHz/24-bit/stereo. Aplikasi yang meminta format lain akan ditolak dan jatuh ke shared mode (di-convert oleh audio engine), bukan diam-diam menghasilkan stream yang rusak.

### Added
- **Diagnostik format stream & rincian pembuangan data** (`src/mcvad_ioctl.h`, `src/adapter.cpp`, `src/minwavert.h`, `src/ringbuffer.h`). `MCVAD_CABLE_INFO` diperluas dengan:
  - `RenderBlockAlign`, `RenderBytesPerSec`, `CaptureBlockAlign`, `CaptureBytesPerSec` — format yang **benar-benar** dinegosiasikan tiap sisi. Kalau pasangan ini berbeda, loopback pasti drift; ini indikator paling langsung.
  - `TrimCount`, `TrimBytes` (dibuang karena batas latensi) dan `FullDropBytes` (dibuang karena ring penuh) — memisahkan dua penyebab overrun yang sebelumnya tercampur di satu counter.
- `IsRenderActive` / `IsCaptureActive` kini benar-benar diisi dari jumlah stream yang berjalan (sebelumnya selalu `0`, sehingga Control Panel selalu menampilkan "Idle" walau sedang streaming).

### Changed
- **DriverVer**: `1.0.0.11` → `1.0.0.12`.

## [1.0.0.11] - 2026-11-25

### Fixed
- **Control Panel: nilai kembali ke default saat panel dibuka ulang** (`control_panel/control_panel.cpp`). Kedua combobox dibuat tanpa control ID (`NULL`), sehingga tidak ada notifikasi `CBN_SELCHANGE` yang bisa ditangkap, dan `UpdateCableUI()` memang tidak pernah menyetel `CB_SETCURSEL`. Akibatnya panel selalu menampilkan default hardcoded di `WM_CREATE` (48000 Hz / 500 ms) berapa pun nilai yang tersimpan di driver — ubah ke 200 ms, Apply, tutup, buka lagi → tampil 500 ms.
  - Combobox sekarang punya ID (`ID_COMBO_RATE` / `ID_COMBO_BUFFER`) dan menangani `CBN_SELCHANGE`.
  - `UpdateCableUI()` menyinkronkan kedua combobox dari status driver (dipetakan nilai → index lewat `IndexOfValue`), jadi panel selalu menampilkan nilai sebenarnya saat dibuka atau saat pindah tab.
  - Flag `g_bConfigDirty` mencegah timer 500 ms menimpa pilihan user yang belum di-Apply (pola yang sama dengan `g_bEnableDirty`), dan direset saat Apply berhasil atau tab dipindah.

- **Setting hilang setiap driver reload / reboot** (`src/adapter.cpp`, `src/driver.h`). `g_ConfiguredBufferMs` hanya variabel statis di memori, dan ukuran ring di `StartDevice` memakai konstanta 2 detik — jadi nilai yang tampil di GUI tidak pernah bertahan.
  - Setting (buffer duration + enable state per kabel) sekarang dipersist ke `HKLM\SOFTWARE\DF-OpenVCable\Settings` sebagai satu nilai `REG_BINARY` (magic `'MCV1'`, 5×BufferMs + 5×Enabled) lewat `RtlWriteRegistryValue`.
  - `StartDevice` memanggil `McvadLoadSettings()` sebelum membuat ring buffer, lalu mengalokasikan ring sesuai durasi tersimpan — sehingga kapasitas sebenarnya konsisten dengan yang ditampilkan panel.
  - Nilai dari registry disanitasi (10–5000 ms); kalau magic tidak cocok atau rusak, kembali ke default 500 ms.

### Changed
- **DriverVer**: `1.0.0.10` → `1.0.0.11`.
- Ring buffer default kini 500 ms (bukan 2 detik tetap) mengikuti `DEFAULT_BUFFER_MS`.

## [1.0.0.10] - 2026-11-25

### Fixed
- **Audio patah-patah walau rate sudah sinkron** (`src/ringbuffer.cpp`, `src/minwavert.cpp`). Setelah 1.0.0.9 kedua endpoint sama-sama 48 kHz/24-bit (288.000 B/s) sehingga drift hilang, tetapi hasil uji masih tidak mulus dengan **underrun 10.427 vs overrun 952**. Penyebabnya bukan drift, tapi **tidak ada cushion sama sekali**: render menulis dan capture membaca dari QPC yang sama pada rate yang sama, jadi `DataAvailable` bertahan di sekitar nol. Setiap jitter scheduling (polling WASAPI ~10 ms) langsung membuat capture kehabisan data → `Read()` mengisi senyap → celah berulang.

  Perbaikan:
  - **Pre-roll cushion 50 ms** (`SetPreroll`): capture tidak mulai mengonsumsi sebelum ring terisi 50 ms. Setelah itu jarak (offset) terjaga konstan karena kedua sisi memakai clock yang sama, jadi latensi stabil di ~50 ms dan tidak ada underrun. Fase pengisian awal tidak dihitung sebagai underrun.
  - Cushion dibatasi separuh kapasitas ring, supaya buffer kecil tidak membuat capture menunggu selamanya.
  - Batas latensi dinaikkan ke 150 ms dan koreksinya membuang **sampai cushion** (bukan sampai batas atas), sehingga latensi kembali normal setelah gangguan, tidak menetap di batas.
  - **Render berhenti menulis saat tidak ada stream capture yang berjalan** (`IsCaptureActive`): mencegah ring terisi audio basi dan membuat counter overrun naik tanpa arti.
  - `Reset()` backlog saat capture `RUN` kini hanya dilakukan kalau backlog > 500 ms (data basi), bukan setiap transisi state — supaya siklus STOP/RUN normal tidak memotong audio.

### Changed
- **DriverVer**: `1.0.0.9` → `1.0.0.10`.

## [1.0.0.9] - 2026-11-25

### Fixed
- **Audio loopback "lag" lalu berubah jadi distorsi warble/robotik** (`src/minwavert.cpp`, `src/driver.h`, `src/ringbuffer.cpp`). Bukan masalah buffer size — menyetel buffer ke 50 ms tidak berpengaruh karena penyebabnya *sample-rate mismatch* antara dua endpoint satu kabel:

  | Endpoint | Format hasil negosiasi | Byte rate |
  |---|---|---|
  | Render `DF-OpenVCable A` | 48000 Hz, 24-bit, 2ch | 288.000 B/s |
  | Capture `DF-OpenVCable A` | **44100** Hz, 24-bit, 2ch | **264.600** B/s |

  Ring buffer loopback memindahkan PCM mentah per-byte tanpa resampling, jadi capture mengonsumsi 23.400 B/s lebih lambat daripada render menulis. Akibatnya:
  - backlog tumbuh linear → **latensi naik terus** ("seperti lag"),
  - audio diputar 44100/48000 = **8,13% lebih lambat** (pitch turun ~1,4 semitone),
  - setelah ring (768 KB ≈ 2,7 detik) penuh, `Write()` membuang sample tertua pada **setiap** panggilan → diskontinuitas terus-menerus → efek "alien".

  Perbaikan:
  - `g_PinDataRangesAudio` dikunci ke satu format: 2 ch, 24-bit, **48000 Hz**. Tidak ada lagi rate lain yang bisa dinegosiasikan, jadi kedua sisi selalu identik.
  - `DEFAULT_CONTAINER_SIZE` 32 → 24 (3 byte/sample, `nBlockAlign` 6) supaya konsisten dengan format yang benar-benar dipakai Windows; perhitungan ukuran ring ikut terkoreksi.
  - `CCircularRingBuffer`: penjaga latensi (`SetLatencyLimit` / `SetAlignment`). Backlog di atas 100 ms dibuang pada batas frame sebelum dibaca, sehingga latensi tidak bisa tumbuh tanpa batas.
  - `CMiniportWaveRTStream::SetState`: sisi capture me-`Reset()` ring saat masuk `KSSTATE_RUN`, membuang audio yang menumpuk selama belum ada klien perekam.
- **Sample rate dari GUI tidak lagi disimpan** (`src/adapter.cpp`): format dikunci 48 kHz, jadi nilai lain hanya akan membuat perhitungan ukuran ring salah. Status yang dilaporkan ke Control Panel selalu 48 kHz (sesuai kenyataan).

### Changed
- **DriverVer**: `1.0.0.8` → `1.0.0.9`.

## [1.0.0.8] - 2026-11-25

### Fixed
- **Nama endpoint Render tetap "Speakers" — akar masalah sebenarnya ditemukan** (`src/mintopo.cpp`). Perbaikan di 1.0.0.7 (registrasi `MediaCategories` di software key + purge `MMDevices`) **tidak bekerja**, dan penyebabnya bukan registry. Bukti dari mesin uji:
  - `KSPROPERTY_PIN_NAME` pada bridge pin Render (`cablea_topo` pin 1) **sudah** mengembalikan `"DF-OpenVCable A"`.
  - Software key `HKLM\SYSTEM\CurrentControlSet\Control\Class\{4d36e96c-...}\0010\MediaCategories` **sudah** memuat lengkap 10 entri dengan nama benar.
  - Tetapi endpoint Render tetap `PKEY_Device_DeviceDesc = "Speakers"` dengan ikon `mmres.dll,-3010`, sedangkan endpoint Capture pada filter yang sama memakai nama custom dengan ikon `mmres.dll,-3014`.

  Kesimpulannya: `AudioEndpointBuilder` memperlakukan pin bridge berkategori **`KSNODETYPE_SPEAKER`** sebagai kasus khusus — nama dan ikon dipaksa ke default form factor speaker dan hasil `KSPROPERTY_PIN_NAME` diabaikan. Ini perilaku OS, bukan bug konfigurasi; kategori `KSNODETYPE_MICROPHONE` pada pin Capture tidak terkena special-case, itulah sebabnya Capture selalu benar.

  Perbaikan: `Category` bridge pin Render diganti dari `KSNODETYPE_SPEAKER` → **`KSNODETYPE_LINE_CONNECTOR`** (`DFF21FE3-F70F-11D0-B917-00A0C9223196`). Ini pola yang sama dipakai driver referensi VB-Audio untuk pin render "CABLE In 16ch". `Name` GUID custom (`PINNAME_MCVAD_CABLE_*_RENDER`) tidak berubah, jadi `MediaCategories` tetap menjadi sumber nama.

### Changed
- **Reference string subdevice Topology di-bump** `CableX_Topo` → `CableX_Topo_v2` (`src/adapter.cpp` + `package/mcvad.inf`, keduanya harus identik). Tanpa ini, `AudioEndpointBuilder` memakai ulang endpoint key yang sama dan nama `"Speakers"` yang ter-cache dari build sebelumnya tetap menempel walau descriptor driver sudah berubah. Konsekuensi: 10 endpoint dibuat ulang dengan ID baru, sehingga pengaturan per-aplikasi (volume/format) untuk endpoint lama kembali ke default.
- **DriverVer**: `1.0.0.7` → `1.0.0.8`.
- **`create_device.ps1`: fungsi `Stop-McvadDriver`** — memaksa unload driver sebelum devnode dihapus/dibuat ulang, dipanggil dari `-Cleanup` dan `-Remove`. Tanpa ini PnP memakai ulang image driver yang masih termuat, sehingga devnode baru tetap memakai `PCPIN_DESCRIPTOR` lama walaupun `ImagePath` sudah menunjuk ke binary baru. Gejalanya: interface `CableA_Topo` (lama) dan `CableA_Topo_v2` (baru) muncul bersamaan, endpoint baru tetap bernama `Speakers` dengan form factor 1, dan endpoint-nya menunjuk ke path pin `cablea_topo` bukan `cablea_topo_v2`.
- **`reload_driver.bat` (baru)**: satu klik untuk recovery — unload paksa, hapus devnode + cache endpoint, buat ulang devnode, restart audio service, lalu verifikasi.
- Endpoint Render kini muncul dengan form factor line-level (bukan speaker) di Sound Settings — konsekuensi langsung dari kategori baru, dan memang lebih tepat untuk virtual cable.

## [1.0.0.7] - 2026-11-25

### Fixed
- **Control Panel: checkbox Enable/Disable kembali tercentang sendiri** (`control_panel/control_panel.cpp`): `UpdateCableUI()` dipanggil timer 500 ms dan selalu menulis ulang status checkbox dari driver, sehingga klik user tertimpa sebelum sempat di-Apply. Ditambahkan flag `g_bEnableDirty` — selama user sudah mengubah checkbox tapi belum menekan Apply, timer tidak menyentuh state-nya. Flag direset saat Apply berhasil dan saat pindah tab. Label teks tetap mengikuti state checkbox, bukan status driver.
- **Nama endpoint Render tetap "Speakers"** (`package/mcvad.inf`, `package/create_device.ps1`): descriptor pin di binary sudah benar (diverifikasi langsung dari tabel `PCPIN_DESCRIPTOR` di `mcvad.sys`: `Category=KSNODETYPE_SPEAKER`, `Name=PINNAME_MCVAD_CABLE_A_RENDER`), tetapi nama endpoint tidak pernah berubah karena dua hal:
  - MediaCategories hanya didaftarkan di key global, sedangkan sejak Windows 10 1809 KS membaca **software key device** (`HKR\MediaCategories`) lebih dulu dan key global berstatus deprecated. Kini keduanya didaftarkan.
  - Property store endpoint di-cache AudioEndpointBuilder dan diwariskan lintas update driver, jadi nama lama menempel. `create_device.ps1` sekarang menghapus paksa entri MMDevices milik DF-OpenVCable dengan mengambil ownership (`SeTakeOwnershipPrivilege` + `SeRestorePrivilege`, lalu FullControl untuk Administrators) karena `BUILTIN\Administrators` tidak punya hak `Delete` pada key tersebut, kemudian me-restart `AudioEndpointBuilder` agar endpoint dibuat ulang dari descriptor driver yang sekarang.

### Changed
- `create_device.ps1`: fungsi baru `Remove-RegistryKeyForce` dan `Restart-AudioServices`; `Clear-AudioEndpointCache` kini benar-benar menghapus (bukan best-effort) dan melaporkan jumlah yang gagal.
- **DriverVer**: `1.0.0.6` → `1.0.0.7`.

## [1.0.0.6] - 2026-11-24

### Fixed
- **Endpoint audio duplikat 2x di Sound Settings** (`package/mcvad.inf`): setiap kabel mendaftarkan **dua** device interface `KSCATEGORY_AUDIO` — WaveRT (`CableX`) dan Topology (`CableX_Topo`). AudioEndpointBuilder menghitung setiap interface `KSCATEGORY_AUDIO` sebagai satu "audio device" lalu membuat endpoint dari bridge pin topology, sehingga setiap kabel muncul dua kali: 10 endpoint Render + 10 endpoint Capture, padahal seharusnya 5 + 5. Registrasi `KSCATEGORY_AUDIO` untuk subdevice Topology dihapus; pola yang dipakai sekarang mengikuti Microsoft ("Installing Device Interfaces for an Audio Adapter"): `KSCATEGORY_AUDIO`/`KSCATEGORY_RENDER`/`KSCATEGORY_CAPTURE` → subdevice Wave, `KSCATEGORY_TOPOLOGY` → subdevice Topology.
- **Filter topology mendeklarasikan `KSCATEGORY_AUDIO`** (`src/mintopo.cpp`): sumber kedua registrasi interface audio ganda per kabel. Daftar category filter kini hanya `KSCATEGORY_TOPOLOGY`, sesuai sample WDK `sysvad` (filter descriptor topology: `CategoryCount = 0`).
- **Nama endpoint Render jatuh ke "Speakers"** (`src/mintopo.cpp`): bridge pin endpoint memakai GUID custom sebagai *Category*, sehingga AudioEndpointBuilder tidak dapat menentukan tipe endpoint dan memakai nama form-factor default. Sekarang *Category* = `KSNODETYPE_SPEAKER` / `KSNODETYPE_MICROPHONE`, sedangkan GUID `PINNAME_MCVAD_CABLE_*` dipakai sebagai *Name* sehingga nama endpoint dibaca dari `MediaCategories` ("DF-OpenVCable A" s/d "DF-OpenVCable E"). Pola ini mengikuti sample WDK `sysvad` (`EndpointsCommon/*toptable.h`).

### Changed
- **`create_device.ps1`**:
  - Enumerasi endpoint kini presisi lewat `DEVPKEY_Device_HardwareIds = ROOT\MCVAD` (sebelumnya mencocokkan string "MCVAD"/"OpenVCable" di semua value properti, berisiko salah tangkap endpoint driver lain).
  - Mode baru `-Verify`: laporan read-only jumlah endpoint Render/Capture beserta status ACTIVE/stale, tanpa mengubah apa pun.
  - Ringkasan endpoint otomatis ditampilkan setelah install/cleanup, dengan target 5 Render + 5 Capture.
  - Cache MMDevices dibersihkan **setelah** devnode dihapus (kalau dibersihkan lebih dulu, AudioEndpointBuilder membuat ulang entry dari devnode yang masih hidup), ditambah fallback `pnputil /remove-device` untuk instance yang tertinggal.
  - Catatan ACL: `BUILTIN\Administrators` hanya punya `SetValue`/`ReadKey` pada key MMDevices (tanpa `Delete`), jadi penghapusan cache bersifat best-effort. Entry sisa berstatus `NOTPRESENT` dan tidak tampil di Sound Settings.
- **`update_driver.bat`**: menampilkan verifikasi jumlah endpoint (target 5 Render + 5 Capture) di akhir proses update.
- **DriverVer**: `1.0.0.5` → `1.0.0.6` (11/24/2026) agar paket baru benar-benar menggantikan driver lama di DriverStore.

## [1.0.0] - 2026-11-19
### Added
- **Harness validasi (`tests/`)**:
  - `tests/mcvad_selftest.ps1` — 11 pemeriksaan read-only (Test Signing, Secure Boot, sertifikat, DriverStore, service, devnode, driver running, endpoint, bugcheck 24 jam) dengan exit code = jumlah FAIL dan mode `-Json`.
  - `tests/audio_loopback.ps1` — uji fungsional audio via WinMM P/Invoke: `-List`, `-Probe`, `-Test`. Memainkan tone 1 kHz ke endpoint playback lalu merekam dari endpoint recording kabel yang sama, menghitung RMS dan magnitudo Goertzel.
  - `docs/VALIDATION.md` — prosedur 12 bagian dengan bukti yang diharapkan per langkah dan tabel interpretasi ProblemCode.
- **CI: lint PowerShell** — setiap `.ps1` di `tests/` dan `package/` di-parse dengan `System.Management.Automation.Language.Parser`; syntax error menggagalkan build.
- **CI/CD Pipeline Hijau (GitHub Actions `windows-2022`)**: Build driver kernel `mcvad.sys` (x64 Release) berhasil 0 error, dilanjutkan pembuatan katalog `mcvad.cat` via Inf2Cat (signability test: 0 error, 0 warning), test-signing `mcvad.sys` + `mcvad.cat` via signtool SHA256, kompilasi `control_panel.exe`, dan packaging zip.
- **Rolling GitHub Release**: Setiap push ke `main` otomatis mempublikasikan ulang rilis `latest-build` sehingga paket dapat diunduh tanpa login:
  `https://github.com/dimzfrantic/DF-OpenVCable/releases/download/latest-build/DF-OpenVCable-v1.0.0-x64.zip`
- **Fail-fast Packaging**: Step packaging memverifikasi keberadaan `mcvad.sys`, `mcvad.inf`, `mcvad.cat`, `control_panel.exe`, dan `MCVAD_TestCert.cer` sebelum zip dibuat.

### Fixed
- **LNK2001 unresolved external `CLSID_PortWaveRT` / `IID_IUnknown` / `IID_IMiniport*`**: `DEFINE_GUID` pada header PortCls hanya mendeklarasikan simbol `extern`. Diperbaiki dengan `#define PUT_GUIDS_HERE` + `#include <initguid.h>` sebelum `driver.h` pada `src/adapter.cpp` sehingga GUID COM terinstansiasi di binary driver.
- **Entry point Control Panel**: `WinMain` (ANSI) tidak cocok dengan build Unicode (`/DUNICODE`). Diubah menjadi `wWinMain` (`PWSTR`).
- **Step build Control Panel tidak deterministik**: Pemanggilan `build.bat` via `cmd` diganti pemanggilan langsung `cl.exe` dari PowerShell dengan pemeriksaan `$LASTEXITCODE`.
- **`mcvad.cat` tidak pernah terbentuk**: `Inf2Cat.exe` hanya disediakan dalam varian **x86** (`Windows Kits\10\bin\<ver>\x86\Inf2Cat.exe`), sedangkan pencarian sebelumnya difilter ke folder `x64`. Pencarian tool kini dilakukan rekursif lintas versi kit tanpa filter arsitektur.

### Changed
- **Migrasi alokasi memori kernel** dari `ExAllocatePoolWithTag(NonPagedPoolNx, ...)` ke `ExAllocatePool2(POOL_FLAG_NON_PAGED, ...)` pada `src/ringbuffer.cpp` dan `src/minwavert.cpp` (menghilangkan warning C4996 deprecated).
- **Rebranding artefak rilis**: nama paket dan artifact CI menjadi `DF-OpenVCable-v1.0.0-x64` (sebelumnya `MCVAD-v1.2.0`).

## [1.2.0] - 2026-11-18
### Added
- **WaveRT Pin & Filter Topologies** (`src/minwavert.cpp`):
  - Implementasi `PCPIN_DESCRIPTOR` untuk Pin 0 (Render Sink) dan Pin 1 (Capture Source).
  - Dukungan `KSDATARANGE_AUDIO` multi-format: 44.1 kHz s/d 192 kHz, 16 s/d 32-bit container, stereo.
  - `CMiniportWaveRT::GetDescription` sekarang menyediakan `PCFILTER_DESCRIPTOR` lengkap dengan kategori `KSCATEGORY_AUDIO`, `KSCATEGORY_RENDER`, `KSCATEGORY_CAPTURE`, dan `KSCATEGORY_REALTIME`.
  - `CMiniportWaveRT::DataRangeIntersection` untuk negosiasi format audio engine otomatis.
### Fixed & Improved
- **Anti-Glitch & Zero-Clock-Drift Streaming Engine** (`src/minwavert.cpp`):
  - Mengganti transfer chunk statis di `GetPosition` dengan streaming berbasis **delta bytes real-time** yang disinkronkan langsung via `KeQueryPerformanceCounter` (QPC).
  - Menangani wrapping boundary pada DMA cyclic buffer secara presisi agar tidak ada sample terpotong atau terduplikasi.
  - Menghitung `bytesPerSec` dinamis langsung dari `WaveFormatExt.Format.nAvgBytesPerSec`.

## [1.1.0] - 2026-11-18
### Added
- **IOCTL Communication Channel** (`src/mcvad_ioctl.h`, `src/adapter.cpp`):
  - Endpoint IOCTL `\Device\MCVAD_Control` dan symlink `\DosDevices\MCVAD_Control`.
  - `IOCTL_MCVAD_GET_CABLE_STATUS`: Polling status real-time 5 kabel.
  - `IOCTL_MCVAD_SET_CABLE_CONFIG`: Mengubah sample rate & durasi buffer secara dinamis.
  - `IOCTL_MCVAD_RESET_CABLE_STATS`: Mereset statistik glitch.
- **Dynamic Ring Buffer Resizing & Glitch Monitoring** (`src/ringbuffer.h`, `src/ringbuffer.cpp`):
  - Method `Resize(newSizeBytes)` dengan proteksi spinlock non-paged pool.
  - Counter `OverrunCount` dan `UnderrunCount`.
- **Control Panel GUI Helper** (`control_panel/`):
  - `control_panel/mcvad_control_panel.py`: GUI portable Tkinter dengan visual progress bar, tab 5 kabel, dan pilihan default (48kHz / 500ms).
  - `control_panel/control_panel.cpp`: Win32 native GUI C++ mandiri.
  - `control_panel/build.bat`: Helper kompilasi otomatis (MSVC / MinGW / Clang).
- **Automated Deployment & Signing Scripts** (`package/`):
  - `package/setup_cert.bat`: Otomasi pembuatan sertifikat test signing lokal dan registrasi ke Root & TrustedPublisher stores.
  - `package/sign_driver.bat`: Helper penandatanganan `mcvad.sys` dan `mcvad.cat`.
  - `package/create_device.ps1`: Enumerator node virtual PnP `ROOT\MCVAD` langsung via SetupAPI tanpa butuh devcon external.
  - `package/install.bat`: One-click installer untuk mengaktifkan test signing, mendaftarkan device node, dan install driver ke Windows.
  - `package/uninstall.bat`: One-click uninstaller bersih untuk mencabut device node dan driver store.

## [1.0.0-draft] - 2026-11-18
### Added
- **Single Driver Architecture**: Menggabungkan 5 pasang virtual audio endpoint (Virtual Cable 1/Main, A, B, C, D) ke dalam satu adapter driver kernel.
- **Circular Ring Buffer Engine** (`src/ringbuffer.h`, `src/ringbuffer.cpp`):
  - Alokasi memori non-paged (`NonPagedPoolNx`).
  - Proteksi spinlock thread-safe antar-core dan IRQL.
  - Mekanisme penanganan overflow (FIFO drop) dan underflow (digital silence padding).
- **WaveRT Miniport & Stream** (`src/minwavert.h`, `src/minwavert.cpp`):
  - Implementasi `IMiniportWaveRT` dan `IMiniportWaveRTStream`.
  - Direct DMA user-mode mapping via `IoAllocateMdl` dan `MmBuildMdlForNonPagedPool`.
  - Simulasi linear clock dan timestamp query via `KeQueryPerformanceCounter`.
- **Adapter & PnP Lifecycle** (`src/adapter.cpp`):
  - Registrasi driver entry via `PcInitializeAdapterDriver` dan `PcAddAdapterDevice`.
  - Instansiasi 5 pasang subdevice terpisah (`Cable1`, `CableA`, `CableB`, `CableC`, `CableD`) via `PcRegisterSubdevice`.
- **Packaging & INF** (`package/mcvad.inf`, `package/mcvad.vcxproj`):
  - File instalasi INF modern Windows 10/11 (Class MEDIA, ClassGUID `{4d36e96c-e325-11ce-bfc1-08002be10318}`).
  - Registrasi 5 interface KSCATEGORY (RENDER, CAPTURE, AUDIO, REALTIME).
  - MSBuild project file siap kompilasi WDK.
