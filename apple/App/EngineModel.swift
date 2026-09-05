//
//  EngineModel.swift
//  Observable wrapper around the Objective-C++ VizEngine. Owns the engine,
//  exposes SwiftUI-friendly state, and polls live status for the readouts.
//
import SwiftUI
import Combine

@MainActor
final class EngineModel: ObservableObject {
    let engine = VizEngine()

    // Live status (refreshed by a display-linked timer).
    @Published var capturing = false { didSet { if capturing != oldValue { syncDockTile() } } }
    /// Times the engine re-opened the tap by itself (device change, wake, stall).
    @Published var tapRestarts = 0
    @Published var sampleRate = 0.0
    @Published var channels = 0
    // Fast-changing readouts live on their own object so their 15 Hz updates
    // only re-render LevelsView, not this whole model's observers (see
    // LevelsView.swift for the frame-rate story behind this).
    let levels = LevelsModel()
    @Published var lastError: String?

    // Controls bound to the UI.
    @Published var sensitivity: Float = 1.0 { didSet { engine.sensitivity = sensitivity } }
    // 0 stretch (classic AVS), 1 fill (round shapes; default), 2 fit
    @Published var aspectMode: Int = 1 { didSet { engine.setAspectMode(aspectMode) } }
    @Published var showHUD = false { didSet { engine.showHUD = showHUD } }

    // Effect stack, mirrored from the engine.
    struct EffectEntry: Identifiable {
        let id: Int          // index in the engine's stack
        let name: String
        var enabled: Bool
    }
    @Published var effects: [EffectEntry] = []

    // Presets: selecting one rebuilds the stack (and thus the effects list).
    struct PresetEntry: Identifiable { let id: Int; let name: String; let modern: Bool }
    @Published var presetNames: [PresetEntry] = []
    @Published var currentPreset: Int = 0 {
        didSet {
            guard currentPreset != engine.currentPreset else { return }
            engine.currentPreset = currentPreset
            currentPresetTitle = presetNames.first { $0.id == currentPreset }?.name ?? ""
            syncEffects()
        }
    }
    /// What the header shows: the built-in preset's name, or the file name of
    /// the loaded .avs/.json (album tracks included).
    @Published var currentPresetTitle: String = ""

    // Window layout, remembered across launches (the AppKit side is in
    // WindowChrome.swift).
    @Published var showSidebar: Bool = UserDefaults.standard.object(forKey: "showSidebar") as? Bool ?? true {
        didSet { UserDefaults.standard.set(showSidebar, forKey: "showSidebar") }
    }
    /// Widget mode: a small always-on-top window with just the visuals.
    @Published var widgetMode: Bool = UserDefaults.standard.bool(forKey: "widgetMode") {
        didSet { UserDefaults.standard.set(widgetMode, forKey: "widgetMode") }
    }
    /// Live Dock icon: a small spectrum with beat flashes and BPM while capturing.
    @Published var liveDockIcon: Bool = UserDefaults.standard.object(forKey: "liveDockIcon") as? Bool ?? true {
        didSet { UserDefaults.standard.set(liveDockIcon, forKey: "liveDockIcon"); syncDockTile() }
    }
    private lazy var dockTile = DockTileController(engine: engine)
    private func syncDockTile() {
        if liveDockIcon && capturing { dockTile.start() } else { dockTile.stop() }
    }

    private var poll: Timer?

    init() {
        presetNames = engine.presetNames.enumerated().map { i, raw in
            let p = raw.split(separator: "|").map(String.init)
            return PresetEntry(id: i, name: p.first ?? raw,
                               modern: p.count > 1 && p[1] == "1")
        }
        currentPreset = Int(engine.currentPreset)
        currentPresetTitle = presetNames.first { $0.id == currentPreset }?.name ?? ""
        syncEffects()

        albumRootPath = UserDefaults.standard.string(forKey: "albumRootPath")
        if albumRootPath != nil { scanAlbums() }
    }

    private func syncEffects() {
        effects = engine.effectNames.enumerated().map { i, name in
            EffectEntry(id: i, name: name, enabled: engine.isEffectEnabled(at: i))
        }
        if let s = selectedEffect, s >= effects.count { selectedEffect = nil }
        syncParams()
    }

    func toggleEffect(_ id: Int) {
        guard let idx = effects.firstIndex(where: { $0.id == id }) else { return }
        effects[idx].enabled.toggle()
        engine.setEffect(id, enabled: effects[idx].enabled)
    }

    // Effect browser: full registry as (key, display name), sorted by name.
    var availableEffects: [(key: String, name: String, modern: Bool)] {
        engine.availableEffects.compactMap { entry in
            let parts = entry.split(separator: "|").map(String.init)
            guard parts.count >= 2 else { return nil }
            return (parts[0], parts[1], parts.count > 2 && parts[2] == "1")
        }
    }

    /// Load any .avs (or native .json) preset file as a standalone preset
    /// (crossfade in; leaves album playback).
    func loadAvsFile(_ url: URL) {
        currentAlbum = nil
        if url.pathExtension.lowercased() == "json" {
            avsLoadReport = engine.loadJsonPreset(atPath: url.path)
        } else {
            avsLoadReport = engine.loadAvsPreset(atPath: url.path)
        }
        currentPresetTitle = url.deletingPathExtension().lastPathComponent
        syncEffects()
    }

    // Preset Browser window state (see PresetBrowserView.swift).
    @Published var browserSource: BrowserSource = .builtin

    // Preset shuffle: rotate through the built-in presets — or the whole
    // library folder — with the engine's crossfade doing the blending.
    enum ShuffleScope: String { case builtin, library }
    @Published var shuffleOn = false { didSet { restartShuffle() } }
    @Published var shuffleInterval: Double = 30 { didSet { restartShuffle() } }
    @Published var shuffleScope: ShuffleScope = .builtin
    private var shuffleTimer: Timer?

    private func restartShuffle() {
        shuffleTimer?.invalidate()
        shuffleTimer = nil
        guard shuffleOn else { return }
        shuffleTimer = Timer.scheduledTimer(withTimeInterval: shuffleInterval,
                                            repeats: true) { [weak self] _ in
            Task { @MainActor in self?.shuffleNext() }
        }
    }

    func shuffleNext() {
        if shuffleScope == .library, let pick = libraryPresets.randomElement() {
            loadAvsFile(pick.url)
            return
        }
        let total = presetNames.count
        guard total > 1 else { return }
        var pick = Int.random(in: 0..<total)
        if pick == currentPreset { pick = (pick + 1) % total }
        currentPreset = pick
    }

    // Album playback: numbered preset sequences (e.g. visbot's VE/"Visual
    // Episode" packs) meant to be experienced in order — later tracks are
    // often pure modifiers authored to warp whatever the previous track left
    // on screen, matching classic AVS's own never-clear-between-presets
    // behavior. A folder qualifies if it directly contains 2+ .avs files
    // whose name starts with a track number ("01 ", "08 ", "A08 ", "27_").
    struct AlbumTrack: Identifiable { let id = UUID(); let title: String; let url: URL }
    struct AlbumEntry: Identifiable {
        let id = UUID(); let name: String; let path: URL; let tracks: [AlbumTrack]
    }
    @Published var albumRootPath: String? {
        didSet { UserDefaults.standard.set(albumRootPath, forKey: "albumRootPath") }
    }
    @Published var albums: [AlbumEntry] = []
    @Published var albumsScanning = false
    @Published var currentAlbum: AlbumEntry?
    @Published var currentTrackIndex: Int = 0

    // Every .avs under the library folder (the same scan finds the albums).
    // `pack` is the top-level folder under the root, i.e. the archive pack.
    struct LibraryEntry: Identifiable {
        let id: String            // path
        let name: String
        let pack: String
        let url: URL
    }
    @Published var libraryPresets: [LibraryEntry] = []

    nonisolated private static let trackPrefixRegex = try! NSRegularExpression(
        pattern: "^[A-Za-z]?([0-9]{1,3})[^0-9A-Za-z]")

    nonisolated private static func trackNumber(_ filename: String) -> Int? {
        let range = NSRange(filename.startIndex..., in: filename)
        guard let m = trackPrefixRegex.firstMatch(in: filename, range: range),
              let r = Range(m.range(at: 1), in: filename) else { return nil }
        return Int(filename[r])
    }

    func chooseLibraryRoot() {
        let panel = NSOpenPanel()
        panel.canChooseFiles = false
        panel.canChooseDirectories = true
        panel.message = "Choose a folder of .avs presets (e.g. presets/extracted): every file is listed in the Preset Browser, numbered sequences become albums"
        if panel.runModal() == .OK, let url = panel.url {
            albumRootPath = url.path
            scanAlbums()
        }
    }

    func scanAlbums() {
        guard let root = albumRootPath else { return }
        albumsScanning = true
        let rootURL = URL(fileURLWithPath: root)
        Task.detached { [weak self] in
            guard let self else { return }
            let t0 = Date()
            let found = Self.scanLibrary(root: rootURL)
            let ms = Int(Date().timeIntervalSince(t0) * 1000)
            await MainActor.run {
                self.albums = found.albums.sorted { $0.tracks.count > $1.tracks.count }
                self.libraryPresets = found.presets
                self.albumsScanning = false
                NSLog("[Library] %@: %d presets, %d albums (%d ms)",
                      rootURL.lastPathComponent, found.presets.count, found.albums.count, ms)
            }
        }
    }

    nonisolated private static func scanLibrary(root: URL)
        -> (albums: [AlbumEntry], presets: [LibraryEntry])
    {
        var albums: [AlbumEntry] = []
        var presets: [LibraryEntry] = []
        let fm = FileManager.default
        let rootComponents = root.standardizedFileURL.pathComponents.count
        guard let en = fm.enumerator(at: root, includingPropertiesForKeys: [.isDirectoryKey],
                                     options: [.skipsHiddenFiles]) else { return (albums, presets) }
        for case let url as URL in en {
            let isDir = (try? url.resourceValues(forKeys: [.isDirectoryKey]))?.isDirectory == true
            if !isDir {
                guard url.pathExtension.lowercased() == "avs" else { continue }
                let comps = url.standardizedFileURL.pathComponents
                let pack = comps.count > rootComponents + 1 ? comps[rootComponents] : root.lastPathComponent
                presets.append(LibraryEntry(id: url.path,
                                            name: url.deletingPathExtension().lastPathComponent,
                                            pack: pack, url: url))
                continue
            }
            guard let items = try? fm.contentsOfDirectory(at: url, includingPropertiesForKeys: nil)
            else { continue }
            var numbered: [(Int, URL)] = []
            for item in items where item.pathExtension.lowercased() == "avs" {
                if let n = trackNumber(item.lastPathComponent) {
                    numbered.append((n, item))
                }
            }
            guard numbered.count >= 2 else { continue }
            numbered.sort { $0.0 < $1.0 }
            let tracks = numbered.map {
                AlbumTrack(title: $0.1.deletingPathExtension().lastPathComponent, url: $0.1)
            }
            albums.append(AlbumEntry(name: url.lastPathComponent, path: url, tracks: tracks))
        }
        presets.sort { $0.name.localizedCaseInsensitiveCompare($1.name) == .orderedAscending }
        return (albums, presets)
    }

    func playAlbum(_ album: AlbumEntry) {
        guard !album.tracks.isEmpty else { return }
        currentAlbum = album
        currentTrackIndex = 0
        avsLoadReport = engine.loadAlbumTrack(atPath: album.tracks[0].url.path, isFirst: true)
        currentPresetTitle = albumTrackTitle(album, 0)
        syncEffects()
    }

    func nextAlbumTrack() {
        guard let album = currentAlbum, currentTrackIndex + 1 < album.tracks.count else { return }
        currentTrackIndex += 1
        avsLoadReport = engine.loadAlbumTrack(
            atPath: album.tracks[currentTrackIndex].url.path, isFirst: false)
        currentPresetTitle = albumTrackTitle(album, currentTrackIndex)
        syncEffects()
    }

    func prevAlbumTrack() {
        guard let album = currentAlbum, currentTrackIndex > 0 else { return }
        currentTrackIndex -= 1
        avsLoadReport = engine.loadAlbumTrack(
            atPath: album.tracks[currentTrackIndex].url.path, isFirst: false)
        currentPresetTitle = albumTrackTitle(album, currentTrackIndex)
        syncEffects()
    }

    private func albumTrackTitle(_ album: AlbumEntry, _ i: Int) -> String {
        "\(album.name) · \(album.tracks[i].url.deletingPathExtension().lastPathComponent)"
    }

    @Published var albumAutoAdvance = false { didSet { restartAlbumAutoAdvance() } }
    @Published var albumAutoAdvanceInterval: Double = 45 { didSet { restartAlbumAutoAdvance() } }
    private var albumTimer: Timer?

    private func restartAlbumAutoAdvance() {
        albumTimer?.invalidate(); albumTimer = nil
        guard albumAutoAdvance else { return }
        albumTimer = Timer.scheduledTimer(withTimeInterval: albumAutoAdvanceInterval,
                                          repeats: true) { [weak self] _ in
            Task { @MainActor in
                guard let self, let album = self.currentAlbum else { return }
                if self.currentTrackIndex + 1 < album.tracks.count {
                    self.nextAlbumTrack()
                } else {
                    self.albumAutoAdvance = false   // stop at the end of the album
                }
            }
        }
    }

    func addEffect(key: String) {
        engine.addEffect(withKey: key)
        syncEffects()
    }

    func removeEffect(_ id: Int) {
        engine.removeEffect(at: id)
        syncEffects()
    }

    func moveEffect(_ id: Int, delta: Int) {
        engine.moveEffect(at: id, to: id + delta)
        syncEffects()
    }

    // Selected effect + its live parameters.
    struct ParamEntry: Identifiable {
        var id: String { key }
        let key: String
        let min: Float
        let max: Float
        var value: Float
        var reg: Int = -1        // bound global register, -1 = none
    }
    @Published var selectedEffect: Int? { didSet { syncParams() } }
    @Published var params: [ParamEntry] = []

    func syncParams() {
        guard let idx = selectedEffect, idx < effects.count else { params = []; return }
        params = engine.paramsForEffect(at: idx).compactMap { entry in
            let p = entry.split(separator: "|").map(String.init)
            guard p.count >= 4, let mn = Float(p[1]), let mx = Float(p[2]),
                  let v = Float(p[3]) else { return nil }
            let reg = p.count >= 5 ? (Int(p[4]) ?? -1) : -1
            return ParamEntry(key: p[0], min: mn, max: mx, value: v, reg: reg)
        }
        syncScripts()
    }

    // Scripts of the selected effect (nil when not scriptable).
    @Published var scripts: [String]? = nil

    func syncScripts() {
        scripts = selectedEffect.flatMap { engine.scriptsForEffect(at: $0) }
    }

    func applyScripts(_ s: [String]) {
        guard let idx = selectedEffect, s.count == 4 else { return }
        engine.setScriptsForEffectAt(idx, initScript: s[0], frameScript: s[1],
                                     beatScript: s[2], pointScript: s[3])
        syncScripts()
        syncParams()
    }

    // Macro knobs: live sliders over reg00-07 (scripts may also write these).
    @Published var macroRegs: [Float] = Array(repeating: 0, count: 8)

    func setMacroReg(_ i: Int, _ v: Float) {
        macroRegs[i] = v
        engine.setGlobalReg(i, value: v)
    }

    func bindParam(_ key: String, toReg reg: Int) {
        guard let idx = selectedEffect else { return }
        engine.bindParamForEffect(at: idx, key: key, toReg: reg)
        syncParams()
    }

    func setParam(_ key: String, _ value: Float) {
        guard let idx = selectedEffect else { return }
        engine.setParamForEffectAt(idx, key: key, value: value)
        if let i = params.firstIndex(where: { $0.key == key }) { params[i].value = value }
    }

    // Classic Winamp .avs preset loading.
    @Published var avsLoadReport: String?

    func openAvsPreset() {
        let panel = NSOpenPanel()
        panel.allowedContentTypes = ["avs", "json"].compactMap { .init(filenameExtension: $0) }
        panel.allowsMultipleSelection = false
        panel.message = "Choose a preset (.avs classic Winamp, or .json native)"
        if panel.runModal() == .OK, let url = panel.url {
            loadAvsFile(url)
        }
    }

    func savePreset() {
        let panel = NSSavePanel()
        panel.allowedContentTypes = [.init(filenameExtension: "json")].compactMap { $0 }
        panel.nameFieldStringValue = "My Preset.json"
        panel.message = "Save the current effect stack as a preset"
        if panel.runModal() == .OK, let url = panel.url {
            let name = url.deletingPathExtension().lastPathComponent
            avsLoadReport = engine.saveJsonPreset(toPath: url.path, name: name)
        }
    }

    // --- Music visual plugin ---------------------------------------------
    // The plugin bundle ships inside the app (Contents/PlugIns). Music loads
    // visual plugins from ~/Library/iTunes/iTunes Plug-ins at launch, so
    // installing is a copy, then a restart of Music and View ▸ Visualizer.
    static let musicPluginName = "MScopesPlugin.bundle"
    static var musicPluginsDir: URL {
        FileManager.default.homeDirectoryForCurrentUser
            .appendingPathComponent("Library/iTunes/iTunes Plug-ins", isDirectory: true)
    }
    static var installedMusicPlugin: URL { musicPluginsDir.appendingPathComponent(musicPluginName) }
    var bundledMusicPlugin: URL? {
        Bundle.main.builtInPlugInsURL?.appendingPathComponent(Self.musicPluginName)
    }
    @Published var musicPluginInstalled = FileManager.default.fileExists(atPath: installedMusicPlugin.path)

    func installMusicPlugin() {
        guard let src = bundledMusicPlugin, FileManager.default.fileExists(atPath: src.path) else {
            lastError = "This build doesn't include the Music plugin."
            return
        }
        let fm = FileManager.default
        do {
            try fm.createDirectory(at: Self.musicPluginsDir, withIntermediateDirectories: true)
            if fm.fileExists(atPath: Self.installedMusicPlugin.path) {
                try fm.removeItem(at: Self.installedMusicPlugin)
            }
            try fm.copyItem(at: src, to: Self.installedMusicPlugin)
            musicPluginInstalled = true
            let alert = NSAlert()
            alert.messageText = "Music plugin installed"
            alert.informativeText = "Copied to ~/Library/iTunes/iTunes Plug-ins.\n\nQuit and reopen Music, then choose View ▸ Visualizer ▸ MScopes (or press ⌘T with the visualizer showing)."
            alert.runModal()
        } catch {
            lastError = "Plugin install failed: \(error.localizedDescription)"
        }
    }

    func uninstallMusicPlugin() {
        do {
            try FileManager.default.removeItem(at: Self.installedMusicPlugin)
            musicPluginInstalled = false
        } catch {
            lastError = "Plugin removal failed: \(error.localizedDescription)"
        }
    }

    func start() {
        do {
            try engine.start()          // ObjC (BOOL, NSError**) imports as throwing
            capturing = true
            lastError = nil
        } catch {
            capturing = false
            lastError = error.localizedDescription
        }
        sampleRate = engine.sampleRate
        channels = engine.channels

        poll?.invalidate()
        // WV_POLL_HZ overrides the readout rate (diagnostic knob; 1 = near-static UI).
        let pollHz = Double(ProcessInfo.processInfo.environment["WV_POLL_HZ"] ?? "") ?? 15
        poll = Timer.scheduledTimer(withTimeInterval: 1.0 / pollHz, repeats: true) { [weak self] _ in
            Task { @MainActor in self?.refresh() }
        }
    }

    func stop() {
        poll?.invalidate(); poll = nil
        engine.stop()
        capturing = false
    }

    private func refresh() {
        // Only publish on this (whole-sidebar) object when something changed.
        if capturing != engine.isCapturing { capturing = engine.isCapturing }
        if tapRestarts != engine.tapRestarts {
            tapRestarts = engine.tapRestarts
            sampleRate = engine.sampleRate; channels = engine.channels
        }
        levels.peak = engine.peak
        levels.bass = engine.bass
        levels.mid = engine.mid
        levels.treble = engine.treble
        levels.bpm = engine.bpm
        levels.beatLevel = engine.beatLevel
        levels.audioCallbacks = engine.audioCallbacks
        levels.frames = engine.renderedFrames
    }
}
