//
//  SidebarView.swift
//  The control sidebar: a scrolling stack of independent sections. Each
//  section is its own view so a change in one doesn't re-evaluate the rest
//  (and so the file that used to hold all of them stops growing).
//
import SwiftUI

struct SidebarView: View {
    @EnvironmentObject var model: EngineModel

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 16) {
                AudioSection()
                MusicPluginSection()
                LevelsView(levels: model.levels)
                ControlsSection()
                PresetSection()
                AlbumsSection()
                MacroKnobsSection()
                EffectStackSection()
                ParametersSection()
            }
            .padding(14)
        }
    }
}

/// Capture status + start/stop.
struct AudioSection: View {
    @EnvironmentObject var model: EngineModel

    var body: some View {
        GroupBox("Audio") {
            VStack(alignment: .leading, spacing: 8) {
                HStack {
                    Circle().fill(model.capturing ? .green : .red).frame(width: 9, height: 9)
                    Text(model.capturing
                         ? "Capturing · \(Int(model.sampleRate)) Hz · \(model.channels) ch"
                         : "Not capturing")
                        .font(.caption).foregroundStyle(.secondary)
                }
                if let e = model.lastError {
                    Text(e).font(.caption2).foregroundStyle(.red).lineLimit(3)
                }
                HStack {
                    Button(model.capturing ? "Stop" : "Start") {
                        model.capturing ? model.stop() : model.start()
                    }
                    CallbackCountLabel(levels: model.levels)
                }
            }.frame(maxWidth: .infinity, alignment: .leading)
        }
    }
}

/// Sensitivity, aspect mode, HUD.
struct ControlsSection: View {
    @EnvironmentObject var model: EngineModel

    var body: some View {
        GroupBox("Controls") {
            VStack(alignment: .leading, spacing: 6) {
                Text("Sensitivity \(String(format: "%.2f", model.sensitivity))×").font(.caption)
                Slider(value: $model.sensitivity, in: 0.25...4.0)
                Text("Aspect").font(.caption)
                Picker("", selection: $model.aspectMode) {
                    Text("Stretch").tag(0)   // classic AVS: shapes follow the window
                    Text("Fill").tag(1)      // round shapes, crops the short axis
                    Text("Fit").tag(2)       // round shapes, fits inside
                }
                .pickerStyle(.segmented)
                .labelsHidden()
                Toggle("In-view HUD", isOn: $model.showHUD).font(.caption)
            }
        }
    }
}

/// Live sliders over reg00-07. Bind any effect param to a reg (right-click it
/// in Parameters) and these perform it.
struct MacroKnobsSection: View {
    @EnvironmentObject var model: EngineModel

    var body: some View {
        GroupBox("Macro Knobs (reg00–07)") {
            VStack(spacing: 3) {
                ForEach(0..<8, id: \.self) { i in
                    HStack(spacing: 6) {
                        Text(String(format: "r%02d", i)).font(.caption2)
                            .monospacedDigit().foregroundStyle(.secondary)
                        Slider(value: Binding(
                            get: { Double(model.macroRegs[i]) },
                            set: { model.setMacroReg(i, Float($0)) }
                        ), in: 0...1)
                    }
                }
            }.frame(maxWidth: .infinity)
        }
    }
}

/// One-click install of the bundled Music visual plugin.
struct MusicPluginSection: View {
    @EnvironmentObject var model: EngineModel

    var body: some View {
        GroupBox("Music Plugin") {
            VStack(alignment: .leading, spacing: 6) {
                HStack {
                    Circle().fill(model.musicPluginInstalled ? .green : .gray).frame(width: 9, height: 9)
                    Text(model.musicPluginInstalled ? "Installed for Music" : "Not installed")
                        .font(.caption).foregroundStyle(.secondary)
                }
                HStack {
                    Button(model.musicPluginInstalled ? "Reinstall" : "Install…") { model.installMusicPlugin() }
                    if model.musicPluginInstalled {
                        Button("Remove") { model.uninstallMusicPlugin() }
                    }
                }
                .font(.caption)
                Text("Copies the plugin into ~/Library/iTunes/iTunes Plug-ins. Then reopen Music and pick View ▸ Visualizer ▸ MScopes.")
                    .font(.caption2).foregroundStyle(.secondary)
            }.frame(maxWidth: .infinity, alignment: .leading)
        }
    }
}
