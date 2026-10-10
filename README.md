<div align="center">

# HL2Minecraft (Half-Life 2 × Minecraft)

**Play Half-Life 2 with full Minecraft mechanics, building, mobs, physics, and inventory rendered directly inside Source Engine's Direct3D 9 world.**

[![English](https://img.shields.io/badge/Language-English_🇬🇧-2ea44f?style=for-the-badge)](README.md)
[![Russian](https://img.shields.io/badge/Язык-Русский_🇷🇺-007ec6?style=for-the-badge)](README.ru.md)

</div>

<details>
<summary><b>🇷🇺 Нажмите здесь, чтобы открыть описание на русском языке / Click to expand Russian version</b></summary>

<br>

**HL2Minecraft** — это мод-композитор, объединяющий **Half-Life 2** и **Minecraft (Fabric 26.3)** в едином игровом процессе в реальном времени. Вы проходите сюжетные карты Half-Life 2 (или любые кастомные карты Source Engine), обладая всеми возможностями игрока из Minecraft: строительством и разрушением блоков, инвентарём, крафтом, полётом на элитрах, оружием, взрывами динамита, мобами и частицами, которые рендерятся напрямую в 3D-сцену DirectX 9 движка Source с честным перекрытием стенами (depth occlusion).

### 🚀 Быстрая установка
1. **Установите Minecraft 26.3 с Fabric Loader:**
   - Установите **Fabric Loader** (`0.19.5+`) и **Fabric API** для версии Minecraft `26.3` (например, через PrismLauncher).
   - Скачайте **`hl2craft-1.0.0.jar`** из раздела **[Releases](../../releases)** и поместите его в папку `mods` вашего инстанса Minecraft.
2. **Установите ReShade (с поддержкой Add-on) в Half-Life 2:**
   - Скачайте **ReShade 6.x with full add-on support** с официального сайта [reshade.me](https://reshade.me/#download).
   - В установщике ReShade выберите исполняемый файл `hl2.exe` в папке вашей игры Half-Life 2 (`Steam\steamapps\common\Half-Life 2\hl2.exe`).
   - Выберите графический API **DirectX 9** и завершите установку.
3. **Запустите Minecraft:**
   - При запуске Minecraft мод **автоматически** найдёт папку Half-Life 2, сам установит/обновит нативный аддон `HL2Minecraft.addon32` (он уже вшит внутрь `.jar` мода) и автоматически запустит Half-Life 2.
4. **Играйте прямо в окне Half-Life 2!**

*(Полная русская документация также доступна по кнопке **[Язык: Русский](README.ru.md)** выше).*

</details>

---

## ✨ Key Features

- **Native Direct3D 9 3D World Rendering inside Half-Life 2:**
  - Minecraft chunk meshes, 3D entity models (mobs, items, projectiles, primed TNT), particles, skybox, and first-person held items/hand are streamed over shared memory (`Local\HL2CraftShm`) and rendered directly inside Half-Life 2's D3D9 pipeline using the hardware Z-buffer.
  - Half-Life 2 walls and props accurately occlude Minecraft blocks and entities.
- **True Two-Way Physics & Collision:**
  - Blocks placed in Minecraft spawn 1:1 physical collision in the Half-Life 2 world, allowing you to pillar up, build bridges, and block NPCs.
  - Half-Life 2 map geometry is scanned in real time via `IEngineTrace`, so Minecraft arrows, ender pearls, Creeper/TNT explosions, falling blocks, and mob pathfinding interact naturally with HL2 walls and floors.
  - Player jump height in HL2 is matched 1:1 to Minecraft (~1.25 blocks) without altering HL2 world gravity, and solid blocks cannot be placed inside the player's hull.
- **Combat & NPC Synchronization:**
  - Live Half-Life 2 NPCs and physics props are mirrored as proxy entities in Minecraft: sword swings, bow/crossbow arrows, and explosions damage HL2 NPCs, while hostile Minecraft mobs (Creepers, Zombies, Skeletons) actively target and attack Half-Life 2 NPCs.
  - Player health, damage, and shield blocking are synchronized between both games.
- **Seamless HUD, GUI & Movement:**
  - Minecraft's HUD, hotbar, inventory (`E`), chat (`T`), and pause menus are composited directly over the Half-Life 2 window.
  - Full support for Creative flight and **Elytra** gliding (with firework boosts) across Half-Life 2 maps.
  - Automatic cleanup of placed blocks and spawned mobs upon map transitions or save reloads in Half-Life 2 (while preserving your Minecraft inventory).

---

## 🚀 Quick Installation (For Players)

1. **Install Minecraft 26.3 with Fabric Loader:**
   - Install **Fabric Loader** (`0.19.5+`) and **Fabric API** for Minecraft `26.3` (e.g., via PrismLauncher).
   - Download **`hl2craft-1.0.0.jar`** from the **[Releases](../../releases)** page and place it into your Minecraft instance's `mods` folder.
2. **Install ReShade (with Add-on Support) into Half-Life 2:**
   - Download **ReShade 6.x with full add-on support** from [reshade.me](https://reshade.me/#download).
   - Point the ReShade installer to `hl2.exe` in your Half-Life 2 directory (`Steam\steamapps\common\Half-Life 2\hl2.exe`).
   - Select **DirectX 9** and finish the installation.
3. **Launch Minecraft:**
   - On startup, the mod **automatically** locates your Half-Life 2 installation, extracts and installs the bundled `HL2Minecraft.addon32` ReShade add-on, and launches Half-Life 2 for you.
   - If Half-Life 2 is installed in a custom directory, an in-game setup screen will prompt you to select the folder with one click.
4. **Play inside the Half-Life 2 Window!**
   - Start a new game or load any map in Half-Life 2 — the Minecraft world and HUD will automatically synchronize with your position.

---

## 🎮 Controls (In Half-Life 2 Window)

| Key | Action |
|---|---|
| `W`, `A`, `S`, `D` | Move |
| `Space` | Jump (Double-tap for Creative Flight / Deploy Elytra in mid-air) |
| `Left Shift` | Sneak / Crouch |
| `Left Ctrl` | Sprint |
| `Left Mouse Button` | Attack / Mine Minecraft blocks |
| `Right Mouse Button` | Place blocks / Use items / Draw bow / Block with Shield |
| `Mouse Wheel` / `1`–`9` | Select hotbar slot |
| `E` | Open Minecraft Inventory |
| `Q` | Drop selected item |
| `F` | Swap item with Offhand |
| `T` / `/` | Open Minecraft Chat / Command input |
| `Esc` | Close open Minecraft GUI / Pause |

---

## 🛠️ Building from Source

The repository is organized into three components:
- `protocol/` — Shared memory protocol header (`hl2craft_protocol.h`).
- `hl2/` — 32-bit C++20 ReShade Add-on and Direct3D 9 world renderer (`HL2Minecraft.addon32`) for Half-Life 2.
- `fabric/` — Fabric client/server mod for Minecraft 26.3 (Java 25).

### 1. Build the C++ Add-on (`HL2Minecraft.addon32`)
Requires a 32-bit MinGW-w64 compiler (`g++.exe` supporting `-m32` and `-std=c++20`, such as `w64devkit`):
```bat
cd hl2
build.bat
```
This compiles `HL2Minecraft.addon32` and automatically copies it into `fabric/src/main/resources/assets/hl2craft/HL2Minecraft.addon32`.

### 2. Build the Fabric Mod (`hl2craft-1.0.0.jar`)
Requires JDK 25:
```bat
cd fabric
gradlew.bat build
```
The compiled mod JAR (with `HL2Minecraft.addon32` bundled inside) will be generated at:
`fabric/build/libs/hl2craft-1.0.0.jar`.
