import Foundation

/// The model prompts, kept as plain text in the repository's `prompts/` folder (bundled with the app) so they
/// can be read and tuned without touching Swift. actor/planner = the shared rules + the app-only rules.
enum Prompts {
    static let planner = load("planner")
    static let actorServer = load("actor")
    static let appRules = load("app-rules")

    static let actor = Self.absoluteHid + "\n" + actorServer + "\n" + appRules
    static let plannerApp = planner + "\n" + appRules

    /// Rules for the bare absolute HID pointer. The upstream text still describes a relative mouse
    /// (`precise` searches for the pointer, swipes have no momentum): both are untrue here.
    static let absoluteHid = """
    The pointer is absolute: a tap lands exactly where the coordinates say, with no drift, so `precise` \
    is only a diagnostic and must not be used to compensate for a miss. A miss means the target was \
    misread, not mis-aimed: look at the screen again and compare it with the last action. A swipe moves \
    the content by the distance you give it, so a longer swipe scrolls further; when a list did not \
    move, the finger probably started on a control instead of empty list space.
    """

    /// One file from the bundled prompts folder, without the file's final newline.
    static func load(_ name: String) -> String {
        guard let url = Bundle.main.url(forResource: name, withExtension: "md", subdirectory: "prompts"),
              let text = try? String(contentsOf: url, encoding: .utf8) else {
            fatalError("prompts/\(name).md is missing from the app bundle")
        }
        return text.hasSuffix("\n") ? String(text.dropLast()) : text
    }
}
