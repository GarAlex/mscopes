//
//  ContentView.swift
//  Layout: the visualization fills the window under our own header bar; a
//  sidebar controls it. Widget mode drops both and leaves just the visuals in
//  a small floating window (see WindowChrome.swift for the AppKit side).
//  The sidebar's sections live in their own views (SidebarView.swift and
//  the *Section.swift files); the preset browser is a separate window.
//
import SwiftUI

struct ContentView: View {
    @EnvironmentObject var model: EngineModel
    @State private var isFullscreen = false
    @State private var hoveringVisual = false

    private var widget: Bool { model.widgetMode }
    private var minSize: CGSize { widget ? CGSize(width: 160, height: 90) : CGSize(width: 480, height: 320) }

    var body: some View {
        HSplitView {
            visual
            if model.showSidebar && !isFullscreen && !widget {
                SidebarView()
                    .frame(width: 260)
                    // The window itself is black (for the visuals); the
                    // sidebar keeps the system window colour so its controls
                    // read in light and dark appearance alike.
                    .background(Color(nsColor: .windowBackgroundColor))
            }
        }
        .frame(minWidth: minSize.width, minHeight: minSize.height)
        .background(WindowChrome(widgetMode: widget))
        .onAppear { model.start() }
        .onDisappear { model.stop() }
        // Leaving macOS fullscreen (Esc or the green button) restores the controls.
        .onReceive(NotificationCenter.default.publisher(
            for: NSWindow.willExitFullScreenNotification)) { _ in
            isFullscreen = false
        }
    }

    private var visual: some View {
        ZStack(alignment: .top) {
            VisualizerView(engine: model.engine)
            if !isFullscreen && !widget {
                HeaderBar(onFullscreen: enterFullscreen)
            }
        }
        .frame(minWidth: minSize.width, minHeight: minSize.height)
        .background(Color.black)
        // Run under the (hidden) title bar too: the header replaces it and
        // must line up with the traffic lights, which live in that strip.
        .ignoresSafeArea()
        .overlay(alignment: .topTrailing) {
            if widget {
                WidgetControls()
                    .opacity(hoveringVisual ? 1 : 0)
                    .animation(.easeInOut(duration: 0.15), value: hoveringVisual)
            }
        }
        .onHover { hoveringVisual = $0 }
        .contextMenu {
            Button("Next Random Preset") { model.shuffleNext() }
            Divider()
            if widget {
                Button("Leave Widget Mode") { model.widgetMode = false }
            } else {
                Button("Widget Mode") { model.widgetMode = true }
                Button(model.showSidebar ? "Hide Sidebar" : "Show Sidebar") { model.showSidebar.toggle() }
                Button("Fullscreen") { enterFullscreen() }
            }
        }
    }

    private func enterFullscreen() {
        isFullscreen = true
        NSCursor.setHiddenUntilMouseMoves(true)
        if let window = NSApp.keyWindow ?? NSApp.mainWindow,
           !window.styleMask.contains(.fullScreen) {
            window.toggleFullScreen(nil)
        }
    }
}
