import CoreGraphics
import Foundation
import Darwin
if CommandLine.arguments.count < 4 { exit(2) }
let pid = pid_t(CommandLine.arguments[1])!
let key = CommandLine.arguments[2]
let duration = Double(CommandLine.arguments[3])!
let source = CGEventSource(stateID: .privateState)
if key == "mouse" {
    let location = CGEvent(source: nil)!.location
    CGEvent(mouseEventSource: source, mouseType: .leftMouseDown, mouseCursorPosition: location, mouseButton: .left)!.postToPid(pid)
    Thread.sleep(forTimeInterval: duration)
    CGEvent(mouseEventSource: source, mouseType: .leftMouseUp, mouseCursorPosition: location, mouseButton: .left)!.postToPid(pid)
} else {
    let keycode = CGKeyCode(key)!
    CGEvent(keyboardEventSource: source, virtualKey: keycode, keyDown: true)!.postToPid(pid)
    Thread.sleep(forTimeInterval: duration)
    CGEvent(keyboardEventSource: source, virtualKey: keycode, keyDown: false)!.postToPid(pid)
}
