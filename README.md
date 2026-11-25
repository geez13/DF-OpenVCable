# DF-OpenVCable (DimzFrantic Open Virtual Audio Cable)

**DF-OpenVCable** adalah driver audio virtual lintas platform untuk **Windows 10/11 (64-bit, WDM/WaveRT kernel-mode)** dan **macOS 13+ (Apple Silicon `arm64` & Intel `x86_64`, CoreAudio HAL `AudioServerPlugIn`)**.

Proyek ini menghadirkan arsitektur **All-in-One Single Driver**: satu paket driver tunggal (`mcvad.sys` di Windows atau `DF-OpenVCable.driver` di macOS) yang langsung menyediakan **5 pasang Virtual Audio Cable independen** secara simultan tanpa memecah-mecah driver menjadi beberapa paket terpisah.

## 🎛️ Daftar Endpoint Audio (5 Pasang Kabel Independen)
1. **DF-OpenVCable A**: Output / Playback & Input / Capture
2. **DF-OpenVCable B**: Output / Playback & Input / Capture
3. **DF-OpenVCable C**: Output / Playback & Input / Capture
4. **DF-OpenVCable D**: Output / Playback & Input / Capture
5. **DF-OpenVCable E**: Output / Playback & Input / Capture

Setiap sinyal audio yang diputar ke perangkat Playback/Output secara otomatis dialirkan secara *loss-free* dan *zero-clock-drift* ke perangkat Recording/Input pada kabel yang bersangkutan melalui ring buffer memori internal driver.

---

## Fitur Utama
- **Windows WaveRT Modern Architecture**: Menggunakan interface `IMiniportWaveRT` standar Windows 10/11 untuk latensi ultra rendah dan efisiensi CPU.
- **macOS CoreAudio HAL `AudioServerPlugIn`**: Driver user-space resmi Apple (`DF-OpenVCable.driver`, Universal `arm64` + `x86_64`) yang berjalan aman di dalam `coreaudiod` tanpa perlu menonaktifkan SIP atau memasang kernel extension (`.kext`).
- **Zero Clock Drift Streaming**: Sinkronisasi transfer data presisi tinggi (`KeQueryPerformanceCounter` di Windows; `mach_absolute_time` + timestamp-indexed lock-free ring buffer di macOS) untuk mencegah audio glitch, stutter, dan dropouts.
- **Dynamic Configuration & Persistence**: Pengaturan buffer duration (50 ms hingga 2000 ms), sample rate (44.1 kHz hingga 192 kHz di macOS), mute/enable per-kabel, serta volume native tersimpan persisten lintas reboot.
- **Native Control Panel GUI & CLI**:
  - Windows: Python (Tkinter) dan Win32 C++ native.
  - macOS: Aplikasi native SwiftUI (`DF-OpenVCable Control Panel.app`) dengan meter level audio real-time dan CLI (`dfovc-ctl`) lengkap dengan uji loopback 1 kHz.
- **One-Click Deployment**: Skrip installer & uninstaller mandiri (`package/install.bat` di Windows; `macos/scripts/install.sh` di macOS).
- **Automated CI/CD**: Dilengkapi workflow GitHub Actions untuk build dan packaging otomatis Windows & macOS.

---

## Panduan Instalasi Cepat — macOS (Apple Silicon & Intel)

### Syarat Sistem:
- **macOS 13.0 (Ventura) atau lebih baru** (`arm64` / `x86_64`).
- Tidak memerlukan disable SIP.
- Untuk build dari source: cukup **Xcode Command Line Tools** (`xcode-select --install`).

### Build & Pemasangan dari Source:
```bash
cd macos

# Kompilasi Universal Binary (arm64 + x86_64): driver, Control Panel .app, & CLI
make

# Jalankan unit test ring buffer + integration test AudioServerPlugIn
make test

# Pasang ke /Library/Audio/Plug-Ins/HAL, /Applications, dan /usr/local/bin
./scripts/install.sh
```

### Menjalankan Control Panel & CLI (macOS):
- **GUI Native**: Buka `/Applications/DF-OpenVCable Control Panel.app`.
- **CLI**:
  ```bash
  dfovc-ctl list
  dfovc-ctl status
  dfovc-ctl config A --rate 96000 --buffer 250
  ```

### Verifikasi Instalasi & Uji Audio Loopback (macOS):
Panduan lengkap tersedia di **`docs/VALIDATION_MACOS.md`**.

```bash
# Cek status instalasi, signature, arsitektur, dan 5 endpoint CoreAudio
./macos/scripts/selftest.sh

# Uji audio loopback 1 kHz secara langsung via CoreAudio (Output -> Input)
dfovc-ctl loopback-test A
```

### Pencabutan / Uninstall Bersih (macOS):
```bash
./macos/scripts/uninstall.sh
```

---

## Panduan Instalasi Cepat — Windows 10/11 (x64)

### Unduh Paket Siap Pakai (tanpa login)
Build CI terbaru selalu tersedia di halaman rilis:
`https://github.com/dimzfrantic/DF-OpenVCable/releases/download/latest-build/DF-OpenVCable-v1.0.0-x64.zip`

Isi paket: `mcvad.sys`, `mcvad.inf`, `mcvad.cat` (test-signed), `MCVAD_TestCert.cer`, `install.bat`, `uninstall.bat`, `setup_cert.bat`, `create_device.ps1`, `control_panel.exe`, `mcvad_control_panel.py`.

### Syarat Sistem:
- Windows 10 atau Windows 11 (64-bit / x64).
- Hak akses Administrator.

### Langkah Pemasangan:
1. Unduh paket rilis atau kompilasi driver.
2. Buka folder `package/`.
3. Klik kanan `install.bat` -> pilih **Run as Administrator**.
4. Skrip akan secara otomatis:
   - Mendaftarkan sertifikat code signing lokal 5 tahun ke Windows Certificate Store.
   - Mengaktifkan mode Windows Test Signing (`bcdedit /set testsigning on`).
   - Mendaftarkan node hardware PnP `ROOT\MCVAD` via Windows SetupAPI.
   - Memasang driver ke Driver Store via `pnputil.exe`.
5. *(Jika Windows Test Signing baru pertama kali diaktifkan, restart komputer 1x agar driver dimuat).*

### Menjalankan Control Panel:
- Versi Python: Jalankan `python control_panel/mcvad_control_panel.py`.
- Versi Native C++: Jalankan `control_panel/control_panel.exe`.

### Verifikasi Instalasi & Uji Audio:
Panduan bertahap lengkap (prasyarat, instalasi, uji tone loopback, troubleshooting BSOD):
**`docs/VALIDATION.md`**

```powershell
# Cek status instalasi & prasyarat
powershell -ExecutionPolicy Bypass -File tests\mcvad_selftest.ps1

# Uji apakah audio benar-benar mengalir (tone 1 kHz playback -> recording)
powershell -ExecutionPolicy Bypass -File tests\audio_loopback.ps1 -Test -Pattern "DF-OpenVCable 1"
```

### Pencabutan / Uninstall Bersih:
- Klik kanan `package/uninstall.bat` -> pilih **Run as Administrator**.

---

## 📄 Lisensi
Open-source under MIT License. Dibuat dan dikembangkan oleh **dimzfrantic**.
