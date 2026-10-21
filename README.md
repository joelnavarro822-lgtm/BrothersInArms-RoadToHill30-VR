# BiA-VR — Brothers in Arms: Road to Hill 30 in VR (beta)

A free fan-made VR mod for **Brothers in Arms: Road to Hill 30** (Steam). Play the whole campaign in a
SteamVR headset with motion controllers: real 3D, head tracking, aiming with your hand, a gun that
sits in your hand, holster weapon switching and menus on a floating screen.

> **Beta.** It works and is fun, but some things are still rough — see *Known issues*.

## Requirements
- Brothers in Arms: Road to Hill 30 on **Steam** (Windows)
- A **SteamVR** headset with two motion controllers (developed on PSVR2; Quest / Index / Vive should work)
- **Keyboard and mouse plugged in** when you start the game (the 2005 engine crashes without them)

## Install
**Easy way:** download the latest `BiA-VR-vX.Y.zip` from **Releases**, unzip it, and double-click **`Install.bat`**.
It finds the game for you.

**By hand:** drag the `System` folder from the zip into the game folder
(`Steam\steamapps\common\Brothers in Arms Road to Hill 30`) and choose to merge/replace.

Then: start **SteamVR**, launch the game from Steam, and put the headset on.

**Uninstall:** double-click `Uninstall.bat`, or delete `d3d9.dll`, `openvr_api.dll` and `BiAVR.ini`
from the game's `System` folder. No game files are changed.

## Controls (PSVR2 names; other controllers use the matching buttons)
| Right controller | | Left controller | |
|---|---|---|---|
| Trigger (R2) | Fire | Stick | Walk / strafe |
| R1 | Aim down sights (most accurate) | □ / X-equivalent | Jump / continue prompts |
| ✕ / A | Use / reload / continue prompts | △ / Y-equivalent | Switch weapon |
| ○ / B | Crouch | L2 | Squad command |
| Stick left/right | Snap turn | Stick click | Pause menu |
| Stick click | Melee | L1 tap | V (situational awareness) |
| | | L1 hold + right stick | Move the gun closer/further (saved per weapon) |

- **Two-handed rifle:** put your left hand out in front of your right hand — it locks on and steers the barrel.
- **Aiming:** hip fire goes where the gun points; hold **R1** to aim down sights for long-range accuracy.
- **Holsters:** reach down to your hip or up over your shoulder and press **R1** to switch weapons.
- **Menus:** point the right controller at the floating screen and pull the trigger.
- **Sights:** hold **R1** for about a second once per weapon — the mod learns that gun's sight alignment.
- **Keyboard:** F6 aiming laser on/off (off by default) · F7 controller aim on/off · F8 recenter view.
- A short tip about R1 and F6 appears in the first chapter.

## Settings
`System\BiAVR.ini` is created on first launch with every option explained. Delete it to reset.

## Known issues
- Hip fire is less accurate at long range than aiming down sights (R1). R1 slows you down and aims along your view - that's how the game's sights work.
- Captions and some HUD elements sit low, near the bottom edge of your view.
- Cutscenes follow your head instead of playing on a fixed screen.
- The left hand can look rotated on some controllers — try `LeftHandRollDegrees` = 0 / 90 / -90 / 180.
- Antivirus tools sometimes flag `d3d9.dll` (it hooks the game's DirectX); this is a common false alarm for mods of this kind.
- If the game shows a "pure virtual function call" error at startup, plug in a keyboard and mouse.

## Troubleshooting
The mod writes `System\BiAVR.log`. If something breaks, include that file when you report it.

## How it works (for modders)
A proxy `d3d9.dll` in the game's `System` folder:
- upgrades the game to D3D9Ex (managed resources become DEFAULT|DYNAMIC) so frames can be shared with SteamVR;
- hooks `UGameEngine::Draw` and renders every frame twice (one per eye), moving the camera via the
  `FPlayerSceneNode` / `FCameraSceneNode` constructors;
- uses Unreal's own reflection (Core.dll exports) to find `PlayerController.Rotation`, the weapon's
  fire modes, `GUIController.MouseX/Y` and more;
- places the first-person weapon in the hand through `UCanvas::DrawActor`, bends the left arm with
  `USkeletalMeshInstance::SetBoneDirection`, and traces the gun's sight line with `ULevel::SingleLineCheck`.

### Build
Visual Studio 2022 (C++ x86 tools). Put the [OpenVR SDK](https://github.com/ValveSoftware/openvr) in
`third_party/openvr-master` and [MinHook](https://github.com/TsudaKageyu/minhook) in
`third_party/minhook-master`, then run `build.bat`. Output: `out\d3d9.dll`.

## Credits & licences
- OpenVR © Valve Corporation (BSD-3) — `LICENSES/OpenVR-LICENSE.txt`
- MinHook © Tsuda Kageyu (BSD-2) — `LICENSES/MinHook-LICENSE.txt`
- Two-handed grip behaviour inspired by the F.E.A.R. VR mod.

Not affiliated with Gearbox Software or Ubisoft. You need to own the game.
