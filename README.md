# Growtopia Proxy

[![Github Action](https://img.shields.io/github/actions/workflow/status/ZTzTopia/GTProxy/cmake_ci.yml?branch=develop&logo=github&logoColor=white)](https://github.com/ZTzTopia/GTProxy/actions?query=workflow%3ACI)
[![GitHub Release](https://img.shields.io/github/v/release/ZTzTopia/GTProxy.svg?color=orange&logo=docusign&logoColor=orange)](https://github.com/ZTzTopia/GTProxy/releases/latest) 

Growtopia Proxy is a free and open-source proxy for [Growtopia](https://growtopiagame.com/), which enables the user to debug incoming and outgoing packets and even modify them. It is a useful tool for developers who want to analyze the game's network traffic and develop custom features.

> I'm just wondering if I'm having a skill issue writing this C++ code since I first started this project, or if it's because the Growtopia packet structure is pretty complicated. I've had to do a lot of rewrites and refactors. Also, the first time I made this project, it was really just for learning about networking and C++.

> I do like to add a GUI like ImGui, Qt, or WebUI, but I don't have time to do it because my own Wi-Fi IP address is flagged by the Growtopia server (Indonesian players are being stupid for real. My friend said if someone is selling residential IP addresses for botting). I will add it if I'm patient enough to find a VPN that can connect to the Growtopia server without being flagged.

## Supported Platforms

- Windows. Tested with Windows 7, 8, 10 and 11.
- GNU/Linux. Tested with Ubuntu and Arch Linux. (but who use GNU/Linux to play growtopia?)
- macOS. Tested with macOS 26 Tahoe.

## Features

- Open source and free to use.
- Support for Growtopia 3.92 and newer versions.
- Includes a built-in http server with metadata from the Growtopia client.
- Works well with sub-server redirection.
- Supports packet debugging and modification.
- Includes a configuration file for customization.
- Support for new login system.

## Download

The latest application executable can be found on the [releases page](https://github.com/ZTzTopia/GTProxy/releases).

## Build

The following dependencies are required to build from source:

- [CMake](https://cmake.org/).
- [Conan, the C/C++ Package Manager](https://conan.io) (2.0+).

To build from source:

1. Clone the source code of this project: `git clone --recurse-submodules https://github.com/ZTzTopia/GTProxy.git`
2. Install Python 3.5+ (Windows only: select 'Add Python to PATH' in installer)
3. CMake will install needed package with Conan. `pip install "conan>2.0"`
    ```shell
    $ mkdir build
    $ cd build
    $ cmake .. -DCMAKE_BUILD_TYPE=Debug
    $ cmake --build .
    ```

### Cross-compiling for Windows on Linux

With MinGW-w64 (`apt install mingw-w64`) this produces a self-contained
`Proxy.exe` (only system DLLs):

```shell
cmake -B build-win -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/mingw-w64-x86_64.cmake \
  -DCONAN_HOST_PROFILE=$PWD/cmake/conan/mingw-w64-x86_64 -DCONAN_BUILD_PROFILE=default \
  -DHTTPLIB_USE_BROTLI_IF_AVAILABLE=OFF -DHTTPLIB_USE_NON_BLOCKING_GETADDRINFO=OFF \
  -DCMAKE_CROSSCOMPILING_EMULATOR=wine64 \
  -DCMAKE_EXE_LINKER_FLAGS="-static -static-libgcc -static-libstdc++"
cmake --build build-win
```

## Usage

1. Point the game's web hosts at the proxy. Add to the hosts file
   (Windows: `C:\Windows\System32\drivers\etc\hosts`, edit as administrator;
   macOS/Linux: `/etc/hosts`):
   ```
   127.0.0.1 www.growtopia1.com
   127.0.0.1 www.growtopia2.com
   ```
2. Trust the proxy's certificate once. On first start the proxy creates a
   certificate authority unique to your machine in `resources/` (`ca.pem`; the
   private keys never leave your computer). The game only talks to the proxy
   after that CA is trusted:
   ```
   Proxy --install-cert      # Windows: current user's Root store (asks to confirm)
                             # macOS: login keychain
   Proxy --uninstall-cert    # remove it again
   ```
3. Start the proxy, then the game. Port 443 must be free.

Each run writes `logs/session-<date>_<time>.txt` (the last 20 are kept). It
contains the whole session, a crash report if the proxy crashes, and has login
tokens and passwords replaced by `***`, so it can be shared when reporting a
problem.

Remove the hosts entries to play without the proxy.

### In-game commands

Type them in the game chat; the proxy answers them itself (they never reach the
server). `/proxyhelp` (or All commands in `/proxy`) opens a window with every
command, including those added by Lua scripts.

| Command | What it does |
|---------|--------------|
| `/proxy` | Menu with pages for worlds, items, looks and info |
| `/warp <world> [door]` | Exit and join a world (or a door in it), like typing the name in the world menu |
| `/back` | Return to the previous world |
| `/rejoin` | Exit and enter the current world again |
| `/nick [name]` | Show another name above you, only on your screen; `/nick` alone resets |
| `/netid` | Show/hide netIDs next to player names (your screen only) |
| `/pos` | Your tile position |
| `/players` | Players you can see in this world |
| `/world` | World name, size, players, dropped items |
| `/find <text>` / `/item <id>` | Search items.dat by name / show an item |
| `/skin <code>`, `/rainbow [ms]` | Change your skin colour on your screen (rainbow is a Lua script) |
| `/inv` | Your inventory with item IDs and marks (favourite / worn / keep) |
| `/dropall` | Drop everything except favourites, worn items and the keep list (asks first; `now`, `stop`, `speed <ms>`) |
| `/daw` | Drop all locks (World, Diamond, Blue Gem, Black Gem) |
| `/dd <WL>` | Drop locks worth that many World Locks, big ones first (`/dd 300` = 3 Diamond Locks); `/dd` alone = all locks |
| `/dw`, `/ddl`, `/db [amount]` | Drop World / Diamond / Blue Gem Locks |
| `/drop <id or name> [amount]` | Drop one item |
| `/fastdrop`, `/fasttrash` | Skip the "how many?" window when you drop / trash (fast trash never applies to locks) |
| `/keep [add\|del] <item>` | Items `/dropall` never drops |
| `/balance` | Your locks in World Locks |
| `/map` | Open the map window (see below) |
| `/goto <x> <y>` | Walk to a tile; `/goto stop` stops |
| `/reach <x> <y>` | Can you get to that tile from where you stand (as a right click in the map window) |
| `/clickwalk` | Toggle: clicking a tile in the game walks there instead of punching |

Drops are the same request and "how many?" answer the game sends when you
press Drop, one stack at a time, each waiting for the server's reply (with a
pause of 200 ms by default). Item settings are saved in `gtproxy-settings.txt`.

### Map window

`/map` (or *Map window* in `/proxy`) opens the world you are in as a map in its
own window: on Windows a Microsoft Edge app window, elsewhere the default
browser. It is served by the proxy itself at `http://127.0.0.1:17171/`, only
reachable from your computer, and shows blocks (in the colours of their seeds),
dangerous tiles, doors, players, dropped items and your path.

- **Left click** a tile: walk there. A click on a block walks to the free tile
  next to it, on your side.
- **Right click**: can you get there from where you stand - *Цель доступна*
  (reachable), *Цель недоступна* (closed off; also for a block in the tile
  or a tile outside the map) or *Нет данных* (no world, items.dat or
  position yet). It only reports, nothing is sent to the server. The check
  is a breadth-first search over the four neighbours of each tile (never
  diagonally between two blocks), from the tile your avatar stands in; your
  collision box from OnSpawn (`colrect`, 20x30 = one tile) has to fit in every
  tile on the way. Each check is logged with your position, the tile and the
  result. The same check: `/reach <x> <y>` in the game, or `reach <x> <y>`
  typed into the proxy's console window.
- **Drag** moves the map, the **wheel** zooms; **C** centres on you, **F**
  follows you, **Esc** stops walking. The header shows the tile under the cursor.

How walking works: the proxy turns the cursor into a tile with the same
conversion the window uses (`src/map/coords.hpp`), finds a path through free
tiles (A*, `src/map/pathfinder.cpp`; blocks are judged by their collision type
in items.dat), then sends the Growtopia server the same state packets the game
sends while you run - a position every 100 ms at 250 px/s (about 8 tiles a
second, never more than a fraction of a tile per packet) - and moves your
avatar in the game with `OnSetPos`. While it walks, the game's own position
packets are held back so the two never disagree. Movement is checked by the
Growtopia server with rules the proxy cannot see: if the server puts you
somewhere else (door, respawn, correction) the walk stops at once.

`config.json` (all optional):

```json
"map": {
   "enabled": true,
   "port": 17171,
   "open_on_start": false,
   "walk_speed": 250,
   "step_interval_ms": 100,
   "gravity": false,
   "jump_height": 3
}
```

`gravity: true` only uses paths you could run and jump along (rising at most
`jump_height` tiles per jump, then falling); the default goes through any free
tile.

## License

This project is licensed under the MIT License. See the [LICENSE](https://github.com/ZTzTopia/GTProxy/blob/main/LICENSE) file for details.
