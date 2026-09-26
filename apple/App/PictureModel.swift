//
//  PictureModel.swift
//  The Picture settings (core/effects/Picture.h): colour looks, grain,
//  vignette, glow, scanlines and quality. They act only when the finished
//  frame is drawn to the window, never on the frame a preset feeds forward,
//  so they apply to every preset, classic ones included, without changing
//  how any preset behaves. Remembered across launches.
//
import AppKit
import SwiftUI
import UniformTypeIdentifiers

@MainActor
final class PictureModel: ObservableObject {
    private let engine: VizEngine
    private let defaults = UserDefaults.standard

    /// Built-in looks; index 0 is "Off". The custom .cube look is tag -1.
    let lookNames: [String]

    @Published var look: Int { didSet { applyLook(); save() } }
    @Published private(set) var customLookName: String?
    @Published var strength: Double { didSet { engine.lookStrength = Float(strength); save() } }
    @Published var grain: Double { didSet { engine.grain = Float(grain); save() } }
    @Published var vignette: Double { didSet { engine.vignette = Float(vignette); save() } }
    @Published var glow: Double { didSet { engine.glow = Float(glow); save() } }
    @Published var scanlines: Double { didSet { engine.scanlines = Float(scanlines); save() } }
    @Published var reactToBeat: Bool { didSet { engine.pictureReactsToBeat = reactToBeat; save() } }
    @Published var sharpScaling: Bool { didSet { engine.sharpScaling = sharpScaling; save() } }
    @Published var lastError: String?

    private var customLookPath: String?
    private var loading = true

    init(engine: VizEngine) {
        self.engine = engine
        lookNames = engine.lookNames
        let d = UserDefaults.standard
        look = d.object(forKey: "picture.look") as? Int ?? 0
        strength = d.object(forKey: "picture.strength") as? Double ?? 1.0
        grain = d.object(forKey: "picture.grain") as? Double ?? 0
        vignette = d.object(forKey: "picture.vignette") as? Double ?? 0
        glow = d.object(forKey: "picture.glow") as? Double ?? 0
        scanlines = d.object(forKey: "picture.scanlines") as? Double ?? 0
        reactToBeat = d.object(forKey: "picture.reactToBeat") as? Bool ?? false
        sharpScaling = d.object(forKey: "picture.sharpScaling") as? Bool ?? true
        customLookPath = d.string(forKey: "picture.customLookPath")

        // didSet doesn't run during init: push everything once.
        if let path = customLookPath, engine.loadCustomLook(atPath: path) == nil {
            customLookName = engine.customLookName
        } else if look == -1 {
            look = 0                      // the remembered .cube file is gone
        }
        engine.lookStrength = Float(strength)
        engine.grain = Float(grain)
        engine.vignette = Float(vignette)
        engine.glow = Float(glow)
        engine.scanlines = Float(scanlines)
        engine.pictureReactsToBeat = reactToBeat
        engine.sharpScaling = sharpScaling
        loading = false
        applyLook()
    }

    var lookIsOff: Bool { look == 0 }

    /// Display name of the current look.
    var currentLookName: String {
        if look == -1 { return customLookName ?? "Custom" }
        return lookNames.indices.contains(look) ? lookNames[look] : "Off"
    }

    /// Next built-in look, wrapping (skips the custom look).
    func nextLook() { look = (max(look, 0) + 1) % max(lookNames.count, 1) }

    func chooseCustomLook() {
        let panel = NSOpenPanel()
        panel.title = "Choose a .cube Look"
        panel.allowedContentTypes = [UTType(filenameExtension: "cube") ?? .data]
        panel.allowsMultipleSelection = false
        guard panel.runModal() == .OK, let url = panel.url else { return }
        loadCustomLook(url)
    }

    func loadCustomLook(_ url: URL) {
        if let err = engine.loadCustomLook(atPath: url.path) {
            lastError = "\(url.lastPathComponent): \(err)"
            return
        }
        lastError = nil
        customLookPath = url.path
        customLookName = engine.customLookName
        look = -1
    }

    func reset() {
        look = 0; strength = 1; grain = 0; vignette = 0; glow = 0; scanlines = 0
        reactToBeat = false; sharpScaling = true
    }

    private func applyLook() {
        guard !loading else { return }
        if look == -1 && customLookName == nil { look = 0; return }
        engine.lookIndex = look
    }

    private func save() {
        guard !loading else { return }
        defaults.set(look, forKey: "picture.look")
        defaults.set(strength, forKey: "picture.strength")
        defaults.set(grain, forKey: "picture.grain")
        defaults.set(vignette, forKey: "picture.vignette")
        defaults.set(glow, forKey: "picture.glow")
        defaults.set(scanlines, forKey: "picture.scanlines")
        defaults.set(reactToBeat, forKey: "picture.reactToBeat")
        defaults.set(sharpScaling, forKey: "picture.sharpScaling")
        defaults.set(customLookPath, forKey: "picture.customLookPath")
    }
}
