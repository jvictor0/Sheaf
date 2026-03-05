import AVFoundation
import Foundation

final class AudioSnippetRecorder {
    private let engine = AVAudioEngine()
    private let targetSampleRate: Double = 16_000
    private let lock = NSLock()
    private var pcmData = Data()
    private var isRecording = false

    func start() throws {
        guard !isRecording else { return }

        let audioSession = AVAudioSession.sharedInstance()
        try audioSession.setCategory(.record, mode: .measurement, options: [.duckOthers])
        try audioSession.setActive(true, options: [])

        let inputNode = engine.inputNode
        let sourceFormat = inputNode.outputFormat(forBus: 0)
        guard let destinationFormat = AVAudioFormat(
            commonFormat: .pcmFormatInt16,
            sampleRate: targetSampleRate,
            channels: 1,
            interleaved: true
        ) else {
            throw NSError(
                domain: "SheafClient.Dictation",
                code: 100,
                userInfo: [NSLocalizedDescriptionKey: "Failed to initialize audio format."]
            )
        }

        guard let converter = AVAudioConverter(from: sourceFormat, to: destinationFormat) else {
            throw NSError(
                domain: "SheafClient.Dictation",
                code: 101,
                userInfo: [NSLocalizedDescriptionKey: "Failed to initialize audio converter."]
            )
        }

        lock.lock()
        pcmData.removeAll(keepingCapacity: true)
        lock.unlock()

        inputNode.removeTap(onBus: 0)
        inputNode.installTap(onBus: 0, bufferSize: 2048, format: sourceFormat) { [weak self] buffer, _ in
            self?.capture(buffer: buffer, converter: converter, destinationFormat: destinationFormat)
        }

        engine.prepare()
        try engine.start()
        isRecording = true
    }

    func stopAndBuildWAV() throws -> Data {
        guard isRecording else {
            throw NSError(
                domain: "SheafClient.Dictation",
                code: 102,
                userInfo: [NSLocalizedDescriptionKey: "Recorder is not running."]
            )
        }

        engine.inputNode.removeTap(onBus: 0)
        engine.stop()
        try? AVAudioSession.sharedInstance().setActive(false, options: [.notifyOthersOnDeactivation])
        isRecording = false

        lock.lock()
        let pcm = pcmData
        lock.unlock()
        guard !pcm.isEmpty else {
            throw NSError(
                domain: "SheafClient.Dictation",
                code: 103,
                userInfo: [NSLocalizedDescriptionKey: "No audio captured."]
            )
        }
        return Self.makeWAV(pcm16MonoData: pcm, sampleRate: Int(targetSampleRate))
    }

    private func capture(buffer: AVAudioPCMBuffer, converter: AVAudioConverter, destinationFormat: AVAudioFormat) {
        let expectedFrames = AVAudioFrameCount(
            (Double(buffer.frameLength) * targetSampleRate / buffer.format.sampleRate).rounded(.up)
        )
        guard let converted = AVAudioPCMBuffer(
            pcmFormat: destinationFormat,
            frameCapacity: max(expectedFrames, 1)
        ) else {
            return
        }

        var consumed = false
        var conversionError: NSError?
        let status = converter.convert(to: converted, error: &conversionError) { _, outStatus in
            if consumed {
                outStatus.pointee = .noDataNow
                return nil
            }
            consumed = true
            outStatus.pointee = .haveData
            return buffer
        }

        guard conversionError == nil, status == .haveData else { return }
        guard let channelData = converted.int16ChannelData else { return }

        let frameLength = Int(converted.frameLength)
        let byteCount = frameLength * MemoryLayout<Int16>.size
        let pointer = UnsafeRawPointer(channelData.pointee)
        lock.lock()
        pcmData.append(pointer.assumingMemoryBound(to: UInt8.self), count: byteCount)
        lock.unlock()
    }

    private static func makeWAV(pcm16MonoData: Data, sampleRate: Int) -> Data {
        let channels: UInt16 = 1
        let bitsPerSample: UInt16 = 16
        let blockAlign = UInt16(channels * (bitsPerSample / 8))
        let byteRate = UInt32(sampleRate) * UInt32(blockAlign)
        let dataSize = UInt32(pcm16MonoData.count)
        let riffSize = UInt32(36) + dataSize

        var data = Data(capacity: Int(riffSize) + 8)
        data.append(contentsOf: [0x52, 0x49, 0x46, 0x46]) // RIFF
        data.append(contentsOf: riffSize.littleEndianBytes)
        data.append(contentsOf: [0x57, 0x41, 0x56, 0x45]) // WAVE
        data.append(contentsOf: [0x66, 0x6D, 0x74, 0x20]) // fmt
        data.append(contentsOf: UInt32(16).littleEndianBytes)
        data.append(contentsOf: UInt16(1).littleEndianBytes) // PCM
        data.append(contentsOf: channels.littleEndianBytes)
        data.append(contentsOf: UInt32(sampleRate).littleEndianBytes)
        data.append(contentsOf: byteRate.littleEndianBytes)
        data.append(contentsOf: blockAlign.littleEndianBytes)
        data.append(contentsOf: bitsPerSample.littleEndianBytes)
        data.append(contentsOf: [0x64, 0x61, 0x74, 0x61]) // data
        data.append(contentsOf: dataSize.littleEndianBytes)
        data.append(pcm16MonoData)
        return data
    }
}

private extension FixedWidthInteger {
    var littleEndianBytes: [UInt8] {
        withUnsafeBytes(of: littleEndian, Array.init)
    }
}
