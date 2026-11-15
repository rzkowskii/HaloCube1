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

// HUD parts the mission scripts hide (hud_show_* false) or flash (hud_blink_* true).
enum { HALO_HUD_HEALTH = 1, HALO_HUD_SHIELD = 2, HALO_HUD_MOTION = 4, HALO_HUD_CROSSHAIR = 8 };

struct HaloHudUnit
{
    float shield = 1, health = 1;          // fractions
    float overshield = 0;                  // overshield layers beyond the full shield, 0 to 2
    bool shieldHit = false;
    int grenades = 0, grenadeType = 0;     // the selected type's count and the type (0 frag, 1 plasma)
    bool flashlight = false;
    float flashlightCharge = 1;
    int hidden = 0, blinking = 0;          // HALO_HUD_* bits
};

// A nav point a mission script shows (activate_nav_point_*, activate_team_nav_point_*): a world
// point (AssaultCube units) and the retail waypoint arrow type ("default", "default_red", ...).
struct HaloHudNav { float x, y, z; const char *type; };

bool halohudload();
bool halohudready();
// Draws everything; call inside the 2D HUD pass (glOrtho over VIRTW x VIRTH).
void halohuddraw(const HaloHudWeapon &weapon, const HaloHudUnit &unit, const HaloHudBlip *blips, int blipCount);
// Draws nav points with the retail waypoint arrows (hud globals, `waypoint`/`arrow` lines in
// hud.txt): the on-screen marker over the point, or the off-screen arrow at the screen edge
// pointing towards it. Without those lines, plain diamonds in the arrow type's colour.
// Uses the last 3D frame's view (mvpmatrix); call inside the 2D HUD pass.
void halohuddrawnav(const HaloHudNav *navs, int count);

#endif
