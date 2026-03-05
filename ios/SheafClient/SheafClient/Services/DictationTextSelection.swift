import Foundation

func dictationInsertionText(from response: DictateAudioResponse) -> String? {
    let revised = response.revisedText.trimmingCharacters(in: .whitespacesAndNewlines)
    guard !revised.isEmpty else {
        return nil
    }
    return revised
}
