import SwiftUI
import UIKit

struct CursorTextView: UIViewRepresentable {
    @Binding var text: String
    @Binding var selectedRange: NSRange
    @Binding var isFocused: Bool
    let placeholder: String

    func makeCoordinator() -> Coordinator {
        Coordinator(self)
    }

    func makeUIView(context: Context) -> UITextView {
        let textView = UITextView()
        textView.delegate = context.coordinator
        textView.font = .preferredFont(forTextStyle: .body)
        textView.backgroundColor = .clear
        textView.autocorrectionType = .yes
        textView.autocapitalizationType = .sentences
        textView.isScrollEnabled = true
        textView.textContainerInset = UIEdgeInsets(top: 8, left: 6, bottom: 8, right: 6)
        textView.textContainer.lineFragmentPadding = 0
        context.coordinator.placeholderLabel.text = placeholder
        context.coordinator.attachPlaceholder(to: textView)
        return textView
    }

    func updateUIView(_ textView: UITextView, context: Context) {
        if textView.text != text {
            textView.text = text
        }
        if textView.selectedRange != selectedRange {
            textView.selectedRange = selectedRange
        }
        context.coordinator.placeholderLabel.isHidden = !text.isEmpty

        if isFocused, !textView.isFirstResponder {
            textView.becomeFirstResponder()
        } else if !isFocused, textView.isFirstResponder {
            textView.resignFirstResponder()
        }
    }

    final class Coordinator: NSObject, UITextViewDelegate {
        var parent: CursorTextView
        let placeholderLabel = UILabel()

        init(_ parent: CursorTextView) {
            self.parent = parent
            super.init()
            placeholderLabel.font = .preferredFont(forTextStyle: .body)
            placeholderLabel.textColor = .placeholderText
            placeholderLabel.numberOfLines = 1
        }

        func attachPlaceholder(to textView: UITextView) {
            guard placeholderLabel.superview == nil else { return }
            textView.addSubview(placeholderLabel)
            placeholderLabel.translatesAutoresizingMaskIntoConstraints = false
            NSLayoutConstraint.activate([
                placeholderLabel.leadingAnchor.constraint(equalTo: textView.leadingAnchor, constant: 8),
                placeholderLabel.topAnchor.constraint(equalTo: textView.topAnchor, constant: 8),
                placeholderLabel.trailingAnchor.constraint(lessThanOrEqualTo: textView.trailingAnchor, constant: -8)
            ])
        }

        func textViewDidChange(_ textView: UITextView) {
            parent.text = textView.text
            parent.selectedRange = textView.selectedRange
            placeholderLabel.isHidden = !textView.text.isEmpty
        }

        func textViewDidChangeSelection(_ textView: UITextView) {
            parent.selectedRange = textView.selectedRange
        }

        func textViewDidBeginEditing(_ textView: UITextView) {
            parent.isFocused = true
            parent.selectedRange = textView.selectedRange
        }

        func textViewDidEndEditing(_ textView: UITextView) {
            parent.isFocused = false
            parent.selectedRange = textView.selectedRange
        }
    }
}

func insertTextAtSelection(_ insertion: String, text: inout String, selection: inout NSRange) {
    let original = text as NSString
    let safeLocation = max(0, min(selection.location, original.length))
    let maxLength = max(0, original.length - safeLocation)
    let safeLength = max(0, min(selection.length, maxLength))
    let safeSelection = NSRange(location: safeLocation, length: safeLength)

    text = original.replacingCharacters(in: safeSelection, with: insertion)
    let newLocation = safeLocation + (insertion as NSString).length
    selection = NSRange(location: newLocation, length: 0)
}
