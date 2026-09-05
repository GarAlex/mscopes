//
//  VisualizerView.swift
//  Hosts the engine's AppKit render view inside SwiftUI.
//
import SwiftUI

struct VisualizerView: NSViewRepresentable {
    let engine: VizEngine

    func makeNSView(context: Context) -> NSView {
        engine.renderView
    }

    func updateNSView(_ nsView: NSView, context: Context) {
        // The engine drives its own redraw loop; nothing to push here.
    }
}
