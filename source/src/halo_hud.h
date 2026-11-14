#ifndef HALO_HUD_H
#define HALO_HUD_H

// The retail HUD drawn from packages/halo/hud.txt (tools/halo_hud.py):
// weapon reticles, ammo meters and counters, the unit shield, health and
// motion sensor panels and the grenade panel, in the original 640x480 HUD
// space anchored to the screen corners.

// Values the weapon HUD shows, filled in by halo.cpp each frame.
struct HaloHudWeapon
{
    int weapon = -1;                       // packages/halo/weapons.txt index
    int loaded = 0, reserve = 0, magazine = 0, maximum = 0;
    float heat = 0, battery = 1;
    bool battery_weapon = false, overheated = false, reloading = false, charging = false, firing = false;
    int zoom = 0;                          // 0 unzoomed, else the zoom level
    bool locked = false;                   // aim assist has a target: the reticle turns red
};

struct HaloHudBlip { float x, y; bool ally; };   // offsets from the player in Halo world units, rotated to view

struct HaloHudUnit
{
    float shield = 1, health = 1;          // fractions
    bool shieldHit = false;
    int grenades = 0, grenadeType = 0;
    bool flashlight = false;
    float flashlightCharge = 1;
};

bool halohudload();
bool halohudready();
// Draws everything; call inside the 2D HUD pass (glOrtho over VIRTW x VIRTH).
void halohuddraw(const HaloHudWeapon &weapon, const HaloHudUnit &unit, const HaloHudBlip *blips, int blipCount);

#endif
