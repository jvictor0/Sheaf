import Foundation

struct DictateAudioResponse: Decodable, Hashable {
    let rawTranscript: String
    let revisedText: String
    let editSummary: String
    let uncertaintyFlags: [String]
    let transcribeMS: Int
    let refineMS: Int

    enum CodingKeys: String, CodingKey {
        case rawTranscript = "raw_transcript"
        case revisedText = "revised_text"
        case editSummary = "edit_summary"
        case uncertaintyFlags = "uncertainty_flags"
        case transcribeMS = "transcribe_ms"
        case refineMS = "refine_ms"
    }
}
