import SwiftUI

struct PenSettings {
    var gamma: Double = 1.0      // <1 мягче (легче выжать нажим), >1 жёстче
    var minPressure: Double = 0.0
    var tilt = true
    func map(_ p: Float) -> Float {
        let v = pow(Double(min(max(p, 0), 1)), gamma)
        return Float(minPressure + (1 - minPressure) * v)
    }
}

struct HotKey: Identifiable {
    let id = UUID()
    let label: String; let mods: UInt8; let vk: UInt16; var hold = false
}

final class AppModel: ObservableObject {
    @Published var connected = false
    @Published var settings = PenSettings()
    @Published var fps = 0
    @Published var rtt = 0.0
    let link = HostLink()
    weak var canvas: CanvasView?
    private var frames = 0, last = Date()

    // Ctrl=1 Shift=2 Alt=4 Win=8; vk — коды Windows
    let keys: [HotKey] = [
        .init(label: "Undo", mods: 1, vk: 0x5A), .init(label: "Redo", mods: 3, vk: 0x5A),
        .init(label: "Brush", mods: 0, vk: 0x42), .init(label: "Eraser", mods: 0, vk: 0x45),
        .init(label: "Hand", mods: 0, vk: 0x20, hold: true),
        .init(label: "[", mods: 0, vk: 0xDB), .init(label: "]", mods: 0, vk: 0xDD),
        .init(label: "Zoom +", mods: 1, vk: 0xBB), .init(label: "Zoom −", mods: 1, vk: 0xBD),
        .init(label: "Save", mods: 1, vk: 0x53),
    ]

    init() {
        link.onRTT = { [weak self] r in DispatchQueue.main.async { self?.rtt = r } }
        link.onConnection = { [weak self] c in
            DispatchQueue.main.async {
                self?.connected = c
                UIApplication.shared.isIdleTimerDisabled = c   // не гасить экран во время работы
            }
        }
        link.start()
    }

    func tick() {   // вызывается с потока приёма
        frames += 1
        let now = Date()
        if now.timeIntervalSince(last) >= 1 {
            let f = frames; frames = 0; last = now
            DispatchQueue.main.async { self.fps = f }
        }
    }
}

@main
struct PenLinkApp: App {
    @StateObject private var model = AppModel()
    var body: some Scene {
        WindowGroup {
            ZStack {
                CanvasRepresentable(model: model).ignoresSafeArea()
                if model.connected { PanelView(model: model) } else { WaitingView() }
            }
            .background(Color.black).statusBarHidden().persistentSystemOverlays(.hidden)
        }
    }
}

struct WaitingView: View {
    var body: some View {
        VStack(spacing: 14) {
            Text("PenLink").font(.largeTitle.bold())
            Text("Подключите iPad к компьютеру кабелем и запустите penlink-host.exe.\nОставьте это приложение открытым, iPad разблокированным.")
                .multilineTextAlignment(.center).foregroundStyle(.secondary)
            ProgressView().padding(.top, 8)
        }.padding().foregroundStyle(.white)
    }
}

/// Перемещаемая и сворачиваемая панель. В свёрнутом виде — маленький круг, который тоже можно тянуть; тап — развернуть.
struct PanelView: View {
    @ObservedObject var model: AppModel
    @State private var pos = CGPoint(x: 90, y: 220)
    @State private var open = true
    @State private var pressed: Set<UUID> = []

    var body: some View {
        GeometryReader { geo in
            Group {
                if open { panel } else {
                    Image(systemName: "pencil.tip.crop.circle.fill").font(.system(size: 34)).padding(10)
                        .background(.ultraThinMaterial, in: Circle())
                        .onTapGesture { open = true }
                }
            }
            .position(pos)
            .gesture(DragGesture().onChanged { v in
                pos = CGPoint(x: min(max(v.location.x, 30), geo.size.width - 30), y: min(max(v.location.y, 30), geo.size.height - 30))
            })
        }
    }

    var panel: some View {
        VStack(spacing: 8) {
            HStack {
                Image(systemName: "line.3.horizontal").foregroundStyle(.secondary)   // ручка для перетаскивания
                Spacer()
                Button { open = false } label: { Image(systemName: "chevron.up.circle") }
            }
            ForEach(model.keys) { k in
                Text(k.label).frame(width: 96, height: 40)
                    .background(pressed.contains(k.id) ? Color.accentColor : Color.gray.opacity(0.35), in: RoundedRectangle(cornerRadius: 8))
                    .gesture(DragGesture(minimumDistance: 0).onChanged { _ in
                        if !pressed.contains(k.id) { pressed.insert(k.id); if k.hold { model.link.sendKey(action: 1, mods: k.mods, vk: k.vk) } }
                    }.onEnded { _ in
                        pressed.remove(k.id); model.link.sendKey(action: k.hold ? 2 : 0, mods: k.mods, vk: k.vk)
                    })
            }
            Divider()
            Text("Нажим").font(.caption)
            Slider(value: $model.settings.gamma, in: 0.4...2.5)
            Text("Мин. нажим").font(.caption)
            Slider(value: $model.settings.minPressure, in: 0...0.3)
            Toggle("Наклон", isOn: $model.settings.tilt).font(.caption)
            Button("Сброс вида") { model.canvas?.resetView() }.font(.caption)
            Text("\(model.fps) fps · RTT \(String(format: "%.1f", model.rtt)) мс").font(.caption2).foregroundStyle(.secondary)
        }
        .padding(10).frame(width: 120)
        .background(.ultraThinMaterial, in: RoundedRectangle(cornerRadius: 14))
    }
}
