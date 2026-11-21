import UIKit
import SwiftUI
import AVFoundation
import CoreMedia

// MARK: - Видео: аппаратный декодер через AVSampleBufferDisplayLayer, показ сразу (DisplayImmediately)
final class VideoView: UIView {
    override class var layerClass: AnyClass { AVSampleBufferDisplayLayer.self }
    private var dl: AVSampleBufferDisplayLayer { layer as! AVSampleBufferDisplayLayer }
    private var fmt: CMVideoFormatDescription?
    private var sps: Data?, pps: Data?
    private var counter: Int64 = 0
    var onSize: ((CGSize) -> Void)?
    var onNeedKey: (() -> Void)?

    override init(frame: CGRect) { super.init(frame: frame); dl.videoGravity = .resize; dl.backgroundColor = UIColor.black.cgColor }
    required init?(coder: NSCoder) { fatalError() }

    func push(_ annexB: Data) {
        var avcc = Data()
        var newPS = false
        for nal in splitNALs(annexB) {
            guard let h = nal.first else { continue }
            switch h & 0x1f {
            case 7: if sps != Data(nal) { sps = Data(nal); newPS = true }
            case 8: if pps != Data(nal) { pps = Data(nal); newPS = true }
            case 1, 5:
                var len = UInt32(nal.count).bigEndian
                avcc.append(Data(bytes: &len, count: 4)); avcc.append(nal)
            default: break
            }
        }
        if newPS || fmt == nil, let s = sps, let p = pps { makeFormat(s, p) }
        guard let fmt = fmt, !avcc.isEmpty else { return }
        if dl.status == .failed { dl.flush(); onNeedKey?(); return }

        var bb: CMBlockBuffer?
        guard CMBlockBufferCreateWithMemoryBlock(allocator: nil, memoryBlock: nil, blockLength: avcc.count, blockAllocator: nil,
                customBlockSource: nil, offsetToData: 0, dataLength: avcc.count, flags: 0, blockBufferOut: &bb) == noErr, let bb = bb else { return }
        avcc.withUnsafeBytes { _ = CMBlockBufferReplaceDataBytes(with: $0.baseAddress!, blockBuffer: bb, offsetIntoDestination: 0, dataLength: avcc.count) }
        var timing = CMSampleTimingInfo(duration: .invalid, presentationTimeStamp: CMTime(value: counter, timescale: 1000), decodeTimeStamp: .invalid)
        counter += 1
        var size = avcc.count
        var sb: CMSampleBuffer?
        guard CMSampleBufferCreateReady(allocator: nil, dataBuffer: bb, formatDescription: fmt, sampleCount: 1, sampleTimingEntryCount: 1,
                sampleTimingArray: &timing, sampleSizeEntryCount: 1, sampleSizeArray: &size, sampleBufferOut: &sb) == noErr, let sb = sb else { return }
        if let arr = CMSampleBufferGetSampleAttachmentsArray(sb, createIfNecessary: true) as? [NSMutableDictionary] {
            arr.first?[kCMSampleAttachmentKey_DisplayImmediately] = true
        }
        dl.enqueue(sb)
    }

    private func makeFormat(_ s: Data, _ p: Data) {
        var out: CMVideoFormatDescription?
        s.withUnsafeBytes { sp in p.withUnsafeBytes { pp in
            let ptrs = [sp.bindMemory(to: UInt8.self).baseAddress!, pp.bindMemory(to: UInt8.self).baseAddress!]
            let sizes = [s.count, p.count]
            CMVideoFormatDescriptionCreateFromH264ParameterSets(allocator: nil, parameterSetCount: 2, parameterSetPointers: ptrs,
                parameterSetSizes: sizes, nalUnitHeaderLength: 4, formatDescriptionOut: &out)
        } }
        if let o = out {
            fmt = o; let d = CMVideoFormatDescriptionGetDimensions(o)
            DispatchQueue.main.async { self.onSize?(CGSize(width: Int(d.width), height: Int(d.height))) }
        }
    }

    private func splitNALs(_ d: Data) -> [Data] {
        let b = [UInt8](d); var starts: [(pos: Int, len: Int)] = []; var i = 0
        while i + 3 <= b.count {
            if b[i] == 0, b[i + 1] == 0, b[i + 2] == 1 { starts.append((i, 3)); i += 3 }
            else if i + 4 <= b.count, b[i] == 0, b[i + 1] == 0, b[i + 2] == 0, b[i + 3] == 1 { starts.append((i, 4)); i += 4 }
            else { i += 1 }
        }
        var res: [Data] = []
        for (k, s) in starts.enumerated() {
            let from = s.pos + s.len, to = k + 1 < starts.count ? starts[k + 1].pos : b.count
            if to > from { res.append(Data(b[from..<to])) }
        }
        return res
    }
}

// MARK: - Холст: перо, жесты, масштаб
final class CanvasView: UIView, UIGestureRecognizerDelegate {
    let video = VideoView()
    var link: HostLink?
    var settings: PenSettings = PenSettings()
    private var videoSize = CGSize.zero
    private var scale: CGFloat = 1, tx: CGFloat = 0, ty: CGFloat = 0

    override init(frame: CGRect) {
        super.init(frame: frame)
        backgroundColor = .black; isMultipleTouchEnabled = true
        addSubview(video)
        video.onSize = { [weak self] s in self?.videoSize = s; self?.fit() }
        let pan = UIPanGestureRecognizer(target: self, action: #selector(onPan(_:)))
        pan.minimumNumberOfTouches = 2; pan.maximumNumberOfTouches = 2
        let pinch = UIPinchGestureRecognizer(target: self, action: #selector(onPinch(_:)))
        let undo = UITapGestureRecognizer(target: self, action: #selector(onUndo)); undo.numberOfTouchesRequired = 2
        let redo = UITapGestureRecognizer(target: self, action: #selector(onRedo)); redo.numberOfTouchesRequired = 3
        for g in [pan, pinch, undo, redo] as [UIGestureRecognizer] {
            g.allowedTouchTypes = [NSNumber(value: UITouch.TouchType.direct.rawValue)]   // только пальцы; Pencil идёт в touches*
            g.delegate = self; g.cancelsTouchesInView = false; g.delaysTouchesBegan = false; addGestureRecognizer(g)
        }
        addGestureRecognizer(UIHoverGestureRecognizer(target: self, action: #selector(onHover(_:))))
    }
    required init?(coder: NSCoder) { fatalError() }
    func gestureRecognizer(_ a: UIGestureRecognizer, shouldRecognizeSimultaneouslyWith b: UIGestureRecognizer) -> Bool { true }

    override func layoutSubviews() { super.layoutSubviews(); fit() }
    private func fit() {
        guard videoSize.width > 0 else { return }
        let s = min(bounds.width / videoSize.width, bounds.height / videoSize.height)
        video.bounds = CGRect(origin: .zero, size: CGSize(width: videoSize.width * s, height: videoSize.height * s))
        video.center = CGPoint(x: bounds.midX, y: bounds.midY)
        apply()
    }
    private func apply() { video.transform = CGAffineTransform(translationX: tx, y: ty).scaledBy(x: scale, y: scale) }
    func resetView() { scale = 1; tx = 0; ty = 0; apply() }

    @objc private func onPan(_ g: UIPanGestureRecognizer) { let t = g.translation(in: self); tx += t.x; ty += t.y; g.setTranslation(.zero, in: self); apply() }
    @objc private func onPinch(_ g: UIPinchGestureRecognizer) {
        let k = min(max(scale * g.scale, 0.5), 8) / scale; g.scale = 1
        let c = g.location(in: self); let a = CGPoint(x: c.x - bounds.midX, y: c.y - bounds.midY)
        tx = a.x - k * (a.x - tx); ty = a.y - k * (a.y - ty); scale *= k; apply()
    }
    @objc private func onUndo() { link?.sendKey(action: 0, mods: 1, vk: 0x5A) }
    @objc private func onRedo() { link?.sendKey(action: 0, mods: 3, vk: 0x5A) }

    // MARK: Pencil
    private func norm(_ p: CGPoint) -> (Float, Float) {
        let w = max(video.bounds.width, 1), h = max(video.bounds.height, 1)
        return (Float(min(max(p.x / w, 0), 1)), Float(min(max(p.y / h, 0), 1)))
    }
    private func sendTouch(_ t: UITouch, _ phase: UInt8) {
        let (x, y) = norm(t.preciseLocation(in: video))                     // координаты с учётом зума/сдвига
        let raw = t.maximumPossibleForce > 0 ? Float(t.force / t.maximumPossibleForce) : 0.5
        let pr = phase == 2 ? 0 : settings.map(raw)
        link?.sendPen(phase: phase, x: x, y: y, pressure: pr,
                      altitude: settings.tilt ? Float(t.altitudeAngle) : Float.pi / 2, azimuth: Float(t.azimuthAngle(in: self)))
    }
    private func pencil(_ touches: Set<UITouch>, _ e: UIEvent?, _ phase: UInt8) {
        for t in touches where t.type == .pencil {
            if phase == 1, let co = e?.coalescedTouches(for: t) { co.forEach { sendTouch($0, 1) } } else { sendTouch(t, phase) }
        }
    }
    override func touchesBegan(_ t: Set<UITouch>, with e: UIEvent?) { pencil(t, e, 0) }
    override func touchesMoved(_ t: Set<UITouch>, with e: UIEvent?) { pencil(t, e, 1) }
    override func touchesEnded(_ t: Set<UITouch>, with e: UIEvent?) { pencil(t, e, 2) }
    override func touchesCancelled(_ t: Set<UITouch>, with e: UIEvent?) { pencil(t, e, 2) }

    @objc private func onHover(_ g: UIHoverGestureRecognizer) {
        let (x, y) = norm(g.location(in: video))
        switch g.state {
        case .began, .changed:
            var alt = Float.pi / 2
            if #available(iOS 16.1, *) { alt = Float(g.altitudeAngle) }
            link?.sendPen(phase: 3, x: x, y: y, pressure: 0, altitude: alt, azimuth: Float(g.azimuthAngle(in: self)))
        default: link?.sendPen(phase: 4, x: x, y: y, pressure: 0, altitude: Float.pi / 2, azimuth: 0)
        }
    }
}

struct CanvasRepresentable: UIViewRepresentable {
    @ObservedObject var model: AppModel
    func makeUIView(context: Context) -> CanvasView {
        let v = CanvasView(); v.link = model.link; model.canvas = v
        v.video.onNeedKey = { [weak model] in model?.link.requestIDR() }
        model.link.onFrame = { [weak v, weak model] d, _ in v?.video.push(d); model?.tick() }
        return v
    }
    func updateUIView(_ v: CanvasView, context: Context) { v.settings = model.settings }
}
