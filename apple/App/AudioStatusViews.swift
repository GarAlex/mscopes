//
//  AudioStatusViews.swift
//  What the visual area says about audio, when it has something to say:
//  the first-launch explanation before macOS asks for permission, capture
//  failing, and nothing heard yet. Shown over the visuals, so they also
//  appear in widget mode and full screen.
//
import SwiftUI

/// Shown once, on first launch, instead of starting capture straight away:
/// the system permission prompt then follows a sentence that explains it.
struct AudioIntroView: View {
    @EnvironmentObject var model: EngineModel
    var compact: Bool

    var body: some View {
        StatusCard(compact: compact) {
            Image(systemName: "waveform")
                .font(.system(size: compact ? 20 : 34, weight: .light))
            Text("MScopes paints whatever your Mac is playing")
                .font(compact ? .headline : .title3.weight(.semibold))
            if !compact {
                Text("To hear it, MScopes needs permission to record system audio. macOS asks once. The sound is analysed in memory to drive the picture; it is never recorded, saved or sent anywhere.")
                    .font(.callout)
                    .foregroundStyle(.secondary)
            }
            Button("Start Listening") { model.start() }
                .keyboardShortcut(.defaultAction)
                .controlSize(compact ? .small : .large)
        }
    }
}

/// Capture couldn't start (most likely the permission is off).
struct AudioProblemView: View {
    @EnvironmentObject var model: EngineModel
    var message: String
    var compact: Bool

    var body: some View {
        StatusCard(compact: compact) {
            Image(systemName: "speaker.slash")
                .font(.system(size: compact ? 18 : 30, weight: .light))
            Text("MScopes can't hear your Mac")
                .font(compact ? .headline : .title3.weight(.semibold))
            if !compact {
                Text("Allow MScopes under Privacy & Security ▸ Screen & System Audio Recording, then try again.")
                    .font(.callout)
                    .foregroundStyle(.secondary)
                Text(message)
                    .font(.caption)
                    .foregroundStyle(.tertiary)
                    .lineLimit(2)
            }
            HStack {
                Button("Open Privacy Settings") { model.openAudioPrivacySettings() }
                Button("Try Again") { model.start() }
                    .keyboardShortcut(.defaultAction)
            }
            .controlSize(compact ? .small : .regular)
        }
    }
}

/// Listening, but nothing heard since capture started.
struct ListenHintView: View {
    @EnvironmentObject var model: EngineModel
    var compact: Bool

    var body: some View {
        VStack(spacing: 6) {
            Text("Play some music: MScopes shows whatever your Mac plays.")
                .font(compact ? .caption : .callout)
            if !compact {
                Button("Playing, but nothing moves? Check the audio permission") {
                    model.openAudioPrivacySettings()
                }
                .buttonStyle(.link)
                .font(.caption)
            }
        }
        .multilineTextAlignment(.center)
        .padding(.horizontal, compact ? 10 : 16)
        .padding(.vertical, compact ? 6 : 10)
        .background(.ultraThinMaterial, in: RoundedRectangle(cornerRadius: 10))
        .environment(\.colorScheme, .dark)
        .padding(.bottom, compact ? 10 : 28)
        .transition(.opacity)
    }
}

/// A centered translucent card over the visuals.
private struct StatusCard<Content: View>: View {
    var compact: Bool
    @ViewBuilder var content: Content

    var body: some View {
        VStack(spacing: compact ? 8 : 14) { content }
            .multilineTextAlignment(.center)
            .frame(maxWidth: compact ? 260 : 420)
            .padding(compact ? 12 : 26)
            .background(.ultraThinMaterial, in: RoundedRectangle(cornerRadius: 16))
            .environment(\.colorScheme, .dark)
            .padding(16)
    }
}
