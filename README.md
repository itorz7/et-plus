# et-plus

EternalTerminal fork with **clipboard image paste** for remote AI tools like Claude Code and Codex.

Copy an image on your Mac, press `Ctrl+V` or `Cmd+V` in an ET session, and the image lands on the remote server as a file. The remote path is typed into your terminal automatically.

## Install

Both **client (Mac)** and **server** need et-plus. Same package, different binaries are used on each side.

```bash
brew install itorz7/tap/et-plus
```

This installs `et`, `etserver`, and `etterminal`. Conflicts with the original `et` package — uninstall it first if you have it:

```bash
brew uninstall et
```

## What gets installed where

| Machine | Binaries used | Role |
|---------|--------------|------|
| **Mac (client)** | `et` | Connects to server. Intercepts Ctrl+V / Cmd+V and sends images. |
| **Server** | `etserver`, `etterminal` | Receives images, saves to `/tmp/et-clipboard-images-<uid>/`, inserts path into terminal. |

## Setup

### Server

Start etserver (if not already running as a system daemon):

```bash
etserver --daemon --port 2022
```

### Client (Mac)

Connect like normal ET:

```bash
et user@server
```

To use the patched `etterminal` on the server (required for image paste), add `--terminal-path`:

```bash
et --terminal-path /path/to/etterminal user@server
```

Or create a wrapper script:

```bash
cat > ~/.local/bin/et-myserver << 'EOF'
#!/bin/sh
exec et --terminal-path /home/myuser/.local/et-image-paste/bin/etterminal "$@" myserver
EOF
chmod +x ~/.local/bin/et-myserver
```

## Usage

1. Copy an image on Mac (screenshot, from browser, from Telegram, etc.)
2. Focus the ET terminal session
3. Press `Ctrl+V` or `Cmd+V`
4. A path like `/tmp/et-clipboard-images-1000/image-AbCd12.png` appears in your terminal

Works inside tmux, Claude Code, Codex, or any shell running in the ET session.

### What happens with each paste method

| Action | Image in clipboard | File path in clipboard | Plain text |
|--------|-------------------|----------------------|------------|
| `Ctrl+V` | Sends image to server | Normal paste | Normal paste |
| `Cmd+V` | Sends image to server (if terminal creates temp file) | Sends file if it's an image on Mac | Normal paste |

Non-image files, folders, URLs, and text always pass through unchanged.

## Supported formats

PNG, JPEG, GIF, TIFF, BMP, WebP. Max 25 MiB per image.

## Debug

```bash
ET_PLUS_DEBUG=1 et user@server
```

Logs raw input to `~/et-plus-debug.log` (rotates at 50KB).

## Disable image paste

For one session:

```bash
ET_DISABLE_CLIPBOARD_IMAGE_PASTE=1 et user@server
```

## Build from source

```bash
git clone https://github.com/itorz7/et-plus.git
cd et-plus

brew install cmake ninja protobuf libsodium gflags

cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DDISABLE_TELEMETRY=ON

cmake --build build --target et etterminal etserver -j$(nproc 2>/dev/null || sysctl -n hw.ncpu)
```

Binaries are in `build/et`, `build/etterminal`, `build/etserver`.

## Upstream

Forked from [MisterTea/EternalTerminal](https://github.com/MisterTea/EternalTerminal). All upstream features (reconnection, port forwarding, tmux integration) work as before.

## License

Apache-2.0 (same as upstream)
