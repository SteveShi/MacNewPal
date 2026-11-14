import SwiftUI

struct WhatsNewSheetView: View {
    @Environment(\.dismiss) private var dismiss

    var body: some View {
        VStack(alignment: .leading, spacing: 20) {
            HStack(spacing: 12) {
                Image(systemName: "sparkles.rectangle.stack.fill")
                    .font(.system(size: 34))
                    .foregroundStyle(.tint)
                VStack(alignment: .leading, spacing: 4) {
                    Text("whatsnew.title")
                        .font(.title2.bold())
                    Text("whatsnew.version")
                        .font(.subheadline)
                        .foregroundStyle(.secondary)
                }
            }

            Divider()

            VStack(alignment: .leading, spacing: 14) {
                featureRow(
                    icon: "wand.and.stars",
                    color: .purple,
                    titleKey: "whatsnew.feature1.title",
                    descKey: "whatsnew.feature1.desc"
                )
                featureRow(
                    icon: "gamecontroller.fill",
                    color: .blue,
                    titleKey: "whatsnew.feature2.title",
                    descKey: "whatsnew.feature2.desc"
                )
                featureRow(
                    icon: "checkmark.shield.fill",
                    color: .green,
                    titleKey: "whatsnew.feature3.title",
                    descKey: "whatsnew.feature3.desc"
                )
            }

            Spacer()

            HStack {
                Spacer()
                Button("whatsnew.done") {
                    dismiss()
                }
                .keyboardShortcut(.defaultAction)
                .buttonStyle(.borderedProminent)
            }
        }
        .padding(24)
        .frame(width: 520, height: 410)
    }

    private func featureRow(
        icon: String,
        color: Color,
        titleKey: LocalizedStringKey,
        descKey: LocalizedStringKey
    ) -> some View {
        HStack(alignment: .top, spacing: 12) {
            Image(systemName: icon)
                .font(.title3)
                .foregroundStyle(color)
                .frame(width: 28, alignment: .center)
            VStack(alignment: .leading, spacing: 3) {
                Text(titleKey)
                    .font(.headline)
                Text(descKey)
                    .font(.callout)
                    .foregroundStyle(.secondary)
                    .fixedSize(horizontal: false, vertical: true)
            }
        }
    }
}
