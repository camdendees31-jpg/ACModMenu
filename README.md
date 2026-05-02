# AC Mod Menu

A world-space, holdable mod menu for **Animal Company** (Wooster Games) on Meta Quest 3S.  
Hold **Y** (left controller) for 0.75 s to open/close.

---

## Deployment — no APKTool needed

The game already has a `nativemods` loader.  
Just build the `.so` and drop it in one folder.

```
Quest 3S  →  Internal shared storage
          →  Android
          →  data
          →  woosterGames.animalCompany
          →  nativemods
          →  libACModMenu.so          ← put it here
```

---

## Option A — Download from GitHub Releases (easiest)

Every time you push a version tag the Actions workflow builds and
publishes the `.so` automatically:

```bash
git tag v1.0.0 && git push --tags
```

Then on the Releases page, download `libACModMenu.so` and copy it to the Quest.

---

## Option B — Build locally

### Prerequisites

| Tool | Get it from |
|------|-------------|
| Android NDK r25c | `sdkmanager "ndk;25.2.9519653"` or Android Studio SDK Manager |
| CMake 3.18+ | bundled with Android Studio, or cmake.org |
| BNM headers | `git clone https://github.com/ByNameModding/BNM-Android BNM` |

```bash
# Clone BNM next to the project files
git clone --depth=1 https://github.com/ByNameModding/BNM-Android BNM

# Configure
cmake -B build \
  -DCMAKE_TOOLCHAIN_FILE=$ANDROID_NDK/build/cmake/android.toolchain.cmake \
  -DANDROID_ABI=arm64-v8a \
  -DANDROID_PLATFORM=android-29 \
  -DANDROID_STL=c++_shared \
  -DCMAKE_BUILD_TYPE=Release

# Build
cmake --build build --config Release

# Output
ls -lh build/libACModMenu.so
```

---

## Option C — GitHub Actions CI (recommended)

Every push to `main` automatically builds and stores the `.so` as a
workflow artifact. Tagged pushes create a full GitHub Release.

### Setup (one time)

1. Push this repository to GitHub.
2. Go to **Settings → Actions → General** and confirm Actions are enabled.
3. That's it — no secrets needed. `GITHUB_TOKEN` is auto-provided.

### Get the built `.so`

**From a regular push:**
```
GitHub → Actions → latest run → Artifacts → libACModMenu-arm64-<sha>
```

**From a release tag:**
```bash
git tag v1.0.0
git push --tags
# Then: GitHub → Releases → v1.0.0 → download libACModMenu.so
```

### Workflow summary

```
push to main / PR
  └─ ubuntu-22.04
       ├─ Checkout (with submodules)
       ├─ NDK r25c setup
       ├─ CMake 3.25 setup
       ├─ Clone BNM headers
       ├─ Clone + build Dobby (static, arm64)
       ├─ cmake configure  (arm64-v8a, Release)
       ├─ cmake build
       ├─ Verify ELF = aarch64 + JNI_OnLoad exported
       └─ Upload artifact (30-day retention)

push tag v*.*.*  (all of the above, plus)
       └─ Create GitHub Release + attach libACModMenu.so
```

---

## Copy to Quest

### Via USB (Windows Explorer — what you already have)

1. Connect Quest 3S via USB.
2. Open `Internal shared storage → Android → data → woosterGames.animalCompany → nativemods`.
3. Drag `libACModMenu.so` into that folder.

### Via ADB (faster for repeated installs)

```bash
adb push build/libACModMenu.so \
  /sdcard/Android/data/woosterGames.animalCompany/nativemods/libACModMenu.so
```

Verify it landed:
```bash
adb shell ls -lh /sdcard/Android/data/woosterGames.animalCompany/nativemods/
```

---

## Verify the mod loaded

```bash
adb logcat -s ACModMenu
```

Expected on game launch:
```
ACModMenu  I  ======================================
ACModMenu  I    ACModMenu JNI_OnLoad
ACModMenu  I  ======================================
ACModMenu  I  WaitForIl2Cpp thread started...
ACModMenu  I  libil2cpp.so detected
ACModMenu  I  IL2CPP API resolved successfully.
ACModMenu  I  BNM initialised.
ACModMenu  I  ACModMenu fully initialised.  Hold Y for 0.75s to open the menu.
```

---

## Using the menu

| Action | Result |
|--------|--------|
| Hold **Y** (left) for 0.75 s | Toggle menu open / closed |
| **Grip** (left) while menu is open | Grab and drag panel in 3D space |
| Release grip | Drop panel; position auto-saved |

Config persisted to:
```
/sdcard/Android/data/woosterGames.animalCompany/files/modmenu_config.json
```

---

## Mod features

| Toggle | What it does |
|--------|-------------|
| Fly Mode | Noclip flight |
| Infinite Money | Injects currency on enable |
| God Mode | Hooks `TakeDamage()` — suppresses all damage |
| Speed Multiplier | Slider 1x–5x, live-applied each frame |
| No Clip | Disables CharacterController collision |

---

## Project layout

```
.
├── .github/
│   └── workflows/
│       └── build.yml       ← CI/CD pipeline
├── mod.cpp                 ← all hooks, input, UI, toggles
├── BNMSetup.h              ← IL2CPP bootstrap & helpers
├── CMakeLists.txt          ← arm64 build config
├── README.md               ← this file
├── BNM/                    ← clone here, or add as submodule
│   └── include/
└── Dobby/                  ← optional, built by CI automatically
    ├── include/
    └── libs/arm64-v8a/
```

### Add BNM as a submodule (optional but recommended)

```bash
git submodule add https://github.com/ByNameModding/BNM-Android BNM
git commit -m "Add BNM submodule"
```

The workflow uses `submodules: recursive` so it clones BNM automatically on every build.

---

## Updating after a game patch

If Animal Company updates, IL2CPP offsets shift. The mod will log
warnings for any method it cannot find but **will not crash the game**.

To restore full functionality:

1. Run `Il2CppDumper` on the new `libil2cpp.so` + `global-metadata.dat`.
2. Search `dump.cs` for the class/method names in `ResolveAllMethods()` in `mod.cpp`.
3. Update any renamed identifiers, rebuild, push a new tag.

---

## Disclaimer

Personal use only. Do not use in online multiplayer.
Modding may violate the game's Terms of Service.
