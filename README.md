# ft8cb-rx 📡

**Web-based FT8 receiver for the 11 meters CB band (27 MHz)**

A Dockerized FT8 decoder adapted for CB callsigns, with a live web interface.  
Supports RTL-SDR (local USB) and KiwiSDR (network) as audio sources.

Built on top of [ft8_lib](https://github.com/kgoba/ft8_lib) with CB callsign patches inspired by [WSJT-CB](https://github.com/vash909/WSJT-CB).

---

## Features

- 🎯 **CB callsign support** — Recognizes formats like `14FR001`, `1AT106`, `26AT715`
- 📡 **RTL-SDR support** — Use a local USB RTL-SDR dongle (RTL-SDR Blog V4 supported)
- 🌐 **KiwiSDR support** — Connect to any public KiwiSDR receiver on the internet
- 🌊 **Live waterfall** — Real-time spectrogram in the browser
- 💬 **Live decoded messages** — FT8 messages appear every 15 seconds
- 🗺️ **Propagation map** — Spots displayed on a Leaflet dark map
- 🐳 **Fully Dockerized** — One `docker compose up` and it runs

---

## Quick Start

```bash
git clone https://github.com/dobeyman/ft8cb-rx
cd ft8cb-rx
cp .env.example .env
# Edit .env to set your source (rtlsdr or kiwisdr)
docker compose up -d
# Open http://localhost:8080
```

---

## Configuration

Copy `.env.example` to `.env` and edit:

```env
# Source: "rtlsdr" or "kiwisdr"
SOURCE=rtlsdr

# RTL-SDR settings (if SOURCE=rtlsdr)
RTLSDR_DEVICE=0
RTLSDR_GAIN=40
RTLSDR_PPM=0

# KiwiSDR settings (if SOURCE=kiwisdr)
KIWISDR_HOST=sdr.example.com
KIWISDR_PORT=8073

# Common
FREQUENCY=27265000
SAMPLE_RATE=12000
```

---

## Architecture

```
[RTL-SDR USB] ──┐
                ├──► [audio-bridge] ──► [ft8cb-decoder] ──► [backend WS] ──► [frontend]
[KiwiSDR net] ──┘         Node.js           C++ patched         Node.js        HTML/JS
```

---

## CB Callsign Patches

The decoder has been patched to accept CB-style callsigns:
- Regex: `^[0-9]{1,3}[A-Z]{1,2}[0-9]{1,3}$`
- Country resolution via numeric CB prefix table
- Hash callsign `<...>` handling

---

## License

GPL v3 — Same as WSJT-X / ft8_lib

## Credits

- [ft8_lib](https://github.com/kgoba/ft8_lib) by kgoba
- [WSJT-CB](https://github.com/vash909/WSJT-CB) CB patches by vash909 / 1XZ001
- [WSJT-X](https://sourceforge.net/projects/wsjt/) by Joe Taylor K1JT
