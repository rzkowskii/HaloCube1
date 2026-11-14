#ifndef HALO_WEAPONS_H
#define HALO_WEAPONS_H

// Retail weapon definitions read from packages/halo/weapons.txt, which
// tools/halo_weapons.py writes from the campaign weap, proj and jpt! tags.
// Speeds and ranges are in Halo world units; times are seconds, angles radians.

#include <string>
#include <vector>

enum HaloWeaponClass { HALO_WC_ASSAULT, HALO_WC_PISTOL, HALO_WC_SHOTGUN, HALO_WC_SNIPER, HALO_WC_PROJECTILE,
                       HALO_WC_VEHICLE, HALO_WC_ACTOR };

struct HaloProjectileDef
{
    bool valid = false;
    float speed = 0, finalSpeed = 0, range = 0, gravity = 0, guided = 0;
    float damageLower = 0, damageUpper = 0, radius = 0;
    int category = -1;
    std::string path;
};

struct HaloTriggerDef
{
    float rate = 0, finalRate = 0, rateAccel = 0, rateDecel = 0;
    int roundsPerShot = 0, projectilesPerShot = 1;
    float distribution = 0;
    float error = 0, finalError = 0, errorAccel = 0, errorDecel = 0, errorMin = 0, errorMax = 0;
    float charge = 0, heat = 0, age = 0;
    int magazine = 0;
    HaloProjectileDef projectile;
};

struct HaloMagazineDef
{
    int loaded = 0, initial = 0, maximum = 0, reloaded = 0, recharge = 0;
    float reload = 0;
};

struct HaloWeaponDef
{
    int index = -1;
    std::string path, label, name, viewmodel, worldmodel;
    HaloWeaponClass kind = HALO_WC_ACTOR;
    float ready = 0, heatRecover = 0, heatOverheat = 0, heatLoss = 0;
    int zoomLevels = 0;
    float zoomMin = 1, zoomMax = 1;
    float autoaimAngle = 0, autoaimRange = 0, magnetismAngle = 0, magnetismRange = 0;
    float meleeLower = 0, meleeUpper = 0;
    std::vector<HaloMagazineDef> magazines;
    std::vector<HaloTriggerDef> triggers;

    bool playable() const { return kind <= HALO_WC_PROJECTILE && viewmodel != "-"; }
    // Weapons with no magazine run on a battery and build heat instead.
    bool usesBattery() const { return magazines.empty() || magazines[0].loaded <= 0; }
    const HaloMagazineDef *magazine() const { return magazines.empty() ? nullptr : &magazines[0]; }
    const HaloTriggerDef *trigger(int i = 0) const { return i >= 0 && i < int(triggers.size()) ? &triggers[i] : nullptr; }
    // The AssaultCube weapon slot whose firing, animation and sound code it reuses.
    int acgun() const;
    // Magnification for a zoom level (1-based); 1 means no zoom.
    float magnification(int level) const;
};

// Per-owner firing state: spread error and fire rate ramp up while firing and
// recover afterwards, heat dissipates over time, battery drains per shot.
struct HaloWeaponState
{
    int def = -1;
    int loaded = 0, reserve = 0;
    float battery = 1, heat = 0, error = 0, rate = 0;
    bool overheated = false;
    int lastShot = 0, chargeStart = 0;
    bool charged = false;
    int zoom = 0;

    void reset(const HaloWeaponDef *d, bool full = true);
    // Advances heat loss and overheat recovery by elapsed milliseconds.
    void tick(const HaloWeaponDef &d, int elapsed);
    // Updates the error and rate ramps for a shot at `millis`; returns the cone angle.
    float shoot(const HaloWeaponDef &d, const HaloTriggerDef &t, int millis);
    // Milliseconds until the next shot at the current point of the rate ramp.
    int delay(const HaloTriggerDef &t) const;
    bool canfire(const HaloWeaponDef &d) const;
};

bool haloweaponsload();
int haloweaponcount();
const HaloWeaponDef *haloweapon(int index);
const HaloWeaponDef *haloweaponfind(const char *path);
// The definition AssaultCube's gun slot stands for when no tag data applies.
const HaloWeaponDef *haloweaponforgun(int gun);

#endif
