# Architecture & Technical Design: MCVAD

## 1. Topologi Subdevice & Filter

Windows Audio Subsystem (PortCls) mengizinkan satu adapter mendaftarkan banyak subdevice:

```
                            ┌─── [ MCVAD Adapter Driver (.sys) ] ───┐
                            │                                       │
[ App 1 Player ]  ──► [ Cable 1 In  (Render)  ] ──► (FIFO Buffer 0) ──► [ Cable 1 Out (Capture) ] ──► [ App 1 Rec ]
[ App 2 Player ]  ──► [ Cable A In  (Render)  ] ──► (FIFO Buffer 1) ──► [ Cable A Out (Capture) ] ──► [ App 2 Rec ]
[ App 3 Player ]  ──► [ Cable B In  (Render)  ] ──► (FIFO Buffer 2) ──► [ Cable B Out (Capture) ] ──► [ App 3 Rec ]
[ App 4 Player ]  ──► [ Cable C In  (Render)  ] ──► (FIFO Buffer 3) ──► [ Cable C Out (Capture) ] ──► [ App 4 Rec ]
[ App 5 Player ]  ──► [ Cable D In  (Render)  ] ──► (FIFO Buffer 4) ──► [ Cable D Out (Capture) ] ──► [ App 5 Rec ]
                            └───────────────────────────────────────┘
```

## 2. Komponen Inti Driver

1. **Adapter / Driver Entry (`adapter.cpp`)**:
   - Mendaftarkan WDF / PortCls adapter driver (`PcInitializeAdapterDriver`, `PcAddAdapterDevice`).
   - Menginisialisasi array konteks virtual cable (`CABLE_CONTEXT g_Cables[MAX_CABLES]`).

2. **Miniport WaveRT (`minwavert.cpp`)**:
   - Mengimplementasikan `IMiniportWaveRT` dan `IMiniportWaveRTStream`.
   - Menangani alokasi buffer DMA virtual (`AllocatePagesForMdl`, `MapAllocatedPages`).
   - Memberikan sinyal posisi stream waktu nyata ke OS (`GetPosition`, `GetClockRegister`).

3. **Loopback Ring Buffer (`ringbuffer.cpp`)**:
   - Mengelola memori antrean sirkular di `NonPagedPoolNx`.
   - Sinkronisasi atomic lock-free / spinlock ringan untuk menjamin pembacaan data tanpa glitch antar thread render dan capture.
   - Kompensasi clock drift jika rate konsumsi capture berbeda tipis dengan produksi render.

4. **INF & PnP Configuration (`mcvad.inf`)**:
   - Mendaftarkan 5 subdevice Interface GUID.
   - Menyediakan FriendlyName di Sound Control Panel Windows.

## 3. Komparasi dengan VB-CABLE Asli
| Parameter | VB-Audio Cable | MCVAD (Replika Kita) |
|---|---|---|
| Jumlah File Driver | 3 paket driver terpisah (CABLE, A+B, C+D) | 1 driver terpadu (`.sys` tunggal) |
| Proses Instalasi | 3x install & 3x reboot | 1x install tunggal |
| Endpoint Aktif | 1 s/d 5 kabel (tergantung paket yang dibeli) | 5 kabel langsung aktif |
| Basis Miniport | PortCls WaveRT / WaveCyclic hybrid | Murni WaveRT Modern |

---

## 4. Arsitektur Versi macOS (`macos/`)

Di macOS, DF-OpenVCable diimplementasikan sebagai satu **CoreAudio HAL `AudioServerPlugIn`** (`DF-OpenVCable.driver`, Universal `arm64` + `x86_64`) yang dimuat langsung oleh daemon `coreaudiod` di `/Library/Audio/Plug-Ins/HAL/`:

```
                    ┌─── [ DF-OpenVCable.driver (AudioServerPlugIn) ] ───┐
                    │                                                    │
[ App 1 Output ] ──► [ DF-OpenVCable A Out ] ──► (Timestamp Ring 0) ──► [ DF-OpenVCable A In ] ──► [ App 1 Rec ]
[ App 2 Output ] ──► [ DF-OpenVCable B Out ] ──► (Timestamp Ring 1) ──► [ DF-OpenVCable B In ] ──► [ App 2 Rec ]
[ App 3 Output ] ──► [ DF-OpenVCable C Out ] ──► (Timestamp Ring 2) ──► [ DF-OpenVCable C In ] ──► [ App 3 Rec ]
[ App 4 Output ] ──► [ DF-OpenVCable D Out ] ──► (Timestamp Ring 3) ──► [ DF-OpenVCable D In ] ──► [ App 4 Rec ]
[ App 5 Output ] ──► [ DF-OpenVCable E Out ] ──► (Timestamp Ring 4) ──► [ DF-OpenVCable E In ] ──► [ App 5 Rec ]
                    └────────────────────────────────────────────────────┘
```

### Komponen Inti macOS
1. **HAL Plug-In (`macos/driver/DFOpenVCableDriver.cpp`)**:
   - Mengekspor factory `DFOpenVCable_Create` dan mendaftarkan 5 `AudioDevice` duplex independen (`DF-OpenVCable A` .. `E`), masing-masing memiliki stream Input & Output 32-bit Float Stereo serta kontrol Volume & Mute native.
   - Clock zero-drift berbasis `mach_absolute_time()` (`GetZeroTimeStamp`) dengan `kAudioDeviceClockAlgorithmRaw`.
   - Perubahan sample rate (`44.1` / `48` / `88.2` / `96` / `192 kHz`) dan ukuran ring buffer (`50`–`2000 ms`) diproses aman melalui protokol `RequestDeviceConfigurationChange` → `PerformDeviceConfigurationChange`.
   - Persistensi pengaturan lintas restart `coreaudiod` dan reboot menggunakan `AudioServerPlugInHostInterface::WriteToStorage` / `CopyFromStorage`.
2. **Timestamp-Indexed Ring Buffer (`macos/driver/ring_buffer.h`)**:
   - Lock-free dan bebas alokasi pada thread IO real-time CoreAudio.
   - Diindeks langsung dengan `mSampleTime` (`WriteMix` menulis ke `mOutputTime`, `ReadInput` membaca dari `mInputTime` pada clock perangkat yang sama), menghasilkan loopback bit-exact tanpa drift dan tanpa glitch.
3. **IPC Sandboxed via Custom CoreAudio Properties (`macos/driver/dfovc_protocol.h`)**:
   - Properti `CFPropertyList` (`dfst`, `dfcs`, `dfcc`, `dfrs`) memungkinkan Control Panel dan CLI berkomunikasi langsung dengan driver di dalam sandbox `coreaudiod` tanpa perlu hak akses root saat runtime.
4. **Control Panel & CLI (`macos/control_panel/`, `macos/cli/`)**:
   - `DF-OpenVCable Control Panel.app`: GUI native SwiftUI dengan 5 tab kabel, kontrol sample rate/buffer/volume/mute/enable, serta meter peak & diagnostik real-time.
   - `dfovc-ctl`: CLI untuk manajemen perangkat dan pengujian loopback tone 1 kHz live via CoreAudio HAL.
