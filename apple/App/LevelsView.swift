//
//  LevelsView.swift
//  The fast-changing audio readouts (meters, beat, BPM, callback count).
//
//  These live on their own ObservableObject (LevelsModel) on purpose: they
//  update 15×/s, and when they were @Published on EngineModel every update
//  re-evaluated the whole sidebar — ~14 ms on the main thread each time,
//  which starved the render tick and pinned the app at ~45 fps. Observing
//  them here confines each update to this small view.
//
import SwiftUI

@MainActor
final class LevelsModel: ObservableObject {
    @Published var peak: Float = 0
    @Published var bass: Float = 0
    @Published var mid: Float = 0
    @Published var treble: Float = 0
    @Published var bpm: Float = 0
    @Published var beatLevel: Float = 0
    @Published var audioCallbacks: UInt64 = 0
    @Published var frames: UInt64 = 0
}

struct LevelsView: View {
    @ObservedObject var levels: LevelsModel

    var body: some View {
        GroupBox("Levels") {
            VStack(spacing: 6) {
                meter("peak", levels.peak)
                meter("bass", levels.bass)
                meter("mid", levels.mid)
                meter("treble", levels.treble)
                HStack(spacing: 8) {
                    Text("beat").font(.caption2).frame(width: 44, alignment: .leading)
                        .foregroundStyle(.secondary)
                    Circle().fill(.tint)
                        .frame(width: 10, height: 10)
                        .opacity(0.15 + 0.85 * Double(levels.beatLevel))
                    Text(levels.bpm > 0 ? String(format: "%.0f BPM", levels.bpm) : "— BPM")
                        .font(.caption2.monospacedDigit())
                        .foregroundStyle(.secondary)
                    Spacer()
                }
            }
        }
    }

    private func meter(_ label: String, _ v: Float) -> some View {
        HStack(spacing: 8) {
            Text(label).font(.caption2).frame(width: 44, alignment: .leading).foregroundStyle(.secondary)
            GeometryReader { geo in
                ZStack(alignment: .leading) {
                    RoundedRectangle(cornerRadius: 2).fill(.quaternary)
                    RoundedRectangle(cornerRadius: 2)
                        .fill(.tint)
                        .frame(width: geo.size.width * CGFloat(min(max(v, 0), 1)))
                }
            }.frame(height: 8)
        }
    }
}

/// Audio-callback counter for the Audio box (a liveness diagnostic).
struct CallbackCountLabel: View {
    @ObservedObject var levels: LevelsModel
    var body: some View {
        Text("cbs \(levels.audioCallbacks) · frames \(levels.frames)")
            .font(.caption2).foregroundStyle(.secondary).monospacedDigit()
            .help("Audio callbacks and rendered frames since capture started — both should keep climbing. Log: ~/Library/Logs/MScopes/engine.log")
    }
}
