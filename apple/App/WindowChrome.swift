//
//  WindowChrome.swift
//  The window's look and its two modes, applied to the AppKit NSWindow that
//  SwiftUI creates for us:
//
//  - Normal: no system title bar. The visualization runs edge to edge and
//    our own HeaderBar (HeaderBar.swift) sits over its top; the standard
//    close/minimize/zoom buttons stay where they always are.
//  - Widget: a small always-on-top window with nothing but visuals — no
//    header, no traffic lights, resizable from its edges, draggable from
//    anywhere, visible on every Space (and beside fullscreen apps). Frames
//    for both modes are remembered separately.
//
import SwiftUI
import AppKit

/// Put this in the content's `.background`. It finds the NSWindow and keeps
/// its chrome in sync with `widgetMode`.
struct WindowChrome: NSViewRepresentable {
    var widgetMode: Bool

    func makeCoordinator() -> WindowChromeController { WindowChromeController() }

    func makeNSView(context: Context) -> NSView {
        let v = ProbeView()
        v.controller = context.coordinator
        return v
    }

    func updateNSView(_ v: NSView, context: Context) {
        context.coordinator.widgetMode = widgetMode
        if let w = v.window { context.coordinator.apply(to: w) }
    }

    /// Invisible, never hit-tested; only there to learn which window we are in.
    final class ProbeView: NSView {
        weak var controller: WindowChromeController?
        override func hitTest(_ point: NSPoint) -> NSView? { nil }
        override func viewDidMoveToWindow() {
            super.viewDidMoveToWindow()
            if let w = window { controller?.apply(to: w) }
        }
    }
}

final class WindowChromeController {
    var widgetMode = false
    private weak var window: NSWindow?
    private var appliedMode: Bool?
    private var observers: [NSObjectProtocol] = []

    private static let normalFrameKey = "chrome.normalFrame"
    private static let widgetFrameKey = "chrome.widgetFrame"
    private static let widgetDefaultSize = NSSize(width: 384, height: 216)

    deinit { observers.forEach(NotificationCenter.default.removeObserver) }

    func apply(to w: NSWindow) {
        if window !== w {
            window = w
            observe(w)
            // Chrome shared by both modes.
            w.titleVisibility = .hidden
            w.titlebarAppearsTransparent = true
            w.styleMask.insert(.fullSizeContentView)
            w.backgroundColor = .black
            w.isOpaque = true
            w.tabbingMode = .disallowed
        }
        guard appliedMode != widgetMode else { return }
        let transition = appliedMode != nil   // false on first apply after launch
        appliedMode = widgetMode
        if widgetMode { enterWidget(w, transition: transition) }
        else          { leaveWidget(w, transition: transition) }
    }

    // MARK: - modes

    private func enterWidget(_ w: NSWindow, transition: Bool) {
        if transition { save(w.frame, key: Self.normalFrameKey) }
        w.level = .floating
        w.collectionBehavior = [.canJoinAllSpaces, .fullScreenAuxiliary]
        w.isMovableByWindowBackground = true
        setButtons(hidden: true, in: w)
        let target = load(key: Self.widgetFrameKey) ?? defaultWidgetFrame(for: w)
        // After SwiftUI's own pass: dropping the sidebar makes it re-fit the
        // window to the content's ideal size, which would undo a frame set
        // right here. Doing it next turn wins, before anything is drawn.
        DispatchQueue.main.async { [weak w] in
            w?.setFrame(target, display: true, animate: transition)
        }
    }

    private func leaveWidget(_ w: NSWindow, transition: Bool) {
        if transition { save(w.frame, key: Self.widgetFrameKey) }
        w.level = .normal
        w.collectionBehavior = []
        w.isMovableByWindowBackground = false
        setButtons(hidden: false, in: w)
        if transition, let f = load(key: Self.normalFrameKey) {
            DispatchQueue.main.async { [weak w] in
                w?.setFrame(f, display: true, animate: true)
            }
        }
    }

    private func setButtons(hidden: Bool, in w: NSWindow) {
        for b in [NSWindow.ButtonType.closeButton, .miniaturizeButton, .zoomButton] {
            w.standardWindowButton(b)?.isHidden = hidden
        }
    }

    /// Bottom-right corner of the window's screen, a little in from the edge.
    private func defaultWidgetFrame(for w: NSWindow) -> NSRect {
        let screen = (w.screen ?? NSScreen.main)?.visibleFrame
            ?? NSRect(x: 0, y: 0, width: 1440, height: 900)
        let s = Self.widgetDefaultSize
        return NSRect(x: screen.maxX - s.width - 24, y: screen.minY + 24,
                      width: s.width, height: s.height)
    }

    // MARK: - remembering frames

    /// Keep the frame of whichever mode is live up to date as the user moves
    /// and resizes, so a relaunch (or the next toggle) lands where they left it.
    private func observe(_ w: NSWindow) {
        observers.forEach(NotificationCenter.default.removeObserver)
        observers = [NSWindow.didMoveNotification, NSWindow.didEndLiveResizeNotification].map { name in
            NotificationCenter.default.addObserver(forName: name, object: w, queue: .main) { [weak self] _ in
                guard let self, let w = self.window, self.appliedMode != nil,
                      !w.styleMask.contains(.fullScreen) else { return }
                self.save(w.frame, key: self.widgetMode ? Self.widgetFrameKey : Self.normalFrameKey)
            }
        }
    }

    private func save(_ f: NSRect, key: String) {
        UserDefaults.standard.set(NSStringFromRect(f), forKey: key)
    }

    /// A saved frame, if it still lands on a connected screen.
    private func load(key: String) -> NSRect? {
        guard let s = UserDefaults.standard.string(forKey: key) else { return nil }
        let f = NSRectFromString(s)
        guard f.width >= 120, f.height >= 68,
              NSScreen.screens.contains(where: { $0.visibleFrame.intersects(f) }) else { return nil }
        return f
    }
}

/// A transparent strip that moves the window when dragged and zooms it on
/// double-click — what the system title bar did, for our own header.
struct WindowDragArea: NSViewRepresentable {
    func makeNSView(context: Context) -> NSView { DragView() }
    func updateNSView(_ nsView: NSView, context: Context) {}

    final class DragView: NSView {
        override func mouseDown(with event: NSEvent) {
            if event.clickCount == 2 { window?.zoom(nil); return }
            window?.performDrag(with: event)
        }
    }
}
