//
//  MScopesApp.swift
//  SwiftUI entry point: the main visualizer window plus the Preset Browser
//  window, both over the one shared EngineModel.
//
import SwiftUI

@main
struct MScopesApp: App {
    @StateObject private var model = EngineModel()

    var body: some Scene {
        WindowGroup {
            ContentView()
                .environmentObject(model)
        }
        .windowStyle(.titleBar)
        .defaultSize(width: 940, height: 560)
        .commands { PresetCommands(model: model) }

        Window("Preset Browser", id: PresetBrowserView.windowID) {
            PresetBrowserView()
                .environmentObject(model)
        }
        .defaultSize(width: 720, height: 520)
    }
}

/// "Presets" menu: the browser window and a random-next shortcut.
struct PresetCommands: Commands {
    let model: EngineModel
    @Environment(\.openWindow) private var openWindow

    var body: some Commands {
        CommandMenu("Presets") {
            Button("Preset Browser…") { openWindow(id: PresetBrowserView.windowID) }
                .keyboardShortcut("b", modifiers: .command)
            Button("Next Random Preset") { model.shuffleNext() }
                .keyboardShortcut("r", modifiers: .command)
        }
        CommandGroup(replacing: .help) {
            Link("MScopes Website", destination: URL(string: "https://mscopes.com")!)
            Link("Getting Presets", destination: URL(string: "https://mscopes.com/presets")!)
        }
    }
}
