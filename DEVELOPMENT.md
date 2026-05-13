# et-plus Development Log

## 2026-05-14 — Initial Development

### What is et-plus?

Fork ของ [MisterTea/EternalTerminal](https://github.com/MisterTea/EternalTerminal) ที่เพิ่ม clipboard image paste สำหรับใช้กับ AI tools (Claude Code, Codex) ผ่าน remote terminal.

Repo: https://github.com/itorz7/et-plus
Homebrew: `brew install itorz7/tap/et-plus`
Homebrew tap repo: https://github.com/itorz7/homebrew-tap

### Upstream

- Upstream: `https://github.com/MisterTea/EternalTerminal`
- Base version: `6.2.11` (commit `77eab5ec5`)
- GitHub Actions sync ทุกวันจันทร์ 09:00 UTC (`.github/workflows/sync-upstream.yml`)
  - ถ้า merge clean → push ตรง
  - ถ้ามี conflict → สร้าง PR ให้ review
  - รัน manual ได้ที่ Actions tab → "Sync upstream" → "Run workflow"

### Files Changed (from upstream)

**New files:**
- `src/terminal/ClipboardImageFrame.cpp` — encode/decode image frame protocol, content-hash dedup
- `src/terminal/ClipboardImageFrame.hpp` — structs: `ClipboardImagePayload`, `ClipboardImageSaveResult`
- `src/terminal/LocalClipboardImage.cpp` — macOS clipboard image reader (ใช้ osascript/JXA)
- `src/terminal/LocalClipboardImage.hpp`
- `test/unit_tests/ClipboardImageFrameTest.cpp` — 5 unit tests
- `.github/workflows/sync-upstream.yml` — upstream auto-sync

**Modified files:**
- `CMakeLists.txt` — เพิ่ม ClipboardImageFrame + LocalClipboardImage เข้า build
- `src/terminal/SshSetupHandler.cpp` — parse `ETCAPS:clipboard-image-paste` จาก SSH output
- `src/terminal/SshSetupHandler.hpp` — เพิ่ม `supportsClipboardImagePaste_` flag
- `src/terminal/TerminalClient.cpp` — main logic: ดัก Ctrl+V, ดัก bracketed paste, ส่ง image frame
- `src/terminal/TerminalClient.hpp` — เพิ่ม `clipboardImagePasteSupported` member
- `src/terminal/TerminalClientMain.cpp` — pass clipboard support flag เข้า TerminalClient
- `src/terminal/TerminalMain.cpp` — etterminal advertise `ETCAPS:clipboard-image-paste`
- `src/terminal/UserTerminalHandler.cpp` — รับ image frame, เขียนไฟล์, ใส่ path ลง PTY
- `test/unit_tests/SshSetupHandlerTest.cpp` — test ETCAPS parsing

### Architecture

```
Mac (client)                              Server
─────────────                             ──────
Terminal App (Warp/cmux/iTerm2)
    │
    ├── Cmd+V → bracketed paste text
    │   ("\e[200~/var/folders/.../clip.png\e[201~")
    │
    └── Ctrl+V → raw 0x16 byte
            │
        ET client (patched `et`)
            │
            ├── Ctrl+V → readLocalClipboardImage()
            │   → osascript reads NSPasteboard
            │   → gets PNG/JPEG bytes
            │
            ├── Bracketed paste → extract path
            │   → stat() file on Mac
            │   → if image exists, read bytes
            │
            └── encodeClipboardImageFrame()
                → sends through ET encrypted connection
                                            │
                                    etserver (relay, unmodified)
                                            │
                                    etterminal (patched)
                                            │
                                    saveClipboardImageFrameToTemp()
                                    → content hash → filename
                                    → /tmp/et-clipboard-images-<uid>/<hash>.png
                                    → write path + space to PTY
                                            │
                                    Shell / Claude Code / Codex
                                    sees: /tmp/et-clipboard-images-1000/a1b2c3.png
```

### How Cmd+V Works (the hard part)

1. User presses `Cmd+V` in terminal app
2. Terminal app reads macOS clipboard → image → creates temp file `/var/folders/.../clipboard-xxx.png`
3. Terminal app sends path as **bracketed paste**: `\e[200~<path>\e[201~`
4. ET client receives this across **multiple read() chunks** (ปกติ 2 chunks):
   - Chunk 1: `\e[200~/var/folders/.../clipboard-2026-` (70 bytes)
   - Chunk 2: `05-14-010853-00D7184F.png\e[201~` (31 bytes)
5. ET client buffers between `\e[200~` and `\e[201~` across chunks
6. When `\e[201~` arrives → check if buffered text is a local image file path
7. If file exists on Mac + is an image → read file → send as image frame
8. If not → re-emit with bracketed paste markers intact (normal text paste)

### Key Decisions & Gotchas

**`clipboardImagePasteSupported` flag:**
- Set to `true` only when remote `etterminal` sends `ETCAPS:clipboard-image-paste` during SSH setup
- If server uses system (non-patched) `etterminal` → flag is `false` → all paste logic disabled
- Fix: `et` wrapper must use `--terminal-path /path/to/patched/etterminal`

**etserver doesn't need patching:**
- etserver is just a relay — forwards packets between client and etterminal
- System etserver (from `brew install et` or apt) works fine
- Only `et` (client) and `etterminal` (server) need the patched version

**Warp terminal doesn't work with client-side interception:**
- Warp bypasses ET stdin completely — sends text directly to remote PTY
- ET client never sees the paste input (confirmed via debug logging)
- cmux/ghostty/iTerm2/Terminal.app all work fine (they send through ET stdin)
- Warp support would require server-side interception (future work)

**Code signing on macOS:**
- Every time binary is copied (`cp` or `scp`), macOS invalidates the ad-hoc signature
- Must `codesign -s - --force <binary>` after every copy
- The `~/.local/bin/et` wrapper handles this automatically

**Content-hash dedup:**
- Same image pasted multiple times → same file path (hash-based filename)
- Uses `std::hash<string>` on image bytes → 16-char hex filename
- Checks if file with same hash+size already exists → skips write

### Debug

```bash
# Client-side debug (on Mac)
ET_PLUS_DEBUG=1 et user@host
cat ~/et-plus-debug.log

# Logs raw input bytes, max 200 bytes per entry, rotates at 50KB
```

### Build & Release Process

**Build from source:**
```bash
git clone https://github.com/itorz7/et-plus.git
cd et-plus
brew install cmake ninja protobuf libsodium gflags
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DDISABLE_TELEMETRY=ON
cmake --build build --target et etterminal etserver -j$(nproc 2>/dev/null || sysctl -n hw.ncpu)
```

**Publish new release:**
```bash
# 1. Make changes, commit
git add -A
git commit -m "description"

# 2. Tag
git tag v6.2.11-plus-2  # increment suffix
git push origin master --tags

# 3. Update homebrew formula
cd /path/to/homebrew-tap
# Edit Formula/et-plus.rb:
#   - Update tag: and revision: fields
#   - Update version if needed
git commit -am "Bump et-plus to vX.Y.Z"
git push
```

**Homebrew tap repo:** https://github.com/itorz7/homebrew-tap
- Formula uses `url` with git clone (not tarball) because submodules (vcpkg) are needed
- `conflicts_with "et"` — can't have both et and et-plus installed

**Install:**
```bash
brew uninstall et  # if installed
brew install itorz7/tap/et-plus
```

### Server Setup

```bash
# Install et-plus on server via brew
brew install itorz7/tap/et-plus

# Or copy binaries manually
scp build/etserver build/etterminal user@server:~/.local/et-image-paste/bin/

# Start etserver
etserver --daemon --port 2022

# Connect from Mac with patched etterminal
et --terminal-path /path/to/etterminal user@server
```

### Project Layout on Local Machines

**Server (k1):**
- Source: `~/Desktop/hobby/et-plus/`
- Binaries: `~/.local/et-image-paste/bin/etserver`, `~/.local/et-image-paste/bin/etterminal`

**Mac (k7):**
- Build cache: `/tmp/et-image-paste/build-macos-release/`
- Binaries: `~/.local/et-image-paste/bin/et`, `~/.local/et-image-paste/bin/etterminal`
- Wrappers: `~/.local/bin/et` (auto-sign + --terminal-path), `~/.local/bin/et-k1`
- Homebrew: `/opt/homebrew/Cellar/et-plus/`

### Known Limitations

- **Warp terminal**: Bypasses ET client stdin. Paste interception doesn't work. (cmux, ghostty, iTerm2, Terminal.app all work.)
- **Max image size**: 25 MiB
- **macOS only for client**: Image clipboard reading uses NSPasteboard via osascript. Linux/Windows clients can't send images.
- **Images persist in /tmp**: Not auto-cleaned. Rely on OS /tmp cleanup or manual removal.

### Commit History (et-plus specific)

| Commit | Description |
|--------|------------|
| `3bfd6fe` | Add clipboard image paste support (Cmd+V / Ctrl+V) |
| `9cbafef` | Replace README with et-plus documentation |
| `7e220b1` | Add weekly upstream sync via GitHub Actions |
| `2883d6f` | Deduplicate images: reuse path for identical content |
