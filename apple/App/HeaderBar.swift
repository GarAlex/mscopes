//
//  HeaderBar.swift
//  Our replacement for the window's title bar: a translucent strip over the
//  top of the visualization with the wordmark, the current preset, and the
//  window-level controls. The area left of the wordmark is where the system
//  close/minimize/zoom buttons sit; the whole strip drags the window.
//
import SwiftUI

struct HeaderBar: View {
    @EnvironmentObject var model: EngineModel
    let onFullscreen: () -> Void

    static let height: CGFloat = 38
    /// Room for the traffic lights, which AppKit draws over this strip.
    private let trafficLightInset: CGFloat = 78

    var body: some View {
        ZStack {
            WindowDragArea()
            HStack(spacing: 12) {
                Spacer().frame(width: trafficLightInset)
                Wordmark()
                if !model.currentPresetTitle.isEmpty {
                    Text(model.currentPresetTitle)
                        .font(.system(size: 12, weight: .medium))
                        .foregroundStyle(.white.opacity(0.65))
                        .lineLimit(1)
                        .truncationMode(.middle)
                        .padding(.top, 1)
                }
                Spacer(minLength: 8)
                HeaderButton(symbol: model.showSidebar ? "sidebar.trailing" : "sidebar.leading",
                             help: model.showSidebar ? "Hide the sidebar (⌃⌘S)" : "Show the sidebar (⌃⌘S)") {
                    model.showSidebar.toggle()
                }
                HeaderButton(symbol: "pip",
                             help: "Widget mode: a small always-on-top window with just the visuals (⌘⇧W)") {
                    model.widgetMode = true
                }
                HeaderButton(symbol: "arrow.up.left.and.arrow.down.right",
                             help: "Fullscreen — press Esc to come back") {
                    onFullscreen()
                }
            }
            .padding(.trailing, 10)
        }
        .frame(height: Self.height)
        .background(
            LinearGradient(colors: [.black.opacity(0.82), .black.opacity(0.35), .clear],
                           startPoint: .top, endPoint: .bottom)
        )
    }
}

/// "MScopes" the way the app draws it: heavy rounded type with a neon sweep.
struct Wordmark: View {
    var body: some View {
        Text("MScopes")
            .font(.system(size: 15, weight: .heavy, design: .rounded))
            .foregroundStyle(
                LinearGradient(colors: [Color(red: 0.45, green: 0.95, blue: 1.0),
                                        Color(red: 0.75, green: 0.55, blue: 1.0),
                                        Color(red: 1.0, green: 0.45, blue: 0.75)],
                               startPoint: .leading, endPoint: .trailing)
            )
            .shadow(color: .cyan.opacity(0.35), radius: 6)
            .fixedSize()
    }
}

/// A small round icon button that reads over any visual.
struct HeaderButton: View {
    let symbol: String
    let help: String
    let action: () -> Void
    @State private var hovering = false

    var body: some View {
        Button(action: action) {
            Image(systemName: symbol)
                .font(.system(size: 12, weight: .semibold))
                .foregroundStyle(.white.opacity(hovering ? 1 : 0.8))
                .frame(width: 26, height: 26)
                .background(Circle().fill(.white.opacity(hovering ? 0.18 : 0.08)))
                .contentShape(Circle())
        }
        .buttonStyle(.plain)
        .onHover { hovering = $0 }
        .help(help)
    }
}

/// Hover-only controls for widget mode: leave it, or close the window.
struct WidgetControls: View {
    @EnvironmentObject var model: EngineModel

    var body: some View {
        HStack(spacing: 6) {
            HeaderButton(symbol: "arrow.down.right.and.arrow.up.left",
                         help: "Back to the full window (⌘⇧W)") {
                model.widgetMode = false
            }
            HeaderButton(symbol: "xmark", help: "Close (⌘W)") {
                NSApp.keyWindow?.close()
            }
        }
        .padding(6)
        .background(Capsule().fill(.black.opacity(0.55)))
        .padding(8)
    }
}
