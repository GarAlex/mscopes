//
//  PictureView.swift
//  The Picture controls — one screen for looks and quality. Shown as a
//  sidebar section and as its own window (View ▸ Picture…, the header's
//  filters button), so it also works in widget mode and full screen.
//
import SwiftUI

struct PictureControls: View {
    @ObservedObject var picture: PictureModel

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            HStack {
                Picker("Look", selection: $picture.look) {
                    ForEach(Array(picture.lookNames.enumerated()), id: \.offset) { i, name in
                        Text(name).tag(i)
                    }
                    if let custom = picture.customLookName {
                        Divider()
                        Text(custom).tag(-1)
                    }
                }
                .pickerStyle(.menu)
                Button {
                    picture.chooseCustomLook()
                } label: {
                    Image(systemName: "folder")
                }
                .help("Load a .cube look from a photo or video tool")
            }
            if let err = picture.lastError {
                Text(err).font(.caption2).foregroundStyle(.red).lineLimit(2)
            }
            PictureSlider(title: "Strength", value: $picture.strength)
                .disabled(picture.lookIsOff)

            PictureSlider(title: "Glow", value: $picture.glow)
            PictureSlider(title: "Grain", value: $picture.grain)
            PictureSlider(title: "Vignette", value: $picture.vignette)
            PictureSlider(title: "Scanlines", value: $picture.scanlines)
            Toggle("React to beat", isOn: $picture.reactToBeat)
                .font(.caption)
                .help("Glow and grain breathe slightly with the music")

            Divider().padding(.vertical, 2)
            Text("Quality").font(.caption).foregroundStyle(.secondary)
            Toggle("Sharp scaling", isOn: $picture.sharpScaling)
                .font(.caption)
                .help("Crisper upscaling when the picture is larger than the render size")

            HStack {
                Spacer()
                Button("Reset") { picture.reset() }
                    .font(.caption)
            }
        }
    }
}

private struct PictureSlider: View {
    let title: String
    @Binding var value: Double

    var body: some View {
        HStack(spacing: 8) {
            Text(title).font(.caption).frame(width: 64, alignment: .leading)
            Slider(value: $value, in: 0...1)
            Text("\(Int((value * 100).rounded()))")
                .font(.caption2).monospacedDigit().foregroundStyle(.secondary)
                .frame(width: 26, alignment: .trailing)
        }
    }
}

/// Sidebar section.
struct PictureSection: View {
    @EnvironmentObject var model: EngineModel

    var body: some View {
        GroupBox("Picture") {
            PictureControls(picture: model.picture)
                .frame(maxWidth: .infinity, alignment: .leading)
        }
    }
}

/// The Picture window.
struct PictureWindow: View {
    static let windowID = "picture"
    @EnvironmentObject var model: EngineModel

    var body: some View {
        PictureControls(picture: model.picture)
            .padding(16)
            .frame(width: 320)
    }
}

/// Look submenu for the visual's right-click menu.
struct LookMenu: View {
    @ObservedObject var picture: PictureModel

    var body: some View {
        Menu("Look") {
            ForEach(Array(picture.lookNames.enumerated()), id: \.offset) { i, name in
                Button {
                    picture.look = i
                } label: {
                    if picture.look == i { Label(name, systemImage: "checkmark") } else { Text(name) }
                }
            }
            if let custom = picture.customLookName {
                Divider()
                Button {
                    picture.look = -1
                } label: {
                    if picture.look == -1 { Label(custom, systemImage: "checkmark") } else { Text(custom) }
                }
            }
            Divider()
            Button("Load .cube Look…") { picture.chooseCustomLook() }
        }
    }
}
