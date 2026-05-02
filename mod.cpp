// ============================================================
//  mod.cpp  –  Animal Company VR Mod Menu
//  Target : arm64-v8a  |  NDK r23+  |  Unity IL2CPP
//  Package: com.WoosterGames.AnimalCompany
//
//  Architecture overview
//  ─────────────────────
//  1.  JNI_OnLoad       – entry point; initialises IL2CPP/BNM,
//                         installs the Update hook, starts the
//                         config loader thread.
//  2.  UpdateHook       – runs every frame inside Unity's player
//                         loop; polls Y-button, drives hold timer,
//                         updates grab drag, ticks UI animations.
//  3.  MenuController   – creates / destroys the world-space
//                         Canvas hierarchy via IL2CPP calls.
//  4.  ToggleHandlers   – one function per mod feature.
//  5.  ConfigIO         – reads/writes JSON to the app's files/.
// ============================================================

// ── Standard & system headers ───────────────────────────────
#include <jni.h>
#include <android/log.h>
#include <pthread.h>
#include <unistd.h>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cmath>
#include <string>
#include <atomic>
#include <functional>
#include <vector>

// ── Project headers ──────────────────────────────────────────
#include "BNMSetup.h"   // IL2CPP resolution, logging, math structs

// ════════════════════════════════════════════════════════════
//  §1  CONFIGURABLE CONSTANTS
//      Change these values without touching any other code.
// ════════════════════════════════════════════════════════════
#define HOLD_TIME_SEC       0.75f   // seconds Y must be held to toggle menu
#define MENU_SCALE          0.001f  // world-space Canvas scale (metres per UI unit)
#define MENU_WIDTH_PX       480.0f  // Canvas width  in UI pixels
#define MENU_HEIGHT_PX      520.0f  // Canvas height in UI pixels
#define TITLE_BAR_HEIGHT_PX  48.0f  // pixels reserved for the title bar
#define SPEED_MIN           1.0f    // minimum speed multiplier
#define SPEED_MAX           5.0f    // maximum speed multiplier
#define SPEED_DEFAULT       1.0f    // starting speed multiplier value
#define GRAB_TRIGGER_THRESH 0.7f    // left-grip value above which grab activates
// NOTE: real package is woosterGames.animalCompany (lowercase 'w')
#define PKG_DATA_DIR "/sdcard/Android/data/woosterGames.animalCompany"
#define CONFIG_PATH  PKG_DATA_DIR "/files/modmenu_config.json"
#define MODS_DIR     PKG_DATA_DIR "/nativemods"   // where this .so lives

// ════════════════════════════════════════════════════════════
//  §2  GLOBAL MOD STATE
// ════════════════════════════════════════════════════════════

// ── Menu open/close ──────────────────────────────────────────
static std::atomic<bool> g_menuOpen       {false};
static std::atomic<bool> g_yHeld          {false};
static float             g_yHoldTimer     = 0.0f;   // seconds Y has been held

// ── Unity managed object handles ────────────────────────────
//  These are raw Il2CppObject* pointers held as uintptr_t to
//  avoid GC moves invalidating them (they will be pinned via
//  GCHandle in MenuController::Create).
static uintptr_t g_canvasGO      = 0; // root GameObject
static uintptr_t g_gcHandle      = 0; // GCHandle to prevent GC collection

// ── Grab / drag state ────────────────────────────────────────
static bool      g_isGrabbed           = false;
static Vector3   g_grabOffsetLocal     = {0,0,0};   // offset from panel origin at grab
static Vector3   g_menuWorldPos        = {0,0,1.2f}; // persisted position

// ── Toggle feature flags ─────────────────────────────────────
static std::atomic<bool>  g_flyMode    {false};
static std::atomic<bool>  g_infiniteMoney {false};
static std::atomic<bool>  g_godMode    {false};
static std::atomic<float> g_speedMult  {SPEED_DEFAULT};
static std::atomic<bool>  g_noClip     {false};

// ── Delta-time (seconds between frames) ──────────────────────
static float g_deltaTime = 0.016f;  // ~60 fps until real value arrives

// ════════════════════════════════════════════════════════════
//  §3  IL2CPP METHOD CACHE
//      We resolve each method once at init time and cache the
//      native function pointer.  If a pointer is null the
//      feature silently degrades (logged once at load time).
//
//      Offset comments use the Il2CppDumper notation:
//          // Offset: 0xABCDEF
//      Replace the 0x00000000 placeholder with the real offset
//      from your dump of the current game version.  BNM will
//      find the method by name independently of offset, so
//      these comments are informational / for Dobby fallback.
// ════════════════════════════════════════════════════════════
namespace Cache {

    // ── Unity engine methods ─────────────────────────────────
    // UnityEngine.Time.get_deltaTime()
    // Offset: 0x00000000  [patch per dump]
    typedef float (*Time_get_deltaTime_fn)();
    Time_get_deltaTime_fn Time_get_deltaTime = nullptr;

    // UnityEngine.GameObject.AddComponent(Type t) : Component
    // Offset: 0x00000000
    typedef Il2CppObject* (*GameObject_AddComponent_fn)(Il2CppObject* go, Il2CppObject* type);
    GameObject_AddComponent_fn GameObject_AddComponent = nullptr;

    // UnityEngine.GameObject..ctor(string name)
    // Offset: 0x00000000
    typedef void (*GameObject_ctor_fn)(Il2CppObject* go, Il2CppString* name);
    GameObject_ctor_fn GameObject_ctor = nullptr;

    // UnityEngine.Object.DontDestroyOnLoad(Object)
    // Offset: 0x00000000
    typedef void (*DontDestroyOnLoad_fn)(Il2CppObject* obj);
    DontDestroyOnLoad_fn DontDestroyOnLoad = nullptr;

    // UnityEngine.Component.get_transform() : Transform
    // Offset: 0x00000000
    typedef Il2CppObject* (*Component_get_transform_fn)(Il2CppObject* comp);
    Component_get_transform_fn Component_get_transform = nullptr;

    // UnityEngine.Transform.set_position(Vector3)
    // Offset: 0x00000000
    typedef void (*Transform_set_position_fn)(Il2CppObject* t, Vector3 pos);
    Transform_set_position_fn Transform_set_position = nullptr;

    // UnityEngine.Transform.get_position() : Vector3
    // Offset: 0x00000000
    typedef Vector3 (*Transform_get_position_fn)(Il2CppObject* t);
    Transform_get_position_fn Transform_get_position = nullptr;

    // UnityEngine.Transform.set_localScale(Vector3)
    // Offset: 0x00000000
    typedef void (*Transform_set_localScale_fn)(Il2CppObject* t, Vector3 s);
    Transform_set_localScale_fn Transform_set_localScale = nullptr;

    // UnityEngine.Canvas (component setup)
    // Offset: 0x00000000
    typedef void (*Canvas_set_renderMode_fn)(Il2CppObject* canvas, int mode); // 0=Screen 1=Camera 2=World
    Canvas_set_renderMode_fn Canvas_set_renderMode = nullptr;

    // UnityEngine.UI.Text.set_text(string)
    // Offset: 0x00000000
    typedef void (*Text_set_text_fn)(Il2CppObject* text, Il2CppString* value);
    Text_set_text_fn Text_set_text = nullptr;

    // UnityEngine.UI.Text.set_fontSize(int)
    // Offset: 0x00000000
    typedef void (*Text_set_fontSize_fn)(Il2CppObject* text, int size);
    Text_set_fontSize_fn Text_set_fontSize = nullptr;

    // UnityEngine.UI.Image.set_color(Color)
    // Offset: 0x00000000
    typedef void (*Image_set_color_fn)(Il2CppObject* img, Color c);
    Image_set_color_fn Image_set_color = nullptr;

    // UnityEngine.UI.Toggle.set_isOn(bool)
    // Offset: 0x00000000
    typedef void (*Toggle_set_isOn_fn)(Il2CppObject* toggle, bool value);
    Toggle_set_isOn_fn Toggle_set_isOn = nullptr;

    // UnityEngine.UI.Slider.set_value(float)
    // Offset: 0x00000000
    typedef void (*Slider_set_value_fn)(Il2CppObject* slider, float value);
    Slider_set_value_fn Slider_set_value = nullptr;

    // UnityEngine.RectTransform.set_sizeDelta(Vector2)
    // Offset: 0x00000000
    typedef void (*RectTransform_set_sizeDelta_fn)(Il2CppObject* rt, Vector2 sd);
    RectTransform_set_sizeDelta_fn RectTransform_set_sizeDelta = nullptr;

    // UnityEngine.RectTransform.set_anchoredPosition3D(Vector3)
    // Offset: 0x00000000
    typedef void (*RectTransform_set_anchoredPosition3D_fn)(Il2CppObject* rt, Vector3 p);
    RectTransform_set_anchoredPosition3D_fn RectTransform_set_anchoredPosition3D = nullptr;

    // ── XR Input ─────────────────────────────────────────────
    // UnityEngine.XR.InputDevice.TryGetFeatureValue(InputFeatureUsage<bool>, out bool) : bool
    // Offset: 0x00000000
    typedef bool (*InputDevice_TryGetFeatureValue_bool_fn)(
        Il2CppObject* device, Il2CppObject* usage, bool* value);
    InputDevice_TryGetFeatureValue_bool_fn InputDevice_TryGetFeatureValue_bool = nullptr;

    // UnityEngine.XR.InputDevice.TryGetFeatureValue(InputFeatureUsage<float>, out float) : bool
    // Offset: 0x00000000
    typedef bool (*InputDevice_TryGetFeatureValue_float_fn)(
        Il2CppObject* device, Il2CppObject* usage, float* value);
    InputDevice_TryGetFeatureValue_float_fn InputDevice_TryGetFeatureValue_float = nullptr;

    // UnityEngine.XR.InputDevices.GetDevicesWithCharacteristics(
    //      InputDeviceCharacteristics, List<InputDevice>)
    // Offset: 0x00000000
    typedef void (*InputDevices_GetDevicesWithCharacteristics_fn)(
        uint32_t characteristics, Il2CppObject* results);
    InputDevices_GetDevicesWithCharacteristics_fn InputDevices_GetDevicesWithChar = nullptr;

    // UnityEngine.XR.InputDevice.TryGetFeatureValue(InputFeatureUsage<Vector3>, out Vector3)
    // Offset: 0x00000000
    typedef bool (*InputDevice_TryGetFeatureValue_vec3_fn)(
        Il2CppObject* device, Il2CppObject* usage, Vector3* value);
    InputDevice_TryGetFeatureValue_vec3_fn InputDevice_TryGetFeatureValue_vec3 = nullptr;

    // ── Game-specific methods ─────────────────────────────────
    // AnimalCompany.PlayerController.SetFlying(bool) [hypothetical name]
    // Offset: 0x00000000
    typedef void (*PlayerController_SetFlying_fn)(Il2CppObject* player, bool state);
    PlayerController_SetFlying_fn PlayerController_SetFlying = nullptr;

    // AnimalCompany.CurrencyManager.AddMoney(int) [hypothetical name]
    // Offset: 0x00000000
    typedef void (*CurrencyManager_AddMoney_fn)(Il2CppObject* mgr, int amount);
    CurrencyManager_AddMoney_fn CurrencyManager_AddMoney = nullptr;

    // AnimalCompany.HealthComponent.TakeDamage(float)
    // Offset: 0x00000000
    typedef void (*HealthComponent_TakeDamage_fn)(Il2CppObject* hp, float dmg);
    HealthComponent_TakeDamage_fn HealthComponent_TakeDamage_original = nullptr;

    // AnimalCompany.CharacterMovement.get_moveSpeed() : float
    // Offset: 0x00000000
    typedef float (*CharacterMovement_getMoveSpeed_fn)(Il2CppObject* mov);
    CharacterMovement_getMoveSpeed_fn CharacterMovement_getMoveSpeed = nullptr;

    // AnimalCompany.CharacterMovement.set_moveSpeed(float)
    // Offset: 0x00000000
    typedef void (*CharacterMovement_setMoveSpeed_fn)(Il2CppObject* mov, float s);
    CharacterMovement_setMoveSpeed_fn CharacterMovement_setMoveSpeed = nullptr;

    // GC handle API – prevents managed objects from being collected
    // Offset: 0x00000000
    typedef uint32_t (*GCHandle_Alloc_fn)(Il2CppObject* obj, bool pin);
    GCHandle_Alloc_fn GCHandle_Alloc = nullptr;

    typedef void (*GCHandle_Free_fn)(uint32_t handle);
    GCHandle_Free_fn GCHandle_Free = nullptr;

    typedef Il2CppObject* (*GCHandle_GetTarget_fn)(uint32_t handle);
    GCHandle_GetTarget_fn GCHandle_GetTarget = nullptr;

} // namespace Cache

// ════════════════════════════════════════════════════════════
//  §4  FORWARD DECLARATIONS
// ════════════════════════════════════════════════════════════
namespace MenuController { void Create(); void Destroy(); void UpdateGrab(); }
namespace ConfigIO       { void Load();   void Save();                        }
namespace ToggleHandlers {
    void OnFlyMode(bool on);
    void OnInfiniteMoney(bool on);
    void OnGodMode(bool on);
    void OnSpeedMult(float v);
    void OnNoClip(bool on);
}

// ════════════════════════════════════════════════════════════
//  §5  INIT HELPERS – resolve Cache:: pointers via BNM / dlsym
// ════════════════════════════════════════════════════════════

// Thin wrapper: look up a method in a class by name and arg count,
// return its native function pointer, or null with a warning.
static void* ResolveMethod(const char* namespaceName,
                            const char* className,
                            const char* methodName,
                            int         argCount)
{
#if BNM_AVAILABLE
    auto cls = BNM::Class(namespaceName, className);
    if (!cls) {
        LOGW("ResolveMethod: class %s::%s not found", namespaceName, className);
        return nullptr;
    }
    auto method = cls.GetMethod(methodName, argCount);
    if (!method) {
        LOGW("ResolveMethod: method %s::%s::%s(%d) not found",
             namespaceName, className, methodName, argCount);
        return nullptr;
    }
    void* ptr = method.GetPointer();
    LOGD("Resolved %s::%s::%s -> %p", namespaceName, className, methodName, ptr);
    return ptr;
#else
    // Fallback: use the raw IL2CPP API we resolved in BNMSetup
    if (!IL2CPP::class_from_name || !IL2CPP::get_method_from_name) return nullptr;
    // Note: class_from_name needs an assembly image; here we iterate through
    // all assemblies.  For brevity we search the main "Assembly-CSharp" image.
    // In production, iterate il2cpp_domain_get_assemblies() for robustness.
    void* assemblyImage = nullptr; // would be resolved from domain assemblies
    Il2CppClass* cls = IL2CPP::class_from_name(assemblyImage, namespaceName, className);
    if (!cls) { LOGW("Fallback: class %s::%s not found", namespaceName, className); return nullptr; }
    void* method = IL2CPP::get_method_from_name(cls, methodName, argCount);
    if (!method) { LOGW("Fallback: method %s not found", methodName); return nullptr; }
    return IL2CPP::method_get_pointer ? IL2CPP::method_get_pointer(method) : nullptr;
#endif
}

// Macro: resolve and store in Cache::; logs once on failure.
#define CACHE_METHOD(NS, CLS, METH, ARGC, DEST) \
    do { \
        void* _p = ResolveMethod(NS, CLS, METH, ARGC); \
        if (_p) Cache::DEST = reinterpret_cast<decltype(Cache::DEST)>(_p); \
        else    LOGW("Cache miss: " NS "::" CLS "::" METH); \
    } while(0)

static void ResolveAllMethods() {
    LOGI("=== Resolving IL2CPP method cache ===");

    // Unity engine
    CACHE_METHOD("UnityEngine",    "Time",          "get_deltaTime",              0, Time_get_deltaTime);
    CACHE_METHOD("UnityEngine",    "GameObject",    "AddComponent",               1, GameObject_AddComponent);
    CACHE_METHOD("UnityEngine",    "GameObject",    ".ctor",                      1, GameObject_ctor);
    CACHE_METHOD("UnityEngine",    "Object",        "DontDestroyOnLoad",          1, DontDestroyOnLoad);
    CACHE_METHOD("UnityEngine",    "Component",     "get_transform",              0, Component_get_transform);
    CACHE_METHOD("UnityEngine",    "Transform",     "set_position_Injected",      2, Transform_set_position);
    CACHE_METHOD("UnityEngine",    "Transform",     "get_position_Injected",      1, Transform_get_position);
    CACHE_METHOD("UnityEngine",    "Transform",     "set_localScale_Injected",    2, Transform_set_localScale);
    CACHE_METHOD("UnityEngine",    "Canvas",        "set_renderMode",             1, Canvas_set_renderMode);
    CACHE_METHOD("UnityEngine.UI", "Text",          "set_text",                   1, Text_set_text);
    CACHE_METHOD("UnityEngine.UI", "Text",          "set_fontSize",               1, Text_set_fontSize);
    CACHE_METHOD("UnityEngine.UI", "Image",         "set_color",                  1, Image_set_color);
    CACHE_METHOD("UnityEngine.UI", "Toggle",        "set_isOn",                   1, Toggle_set_isOn);
    CACHE_METHOD("UnityEngine.UI", "Slider",        "set_value",                  1, Slider_set_value);
    CACHE_METHOD("UnityEngine",    "RectTransform", "set_sizeDelta_Injected",     2, RectTransform_set_sizeDelta);
    CACHE_METHOD("UnityEngine",    "RectTransform", "set_anchoredPosition3D_Injected", 2, RectTransform_set_anchoredPosition3D);

    // XR Input
    CACHE_METHOD("UnityEngine.XR", "InputDevices",
                 "GetDevicesWithCharacteristics", 2, InputDevices_GetDevicesWithChar);

    // GC handle management
    CACHE_METHOD("System.Runtime.InteropServices",
                 "GCHandle", "Alloc", 2, GCHandle_Alloc);
    CACHE_METHOD("System.Runtime.InteropServices",
                 "GCHandle", "Free",  1, GCHandle_Free);
    CACHE_METHOD("System.Runtime.InteropServices",
                 "GCHandle", "GetTarget", 1, GCHandle_GetTarget);

    // Game-specific  (names are best guesses; verify against your dump)
    CACHE_METHOD("AnimalCompany", "PlayerController",   "SetFlying",     1, PlayerController_SetFlying);
    CACHE_METHOD("AnimalCompany", "CurrencyManager",    "AddMoney",      1, CurrencyManager_AddMoney);
    CACHE_METHOD("AnimalCompany", "HealthComponent",    "TakeDamage",    1, HealthComponent_TakeDamage_original);
    CACHE_METHOD("AnimalCompany", "CharacterMovement",  "get_moveSpeed", 0, CharacterMovement_getMoveSpeed);
    CACHE_METHOD("AnimalCompany", "CharacterMovement",  "set_moveSpeed", 1, CharacterMovement_setMoveSpeed);

    LOGI("=== Method cache complete ===");
}

// ════════════════════════════════════════════════════════════
//  §6  XR INPUT HELPERS
//      Thin wrappers around Unity's XR Input System so the
//      rest of the code reads clearly.
// ════════════════════════════════════════════════════════════

// InputDeviceCharacteristics flags (mirrors Unity's enum values)
static const uint32_t kCharLeft       = 0x004; // Left
static const uint32_t kCharController = 0x800; // Controller
static const uint32_t kCharLeftCtrl   = kCharLeft | kCharController;

// We cache the left-controller InputDevice managed object across frames.
static uintptr_t g_leftControllerGCHandle = 0;   // GCHandle

// Obtain (or refresh) a handle to the left XR controller device.
static Il2CppObject* GetLeftController() {
    if (g_leftControllerGCHandle && Cache::GCHandle_GetTarget) {
        Il2CppObject* obj = Cache::GCHandle_GetTarget(
            static_cast<uint32_t>(g_leftControllerGCHandle));
        if (obj) return obj;
    }

    if (!Cache::InputDevices_GetDevicesWithChar || !IL2CPP::object_new) return nullptr;

    // Create a List<InputDevice> to receive results
    // (In a full implementation, resolve the generic List<T> class properly.)
    // For the prototype we use an icall-based approach via InputDevice value type.
    // The detailed list manipulation is engine-version specific; here we leave
    // a safe stub and document where to extend it.
    LOGD("GetLeftController: refreshing device list (stub – extend per engine version)");
    return nullptr;
}

// Poll a boolean feature (button press/touch) from the left controller.
// Returns false if the device or API is unavailable.
static bool GetButton(const char* usageName) {
    Il2CppObject* dev = GetLeftController();
    if (!dev || !Cache::InputDevice_TryGetFeatureValue_bool) return false;

    // Resolve the InputFeatureUsage<bool> for the given usage name
    // (In the full impl, look up CommonUsages.<usageName> static field.)
    // Stub: always returns false until the feature-usage lookup is wired up.
    (void)usageName;
    bool value = false;
    // Cache::InputDevice_TryGetFeatureValue_bool(dev, usageObj, &value);
    return value;
}

// Poll a float axis from the left controller (e.g. grip, trigger).
static float GetAxis(const char* usageName) {
    Il2CppObject* dev = GetLeftController();
    if (!dev || !Cache::InputDevice_TryGetFeatureValue_float) return 0.0f;
    (void)usageName;
    float value = 0.0f;
    // Cache::InputDevice_TryGetFeatureValue_float(dev, usageObj, &value);
    return value;
}

// Poll the left-hand 3D position (for menu spawning).
static Vector3 GetLeftHandPosition() {
    Il2CppObject* dev = GetLeftController();
    if (!dev || !Cache::InputDevice_TryGetFeatureValue_vec3) return {0,1.2f,0};
    Vector3 pos = {0, 1.2f, 0};
    // Cache::InputDevice_TryGetFeatureValue_vec3(dev, devicePositionUsage, &pos);
    return pos;
}

// ════════════════════════════════════════════════════════════
//  §7  CONFIG I/O
//      Minimal JSON read/write with no external dependencies.
//      We use manual string building rather than a JSON library
//      to keep the binary small and dependency-free.
// ════════════════════════════════════════════════════════════
namespace ConfigIO {

    static bool TryParseFloat(const char* json, const char* key, float& out) {
        char search[128];
        snprintf(search, sizeof(search), "\"%s\"", key);
        const char* p = strstr(json, search);
        if (!p) return false;
        p = strchr(p, ':');
        if (!p) return false;
        out = strtof(p + 1, nullptr);
        return true;
    }

    void Load() {
        FILE* f = fopen(CONFIG_PATH, "r");
        if (!f) {
            LOGI("Config not found at %s – using defaults.", CONFIG_PATH);
            return;
        }
        char buf[512] = {};
        fread(buf, 1, sizeof(buf) - 1, f);
        fclose(f);

        float x = g_menuWorldPos.x;
        float y = g_menuWorldPos.y;
        float z = g_menuWorldPos.z;
        TryParseFloat(buf, "pos_x", x);
        TryParseFloat(buf, "pos_y", y);
        TryParseFloat(buf, "pos_z", z);
        g_menuWorldPos = {x, y, z};

        float spd = SPEED_DEFAULT;
        TryParseFloat(buf, "speed_mult", spd);
        g_speedMult.store(spd);

        LOGI("Config loaded: pos=(%.2f,%.2f,%.2f) speed=%.2f",
             x, y, z, spd);
    }

    void Save() {
        // Best-effort write; do not crash if the path is read-only.
        FILE* f = fopen(CONFIG_PATH, "w");
        if (!f) {
            LOGW("Config: cannot write to %s", CONFIG_PATH);
            return;
        }
        fprintf(f,
            "{\n"
            "  \"pos_x\": %.4f,\n"
            "  \"pos_y\": %.4f,\n"
            "  \"pos_z\": %.4f,\n"
            "  \"speed_mult\": %.4f,\n"
            "  \"fly_mode\": %s,\n"
            "  \"god_mode\": %s,\n"
            "  \"no_clip\": %s\n"
            "}\n",
            g_menuWorldPos.x,
            g_menuWorldPos.y,
            g_menuWorldPos.z,
            g_speedMult.load(),
            g_flyMode.load()       ? "true" : "false",
            g_godMode.load()       ? "true" : "false",
            g_noClip.load()        ? "true" : "false"
        );
        fclose(f);
        LOGI("Config saved to %s", CONFIG_PATH);
    }
} // namespace ConfigIO

// ════════════════════════════════════════════════════════════
//  §8  MENU CONTROLLER
//      Creates a world-space Unity Canvas holding all UI panels.
//      Everything here is thin IL2CPP API calls; real game
//      patches happen in §9 (ToggleHandlers).
//
//      Layout (Y = 0 at centre of Canvas):
//        ┌──────────────────────────┐  ← title bar   (+y)
//        │   AC Mod Menu       [X]  │
//        ├──────────────────────────┤
//        │ [✓] Fly Mode             │
//        │ [✓] Infinite Money       │
//        │ [✓] God Mode             │
//        │ ─── Speed ──[slider]──── │
//        │ [✓] No Clip              │
//        └──────────────────────────┘  (-y)
// ════════════════════════════════════════════════════════════
namespace MenuController {

    // ── Internal helpers ─────────────────────────────────────
    // All IL2CPP calls are guarded: if a cache pointer is null the
    // call is skipped and a warning is logged once per session.

    static Il2CppObject* g_canvasObjPtr  = nullptr; // raw ptr (also in g_gcHandle)
    static Il2CppObject* g_canvasTransform = nullptr;

    // Pin a managed object so the GC does not move or collect it.
    static void PinObject(Il2CppObject* obj, uintptr_t& handleOut) {
        if (!Cache::GCHandle_Alloc || !obj) return;
        handleOut = Cache::GCHandle_Alloc(obj, /*pin=*/true);
    }

    // Retrieve the pinned object from a GCHandle.
    static Il2CppObject* GetPinned(uintptr_t handle) {
        if (!handle || !Cache::GCHandle_GetTarget) return nullptr;
        return Cache::GCHandle_GetTarget(static_cast<uint32_t>(handle));
    }

    // Unpin and clear a GCHandle.
    static void FreePin(uintptr_t& handle) {
        if (handle && Cache::GCHandle_Free) {
            Cache::GCHandle_Free(static_cast<uint32_t>(handle));
            handle = 0;
        }
    }

    // Create a new named GameObject and optionally attach it to a parent.
    // Returns the Transform of the new object.
    static Il2CppObject* NewGameObject(const char* name,
                                        Il2CppObject* parentTransform = nullptr) {
        if (!Cache::GameObject_ctor || !IL2CPP::object_new || !IL2CPP::string_new)
            return nullptr;

        // Allocate and construct the managed GameObject
        Il2CppClass* goClass = nullptr;
#if BNM_AVAILABLE
        goClass = BNM::Class("UnityEngine", "GameObject").GetIl2CppClass();
#endif
        if (!goClass) return nullptr;
        Il2CppObject* go = IL2CPP::object_new(goClass);
        if (!go) return nullptr;
        Cache::GameObject_ctor(go, IL2CPP::string_new(name));

        // Get the transform and optionally re-parent
        if (!Cache::Component_get_transform) return go;
        Il2CppObject* t = Cache::Component_get_transform(go);
        if (parentTransform && t) {
            // SetParent – resolve lazily (omitted for brevity, log if needed)
            LOGD("NewGameObject: parent not wired (SetParent stub)");
        }
        return go;
    }

    // Spawn and configure the world-space Canvas root.
    void Create() {
        LOGI("MenuController::Create() – spawning world-space Canvas");

        if (g_canvasObjPtr) {
            LOGW("Menu already exists; skipping Create()");
            return;
        }

        // ── 1. Root GameObject ───────────────────────────────
        Il2CppObject* rootGO = NewGameObject("ACModMenuRoot");
        if (!rootGO) {
            LOGE("Failed to create root GameObject – aborting menu spawn");
            return;
        }
        PinObject(rootGO, g_canvasGO);
        g_canvasObjPtr = rootGO;

        // Keep the menu alive across scene loads
        if (Cache::DontDestroyOnLoad) Cache::DontDestroyOnLoad(rootGO);

        // ── 2. Attach Canvas component ───────────────────────
        //     renderMode 2 = WorldSpace
#if BNM_AVAILABLE
        auto canvasClass = BNM::Class("UnityEngine", "Canvas");
        Il2CppObject* canvas = nullptr;
        if (canvasClass && Cache::GameObject_AddComponent) {
            canvas = Cache::GameObject_AddComponent(rootGO,
                reinterpret_cast<Il2CppObject*>(canvasClass.GetIl2CppClass()));
        }
        if (canvas && Cache::Canvas_set_renderMode)
            Cache::Canvas_set_renderMode(canvas, 2);
#endif

        // ── 3. Size & scale the Canvas ───────────────────────
        if (Cache::Component_get_transform) {
            g_canvasTransform = Cache::Component_get_transform(rootGO);
            if (g_canvasTransform) {
                // Position: above the left hand
                Vector3 spawnPos = GetLeftHandPosition();
                spawnPos.y += 0.15f; // 15 cm above palm
                if (Cache::Transform_set_position)
                    Cache::Transform_set_position(g_canvasTransform, spawnPos);
                g_menuWorldPos = spawnPos;

                // Scale: MENU_SCALE converts UI pixels to world metres
                if (Cache::Transform_set_localScale)
                    Cache::Transform_set_localScale(g_canvasTransform,
                        {MENU_SCALE, MENU_SCALE, MENU_SCALE});
            }
        }

        // ── 4. Title bar background ──────────────────────────
        //  (Full UI hierarchy construction requires RectTransform, Image,
        //   and Text components on child GameObjects.  The pattern below
        //   is identical for each child; it is shown in full for the title
        //   bar and abbreviated for the toggles to keep file size manageable.)
        Il2CppObject* titleBarGO = NewGameObject("TitleBar", g_canvasTransform);
        if (titleBarGO) {
#if BNM_AVAILABLE
            auto imageClass = BNM::Class("UnityEngine.UI", "Image");
            if (imageClass && Cache::GameObject_AddComponent) {
                Il2CppObject* img = Cache::GameObject_AddComponent(titleBarGO,
                    reinterpret_cast<Il2CppObject*>(imageClass.GetIl2CppClass()));
                // Dark blue background
                if (img && Cache::Image_set_color)
                    Cache::Image_set_color(img, {0.10f, 0.15f, 0.30f, 0.95f});
            }
            auto textClass = BNM::Class("UnityEngine.UI", "Text");
            if (textClass && Cache::GameObject_AddComponent && IL2CPP::string_new) {
                Il2CppObject* lbl = Cache::GameObject_AddComponent(titleBarGO,
                    reinterpret_cast<Il2CppObject*>(textClass.GetIl2CppClass()));
                if (lbl) {
                    if (Cache::Text_set_text)
                        Cache::Text_set_text(lbl, IL2CPP::string_new("AC Mod Menu"));
                    if (Cache::Text_set_fontSize)
                        Cache::Text_set_fontSize(lbl, 24);
                }
            }
#endif
        }

        // ── 5. Toggle rows ───────────────────────────────────
        //  Each feature gets a child GO with a Toggle component.
        //  We store the raw pointers so UpdateGrab can hit-test them.
        struct ToggleRow { const char* label; bool initState; };
        static const ToggleRow rows[] = {
            {"Fly Mode",        false},
            {"Infinite Money",  false},
            {"God Mode",        false},
            {"No Clip",         false},
        };
        for (int i = 0; i < 4; ++i) {
            Il2CppObject* rowGO = NewGameObject(rows[i].label, g_canvasTransform);
            if (!rowGO) continue;
#if BNM_AVAILABLE
            auto toggleClass = BNM::Class("UnityEngine.UI", "Toggle");
            if (toggleClass && Cache::GameObject_AddComponent) {
                Il2CppObject* tog = Cache::GameObject_AddComponent(rowGO,
                    reinterpret_cast<Il2CppObject*>(toggleClass.GetIl2CppClass()));
                if (tog && Cache::Toggle_set_isOn)
                    Cache::Toggle_set_isOn(tog, rows[i].initState);
            }
#endif
        }

        // ── 6. Speed slider row ──────────────────────────────
        Il2CppObject* sliderGO = NewGameObject("SpeedSlider", g_canvasTransform);
        if (sliderGO) {
#if BNM_AVAILABLE
            auto sliderClass = BNM::Class("UnityEngine.UI", "Slider");
            if (sliderClass && Cache::GameObject_AddComponent) {
                Il2CppObject* sld = Cache::GameObject_AddComponent(sliderGO,
                    reinterpret_cast<Il2CppObject*>(sliderClass.GetIl2CppClass()));
                if (sld && Cache::Slider_set_value)
                    Cache::Slider_set_value(sld, g_speedMult.load());
            }
#endif
        }

        // ── 7. Close button row ──────────────────────────────
        //  (Button onClick delegate wiring omitted here; a real impl
        //   would call Button.onClick.AddListener via a delegate thunk.)
        NewGameObject("CloseButton", g_canvasTransform);

        LOGI("MenuController: Canvas hierarchy created.");
        ConfigIO::Load();           // restore last saved position
        if (g_canvasTransform && Cache::Transform_set_position)
            Cache::Transform_set_position(g_canvasTransform, g_menuWorldPos);
    }

    // Destroy the Canvas and release the GC pin.
    void Destroy() {
        LOGI("MenuController::Destroy()");
        // SetActive(false) is safer than Destroy() during gameplay;
        // find and call it via BNM.
#if BNM_AVAILABLE
        auto goClass = BNM::Class("UnityEngine", "GameObject");
        auto setActive = goClass.GetMethod("SetActive", 1);
        Il2CppObject* rootGO = GetPinned(g_canvasGO);
        if (setActive && rootGO) {
            auto fn = reinterpret_cast<void(*)(Il2CppObject*, bool)>(
                setActive.GetPointer());
            if (fn) fn(rootGO, false);
        }
#endif
        ConfigIO::Save();   // persist position before hiding
        FreePin(g_canvasGO);
        g_canvasObjPtr    = nullptr;
        g_canvasTransform = nullptr;
    }

    // Called every frame while the menu is open.
    // Handles grabbing the panel and dragging it in world space.
    void UpdateGrab() {
        if (!g_menuOpen || !g_canvasTransform) return;

        float grip = GetAxis("gripButton");   // 0.0–1.0

        if (!g_isGrabbed && grip > GRAB_TRIGGER_THRESH) {
            // ── Start grab ───────────────────────────────────
            g_isGrabbed = true;
            Vector3 handPos  = GetLeftHandPosition();
            Vector3 panelPos = g_menuWorldPos;
            g_grabOffsetLocal = {
                panelPos.x - handPos.x,
                panelPos.y - handPos.y,
                panelPos.z - handPos.z
            };
            LOGD("Grab started. offset=(%.2f,%.2f,%.2f)",
                 g_grabOffsetLocal.x, g_grabOffsetLocal.y, g_grabOffsetLocal.z);
        }
        else if (g_isGrabbed && grip <= GRAB_TRIGGER_THRESH) {
            // ── Release grab ─────────────────────────────────
            g_isGrabbed = false;
            ConfigIO::Save();   // persist new position immediately on release
            LOGD("Grab released. new pos=(%.2f,%.2f,%.2f)",
                 g_menuWorldPos.x, g_menuWorldPos.y, g_menuWorldPos.z);
        }

        if (g_isGrabbed) {
            // ── Drag: follow hand + offset ───────────────────
            Vector3 handPos = GetLeftHandPosition();
            g_menuWorldPos  = {
                handPos.x + g_grabOffsetLocal.x,
                handPos.y + g_grabOffsetLocal.y,
                handPos.z + g_grabOffsetLocal.z
            };
            if (Cache::Transform_set_position)
                Cache::Transform_set_position(g_canvasTransform, g_menuWorldPos);
        }
    }

} // namespace MenuController

// ════════════════════════════════════════════════════════════
//  §9  TOGGLE HANDLERS
//      Each function is called when the corresponding UI
//      element changes state.  Real IL2CPP patches go here.
// ════════════════════════════════════════════════════════════
namespace ToggleHandlers {

    // ── God Mode hook variables ──────────────────────────────
    //  We hook HealthComponent::TakeDamage and zero the damage
    //  argument when g_godMode is true.
    static void* orig_TakeDamage = nullptr;

    // The replacement function – must match the original signature exactly.
    static void Hook_TakeDamage(Il2CppObject* self, float damage) {
        if (g_godMode.load()) {
            LOGD("God Mode: suppressed %.1f damage", damage);
            return;  // swallow the damage call entirely
        }
        // Call the original implementation
        if (orig_TakeDamage)
            reinterpret_cast<decltype(&Hook_TakeDamage)>(orig_TakeDamage)(self, damage);
    }

    // ────────────────────────────────────────────────────────
    void OnFlyMode(bool on) {
        g_flyMode.store(on);
        LOGI("[FlyMode] %s", on ? "ON" : "OFF");

        // Attempt to call PlayerController.SetFlying if cached
        // We need a reference to the local player instance.
        // For now: find via FindObjectOfType or a cached singleton ref.
        if (Cache::PlayerController_SetFlying) {
            // TODO: obtain player instance reference (cached in §11)
            LOGD("FlyMode: PlayerController.SetFlying(%d) – player ref TBD", (int)on);
        } else {
            LOGW("FlyMode: PlayerController::SetFlying not resolved");
        }
    }

    void OnInfiniteMoney(bool on) {
        g_infiniteMoney.store(on);
        LOGI("[InfiniteMoney] %s", on ? "ON" : "OFF");

        if (on && Cache::CurrencyManager_AddMoney) {
            // Pump a large amount once when enabled.
            // CurrencyManager instance: same caveat as above.
            LOGD("InfiniteMoney: CurrencyManager.AddMoney(999999) – mgr ref TBD");
        }
    }

    void OnGodMode(bool on) {
        g_godMode.store(on);
        LOGI("[GodMode] %s", on ? "ON" : "OFF");

        // Install the TakeDamage hook on first enable
        static bool hookInstalled = false;
        if (!hookInstalled && Cache::HealthComponent_TakeDamage_original) {
            hookInstalled = SafeHook(
                reinterpret_cast<void*>(Cache::HealthComponent_TakeDamage_original),
                reinterpret_cast<void*>(Hook_TakeDamage),
                &orig_TakeDamage,
                "GodMode/TakeDamage"
            );
        }
    }

    void OnSpeedMult(float v) {
        // Clamp to configured range
        if (v < SPEED_MIN) v = SPEED_MIN;
        if (v > SPEED_MAX) v = SPEED_MAX;
        g_speedMult.store(v);
        LOGI("[SpeedMult] %.2fx", v);

        // Apply live if we have a player movement reference
        // (CharacterMovement cached instance – see §11)
        if (Cache::CharacterMovement_setMoveSpeed) {
            LOGD("SpeedMult: set_moveSpeed(%.2f) – movmnt ref TBD", v);
        }
    }

    void OnNoClip(bool on) {
        g_noClip.store(on);
        LOGI("[NoClip] %s", on ? "ON" : "OFF");
        // NoClip typically requires disabling the CharacterController component
        // or setting the collision detection mode.
        // Resolve UnityEngine.CharacterController.enabled field and set it here.
        LOGD("NoClip: CharacterController.enabled toggle – impl TBD");
    }

} // namespace ToggleHandlers

// ════════════════════════════════════════════════════════════
//  §10  UNITY UPDATE HOOK
//       We hook the static method
//         UnityEngine.MonoBehaviour.Update   (or the player loop
//         equivalent in newer Unity versions).
//
//       Alternative target: UnityEngine.PlayerLoop.Update
//       which is version-independent.  BNM can find either.
// ════════════════════════════════════════════════════════════

// Original function pointer (filled by the hook library)
static void (*orig_UnityUpdate)(Il2CppObject*) = nullptr;

// ── Per-frame logic ──────────────────────────────────────────
static void Hook_Update(Il2CppObject* self) {
    // Always forward to the original first to avoid stalls
    if (orig_UnityUpdate) orig_UnityUpdate(self);

    // ── Delta time ──────────────────────────────────────────
    if (Cache::Time_get_deltaTime)
        g_deltaTime = Cache::Time_get_deltaTime();

    // ── Y-button hold detection ──────────────────────────────
    //  "SecondaryButton" = Y on the left Oculus controller.
    bool yPressed = GetButton("secondaryButton");
    if (yPressed) {
        g_yHoldTimer += g_deltaTime;
        if (!g_yHeld && g_yHoldTimer >= HOLD_TIME_SEC) {
            g_yHeld = true;
            // Toggle the menu
            if (!g_menuOpen.load()) {
                g_menuOpen.store(true);
                MenuController::Create();
                LOGI("Menu OPENED");
            } else {
                g_menuOpen.store(false);
                MenuController::Destroy();
                LOGI("Menu CLOSED");
            }
        }
    } else {
        // Button released – reset hold state
        g_yHoldTimer = 0.0f;
        g_yHeld      = false;
    }

    // ── Per-frame menu maintenance ───────────────────────────
    if (g_menuOpen.load()) {
        MenuController::UpdateGrab();
    }
}

// ────────────────────────────────────────────────────────────
// Install the Update hook via BNM (preferred) or Dobby.
// This is deferred until the IL2CPP runtime is warm (§11).
// ────────────────────────────────────────────────────────────
static bool InstallUpdateHook() {
#if BNM_AVAILABLE
    // Hook any MonoBehaviour subclass's Update; here we hook a
    // game-specific one so we only run once per player frame.
    // Fallback: hook UnityEngine.MonoBehaviour Update directly.
    auto cls = BNM::Class("AnimalCompany", "GameManager"); // adjust class name
    if (!cls) {
        LOGW("GameManager not found; falling back to MonoBehaviour.Update hook");
        cls = BNM::Class("UnityEngine", "MonoBehaviour");
    }
    auto meth = cls.GetMethod("Update", 0);
    if (!meth) {
        LOGE("Update method not found – hook not installed!");
        return false;
    }
    void* targetPtr = meth.GetPointer();
    return SafeHook(targetPtr,
                    reinterpret_cast<void*>(Hook_Update),
                    reinterpret_cast<void**>(&orig_UnityUpdate),
                    "Update");
#else
    // Dobby-only path: use a known offset from dump (fill in real offset)
    // Example: uintptr_t base = (uintptr_t)dlopen("libil2cpp.so", RTLD_NOLOAD);
    //          void* target   = (void*)(base + 0xDEADBEEF);
    LOGW("BNM unavailable; Update hook requires manual offset patching.");
    return false;
#endif
}

// ════════════════════════════════════════════════════════════
//  §11  IL2CPP-READY THREAD
//       libil2cpp.so is loaded asynchronously by the game.
//       We spin on a background thread until it appears in
//       /proc/self/maps, then trigger our initialisation.
// ════════════════════════════════════════════════════════════
static void* WaitForIl2CppThread(void*) {
    LOGI("WaitForIl2Cpp thread started – polling for libil2cpp.so...");

    // Poll until the library appears in the process address space
    for (int attempt = 0; attempt < 300; ++attempt) {
        void* handle = dlopen("libil2cpp.so", RTLD_NOW | RTLD_NOLOAD);
        if (handle) {
            dlclose(handle);
            break;
        }
        usleep(200'000); // 200 ms between polls
    }

    // Give Unity's runtime a moment to finish internal initialisation
    usleep(500'000); // 500 ms buffer

    LOGI("libil2cpp.so detected – starting mod initialisation");

    // ── Initialise IL2CPP API ────────────────────────────────
    if (!IL2CPP::Init()) {
        LOGE("IL2CPP::Init() failed – aborting mod load");
        return nullptr;
    }

#if BNM_AVAILABLE
    // BNM needs a warm IL2CPP domain; wait for il2cpp_domain_get() != null
    for (int i = 0; i < 100; ++i) {
        if (IL2CPP::domain_get && IL2CPP::domain_get()) break;
        usleep(100'000);
    }
    BNM::OnLoad(); // BNM internal metadata scan
    LOGI("BNM initialised.");
#endif

    // ── Resolve method cache ─────────────────────────────────
    ResolveAllMethods();

    // ── Install hooks ────────────────────────────────────────
    if (!InstallUpdateHook()) {
        LOGE("Could not install Update hook – input polling disabled.");
    }

    // ── Pre-load config ──────────────────────────────────────
    ConfigIO::Load();

    LOGI("ACModMenu fully initialised.  Hold Y for %.2fs to open the menu.",
         HOLD_TIME_SEC);
    return nullptr;
}

// ════════════════════════════════════════════════════════════
//  §12  JNI ENTRY POINT
//       Called by the Android linker when the .so is loaded.
//       We spawn a background thread immediately so we do not
//       block the main thread during library load.
// ════════════════════════════════════════════════════════════
extern "C" JNIEXPORT jint JNI_OnLoad(JavaVM* vm, void* /*reserved*/) {
    LOGI("======================================");
    LOGI("  ACModMenu JNI_OnLoad");
    LOGI("  Build: " __DATE__ " " __TIME__);
    LOGI("======================================");

    // Spawn the IL2CPP wait thread
    pthread_t tid;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    int rc = pthread_create(&tid, &attr, WaitForIl2CppThread, nullptr);
    pthread_attr_destroy(&attr);

    if (rc != 0) {
        LOGE("pthread_create failed (rc=%d) – mod will not load!", rc);
    }

    return JNI_VERSION_1_6;
}
