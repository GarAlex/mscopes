//
// png-diff.swift — compare same-named PNGs in two directories (e.g. GPU vs
// forced-CPU renders) and report per-image mean/max absolute channel
// difference on a 0..255 scale, plus the share of pixels off by more than 8.
//
//   swift tools/png-diff.swift <dirA> <dirB> [--all]
//
// Prints the worst images first (all of them with --all, else the top 12) and
// a summary line. Exits 1 if any image's mean difference exceeds 2.0.
//
import AppKit

func pixels(_ url: URL) -> (Int, Int, [UInt8])? {
    guard let img = NSImage(contentsOf: url),
          let cg = img.cgImage(forProposedRect: nil, context: nil, hints: nil) else { return nil }
    let w = cg.width, h = cg.height
    var buf = [UInt8](repeating: 0, count: w * h * 4)
    let cs = CGColorSpaceCreateDeviceRGB()
    guard let ctx = CGContext(data: &buf, width: w, height: h, bitsPerComponent: 8,
                              bytesPerRow: w * 4, space: cs,
                              bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue) else { return nil }
    ctx.draw(cg, in: CGRect(x: 0, y: 0, width: w, height: h))
    return (w, h, buf)
}

let args = CommandLine.arguments
guard args.count >= 3 else { print("usage: png-diff <dirA> <dirB> [--all]"); exit(2) }
let dirA = URL(fileURLWithPath: args[1]), dirB = URL(fileURLWithPath: args[2])
let showAll = args.contains("--all")

let fm = FileManager.default
let names = ((try? fm.contentsOfDirectory(atPath: dirA.path)) ?? []).filter { $0.hasSuffix(".png") }.sorted()
struct Row { let name: String; let mean: Double; let max: Int; let bad: Double }
var rows: [Row] = []
var missing = 0
for n in names {
    guard let a = pixels(dirA.appendingPathComponent(n)), let b = pixels(dirB.appendingPathComponent(n)) else { missing += 1; continue }
    guard a.0 == b.0, a.1 == b.1 else { print("size mismatch: \(n)"); continue }
    var sum = 0, mx = 0, bad = 0
    let n4 = a.0 * a.1
    for i in 0..<n4 {
        var pmax = 0
        for c in 0..<3 {
            let d = abs(Int(a.2[i * 4 + c]) - Int(b.2[i * 4 + c]))
            sum += d; if d > pmax { pmax = d }
        }
        if pmax > mx { mx = pmax }
        if pmax > 8 { bad += 1 }
    }
    rows.append(Row(name: n, mean: Double(sum) / Double(n4 * 3), max: mx, bad: Double(bad) / Double(n4)))
}
rows.sort { $0.mean > $1.mean }
func pad(_ s: String, _ n: Int) -> String { s.count >= n ? String(s.prefix(n)) : s + String(repeating: " ", count: n - s.count) }
print(pad("image", 48) + "     mean   max     >8px")
for r in showAll ? rows : Array(rows.prefix(12)) {
    print(pad(r.name, 48) + String(format: " %8.3f %5d %7.2f%%", r.mean, r.max, r.bad * 100))
}
let overall = rows.isEmpty ? 0 : rows.map { $0.mean }.reduce(0, +) / Double(rows.count)
let worst = rows.first?.mean ?? 0
print(String(format: "== %d images compared (%d missing in B): mean-of-means %.3f, worst mean %.3f ==",
             rows.count, missing, overall, worst))
exit(worst > 2.0 ? 1 : 0)
