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

constexpr uintptr_t OFFSET_VEHICLE_SPAWNER = 0xDD3070;    // Ghidra: FUN_140dd3070
constexpr uintptr_t RVA_CCAMERAMANAGER_VTABLE = 0x2309C20;  // CCameraManager vtable (Ghidra)
constexpr uintptr_t OFFSET_CAMERA_FORWARD = 0x044;      // live forward, confirmed by flip test

typedef int64_t(__fastcall* tVehicleSpawner)(int64_t*, int64_t*, float*);
tVehicleSpawner oVehicleSpawner = nullptr;

// Published by the worker thread, read by the game thread.
static std::atomic<uintptr_t> g_LiveMgr{ 0 };

// ============================================================================
// LOGGING (thread-safe: worker + hook both log)
// ============================================================================
static CRITICAL_SECTION g_LogCS;
static FILE* g_LogFile = nullptr;
static bool  g_LogInit = false;

static void LogInit() {
    if (g_LogInit) return;
    InitializeCriticalSection(&g_LogCS);
    fopen_s(&g_LogFile, "jc3_rebel_orient.log", "a");
    g_LogInit = true;
}

void LogMessage(const char* format, ...) {
    if (!g_LogInit || !g_LogFile) return;
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

// Cheap validity check for a candidate/cached pointer: vtable present AND
// +0x044 holds a unit vector. Two small reads — safe on the hot path.
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
// Count unit-length vectors in [base, base+0x400). Real manager = "rich";
// a transient garbage block holding the vtable word = sparse.
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

// Full address-space scan: find every vtable match, pick the richest candidate
// whose +0x044 is a live forward, publish atomically.
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

                        // Cheap gate first: +0x044 must hold a unit vector.
                        float v[3] = { 0, 0, 0 };
                        if (!SafeReadMemory(cand + OFFSET_CAMERA_FORWARD, v, sizeof(v))) continue;
                        const float len = VecLen(v);
                        if (len < 0.9f || len > 1.1f) continue;

                        // Expensive richness, only for gated candidates.
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

// Worker: the ONLY place the expensive scan runs. Hunts immediately, then
// re-hunts (self-heals) only if the cached manager dies.
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
    if (!IsLiveCameraManager(mgr)) return false;   // worker re-hunts within ~300ms

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
// VEHICLE SPAWNER HOOK (unchanged — verified working)
// ============================================================================
int64_t __fastcall hkVehicleSpawner(int64_t* param_1, int64_t* param_2, float* param_3) {
    if (param_3 != nullptr) {
        __try {
            float camX, camZ;
            if (GetCameraForwardVector(camX, camZ)) {
                const float fX = camX, fZ = camZ;
                const float rX = fZ, rZ = -fX;
                const float crateX = param_3[12], crateY = param_3[13], crateZ = param_3[14];

                param_3[0] = rX;   param_3[1] = 0.0f; param_3[2] = rZ;   param_3[3] = 0.0f;
                param_3[4] = 0.0f; param_3[5] = 1.0f; param_3[6] = 0.0f; param_3[7] = 0.0f;
                param_3[8] = fX;   param_3[9] = 0.0f; param_3[10] = fZ;   param_3[11] = 0.0f;
                param_3[12] = crateX; param_3[13] = crateY; param_3[14] = crateZ; param_3[15] = 1.0f;
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            LogMessage("[ERROR] exception inside spawner hook\n");
        }
    }
    return oVehicleSpawner(param_1, param_2, param_3);   // forward ALL original args
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
        LogMessage("[FATAL] MH_CreateHook\n"); return 1;
    }
    if (MH_EnableHook(MH_ALL_HOOKS) != MH_OK) { LogMessage("[FATAL] MH_EnableHook\n"); return 1; }
    LogMessage("[OK] hook installed\n");

    // Start the background hunt so the game thread never does the full scan.
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