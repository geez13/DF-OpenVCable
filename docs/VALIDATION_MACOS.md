# Panduan Validasi & Pengujian macOS — DF-OpenVCable

Dokumen ini menjelaskan cara build, memasang, memverifikasi, dan menguji **DF-OpenVCable versi macOS** (`DF-OpenVCable.driver`, `DF-OpenVCable Control Panel.app`, dan `dfovc-ctl`).

---

## 1. Prasyarat Sistem
- **macOS 13.0 (Ventura) atau lebih baru** (Apple Silicon `arm64` maupun Intel `x86_64`).
- Tidak memerlukan **System Integrity Protection (SIP) disabled** maupun kernel extension (`.kext`) — driver menggunakan arsitektur user-space resmi Apple **CoreAudio HAL `AudioServerPlugIn`** yang dimuat aman oleh `coreaudiod`.
- Jika ingin kompilasi dari source: cukup **Xcode Command Line Tools** (`xcode-select --install`).

---

## 2. Kompilasi dari Source & Unit Test

```bash
cd macos

# Build universal binary (arm64 + x86_64): driver, Control Panel .app, CLI
make

# Jalankan unit test ring buffer (bit-exact loopback, wrap-around, Goertzel 1 kHz, mute/underrun)
make test

# Kemas menjadi zip rilis siap pakai
make package
```

Untuk iterasi lokal lebih cepat (hanya arsitektur Mac yang sedang dipakai):

```bash
make UNIVERSAL=0 all test
```

---

## 3. Instalasi & Reload Driver

```bash
cd macos
./scripts/install.sh
```

Skrip instalasi akan:
1. Menyalin `DF-OpenVCable.driver` ke `/Library/Audio/Plug-Ins/HAL/` dengan kepemilikan `root:wheel`.
2. Menyalin `DF-OpenVCable Control Panel.app` ke `/Applications/`.
3. Menyalin CLI `dfovc-ctl` ke `/usr/local/bin/dfovc-ctl`.
4. Me-restart `coreaudiod` agar 5 perangkat virtual langsung muncul di **System Settings → Sound** dan **Audio MIDI Setup**.

---

## 4. Verifikasi Instalasi (Self-Test)

```bash
./macos/scripts/selftest.sh
```

Hasil yang diharapkan: semua pemeriksaan berstatus `[PASS]` dengan `0 FAIL`:
- Bundle terpasang di `/Library/Audio/Plug-Ins/HAL/DF-OpenVCable.driver`
- `Info.plist` valid
- Code signature valid (`codesign --verify`)
- Arsitektur Mach-O cocok dengan host (`arm64` / `x86_64`)
- Simbol factory `_DFOpenVCable_Create` terekspor
- `coreaudiod` berjalan
- 5 perangkat aktif: `DF-OpenVCable A` s/d `DF-OpenVCable E`
- IPC properti kustom CoreAudio (`dfst`) responsif

---

## 5. Uji Audio Loopback 1 kHz (End-to-End Live CoreAudio)

```bash
# Uji kabel A (default)
dfovc-ctl loopback-test A

# Atau uji seluruh 5 kabel secara berurutan
for c in A B C D E; do
  dfovc-ctl loopback-test "$c"
done
```

Uji ini memutar gelombang sinus 1 kHz secara real-time ke stream **Output** kabel yang dipilih sambil merekam dari stream **Input** kabel yang sama melalui `coreaudiod`, lalu menghitung **RMS** dan rasio magnitudo **Goertzel 1000 Hz vs 500 Hz**.

---

## 6. Konfigurasi Runtime & Persistensi

Anda bisa mengatur kabel via GUI (**`/Applications/DF-OpenVCable Control Panel.app`**) atau via CLI:

```bash
# Lihat status real-time seluruh 5 kabel
dfovc-ctl status

# Ubah sample rate & ukuran ring buffer Cable A ke 96 kHz / 250 ms
dfovc-ctl config A --rate 96000 --buffer 250

# Nonaktifkan (mute) Cable B sementara
dfovc-ctl config B --disable

# Aktifkan kembali Cable B & reset statistik glitch
dfovc-ctl config B --enable
dfovc-ctl reset-stats B
```

Pengaturan disimpan menggunakan penyimpanan persisten resmi `coreaudiod` (`AudioServerPlugInHostInterface::WriteToStorage`), sehingga tetap bertahan setelah `./macos/scripts/reload.sh` maupun reboot Mac.

---

## 7. Pencabutan / Uninstall Bersih

```bash
./macos/scripts/uninstall.sh
```

---

## 8. Troubleshooting

| Gejala | Penyebab Umum | Solusi |
|---|---|---|
| Perangkat tidak muncul di Sound Settings | `coreaudiod` belum restart atau kepemilikan file bukan `root:wheel` | Jalankan `./macos/scripts/install.sh` lalu `./macos/scripts/selftest.sh` |
| Driver ditolak di Apple Silicon | Binary belum ditandatangani (`codesign`) atau kena quarantine flag | Pastikan `codesign --verify --deep --strict /Library/Audio/Plug-Ins/HAL/DF-OpenVCable.driver` sukses dan jalankan `sudo xattr -dr com.apple.quarantine /Library/Audio/Plug-Ins/HAL/DF-OpenVCable.driver` |
| Loopback test gagal (RMS ≈ 0) | Kabel sedang di-disable atau volume output/input 0 / muted | Jalankan `dfovc-ctl config A --enable --out-vol 1.0 --in-vol 1.0 --out-mute off --in-mute off` |
| Cek log internal `coreaudiod` | Ingin melihat pesan load plug-in dari sistem | `log show --last 2m --predicate 'process == "coreaudiod"'` |
