import AppKit
import CoreAudio
import Foundation
import SwiftUI

// ============================================================================
// CoreAudio Bridge for DF-OpenVCable.driver
// ============================================================================

private func fourCC(_ s: String) -> UInt32 {
    var result: UInt32 = 0
    for byte in s.utf8.prefix(4) {
        result = (result << 8) | UInt32(byte)
    }
    return result
}

private let kBundleID = "com.dfopenvcable.driver"
private let kPropAllStatus: AudioObjectPropertySelector = fourCC("dfst")
private let kPropCableStatus: AudioObjectPropertySelector = fourCC("dfcs")
private let kPropCableConfig: AudioObjectPropertySelector = fourCC("dfcc")
private let kPropResetStats: AudioObjectPropertySelector = fourCC("dfrs")

private let kCableUIDs = [
    "com.dfopenvcable.device.cable_a",
    "com.dfopenvcable.device.cable_b",
    "com.dfopenvcable.device.cable_c",
    "com.dfopenvcable.device.cable_d",
    "com.dfopenvcable.device.cable_e",
]

struct CableStatus: Identifiable, Equatable {
    var id: Int { cableIndex }
    var cableIndex: Int
    var name: String
    var uid: String
    var sampleRate: Double
    var bitsPerSample: Int
    var channels: Int
    var bufferDurationMs: Int
    var bufferSizeBytes: Int
    var currentBufferFill: Int
    var overrunCount: Int
    var underrunCount: Int
    var isRenderActive: Bool
    var isCaptureActive: Bool
    var isEnabled: Bool
    var outputVolume: Double
    var inputVolume: Double
    var outputMute: Bool
    var inputMute: Bool
    var outputPeak: Double
    var inputPeak: Double
    var ioClientCount: Int

    static func offlineDefaults() -> [CableStatus] {
        let letters = ["A", "B", "C", "D", "E"]
        return (0..<5).map { idx in
            CableStatus(
                cableIndex: idx,
                name: "DF-OpenVCable \(letters[idx])",
                uid: kCableUIDs[idx],
                sampleRate: 48000,
                bitsPerSample: 32,
                channels: 2,
                bufferDurationMs: 500,
                bufferSizeBytes: 192000,
                currentBufferFill: 0,
                overrunCount: 0,
                underrunCount: 0,
                isRenderActive: false,
                isCaptureActive: false,
                isEnabled: true,
                outputVolume: 1.0,
                inputVolume: 1.0,
                outputMute: false,
                inputMute: false,
                outputPeak: 0.0,
                inputPeak: 0.0,
                ioClientCount: 0
            )
        }
    }
}

final class DFOVCDriverClient {
    func findPlugInObjectID() -> AudioObjectID? {
        var addr = AudioObjectPropertyAddress(
            mSelector: kAudioHardwarePropertyTranslateBundleIDToPlugIn,
            mScope: kAudioObjectPropertyScopeGlobal,
            mElement: kAudioObjectPropertyElementMain
        )
        var bundleID: CFString = kBundleID as CFString
        var plugInID: AudioObjectID = kAudioObjectUnknown
        var dataSize = UInt32(MemoryLayout<AudioObjectID>.size)

        let err = withUnsafePointer(to: &bundleID) { qualPtr in
            AudioObjectGetPropertyData(
                AudioObjectID(kAudioObjectSystemObject),
                &addr,
                UInt32(MemoryLayout<CFString>.size),
                qualPtr,
                &dataSize,
                &plugInID
            )
        }
        if err == noErr && plugInID != kAudioObjectUnknown {
            return plugInID
        }
        return nil
    }

    func findDeviceObjectID(uid: String) -> AudioObjectID? {
        var addr = AudioObjectPropertyAddress(
            mSelector: kAudioHardwarePropertyTranslateUIDToDevice,
            mScope: kAudioObjectPropertyScopeGlobal,
            mElement: kAudioObjectPropertyElementMain
        )
        var cfUID: CFString = uid as CFString
        var devID: AudioObjectID = kAudioObjectUnknown
        var dataSize = UInt32(MemoryLayout<AudioObjectID>.size)

        let err = withUnsafePointer(to: &cfUID) { qualPtr in
            AudioObjectGetPropertyData(
                AudioObjectID(kAudioObjectSystemObject),
                &addr,
                UInt32(MemoryLayout<CFString>.size),
                qualPtr,
                &dataSize,
                &devID
            )
        }
        if err == noErr && devID != kAudioObjectUnknown {
            return devID
        }
        return nil
    }

    func fetchAllStatus() -> (connected: Bool, version: String, cables: [CableStatus]) {
        if let plugInID = findPlugInObjectID(),
           let dict = getPlistProperty(objectID: plugInID, selector: kPropAllStatus) as? [String: Any],
           let rawCables = dict["cables"] as? [[String: Any]] {
            let version = dict["version"] as? String ?? "1.0.0"
            let parsed = rawCables.compactMap { parseCable($0) }
            if parsed.count == 5 {
                return (true, version, parsed)
            }
        }

        // Fallback: query per-device custom property directly
        var cables: [CableStatus] = []
        for uid in kCableUIDs {
            guard let devID = findDeviceObjectID(uid: uid),
                  let dict = getPlistProperty(objectID: devID, selector: kPropCableStatus) as? [String: Any],
                  let c = parseCable(dict) else {
                return (false, "N/A", CableStatus.offlineDefaults())
            }
            cables.append(c)
        }
        return (true, "1.0.0", cables)
    }

    @discardableResult
    func applyConfig(
        cableIndex: Int,
        sampleRate: Double,
        bufferMs: Int,
        enabled: Bool,
        outputVolume: Double,
        inputVolume: Double,
        outputMute: Bool,
        inputMute: Bool
    ) -> Bool {
        let payload: [String: Any] = [
            "cableIndex": cableIndex,
            "sampleRate": sampleRate,
            "bufferDurationMs": bufferMs,
            "isEnabled": enabled,
            "outputVolume": outputVolume,
            "inputVolume": inputVolume,
            "outputMute": outputMute,
            "inputMute": inputMute,
        ]
        if let plugInID = findPlugInObjectID() {
            return setPlistProperty(objectID: plugInID, selector: kPropCableConfig, dict: payload)
        }
        if cableIndex >= 0 && cableIndex < kCableUIDs.count,
           let devID = findDeviceObjectID(uid: kCableUIDs[cableIndex]) {
            return setPlistProperty(objectID: devID, selector: kPropCableConfig, dict: payload)
        }
        return false
    }

    @discardableResult
    func resetStats(cableIndex: Int) -> Bool {
        let payload: [String: Any] = ["cableIndex": cableIndex]
        if let plugInID = findPlugInObjectID() {
            return setPlistProperty(objectID: plugInID, selector: kPropResetStats, dict: payload)
        }
        if cableIndex >= 0 && cableIndex < kCableUIDs.count,
           let devID = findDeviceObjectID(uid: kCableUIDs[cableIndex]) {
            return setPlistProperty(objectID: devID, selector: kPropResetStats, dict: payload)
        }
        return false
    }

    private func getPlistProperty(objectID: AudioObjectID, selector: AudioObjectPropertySelector) -> Any? {
        var addr = AudioObjectPropertyAddress(
            mSelector: selector,
            mScope: kAudioObjectPropertyScopeGlobal,
            mElement: kAudioObjectPropertyElementMain
        )
        var dataSize = UInt32(MemoryLayout<Unmanaged<CFPropertyList>?>.size)
        var rawPlist: Unmanaged<CFPropertyList>? = nil
        let err = AudioObjectGetPropertyData(objectID, &addr, 0, nil, &dataSize, &rawPlist)
        guard err == noErr, let unmanaged = rawPlist else { return nil }
        return unmanaged.takeRetainedValue()
    }

    private func setPlistProperty(objectID: AudioObjectID, selector: AudioObjectPropertySelector, dict: [String: Any]) -> Bool {
        var addr = AudioObjectPropertyAddress(
            mSelector: selector,
            mScope: kAudioObjectPropertyScopeGlobal,
            mElement: kAudioObjectPropertyElementMain
        )
        let cfDict = dict as CFDictionary
        var rawPtr: UnsafeRawPointer? = UnsafeRawPointer(Unmanaged.passUnretained(cfDict).toOpaque())
        let size = UInt32(MemoryLayout<UnsafeRawPointer?>.size)
        let err = withExtendedLifetime(cfDict) {
            withUnsafePointer(to: &rawPtr) { ptr in
                AudioObjectSetPropertyData(objectID, &addr, 0, nil, size, ptr)
            }
        }
        return err == noErr
    }

    private func parseCable(_ d: [String: Any]) -> CableStatus? {
        guard let idx = (d["cableIndex"] as? NSNumber)?.intValue else { return nil }
        return CableStatus(
            cableIndex: idx,
            name: d["name"] as? String ?? "DF-OpenVCable",
            uid: d["uid"] as? String ?? "",
            sampleRate: (d["sampleRate"] as? NSNumber)?.doubleValue ?? 48000,
            bitsPerSample: (d["bitsPerSample"] as? NSNumber)?.intValue ?? 32,
            channels: (d["channels"] as? NSNumber)?.intValue ?? 2,
            bufferDurationMs: (d["bufferDurationMs"] as? NSNumber)?.intValue ?? 500,
            bufferSizeBytes: (d["bufferSizeBytes"] as? NSNumber)?.intValue ?? 0,
            currentBufferFill: (d["currentBufferFill"] as? NSNumber)?.intValue ?? 0,
            overrunCount: (d["overrunCount"] as? NSNumber)?.intValue ?? 0,
            underrunCount: (d["underrunCount"] as? NSNumber)?.intValue ?? 0,
            isRenderActive: (d["isRenderActive"] as? NSNumber)?.boolValue ?? false,
            isCaptureActive: (d["isCaptureActive"] as? NSNumber)?.boolValue ?? false,
            isEnabled: (d["isEnabled"] as? NSNumber)?.boolValue ?? true,
            outputVolume: (d["outputVolume"] as? NSNumber)?.doubleValue ?? 1.0,
            inputVolume: (d["inputVolume"] as? NSNumber)?.doubleValue ?? 1.0,
            outputMute: (d["outputMute"] as? NSNumber)?.boolValue ?? false,
            inputMute: (d["inputMute"] as? NSNumber)?.boolValue ?? false,
            outputPeak: (d["outputPeak"] as? NSNumber)?.doubleValue ?? 0.0,
            inputPeak: (d["inputPeak"] as? NSNumber)?.doubleValue ?? 0.0,
            ioClientCount: (d["ioClientCount"] as? NSNumber)?.intValue ?? 0
        )
    }
}

// ============================================================================
// ViewModel
// ============================================================================

@MainActor
final class ControlPanelViewModel: ObservableObject {
    static let sampleRates: [Double] = [44100, 48000, 88200, 96000, 192000]
    static let bufferDurations: [Int] = [50, 100, 250, 500, 1000, 2000]
    static let tabLabels = ["Cable A", "Cable B", "Cable C", "Cable D", "Cable E"]

    @Published var isConnected = false
    @Published var driverVersion = "1.0.0"
    @Published var cables: [CableStatus] = CableStatus.offlineDefaults()
    @Published var selectedCable = 0 {
        didSet {
            isDirty = false
            syncEditorFromDriver()
        }
    }

    // Editable controls for the currently selected cable
    @Published var editEnabled = true
    @Published var editSampleRate: Double = 48000
    @Published var editBufferMs: Int = 500
    @Published var editOutputVolume: Double = 1.0
    @Published var editInputVolume: Double = 1.0
    @Published var editOutputMute = false
    @Published var editInputMute = false
    @Published var isDirty = false
    @Published var feedbackMessage: String? = nil

    private let client = DFOVCDriverClient()
    private var timer: Timer?

    init() {
        refresh()
        timer = Timer.scheduledTimer(withTimeInterval: 0.25, repeats: true) { [weak self] _ in
            Task { @MainActor in
                self?.refresh()
            }
        }
    }

    deinit {
        timer?.invalidate()
    }

    var currentCable: CableStatus {
        cables[min(max(selectedCable, 0), cables.count - 1)]
    }

    func refresh() {
        let res = client.fetchAllStatus()
        isConnected = res.connected
        driverVersion = res.version
        cables = res.cables
        if !isDirty {
            syncEditorFromDriver()
        }
    }

    func syncEditorFromDriver() {
        let c = currentCable
        editEnabled = c.isEnabled
        editSampleRate = c.sampleRate
        editBufferMs = c.bufferDurationMs
        editOutputVolume = c.outputVolume
        editInputVolume = c.inputVolume
        editOutputMute = c.outputMute
        editInputMute = c.inputMute
    }

    func applyCurrentSettings() {
        guard isConnected else {
            feedbackMessage = "Driver is not loaded. Run macos/scripts/install.sh first."
            return
        }
        let ok = client.applyConfig(
            cableIndex: selectedCable,
            sampleRate: editSampleRate,
            bufferMs: editBufferMs,
            enabled: editEnabled,
            outputVolume: editOutputVolume,
            inputVolume: editInputVolume,
            outputMute: editOutputMute,
            inputMute: editInputMute
        )
        if ok {
            isDirty = false
            feedbackMessage = "Applied settings to \(currentCable.name) (\(Int(editSampleRate)) Hz, \(editBufferMs) ms)."
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.15) { [weak self] in
                self?.refresh()
            }
        } else {
            feedbackMessage = "Failed to apply settings to CoreAudio driver."
        }
    }

    func restoreDefault() {
        editEnabled = true
        editSampleRate = 48000
        editBufferMs = 500
        editOutputVolume = 1.0
        editInputVolume = 1.0
        editOutputMute = false
        editInputMute = false
        isDirty = true
        feedbackMessage = "Defaults loaded (48000 Hz / 500 ms). Click Apply Settings to save."
    }

    func resetStats() {
        guard isConnected else { return }
        if client.resetStats(cableIndex: selectedCable) {
            feedbackMessage = "Glitch counters reset for \(currentCable.name)."
            refresh()
        }
    }
}

// ============================================================================
// SwiftUI Views
// ============================================================================

struct LevelMeterBar: View {
    let label: String
    let level: Double
    let active: Bool

    var body: some View {
        HStack(spacing: 8) {
            Text(label)
                .font(.caption)
                .frame(width: 68, alignment: .leading)
                .foregroundColor(.secondary)
            GeometryReader { geo in
                ZStack(alignment: .leading) {
                    RoundedRectangle(cornerRadius: 4)
                        .fill(Color.secondary.opacity(0.18))
                    RoundedRectangle(cornerRadius: 4)
                        .fill(level > 0.9 ? Color.red : (level > 0.65 ? Color.orange : Color.green))
                        .frame(width: max(0, min(1, level)) * geo.size.width)
                        .animation(.linear(duration: 0.1), value: level)
                }
            }
            .frame(height: 10)
            Text(active ? String(format: "%.0f%%", min(100, level * 100)) : "Idle")
                .font(.caption.monospacedDigit())
                .frame(width: 44, alignment: .trailing)
                .foregroundColor(active ? .primary : .secondary)
        }
    }
}

struct ContentView: View {
    @StateObject private var vm = ControlPanelViewModel()

    var body: some View {
        VStack(alignment: .leading, spacing: 14) {
            // Header
            HStack {
                VStack(alignment: .leading, spacing: 2) {
                    Text("DF-OpenVCable Control Panel")
                        .font(.title2.bold())
                    Text("5-in-1 Virtual Audio Cable for macOS (CoreAudio HAL)")
                        .font(.caption)
                        .foregroundColor(.secondary)
                }
                Spacer()
                HStack(spacing: 6) {
                    Circle()
                        .fill(vm.isConnected ? Color.green : Color.orange)
                        .frame(width: 9, height: 9)
                    Text(vm.isConnected ? "Driver Live (v\(vm.driverVersion))" : "Driver Not Loaded")
                        .font(.caption.bold())
                        .foregroundColor(vm.isConnected ? .green : .orange)
                }
                .padding(.horizontal, 10)
                .padding(.vertical, 5)
                .background(Color.secondary.opacity(0.12))
                .cornerRadius(8)
            }

            // 5-Cable Selector
            Picker("Cable", selection: $vm.selectedCable) {
                ForEach(0..<5, id: \.self) { idx in
                    Text(ControlPanelViewModel.tabLabels[idx]).tag(idx)
                }
            }
            .pickerStyle(.segmented)
            .labelsHidden()

            // Cable Configuration Box
            GroupBox(label: Label("\(vm.currentCable.name) Configuration", systemImage: "slider.horizontal.3")) {
                VStack(alignment: .leading, spacing: 10) {
                    HStack {
                        Toggle(vm.editEnabled ? "Cable Enabled (Active)" : "Cable Disabled (Muted)",
                               isOn: Binding(
                                get: { vm.editEnabled },
                                set: { vm.editEnabled = $0; vm.isDirty = true }
                               ))
                        .toggleStyle(.switch)
                        Spacer()
                        Button("Reset Defaults") { vm.restoreDefault() }
                            .controlSize(.small)
                    }

                    HStack(spacing: 20) {
                        HStack {
                            Text("Sample Rate:")
                                .frame(width: 95, alignment: .leading)
                            Picker("", selection: Binding(
                                get: { vm.editSampleRate },
                                set: { vm.editSampleRate = $0; vm.isDirty = true }
                            )) {
                                ForEach(ControlPanelViewModel.sampleRates, id: \.self) { sr in
                                    Text("\(Int(sr)) Hz").tag(sr)
                                }
                            }
                            .labelsHidden()
                            .frame(width: 130)
                        }

                        HStack {
                            Text("Buffer Duration:")
                                .frame(width: 105, alignment: .leading)
                            Picker("", selection: Binding(
                                get: { vm.editBufferMs },
                                set: { vm.editBufferMs = $0; vm.isDirty = true }
                            )) {
                                ForEach(ControlPanelViewModel.bufferDurations, id: \.self) { ms in
                                    Text("\(ms) ms").tag(ms)
                                }
                            }
                            .labelsHidden()
                            .frame(width: 120)
                        }
                    }

                    Divider()

                    // Volume & Mute Controls
                    HStack(spacing: 16) {
                        VStack(alignment: .leading, spacing: 4) {
                            HStack {
                                Text("Output Gain: \(Int(vm.editOutputVolume * 100))%")
                                    .font(.caption)
                                Spacer()
                                Toggle("Mute", isOn: Binding(
                                    get: { vm.editOutputMute },
                                    set: { vm.editOutputMute = $0; vm.isDirty = true }
                                ))
                                .controlSize(.small)
                            }
                            Slider(value: Binding(
                                get: { vm.editOutputVolume },
                                set: { vm.editOutputVolume = $0; vm.isDirty = true }
                            ), in: 0...1)
                        }

                        VStack(alignment: .leading, spacing: 4) {
                            HStack {
                                Text("Input Gain: \(Int(vm.editInputVolume * 100))%")
                                    .font(.caption)
                                Spacer()
                                Toggle("Mute", isOn: Binding(
                                    get: { vm.editInputMute },
                                    set: { vm.editInputMute = $0; vm.isDirty = true }
                                ))
                                .controlSize(.small)
                            }
                            Slider(value: Binding(
                                get: { vm.editInputVolume },
                                set: { vm.editInputVolume = $0; vm.isDirty = true }
                            ), in: 0...1)
                        }
                    }
                }
                .padding(8)
            }

            // Live Diagnostics Box
            GroupBox(label: Label("Live Diagnostics & Metrics", systemImage: "waveform.path.ecg")) {
                let c = vm.currentCable
                let fillPct = c.bufferSizeBytes > 0
                    ? (Double(c.currentBufferFill) / Double(c.bufferSizeBytes)) * 100.0
                    : 0.0

                VStack(alignment: .leading, spacing: 8) {
                    HStack {
                        Text("State: \(c.isEnabled ? "Active" : "Disabled")")
                        Text("|").foregroundColor(.secondary)
                        Text("Format: \(Int(c.sampleRate)) Hz / \(c.bitsPerSample)-bit Float / \(c.channels)ch")
                        Text("|").foregroundColor(.secondary)
                        Text("Ring: \(c.bufferDurationMs) ms (\(c.bufferSizeBytes / 1024) KB)")
                    }
                    .font(.callout.monospacedDigit())

                    HStack {
                        Text("Buffer Queue:")
                            .font(.caption)
                            .foregroundColor(.secondary)
                            .frame(width: 90, alignment: .leading)
                        ProgressView(value: min(100, max(0, fillPct)), total: 100)
                        Text(String(format: "%d / %d B (%.1f%%)", c.currentBufferFill, c.bufferSizeBytes, fillPct))
                            .font(.caption.monospacedDigit())
                            .frame(width: 170, alignment: .trailing)
                    }

                    LevelMeterBar(label: "Playback:", level: c.outputPeak, active: c.isRenderActive)
                    LevelMeterBar(label: "Capture:", level: c.inputPeak, active: c.isCaptureActive)

                    HStack {
                        Label("Playback: \(c.isRenderActive ? "Streaming" : "Idle")",
                              systemImage: c.isRenderActive ? "speaker.wave.2.fill" : "speaker.fill")
                            .foregroundColor(c.isRenderActive ? .green : .secondary)
                        Spacer()
                        Label("Capture: \(c.isCaptureActive ? "Listening" : "Idle")",
                              systemImage: c.isCaptureActive ? "mic.fill" : "mic.slash")
                            .foregroundColor(c.isCaptureActive ? .green : .secondary)
                        Spacer()
                        Text("Overruns: \(c.overrunCount)  |  Underruns: \(c.underrunCount)")
                            .font(.callout.monospacedDigit())
                            .foregroundColor((c.overrunCount > 0 || c.underrunCount > 0) ? .orange : .primary)
                    }
                    .font(.callout)
                }
                .padding(8)
            }

            // Footer Actions
            HStack {
                if let msg = vm.feedbackMessage {
                    Text(msg)
                        .font(.caption)
                        .foregroundColor(.secondary)
                        .lineLimit(1)
                } else if vm.isDirty {
                    Text("Unsaved changes — click Apply Settings.")
                        .font(.caption)
                        .foregroundColor(.orange)
                } else {
                    Text("UID: \(vm.currentCable.uid)")
                        .font(.caption.monospaced())
                        .foregroundColor(.secondary)
                }
                Spacer()
                Button("Reset Stats") { vm.resetStats() }
                    .disabled(!vm.isConnected)
                Button("Apply Settings") { vm.applyCurrentSettings() }
                    .keyboardShortcut(.defaultAction)
                    .disabled(!vm.isConnected)
            }
        }
        .padding(16)
        .frame(width: 660, height: 485)
    }
}

// ============================================================================
// App Entry Point
// ============================================================================

@main
struct DFOpenVCableControlPanelApp: App {
    init() {
        NSApplication.shared.setActivationPolicy(.regular)
        NSApplication.shared.activate(ignoringOtherApps: true)
    }

    var body: some Scene {
        WindowGroup("DF-OpenVCable Control Panel") {
            ContentView()
        }
        .windowResizability(.contentSize)
    }
}
