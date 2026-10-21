// BiA-VR: stereo VR for Brothers in Arms: Road to Hill 30 (Unreal Engine 2.5, D3D9, 32-bit).
//
// Ships as a proxy d3d9.dll in the game's System folder.
//  * Direct3DCreate9 is upgraded to Direct3DCreate9Ex so render targets can be shared
//    with D3D11 / SteamVR. Ex devices reject D3DPOOL_MANAGED, so those resources are
//    transparently created as DEFAULT|DYNAMIC (lockable, and they survive Reset on Ex).
//  * UGameEngine::Draw is hooked and called twice per frame, once per eye.
//  * FPlayerSceneNode's constructor is hooked to offset the camera by the HMD pose + eye.
//  * Each eye's backbuffer is copied into a shared texture and submitted to SteamVR.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>
#include <d3d11.h>
#include <math.h>
#include <stdio.h>
#include <stdarg.h>
#include <share.h>
#include "openvr.h"
#include "MinHook.h"
#include <intrin.h>

// ---------------------------------------------------------------------------
// Logging / config

static FILE* g_log;
static void Log(const char* fmt, ...) {
    if (!g_log) return;
    SYSTEMTIME t; GetLocalTime(&t);
    fprintf(g_log, "[%02d:%02d:%02d.%03d] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    va_list a; va_start(a, fmt); vfprintf(g_log, fmt, a); va_end(a);
    fputc('\n', g_log); fflush(g_log);
}

static char g_iniPath[MAX_PATH];
static float CfgF(const char* key, float def) {
    char d[32], buf[64]; sprintf_s(d, "%g", def);
    GetPrivateProfileStringA("BiAVR", key, d, buf, sizeof buf, g_iniPath);
    return (float)atof(buf);
}
static int CfgI(const char* key, int def) { return GetPrivateProfileIntA("BiAVR", key, def, g_iniPath); }

struct Config {
    int   enabled;          // 0 = flat game, only the D3D9Ex compatibility layer runs
    float worldScale;       // Unreal units per metre (0 = measure from the player's eye height)
    float eyeHeightMetres;  // real standing eye height used for the auto scale
    int   positional;       // apply HMD translation (lean)
    int   keepMousePitch;   // 1 = mouse pitch still tilts the view (crosshair stays honest)
    int   recenterKey;      // virtual-key code
    int   overrideFov;      // replace the game's FOV with the headset's
    int   useSharedHandle;  // submit DXGI shared handle directly instead of a D3D11 texture
    float menuDistance;     // metres from your head to the floating menu screen
    float menuWidth;        // metres wide
    float hudScale;         // shrink HUD/captions toward the centre of view (1 = untouched)
    int   exactProjection;  // replace the game's lens with the headset's exact per-eye lens
    int   controllerAim;    // aim/shoot with the right motion controller
    float aimPitchOffset;   // degrees added to the controller's pointing angle
    float aimTilt;          // controller raw pose -> pointing direction, rotation about its X axis
    bool  aimTiltSet;       // AimTiltDegrees present in BiAVR.ini (otherwise auto from the controller)
    float snapTurnDeg;      // right-stick snap turn step
    float aimConvergence;   // metres at which bullets cross the gun's sight line (0 = parallel)
    int   toggleAimKey;     // keyboard key that turns controller aiming on/off
    float gunFwd, gunRight, gunUp; // extra gun offset from the hand (Unreal units)
    int   gunLaser;         // laser sight from the gun during gameplay
    int   laserKey;         // keyboard key toggling the laser
    int   raiseAccuracy;    // 3 = no spread at all, 2 = always aim-down-sights accuracy, 1 = only near your face, 0 = off
    float raiseDistance;    // metres between controller and headset that counts as "raised"
    int   twoHanded;        // left hand in front of the right steers the gun
    int   leftArmIK;        // bend the game left arm to the left controller
    int   gunTraceFire;     // bullets travel along the gun sight line (the laser)
    float leftHandRoll;     // degrees added to the left hand bone roll
    int   zoomedFire;       // while firing, the game logic sees the weapon as zoomed (R1 accuracy, no R1 pose)
    float twoHandSmoothing; // 0 = none, 0.9 = very smooth (two-handed only)
} g_cfg;
static bool g_autoScale;

static const char* kDefaultIni =
"[BiAVR]\r\n"
"; BiA-VR settings. Delete this file to get the defaults back.\r\n"
"; 0 = VR off (the game runs normally on the monitor)\r\n"
"Enabled=1\r\n"
"; Game units per metre. 0 = measure it from your soldier's eye height (recommended)\r\n"
"WorldScale=0\r\n"
"; Your standing eye height in metres (about your height minus 10 cm); used when WorldScale=0\r\n"
"EyeHeightMetres=1.62\r\n"
"PositionalTracking=1\r\n"
"; Aim and shoot with the right motion controller (F7 toggles in game)\r\n"
"ControllerAim=1\r\n"
"; Right-stick snap turn step in degrees\r\n"
"SnapTurnDegrees=30\r\n"
"; Red laser from the gun (F6 toggles)\r\n"
"GunLaser=1\r\n"
"; Put your left hand on the rifle's front to aim with both hands\r\n"
"TwoHandedAim=1\r\n"
"TwoHandSmoothing=0.5\r\n"
"; The game's left arm follows your left controller\r\n"
"LeftArmIK=1\r\n"
"; Twist correction for the left hand (try 90 / -90 / 180 if it looks rotated)\r\n"
"LeftHandRollDegrees=180\r\n"
"; 3 = no bullet spread (your hand aims), 2 = aim-down-sights accuracy always, 0 = game default\r\n"
"RaiseToAimAccuracy=3\r\n"
"; R1 accuracy while you shoot, without the R1 slowdown (experimental)\r\n"
"ZoomedAccuracyWhenFiring=1\r\n"
"; HUD size toward the centre of view (1 = full size)\r\n"
"HudScale=0.55\r\n"
"; Floating menu screen: distance and width in metres\r\n"
"MenuDistance=2.2\r\n"
"MenuWidth=1.8\r\n"
"; Uncomment to override the controller's pointing angle (degrees, e.g. -39.4 for PSVR2 Sense)\r\n"
";AimTiltDegrees=0\r\n";

static void WriteDefaultIniIfMissing() {
    if (GetFileAttributesA(g_iniPath) != INVALID_FILE_ATTRIBUTES) return;
    FILE* f = nullptr;
    if (fopen_s(&f, g_iniPath, "wb") == 0 && f) { fputs(kDefaultIni, f); fclose(f); }
}

static void LoadConfig() {
    WriteDefaultIniIfMissing();
    g_cfg.enabled         = CfgI("Enabled", 1);
    g_cfg.worldScale      = CfgF("WorldScale", 0.f);
    g_cfg.eyeHeightMetres = CfgF("EyeHeightMetres", 1.62f);
    g_cfg.positional      = CfgI("PositionalTracking", 1);
    g_cfg.keepMousePitch  = CfgI("KeepMousePitch", 1);
    g_cfg.recenterKey     = CfgI("RecenterKey", VK_F8);
    g_cfg.overrideFov     = CfgI("OverrideFOV", 1);
    g_cfg.useSharedHandle = CfgI("SubmitSharedHandle", 0);
    g_cfg.menuDistance    = CfgF("MenuDistance", 2.2f);
    g_cfg.menuWidth       = CfgF("MenuWidth", 1.8f);
    g_cfg.hudScale        = CfgF("HudScale", 0.55f);
    g_cfg.exactProjection = CfgI("ExactProjection", 1);
    g_cfg.controllerAim   = CfgI("ControllerAim", 1);
    g_cfg.aimPitchOffset  = CfgF("AimPitchOffset", 0.f);
    g_cfg.aimTilt         = CfgF("AimTiltDegrees", 0.f);
    { char b[32]; GetPrivateProfileStringA("BiAVR", "AimTiltDegrees", "", b, sizeof b, g_iniPath); g_cfg.aimTiltSet = b[0] != 0; }
    g_cfg.snapTurnDeg     = CfgF("SnapTurnDegrees", 30.f);
    g_cfg.aimConvergence  = CfgF("AimConvergence", 20.f);
    g_cfg.toggleAimKey    = CfgI("ToggleAimKey", VK_F7);
    g_cfg.gunFwd          = CfgF("GunOffsetForward", -55.f);
    g_cfg.gunRight        = CfgF("GunOffsetRight", -11.f);
    g_cfg.gunUp           = CfgF("GunOffsetUp", 14.f);
    g_cfg.gunLaser        = CfgI("GunLaser", 1);
    g_cfg.laserKey        = CfgI("LaserKey", VK_F6);
    g_cfg.raiseAccuracy   = CfgI("RaiseToAimAccuracy", 3);
    g_cfg.raiseDistance   = CfgF("RaiseDistance", 0.25f);
    g_cfg.twoHanded       = CfgI("TwoHandedAim", 1);
    g_cfg.leftArmIK       = CfgI("LeftArmIK", 1);
    g_cfg.gunTraceFire    = CfgI("BulletsFollowGun", 0);
    g_cfg.leftHandRoll    = CfgF("LeftHandRollDegrees", 180.f);
    g_cfg.zoomedFire      = CfgI("ZoomedAccuracyWhenFiring", 1);
    g_cfg.twoHandSmoothing = CfgF("TwoHandSmoothing", 0.5f);
}

// ---------------------------------------------------------------------------
// Unreal types (UE2 layout)

struct FVector  { float X, Y, Z; };
struct FRotator { int Pitch, Yaw, Roll; };

// ---------------------------------------------------------------------------
// VR state

static vr::IVRSystem* g_vr;
static bool  g_vrOk;
static float g_tanX = 1.f, g_tanY = 1.f;        // symmetric half-tangents we render with
static vr::VRTextureBounds_t g_bounds[2];
static float g_rawL[2], g_rawR[2], g_rawT[2], g_rawB[2];
static float g_needX = 1.f, g_needY = 1.f;      // half-tangents both eyes need covered
static UINT  g_projW, g_projH;                  // backbuffer size the projection was fitted to
static vr::HmdMatrix34_t g_eyeToHead[2];
static vr::TrackedDevicePose_t g_poses[vr::k_unMaxTrackedDeviceCount];
static vr::HmdMatrix34_t g_hmd;                  // pose used for the current frame
static bool  g_hmdValid;
static int   g_eye = -1;                         // -1: not inside a stereo draw
static bool  g_inDraw;                          // inside any hooked UGameEngine::Draw
static bool  g_sceneSeen;                       // the 3D world was drawn during this Draw
static bool  g_menuMode;                        // last frame had no 3D world: show a floating screen
static vr::VROverlayHandle_t g_menuOverlay = vr::k_ulOverlayHandleInvalid;
static bool  g_menuVisible;
static bool  g_guiSeen;                         // a GUI menu page was drawn during this Draw
static bool  g_afterScene;                      // the 3D world has finished drawing for this eye
static bool  g_exactUsed[2];                    // this eye was rendered with the exact headset lens
struct FrameStats { int persp, ortho, rhwDraws, shaderDraws, draws; float gameA, gameB; } g_stats;
static DWORD g_fvf; static void* g_vs;
static int   g_statFrames;
static float g_leftPos[3]; static float g_leftAxes[3][3]; static bool g_leftValid;   // Unreal-local (body) frame, metres / axes
static int   g_pawnOff = -1;
static int   g_rotOffset = -1, g_locOffset = -1;
static void* g_bbPtr;               // current backbuffer surface (identity only, not referenced)
static bool  g_rtIsBB = true;
static bool  g_aimActive;                       // controller aiming drives the player this frame
static int   g_bodyYaw;                         // Unreal yaw of the player's body (snap turn)
static FRotator g_lastGameRot;                  // camera rotation the game asked for (before VR)
static FVector g_lastGameLoc;                   // camera location the game asked for (= where bullets start)
static float g_laserLen = 12.f;                 // metres, shortened to the traced hit point
static FVector g_gunStart, g_gunDir; static bool g_gunRayValid;   // world-space sight line of the gun
static FRotator g_bulletRot; static float g_bulletDir[3]; static bool g_bulletValid;   // eye -> laser target
static bool  g_inOwnTrace;                      // our own SingleLineCheck call (do not rewrite)
static bool  g_handValid;                       // right controller pose usable this frame
static float g_handPos[3];                      // right controller position, OpenVR seated space (m)
static float g_handAxes[3][3];                  // right controller aim axes in Unreal world (fwd, right, up)
static vr::HmdMatrix34_t g_handPose;            // raw right controller pose (for the laser)
static float g_aimVR[3][3];                     // final aim axes in OpenVR space: fwd, up, right
static bool  g_twoHand;                         // left hand is steering the gun
static bool  g_leftGrabbing;                    // L1 is holding the gun (not adjusting it)
static FRotator g_drawActorSavedRot;
static void* g_drawActor;                       // actor being drawn by UCanvas::DrawActor (first-person gun)
static FVector g_drawActorSavedLoc; static bool g_drawActorMoved;
static int   g_framesRendered;

static IDirect3DDevice9* g_dev;
static IDirect3DTexture9* g_eyeTex9[2];
static HANDLE g_eyeShare[2];
static ID3D11Device* g_d11;
static ID3D11Texture2D* g_eyeTex11[2];
static UINT g_eyeW, g_eyeH;
static IDirect3DQuery9* g_query;
static bool g_capturedThisEye;

static void InitVR() {
    vr::EVRInitError err = vr::VRInitError_None;
    g_vr = vr::VR_Init(&err, vr::VRApplication_Scene);
    if (err != vr::VRInitError_None || !vr::VRCompositor()) {
        Log("VR_Init failed: %s", vr::VR_GetVRInitErrorAsEnglishDescription(err));
        g_vr = nullptr; return;
    }
    vr::VRCompositor()->SetTrackingSpace(vr::TrackingUniverseSeated);

    float l[2], r[2], t[2], b[2];
    for (int e = 0; e < 2; ++e) {
        g_vr->GetProjectionRaw((vr::EVREye)e, &l[e], &r[e], &t[e], &b[e]);
        g_eyeToHead[e] = g_vr->GetEyeToHeadTransform((vr::EVREye)e);
        Log("eye %d tangents l=%.3f r=%.3f t=%.3f b=%.3f  offset x=%.4f", e, l[e], r[e], t[e], b[e], g_eyeToHead[e].m[0][3]);
    }
    g_tanX = g_tanY = 0.f;
    for (int e = 0; e < 2; ++e) {
        g_tanX = max(g_tanX, max(fabsf(l[e]), fabsf(r[e])));
        g_tanY = max(g_tanY, max(fabsf(t[e]), fabsf(b[e])));
    }
    g_needX = g_tanX; g_needY = g_tanY;
    for (int e = 0; e < 2; ++e) { g_rawL[e] = l[e]; g_rawR[e] = r[e]; g_rawT[e] = t[e]; g_rawB[e] = b[e]; }
    uint32_t rw, rh; g_vr->GetRecommendedRenderTargetSize(&rw, &rh);
    float hfov = 2.f * atanf(g_tanX) * 57.2957795f;
    float aspect = g_tanX / g_tanY;
    // Recommended size covers one eye's asymmetric frustum; scale to the symmetric one.
    float eyeSpanX = (r[0] - l[0]) * 0.5f;
    UINT sugW = (UINT)(rw * g_tanX / eyeSpanX), sugH = (UINT)(sugW / aspect);
    Log("render FOV %.1f deg, aspect %.4f, recommended %ux%u -> suggested game resolution %ux%u", hfov, aspect, rw, rh, sugW, sugH);
    g_vrOk = true;
}

// ---------------------------------------------------------------------------
// Math: Unreal rotators <-> axes (X fwd, Y right, Z up)

static const float kU2R = 3.14159265f / 32768.f;

static void RotToAxes(const FRotator& R, float X[3], float Y[3], float Z[3]) {
    float SP = sinf(R.Pitch * kU2R), CP = cosf(R.Pitch * kU2R);
    float SY = sinf(R.Yaw * kU2R),   CY = cosf(R.Yaw * kU2R);
    float SR = sinf(R.Roll * kU2R),  CR = cosf(R.Roll * kU2R);
    X[0] = CP * CY;                 X[1] = CP * SY;                 X[2] = SP;
    Y[0] = SR * SP * CY - CR * SY;  Y[1] = SR * SP * SY + CR * CY;  Y[2] = -SR * CP;
    Z[0] = -(CR * SP * CY + SR * SY); Z[1] = CY * SR - CR * SP * SY; Z[2] = CR * CP;
}

static int RadToU(float r) { return (int)lrintf(r / kU2R) & 0xFFFF; }

static FRotator AxesToRot(const float X[3], const float Y[3], const float Z[3]) {
    FRotator R;
    float pitch = atan2f(X[2], sqrtf(X[0] * X[0] + X[1] * X[1]));
    float yaw   = atan2f(X[1], X[0]);
    R.Pitch = RadToU(pitch); R.Yaw = RadToU(yaw); R.Roll = 0;
    float sx[3], sy[3], sz[3]; RotToAxes(R, sx, sy, sz);
    float roll = atan2f(Z[0] * sy[0] + Z[1] * sy[1] + Z[2] * sy[2],
                        Y[0] * sy[0] + Y[1] * sy[1] + Y[2] * sy[2]);
    R.Roll = RadToU(roll);
    return R;
}

// OpenVR (x right, y up, z back) -> Unreal local (x fwd, y right, z up)
static void VrToU(float x, float y, float z, float o[3]) { o[0] = -z; o[1] = x; o[2] = y; }

static void Combine(const float PX[3], const float PY[3], const float PZ[3], const float v[3], float o[3]) {
    for (int i = 0; i < 3; ++i) o[i] = v[0] * PX[i] + v[1] * PY[i] + v[2] * PZ[i];
}

// ---------------------------------------------------------------------------
// Engine hooks

typedef void (__fastcall* Draw_t)(void* self, void* edx, void* viewport, int blit, BYTE* hitData, int* hitSize);
static Draw_t oDraw;

typedef void* (__fastcall* PlayerSceneNode_t)(void* self, void* edx, void* viewport, void* target, void* viewActor,
                                              FVector loc, FRotator rot, float fov);
static PlayerSceneNode_t oPlayerSceneNode;

// Turns the game's camera (loc/rot) into this eye's headset camera. handWorld (optional) gets the
// right controller's world position in the same frame.
static void ApplyVRCamera(FVector& loc, FRotator& rot, float& fov, FVector* handWorld) {
    const vr::HmdMatrix34_t& m = g_hmd;
    FVector gameLoc = loc;
    FRotator base = rot;
    if (!g_cfg.keepMousePitch) base.Pitch = 0;
    base.Roll = 0;
    if (g_aimActive) { base.Pitch = 0; base.Yaw = g_bodyYaw; }
    float PX[3], PY[3], PZ[3]; RotToAxes(base, PX, PY, PZ);

    // Head axes in Unreal-local space, then into world via the player's view basis.
    float hf[3], hr[3], hu[3];
    VrToU(-m.m[0][2], -m.m[1][2], -m.m[2][2], hf);
    VrToU(m.m[0][0], m.m[1][0], m.m[2][0], hr);
    VrToU(m.m[0][1], m.m[1][1], m.m[2][1], hu);
    float WX[3], WY[3], WZ[3];
    Combine(PX, PY, PZ, hf, WX); Combine(PX, PY, PZ, hr, WY); Combine(PX, PY, PZ, hu, WZ);
    rot = AxesToRot(WX, WY, WZ);

    // Translation: head position (yaw-only basis so leaning stays level) + eye offset.
    float s = g_cfg.worldScale;
    FRotator yawOnly = { 0, base.Yaw, 0 };
    float YX[3], YY[3], YZ[3]; RotToAxes(yawOnly, YX, YY, YZ);
    float headOff[3] = { 0, 0, 0 };
    if (g_cfg.positional) {
        float hp[3]; VrToU(m.m[0][3], m.m[1][3], m.m[2][3], hp);
        Combine(YX, YY, YZ, hp, headOff);
        loc.X += headOff[0] * s; loc.Y += headOff[1] * s; loc.Z += headOff[2] * s;
    }
    const vr::HmdMatrix34_t& e = g_eyeToHead[g_eye];
    float el[3], w[3]; VrToU(e.m[0][3], e.m[1][3], e.m[2][3], el);
    Combine(WX, WY, WZ, el, w);
    loc.X += w[0] * s; loc.Y += w[1] * s; loc.Z += w[2] * s;

    if (g_cfg.overrideFov) fov = 2.f * atanf(g_tanX) * 57.2957795f;

    if (handWorld && g_handValid) {
        // The game's camera position stands for the head; the hand is offset from it by the
        // tracked hand-minus-head vector (the head offset is only applied when positional).
        float hp[3], hd[3], rel[3], wh[3];
        VrToU(g_handPos[0], g_handPos[1], g_handPos[2], hp);
        VrToU(m.m[0][3], m.m[1][3], m.m[2][3], hd);
        for (int i = 0; i < 3; ++i) rel[i] = g_cfg.positional ? hp[i] : hp[i] - hd[i];
        Combine(YX, YY, YZ, rel, wh);
        handWorld->X = gameLoc.X + wh[0] * s;
        handWorld->Y = gameLoc.Y + wh[1] * s;
        handWorld->Z = gameLoc.Z + wh[2] * s;
    }
}

static void* __fastcall hkPlayerSceneNode(void* self, void* edx, void* viewport, void* target, void* viewActor,
                                          FVector loc, FRotator rot, float fov) {
    if (g_inDraw) g_sceneSeen = true;
    if ((g_eye == 0 || g_eye == 1) && g_hmdValid) {
        g_lastGameRot = rot; g_lastGameLoc = loc;
        ApplyVRCamera(loc, rot, fov, nullptr);
    }
    return oPlayerSceneNode(self, edx, viewport, target, viewActor, loc, rot, fov);
}

// The first-person gun is drawn by UCanvas::DrawActor through its own FCameraSceneNode.
struct SightCal { wchar_t cls[64]; float f, r, u; };
static const wchar_t* ObjClassName(void* obj);
static SightCal* FindSight(const wchar_t* cls, bool create);
static void SaveSight(const SightCal* sc);
static void ProbeBones(void* actor);
static bool ZoomHeld();
static void SolveLeftArm(void* actor, const FVector& gameLoc);
static int g_zoomFrames;
static wchar_t g_gunClass[64];                  // class of the first-person weapon last drawn
static float g_gunFwdCur; static bool g_gunFwdLoaded;
typedef void* (__fastcall* CameraSceneNode_t)(void* self, void* edx, void* viewport, void* target, void* viewActor,
                                              FVector loc, FRotator rot, float fov, float a, float b);
static CameraSceneNode_t oCameraSceneNode;
static int g_locOffsetForGun = -1;
static void* __fastcall hkCameraSceneNode(void* self, void* edx, void* viewport, void* target, void* viewActor,
                                          FVector loc, FRotator rot, float fov, float a, float b) {
    if (g_drawActor && (g_eye == 0 || g_eye == 1) && g_hmdValid) {
        FVector gameLoc = loc, hand;
        FRotator gameRot = rot;
        ApplyVRCamera(loc, rot, fov, g_aimActive ? &hand : nullptr);
        if (g_aimActive && g_handValid && g_locOffsetForGun > 0 && !g_drawActorMoved) {
            // Re-express the gun's placement (relative to the game camera) relative to the hand,
            // using the hand's full orientation so it follows 1:1 even past the game's pitch limits.
            float AX[3], AY[3], AZ[3]; RotToAxes(gameRot, AX, AY, AZ);
            FVector* aloc = (FVector*)((BYTE*)g_drawActor + g_locOffsetForGun);
            FRotator* arot = (FRotator*)((BYTE*)g_drawActor + g_rotOffset);
            g_drawActorSavedLoc = *aloc; g_drawActorSavedRot = *arot;
            float d[3] = { aloc->X - gameLoc.X, aloc->Y - gameLoc.Y, aloc->Z - gameLoc.Z };
            float rf = d[0] * AX[0] + d[1] * AX[1] + d[2] * AX[2];
            float rr = d[0] * AY[0] + d[1] * AY[1] + d[2] * AY[2];
            float ru = d[0] * AZ[0] + d[1] * AZ[1] + d[2] * AZ[2];
            float lf = rf + g_gunFwdCur, lr = rr + g_cfg.gunRight, lu = ru + g_cfg.gunUp;
            const wchar_t* cls = ObjClassName(g_drawActor);
            if (wcscmp(cls, g_gunClass)) {
                // New weapon: load its forward offset ([GunForward] in BiAVR.ini, else the global one).
                wcsncpy_s(g_gunClass, cls, _TRUNCATE);
                char a[64]; WideCharToMultiByte(CP_ACP, 0, cls, -1, a, sizeof a, nullptr, nullptr);
                char v[32]; GetPrivateProfileStringA("GunForward", a, "", v, sizeof v, g_iniPath);
                g_gunFwdCur = v[0] ? (float)atof(v) : g_cfg.gunFwd;
                Log("weapon %s: forward offset %.1f", a, g_gunFwdCur);
            }
            static float hipF = 0.f; static bool haveHipF;
            if (ZoomHeld()) {
                // Aiming down sights: the game lines the sights up sideways/vertically; keep the
                // gun at the hip-fire distance from the hand (the R1 pose pushes it ~36 cm forward).
                lr = rr; lu = ru;
                lf = (haveHipF ? hipF : rf) + g_gunFwdCur;
                if (g_eye == 0 && ++g_zoomFrames > 60) {      // ~0.7 s, once the raise animation settles
                    SightCal* sc = FindSight(cls, true);
                    if (sc) {
                        bool changed = fabsf(sc->f - rf) + fabsf(sc->r - rr) + fabsf(sc->u - ru) > 0.5f;
                        sc->f = rf; sc->r = rr; sc->u = ru;
                        if (changed && g_zoomFrames % 30 == 1) SaveSight(sc);
                    }
                }
            } else {
                if (g_eye == 0) { g_zoomFrames = 0; hipF = rf; haveHipF = true; }
                if (SightCal* sc = FindSight(cls, false)) { lr = sc->r; lu = sc->u; }
            }
            if (g_eye == 0) ProbeBones(g_drawActor);
            SolveLeftArm(g_drawActor, gameLoc);
            const float (*H)[3] = g_handAxes;
            aloc->X = hand.X + lf * H[0][0] + lr * H[1][0] + lu * H[2][0];
            aloc->Y = hand.Y + lf * H[0][1] + lr * H[1][1] + lu * H[2][1];
            aloc->Z = hand.Z + lf * H[0][2] + lr * H[1][2] + lu * H[2][2];
            *arot = AxesToRot(H[0], H[1], H[2]);
            float dx = lf, dy = lr, dz = lu;
            g_drawActorMoved = true;
            static int logs; if (logs++ < 3) Log("gun placed at hand: local offset (%.1f, %.1f, %.1f)", dx, dy, dz);
        }
    }
    return oCameraSceneNode(self, edx, viewport, target, viewActor, loc, rot, fov, a, b);
}

typedef void (__fastcall* CanvasDrawActor_t)(void* self, void* edx, void* actor, float fov);
static CanvasDrawActor_t oCanvasDrawActor;
static void __fastcall hkCanvasDrawActor(void* self, void* edx, void* actor, float fov) {
    if (!(g_eye == 0 || g_eye == 1)) { oCanvasDrawActor(self, edx, actor, fov); return; }
    g_drawActor = actor; g_drawActorMoved = false;
    oCanvasDrawActor(self, edx, actor, fov);
    if (g_drawActorMoved) {
        *(FVector*)((BYTE*)actor + g_locOffsetForGun) = g_drawActorSavedLoc;
        *(FRotator*)((BYTE*)actor + g_rotOffset) = g_drawActorSavedRot;
    }
    g_drawActor = nullptr; g_drawActorMoved = false;
}

static void FitProjection(UINT w, UINT h) {
    if (!w || !h || (w == g_projW && h == g_projH)) return;
    float aspect = (float)w / (float)h;
    g_tanX = max(g_needX, g_needY * aspect);
    g_tanY = g_tanX / aspect;
    for (int e = 0; e < 2; ++e) {
        g_bounds[e].uMin = 0.5f + 0.5f * g_rawL[e] / g_tanX;
        g_bounds[e].uMax = 0.5f + 0.5f * g_rawR[e] / g_tanX;
        g_bounds[e].vMin = 0.5f + 0.5f * g_rawT[e] / g_tanY;
        g_bounds[e].vMax = 0.5f + 0.5f * g_rawB[e] / g_tanY;
    }
    g_projW = w; g_projH = h;
    Log("projection fitted to %ux%u: horizontal FOV %.1f deg, uses %.0f%% x %.0f%% of the image",
        w, h, 2.f * atanf(g_tanX) * 57.2957795f, 100.f * g_needX / g_tanX, 100.f * g_needY / g_tanY);
}

typedef void (__fastcall* GuiDraw_t)(void* self, void* edx, void* canvas);
static GuiDraw_t oPageDraw, oGuiPostRender;
static void __fastcall hkPageDraw(void* self, void* edx, void* canvas) {
    if (g_inDraw) g_guiSeen = true;
    oPageDraw(self, edx, canvas);
}
static void __fastcall hkGuiPostRender(void* self, void* edx, void* canvas) {
    if (g_inDraw) g_guiSeen = true;
    oGuiPostRender(self, edx, canvas);
}

static bool g_guiHooked;
static void TryHookGui() {
    if (g_guiHooked) return;
    HMODULE gui = GetModuleHandleA("GUI.dll");
    if (!gui) return;
    g_guiHooked = true;
    void* pageDraw = GetProcAddress(gui, "?Draw@UGUIPage@@UAEXPAVUCanvas@@@Z");
    void* postRender = GetProcAddress(gui, "?NativePostRender@UGUIController@@UAEXPAVUCanvas@@@Z");
    MH_STATUS a = pageDraw ? MH_CreateHook(pageDraw, (void*)hkPageDraw, (void**)&oPageDraw) : MH_ERROR_FUNCTION_NOT_FOUND;
    MH_STATUS b = postRender ? MH_CreateHook(postRender, (void*)hkGuiPostRender, (void**)&oGuiPostRender) : MH_ERROR_FUNCTION_NOT_FOUND;
    if (a == MH_OK) MH_EnableHook(pageDraw);
    if (b == MH_OK) MH_EnableHook(postRender);
    Log("GUI hooks: page draw=%d controller postrender=%d", a, b);
}

typedef void (__fastcall* SceneRender_t)(void* self, void* edx, void* ri);
static SceneRender_t oSceneRender;
static void __fastcall hkSceneRender(void* self, void* edx, void* ri) {
    oSceneRender(self, edx, ri);
    if (g_eye == 0 || g_eye == 1) g_afterScene = true;
}

static bool EnsureEyeTextures() {
    IDirect3DSurface9* bb = nullptr;
    if (FAILED(g_dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb))) return false;
    D3DSURFACE_DESC d; bb->GetDesc(&d); bb->Release();
    if (g_eyeTex9[0] && d.Width == g_eyeW && d.Height == g_eyeH) return true;

    for (int e = 0; e < 2; ++e) {
        if (g_eyeTex11[e]) { g_eyeTex11[e]->Release(); g_eyeTex11[e] = nullptr; }
        if (g_eyeTex9[e])  { g_eyeTex9[e]->Release();  g_eyeTex9[e] = nullptr; }
        g_eyeShare[e] = nullptr;
        HRESULT hr = g_dev->CreateTexture(d.Width, d.Height, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8,
                                          D3DPOOL_DEFAULT, &g_eyeTex9[e], &g_eyeShare[e]);
        if (FAILED(hr)) { Log("eye texture create failed %08lx", hr); return false; }
        if (g_d11) {
            hr = g_d11->OpenSharedResource(g_eyeShare[e], __uuidof(ID3D11Texture2D), (void**)&g_eyeTex11[e]);
            if (FAILED(hr)) Log("OpenSharedResource failed %08lx", hr);
        }
    }
    if (!g_query) g_dev->CreateQuery(D3DQUERYTYPE_EVENT, &g_query);
    g_eyeW = d.Width; g_eyeH = d.Height;
    Log("eye textures %ux%u (backbuffer format %d, msaa %d)", d.Width, d.Height, d.Format, d.MultiSampleType);
    return true;
}

static void CaptureEye(int eye) {
    if (!g_dev || !EnsureEyeTextures()) return;
    IDirect3DSurface9 *bb = nullptr, *dst = nullptr;
    if (SUCCEEDED(g_dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) &&
        SUCCEEDED(g_eyeTex9[eye]->GetSurfaceLevel(0, &dst))) {
        HRESULT hr = g_dev->StretchRect(bb, nullptr, dst, nullptr, D3DTEXF_NONE);
        if (FAILED(hr) && g_framesRendered < 5) Log("StretchRect failed %08lx", hr);
    }
    if (dst) dst->Release();
    if (bb) bb->Release();
    g_capturedThisEye = true;
}

static void WaitGpu() {
    if (!g_query) return;
    g_query->Issue(D3DISSUE_END);
    for (int spin = 0; spin < 200000 && g_query->GetData(nullptr, 0, D3DGETDATA_FLUSH) == S_FALSE; ++spin) {}
}

static vr::Texture_t EyeTexture(int e) {
    vr::Texture_t tex;
    if (g_cfg.useSharedHandle || !g_eyeTex11[e]) { tex.handle = g_eyeShare[e]; tex.eType = vr::TextureType_DXGISharedHandle; }
    else { tex.handle = g_eyeTex11[e]; tex.eType = vr::TextureType_DirectX; }
    tex.eColorSpace = vr::ColorSpace_Gamma;
    return tex;
}

// onlyTex >= 0 submits that one texture to both eyes, uncropped (used for the black backdrop).
static void SubmitEyes(int onlyTex = -1) {
    if (!g_eyeTex9[0]) return;
    WaitGpu();
    for (int e = 0; e < 2; ++e) {
        vr::Texture_t tex = EyeTexture(onlyTex >= 0 ? onlyTex : e);
        static const vr::VRTextureBounds_t full = { 0, 0, 1, 1 };
        const vr::VRTextureBounds_t* bounds = onlyTex >= 0 ? nullptr : (g_exactUsed[e] ? &full : &g_bounds[e]);
        vr::EVRCompositorError ce = vr::VRCompositor()->Submit((vr::EVREye)e, &tex, bounds);
        if (ce != vr::VRCompositorError_None && g_framesRendered < 5) Log("Submit eye %d error %d", e, ce);
    }
    vr::VRCompositor()->PostPresentHandoff();
}

static void PumpVR() {
    vr::VREvent_t ev;
    while (g_vr->PollNextEvent(&ev, sizeof ev)) {
        if (ev.eventType == vr::VREvent_Quit) { Log("SteamVR quit"); g_vr->AcknowledgeQuit_Exiting(); g_vrOk = false; }
    }
    static bool wasDown;
    bool down = (GetAsyncKeyState(g_cfg.recenterKey) & 0x8000) != 0;
    if (down && !wasDown) { vr::VRChaperone()->ResetZeroPose(vr::TrackingUniverseSeated); Log("recenter"); }
    wasDown = down;
}

// ---------------------------------------------------------------------------
// Unreal reflection (Core.dll exports) to find the local PlayerController and its Rotation

struct TArrayRaw { void** Data; int Num, Max; };
typedef void* (__cdecl* StaticFindObject_t)(void* cls, void* outer, const wchar_t* name, int exact);
typedef void* (__cdecl* StaticClass_t)();
typedef int (__fastcall* IsA_t)(void* self, void* edx, void* cls);
static StaticFindObject_t pStaticFindObject;
static IsA_t pIsA;
static TArrayRaw* pGObjObjects;
static void* g_pcClass;
static int   g_offsetField = -1;                // byte offset of UProperty::Offset
static void* g_guiClass; static int g_mouseXOff = -1, g_mouseYOff = -1;
static void* g_gui; static int g_guiIndex = -1;
static bool  g_forceScan;
static void* g_pc; static int g_pcIndex = -1;
static bool  g_reflectTried;

static void InitReflection() {
    if (g_reflectTried) return;
    g_reflectTried = true;
    HMODULE core = GetModuleHandleA("Core.dll");
    if (!core) { Log("Core.dll missing"); return; }
    pStaticFindObject = (StaticFindObject_t)GetProcAddress(core, "?StaticFindObject@UObject@@SAPAV1@PAVUClass@@PAV1@PBGH@Z");
    pIsA = (IsA_t)GetProcAddress(core, "?IsA@UObject@@QBEHPAVUClass@@@Z");
    pGObjObjects = (TArrayRaw*)GetProcAddress(core, "?GObjObjects@UObject@@0V?$TArray@PAVUObject@@@@A");
    auto classClass = (StaticClass_t)GetProcAddress(core, "?StaticClass@UClass@@SAPAV1@XZ");
    auto propClass = (StaticClass_t)GetProcAddress(core, "?StaticClass@UProperty@@SAPAVUClass@@XZ");
    if (!pStaticFindObject || !pIsA || !pGObjObjects || !classClass || !propClass) { Log("reflection exports missing"); return; }
    void* anyPackage = (void*)-1;
    void* actorClass = pStaticFindObject(classClass(), anyPackage, L"Actor", 0);
    g_pcClass = pStaticFindObject(classClass(), anyPackage, L"PlayerController", 0);
    void* locProp = actorClass ? pStaticFindObject(propClass(), actorClass, L"Location", 0) : nullptr;
    void* rotProp = actorClass ? pStaticFindObject(propClass(), actorClass, L"Rotation", 0) : nullptr;
    Log("reflection: Actor %p PlayerController %p Location prop %p Rotation prop %p", actorClass, g_pcClass, locProp, rotProp);
    if (!locProp || !rotProp || !g_pcClass) return;
    auto dump = [](const char* name, void* obj) {
        char line[512]; int n = sprintf_s(line, "%s:", name);
        for (int k = 0; k < 0x70; k += 4) n += sprintf_s(line + n, sizeof line - n, " %x", *(int*)((BYTE*)obj + k));
        Log("%s", line);
    };
    dump("Location prop", locProp); dump("Rotation prop", rotProp);
    // UE2 UProperty: ... INT ArrayDim; INT ElementSize; DWORD PropertyFlags; FName Category; WORD RepOffset, RepIndex; INT Offset;
    // Both are 12-byte structs with ArrayDim 1. Find the slot that is plausible for both.
    for (int k = 0x30; k < 0x70; k += 4) {
        BYTE* L = (BYTE*)locProp; BYTE* R = (BYTE*)rotProp;
        int lo = *(int*)(L + k), ro = *(int*)(R + k);
        if (lo < 0x28 || lo > 0x2000 || ro < 0x28 || ro > 0x2000 || lo == ro || (lo & 3) || (ro & 3)) continue;
        bool shape = false;
        for (int j = 8; j <= 20; j += 4) {
            int a = *(int*)(L + k - j), b = *(int*)(R + k - j);
            if (a == 0x000C0001 && b == 0x000C0001) shape = true;                       // packed WORD ArrayDim, ElementSize
            if (a == 12 && b == 12 && *(int*)(L + k - j - 4) == 1 && *(int*)(R + k - j - 4) == 1) shape = true;
        }
        if (!shape || ro != lo + 12) continue;
        g_locOffset = lo; g_rotOffset = ro; g_locOffsetForGun = lo; g_offsetField = k;
        Log("property offset field +0x%x: Actor.Location @0x%x, Actor.Rotation @0x%x", k, lo, ro);
        return;
    }
    Log("could not determine property offsets");
}

static int YawDiff(int a, int b) { int d = (a - b) & 0xFFFF; return d > 32768 ? 65536 - d : d; }

// Finds the local PlayerController (re-validated against GObjObjects so a freed one is never used).
static FRotator* PlayerRotation() {
    if (g_rotOffset < 0 || !pGObjObjects) return nullptr;
    if (g_pc && g_pcIndex >= 0 && g_pcIndex < pGObjObjects->Num && pGObjObjects->Data[g_pcIndex] == g_pc)
        return (FRotator*)((BYTE*)g_pc + g_rotOffset);
    g_pc = nullptr; g_pcIndex = -1;
    static int lastScan; if (!g_forceScan && g_framesRendered - lastScan < 30 && lastScan) return nullptr;
    g_forceScan = false;
    lastScan = g_framesRendered ? g_framesRendered : 1;
    int best = -1, bestDiff = 1 << 30;
    for (int i = 0; i < pGObjObjects->Num; ++i) {
        void* o = pGObjObjects->Data[i];
        if (!o || !pIsA(o, nullptr, g_pcClass)) continue;
        FRotator* r = (FRotator*)((BYTE*)o + g_rotOffset);
        int d = YawDiff(r->Yaw, g_lastGameRot.Yaw);
        if (d < bestDiff) { bestDiff = d; best = i; }
    }
    if (best < 0) return nullptr;
    g_pc = pGObjObjects->Data[best]; g_pcIndex = best;
    Log("PlayerController %p (object #%d, yaw diff %d)", g_pc, best, bestDiff);
    return (FRotator*)((BYTE*)g_pc + g_rotOffset);
}

// ---------------------------------------------------------------------------
// Motion controllers -> aim, keys and mouse buttons

static void SendKey(WORD vk, bool down) {
    INPUT in = {}; in.type = INPUT_KEYBOARD;
    in.ki.wScan = (WORD)MapVirtualKeyA(vk, MAPVK_VK_TO_VSC);
    in.ki.dwFlags = KEYEVENTF_SCANCODE | (down ? 0 : KEYEVENTF_KEYUP);
    SendInput(1, &in, sizeof in);
}
static void SendMouseButton(bool left, bool down) {
    INPUT in = {}; in.type = INPUT_MOUSE;
    in.mi.dwFlags = left ? (down ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP) : (down ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP);
    SendInput(1, &in, sizeof in);
}

enum Action { A_HOLSTER, A_FIRE, A_ZOOM, A_FWD, A_BACK, A_LEFT, A_RIGHT, A_USE, A_DUCK, A_JUMP, A_WEAPON, A_SQUAD, A_MELEE, A_MENU, A_COUNT };
static bool g_actState[A_COUNT];
static void SetAction(int a, bool on) {
    if (g_actState[a] == on) return;
    g_actState[a] = on;
    static int logs; static const char* names[] = { "holster", "fire", "zoom", "fwd", "back", "left", "right", "use", "duck", "jump", "weapon", "squad", "melee", "menu" };
    if (logs++ < 300) Log("input %s %s", names[a], on ? "down" : "up");
    switch (a) {
    case A_HOLSTER: SendKey(VK_TAB, on); break;
    case A_FIRE:   SendMouseButton(true, on); break;
    case A_ZOOM:   SendMouseButton(false, on); break;
    case A_FWD:    SendKey('W', on); break;
    case A_BACK:   SendKey('S', on); break;
    case A_LEFT:   SendKey('A', on); break;
    case A_RIGHT:  SendKey('D', on); break;
    case A_USE:    SendKey('R', on); break;
    case A_DUCK:   SendKey('C', on); break;
    case A_JUMP:   SendKey(VK_SPACE, on); break;
    case A_WEAPON: SendKey(VK_TAB, on); break;
    case A_SQUAD:  SendKey(VK_SHIFT, on); break;
    case A_MELEE:  SendKey('Q', on); break;
    case A_MENU:   SendKey(VK_ESCAPE, on); break;
    }
}
static void ReleaseAllActions() { for (int a = 0; a < A_COUNT; ++a) SetAction(a, false); }

static void AimAxesVR(const vr::HmdMatrix34_t& m, float fwd[3], float up[3], float right[3]);
static void ResolveAimFrame(vr::TrackedDeviceIndex_t dev, const vr::VRControllerState_t* st);
// ULevel::SingleLineCheck(FCheckResult& Hit, AActor* Source, const FVector& End, const FVector& Start, DWORD Flags, const FVector& Extent)
typedef int (__fastcall* SingleLineCheck_t)(void* level, void* edx, void* hit, void* source, const FVector* end, const FVector* start, DWORD flags, const FVector* extent);
static SingleLineCheck_t pSingleLineCheck;
static SingleLineCheck_t oSingleLineCheck;
static int g_xlevelOff = -1;
static bool g_traceBroken;
static void* FindProp(void* outer, const wchar_t* name);
static int PropOffset(void* prop);
static int g_pawnOffForTrace = -1;

// Traces from start to end; returns true and the hit point if something was hit.
static bool TraceWorld(void* source, const FVector& start, const FVector& end, FVector& hitLoc) {
    if (g_traceBroken || !g_pc) return false;
    if (!pSingleLineCheck) {
        HMODULE eng = GetModuleHandleA("Engine.dll");
        pSingleLineCheck = (SingleLineCheck_t)GetProcAddress(eng, "?SingleLineCheck@ULevel@@QAEHAAUFCheckResult@@PAVAActor@@ABVFVector@@2K2@Z");
        HMODULE core = GetModuleHandleA("Core.dll");
        auto classClass = (StaticClass_t)GetProcAddress(core, "?StaticClass@UClass@@SAPAV1@XZ");
        void* actorClass = pStaticFindObject(classClass(), (void*)-1, L"Actor", 0);
        g_xlevelOff = PropOffset(actorClass ? FindProp(actorClass, L"XLevel") : nullptr);
        Log("trace: SingleLineCheck %p, Actor.XLevel @0x%x", pSingleLineCheck, g_xlevelOff);
        if (!pSingleLineCheck || g_xlevelOff < 0) { g_traceBroken = true; return false; }
    }
    void* level = *(void**)((BYTE*)g_pc + g_xlevelOff);
    if (!level) return false;
    // FCheckResult: Next, Actor, FVector Location (+8), FVector Normal, Primitive, FLOAT Time (+36), ...
    alignas(16) BYTE hit[256] = {};
    *(float*)(hit + 36) = 1.f;
    FVector extent = { 0, 0, 0 };
    const DWORD flags = 0x00001 | 0x00002 | 0x00004 | 0x00010 | 0x00080 | 0x10000;   // pawns, movers, level, others, geometry, terrain
    int clear = 1;
    g_inOwnTrace = true;
    __try { clear = (oSingleLineCheck ? oSingleLineCheck : pSingleLineCheck)(level, nullptr, hit, source, &end, &start, flags, &extent); }
    __except (EXCEPTION_EXECUTE_HANDLER) { g_inOwnTrace = false; g_traceBroken = true; Log("trace call failed; using fixed convergence"); return false; }
    g_inOwnTrace = false;
    if (false) {}
    if (clear) return false;
    hitLoc = *(FVector*)(hit + 8);
    return true;
}

static bool Pressed(const vr::VRControllerState_t& st, vr::EVRButtonId b) { return (st.ulButtonPressed & vr::ButtonMaskFromId(b)) != 0; }

// Reads both controllers; sends game input; returns true if the right hand pose is usable for aim.
static bool UpdateControllers(FRotator& aimOut) {
    vr::TrackedDeviceIndex_t ri = g_vr->GetTrackedDeviceIndexForControllerRole(vr::TrackedControllerRole_RightHand);
    vr::TrackedDeviceIndex_t li = g_vr->GetTrackedDeviceIndexForControllerRole(vr::TrackedControllerRole_LeftHand);
    vr::VRControllerState_t rs = {}, ls = {};
    bool rOk = ri != vr::k_unTrackedDeviceIndexInvalid && g_vr->GetControllerState(ri, &rs, sizeof rs);
    bool lOk = li != vr::k_unTrackedDeviceIndexInvalid && g_vr->GetControllerState(li, &ls, sizeof ls);
    if (rOk) ResolveAimFrame(ri, &rs);
    static int lastRi = -2, lastLi = -2;
    if ((int)ri != lastRi || (int)li != lastLi) {
        lastRi = ri; lastLi = li;
        Log("controllers: right #%d (state %d) left #%d (state %d)", (int)ri, rOk, (int)li, lOk);
    }
    static float lastAx[4]; static int axLogs;
    float ax[4] = { rs.rAxis[0].x, rs.rAxis[1].x, ls.rAxis[0].y, ls.rAxis[1].x };
    for (int i = 0; i < 4 && axLogs < 20; ++i)
        if (fabsf(ax[i] - lastAx[i]) > 0.5f) {
            ++axLogs; memcpy(lastAx, ax, sizeof ax);
            Log("axes: R stick x %.2f  R trigger %.2f  L stick y %.2f  L trigger %.2f", ax[0], ax[1], ax[2], ax[3]);
            break;
        }

    // One-time-per-change log so the button layout can be checked.
    static uint64_t lastR, lastL; static int logs;
    if (logs < 60 && ((rOk && rs.ulButtonPressed != lastR) || (lOk && ls.ulButtonPressed != lastL))) {
        ++logs; lastR = rs.ulButtonPressed; lastL = ls.ulButtonPressed;
        Log("buttons R=%llx L=%llx  R axes (%.2f,%.2f) trig %.2f  L axes (%.2f,%.2f) trig %.2f",
            rs.ulButtonPressed, ls.ulButtonPressed, rs.rAxis[0].x, rs.rAxis[0].y, rs.rAxis[1].x,
            ls.rAxis[0].x, ls.rAxis[0].y, ls.rAxis[1].x);
    }

    if (rOk) {
        SetAction(A_FIRE, rs.rAxis[1].x > 0.6f || (g_actState[A_FIRE] && rs.rAxis[1].x > 0.4f));
        // Holster zones, relative to the head: low at the hip, or up/behind the shoulder.
        int zone = 0;
        if (ri != vr::k_unTrackedDeviceIndexInvalid && g_poses[ri].bPoseIsValid && g_hmdValid) {
            const vr::HmdMatrix34_t& hm = g_poses[ri].mDeviceToAbsoluteTracking;
            float dx = hm.m[0][3] - g_hmd.m[0][3], dy = hm.m[1][3] - g_hmd.m[1][3], dz = hm.m[2][3] - g_hmd.m[2][3];
            float fx = -g_hmd.m[0][2], fz = -g_hmd.m[2][2], fl = sqrtf(fx * fx + fz * fz);
            float ahead = fl > 1e-3f ? (dx * fx + dz * fz) / fl : 0.f;
            if (dy < -0.45f) zone = 1;                              // hip / thigh
            else if (dy > -0.10f && ahead < 0.05f) zone = 2;        // over the shoulder
        }
        static int lastZone;
        if (zone && zone != lastZone) g_vr->TriggerHapticPulse(ri, 0, 3000);
        lastZone = zone;
        bool r1 = Pressed(rs, vr::k_EButton_Grip);
        SetAction(A_HOLSTER, zone != 0 && r1);
        SetAction(A_ZOOM, zone == 0 && r1 && !g_actState[A_HOLSTER]);
        SetAction(A_USE,  Pressed(rs, vr::k_EButton_A));
        SetAction(A_DUCK, Pressed(rs, vr::k_EButton_ApplicationMenu));
        SetAction(A_MELEE, Pressed(rs, vr::k_EButton_SteamVR_Touchpad));
        static bool turned, adjusting;
        float x = rs.rAxis[0].x;
        bool adjust = lOk && Pressed(ls, vr::k_EButton_Grip) && !g_leftGrabbing && !g_twoHand;
        if (adjust) {
            float dt = 1.f / 90.f, rate = 25.f;
            if (fabsf(rs.rAxis[0].y) > 0.2f) g_gunFwdCur += rs.rAxis[0].y * rate * dt;
            if (fabsf(x) > 0.2f) g_cfg.gunRight += x * rate * dt;
            adjusting = true;
            x = 0;                                   // no snap turn while adjusting
        } else if (adjusting) {
            adjusting = false;
            char v[32];
            char a[64]; WideCharToMultiByte(CP_ACP, 0, g_gunClass, -1, a, sizeof a, nullptr, nullptr);
            sprintf_s(v, "%.1f", g_gunFwdCur);
            if (a[0]) WritePrivateProfileStringA("GunForward", a, v, g_iniPath);
            sprintf_s(v, "%.1f", g_cfg.gunRight); WritePrivateProfileStringA("BiAVR", "GunOffsetRight", v, g_iniPath);
            Log("gun offset saved for %s: forward %.1f, right %.1f (all weapons)", a, g_gunFwdCur, g_cfg.gunRight);
        }
        if (!turned && fabsf(x) > 0.7f) {
            g_bodyYaw = (g_bodyYaw + (int)((x > 0 ? 1 : -1) * g_cfg.snapTurnDeg * 65536.f / 360.f)) & 0xFFFF;
            turned = true;
        } else if (fabsf(x) < 0.3f) turned = false;
    }
    if (lOk) {
        float x = ls.rAxis[0].x, y = ls.rAxis[0].y;
        SetAction(A_FWD, y > 0.5f);  SetAction(A_BACK, y < -0.5f);
        SetAction(A_RIGHT, x > 0.5f); SetAction(A_LEFT, x < -0.5f);
        SetAction(A_JUMP,   Pressed(ls, vr::k_EButton_A));
        SetAction(A_WEAPON, Pressed(ls, vr::k_EButton_ApplicationMenu));
        SetAction(A_SQUAD,  ls.rAxis[1].x > 0.6f);
        SetAction(A_MENU,   Pressed(ls, vr::k_EButton_SteamVR_Touchpad));
    }

    g_handValid = false;
    if (!rOk || !g_poses[ri].bPoseIsValid) return false;
    const vr::HmdMatrix34_t& m = g_poses[ri].mDeviceToAbsoluteTracking;
    g_handPos[0] = m.m[0][3]; g_handPos[1] = m.m[1][3]; g_handPos[2] = m.m[2][3];
    g_handValid = true;
    float af[3], au[3], ar[3]; AimAxesVR(m, af, au, ar);

    // Two-handed grip (same scheme as F.E.A.R. VR's two_handed_grip.h):
    //  - engage: squeeze L1 while the left hand sits where a fore-end would be
    //    (5-60 cm ahead of the right hand along its aim axis, within 22 cm of that axis);
    //  - latched until L1 is released, so a drifting hand never flickers it off;
    //  - steering follows the right->left hand line 1:1 up to ~55 deg off the gun axis,
    //    then eases toward 90 deg instead of stopping dead.
    bool twoHand = g_twoHand;
    bool leftPose = g_cfg.twoHanded && lOk && li != vr::k_unTrackedDeviceIndexInvalid && g_poses[li].bPoseIsValid;
    bool l1 = lOk && Pressed(ls, vr::k_EButton_Grip);
    float d[3] = { 0, 0, 0 }, len = 0, along = 0, lateral = 1e9f;
    if (leftPose) {
        const vr::HmdMatrix34_t& lm = g_poses[li].mDeviceToAbsoluteTracking;
        for (int i = 0; i < 3; ++i) d[i] = lm.m[i][3] - m.m[i][3];
        len = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        along = d[0] * af[0] + d[1] * af[1] + d[2] * af[2];
        lateral = sqrtf(max(len * len - along * along, 0.f));
    }
    // Auto-attach (Titanfall style): on when the left hand reaches the fore-end zone,
    // off when it clearly leaves it. The release zone is wider so it doesn't flicker.
    bool onWeapon = leftPose && along >= 0.05f && along <= 0.60f && lateral <= 0.22f;
    bool nearWeapon = leftPose && along >= 0.0f && along <= 0.75f && lateral <= 0.32f;
    (void)l1;
    if (!twoHand && onWeapon) twoHand = true;
    if (twoHand && !nearWeapon) twoHand = false;
    g_leftGrabbing = twoHand;
    static int dbg; if (leftPose && dbg < 30 && g_framesRendered % 90 == 0) { ++dbg; Log("left hand vs gun: ahead %.2f m, off-axis %.2f m, attached %d", along, lateral, twoHand); }

    if (twoHand && len > 0.12f) {
        float dh[3] = { d[0] / len, d[1] / len, d[2] / len };
        float c = min(max(dh[0] * af[0] + dh[1] * af[1] + dh[2] * af[2], -1.f), 1.f);
        float ang = acosf(c);
        const float soft = 0.96f, maxA = 1.57f;
        float lim = ang <= soft ? ang : soft + (maxA - soft) * (1.f - expf(-(ang - soft) / (maxA - soft)));
        float perp[3] = { dh[0] - af[0] * c, dh[1] - af[1] * c, dh[2] - af[2] * c };
        float pl = sqrtf(perp[0] * perp[0] + perp[1] * perp[1] + perp[2] * perp[2]);
        float nf[3];
        if (pl > 1e-5f) for (int i = 0; i < 3; ++i) nf[i] = af[i] * cosf(lim) + perp[i] / pl * sinf(lim);
        else for (int i = 0; i < 3; ++i) nf[i] = af[i];
        // Keep the right hand's roll: up = right controller up made perpendicular to the barrel.
        float k = au[0] * nf[0] + au[1] * nf[1] + au[2] * nf[2];
        float nu[3] = { au[0] - k * nf[0], au[1] - k * nf[1], au[2] - k * nf[2] };
        float ul = sqrtf(nu[0] * nu[0] + nu[1] * nu[1] + nu[2] * nu[2]);
        if (ul > 1e-3f) {
            for (int i = 0; i < 3; ++i) nu[i] /= ul;
            float nr[3] = { nf[1] * nu[2] - nf[2] * nu[1], nf[2] * nu[0] - nf[0] * nu[2], nf[0] * nu[1] - nf[1] * nu[0] };
            if (nr[0] * ar[0] + nr[1] * ar[1] + nr[2] * ar[2] < 0) for (int i = 0; i < 3; ++i) nr[i] = -nr[i];
            for (int i = 0; i < 3; ++i) { af[i] = nf[i]; au[i] = nu[i]; ar[i] = nr[i]; }
        }
    }
    static int ldbg;
    if (ldbg < 12 && g_framesRendered % 120 == 0) {
        ++ldbg;
        Log("left controller: index %d, state ok %d, pose valid %d, tracking result %d, twoHanded cfg %d",
            (int)li, lOk, li < vr::k_unMaxTrackedDeviceCount ? (int)g_poses[li].bPoseIsValid : -1,
            li < vr::k_unMaxTrackedDeviceCount ? (int)g_poses[li].eTrackingResult : -1, g_cfg.twoHanded);
    }
    g_leftValid = false;
    if (lOk && li != vr::k_unTrackedDeviceIndexInvalid && g_poses[li].bPoseIsValid) {
        const vr::HmdMatrix34_t& lm = g_poses[li].mDeviceToAbsoluteTracking;
        float lf[3], lu[3], lr[3]; AimAxesVR(lm, lf, lu, lr);
        VrToU(lm.m[0][3], lm.m[1][3], lm.m[2][3], g_leftPos);
        VrToU(lf[0], lf[1], lf[2], g_leftAxes[0]);
        VrToU(lr[0], lr[1], lr[2], g_leftAxes[1]);
        VrToU(lu[0], lu[1], lu[2], g_leftAxes[2]);
        g_leftValid = true;
    }
    if (twoHand != g_twoHand) { g_twoHand = twoHand; static int logs; if (logs++ < 20) Log("two-handed aim %s", twoHand ? "on" : "off"); }

    // Light smoothing while two-handed (steady rifle); one-handed stays raw for responsiveness.
    static float sf[3], su[3]; static bool hadSmooth;
    if (twoHand && g_cfg.twoHandSmoothing > 0.f) {
        float a = min(max(g_cfg.twoHandSmoothing, 0.f), 0.95f);
        if (!hadSmooth) { memcpy(sf, af, sizeof sf); memcpy(su, au, sizeof su); }
        for (int i = 0; i < 3; ++i) { sf[i] = sf[i] * a + af[i] * (1 - a); su[i] = su[i] * a + au[i] * (1 - a); }
        float fl = sqrtf(sf[0] * sf[0] + sf[1] * sf[1] + sf[2] * sf[2]);
        for (int i = 0; i < 3; ++i) af[i] = sf[i] / fl;
        float k = su[0] * af[0] + su[1] * af[1] + su[2] * af[2];
        float nu[3] = { su[0] - k * af[0], su[1] - k * af[1], su[2] - k * af[2] };
        float ul = sqrtf(nu[0] * nu[0] + nu[1] * nu[1] + nu[2] * nu[2]);
        if (ul > 1e-3f) {
            for (int i = 0; i < 3; ++i) au[i] = nu[i] / ul;
            float nr[3] = { af[1] * au[2] - af[2] * au[1], af[2] * au[0] - af[0] * au[2], af[0] * au[1] - af[1] * au[0] };
            if (nr[0] * ar[0] + nr[1] * ar[1] + nr[2] * ar[2] < 0) for (int i = 0; i < 3; ++i) nr[i] = -nr[i];
            memcpy(ar, nr, sizeof nr);
        }
        hadSmooth = true;
    } else hadSmooth = false;
    for (int i = 0; i < 3; ++i) { g_aimVR[0][i] = af[i]; g_aimVR[1][i] = au[i]; g_aimVR[2][i] = ar[i]; }

    float f[3], r[3], u[3];
    VrToU(af[0], af[1], af[2], f);
    VrToU(ar[0], ar[1], ar[2], r);
    VrToU(au[0], au[1], au[2], u);
    FRotator body = { 0, g_bodyYaw, 0 };
    float BX[3], BY[3], BZ[3]; RotToAxes(body, BX, BY, BZ);
    float WX[3], WY[3], WZ[3];
    Combine(BX, BY, BZ, f, WX); Combine(BX, BY, BZ, r, WY); Combine(BX, BY, BZ, u, WZ);
    for (int i = 0; i < 3; ++i) { g_handAxes[0][i] = WX[i]; g_handAxes[1][i] = WY[i]; g_handAxes[2][i] = WZ[i]; }
    g_handPose = m;
    aimOut = AxesToRot(WX, WY, WZ);
    // Bullets leave from the soldier's eye (the tracking origin), not the gun in the hand.
    // Converge: aim the eye at the point the gun's sight line reaches at AimConvergence metres.
    g_laserLen = 12.f;
    bool traced = false;
    float tw[3];
    if (g_pc && g_pawnOff > 0) {
        // Gun muzzle-ish point and direction in the world, then find what the sight line hits.
        float s = g_cfg.worldScale;
        float hl[3], hw[3]; VrToU(m.m[0][3], m.m[1][3], m.m[2][3], hl); Combine(BX, BY, BZ, hl, hw);
        FVector start = { g_lastGameLoc.X + hw[0] * s, g_lastGameLoc.Y + hw[1] * s, g_lastGameLoc.Z + hw[2] * s };
        FVector end = { start.X + WX[0] * 30000.f, start.Y + WX[1] * 30000.f, start.Z + WX[2] * 30000.f };
        g_gunStart = start; g_gunDir.X = WX[0]; g_gunDir.Y = WX[1]; g_gunDir.Z = WX[2]; g_gunRayValid = true;
        FVector hit;
        void* pawn = *(void**)((BYTE*)g_pc + g_pawnOff);
        if (TraceWorld(pawn, start, end, hit)) {
            tw[0] = hit.X - g_lastGameLoc.X; tw[1] = hit.Y - g_lastGameLoc.Y; tw[2] = hit.Z - g_lastGameLoc.Z;
            float dx = hit.X - start.X, dy = hit.Y - start.Y, dz = hit.Z - start.Z;
            g_laserLen = min(sqrtf(dx * dx + dy * dy + dz * dz) / s, 60.f);
            traced = true;
        }
    }
    if (traced || g_cfg.aimConvergence > 0.f) {
        if (!traced) {
            float tv[3] = { m.m[0][3] + af[0] * g_cfg.aimConvergence, m.m[1][3] + af[1] * g_cfg.aimConvergence, m.m[2][3] + af[2] * g_cfg.aimConvergence };
            float tl[3]; VrToU(tv[0], tv[1], tv[2], tl);
            Combine(BX, BY, BZ, tl, tw);
        }
        aimOut.Yaw = RadToU(atan2f(tw[1], tw[0]));
        aimOut.Pitch = RadToU(atan2f(tw[2], sqrtf(tw[0] * tw[0] + tw[1] * tw[1])));
    }
    aimOut.Pitch = (aimOut.Pitch + (int)(g_cfg.aimPitchOffset * 65536.f / 360.f)) & 0xFFFF;
    aimOut.Roll = 0;
    g_bulletRot = aimOut;
    {
        float cp = cosf(aimOut.Pitch * kU2R), sp = sinf(aimOut.Pitch * kU2R), cy = cosf(aimOut.Yaw * kU2R), sy = sinf(aimOut.Yaw * kU2R);
        g_bulletDir[0] = cp * cy; g_bulletDir[1] = cp * sy; g_bulletDir[2] = sp;
        g_bulletValid = true;
    }
    return true;
}

// Pointing axes of a controller (OpenVR space). The pointing frame relative to the raw pose comes
// from the controller's SteamVR render-model "tip" component, so it is right for any headset;
// AimTiltDegrees in BiAVR.ini overrides it with a simple tilt about the controller's X axis.
static float g_aimLocal[3][3] = { { 0, 0, -1 }, { 0, 1, 0 }, { 1, 0, 0 } };   // local fwd, up, right
static bool  g_aimLocalSet;

static void SetAimTilt(float deg) {
    float t = deg * 3.14159265f / 180.f;
    float f[3] = { 0, sinf(t), -cosf(t) }, u[3] = { 0, cosf(t), sinf(t) }, r[3] = { 1, 0, 0 };
    memcpy(g_aimLocal[0], f, sizeof f); memcpy(g_aimLocal[1], u, sizeof u); memcpy(g_aimLocal[2], r, sizeof r);
}

static void ResolveAimFrame(vr::TrackedDeviceIndex_t dev, const vr::VRControllerState_t* st) {
    if (g_aimLocalSet || dev == vr::k_unTrackedDeviceIndexInvalid) return;
    if (g_cfg.aimTiltSet) { SetAimTilt(g_cfg.aimTilt); g_aimLocalSet = true; Log("aim frame: AimTiltDegrees %.1f from BiAVR.ini", g_cfg.aimTilt); return; }
    static int tries; if (tries > 600) return;
    char type[128] = "", model[256] = "";
    g_vr->GetStringTrackedDeviceProperty(dev, vr::Prop_ControllerType_String, type, sizeof type);
    g_vr->GetStringTrackedDeviceProperty(dev, vr::Prop_RenderModelName_String, model, sizeof model);
    vr::RenderModel_ComponentState_t cs = {};
    vr::RenderModel_ControllerMode_State_t mode = {};
    if (model[0] && vr::VRRenderModels() && vr::VRRenderModels()->GetComponentState(model, "tip", st, &mode, &cs)) {
        const vr::HmdMatrix34_t& t = cs.mTrackingToComponentLocal;
        for (int i = 0; i < 3; ++i) { g_aimLocal[0][i] = -t.m[i][2]; g_aimLocal[1][i] = t.m[i][1]; g_aimLocal[2][i] = t.m[i][0]; }
        g_aimLocalSet = true;
        float pitch = atan2f(g_aimLocal[0][1], -g_aimLocal[0][2]) * 57.2958f;
        Log("aim frame: controller '%s' (%s) tip component, pitch %.1f deg", type, model, pitch);
        return;
    }
    if (++tries == 600) {
        // No tip component: known fallbacks, else straight along the raw pose.
        float deg = strstr(type, "playstation_vr2") ? -39.4f : 0.f;
        SetAimTilt(deg); g_aimLocalSet = true;
        Log("aim frame: no tip component for '%s' (%s); using tilt %.1f", type, model, deg);
    }
}

static void AimAxesVR(const vr::HmdMatrix34_t& m, float fwd[3], float up[3], float right[3]) {
    for (int i = 0; i < 3; ++i) {
        fwd[i]   = m.m[i][0] * g_aimLocal[0][0] + m.m[i][1] * g_aimLocal[0][1] + m.m[i][2] * g_aimLocal[0][2];
        up[i]    = m.m[i][0] * g_aimLocal[1][0] + m.m[i][1] * g_aimLocal[1][1] + m.m[i][2] * g_aimLocal[1][2];
        right[i] = m.m[i][0] * g_aimLocal[2][0] + m.m[i][1] * g_aimLocal[2][1] + m.m[i][2] * g_aimLocal[2][2];
    }
}

static void ShowLaser(const vr::HmdMatrix34_t& m, float length);
static void HideLaser();
static void ShowLaserAxes(const float origin[3], const float f[3], const float u[3], const float r[3], float length);

static int HeadYawU() {
    const vr::HmdMatrix34_t& m = g_hmd;
    float f[3]; VrToU(-m.m[0][2], -m.m[1][2], -m.m[2][2], f);
    return RadToU(atan2f(f[1], f[0]));
}

// ---------------------------------------------------------------------------
// Raise-to-aim accuracy: copy the weapon's "zoomed" aim error into its hip-fire slots while the
// controller is near the headset; put the originals back when it is lowered.

struct AccProp { const wchar_t* name; const wchar_t* zoomedName; int off, zoomedOff; };
static AccProp g_acc[] = {
    { L"AimErrorMax_Human_Standing",   L"AimErrorMax_Human_Zoomed", -1, -1 },
    { L"AimErrorMax_Human_MovingSlow", L"AimErrorMax_Human_Zoomed", -1, -1 },
    { L"AimErrorMax_Human_MovingFast", L"AimErrorMax_Human_Zoomed", -1, -1 },
    { L"AimErrorMax_Human_Crouched",   L"AimErrorMax_Human_Zoomed", -1, -1 },
    { L"AimErrorMin_Human",            L"AimErrorMin_Human_Zoomed", -1, -1 },
};
static const int kAcc = sizeof g_acc / sizeof g_acc[0];
static int   g_accBase;                 // offset of the struct holding them (0 if directly on the class)
static void* g_accHolderClass;          // class whose instances carry the values
static int   g_weaponOff = -1, g_fireWeaponOff = -1;
static void* g_fmWeapon; static void* g_fireModes[4]; static int g_fireModeIdx[4]; static int g_numFireModes;
static bool  g_accReady;
static void* g_accTarget;               // object currently boosted
static DWORD g_accSaved[kAcc];

typedef const wchar_t* (__fastcall* GetName_t)(void* self, void* edx);
static GetName_t pGetName;

static void* FindProp(void* outer, const wchar_t* name) {
    HMODULE core = GetModuleHandleA("Core.dll");
    auto propClass = (StaticClass_t)GetProcAddress(core, "?StaticClass@UProperty@@SAPAVUClass@@XZ");
    return pStaticFindObject(propClass(), outer, name, 0);
}
static int PropOffset(void* prop) { return prop ? *(int*)((BYTE*)prop + g_offsetField) : -1; }
static void* PropOuter(void* prop) { return *(void**)((BYTE*)prop + 0x1c); }

static void InitAccuracy() {
    static bool tried; if (tried || g_offsetField < 0) return; tried = true;
    HMODULE core = GetModuleHandleA("Core.dll");
    pGetName = (GetName_t)GetProcAddress(core, "?GetName@UObject@@QBEPBGXZ");
    auto classClass = (StaticClass_t)GetProcAddress(core, "?StaticClass@UClass@@SAPAV1@XZ");
    void* anyPkg = (void*)-1;
    void* controllerClass = pStaticFindObject(classClass(), anyPkg, L"Controller", 0);
    void* pawnClass = pStaticFindObject(classClass(), anyPkg, L"Pawn", 0);
    g_pawnOff = PropOffset(controllerClass ? FindProp(controllerClass, L"Pawn") : nullptr);
    g_weaponOff = PropOffset(pawnClass ? FindProp(pawnClass, L"Weapon") : nullptr);

    void* p0 = FindProp(anyPkg, g_acc[0].name);
    if (!p0) { Log("accuracy: %ls not found", g_acc[0].name); return; }
    void* outer = PropOuter(p0);
    bool outerIsClass = pIsA(outer, nullptr, classClass()) != 0;
    Log("accuracy: Controller.Pawn @0x%x Pawn.Weapon @0x%x; %ls lives in %ls (%s)", g_pawnOff, g_weaponOff,
        g_acc[0].name, pGetName ? pGetName(outer, nullptr) : L"?", outerIsClass ? "class" : "struct");
    if (outerIsClass) { g_accHolderClass = outer; g_accBase = 0; }
    else {
        // Values sit in a struct; find the class property of that struct type ("WeaponAimError").
        void* holderProp = FindProp(anyPkg, L"WeaponAimError");
        if (!holderProp) { Log("accuracy: WeaponAimError holder not found"); return; }
        g_accHolderClass = PropOuter(holderProp);
        g_accBase = PropOffset(holderProp);
        Log("accuracy: struct held by %ls at 0x%x", pGetName ? pGetName(g_accHolderClass, nullptr) : L"?", g_accBase);
    }
    for (int i = 0; i < kAcc; ++i) {
        g_acc[i].off = PropOffset(FindProp(outer, g_acc[i].name));
        g_acc[i].zoomedOff = PropOffset(FindProp(outer, g_acc[i].zoomedName));
        Log("accuracy: %ls @0x%x <- %ls @0x%x", g_acc[i].name, g_acc[i].off, g_acc[i].zoomedName, g_acc[i].zoomedOff);
        if (g_acc[i].off < 0 || g_acc[i].zoomedOff < 0) return;
    }
    g_fireWeaponOff = PropOffset(outerIsClass ? FindProp(outer, L"Weapon") : nullptr);
    Log("accuracy: %ls.Weapon @0x%x", pGetName ? pGetName(outer, nullptr) : L"?", g_fireWeaponOff);
    g_accReady = g_pawnOff > 0 && g_weaponOff > 0;
}

static void AccRestore() {
    if (!g_accTarget) return;
    BYTE* b = (BYTE*)g_accTarget + g_accBase;
    for (int i = 0; i < kAcc; ++i) *(DWORD*)(b + g_acc[i].off) = g_accSaved[i];
    g_accTarget = nullptr;
}

// Fire-mode objects (WeaponFire) belonging to the given weapon; rescanned when the weapon changes.
static int FireModesFor(void* weapon) {
    if (weapon != g_fmWeapon) {
        g_fmWeapon = weapon; g_numFireModes = 0;
        if (weapon && g_fireWeaponOff > 0)
            for (int i = 0; i < pGObjObjects->Num && g_numFireModes < 4; ++i) {
                void* o = pGObjObjects->Data[i];
                if (o && pIsA(o, nullptr, g_accHolderClass) && *(void**)((BYTE*)o + g_fireWeaponOff) == weapon) {
                    g_fireModeIdx[g_numFireModes] = i; g_fireModes[g_numFireModes++] = o;
                }
            }
        Log("weapon %p has %d fire mode(s)", weapon, g_numFireModes);
    }
    // Drop any that were freed.
    for (int k = 0; k < g_numFireModes; ++k) {
        int i = g_fireModeIdx[k];
        if (i >= pGObjObjects->Num || pGObjObjects->Data[i] != g_fireModes[k]) { g_fmWeapon = nullptr; return 0; }
    }
    return g_numFireModes;
}

static void MeasureWorldScale(void* pawn) {
    static bool done; if (done || !g_autoScale || !pawn || g_offsetField < 0) return;
    void* anyPkg = (void*)-1;
    HMODULE core = GetModuleHandleA("Core.dll");
    auto classClass = (StaticClass_t)GetProcAddress(core, "?StaticClass@UClass@@SAPAV1@XZ");
    void* actorClass = pStaticFindObject(classClass(), anyPkg, L"Actor", 0);
    void* pawnClass = pStaticFindObject(classClass(), anyPkg, L"Pawn", 0);
    int chOff = PropOffset(actorClass ? FindProp(actorClass, L"CollisionHeight") : nullptr);
    int beOff = PropOffset(pawnClass ? FindProp(pawnClass, L"BaseEyeHeight") : nullptr);
    if (chOff < 0 || beOff < 0) { done = true; Log("world scale: properties not found"); return; }
    float ch = *(float*)((BYTE*)pawn + chOff), be = *(float*)((BYTE*)pawn + beOff);
    float eye = ch + be;
    done = true;
    if (eye < 40.f || eye > 400.f) { Log("world scale: implausible eye height %.1f (collision %.1f + eye %.1f), keeping %.1f", eye, ch, be, g_cfg.worldScale); return; }
    g_cfg.worldScale = eye / g_cfg.eyeHeightMetres;
    Log("world scale: eye height %.1f units (collision %.1f + eye %.1f) / %.2f m -> %.1f units per metre", eye, ch, be, g_cfg.eyeHeightMetres, g_cfg.worldScale);
}

static void UpdateRaiseAccuracy() {
    if (!g_cfg.raiseAccuracy || !g_pc || !g_handValid || !g_hmdValid) { AccRestore(); return; }
    InitAccuracy();
    if (!g_accReady) return;
    void* pawn = *(void**)((BYTE*)g_pc + g_pawnOff);
    MeasureWorldScale(pawn);
    void* weapon = pawn ? *(void**)((BYTE*)pawn + g_weaponOff) : nullptr;
    int nfm = FireModesFor(weapon);
    if (g_cfg.raiseAccuracy >= 2) {
        static void* loggedFor;
        for (int k = 0; k < nfm; ++k) {
            BYTE* b = (BYTE*)g_fireModes[k] + g_accBase;
            if (loggedFor != weapon && k == 0)
                Log("accuracy mode %d: hip aim error %d, zoomed %d", g_cfg.raiseAccuracy, *(int*)(b + g_acc[0].off), *(int*)(b + g_acc[0].zoomedOff));
            for (int i = 0; i < kAcc; ++i) {
                if (g_cfg.raiseAccuracy == 3) { *(int*)(b + g_acc[i].off) = 0; *(int*)(b + g_acc[i].zoomedOff) = 0; }
                else *(DWORD*)(b + g_acc[i].off) = *(DWORD*)(b + g_acc[i].zoomedOff);
            }
        }
        loggedFor = weapon;
        return;
    }
    void* target = nfm ? g_fireModes[0] : nullptr;

    float dx = g_handPos[0] - g_hmd.m[0][3], dy = g_handPos[1] - g_hmd.m[1][3], dz = g_handPos[2] - g_hmd.m[2][3];
    float dist = sqrtf(dx * dx + dy * dy + dz * dz);
    static bool raised;
    raised = raised ? dist < g_cfg.raiseDistance + 0.05f : dist < g_cfg.raiseDistance;

    if (g_accTarget && (g_accTarget != target || !raised)) {
        AccRestore();
        static int logs; if (logs++ < 20) Log("raise-to-aim off (%.2f m)", dist);
    }
    if (raised && target && !g_accTarget) {
        BYTE* b = (BYTE*)target + g_accBase;
        for (int i = 0; i < kAcc; ++i) {
            g_accSaved[i] = *(DWORD*)(b + g_acc[i].off);
            *(DWORD*)(b + g_acc[i].off) = *(DWORD*)(b + g_acc[i].zoomedOff);
        }
        g_accTarget = target;
        static int logs; if (logs++ < 20) {
            float f0 = *(float*)&g_accSaved[0], f1 = *(float*)(b + g_acc[0].off);
            Log("raise-to-aim on (%.2f m): standing aim error %g -> %g (as float) / %d -> %d (as int)",
                dist, f0, f1, (int)g_accSaved[0], *(int*)(b + g_acc[0].off));
        }
    }
}

static void UpdateAim() {
    static bool keyWas;
    bool key = (GetAsyncKeyState(g_cfg.toggleAimKey) & 0x8000) != 0;
    if (key && !keyWas) { g_cfg.controllerAim = !g_cfg.controllerAim; Log("controller aim %s", g_cfg.controllerAim ? "on" : "off"); }
    keyWas = key;

    bool wasActive = g_aimActive;
    g_aimActive = false;
    if (g_menuMode) { ReleaseAllActions(); AccRestore(); return; }
    InitReflection();
    FRotator* pcRot = g_cfg.controllerAim ? PlayerRotation() : nullptr;
    if (pcRot && !wasActive) {
        // Keep the current view direction when controller aiming kicks in.
        g_bodyYaw = (pcRot->Yaw - (g_hmdValid ? HeadYawU() : 0)) & 0xFFFF;
        Log("controller aim active, body yaw %d", g_bodyYaw);
    }
    // If the camera never follows what we write, we're steering the wrong (old) PlayerController.
    static int lastWrittenYaw = -1, mismatch;
    if (pcRot && lastWrittenYaw >= 0) {
        if (YawDiff(g_lastGameRot.Yaw, lastWrittenYaw) > 1500) ++mismatch; else mismatch = 0;
        if (mismatch > 20) {
            Log("PlayerController %p is not driving the camera (camera yaw %d, written %d); rescanning", g_pc, g_lastGameRot.Yaw, lastWrittenYaw);
            g_pc = nullptr; g_pcIndex = -1; g_forceScan = true; mismatch = 0; lastWrittenYaw = -1;
            pcRot = PlayerRotation();
        }
    }
    FRotator aim;
    bool aimOk = UpdateControllers(aim);
    if (pcRot && aimOk) { pcRot->Pitch = aim.Pitch; pcRot->Yaw = aim.Yaw; pcRot->Roll = 0; lastWrittenYaw = aim.Yaw; }
    else lastWrittenYaw = -1;
    static int aimLogs, lastWrittenPitch = 0;
    if (pcRot && aimOk && aimLogs < 40 && g_framesRendered % 45 == 0) {
        ++aimLogs;
        int sp = (aim.Pitch > 32767 ? aim.Pitch - 65536 : aim.Pitch), gp = (g_lastGameRot.Pitch > 32767 ? g_lastGameRot.Pitch - 65536 : g_lastGameRot.Pitch);
        int lw = (lastWrittenPitch > 32767 ? lastWrittenPitch - 65536 : lastWrittenPitch);
        Log("aim pitch written %d (%.0f deg), game camera pitch after last write %d (last written %d)", sp, sp * 360.f / 65536.f, gp, lw);
    }
    if (pcRot && aimOk) lastWrittenPitch = aim.Pitch;

    static bool laserKeyWas;
    bool lk = (GetAsyncKeyState(g_cfg.laserKey) & 0x8000) != 0;
    if (lk && !laserKeyWas) g_cfg.gunLaser = !g_cfg.gunLaser;
    laserKeyWas = lk;
    if (pcRot && aimOk && g_cfg.gunLaser && g_handValid) {
        float o[3] = { g_handPose.m[0][3], g_handPose.m[1][3], g_handPose.m[2][3] };
        ShowLaserAxes(o, g_aimVR[0], g_aimVR[1], g_aimVR[2], g_laserLen);
    }
    else if (!g_menuMode) HideLaser();
    g_aimActive = pcRot != nullptr;
    if (g_aimActive) UpdateRaiseAccuracy(); else AccRestore();
}

static void InitGuiReflection() {
    static bool tried; if (tried || g_offsetField < 0) return; tried = true;
    HMODULE core = GetModuleHandleA("Core.dll");
    auto classClass = (StaticClass_t)GetProcAddress(core, "?StaticClass@UClass@@SAPAV1@XZ");
    auto propClass = (StaticClass_t)GetProcAddress(core, "?StaticClass@UProperty@@SAPAVUClass@@XZ");
    g_guiClass = pStaticFindObject(classClass(), (void*)-1, L"GUIController", 0);
    void* mx = g_guiClass ? pStaticFindObject(propClass(), g_guiClass, L"MouseX", 0) : nullptr;
    void* my = g_guiClass ? pStaticFindObject(propClass(), g_guiClass, L"MouseY", 0) : nullptr;
    if (mx) g_mouseXOff = *(int*)((BYTE*)mx + g_offsetField);
    if (my) g_mouseYOff = *(int*)((BYTE*)my + g_offsetField);
    Log("GUIController %p MouseX @0x%x MouseY @0x%x", g_guiClass, g_mouseXOff, g_mouseYOff);
}

static float* GuiMouse() {
    InitReflection();
    InitGuiReflection();
    if (g_mouseXOff < 0 || g_mouseYOff < 0 || !pGObjObjects) return nullptr;
    if (!(g_gui && g_guiIndex >= 0 && g_guiIndex < pGObjObjects->Num && pGObjObjects->Data[g_guiIndex] == g_gui)) {
        g_gui = nullptr; g_guiIndex = -1;
        for (int i = 0; i < pGObjObjects->Num; ++i) {
            void* o = pGObjObjects->Data[i];
            if (o && pIsA(o, nullptr, g_guiClass)) { g_gui = o; g_guiIndex = i; Log("GUIController instance %p", o); break; }
        }
        if (!g_gui) return nullptr;
    }
    return (float*)g_gui;
}

typedef void (__fastcall* LookUnderCursor_t)(void* self, void* edx, float dx, float dy);
static LookUnderCursor_t pLookUnderCursor;
static bool g_hoverBroken;
static void GuiHover(void* gui, float dx, float dy) {
    if (g_hoverBroken) return;
    if (!pLookUnderCursor) {
        HMODULE h = GetModuleHandleA("GUI.dll");
        pLookUnderCursor = h ? (LookUnderCursor_t)GetProcAddress(h, "?LookUnderCursor@UGUIController@@UAEXMM@Z") : nullptr;
        if (!pLookUnderCursor) { g_hoverBroken = true; return; }
    }
    __try { pLookUnderCursor(gui, nullptr, dx, dy); }
    __except (EXCEPTION_EXECUTE_HANDLER) { g_hoverBroken = true; Log("menu hover call failed; disabled"); }
}

static vr::VROverlayHandle_t g_laser = vr::k_ulOverlayHandleInvalid;
static const int kLaserW = 1, kLaserH = 1024;
static const float kLaserWidthM = 0.004f;
static void ShowLaserAxes(const float origin[3], const float f[3], const float u[3], const float r[3], float length);
static void ShowLaser(const vr::HmdMatrix34_t& m, float length) {
    float f[3], u[3], r[3]; AimAxesVR(m, f, u, r);
    float o[3] = { m.m[0][3], m.m[1][3], m.m[2][3] };
    ShowLaserAxes(o, f, u, r, length);
}
static void ShowLaserAxes(const float origin[3], const float f[3], const float u[3], const float r[3], float length) {
    if (g_laser == vr::k_ulOverlayHandleInvalid) {
        if (vr::VROverlay()->CreateOverlay("bia.vr.laser", "Pointer", &g_laser) != vr::VROverlayError_None) { g_laser = vr::k_ulOverlayHandleInvalid; return; }
        static uint8_t px[kLaserW * kLaserH * 4];
        for (int y = 0; y < kLaserH; ++y)
            for (int x = 0; x < kLaserW; ++x) {
                uint8_t* p = &px[(y * kLaserW + x) * 4];
                p[0] = 255; p[1] = 20; p[2] = 20; p[3] = (uint8_t)(230 - y * 110 / kLaserH);
            }
        vr::VROverlay()->SetOverlayRaw(g_laser, px, kLaserW, kLaserH, 4);
        vr::VROverlay()->SetOverlaySortOrder(g_laser, 10);
    }
    // Overlay quad: local X = width, local Y = height (texture rows run top->bottom, so Y points back along the ray).
    // Height = width * texH/texW / texelAspect, so pick the aspect that gives the wanted length.
    float natural = kLaserWidthM * kLaserH / kLaserW;
    float aspect = natural / max(length, 0.05f);
    vr::VROverlay()->SetOverlayWidthInMeters(g_laser, kLaserWidthM);
    vr::VROverlay()->SetOverlayTexelAspect(g_laser, aspect);
    float n[3] = { r[1] * f[2] - r[2] * f[1], r[2] * f[0] - r[0] * f[2], r[0] * f[1] - r[1] * f[0] };   // right x fwd
    float c[3]; for (int i = 0; i < 3; ++i) c[i] = origin[i] + f[i] * length * 0.5f;
    vr::HmdMatrix34_t t = {{
        { r[0], -f[0], -n[0], c[0] },
        { r[1], -f[1], -n[1], c[1] },
        { r[2], -f[2], -n[2], c[2] },
    }};
    vr::VROverlay()->SetOverlayTransformAbsolute(g_laser, vr::TrackingUniverseSeated, &t);
    vr::VROverlay()->ShowOverlay(g_laser);
}
static void HideLaser() { if (g_laser != vr::k_ulOverlayHandleInvalid) vr::VROverlay()->HideOverlay(g_laser); }

// Per weapon: the gun-model offset the game itself uses while aiming down sights (R1), which puts
// the sights exactly on the aim line. Learned while R1 is held, saved in BiAVR.ini [Sights].
static SightCal g_sights[32]; static int g_numSights;
typedef const wchar_t* (__fastcall* GetNameFn_t)(void* self, void* edx);
static const wchar_t* ObjClassName(void* obj) {
    static GetNameFn_t getName = (GetNameFn_t)GetProcAddress(GetModuleHandleA("Core.dll"), "?GetName@UObject@@QBEPBGXZ");
    void* cls = obj ? *(void**)((BYTE*)obj + 0x28) : nullptr;     // UObject::Class
    return cls && getName ? getName(cls, nullptr) : L"";
}
static SightCal* FindSight(const wchar_t* cls, bool create) {
    for (int i = 0; i < g_numSights; ++i) if (!wcscmp(g_sights[i].cls, cls)) return &g_sights[i];
    char a[64]; WideCharToMultiByte(CP_ACP, 0, cls, -1, a, sizeof a, nullptr, nullptr);
    char v[64]; GetPrivateProfileStringA("Sights", a, "", v, sizeof v, g_iniPath);
    SightCal sc = {}; wcsncpy_s(sc.cls, cls, _TRUNCATE);
    bool have = v[0] && sscanf_s(v, "%f,%f,%f", &sc.f, &sc.r, &sc.u) == 3;
    if (!have && !create) return nullptr;
    if (g_numSights >= 32) return nullptr;
    g_sights[g_numSights] = sc;
    if (have) Log("sights for %ls loaded: %.1f, %.1f, %.1f", cls, sc.f, sc.r, sc.u);
    return &g_sights[g_numSights++];
}
static void SaveSight(const SightCal* sc) {
    char a[64]; WideCharToMultiByte(CP_ACP, 0, sc->cls, -1, a, sizeof a, nullptr, nullptr);
    char v[64]; sprintf_s(v, "%.1f,%.1f,%.1f", sc->f, sc->r, sc->u);
    WritePrivateProfileStringA("Sights", a, v, g_iniPath);
    Log("sights for %s learned: %s", a, v);
}

// Bone probe for the left-arm work: which bones the first-person arms have, and where.
typedef void (__fastcall* FNameCtor_t)(void* self, void* edx, const wchar_t* name, int find);
typedef int (__fastcall* MatchRefBone_t)(void* inst, void* edx, int name);
typedef void* (__fastcall* GetBoneCoords_t)(void* inst, void* edx, float* out, DWORD idx);
static void ProbeBones(void* actor) {
    static void* probed[8]; static int nprobed;
    for (int i = 0; i < nprobed; ++i) if (probed[i] == actor) return;
    if (nprobed >= 8) return;
    probed[nprobed++] = actor;
    HMODULE core = GetModuleHandleA("Core.dll"), eng = GetModuleHandleA("Engine.dll");
    auto fname = (FNameCtor_t)GetProcAddress(core, "??0FName@@QAE@PBGW4EFindName@@@Z");
    auto match = (MatchRefBone_t)GetProcAddress(eng, "?MatchRefBone@USkeletalMeshInstance@@QAEHVFName@@@Z");
    auto coords = (GetBoneCoords_t)GetProcAddress(eng, "?GetBoneCoords@USkeletalMeshInstance@@QAE?AVFCoords@@K@Z");
    auto classClass = (StaticClass_t)GetProcAddress(core, "?StaticClass@UClass@@SAPAV1@XZ");
    void* actorClass = pStaticFindObject(classClass(), (void*)-1, L"Actor", 0);
    int miOff = PropOffset(actorClass ? FindProp(actorClass, L"MeshInstance") : nullptr);
    void* mi = miOff > 0 ? *(void**)((BYTE*)actor + miOff) : nullptr;
    Log("bones of %ls: MeshInstance @0x%x = %p", ObjClassName(actor), miOff, mi);
    if (!mi || !fname || !match || !coords) return;
    static const wchar_t* names[] = {
        L"Bip01 L Clavicle", L"Bip01 L UpperArm", L"Bip01 L Forearm", L"Bip01 L Hand",
        L"Bip01 R Clavicle", L"Bip01 R UpperArm", L"Bip01 R Forearm", L"Bip01 R Hand",
        L"L_Clavicle", L"L_UpperArm", L"L_Forearm", L"L_Hand", L"LeftArm", L"LeftForeArm", L"LeftHand",
        L"lclavicle", L"lupperarm", L"lforearm", L"lhand", L"Bip01", L"Bip01 Spine", L"weapon", L"Weapon", L"root" };
    __try {
        for (const wchar_t* n : names) {
            int fn = 0; fname(&fn, nullptr, n, 0);
            int idx = fn ? match(mi, nullptr, fn) : -1;
            if (fn && idx >= 0) Log("  bone '%ls' = #%d", n, idx);
        }
        for (DWORD i = 0; i < 80; ++i) {
            float c[12] = {};
            coords(mi, nullptr, c, i);
            if (c[0] == 0 && c[1] == 0 && c[2] == 0) continue;
            if (!(fabsf(c[0]) < 1e6f)) break;
            Log("  bone #%u at (%.1f, %.1f, %.1f)", i, c[0], c[1], c[2]);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { Log("  bone probe failed"); }
}

static bool ZoomHeld() { return g_actState[A_ZOOM]; }

// ---------------------------------------------------------------------------
// Left arm IK: bend Bip01 L UpperArm / L Forearm / L Hand of the first-person arms toward the
// left controller. Shoulder position comes from the animated skeleton; elbow from a 2-bone solve.

typedef int (__fastcall* SetBoneDirection_t)(void* inst, void* edx, int name, FRotator turn, FVector trans, float alpha, int space);

static FRotator DirToRot(const float d[3]) {
    FRotator r; r.Yaw = RadToU(atan2f(d[1], d[0])); r.Pitch = RadToU(atan2f(d[2], sqrtf(d[0] * d[0] + d[1] * d[1]))); r.Roll = 0; return r;
}

static void SolveLeftArm(void* actor, const FVector& gameLoc) {
    if (!g_cfg.leftArmIK || !g_leftValid) return;
    // Titanfall-style: while the left hand holds the fore-end (two-handed), the weapon's own grip
    // pose shows the hand on the gun; otherwise the arm follows the controller. Blend ~0.15 s.
    static float follow = 1.f;
    if (g_eye == 0) { float target = g_twoHand ? 0.f : 1.f; float step = 1.f / 13.f; follow += max(-step, min(step, target - follow)); }
    float alpha = follow;
    static FNameCtor_t fname; static MatchRefBone_t match; static GetBoneCoords_t coords; static SetBoneDirection_t setDir; static int miOff = -2;
    if (miOff == -2) {
        HMODULE core = GetModuleHandleA("Core.dll"), eng = GetModuleHandleA("Engine.dll");
        fname = (FNameCtor_t)GetProcAddress(core, "??0FName@@QAE@PBGW4EFindName@@@Z");
        match = (MatchRefBone_t)GetProcAddress(eng, "?MatchRefBone@USkeletalMeshInstance@@QAEHVFName@@@Z");
        coords = (GetBoneCoords_t)GetProcAddress(eng, "?GetBoneCoords@USkeletalMeshInstance@@QAE?AVFCoords@@K@Z");
        setDir = (SetBoneDirection_t)GetProcAddress(eng, "?SetBoneDirection@USkeletalMeshInstance@@QAEHVFName@@VFRotator@@VFVector@@MH@Z");
        auto classClass = (StaticClass_t)GetProcAddress(core, "?StaticClass@UClass@@SAPAV1@XZ");
        void* actorClass = pStaticFindObject(classClass(), (void*)-1, L"Actor", 0);
        miOff = PropOffset(actorClass ? FindProp(actorClass, L"MeshInstance") : nullptr);
        Log("left arm IK: SetBoneDirection %p, MeshInstance @0x%x", setDir, miOff);
    }
    if (!fname || !match || !coords || !setDir || miOff <= 0) return;
    void* mi = *(void**)((BYTE*)actor + miOff);
    if (!mi) return;
    __try {
        int nUpper = 0, nFore = 0, nHand = 0;
        fname(&nUpper, nullptr, L"Bip01 L UpperArm", 0);
        fname(&nFore, nullptr, L"Bip01 L Forearm", 0);
        fname(&nHand, nullptr, L"Bip01 L Hand", 0);
        int iU = match(mi, nullptr, nUpper), iF = match(mi, nullptr, nFore), iH = match(mi, nullptr, nHand);
        if (iU < 0 || iF < 0 || iH < 0) return;
        float cu[12], cf[12], ch[12];
        coords(mi, nullptr, cu, iU); coords(mi, nullptr, cf, iF); coords(mi, nullptr, ch, iH);
        float S[3] = { cu[0], cu[1], cu[2] };
        float L1 = sqrtf((cf[0] - cu[0]) * (cf[0] - cu[0]) + (cf[1] - cu[1]) * (cf[1] - cu[1]) + (cf[2] - cu[2]) * (cf[2] - cu[2]));
        float L2 = sqrtf((ch[0] - cf[0]) * (ch[0] - cf[0]) + (ch[1] - cf[1]) * (ch[1] - cf[1]) + (ch[2] - cf[2]) * (ch[2] - cf[2]));
        if (L1 < 1.f || L2 < 1.f) return;

        // Target: the real left hand, in world units.
        FRotator body = { 0, g_bodyYaw, 0 };
        float BX[3], BY[3], BZ[3]; RotToAxes(body, BX, BY, BZ);
        float w[3]; Combine(BX, BY, BZ, g_leftPos, w);
        float sc = g_cfg.worldScale;
        float T[3] = { gameLoc.X + w[0] * sc, gameLoc.Y + w[1] * sc, gameLoc.Z + w[2] * sc };

        // Two-bone solve with the elbow pointing down and out to the left.
        float st[3] = { T[0] - S[0], T[1] - S[1], T[2] - S[2] };
        float dist = sqrtf(st[0] * st[0] + st[1] * st[1] + st[2] * st[2]);
        if (dist < 1e-3f) return;
        float n[3] = { st[0] / dist, st[1] / dist, st[2] / dist };
        float reach = min(dist, (L1 + L2) * 0.999f);
        float a = (L1 * L1 - L2 * L2 + reach * reach) / (2.f * reach);
        float h = sqrtf(max(L1 * L1 - a * a, 0.f));
        float pole[3] = { -BY[0] * 0.5f, -BY[1] * 0.5f, -1.0f };       // down + toward the player's left
        float k = pole[0] * n[0] + pole[1] * n[1] + pole[2] * n[2];
        float pp[3] = { pole[0] - k * n[0], pole[1] - k * n[1], pole[2] - k * n[2] };
        float pl = sqrtf(pp[0] * pp[0] + pp[1] * pp[1] + pp[2] * pp[2]);
        if (pl < 1e-4f) return;
        float E[3]; for (int i = 0; i < 3; ++i) E[i] = S[i] + n[i] * a + pp[i] / pl * h;
        float Tc[3]; for (int i = 0; i < 3; ++i) Tc[i] = S[i] + n[i] * reach;

        float du[3] = { E[0] - S[0], E[1] - S[1], E[2] - S[2] };
        float df[3] = { Tc[0] - E[0], Tc[1] - E[1], Tc[2] - E[2] };
        FVector zero = { 0, 0, 0 };
        setDir(mi, nullptr, nUpper, DirToRot(du), zero, alpha, 1);
        setDir(mi, nullptr, nFore, DirToRot(df), zero, alpha, 1);
        // Hand: point along the controller's forward, in world.
        float hf[3], hr[3], hu[3];
        Combine(BX, BY, BZ, g_leftAxes[0], hf); Combine(BX, BY, BZ, g_leftAxes[1], hr); Combine(BX, BY, BZ, g_leftAxes[2], hu);
        FRotator handRot = AxesToRot(hf, hr, hu);
        handRot.Roll = (handRot.Roll + (int)(g_cfg.leftHandRoll * 65536.f / 360.f)) & 0xFFFF;
        setDir(mi, nullptr, nHand, handRot, zero, alpha, 1);

        static int logs;
        if (logs < 6 && g_framesRendered % 120 == 0) {
            ++logs;
            Log("left arm: shoulder (%.0f,%.0f,%.0f) target (%.0f,%.0f,%.0f) dist %.0f reach %.0f; hand bone was (%.0f,%.0f,%.0f)",
                S[0], S[1], S[2], T[0], T[1], T[2], dist, L1 + L2, ch[0], ch[1], ch[2]);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { g_cfg.leftArmIK = 0; Log("left arm IK crashed; disabled"); }
}


// Places the floating menu screen in front of where the head is facing (level, yaw only).
static vr::HmdMatrix34_t g_menuXform;
static void PlaceMenu() {
    if (g_menuOverlay == vr::k_ulOverlayHandleInvalid) return;
    const vr::HmdMatrix34_t& m = g_hmd;
    float fx = -m.m[0][2], fz = -m.m[2][2];
    float len = sqrtf(fx * fx + fz * fz);
    if (len < 1e-3f || !g_hmdValid) { fx = 0; fz = -1; len = 1; }
    fx /= len; fz /= len;
    float zx = -fx, zz = -fz;                     // overlay front faces back toward the player
    float d = g_cfg.menuDistance;
    float px = g_hmdValid ? m.m[0][3] : 0, py = g_hmdValid ? m.m[1][3] : 0, pz = g_hmdValid ? m.m[2][3] : 0;
    vr::HmdMatrix34_t t = {{
        { zz,  0, zx, px + fx * d },
        { 0,   1, 0,  py },
        { -zx, 0, zz, pz + fz * d },
    }};
    vr::VROverlay()->SetOverlayTransformAbsolute(g_menuOverlay, vr::TrackingUniverseSeated, &t);
    g_menuXform = t;
}

// Ray vs the floating screen. Returns UV with (0,0) at the top-left of the game image.
static bool MenuHit(const float o[3], const float d[3], float& u, float& v, float& dist) {
    const vr::HmdMatrix34_t& t = g_menuXform;
    float c[3] = { t.m[0][3], t.m[1][3], t.m[2][3] };
    float ax[3] = { t.m[0][0], t.m[1][0], t.m[2][0] }, ay[3] = { t.m[0][1], t.m[1][1], t.m[2][1] }, az[3] = { t.m[0][2], t.m[1][2], t.m[2][2] };
    float dn = d[0] * az[0] + d[1] * az[1] + d[2] * az[2];
    if (fabsf(dn) < 1e-4f) return false;
    float k = ((c[0] - o[0]) * az[0] + (c[1] - o[1]) * az[1] + (c[2] - o[2]) * az[2]) / dn;
    if (k <= 0) return false;
    float p[3] = { o[0] + d[0] * k - c[0], o[1] + d[1] * k - c[1], o[2] + d[2] * k - c[2] };
    float W = g_cfg.menuWidth, H = g_eyeW ? W * g_eyeH / g_eyeW : W * 0.5625f;
    u = (p[0] * ax[0] + p[1] * ax[1] + p[2] * ax[2]) / W + 0.5f;
    v = 0.5f - (p[0] * ay[0] + p[1] * ay[1] + p[2] * ay[2]) / H;
    dist = k;
    return u >= 0 && u <= 1 && v >= 0 && v <= 1;
}

static void ShowMenuFrame() {
    if (!g_eyeTex9[0]) return;
    if (g_menuOverlay == vr::k_ulOverlayHandleInvalid) {
        vr::EVROverlayError oe = vr::VROverlay()->CreateOverlay("bia.vr.menu", "Brothers in Arms", &g_menuOverlay);
        Log("menu overlay create %d", oe);
        if (oe != vr::VROverlayError_None) { g_menuOverlay = vr::k_ulOverlayHandleInvalid; return; }
        vr::VROverlay()->SetOverlayWidthInMeters(g_menuOverlay, g_cfg.menuWidth);
        vr::VROverlay()->SetOverlayInputMethod(g_menuOverlay, vr::VROverlayInputMethod_Mouse);
        vr::HmdVector2_t scale = { 1.f, 1.f };
        vr::VROverlay()->SetOverlayMouseScale(g_menuOverlay, &scale);
    }
    // Right controller pointing at the floating screen moves the mouse; trigger clicks.
    {
        vr::TrackedDeviceIndex_t ri = g_vr->GetTrackedDeviceIndexForControllerRole(vr::TrackedControllerRole_RightHand);
        vr::VRControllerState_t rs = {};
        if (ri != vr::k_unTrackedDeviceIndexInvalid && g_poses[ri].bPoseIsValid && g_vr->GetControllerState(ri, &rs, sizeof rs)) {
            const vr::HmdMatrix34_t& m = g_poses[ri].mDeviceToAbsoluteTracking;
            vr::VROverlayIntersectionParams_t ip = {};
            ip.vSource.v[0] = m.m[0][3]; ip.vSource.v[1] = m.m[1][3]; ip.vSource.v[2] = m.m[2][3];
            float af[3], au[3], ar[3]; AimAxesVR(m, af, au, ar);
            ip.vDirection.v[0] = af[0]; ip.vDirection.v[1] = af[1]; ip.vDirection.v[2] = af[2];
            ip.eOrigin = vr::TrackingUniverseSeated;
            vr::VROverlayIntersectionResults_t ir = {};
            static bool trigWas;
            bool trig = rs.rAxis[1].x > 0.6f;
            float hu, hv, hd;
            float ro[3] = { ip.vSource.v[0], ip.vSource.v[1], ip.vSource.v[2] }, rd[3] = { ip.vDirection.v[0], ip.vDirection.v[1], ip.vDirection.v[2] };
            if (MenuHit(ro, rd, hu, hv, hd)) {
                ir.vUVs.v[0] = hu; ir.vUVs.v[1] = 1.f - hv; ir.fDistance = hd;
                ShowLaser(m, ir.fDistance);
                float* gui = GuiMouse();
                if (gui && g_projW) {
                    float* mx = (float*)((BYTE*)gui + g_mouseXOff); float* my = (float*)((BYTE*)gui + g_mouseYOff);
                    float nx = ir.vUVs.v[0] * g_projW, ny = (1.f - ir.vUVs.v[1]) * g_projH;
                    float dx = nx - *mx, dy = ny - *my;
                    *mx = nx; *my = ny;
                    if (fabsf(dx) + fabsf(dy) > 0.5f) {
                        // Nudge the real mouse by one count so the game runs its own hover logic.
                        static int sign = 1; sign = -sign;
                        INPUT nudge = {}; nudge.type = INPUT_MOUSE; nudge.mi.dx = sign; nudge.mi.dwFlags = MOUSEEVENTF_MOVE;
                        SendInput(1, &nudge, sizeof nudge);
                    }
                }
                if (trig != trigWas) {
                    INPUT c = {}; c.type = INPUT_MOUSE; c.mi.dwFlags = trig ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP;
                    SendInput(1, &c, sizeof c);
                    static int logs; if (logs++ < 10) Log("menu point click %s at %.2f,%.2f", trig ? "down" : "up", ir.vUVs.v[0], ir.vUVs.v[1]);
                }
            } else ShowLaser(m, 3.0f);
            trigWas = trig;
        }
    }
    // Laser pointer on the floating screen drives the Windows mouse (normalised 0..1, y from bottom).
    vr::VREvent_t ev;
    while (vr::VROverlay()->PollNextOverlayEvent(g_menuOverlay, &ev, sizeof ev)) {
        INPUT in = {}; in.type = INPUT_MOUSE;
        switch (ev.eventType) {
        case vr::VREvent_MouseMove:
            in.mi.dx = (LONG)(ev.data.mouse.x * 65535.f);
            in.mi.dy = (LONG)((1.f - ev.data.mouse.y) * 65535.f);
            in.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE;
            SendInput(1, &in, sizeof in);
            break;
        case vr::VREvent_MouseButtonDown:
        case vr::VREvent_MouseButtonUp: {
            if (g_framesRendered == 0 && !g_menuVisible) break;
            bool down = ev.eventType == vr::VREvent_MouseButtonDown;
            bool left = ev.data.mouse.button != vr::VRMouseButton_Right;
            in.mi.dwFlags = left ? (down ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP) : (down ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP);
            SendInput(1, &in, sizeof in);
            static int clickLogs; if (clickLogs++ < 10) Log("menu click %s at %.2f,%.2f", down ? "down" : "up", ev.data.mouse.x, ev.data.mouse.y);
            break;
        }
        }
    }
    // Black world behind the menu.
    IDirect3DSurface9* s = nullptr;
    if (SUCCEEDED(g_eyeTex9[1]->GetSurfaceLevel(0, &s))) { g_dev->ColorFill(s, nullptr, D3DCOLOR_XRGB(0, 0, 0)); s->Release(); }
    WaitGpu();
    vr::Texture_t tex = EyeTexture(0);
    vr::VROverlay()->SetOverlayTexture(g_menuOverlay, &tex);
    if (!g_menuVisible) { PlaceMenu(); vr::VROverlay()->ShowOverlay(g_menuOverlay); g_menuVisible = true; Log("menu screen on"); }
    SubmitEyes(1);
}

static void HideMenu() {
    if (g_menuVisible) { vr::VROverlay()->HideOverlay(g_menuOverlay); g_menuVisible = false; HideLaser(); Log("menu screen off"); }
}

static void __fastcall hkDraw(void* self, void* edx, void* viewport, int blit, BYTE* hitData, int* hitSize) {
    if (!g_vrOk || !g_dev || hitData) { oDraw(self, edx, viewport, blit, hitData, hitSize); return; }

    PumpVR();
    if (!g_vrOk) { HideMenu(); oDraw(self, edx, viewport, blit, hitData, hitSize); return; }
    vr::VRCompositor()->WaitGetPoses(g_poses, vr::k_unMaxTrackedDeviceCount, nullptr, 0);
    g_hmdValid = g_poses[vr::k_unTrackedDeviceIndex_Hmd].bPoseIsValid;
    if (g_hmdValid) g_hmd = g_poses[vr::k_unTrackedDeviceIndex_Hmd].mDeviceToAbsoluteTracking;

    {
        IDirect3DSurface9* bb = nullptr;
        if (SUCCEEDED(g_dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb))) {
            D3DSURFACE_DESC d; bb->GetDesc(&d); g_bbPtr = bb; bb->Release(); FitProjection(d.Width, d.Height);
        }
    }

    TryHookGui();
    UpdateAim();
    g_inDraw = true; g_sceneSeen = false; g_guiSeen = false;
    if (g_menuMode) {
        // Menu: draw once, flat, captured on Present (or after), and shown on the floating screen.
        g_eye = 2; g_capturedThisEye = false;
        oDraw(self, edx, viewport, blit, nullptr, nullptr);
        if (!g_capturedThisEye) CaptureEye(0);
        g_eye = -1; g_inDraw = false;
        if (g_sceneSeen && !g_guiSeen) { g_menuMode = false; Log("menu closed"); }   // gameplay is back; go stereo next frame
        ShowMenuFrame();
        return;
    }

    memset(&g_stats, 0, sizeof g_stats);
    g_exactUsed[0] = g_exactUsed[1] = false;
    g_eye = 0; g_capturedThisEye = false; g_afterScene = false; g_rtIsBB = true;
    oDraw(self, edx, viewport, 0, nullptr, nullptr);
    CaptureEye(0);
    if (!g_sceneSeen || g_guiSeen) {              // no 3D world, or a menu on top of it
        g_eye = -1; g_inDraw = false; g_menuMode = true;
        Log("menu opened (world drawn=%d, gui=%d)", g_sceneSeen, g_guiSeen);
        ShowMenuFrame();
        return;
    }
    HideMenu();

    g_eye = 1; g_capturedThisEye = false; g_afterScene = false; g_rtIsBB = true;
    oDraw(self, edx, viewport, blit, nullptr, nullptr);   // may Present; hkPresent captures first
    if (!g_capturedThisEye) CaptureEye(1);
    g_eye = -1; g_inDraw = false;

    SubmitEyes();
    if (++g_framesRendered == 1) Log("first stereo frame submitted");
    if (g_statFrames < 3 || g_framesRendered % 3000 == 0) {
        ++g_statFrames;
        Log("frame %d: lens replaced %d (game lens _11=%.3f _22=%.3f), hud matrices scaled %d, draws %d (screen-space %d, shader %d), exact L=%d R=%d",
            g_framesRendered, g_stats.persp, g_stats.gameA, g_stats.gameB, g_stats.ortho, g_stats.draws, g_stats.rhwDraws,
            g_stats.shaderDraws, g_exactUsed[0], g_exactUsed[1]);
    }
}

// While the trigger is held, the weapon's long hit-scan trace from near the player's eye is
// re-aimed to start at the gun and follow its sight line (exactly the laser).
static int __fastcall hkSingleLineCheck(void* level, void* edx, void* hit, void* source, const FVector* end, const FVector* start, DWORD flags, const FVector* extent) {
    if (!g_inOwnTrace && g_cfg.gunTraceFire == 2 && g_gunRayValid && g_actState[A_FIRE] && g_pc && g_pawnOff > 0 && start && end) {
        void* pawn = *(void**)((BYTE*)g_pc + g_pawnOff);
        float dx = end->X - start->X, dy = end->Y - start->Y, dz = end->Z - start->Z;
        float len = sqrtf(dx * dx + dy * dy + dz * dz);
        float ox = start->X - g_lastGameLoc.X, oy = start->Y - g_lastGameLoc.Y, oz = start->Z - g_lastGameLoc.Z;
        float fromEye = sqrtf(ox * ox + oy * oy + oz * oz);
        static int logs;
        if (source == pawn && len > 1500.f && fromEye < 200.f) {
            FVector ns = g_gunStart;
            FVector ne = { ns.X + g_gunDir.X * len, ns.Y + g_gunDir.Y * len, ns.Z + g_gunDir.Z * len };
            if (logs++ < 12) Log("fire trace re-aimed: start was %.0f units from the eye, length %.0f, flags %lx", fromEye, len, flags);
            return oSingleLineCheck(level, edx, hit, source, &ne, &ns, flags, extent);
        }
        if (source == pawn && logs < 12 && len > 300.f) { ++logs; Log("pawn trace while firing (not re-aimed): length %.0f, %.0f from eye", len, fromEye); }
    }
    return oSingleLineCheck(level, edx, hit, source, end, start, flags, extent);
}

// AActor* ULevel::SpawnActor(UClass*, FName, const FVector& Loc, const FRotator& Rot, AActor* Template,
//                            UBOOL bNoCollisionFail, UBOOL bRemoteOwned, AActor* Owner, APawn* Instigator)
typedef void* (__fastcall* SpawnActor_t)(void* level, void* edx, void* cls, int name, const FVector* loc, const FRotator* rot,
                                         void* tmpl, int noFail, int remote, void* owner, void* instigator);
static SpawnActor_t oSpawnActor;
static int g_velOff = -2;
static void* __fastcall hkSpawnActor(void* level, void* edx, void* cls, int name, const FVector* loc, const FRotator* rot,
                                     void* tmpl, int noFail, int remote, void* owner, void* instigator) {
    bool steer = false;
    void* pawn = (g_pc && g_pawnOff > 0) ? *(void**)((BYTE*)g_pc + g_pawnOff) : nullptr;
    static int logs;
    if (g_cfg.gunTraceFire == 1 && g_bulletValid && pawn && loc && rot && (instigator == pawn || owner == pawn) && g_actState[A_FIRE]) {
        float ox = loc->X - g_lastGameLoc.X, oy = loc->Y - g_lastGameLoc.Y, oz = loc->Z - g_lastGameLoc.Z;
        float fromEye = sqrtf(ox * ox + oy * oy + oz * oz);
        if (fromEye < 150.f) steer = true;
        if (logs < 15) { ++logs; Log("spawn while firing: %ls at %.0f from eye, rot (%d,%d) -> %s", cls ? ObjClassName(cls) : L"?", fromEye, rot->Pitch, rot->Yaw, steer ? "steered" : "left alone"); }
    }
    FRotator r2 = steer ? g_bulletRot : (rot ? *rot : FRotator{});
    void* a = oSpawnActor(level, edx, cls, name, loc, steer ? &r2 : rot, tmpl, noFail, remote, owner, instigator);
    if (steer && a) {
        if (g_velOff == -2) {
            auto classClass = (StaticClass_t)GetProcAddress(GetModuleHandleA("Core.dll"), "?StaticClass@UClass@@SAPAV1@XZ");
            void* actorClass = pStaticFindObject(classClass(), (void*)-1, L"Actor", 0);
            g_velOff = PropOffset(actorClass ? FindProp(actorClass, L"Velocity") : nullptr);
            Log("Actor.Velocity @0x%x", g_velOff);
        }
        if (g_velOff > 0) {
            FVector* v = (FVector*)((BYTE*)a + g_velOff);
            float sp = sqrtf(v->X * v->X + v->Y * v->Y + v->Z * v->Z);
            if (sp > 1.f) { v->X = g_bulletDir[0] * sp; v->Y = g_bulletDir[1] * sp; v->Z = g_bulletDir[2] * sp; }
            if (logs < 20) { ++logs; Log("  bullet speed %.0f re-pointed along the laser", sp); }
        }
    }
    return a;
}

// ---------------------------------------------------------------------------
// "Zoomed while firing": find the weapon's zoom state (bZoomed / m_fZoomed), log it while R1 is
// used, and while the trigger is held force it on for the game's logic tick only (restored before
// the frame is drawn, so the normal gun model and camera stay).

struct ZoomFlag { const wchar_t* name; void* prop; void* holderClass; int off; DWORD mask; bool isFloat, isByte; };
static ZoomFlag g_zf[] = { { L"bZoomed" }, { L"m_fZoomed" }, { L"fZoomed" }, { L"bZoom" }, { L"bIsAtAiming" } };
static const int kZf = sizeof g_zf / sizeof g_zf[0];
static bool g_zfReady;

static void InitZoomFlags() {
    static bool tried; if (tried || g_offsetField < 0) return; tried = true;
    HMODULE core = GetModuleHandleA("Core.dll");
    auto propClassFn = (StaticClass_t)GetProcAddress(core, "?StaticClass@UProperty@@SAPAVUClass@@XZ");
    auto boolClassFn = (StaticClass_t)GetProcAddress(core, "?StaticClass@UBoolProperty@@SAPAVUClass@@XZ");
    auto floatClassFn = (StaticClass_t)GetProcAddress(core, "?StaticClass@UFloatProperty@@SAPAVUClass@@XZ");
    auto byteClassFn = (StaticClass_t)GetProcAddress(core, "?StaticClass@UByteProperty@@SAPAVUClass@@XZ");
    for (int i = 0; i < kZf; ++i) {
        ZoomFlag& z = g_zf[i];
        z.prop = pStaticFindObject(propClassFn(), (void*)-1, z.name, 0);
        if (!z.prop) { Log("zoom flag %ls: not found", z.name); continue; }
        z.holderClass = PropOuter(z.prop);
        z.off = PropOffset(z.prop);
        bool isBool = boolClassFn && pIsA(z.prop, nullptr, boolClassFn());
        z.isFloat = floatClassFn && pIsA(z.prop, nullptr, floatClassFn());
        z.isByte = byteClassFn && pIsA(z.prop, nullptr, byteClassFn());
        if (isBool) {
            // UBoolProperty::BitMask follows the UProperty fields; take the first power of two after Offset.
            for (int k = g_offsetField + 4; k < g_offsetField + 0x30; k += 4) {
                DWORD v = *(DWORD*)((BYTE*)z.prop + k);
                if (v && !(v & (v - 1))) { z.mask = v; break; }
            }
        }
        Log("zoom flag %ls: in %ls @0x%x, %s%s", z.name, pGetName ? pGetName(z.holderClass, nullptr) : L"?", z.off,
            isBool ? "bool" : z.isFloat ? "float" : z.isByte ? "byte" : "other", isBool ? "" : "");
        if (isBool) Log("  bit mask 0x%lx", z.mask);
    }
    g_zfReady = true;
}

static void* ZoomHolder(const ZoomFlag& z) {
    if (!z.prop || !g_pc || g_pawnOff <= 0) return nullptr;
    void* pawn = *(void**)((BYTE*)g_pc + g_pawnOff);
    void* weapon = (pawn && g_weaponOff > 0) ? *(void**)((BYTE*)pawn + g_weaponOff) : nullptr;
    void* cands[3] = { weapon, pawn, g_pc };
    for (void* c : cands) if (c && pIsA(c, nullptr, z.holderClass)) return c;
    return nullptr;
}

static DWORD ReadZoom(const ZoomFlag& z, void* o) {
    if (z.mask) return (*(DWORD*)((BYTE*)o + z.off) & z.mask) ? 1 : 0;
    if (z.isByte) return *((BYTE*)o + z.off);
    return *(DWORD*)((BYTE*)o + z.off);
}

static void LogZoomState(const char* when) {
    char line[512]; int n = sprintf_s(line, "zoom state %s:", when);
    for (int i = 0; i < kZf; ++i) {
        void* o = ZoomHolder(g_zf[i]);
        if (!o) continue;
        DWORD v = ReadZoom(g_zf[i], o);
        if (g_zf[i].isFloat) n += sprintf_s(line + n, sizeof line - n, " %ls=%g", g_zf[i].name, *(float*)&v);
        else n += sprintf_s(line + n, sizeof line - n, " %ls=%lu", g_zf[i].name, v);
    }
    Log("%s", line);
}

typedef void (__fastcall* EngineTick_t)(void* self, void* edx, float dt);
static EngineTick_t oEngineTick;
static void __fastcall hkEngineTick(void* self, void* edx, float dt) {
    if (!g_zfReady) InitZoomFlags();
    // Log the zoom state when R1 changes, so the real flag can be identified.
    static bool zoomWas; bool zoom = g_actState[A_ZOOM];
    if (g_zfReady && zoom != zoomWas) { LogZoomState(zoom ? "R1 pressed" : "R1 released"); zoomWas = zoom; }

    struct Saved { void* o; int off; DWORD v; } saved[kZf]; int ns = 0;
    if (g_zfReady && g_cfg.zoomedFire && g_actState[A_FIRE] && !zoom && g_aimActive) {
        for (int i = 0; i < kZf; ++i) {
            const ZoomFlag& z = g_zf[i];
            if (!z.mask && !z.isFloat) continue;          // only flags we understand
            void* o = ZoomHolder(z);
            if (!o) continue;
            DWORD* p = (DWORD*)((BYTE*)o + z.off);
            saved[ns++] = { o, z.off, *p };
            if (z.mask) *p |= z.mask; else *(float*)p = 1.f;
        }
        static int logs; if (logs++ < 5) Log("firing: forced %d zoom flag(s) on for the logic tick", ns);
    }
    oEngineTick(self, edx, dt);
    for (int i = 0; i < ns; ++i) *(DWORD*)((BYTE*)saved[i].o + saved[i].off) = saved[i].v;
}

// ---------------------------------------------------------------------------
// D3D9 hooks (vtable patches)

typedef HRESULT (WINAPI* CreateDevice_t)(IDirect3D9*, UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS*, IDirect3DDevice9**);
typedef HRESULT (WINAPI* Present_t)(IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);
typedef HRESULT (WINAPI* Reset_t)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);
typedef HRESULT (WINAPI* CreateTexture_t)(IDirect3DDevice9*, UINT, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL, IDirect3DTexture9**, HANDLE*);
typedef HRESULT (WINAPI* CreateVolumeTexture_t)(IDirect3DDevice9*, UINT, UINT, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL, IDirect3DVolumeTexture9**, HANDLE*);
typedef HRESULT (WINAPI* CreateCubeTexture_t)(IDirect3DDevice9*, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL, IDirect3DCubeTexture9**, HANDLE*);
typedef HRESULT (WINAPI* CreateVertexBuffer_t)(IDirect3DDevice9*, UINT, DWORD, DWORD, D3DPOOL, IDirect3DVertexBuffer9**, HANDLE*);
typedef HRESULT (WINAPI* CreateIndexBuffer_t)(IDirect3DDevice9*, UINT, DWORD, D3DFORMAT, D3DPOOL, IDirect3DIndexBuffer9**, HANDLE*);

static CreateDevice_t oCreateDevice;
static Present_t oPresent;
static Reset_t oReset;
static CreateTexture_t oCreateTexture;
static CreateVolumeTexture_t oCreateVolumeTexture;
static CreateCubeTexture_t oCreateCubeTexture;
static CreateVertexBuffer_t oCreateVertexBuffer;
static CreateIndexBuffer_t oCreateIndexBuffer;

typedef HRESULT (WINAPI* SetTransform_t)(IDirect3DDevice9*, D3DTRANSFORMSTATETYPE, const D3DMATRIX*);
typedef HRESULT (WINAPI* SetFVF_t)(IDirect3DDevice9*, DWORD);
typedef HRESULT (WINAPI* SetVertexShader_t)(IDirect3DDevice9*, IDirect3DVertexShader9*);
typedef HRESULT (WINAPI* DrawPrimitive_t)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, UINT);
typedef HRESULT (WINAPI* DrawIndexedPrimitive_t)(IDirect3DDevice9*, D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT);
typedef HRESULT (WINAPI* DrawPrimitiveUP_t)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, const void*, UINT);
typedef HRESULT (WINAPI* DrawIndexedPrimitiveUP_t)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, UINT, UINT, const void*, D3DFORMAT, const void*, UINT);
static SetTransform_t oSetTransform;
static SetFVF_t oSetFVF;
static SetVertexShader_t oSetVertexShader;
static DrawPrimitive_t oDrawPrimitive;
static DrawIndexedPrimitive_t oDrawIndexedPrimitive;
static DrawPrimitiveUP_t oDrawPrimitiveUP;
static DrawIndexedPrimitiveUP_t oDrawIndexedPrimitiveUP;

static bool StereoEye() { return g_eye == 0 || g_eye == 1; }

static bool TargetIsBackbuffer(IDirect3DDevice9* d) {
    IDirect3DSurface9 *rt = nullptr, *bb = nullptr;
    bool same = false;
    if (SUCCEEDED(d->GetRenderTarget(0, &rt)) && SUCCEEDED(d->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb))) same = rt == bb;
    if (rt) rt->Release();
    if (bb) bb->Release();
    return same;
}

static HRESULT WINAPI hkSetTransform(IDirect3DDevice9* d, D3DTRANSFORMSTATETYPE st, const D3DMATRIX* m) {
    if (st != D3DTS_PROJECTION || !m || !StereoEye() || !TargetIsBackbuffer(d)) return oSetTransform(d, st, m);
    D3DMATRIX p = *m;
    if (m->_34 != 0.f) {
        if (!g_cfg.exactProjection) return oSetTransform(d, st, m);
        // Perspective: swap in this eye's exact asymmetric frustum, keep the game's depth range.
        int e = g_eye;
        float cx = (g_rawR[e] + g_rawL[e]) * 0.5f, sx = (g_rawR[e] - g_rawL[e]) * 0.5f;
        float up = -g_rawT[e], down = g_rawB[e];
        float cy = (up - down) * 0.5f, sy = (up + down) * 0.5f;
        float signX = m->_11 >= 0.f ? 1.f : -1.f, signY = m->_22 >= 0.f ? 1.f : -1.f;
        if (!g_stats.persp) { g_stats.gameA = m->_11; g_stats.gameB = m->_22; }
        p._11 = signX / sx; p._21 = 0; p._31 = -cx * m->_34 / sx; p._41 = 0;
        p._12 = 0; p._22 = signY / sy; p._32 = -cy * m->_34 / sy; p._42 = 0;
        g_stats.persp++;
        g_exactUsed[e] = true;
    } else if (g_afterScene && g_cfg.hudScale > 0.f && g_cfg.hudScale < 1.f) {
        // Orthographic after the world: HUD / captions / crosshair. Shrink toward the centre.
        float k = g_cfg.hudScale;
        p._11 *= k; p._21 *= k; p._31 *= k; p._41 *= k;
        p._12 *= k; p._22 *= k; p._32 *= k; p._42 *= k;
        g_stats.ortho++;
    }
    return oSetTransform(d, st, &p);
}

typedef HRESULT (WINAPI* SetVSConstF_t)(IDirect3DDevice9*, UINT, const float*, UINT);
static SetVSConstF_t oSetVSConstF;
static int g_vsLogBudget = 80;
typedef HRESULT (WINAPI* SetRenderTarget_t)(IDirect3DDevice9*, DWORD, IDirect3DSurface9*);
static SetRenderTarget_t oSetRenderTarget;
static HRESULT WINAPI hkSetRenderTarget(IDirect3DDevice9* d, DWORD i, IDirect3DSurface9* rt) {
    if (i == 0) g_rtIsBB = rt == g_bbPtr;
    return oSetRenderTarget(d, i, rt);
}

// BiA's shaders take the world-view-projection matrix as rows in c8..c11 (x, y, z, w).
static HRESULT WINAPI hkSetVSConstF(IDirect3DDevice9* d, UINT reg, const float* c, UINT n) {
    if (!StereoEye() || !g_rtIsBB || reg > 8 || reg + n < 12 || n > 64) return oSetVSConstF(d, reg, c, n);
    float buf[64 * 4];
    memcpy(buf, c, n * 16);
    float* rx = buf + (8 - reg) * 4; float* ry = rx + 4; float* rw = rx + 12;
    bool persp = fabsf(rw[0]) + fabsf(rw[1]) + fabsf(rw[2]) > 1e-5f;
    if (persp && g_cfg.exactProjection) {
        int e = g_eye;
        float cx = (g_rawR[e] + g_rawL[e]) * 0.5f, sx = (g_rawR[e] - g_rawL[e]) * 0.5f;
        float up = -g_rawT[e], down = g_rawB[e];
        float cy = (up - down) * 0.5f, sy = (up + down) * 0.5f;
        float ax = g_tanX / sx, bx = cx / sx, ay = g_tanY / sy, by = cy / sy;
        for (int i = 0; i < 4; ++i) { rx[i] = rx[i] * ax - rw[i] * bx; ry[i] = ry[i] * ay - rw[i] * by; }
        g_exactUsed[e] = true;
        g_stats.persp++;
    } else if (!persp && g_afterScene && g_cfg.hudScale > 0.f && g_cfg.hudScale < 1.f) {
        for (int i = 0; i < 4; ++i) { rx[i] *= g_cfg.hudScale; ry[i] *= g_cfg.hudScale; }
        g_stats.ortho++;
    } else return oSetVSConstF(d, reg, c, n);
    return oSetVSConstF(d, reg, buf, n);
}

static HRESULT WINAPI hkSetFVF(IDirect3DDevice9* d, DWORD fvf) { g_fvf = fvf; return oSetFVF(d, fvf); }
static HRESULT WINAPI hkSetVertexShader(IDirect3DDevice9* d, IDirect3DVertexShader9* vs) { g_vs = vs; return oSetVertexShader(d, vs); }
static void CountDraw() {
    if (!StereoEye() || g_statFrames >= 3 && g_framesRendered % 3000 != 2999) return;
    g_stats.draws++;
    if (g_vs) g_stats.shaderDraws++;
    else if ((g_fvf & D3DFVF_POSITION_MASK) == D3DFVF_XYZRHW && g_afterScene) g_stats.rhwDraws++;
}
static HRESULT WINAPI hkDrawPrimitive(IDirect3DDevice9* d, D3DPRIMITIVETYPE t, UINT a, UINT b) { CountDraw(); return oDrawPrimitive(d, t, a, b); }
static HRESULT WINAPI hkDrawIndexedPrimitive(IDirect3DDevice9* d, D3DPRIMITIVETYPE t, INT a, UINT b, UINT c, UINT e, UINT f) { CountDraw(); return oDrawIndexedPrimitive(d, t, a, b, c, e, f); }
static HRESULT WINAPI hkDrawPrimitiveUP(IDirect3DDevice9* d, D3DPRIMITIVETYPE t, UINT n, const void* v, UINT st) { CountDraw(); return oDrawPrimitiveUP(d, t, n, v, st); }
static HRESULT WINAPI hkDrawIndexedPrimitiveUP(IDirect3DDevice9* d, D3DPRIMITIVETYPE t, UINT a, UINT b, UINT c, const void* i, D3DFORMAT f, const void* v, UINT st) { CountDraw(); return oDrawIndexedPrimitiveUP(d, t, a, b, c, i, f, v, st); }

static void FixPool(D3DPOOL& pool, DWORD& usage) {
    if (pool == D3DPOOL_MANAGED) { pool = D3DPOOL_DEFAULT; usage |= D3DUSAGE_DYNAMIC; }
}

static HRESULT WINAPI hkCreateTexture(IDirect3DDevice9* d, UINT w, UINT h, UINT lv, DWORD usage, D3DFORMAT f, D3DPOOL pool, IDirect3DTexture9** t, HANDLE* sh) {
    D3DPOOL p0 = pool; FixPool(pool, usage);
    HRESULT hr = oCreateTexture(d, w, h, lv, usage, f, pool, t, sh);
    if (FAILED(hr) && p0 == D3DPOOL_MANAGED) {
        hr = oCreateTexture(d, w, h, lv, usage & ~D3DUSAGE_AUTOGENMIPMAP, f, pool, t, sh);
        if (FAILED(hr)) Log("CreateTexture %ux%u fmt %d failed %08lx", w, h, f, hr);
    }
    return hr;
}
static HRESULT WINAPI hkCreateVolumeTexture(IDirect3DDevice9* d, UINT w, UINT h, UINT dp, UINT lv, DWORD usage, D3DFORMAT f, D3DPOOL pool, IDirect3DVolumeTexture9** t, HANDLE* sh) {
    FixPool(pool, usage);
    HRESULT hr = oCreateVolumeTexture(d, w, h, dp, lv, usage, f, pool, t, sh);
    if (FAILED(hr)) Log("CreateVolumeTexture failed %08lx", hr);
    return hr;
}
static HRESULT WINAPI hkCreateCubeTexture(IDirect3DDevice9* d, UINT e, UINT lv, DWORD usage, D3DFORMAT f, D3DPOOL pool, IDirect3DCubeTexture9** t, HANDLE* sh) {
    FixPool(pool, usage);
    HRESULT hr = oCreateCubeTexture(d, e, lv, usage, f, pool, t, sh);
    if (FAILED(hr)) Log("CreateCubeTexture failed %08lx", hr);
    return hr;
}
static HRESULT WINAPI hkCreateVertexBuffer(IDirect3DDevice9* d, UINT len, DWORD usage, DWORD fvf, D3DPOOL pool, IDirect3DVertexBuffer9** vb, HANDLE* sh) {
    FixPool(pool, usage);
    HRESULT hr = oCreateVertexBuffer(d, len, usage, fvf, pool, vb, sh);
    if (FAILED(hr)) Log("CreateVertexBuffer failed %08lx", hr);
    return hr;
}
static HRESULT WINAPI hkCreateIndexBuffer(IDirect3DDevice9* d, UINT len, DWORD usage, D3DFORMAT f, D3DPOOL pool, IDirect3DIndexBuffer9** ib, HANDLE* sh) {
    FixPool(pool, usage);
    HRESULT hr = oCreateIndexBuffer(d, len, usage, f, pool, ib, sh);
    if (FAILED(hr)) Log("CreateIndexBuffer failed %08lx", hr);
    return hr;
}

static void ShowMenuFrame();
static HRESULT WINAPI hkPresent(IDirect3DDevice9* d, const RECT* a, const RECT* b, HWND w, const RGNDATA* r) {
    if (!g_inDraw && g_eye == -1 && g_vrOk && g_dev) {
        // Movies and loading screens present without going through the engine's Draw.
        vr::VRCompositor()->WaitGetPoses(g_poses, vr::k_unMaxTrackedDeviceCount, nullptr, 0);
        g_hmdValid = g_poses[vr::k_unTrackedDeviceIndex_Hmd].bPoseIsValid;
        if (g_hmdValid) g_hmd = g_poses[vr::k_unTrackedDeviceIndex_Hmd].mDeviceToAbsoluteTracking;
        CaptureEye(0);
        g_menuMode = true;
        ShowMenuFrame();
        static int logs; if (logs++ < 3) Log("frame presented outside Draw (movie/loading) -> floating screen");
    }
    if (g_eye == 0) return D3D_OK;                 // left eye never goes to the monitor
    if (g_eye == 1 && !g_capturedThisEye) CaptureEye(1);
    if (g_eye == 2 && !g_capturedThisEye) CaptureEye(0);
    return oPresent(d, a, b, w, r);
}

static void ForcePresentParams(D3DPRESENT_PARAMETERS* pp) {
    if (!g_vrOk || !pp) return;
    pp->PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;   // SteamVR paces the frame
}

static HRESULT WINAPI hkReset(IDirect3DDevice9* d, D3DPRESENT_PARAMETERS* pp) {
    ForcePresentParams(pp);
    HRESULT hr = oReset(d, pp);
    Log("Reset %ux%u windowed=%d -> %08lx", pp ? pp->BackBufferWidth : 0, pp ? pp->BackBufferHeight : 0, pp ? pp->Windowed : -1, hr);
    return hr;
}

static void PatchVtable(void* obj, int index, void* hook, void** orig) {
    void** vt = *(void***)obj;
    if (vt[index] == hook) return;
    DWORD old; VirtualProtect(&vt[index], sizeof(void*), PAGE_EXECUTE_READWRITE, &old);
    *orig = vt[index]; vt[index] = hook;
    VirtualProtect(&vt[index], sizeof(void*), old, &old);
}

static void InstallEngineHooks();

static HRESULT WINAPI hkCreateDevice(IDirect3D9* self, UINT adapter, D3DDEVTYPE type, HWND focus, DWORD flags,
                                     D3DPRESENT_PARAMETERS* pp, IDirect3DDevice9** out) {
    ForcePresentParams(pp);
    HRESULT hr = oCreateDevice(self, adapter, type, focus, flags, pp, out);
    Log("CreateDevice %ux%u windowed=%d fmt=%d msaa=%d flags=%08lx -> %08lx", pp->BackBufferWidth, pp->BackBufferHeight,
        pp->Windowed, pp->BackBufferFormat, pp->MultiSampleType, flags, hr);
    if (FAILED(hr)) return hr;
    IDirect3DDevice9* dev = *out;
    PatchVtable(dev, 16, (void*)hkReset, (void**)&oReset);
    PatchVtable(dev, 17, (void*)hkPresent, (void**)&oPresent);
    PatchVtable(dev, 23, (void*)hkCreateTexture, (void**)&oCreateTexture);
    PatchVtable(dev, 24, (void*)hkCreateVolumeTexture, (void**)&oCreateVolumeTexture);
    PatchVtable(dev, 25, (void*)hkCreateCubeTexture, (void**)&oCreateCubeTexture);
    PatchVtable(dev, 26, (void*)hkCreateVertexBuffer, (void**)&oCreateVertexBuffer);
    PatchVtable(dev, 27, (void*)hkCreateIndexBuffer, (void**)&oCreateIndexBuffer);
    PatchVtable(dev, 44, (void*)hkSetTransform, (void**)&oSetTransform);
    PatchVtable(dev, 81, (void*)hkDrawPrimitive, (void**)&oDrawPrimitive);
    PatchVtable(dev, 82, (void*)hkDrawIndexedPrimitive, (void**)&oDrawIndexedPrimitive);
    PatchVtable(dev, 83, (void*)hkDrawPrimitiveUP, (void**)&oDrawPrimitiveUP);
    PatchVtable(dev, 84, (void*)hkDrawIndexedPrimitiveUP, (void**)&oDrawIndexedPrimitiveUP);
    PatchVtable(dev, 89, (void*)hkSetFVF, (void**)&oSetFVF);
    PatchVtable(dev, 92, (void*)hkSetVertexShader, (void**)&oSetVertexShader);
    PatchVtable(dev, 94, (void*)hkSetVSConstF, (void**)&oSetVSConstF);
    PatchVtable(dev, 37, (void*)hkSetRenderTarget, (void**)&oSetRenderTarget);
    g_dev = dev;
    for (int e = 0; e < 2; ++e) { if (g_eyeTex9[e]) { g_eyeTex9[e]->Release(); g_eyeTex9[e] = nullptr; } }
    if (g_query) { g_query->Release(); g_query = nullptr; }
    g_eyeW = g_eyeH = 0;
    if (g_vrOk && !g_d11) {
        HRESULT h = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &g_d11, nullptr, nullptr);
        Log("D3D11 device %08lx", h);
    }
    return hr;
}

// ---------------------------------------------------------------------------
// Real d3d9 + exports

static HMODULE g_real;
typedef HRESULT (WINAPI* Create9Ex_t)(UINT, IDirect3D9Ex**);
typedef IDirect3D9* (WINAPI* Create9_t)(UINT);

static bool LoadReal() {
    if (g_real) return true;
    char path[MAX_PATH]; GetSystemDirectoryA(path, MAX_PATH); strcat_s(path, "\\d3d9.dll");
    g_real = LoadLibraryA(path);
    if (!g_real) Log("could not load %s", path);
    return g_real != nullptr;
}

static bool g_engineHooked;
static void InstallEngineHooks() {
    if (g_engineHooked) return;
    g_engineHooked = true;
    HMODULE eng = GetModuleHandleA("Engine.dll");
    if (!eng) { Log("Engine.dll not loaded"); return; }
    void* draw = GetProcAddress(eng, "?Draw@UGameEngine@@UAEXPAVUViewport@@HPAEPAH@Z");
    void* psn  = GetProcAddress(eng, "??0FPlayerSceneNode@@QAE@PAVUViewport@@PAVFRenderTarget@@PAVAActor@@VFVector@@VFRotator@@M@Z");
    void* psr  = GetProcAddress(eng, "?Render@FPlayerSceneNode@@UAEXPAVFRenderInterface@@@Z");
    void* csn  = GetProcAddress(eng, "??0FCameraSceneNode@@QAE@PAVUViewport@@PAVFRenderTarget@@PAVAActor@@VFVector@@VFRotator@@MMM@Z");
    void* cda  = GetProcAddress(eng, "?DrawActor@UCanvas@@QAEXPAVAActor@@M@Z");
    Log("Engine.dll %p  Draw %p  FPlayerSceneNode %p", eng, draw, psn);
    if (!draw || !psn) return;
    MH_Initialize();
    MH_STATUS a = MH_CreateHook(draw, (void*)hkDraw, (void**)&oDraw);
    MH_STATUS b = MH_CreateHook(psn, (void*)hkPlayerSceneNode, (void**)&oPlayerSceneNode);
    MH_STATUS r = psr ? MH_CreateHook(psr, (void*)hkSceneRender, (void**)&oSceneRender) : MH_ERROR_FUNCTION_NOT_FOUND;
    Log("scene render hook %d", r);
    MH_STATUS c1 = csn ? MH_CreateHook(csn, (void*)hkCameraSceneNode, (void**)&oCameraSceneNode) : MH_ERROR_FUNCTION_NOT_FOUND;
    MH_STATUS c2 = cda ? MH_CreateHook(cda, (void*)hkCanvasDrawActor, (void**)&oCanvasDrawActor) : MH_ERROR_FUNCTION_NOT_FOUND;
    void* slc = GetProcAddress(eng, "?SingleLineCheck@ULevel@@QAEHAAUFCheckResult@@PAVAActor@@ABVFVector@@2K2@Z");
    MH_STATUS c3 = slc ? MH_CreateHook(slc, (void*)hkSingleLineCheck, (void**)&oSingleLineCheck) : MH_ERROR_FUNCTION_NOT_FOUND;
    Log("fire trace hook %d", c3);
    void* spa = GetProcAddress(eng, "?SpawnActor@ULevel@@QAEPAVAActor@@PAVUClass@@VFName@@ABVFVector@@ABVFRotator@@PAV2@HH4PAVAPawn@@@Z");
    MH_STATUS c4 = spa ? MH_CreateHook(spa, (void*)hkSpawnActor, (void**)&oSpawnActor) : MH_ERROR_FUNCTION_NOT_FOUND;
    Log("spawn hook %d", c4);
    void* etk = GetProcAddress(eng, "?Tick@UGameEngine@@UAEXM@Z");
    MH_STATUS c5 = etk ? MH_CreateHook(etk, (void*)hkEngineTick, (void**)&oEngineTick) : MH_ERROR_FUNCTION_NOT_FOUND;
    Log("engine tick hook %d", c5);
    Log("gun hooks: camera node %d, canvas draw actor %d", c1, c2);
    MH_STATUS c = MH_EnableHook(MH_ALL_HOOKS);
    Log("hooks: draw=%d scenenode=%d enable=%d", a, b, c);
}

// ---------------------------------------------------------------------------
// Crash diagnostics: log who triggered "R6025 pure virtual function call".

// Nearest exported symbol at or below addr inside module m.
static const char* NearestExport(HMODULE m, DWORD_PTR addr, DWORD* disp) {
    BYTE* base = (BYTE*)m;
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)base;
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    IMAGE_DATA_DIRECTORY dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (!dir.VirtualAddress) return nullptr;
    IMAGE_EXPORT_DIRECTORY* ex = (IMAGE_EXPORT_DIRECTORY*)(base + dir.VirtualAddress);
    DWORD* names = (DWORD*)(base + ex->AddressOfNames);
    WORD* ords = (WORD*)(base + ex->AddressOfNameOrdinals);
    DWORD* funcs = (DWORD*)(base + ex->AddressOfFunctions);
    DWORD rva = (DWORD)(addr - (DWORD_PTR)base), best = 0; const char* bestName = nullptr;
    for (DWORD i = 0; i < ex->NumberOfNames; ++i) {
        DWORD f = funcs[ords[i]];
        if (f <= rva && f > best) { best = f; bestName = (const char*)(base + names[i]); }
    }
    *disp = rva - best;
    return bestName;
}

static void LogAddress(const char* tag, DWORD_PTR a) {
    HMODULE m = nullptr;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)a, &m) || !m) return;
    char path[MAX_PATH]; GetModuleFileNameA(m, path, MAX_PATH);
    const char* name = strrchr(path, '\\'); name = name ? name + 1 : path;
    DWORD disp = 0; const char* sym = NearestExport(m, a, &disp);
    Log("  %s %08lx %s+0x%lx  %s+0x%lx", tag, (unsigned long)a, name, (unsigned long)(a - (DWORD_PTR)m), sym ? sym : "?", disp);
}

typedef int (__cdecl* purecall_t)();
static purecall_t oPurecall[3];
static void PurecallReport() {
    Log("R6025 pure virtual function call - call stack:");
    void* frames[32]; USHORT n = CaptureStackBackTrace(1, 32, frames, nullptr);
    for (USHORT i = 0; i < n; ++i) LogAddress("frame", (DWORD_PTR)frames[i]);
    // Frame pointers are often missing in 2005 code, so also scan the raw stack for code addresses.
    DWORD_PTR* sp = (DWORD_PTR*)_AddressOfReturnAddress();
    int shown = 0;
    for (int i = 0; i < 400 && shown < 40; ++i) {
        DWORD_PTR v = sp[i];
        MEMORY_BASIC_INFORMATION mbi;
        if (v < 0x10000 || !VirtualQuery((void*)v, &mbi, sizeof mbi)) continue;
        if (!(mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE))) continue;
        LogAddress("stack", v); ++shown;
    }
}
static int __cdecl hkPurecall0() { PurecallReport(); return oPurecall[0](); }
static int __cdecl hkPurecall1() { PurecallReport(); return oPurecall[1](); }
static int __cdecl hkPurecall2() { PurecallReport(); return oPurecall[2](); }

// The engine's own error paths: appThrowf(fmt, ...) and appFailAssert(expr, file, line).
typedef void (__cdecl* appThrowf_t)(const wchar_t* fmt, ...);
typedef void (__cdecl* appFailAssert_t)(const char* expr, const char* file, int line);
static appThrowf_t oAppThrowf;
static appFailAssert_t oAppFailAssert;
static void __cdecl hkAppThrowf(const wchar_t* fmt, ...) {
    wchar_t buf[1024];
    va_list a; va_start(a, fmt); _vsnwprintf_s(buf, _countof(buf), _TRUNCATE, fmt, a); va_end(a);
    Log("game error (appThrowf): %ls", buf);
    va_start(a, fmt);
    // Re-throw through the original with the already formatted text.
    va_end(a);
    oAppThrowf(L"%s", buf);
}
static void __cdecl hkAppFailAssert(const char* expr, const char* file, int line) {
    Log("game assertion failed: %s (%s:%d)", expr ? expr : "?", file ? file : "?", line);
    oAppFailAssert(expr, file, line);
}

static void InstallPurecallHooks() {
    const char* dlls[3] = { "MSVCR71.dll", "MSVCR70.dll", "msvcrt.dll" };
    void* hooks[3] = { (void*)hkPurecall0, (void*)hkPurecall1, (void*)hkPurecall2 };
    MH_Initialize();
    for (int i = 0; i < 3; ++i) {
        HMODULE m = GetModuleHandleA(dlls[i]);
        void* f = m ? GetProcAddress(m, "_purecall") : nullptr;
        if (!f) continue;
        MH_STATUS st = MH_CreateHook(f, hooks[i], (void**)&oPurecall[i]);
        if (st == MH_OK) MH_EnableHook(f);
        Log("purecall hook %s: %d", dlls[i], st);
    }
    HMODULE core = GetModuleHandleA("Core.dll");
    void* t = core ? GetProcAddress(core, "?appThrowf@@YAXPBGZZ") : nullptr;
    void* f = core ? GetProcAddress(core, "?appFailAssert@@YAXPBD0H@Z") : nullptr;
    if (t && MH_CreateHook(t, (void*)hkAppThrowf, (void**)&oAppThrowf) == MH_OK) MH_EnableHook(t);
    if (f && MH_CreateHook(f, (void*)hkAppFailAssert, (void**)&oAppFailAssert) == MH_OK) MH_EnableHook(f);
    Log("error hooks: appThrowf %p appFailAssert %p", t, f);
}

static void Startup() {
    static bool done; if (done) return; done = true;
    char dir[MAX_PATH]; GetModuleFileNameA(nullptr, dir, MAX_PATH);
    char* slash = strrchr(dir, '\\'); if (slash) slash[1] = 0;
    char logPath[MAX_PATH]; sprintf_s(logPath, "%sBiAVR.log", dir);
    sprintf_s(g_iniPath, "%sBiAVR.ini", dir);
    g_log = _fsopen(logPath, "w", _SH_DENYWR);
    LoadConfig();
    Log("BiA-VR starting (enabled=%d worldScale=%.1f)", g_cfg.enabled, g_cfg.worldScale);
    InstallPurecallHooks();
    g_autoScale = g_cfg.worldScale <= 0.f;
    if (g_autoScale) g_cfg.worldScale = 52.5f;
    if (g_cfg.enabled) { InitVR(); if (g_vrOk) InstallEngineHooks(); }
}

extern "C" IDirect3D9* WINAPI Direct3DCreate9(UINT sdk) {
    Startup();
    if (!LoadReal()) return nullptr;
    auto create9ex = (Create9Ex_t)GetProcAddress(g_real, "Direct3DCreate9Ex");
    IDirect3D9Ex* ex = nullptr;
    if (create9ex && SUCCEEDED(create9ex(sdk, &ex)) && ex) {
        PatchVtable(ex, 16, (void*)hkCreateDevice, (void**)&oCreateDevice);
        Log("Direct3DCreate9 -> Direct3D9Ex");
        return ex;
    }
    Log("Direct3DCreate9Ex unavailable, falling back to plain D3D9 (no VR output)");
    g_vrOk = false;
    return ((Create9_t)GetProcAddress(g_real, "Direct3DCreate9"))(sdk);
}

extern "C" HRESULT WINAPI Direct3DCreate9Ex(UINT sdk, IDirect3D9Ex** out) {
    Startup();
    if (!LoadReal()) return E_FAIL;
    return ((Create9Ex_t)GetProcAddress(g_real, "Direct3DCreate9Ex"))(sdk, out);
}

BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_DETACH && g_log) { fclose(g_log); g_log = nullptr; }
    return TRUE;
}
