//
//  EffectStackSection.swift
//  The editable effect stack: toggle, reorder, remove, add from the registry
//  (classic AVS ports and modern effects in separate menus).
//
import SwiftUI

struct EffectStackSection: View {
    @EnvironmentObject var model: EngineModel

    var body: some View {
        GroupBox("Effect Stack") {
            VStack(alignment: .leading, spacing: 4) {
                ForEach(model.effects) { fx in
                    HStack(spacing: 4) {
                        Toggle(isOn: Binding(
                            get: { fx.enabled },
                            set: { _ in model.toggleEffect(fx.id) }
                        )) { EmptyView() }
                        .toggleStyle(.checkbox)
                        .labelsHidden()
                        Text(fx.name)
                            .fontWeight(model.selectedEffect == fx.id ? .bold : .regular)
                            .foregroundStyle(model.selectedEffect == fx.id ? Color.accentColor : .primary)
                            .onTapGesture {
                                model.selectedEffect = (model.selectedEffect == fx.id) ? nil : fx.id
                            }
                        Spacer()
                        Button { model.moveEffect(fx.id, delta: -1) } label: {
                            Image(systemName: "chevron.up")
                        }.buttonStyle(.borderless).disabled(fx.id == 0)
                        Button { model.moveEffect(fx.id, delta: 1) } label: {
                            Image(systemName: "chevron.down")
                        }.buttonStyle(.borderless).disabled(fx.id == model.effects.count - 1)
                        Button { model.removeEffect(fx.id) } label: {
                            Image(systemName: "xmark.circle")
                        }.buttonStyle(.borderless).foregroundStyle(.secondary)
                    }
                    .font(.caption)
                }
                Divider().padding(.vertical, 2)
                HStack {
                    Menu {
                        ForEach(model.availableEffects.filter { !$0.modern }, id: \.key) { fx in
                            Button(fx.name) { model.addEffect(key: fx.key) }
                        }
                    } label: {
                        Label("Classic (AVS)", systemImage: "plus")
                    }
                    Menu {
                        ForEach(model.availableEffects.filter { $0.modern }, id: \.key) { fx in
                            Button(fx.name) { model.addEffect(key: fx.key) }
                        }
                    } label: {
                        Label("Modern", systemImage: "sparkles")
                    }
                }
                .menuStyle(.borderlessButton)
                .font(.caption)
            }.frame(maxWidth: .infinity, alignment: .leading)
        }
    }
}
