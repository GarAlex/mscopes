//
//  AlbumsSection.swift
//  Numbered preset sequences meant to play in order (real AVS never clears
//  between preset switches — these packs compose whole shows around that).
//  Point at a library folder once; browse albums in the Preset Browser and
//  drive playback from here.
//
import SwiftUI

struct AlbumsSection: View {
    @EnvironmentObject var model: EngineModel
    @Environment(\.openWindow) private var openWindow

    var body: some View {
        GroupBox("Albums") {
            VStack(alignment: .leading, spacing: 6) {
                HStack {
                    Button {
                        model.chooseLibraryRoot()
                    } label: {
                        Label(model.albumRootPath == nil ? "Choose Library Folder…" : "Change Folder…",
                              systemImage: "folder.badge.gearshape")
                    }
                    if model.albumsScanning {
                        ProgressView().controlSize(.small)
                    }
                }
                .font(.caption)
                if let root = model.albumRootPath {
                    Text(root).font(.caption2).foregroundStyle(.secondary).lineLimit(1)
                }
                if !model.albums.isEmpty {
                    Button {
                        model.browserSource = .albums
                        openWindow(id: PresetBrowserView.windowID)
                    } label: {
                        Label("Browse \(model.albums.count) Albums (\(model.albums.reduce(0) { $0 + $1.tracks.count }) tracks)…",
                              systemImage: "square.stack")
                    }
                    .font(.caption)
                }
                if let album = model.currentAlbum {
                    Divider()
                    Text(album.name).font(.caption).fontWeight(.semibold).lineLimit(1)
                    Text("Track \(model.currentTrackIndex + 1)/\(album.tracks.count): "
                         + album.tracks[model.currentTrackIndex].title)
                        .font(.caption2).foregroundStyle(.secondary).lineLimit(2)
                    HStack {
                        Button { model.prevAlbumTrack() } label: {
                            Image(systemName: "backward.end.fill")
                        }.disabled(model.currentTrackIndex == 0)
                        Button { model.nextAlbumTrack() } label: {
                            Image(systemName: "forward.end.fill")
                        }.disabled(model.currentTrackIndex + 1 >= album.tracks.count)
                        Toggle("Auto", isOn: $model.albumAutoAdvance).font(.caption2)
                        Picker("", selection: $model.albumAutoAdvanceInterval) {
                            Text("30s").tag(30.0)
                            Text("45s").tag(45.0)
                            Text("1m").tag(60.0)
                            Text("2m").tag(120.0)
                        }
                        .pickerStyle(.menu).labelsHidden().font(.caption2)
                        .disabled(!model.albumAutoAdvance)
                    }
                    .buttonStyle(.borderless)
                }
            }.frame(maxWidth: .infinity, alignment: .leading)
        }
    }
}
