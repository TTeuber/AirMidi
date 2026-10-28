import Cocoa
import FlutterMacOS

class MainFlutterWindow: NSWindow {
  override func awakeFromNib() {
    let flutterViewController = FlutterViewController()
    let windowFrame = self.frame
    self.contentViewController = flutterViewController
    self.setFrame(windowFrame, display: true)

    // --window WxH: the content size in points (tool/shots.py takes every screenshot at one size). Set after the
    // window's restored frame (the app's last size) has been applied, and not restored or saved itself.
    let args = CommandLine.arguments
    if let i = args.firstIndex(of: "--window"), i + 1 < args.count {
      let wh = args[i + 1].split(separator: "x").compactMap { Double($0) }
      if wh.count == 2 {
        self.isRestorable = false
        DispatchQueue.main.async {
          self.setContentSize(NSSize(width: wh[0], height: wh[1]))
          self.setFrameTopLeftPoint(NSPoint(x: 60, y: (self.screen?.visibleFrame.maxY ?? 900) - 20))
        }
      }
    }

    RegisterGeneratedPlugins(registry: flutterViewController)

    super.awakeFromNib()
  }
}
