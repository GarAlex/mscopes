//
//  PresetSection.swift
//  Built-in preset picker, historical menu, shuffle, open/save, and the door
//  to the Preset Browser window.
//
import SwiftUI

struct PresetSection: View {
    @EnvironmentObject var model: EngineModel
    @Environment(\.openWindow) private var openWindow

    var body: some View {
        GroupBox("Preset") {
            VStack(alignment: .leading, spacing: 6) {
                Picker("", selection: $model.currentPreset) {
                    Section("Classic style") {
                        ForEach(model.presetNames.filter { !$0.modern }) { p in
                            Text(p.name).tag(p.id)
                        }
                    }
                    Section("Modern") {
                        ForEach(model.presetNames.filter { $0.modern }) { p in
                            Text(p.name).tag(p.id)
                        }
                    }
                }
                .pickerStyle(.menu)
                .labelsHidden()

                Button {
                    openWindow(id: PresetBrowserView.windowID)
                } label: {
                    Label("Preset Browser…", systemImage: "list.bullet.rectangle")
                }
                .font(.caption)
                .help("Browse built-in, historical, library and album presets (⌘B)")

                HStack {
                    Toggle("Shuffle", isOn: $model.shuffleOn).font(.caption)
                    Picker("", selection: $model.shuffleInterval) {
                        Text("15s").tag(15.0)
                        Text("30s").tag(30.0)
                        Text("1m").tag(60.0)
                        Text("2m").tag(120.0)
                    }
                    .pickerStyle(.menu).labelsHidden().font(.caption)
                    .disabled(!model.shuffleOn)
                }
                HStack {
                    Button {
                        model.openAvsPreset()
                    } label: {
                        Label("Open…", systemImage: "folder")
                    }
                    Button {
                        model.savePreset()
                    } label: {
                        Label("Save…", systemImage: "square.and.arrow.down")
                    }
                }
                .font(.caption)
                if let report = model.avsLoadReport {
                    Text(report).font(.caption2).foregroundStyle(.secondary).lineLimit(4)
                }
            }.frame(maxWidth: .infinity, alignment: .leading)
        }
    }
}
