//
//  PresetBrowserView.swift
//  The Preset Browser window: one searchable list over every preset the app
//  knows — the built-in stacks, every .avs file under the library folder
//  (with its pack as subtitle), and the numbered albums. Selecting a row loads it, so the arrow keys walk a
//  library of thousands with the visuals following along.
//
import SwiftUI

enum BrowserSource: String, CaseIterable, Identifiable {
    case builtin, library, albums
    var id: String { rawValue }
    var title: String {
        switch self {
        case .builtin: return "Built-in"
        case .library: return "Library"
        case .albums:  return "Albums"
        }
    }
}

struct PresetBrowserView: View {
    static let windowID = "presetBrowser"

    @EnvironmentObject var model: EngineModel
    @State private var search = ""
    @State private var selection: String?

    private struct Row: Identifiable {
        enum Payload { case builtin(Int), avs(URL), album(UUID) }
        let id: String
        let name: String
        let subtitle: String
        let payload: Payload
    }

    private var rows: [Row] {
        let all: [Row]
        switch model.browserSource {
        case .builtin:
            all = model.presetNames.map {
                Row(id: "b:\($0.id)", name: $0.name,
                    subtitle: $0.modern ? "Modern" : "Classic style", payload: .builtin($0.id))
            }
        case .library:
            all = model.libraryPresets.map {
                Row(id: "l:\($0.id)", name: $0.name, subtitle: $0.pack, payload: .avs($0.url))
            }
        case .albums:
            all = model.albums.map {
                Row(id: "a:\($0.id.uuidString)", name: $0.name,
                    subtitle: "\($0.tracks.count) tracks", payload: .album($0.id))
            }
        }
        guard !search.isEmpty else { return all }
        return all.filter {
            $0.name.localizedCaseInsensitiveContains(search)
            || $0.subtitle.localizedCaseInsensitiveContains(search)
        }
    }

    var body: some View {
        VStack(spacing: 8) {
            HStack {
                Picker("", selection: $model.browserSource) {
                    ForEach(BrowserSource.allCases) { Text($0.title).tag($0) }
                }
                .pickerStyle(.segmented)
                .labelsHidden()
                .frame(maxWidth: 300)
                TextField("Search…", text: $search)
                    .textFieldStyle(.roundedBorder)
            }

            if model.browserSource == .library && model.albumRootPath == nil {
                libraryPrompt
            } else if model.browserSource == .albums && model.albums.isEmpty {
                model.albumRootPath == nil ? AnyView(libraryPrompt) : AnyView(emptyNote("No numbered albums found under the library folder."))
            } else {
                let visible = rows
                List(visible, selection: $selection) { row in
                    VStack(alignment: .leading, spacing: 1) {
                        Text(row.name).lineLimit(1)
                        Text(row.subtitle).font(.caption).foregroundStyle(.secondary).lineLimit(1)
                    }
                    .padding(.vertical, 1)
                }
                .listStyle(.inset)
                .onChange(of: selection) { _, new in
                    guard let new, let row = visible.first(where: { $0.id == new }) else { return }
                    load(row)
                }
            }

            HStack {
                Text(footer).font(.caption).foregroundStyle(.secondary)
                if model.albumsScanning { ProgressView().controlSize(.small) }
                Spacer()
                if model.albumRootPath != nil {
                    Button("Change Library Folder…") { model.chooseLibraryRoot() }.font(.caption)
                }
                Picker("Shuffle over", selection: $model.shuffleScope) {
                    Text("Built-in").tag(EngineModel.ShuffleScope.builtin)
                    Text("Library").tag(EngineModel.ShuffleScope.library)
                        .disabled(model.libraryPresets.isEmpty)
                }
                .font(.caption)
                .frame(maxWidth: 260)
            }
        }
        .padding(12)
        .frame(minWidth: 560, minHeight: 380)
        .onChange(of: model.browserSource) { _, _ in selection = nil }
    }

    private var footer: String {
        let n = rows.count
        switch model.browserSource {
        case .builtin:    return "\(n) built-in presets"
        case .library:    return search.isEmpty ? "\(n) presets in the library" : "\(n) of \(model.libraryPresets.count) presets"
        case .albums:     return "\(n) albums"
        }
    }

    private var libraryPrompt: some View {
        VStack(spacing: 10) {
            Spacer()
            Image(systemName: "folder.badge.plus").font(.system(size: 36)).foregroundStyle(.secondary)
            Text("Choose a folder of .avs presets to browse it here — every file underneath is listed, and numbered sequences become albums.")
                .font(.callout).multilineTextAlignment(.center).foregroundStyle(.secondary)
                .frame(maxWidth: 380)
            Button("Choose Library Folder…") { model.chooseLibraryRoot() }
            Text("Presets come from your own Winamp install (Plugins/avs) or the community archives — see docs/PRESETS.md in the source tree.")
                .font(.caption).multilineTextAlignment(.center).foregroundStyle(.tertiary)
                .frame(maxWidth: 380)
            Spacer()
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
    }

    private func emptyNote(_ s: String) -> some View {
        VStack { Spacer(); Text(s).foregroundStyle(.secondary); Spacer() }
            .frame(maxWidth: .infinity, maxHeight: .infinity)
    }

    private func load(_ row: Row) {
        switch row.payload {
        case .builtin(let i): model.currentPreset = i
        case .avs(let url):   model.loadAvsFile(url)
        case .album(let id):
            if let album = model.albums.first(where: { $0.id == id }) { model.playAlbum(album) }
        }
    }
}
