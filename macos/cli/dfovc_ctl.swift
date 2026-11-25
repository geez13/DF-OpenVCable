import CoreAudio
import Foundation

// ============================================================================
// dfovc-ctl: macOS CLI Controller & Live Audio Loopback Tester for DF-OpenVCable
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

private let kCableNames = [
    "DF-OpenVCable A",
    "DF-OpenVCable B",
    "DF-OpenVCable C",
    "DF-OpenVCable D",
    "DF-OpenVCable E",
]

func parseCableIndex(_ arg: String) -> Int? {
    switch arg.uppercased() {
    case "A", "0": return 0
    case "B", "1": return 1
    case "C", "2": return 2
    case "D", "3": return 3
    case "E", "4": return 4
    default: return nil
    }
}

func findPlugInID() -> AudioObjectID? {
    var addr = AudioObjectPropertyAddress(
        mSelector: kAudioHardwarePropertyTranslateBundleIDToPlugIn,
        mScope: kAudioObjectPropertyScopeGlobal,
        mElement: kAudioObjectPropertyElementMain
    )
    var cfBundle: CFString = kBundleID as CFString
    var plugInID: AudioObjectID = kAudioObjectUnknown
    var size = UInt32(MemoryLayout<AudioObjectID>.size)
    let err = withUnsafePointer(to: &cfBundle) { ptr in
        AudioObjectGetPropertyData(
            AudioObjectID(kAudioObjectSystemObject),
            &addr,
            UInt32(MemoryLayout<CFString>.size),
            ptr,
            &size,
            &plugInID
        )
    }
    return (err == noErr && plugInID != kAudioObjectUnknown) ? plugInID : nil
}

func findDeviceID(uid: String) -> AudioObjectID? {
    var addr = AudioObjectPropertyAddress(
        mSelector: kAudioHardwarePropertyTranslateUIDToDevice,
        mScope: kAudioObjectPropertyScopeGlobal,
        mElement: kAudioObjectPropertyElementMain
    )
    var cfUID: CFString = uid as CFString
    var devID: AudioObjectID = kAudioObjectUnknown
    var size = UInt32(MemoryLayout<AudioObjectID>.size)
    let err = withUnsafePointer(to: &cfUID) { ptr in
        AudioObjectGetPropertyData(
            AudioObjectID(kAudioObjectSystemObject),
            &addr,
            UInt32(MemoryLayout<CFString>.size),
            ptr,
            &size,
            &devID
        )
    }
    return (err == noErr && devID != kAudioObjectUnknown) ? devID : nil
}

func getPlist(objectID: AudioObjectID, selector: AudioObjectPropertySelector) -> Any? {
    var addr = AudioObjectPropertyAddress(
        mSelector: selector,
        mScope: kAudioObjectPropertyScopeGlobal,
        mElement: kAudioObjectPropertyElementMain
    )
    var size = UInt32(MemoryLayout<Unmanaged<CFPropertyList>?>.size)
    var raw: Unmanaged<CFPropertyList>? = nil
    let err = AudioObjectGetPropertyData(objectID, &addr, 0, nil, &size, &raw)
    guard err == noErr, let u = raw else { return nil }
    return u.takeRetainedValue()
}

func setPlist(objectID: AudioObjectID, selector: AudioObjectPropertySelector, dict: [String: Any]) -> OSStatus {
    var addr = AudioObjectPropertyAddress(
        mSelector: selector,
        mScope: kAudioObjectPropertyScopeGlobal,
        mElement: kAudioObjectPropertyElementMain
    )
    let cfDict = dict as CFDictionary
    var rawPtr: UnsafeRawPointer? = UnsafeRawPointer(Unmanaged.passUnretained(cfDict).toOpaque())
    let size = UInt32(MemoryLayout<UnsafeRawPointer?>.size)
    return withExtendedLifetime(cfDict) {
        withUnsafePointer(to: &rawPtr) { ptr in
            AudioObjectSetPropertyData(objectID, &addr, 0, nil, size, ptr)
        }
    }
}

func getStringProp(objectID: AudioObjectID, selector: AudioObjectPropertySelector) -> String? {
    var addr = AudioObjectPropertyAddress(
        mSelector: selector,
        mScope: kAudioObjectPropertyScopeGlobal,
        mElement: kAudioObjectPropertyElementMain
    )
    var size = UInt32(MemoryLayout<Unmanaged<CFString>?>.size)
    var raw: Unmanaged<CFString>? = nil
    let err = AudioObjectGetPropertyData(objectID, &addr, 0, nil, &size, &raw)
    guard err == noErr, let u = raw else { return nil }
    return u.takeRetainedValue() as String
}

func cmdList() -> Int32 {
    var addr = AudioObjectPropertyAddress(
        mSelector: kAudioHardwarePropertyDevices,
        mScope: kAudioObjectPropertyScopeGlobal,
        mElement: kAudioObjectPropertyElementMain
    )
    var size: UInt32 = 0
    guard AudioObjectGetPropertyDataSize(AudioObjectID(kAudioObjectSystemObject), &addr, 0, nil, &size) == noErr else {
        fputs("Error: could not query CoreAudio device list.\n", stderr)
        return 1
    }
    let count = Int(size) / MemoryLayout<AudioObjectID>.size
    var devices = [AudioObjectID](repeating: 0, count: count)
    guard AudioObjectGetPropertyData(AudioObjectID(kAudioObjectSystemObject), &addr, 0, nil, &size, &devices) == noErr else {
        fputs("Error: could not read CoreAudio device list.\n", stderr)
        return 1
    }

    var dfCount = 0
    print("CoreAudio Devices (\(count) total):")
    for id in devices {
        let name = getStringProp(objectID: id, selector: kAudioObjectPropertyName) ?? "<unknown>"
        let uid = getStringProp(objectID: id, selector: kAudioDevicePropertyDeviceUID) ?? ""
        let isDF = uid.hasPrefix("com.dfopenvcable.device.")
        if isDF { dfCount += 1 }
        let tag = isDF ? "[DF-OpenVCable]" : "               "
        print(String(format: "  %4u  %@  %-28@  (%@)", id, tag, name as NSString, uid as NSString))
    }
    print("\nDF-OpenVCable devices detected: \(dfCount) / 5")
    return 0
}

func cmdStatus(jsonMode: Bool) -> Int32 {
    var statusDict: [String: Any]? = nil
    if let plugInID = findPlugInID(),
       let d = getPlist(objectID: plugInID, selector: kPropAllStatus) as? [String: Any] {
        statusDict = d
    } else {
        var cables: [[String: Any]] = []
        for uid in kCableUIDs {
            if let devID = findDeviceID(uid: uid),
               let d = getPlist(objectID: devID, selector: kPropCableStatus) as? [String: Any] {
                cables.append(d)
            }
        }
        if !cables.isEmpty {
            statusDict = ["version": "1.0.0", "cableCount": cables.count, "cables": cables]
        }
    }

    guard let status = statusDict else {
        fputs("Error: DF-OpenVCable.driver is not loaded in CoreAudio.\n", stderr)
        return 1
    }

    if jsonMode {
        if let data = try? JSONSerialization.data(withJSONObject: status, options: [.prettyPrinted, .sortedKeys]),
           let str = String(data: data, encoding: .utf8) {
            print(str)
            return 0
        }
        return 1
    }

    let ver = status["version"] as? String ?? "1.0.0"
    print("DF-OpenVCable macOS Driver v\(ver)")
    print(String(repeating: "-", count: 92))
    print("Cable              State     Rate      Buffer    Fill        Over  Under  Render  Capture")
    print(String(repeating: "-", count: 92))
    if let cables = status["cables"] as? [[String: Any]] {
        for c in cables {
            let name = c["name"] as? String ?? "?"
            let en = (c["isEnabled"] as? NSNumber)?.boolValue ?? false
            let sr = (c["sampleRate"] as? NSNumber)?.intValue ?? 0
            let bufMs = (c["bufferDurationMs"] as? NSNumber)?.intValue ?? 0
            let fill = (c["currentBufferFill"] as? NSNumber)?.intValue ?? 0
            let cap = (c["bufferSizeBytes"] as? NSNumber)?.intValue ?? 1
            let ov = (c["overrunCount"] as? NSNumber)?.intValue ?? 0
            let un = (c["underrunCount"] as? NSNumber)?.intValue ?? 0
            let rAct = (c["isRenderActive"] as? NSNumber)?.boolValue ?? false
            let cAct = (c["isCaptureActive"] as? NSNumber)?.boolValue ?? false
            let pct = cap > 0 ? (Double(fill) / Double(cap)) * 100.0 : 0.0

            print(String(
                format: "%-18@ %-8@ %6dHz  %4dms    %5.1f%%      %4d  %5d  %-7@ %-7@",
                name as NSString,
                (en ? "Active" : "Muted") as NSString,
                sr,
                bufMs,
                pct,
                ov,
                un,
                (rAct ? "ACTIVE" : "Idle") as NSString,
                (cAct ? "ACTIVE" : "Idle") as NSString
            ))
        }
    }
    return 0
}

func cmdConfig(args: [String]) -> Int32 {
    guard args.count >= 1, let idx = parseCableIndex(args[0]) else {
        fputs("Usage: dfovc-ctl config <A|B|C|D|E> [--rate HZ] [--buffer MS] [--enable|--disable] [--out-vol 0..1] [--in-vol 0..1] [--out-mute on|off] [--in-mute on|off]\n", stderr)
        return 2
    }

    var payload: [String: Any] = ["cableIndex": idx]
    var i = 1
    while i < args.count {
        let flag = args[i]
        switch flag {
        case "--rate":
            i += 1
            guard i < args.count, let sr = Double(args[i]) else { fputs("Invalid --rate\n", stderr); return 2 }
            payload["sampleRate"] = sr
        case "--buffer":
            i += 1
            guard i < args.count, let ms = Int(args[i]) else { fputs("Invalid --buffer\n", stderr); return 2 }
            payload["bufferDurationMs"] = ms
        case "--enable":
            payload["isEnabled"] = true
        case "--disable":
            payload["isEnabled"] = false
        case "--out-vol":
            i += 1
            guard i < args.count, let v = Double(args[i]) else { fputs("Invalid --out-vol\n", stderr); return 2 }
            payload["outputVolume"] = v
        case "--in-vol":
            i += 1
            guard i < args.count, let v = Double(args[i]) else { fputs("Invalid --in-vol\n", stderr); return 2 }
            payload["inputVolume"] = v
        case "--out-mute":
            i += 1
            guard i < args.count else { fputs("Invalid --out-mute\n", stderr); return 2 }
            payload["outputMute"] = (args[i].lowercased() == "on" || args[i] == "1" || args[i].lowercased() == "true")
        case "--in-mute":
            i += 1
            guard i < args.count else { fputs("Invalid --in-mute\n", stderr); return 2 }
            payload["inputMute"] = (args[i].lowercased() == "on" || args[i] == "1" || args[i].lowercased() == "true")
        default:
            fputs("Unknown option: \(flag)\n", stderr)
            return 2
        }
        i += 1
    }

    let err: OSStatus
    if let plugInID = findPlugInID() {
        err = setPlist(objectID: plugInID, selector: kPropCableConfig, dict: payload)
    } else if let devID = findDeviceID(uid: kCableUIDs[idx]) {
        err = setPlist(objectID: devID, selector: kPropCableConfig, dict: payload)
    } else {
        fputs("Error: DF-OpenVCable.driver is not loaded.\n", stderr)
        return 1
    }

    if err != noErr {
        fputs("Error: Failed to apply config (OSStatus \(err)).\n", stderr)
        return 1
    }
    print("Updated \(kCableNames[idx]) configuration successfully.")
    return 0
}

func cmdResetStats(args: [String]) -> Int32 {
    var payload: [String: Any] = [:]
    if let first = args.first, first.lowercased() != "all" {
        guard let idx = parseCableIndex(first) else {
            fputs("Usage: dfovc-ctl reset-stats [A|B|C|D|E|all]\n", stderr)
            return 2
        }
        payload["cableIndex"] = idx
    }

    guard let plugInID = findPlugInID() else {
        fputs("Error: DF-OpenVCable.driver is not loaded.\n", stderr)
        return 1
    }
    let err = setPlist(objectID: plugInID, selector: kPropResetStats, dict: payload)
    if err != noErr {
        fputs("Error: Failed to reset stats (OSStatus \(err)).\n", stderr)
        return 1
    }
    print("Glitch counters reset.")
    return 0
}

// ============================================================================
// Live 1 kHz CoreAudio Loopback Verification (mirrors tests/audio_loopback.ps1)
// ============================================================================

final class LoopbackTestContext {
    let sampleRate: Double
    let targetFreq: Double = 1000.0
    var phase: Double = 0.0
    var captured: [Float] = []
    let maxSamples: Int
    let lock = NSLock()

    init(sampleRate: Double, durationSeconds: Double) {
        self.sampleRate = sampleRate
        self.maxSamples = Int(sampleRate * durationSeconds)
        self.captured.reserveCapacity(self.maxSamples)
    }
}

private func goertzelMagnitude(_ samples: [Float], sampleRate: Double, freq: Double) -> Double {
    let n = samples.count
    guard n > 0 else { return 0.0 }
    let k = round((Double(n) * freq) / sampleRate)
    let omega = (2.0 * Double.pi * k) / Double(n)
    let coeff = 2.0 * cos(omega)
    var s0 = 0.0, s1 = 0.0, s2 = 0.0
    for x in samples {
        s0 = Double(x) + coeff * s1 - s2
        s2 = s1
        s1 = s0
    }
    let power = max(0.0, s1 * s1 + s2 * s2 - coeff * s1 * s2)
    return (2.0 * sqrt(power)) / Double(n)
}

func cmdLoopbackTest(args: [String]) -> Int32 {
    let cableArg = args.first ?? "A"
    guard let idx = parseCableIndex(cableArg) else {
        fputs("Usage: dfovc-ctl loopback-test [A|B|C|D|E]\n", stderr)
        return 2
    }
    guard let devID = findDeviceID(uid: kCableUIDs[idx]) else {
        fputs("FAIL: Device \(kCableNames[idx]) (\(kCableUIDs[idx])) not found in CoreAudio.\n", stderr)
        return 1
    }

    // Read device nominal sample rate
    var srAddr = AudioObjectPropertyAddress(
        mSelector: kAudioDevicePropertyNominalSampleRate,
        mScope: kAudioObjectPropertyScopeGlobal,
        mElement: kAudioObjectPropertyElementMain
    )
    var sampleRate: Float64 = 48000.0
    var srSize = UInt32(MemoryLayout<Float64>.size)
    AudioObjectGetPropertyData(devID, &srAddr, 0, nil, &srSize, &sampleRate)

    print("Running 1 kHz loopback test on \(kCableNames[idx]) (deviceID=\(devID), rate=\(Int(sampleRate)) Hz)...")

    let ctx = LoopbackTestContext(sampleRate: sampleRate, durationSeconds: 1.5)
    let ctxPtr = Unmanaged.passRetained(ctx).toOpaque()

    let ioProc: AudioDeviceIOProc = { _, _, inInputData, _, outOutputData, _, inClientData in
        guard let rawPtr = inClientData else { return noErr }
        let context = Unmanaged<LoopbackTestContext>.fromOpaque(rawPtr).takeUnretainedValue()

        // 1. Capture looped-back input audio (channel 0)
        let inList = UnsafeMutableAudioBufferListPointer(UnsafeMutablePointer(mutating: inInputData))
        if let firstIn = inList.first, let dataPtr = firstIn.mData {
            let channels = max(1, Int(firstIn.mNumberChannels))
            let totalFloats = Int(firstIn.mDataByteSize) / MemoryLayout<Float>.size
            let frames = totalFloats / channels
            let floats = dataPtr.assumingMemoryBound(to: Float.self)

            context.lock.lock()
            let remaining = context.maxSamples - context.captured.count
            let toTake = min(frames, max(0, remaining))
            for f in 0..<toTake {
                context.captured.append(floats[f * channels])
            }
            context.lock.unlock()
        }

        // 2. Generate 1 kHz sine wave on output
        let outList = UnsafeMutableAudioBufferListPointer(outOutputData)
        if let firstOut = outList.first, let dataPtr = firstOut.mData {
            let channels = max(1, Int(firstOut.mNumberChannels))
            let totalFloats = Int(firstOut.mDataByteSize) / MemoryLayout<Float>.size
            let frames = totalFloats / channels
            let floats = dataPtr.assumingMemoryBound(to: Float.self)
            let phaseStep = 2.0 * Double.pi * context.targetFreq / context.sampleRate

            for f in 0..<frames {
                let sample = Float(0.5 * sin(context.phase))
                context.phase += phaseStep
                if context.phase >= 2.0 * Double.pi {
                    context.phase -= 2.0 * Double.pi
                }
                for ch in 0..<channels {
                    floats[f * channels + ch] = sample
                }
            }
        }

        return noErr
    }

    var procID: AudioDeviceIOProcID? = nil
    var err = AudioDeviceCreateIOProcID(devID, ioProc, ctxPtr, &procID)
    guard err == noErr, let validProcID = procID else {
        Unmanaged<LoopbackTestContext>.fromOpaque(ctxPtr).release()
        fputs("FAIL: AudioDeviceCreateIOProcID failed (OSStatus \(err)).\n", stderr)
        return 1
    }

    err = AudioDeviceStart(devID, validProcID)
    guard err == noErr else {
        AudioDeviceDestroyIOProcID(devID, validProcID)
        Unmanaged<LoopbackTestContext>.fromOpaque(ctxPtr).release()
        fputs("FAIL: AudioDeviceStart failed (OSStatus \(err)).\n", stderr)
        return 1
    }

    // Stream for 1.5 seconds
    Thread.sleep(forTimeInterval: 1.5)

    AudioDeviceStop(devID, validProcID)
    AudioDeviceDestroyIOProcID(devID, validProcID)

    ctx.lock.lock()
    let samples = ctx.captured
    ctx.lock.unlock()
    Unmanaged<LoopbackTestContext>.fromOpaque(ctxPtr).release()

    // Skip initial 200ms startup preroll window for steady-state analysis
    let skip = min(samples.count, Int(sampleRate * 0.2))
    let steady = Array(samples.dropFirst(skip))
    guard steady.count > 1024 else {
        fputs("FAIL: Captured too few samples (\(steady.count)).\n", stderr)
        return 1
    }

    var sumSq = 0.0
    for s in steady { sumSq += Double(s) * Double(s) }
    let rms = sqrt(sumSq / Double(steady.count))
    let mag1k = goertzelMagnitude(steady, sampleRate: sampleRate, freq: 1000.0)
    let mag500 = goertzelMagnitude(steady, sampleRate: sampleRate, freq: 500.0)
    let ratio = mag500 > 1e-9 ? (mag1k / mag500) : (mag1k > 0.05 ? 9999.0 : 0.0)

    print(String(format: "  Captured samples : %d", steady.count))
    print(String(format: "  RMS level        : %.4f", rms))
    print(String(format: "  1000 Hz Goertzel : %.4f", mag1k))
    print(String(format: "   500 Hz Goertzel : %.4f", mag500))
    print(String(format: "  Tone/Noise Ratio : %.1fx", ratio))

    if rms > 0.05 && mag1k > 0.1 && ratio > 10.0 {
        print("PASS: \(kCableNames[idx]) loopback verified cleanly!")
        return 0
    } else {
        fputs("FAIL: Loopback audio signal too weak or distorted (check cable is enabled and unmuted).\n", stderr)
        return 1
    }
}

func printUsage() {
    print("""
    dfovc-ctl - DF-OpenVCable macOS Command-Line Controller

    Usage:
      dfovc-ctl list
      dfovc-ctl status [--json]
      dfovc-ctl config <A|B|C|D|E> [--rate HZ] [--buffer MS] [--enable|--disable]
                                   [--out-vol 0..1] [--in-vol 0..1]
                                   [--out-mute on|off] [--in-mute on|off]
      dfovc-ctl reset-stats [A|B|C|D|E|all]
      dfovc-ctl loopback-test [A|B|C|D|E]
    """)
}

let args = Array(CommandLine.arguments.dropFirst())
guard let cmd = args.first else {
    printUsage()
    exit(0)
}

switch cmd {
case "list":
    exit(cmdList())
case "status":
    exit(cmdStatus(jsonMode: args.contains("--json")))
case "config":
    exit(cmdConfig(args: Array(args.dropFirst())))
case "reset-stats":
    exit(cmdResetStats(args: Array(args.dropFirst())))
case "loopback-test":
    exit(cmdLoopbackTest(args: Array(args.dropFirst())))
case "-h", "--help", "help":
    printUsage()
    exit(0)
default:
    fputs("Unknown command: \(cmd)\n", stderr)
    printUsage()
    exit(2)
}
