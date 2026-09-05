//
//  DockTile.swift
//  A live Dock icon: while audio is captured, the Dock tile shows a small
//  spectrum in the app's colours, flashes on detected beats and prints the
//  BPM once the detector has locked. Drawn through NSDockTile's content
//  view (the sanctioned way — Activity Monitor's CPU graph works like this)
//  at a modest rate; the bundle's static icon comes back when capture stops
//  or the option is off.
//
import AppKit

final class DockTileController {
    private let engine: VizEngine
    private let view = DockTileView()
    private var timer: Timer?
    private static let rate: TimeInterval = 1.0 / 15.0

    init(engine: VizEngine) { self.engine = engine }

    var running: Bool { timer != nil }

    func start() {
        guard timer == nil else { return }
        NSApp.dockTile.contentView = view
        view.frame = NSRect(x: 0, y: 0, width: 128, height: 128)
        let t = Timer(timeInterval: Self.rate, repeats: true) { [weak self] _ in self?.tick() }
        t.tolerance = Self.rate / 2
        RunLoop.main.add(t, forMode: .common)
        timer = t
        tick()
    }

    func stop() {
        timer?.invalidate(); timer = nil
        NSApp.dockTile.contentView = nil
        NSApp.dockTile.display()
    }

    private func tick() {
        var bands = [Float](repeating: 0, count: DockTileView.bandCount)
        engine.copyBands(&bands, count: bands.count)
        view.update(bands: bands, beat: engine.beatLevel, bpm: engine.bpm)
        NSApp.dockTile.display()
    }
}

final class DockTileView: NSView {
    static let bandCount = 20
    private var bands = [Float](repeating: 0, count: DockTileView.bandCount)
    private var smoothed = [Float](repeating: 0, count: DockTileView.bandCount)
    private var beat: Float = 0
    private var bpm: Float = 0

    func update(bands: [Float], beat: Float, bpm: Float) {
        self.bands = bands
        // Fast attack, slower release, like a meter. Highs are lifted a
        // little (music rolls off with frequency) and compressed so a
        // 128-pixel tile shows the whole shape, not just the bass.
        let n = max(bands.count - 1, 1)
        for i in smoothed.indices {
            let tilt = 0.7 + 1.1 * Float(i) / Float(n)
            let v = min(1, sqrt(max(0, bands[i])) * tilt)
            smoothed[i] = v > smoothed[i] ? v : smoothed[i] * 0.72 + v * 0.28
        }
        self.beat = beat
        self.bpm = bpm
        needsDisplay = true
    }

    override func draw(_ dirtyRect: NSRect) {
        guard let ctx = NSGraphicsContext.current?.cgContext else { return }
        let b = bounds
        let radius = b.width * 0.2237   // the macOS icon squircle, near enough

        // Tile: near-black, a faint glass edge, brighter on a beat.
        let tile = NSBezierPath(roundedRect: b.insetBy(dx: 1, dy: 1), xRadius: radius, yRadius: radius)
        NSColor(calibratedWhite: 0.06 + 0.06 * CGFloat(beat), alpha: 1).setFill()
        tile.fill()
        NSColor(calibratedWhite: 1, alpha: 0.10 + 0.25 * CGFloat(beat)).setStroke()
        tile.lineWidth = 1.5
        tile.stroke()

        // Bars, cyan → violet → pink across the width, clipped to the tile.
        ctx.saveGState()
        tile.addClip()
        let n = smoothed.count
        let inset = b.width * 0.14
        let bottom = b.height * 0.24
        let top = b.height * 0.80
        let slot = (b.width - 2 * inset) / CGFloat(n)
        let barW = slot * 0.62
        let stops: [(CGFloat, NSColor)] = [
            (0.0, NSColor(calibratedRed: 0.45, green: 0.95, blue: 1.00, alpha: 1)),
            (0.5, NSColor(calibratedRed: 0.75, green: 0.55, blue: 1.00, alpha: 1)),
            (1.0, NSColor(calibratedRed: 1.00, green: 0.45, blue: 0.75, alpha: 1)),
        ]
        for i in 0..<n {
            let t = CGFloat(i) / CGFloat(max(n - 1, 1))
            let color = Self.lerp(stops, t)
            let h = max(2, CGFloat(smoothed[i]) * (top - bottom))
            let x = inset + CGFloat(i) * slot + (slot - barW) / 2
            let r = NSRect(x: x, y: bottom, width: barW, height: h)
            color.withAlphaComponent(0.55 + 0.45 * CGFloat(smoothed[i])).setFill()
            NSBezierPath(roundedRect: r, xRadius: barW / 2, yRadius: barW / 2).fill()
            // Glow on beats.
            if beat > 0.05 {
                color.withAlphaComponent(0.35 * CGFloat(beat)).setFill()
                NSBezierPath(roundedRect: r.insetBy(dx: -barW * 0.6, dy: -barW * 0.6),
                             xRadius: barW, yRadius: barW).fill()
            }
        }
        ctx.restoreGState()

        // BPM, once the detector has locked.
        if bpm > 0 {
            let text = String(format: "%.0f", bpm)
            let attrs: [NSAttributedString.Key: Any] = [
                .font: NSFont.systemFont(ofSize: b.height * 0.13, weight: .heavy),
                .foregroundColor: NSColor(calibratedWhite: 1, alpha: 0.55 + 0.45 * CGFloat(beat)),
            ]
            let size = text.size(withAttributes: attrs)
            text.draw(at: NSPoint(x: (b.width - size.width) / 2, y: b.height * 0.07), withAttributes: attrs)
        }
    }

    private static func lerp(_ stops: [(CGFloat, NSColor)], _ t: CGFloat) -> NSColor {
        var lo = stops[0], hi = stops[stops.count - 1]
        for s in stops { if s.0 <= t { lo = s }; if s.0 >= t { hi = s; break } }
        let span = max(hi.0 - lo.0, 0.0001)
        return lo.1.blended(withFraction: (t - lo.0) / span, of: hi.1) ?? lo.1
    }
}
