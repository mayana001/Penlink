import Foundation
import Darwin

/// iPad слушает TCP-порт 47801. Компьютер подключается к нему сам: по USB через службу Apple (usbmuxd) или по Wi-Fi.
/// Сообщения: [u32 длина LE][тип + данные]. По USB потерь пакетов нет, поэтому пересборка и IDR при потерях не нужны.
final class HostLink {
    static let port: UInt16 = 47801
    var onFrame: ((Data, Bool) -> Void)?      // Annex B, isKey
    var onRTT: ((Double) -> Void)?            // миллисекунды
    var onConnection: ((Bool) -> Void)?

    private var lfd: Int32 = -1
    private var cfd: Int32 = -1
    private let lock = NSLock()
    private var needKey = true
    private var lastIdr = Date.distantPast

    func start() { Thread.detachNewThread { [weak self] in self?.acceptLoop() } }

    // MARK: приём соединений
    private func acceptLoop() {
        Thread.current.threadPriority = 1.0
        while true {
            if lfd < 0 && !openListener() { Thread.sleep(forTimeInterval: 1); continue }
            let c = accept(lfd, nil, nil)
            if c < 0 { Darwin.close(lfd); lfd = -1; Thread.sleep(forTimeInterval: 0.5); continue }   // после сворачивания приложения
            serve(c)
        }
    }

    private func openListener() -> Bool {
        let s = socket(AF_INET, SOCK_STREAM, 0)
        guard s >= 0 else { return false }
        var one: Int32 = 1
        setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, 4)
        var a = sockaddr_in()
        a.sin_family = sa_family_t(AF_INET); a.sin_port = HostLink.port.bigEndian; a.sin_addr.s_addr = 0
        let r = withUnsafePointer(to: &a) {
            $0.withMemoryRebound(to: sockaddr.self, capacity: 1) { bind(s, $0, socklen_t(MemoryLayout<sockaddr_in>.size)) }
        }
        guard r == 0, listen(s, 1) == 0 else { Darwin.close(s); return false }
        lfd = s
        return true
    }

    private func serve(_ c: Int32) {
        var one: Int32 = 1
        setsockopt(c, IPPROTO_TCP, TCP_NODELAY, &one, 4)
        setsockopt(c, SOL_SOCKET, SO_NOSIGPIPE, &one, 4)
        lock.lock(); cfd = c; lock.unlock()
        needKey = true
        onConnection?(true)
        Thread.detachNewThread { [weak self] in                      // ping для RTT
            while let s = self, s.currentFD() == c {
                var p: [UInt8] = [5]; p += le64(DispatchTime.now().uptimeNanoseconds)
                s.send(p); Thread.sleep(forTimeInterval: 1)
            }
        }
        var hdr = [UInt8](repeating: 0, count: 4)
        var buf = [UInt8](repeating: 0, count: 1 << 20)
        while readExact(c, &hdr, 4) {
            let n = Int(u32(hdr, 0))
            if n <= 0 || n > (32 << 20) { break }
            if buf.count < n { buf = [UInt8](repeating: 0, count: n) }
            if !readExact(c, &buf, n) { break }
            handle(buf, n)
        }
        lock.lock(); cfd = -1; lock.unlock()
        Darwin.close(c)
        onConnection?(false)
    }

    private func currentFD() -> Int32 { lock.lock(); defer { lock.unlock() }; return cfd }

    private func readExact(_ fd: Int32, _ p: UnsafeMutableRawPointer, _ n: Int) -> Bool {
        var got = 0
        while got < n {
            let r = recv(fd, p + got, n - got, 0)
            if r <= 0 { return false }
            got += r
        }
        return true
    }

    private func handle(_ b: [UInt8], _ n: Int) {
        switch b[0] {
        case 6:   // pong
            guard n >= 9 else { return }
            var t: UInt64 = 0
            for i in 0..<8 { t |= UInt64(b[1 + i]) << (8 * UInt64(i)) }
            onRTT?(Double(DispatchTime.now().uptimeNanoseconds - t) / 1e6)
        case 1:   // видеокадр: тип, флаги, fragIdx(2), fragCount(2), frameId(4), hostUs(4), данные
            guard n > 14, let cb = onFrame else { return }
            let key = (b[1] & 1) != 0
            if key { needKey = false }
            if needKey { return }                     // ждём ключевой кадр после запроса
            cb(Data(b[14..<n]), key)
        default: break
        }
    }

    // MARK: отправка
    func send(_ payload: [UInt8]) {
        lock.lock(); defer { lock.unlock() }
        guard cfd >= 0 else { return }
        let m = le32(UInt32(payload.count)) + payload
        m.withUnsafeBytes { raw in
            var off = 0
            while off < m.count {
                let r = Darwin.send(cfd, raw.baseAddress! + off, m.count - off, 0)
                if r <= 0 { return }
                off += r
            }
        }
    }

    /// phase: 0 down, 1 move, 2 up, 3 hover, 4 hover end
    func sendPen(phase: UInt8, buttons: UInt8 = 0, x: Float, y: Float, pressure: Float, altitude: Float, azimuth: Float) {
        var p: [UInt8] = [2, phase, buttons]
        for v in [x, y, pressure, altitude, azimuth] { p += le32(v.bitPattern) }
        send(p)
    }
    /// action: 0 tap, 1 down, 2 up. mods: 1 Ctrl, 2 Shift, 4 Alt, 8 Win
    func sendKey(action: UInt8, mods: UInt8, vk: UInt16) { send([4, action, mods] + le16(vk)) }
    func requestIDR() {
        guard Date().timeIntervalSince(lastIdr) > 0.1 else { return }
        lastIdr = Date(); needKey = true; send([3])
    }
}

private func le16(_ v: UInt16) -> [UInt8] { [UInt8(v & 0xff), UInt8(v >> 8)] }
private func le32(_ v: UInt32) -> [UInt8] { (0..<4).map { UInt8((v >> (8 * UInt32($0))) & 0xff) } }
private func le64(_ v: UInt64) -> [UInt8] { (0..<8).map { UInt8((v >> (8 * UInt64($0))) & 0xff) } }
private func u32(_ b: [UInt8], _ o: Int) -> UInt32 {
    UInt32(b[o]) | UInt32(b[o + 1]) << 8 | UInt32(b[o + 2]) << 16 | UInt32(b[o + 3]) << 24
}
