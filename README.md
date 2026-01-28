# PSP DRP

Display your PSP gaming activity on Discord! This project consists of two components:

1. **PSP Plugin** - Runs on your PSP under ARK 4 CFW, detects games/apps, and sends data over WiFi
2. **Desktop Companion** - Rust application that receives PSP data and updates Discord Rich Presence

## Features

- Show currently playing game with title and elapsed time
- Configurable activation (games only or always active)
- Auto-discovery or manual IP configuration
- Support for UMD, ISO/CSO games

## Requirements

### PSP
- PSP-1000 (or 2000/3000)
- Custom Firmware 6.61 with ARK 4 CFW (have not tested with other CFWs)
- WiFi connection to same network as desktop app

### Desktop
- Windows, macOS, or Linux (only tested on Windows)
- Discord desktop application running
- Rust toolchain (for building)

### Development
- Docker (for building PSP plugin)
- Rust 1.70+ (for desktop app)

## Quick Start

### 1. Create Discord Application

1. Go to https://discord.com/developers/applications
2. Click **New Application** and name it (e.g., "PSP Gaming")
3. Copy the **Application ID** - this is your Client ID
4. Go to **Rich Presence** > **Art Assets**
5. Upload a PSP logo image and name it `psp_logo`

### 2. Build Desktop Companion

```bash
cd desktop-companion
cargo build --release
```

The binary will be at `target/release/psp-drp.exe` (Windows) or `target/release/psp-drp` (Linux/macOS).

### 3. Configure Desktop App

Run the app once to generate the config file, then edit it:

**All OSes:** `config.toml` in the same directory as the desktop app binary

Set your Discord Client ID:

```toml
[discord]
client_id = "YOUR_APPLICATION_ID_HERE"
show_elapsed_time = true
state_text = "Playing on PSP"

[network]
listen_port = 9276
discovery_port = 9277
auto_discovery = true

[display]
log_level = "info"
```

### 4. Build PSP Plugin

Requires Docker:

```bash
docker compose run --rm pspdev make
```

This creates `psp_drp.prx` in the `psp-plugin` folder.

### 5. Install PSP Plugin

1. Copy all PRX files to `ms0:/SEPLUGINS/pspdrp/`:
   - `psp-plugin/loader/psp_drp_loader.prx`
   - `psp-plugin/ui/psp_drp_ui.prx`
   - `psp-plugin/net/psp_drp_net.prx`
2. Update `ms0:/SEPLUGINS/plugins.txt` and add:
   ```
   xmb, ms0:/SEPLUGINS/pspdrp/psp_drp_loader.prx, on
   game, ms0:/SEPLUGINS/pspdrp/psp_drp_loader.prx, on
   ```
   (Do not add `psp_drp_ui.prx` or `psp_drp_net.prx` to plugins.txt)
3. Create config file `ms0:/SEPLUGINS/pspdrp/psp_drp.ini`:
   ```ini
   enabled = 1
   desktop_ip = 192.168.1.100  ; Your PC's IP address
   port = 9276
   auto_discovery = 1
   always_active = 0
   send_icons = 1
   psp_name = My PSP
   ```
4. Restart your PSP

### 6. Run

1. Start the desktop companion app
2. Ensure your PSP is connected to WiFi
3. Launch a game - your Discord status should update!

### Skip Modifier

### Debug Logs

- Logging : `ms0:/psp_drp.log`

## Configuration

### PSP Config (`ms0:/SEPLUGINS/pspdrp/psp_drp.ini`)

| Option | Description | Default |
|--------|-------------|---------|
| `enabled` | Enable/disable plugin | `1` |
| `desktop_ip` | Desktop app IP (empty = auto-discovery) | `` |
| `port` | Connection port | `9276` |
| `auto_discovery` | Find desktop automatically and save its IP | `1` |
| `always_active` | Show status even on XMB | `0` |
| `send_icons` | Send game icons to desktop | `1` |
| `psp_name` | Custom name shown in Discord | `PSP` |

### Desktop Config

| Option | Description | Default |
|--------|-------------|---------|
| `discord.client_id` | Your Discord Application ID | (required) |
| `discord.show_elapsed_time` | Show play time | `true` |
| `discord.state_text` | Status text below game title | `Playing on PSP` |
| `network.listen_port` | Port for PSP connections | `9276` |
| `network.auto_discovery` | Respond to PSP discovery | `true` |
| `network.timeout_seconds` | Disconnect timeout | `90` |

## Troubleshooting

### PSP won't connect
- Ensure WiFi switch is ON
- Check that PSP and PC are on same network
- Verify IP address in config is correct
- Check firewall allows UDP port 9276

### Discord status not updating
- Ensure Discord desktop app is running (not web version)
- Verify client_id is correct in config
- Check desktop app logs for errors

### Plugin not loading
- Verify ARK 4 is installed correctly
- Check `vsh.txt` has correct path
- Try disabling other plugins temporarily

## Development

### Building PSP DRP Desktop App

```bash
cd desktop-companion
cargo build --release
```

### Building PSP Plugin (Interactive)

```bash
docker compose run --rm pspdev /bin/bash
# Inside container:
make clean
make
```

## Credits

- [PSPSDK](https://github.com/pspdev/pspsdk) - PSP development toolchain
- [ARK-4](https://github.com/PSP-Archive/ARK-4) - Custom firmware
- [discord-rich-presence](https://crates.io/crates/discord-rich-presence) - Rust Discord RPC library
