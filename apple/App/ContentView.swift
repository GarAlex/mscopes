//
//  ContentView.swift
//  Layout: the visualization fills the window; a sidebar controls it.
//  The sidebar's sections live in their own views (SidebarView.swift and
//  the *Section.swift files); the preset browser is a separate window.
//
import SwiftUI

struct ContentView: View {
    @EnvironmentObject var model: EngineModel
    @State private var isFullscreen = false

    var body: some View {
        HSplitView {
            VisualizerView(engine: model.engine)
                .frame(minWidth: 480, minHeight: 320)
                .background(Color.black)

            if !isFullscreen {
                SidebarView(onFullscreen: enterFullscreen)
                    .frame(width: 260)
            }
        }
        .frame(minWidth: 480, minHeight: 320)
        .onAppear { model.start() }
        .onDisappear { model.stop() }
        // Leaving macOS fullscreen (Esc or the green button) restores the controls.
        .onReceive(NotificationCenter.default.publisher(
            for: NSWindow.willExitFullScreenNotification)) { _ in
            isFullscreen = false
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
