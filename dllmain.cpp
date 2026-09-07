#include "pch.h"
#include <windows.h>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <atomic>
#include "MinHook.h"

// ============================================================================
// CONSTANTS & GLOBALS
// ============================================================================
uintptr_t g_GameBase = 0;

constexpr uintptr_t OFFSET_VEHICLE_SPAWNER = 0xDD3070;      // Ghidra: FUN_140dd3070
constexpr uintptr_t OFFSET_DROP_LIFECYCLE = 0xDEd9e0;      // Ghidra: FUN_140ded9e0 (lifecycle state machine)
constexpr uintptr_t OFFSET_STATE_ENUM = 0x12C;         // state on drop-request object (param_1 + 300)
constexpr uintptr_t RVA_CCAMERAMANAGER_VTABLE = 0x2309C20;  // CCameraManager vtable (Ghidra)
constexpr uintptr_t OFFSET_CAMERA_FORWARD = 0x044;         // live forward, confirmed by flip test

// Lifecycle state value for "beacon thrown / airborne" (the initiation point).
// Adjust if the logged state sequence shows a different number.
constexpr int32_t STATE_BEACON_THROWN = 2;

typedef int64_t(__fastcall* tVehicleSpawner)(int64_t*, int64_t*, float*);
tVehicleSpawner oVehicleSpawner = nullptr;

// Lifecycle state machine. Two params: drop object ptr (RCX) + delta time (XMM0).
typedef void(__fastcall* tDropLifecycle)(int64_t* dropObj, float deltaTime);
tDropLifecycle oDropLifecycle = nullptr;

// Published by the worker thread, read by the game thread.
static std::atomic<uintptr_t> g_LiveMgr{ 0 };

// Facing vector locked in when the rebel drop is initiated (beacon thrown).
static std::atomic<float> g_LockedFwdX{ 0.0f };
static std::atomic<float> g_LockedFwdZ{ 0.0f };
static std::atomic<bool>  g_HasLocked{ false };

// ============================================================================
// LOGGING (thread-safe: worker + hook both log)
// Disabled by default. Auto-creates jc3_rebel_orient.ini on first launch.
// ============================================================================
static CRITICAL_SECTION g_LogCS;
static FILE* g_LogFile = nullptr;
static bool  g_LogInit = false;
static bool  g_LoggingEnabled = false;

static bool GetGameDirPath(const char* fileName, char* out, size_t outSize) {
    char exePath[MAX_PATH] = {};
    if (GetModuleFileNameA(nullptr, exePath, MAX_PATH) == 0) return false;
    char* slash = strrchr(exePath, '\\');
    if (slash) slash[1] = '\0';
    snprintf(out, outSize, "%s%s", exePath, fileName);
    return true;
}

static void EnsureConfigFile(const char* iniPath) {
    if (GetFileAttributesA(iniPath) != INVALID_FILE_ATTRIBUTES) return;
    FILE* f = nullptr;
    if (fopen_s(&f, iniPath, "w") == 0 && f) {
        fprintf(f, "; RebelDropDirectionMod configuration\n");
        fprintf(f, "; Set Logging=1 to write jc3_rebel_orient.log next to the game exe.\n");
        fprintf(f, "[General]\n");
        fprintf(f, "Logging=0\n");
        fclose(f);
    }
}

static void LogInit() {
    if (g_LogInit) return;
    InitializeCriticalSection(&g_LogCS);
    char iniPath[MAX_PATH] = {};
    if (GetGameDirPath("jc3_rebel_orient.ini", iniPath, sizeof(iniPath))) {
        EnsureConfigFile(iniPath);
        g_LoggingEnabled = (GetPrivateProfileIntA("General", "Logging", 0, iniPath) != 0);
        if (g_LoggingEnabled) {
            char logPath[MAX_PATH] = {};
            if (GetGameDirPath("jc3_rebel_orient.log", logPath, sizeof(logPath))) {
                errno_t err = fopen_s(&g_LogFile, logPath, "a");
                if (err != 0) g_LogFile = nullptr;
            }
        }
    }
    g_LogInit = true;
}

void LogMessage(const char* format, ...) {
    if (!g_LoggingEnabled || !g_LogFile) return;
    EnterCriticalSection(&g_LogCS);
    va_list args; va_start(args, format);
    vfprintf(g_LogFile, format, args); va_end(args);
    fflush(g_LogFile);
    LeaveCriticalSection(&g_LogCS);
}

// ============================================================================
// MEMORY UTILITIES
// ============================================================================
bool SafeReadMemory(uintptr_t address, void* buffer, size_t size) {
    __try { memcpy(buffer, reinterpret_cast<void*>(address), size); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static float VecLen(const float* v) {
    return sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
}

static bool IsLiveCameraManager(uintptr_t mgr) {
    if (mgr == 0) return false;
    uintptr_t vt = 0;
    if (!SafeReadMemory(mgr, &vt, sizeof(vt))) return false;
    if (vt != g_GameBase + RVA_CCAMERAMANAGER_VTABLE) return false;
    float v[3] = { 0, 0, 0 };
    if (!SafeReadMemory(mgr + OFFSET_CAMERA_FORWARD, v, sizeof(v))) return false;
    const float len = VecLen(v);
    return (len > 0.9f && len < 1.1f);
}

// ============================================================================
// EXPENSIVE HUNT  -- ONLY EVER RUN ON THE WORKER THREAD
// ============================================================================
static int CountUnitVectors(uintptr_t base) {
    int score = 0;
    for (uintptr_t off = 0; off < 0x400; off += 4) {
        float v[3] = { 0, 0, 0 };
        if (!SafeReadMemory(base + off, v, sizeof(v))) continue;
        float len = VecLen(v);
        if (len > 0.9f && len < 1.1f) score++;
    }
    return score;
}

static void DoFullHunt() {
    const uintptr_t targetVTable = g_GameBase + RVA_CCAMERAMANAGER_VTABLE;
    SYSTEM_INFO si; GetSystemInfo(&si);
    uintptr_t p = reinterpret_cast<uintptr_t>(si.lpMinimumApplicationAddress);
    const uintptr_t maxAddr = reinterpret_cast<uintptr_t>(si.lpMaximumApplicationAddress);
    MEMORY_BASIC_INFORMATION mbi;

    uintptr_t best = 0;
    int bestScore = -1;
    int candidateCount = 0;

    while (p < maxAddr) {
        if (VirtualQuery(reinterpret_cast<LPCVOID>(p), &mbi, sizeof(mbi))) {
            if (mbi.State == MEM_COMMIT &&
                (mbi.Protect == PAGE_READWRITE || mbi.Protect == PAGE_EXECUTE_READWRITE) &&
                !(mbi.Protect & PAGE_GUARD)) {

                uintptr_t* buf = reinterpret_cast<uintptr_t*>(mbi.BaseAddress);
                const size_t n = mbi.RegionSize / sizeof(uintptr_t);
                __try {
                    for (size_t i = 0; i < n; ++i) {
                        if (buf[i] != targetVTable) continue;
                        ++candidateCount;
                        const uintptr_t cand = reinterpret_cast<uintptr_t>(&buf[i]);

                        float v[3] = { 0, 0, 0 };
                        if (!SafeReadMemory(cand + OFFSET_CAMERA_FORWARD, v, sizeof(v))) continue;
                        const float len = VecLen(v);
                        if (len < 0.9f || len > 1.1f) continue;

                        const int score = CountUnitVectors(cand);
                        if (score > bestScore) { bestScore = score; best = cand; }
                    }
                }
                __except (EXCEPTION_EXECUTE_HANDLER) {}
            }
            p += mbi.RegionSize;
        }
        else {
            p += 0x1000;
        }
    }

    g_LiveMgr.store(best);
    if (best)
        LogMessage("[Cam] hunt: live manager 0x%llX (richness %d, %d candidates)\n",
            best, bestScore, candidateCount);
    else
        LogMessage("[Cam] hunt: no live manager found (%d candidates)\n", candidateCount);
}

DWORD WINAPI CameraHuntThread(LPVOID) {
    while (true) {
        if (!IsLiveCameraManager(g_LiveMgr.load()))
            DoFullHunt();
        Sleep(300);
    }
    return 0;
}

// ============================================================================
// HOT PATH  -- game thread. Cheap reads only, NEVER the full scan.
// ============================================================================
bool GetCameraForwardVector(float& outX, float& outZ) {
    const uintptr_t mgr = g_LiveMgr.load();
    if (!IsLiveCameraManager(mgr)) return false;

    float fX, fY, fZ;
    if (!SafeReadMemory(mgr + OFFSET_CAMERA_FORWARD, &fX, sizeof(fX)) ||
        !SafeReadMemory(mgr + OFFSET_CAMERA_FORWARD + 4, &fY, sizeof(fY)) ||
        !SafeReadMemory(mgr + OFFSET_CAMERA_FORWARD + 8, &fZ, sizeof(fZ)))
        return false;

    const float lenXZ = sqrtf(fX * fX + fZ * fZ);
    if (lenXZ < 0.001f) return false;

    outX = fX / lenXZ;
    outZ = fZ / lenXZ;
    return true;
}

// ============================================================================
// LIFECYCLE STATE MACHINE HOOK — captures the camera at the initiation
// ============================================================================
void __fastcall hkDropLifecycle(int64_t* param_1, float param_2) {
    // Read the state BEFORE the original advances it, so we see the phase
    // being processed this frame.
    int32_t state = 0;
    if (param_1) SafeReadMemory((uintptr_t)param_1 + OFFSET_STATE_ENUM, &state, sizeof(state));

    static std::atomic<int32_t> s_LastState{ 0 };
    const int32_t prev = s_LastState.load();

    // Diagnostic: log every transition so the state->phase mapping can be
    // confirmed (or STATE_BEACON_THROWN adjusted) from a single throw.
    if (state != prev)
        LogMessage("[Lifecycle] state %d -> %d\n", prev, state);

    // Capture the camera on the transition INTO the "beacon thrown" state —
    // the initiation. Fires once per drop, not per frame.
    if (state == STATE_BEACON_THROWN && prev != STATE_BEACON_THROWN) {
        float fX, fZ;
        if (GetCameraForwardVector(fX, fZ)) {
            g_LockedFwdX.store(fX);
            g_LockedFwdZ.store(fZ);
            g_HasLocked.store(true);
            LogMessage("[Lifecycle] initiation - LOCKED facing (%.3f, %.3f)\n", fX, fZ);
        }
        else {
            LogMessage("[Lifecycle] initiation - camera read failed\n");
        }
    }
    s_LastState.store(state);

    // Forward both params with the correct types so the float lands back in XMM0.
    oDropLifecycle(param_1, param_2);
}

// ============================================================================
// VEHICLE SPAWNER HOOK — uses the throw-locked facing
// ============================================================================
int64_t __fastcall hkVehicleSpawner(int64_t* param_1, int64_t* param_2, float* param_3) {
    if (param_3 != nullptr) {
        __try {
            float fX = 0.0f, fZ = 0.0f;
            bool have = false;

            if (g_HasLocked.load()) {
                fX = g_LockedFwdX.load();
                fZ = g_LockedFwdZ.load();
                have = true;
            }
            else if (GetCameraForwardVector(fX, fZ)) {
                have = true;   // fallback: no throw seen yet, use live camera
            }

            if (have) {
                const float lenXZ = sqrtf(fX * fX + fZ * fZ);
                if (lenXZ > 0.001f) {
                    fX /= lenXZ;
                    fZ /= lenXZ;
                    const float rX = fZ, rZ = -fX;
                    const float crateX = param_3[12], crateY = param_3[13], crateZ = param_3[14];

                    param_3[0] = rX;   param_3[1] = 0.0f; param_3[2] = rZ;   param_3[3] = 0.0f;
                    param_3[4] = 0.0f; param_3[5] = 1.0f; param_3[6] = 0.0f; param_3[7] = 0.0f;
                    param_3[8] = fX;   param_3[9] = 0.0f; param_3[10] = fZ;   param_3[11] = 0.0f;
                    param_3[12] = crateX; param_3[13] = crateY; param_3[14] = crateZ; param_3[15] = 1.0f;
                }
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            LogMessage("[ERROR] exception inside spawner hook\n");
        }
    }
    return oVehicleSpawner(param_1, param_2, param_3);
}

// ============================================================================
// DLL ENTRY
// ============================================================================
DWORD WINAPI MainThread(LPVOID) {
    LogInit();
    LogMessage("==========================================\n");
    LogMessage("JC3 rebel-drop orientation mod starting\n");

    g_GameBase = reinterpret_cast<uintptr_t>(GetModuleHandleA(NULL));

    if (MH_Initialize() != MH_OK) { LogMessage("[FATAL] MH_Initialize\n"); return 1; }
    if (MH_CreateHook(reinterpret_cast<LPVOID>(g_GameBase + OFFSET_VEHICLE_SPAWNER),
        &hkVehicleSpawner, reinterpret_cast<LPVOID*>(&oVehicleSpawner)) != MH_OK) {
        LogMessage("[FATAL] MH_CreateHook spawner\n"); return 1;
    }
    if (MH_CreateHook(reinterpret_cast<LPVOID>(g_GameBase + OFFSET_DROP_LIFECYCLE),
        &hkDropLifecycle, reinterpret_cast<LPVOID*>(&oDropLifecycle)) != MH_OK) {
        LogMessage("[FATAL] MH_CreateHook lifecycle\n"); return 1;
    }
    if (MH_EnableHook(MH_ALL_HOOKS) != MH_OK) { LogMessage("[FATAL] MH_EnableHook\n"); return 1; }
    LogMessage("[OK] hooks installed\n");

    CreateThread(NULL, 0, CameraHuntThread, NULL, 0, NULL);
    return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID) {
    if (ul_reason_for_call == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        CreateThread(NULL, 0, MainThread, NULL, 0, NULL);
    }
    return TRUE;
}