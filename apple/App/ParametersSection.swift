//
//  ParametersSection.swift
//  Parameters of the selected effect (click an effect name in the stack):
//  live sliders, register binding via the context menu, and the EEL script
//  editor for scripted hosts.
//
import SwiftUI

struct ParametersSection: View {
    @EnvironmentObject var model: EngineModel
    @State private var editingScripts = false
    @State private var draft = ["", "", "", ""]

    var body: some View {
        if let sel = model.selectedEffect, sel < model.effects.count, !model.params.isEmpty {
            GroupBox("Parameters — \(model.effects[sel].name)") {
                VStack(alignment: .leading, spacing: 6) {
                    ForEach(model.params) { p in
                        paramRow(p)
                    }
                }.frame(maxWidth: .infinity, alignment: .leading)
            }
            if model.scripts != nil {
                Button {
                    var d = model.scripts ?? []
                    while d.count < 4 { d.append("") }      // editor binds draft[0..3]
                    draft = d
                    editingScripts = true
                } label: {
                    Label("Edit Scripts…", systemImage: "chevron.left.forwardslash.chevron.right")
                }
                .font(.caption)
                .sheet(isPresented: $editingScripts) { scriptEditor }
            }
        }
    }

    private func paramRow(_ p: EngineModel.ParamEntry) -> some View {
        let isInt = ["example", "mode", "palette", "channel", "source",
                     "grid_w", "grid_h", "kernel_mode", "blend",
                     "levels", "radius"].contains(p.key)
                    || (p.max - p.min <= 15 && p.min == p.min.rounded()
                        && p.max == p.max.rounded() && p.max - p.min >= 1)
        return HStack(spacing: 8) {
            Text(p.reg >= 0 ? "\(p.key) ⇠r\(String(format: "%02d", p.reg))" : p.key)
                .font(.caption2)
                .frame(width: 82, alignment: .leading)
                .foregroundStyle(p.reg >= 0 ? .orange : Color.secondary)
            Slider(value: Binding(
                get: { Double(p.value) },
                set: { model.setParam(p.key, Float($0)) }
            ), in: Double(p.min)...Double(p.max),
               step: isInt ? 1 : 0.0001)
            .disabled(p.reg >= 0)   // reg drives it each frame
            Text(isInt ? "\(Int(p.value))" : String(format: "%.2f", p.value))
                .font(.caption2).monospacedDigit()
                .frame(width: 38, alignment: .trailing)
        }
        .contextMenu {
            Menu("Drive from register") {
                ForEach(0..<8, id: \.self) { r in
                    Button("reg0\(r)") { model.bindParam(p.key, toReg: r) }
                }
            }
            if p.reg >= 0 {
                Button("Unbind (manual slider)") { model.bindParam(p.key, toReg: -1) }
            }
        }
    }

    private var scriptEditor: some View {
        VStack(alignment: .leading, spacing: 8) {
            Text("Edit Scripts (EEL)").font(.headline)
            ForEach(Array(["init", "frame", "beat", "point"].enumerated()), id: \.offset) { i, label in
                Text(label).font(.caption).foregroundStyle(.secondary)
                TextEditor(text: $draft[i])
                    .font(.system(.caption, design: .monospaced))
                    .frame(minHeight: i == 3 ? 110 : 54)
                    .overlay(RoundedRectangle(cornerRadius: 4).stroke(.quaternary))
            }
            HStack {
                Spacer()
                Button("Cancel") { editingScripts = false }
                Button("Apply") {
                    model.applyScripts(draft)
                    editingScripts = false
                }.keyboardShortcut(.defaultAction)
            }
        }
        .padding(16)
        .frame(width: 560, height: 520)
    }
}
