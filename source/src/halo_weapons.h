#ifndef HALO_WEAPONS_H
#define HALO_WEAPONS_H

// Retail weapon definitions read from packages/halo/weapons.txt, which
// tools/halo_weapons.py writes from the campaign weap, proj and jpt! tags.
// Speeds and ranges are in Halo world units; times are seconds, angles radians.

#include <string>
#include <vector>

enum HaloWeaponClass { HALO_WC_ASSAULT, HALO_WC_PISTOL, HALO_WC_SHOTGUN, HALO_WC_SNIPER, HALO_WC_PROJECTILE,
                       HALO_WC_VEHICLE, HALO_WC_ACTOR };

// Sound roles of the `sound <role> <tag>` lines in weapons.txt and equipment.txt.
enum HaloSoundRole { HALO_SOUND_FIRE, HALO_SOUND_CHARGED, HALO_SOUND_EMPTY, HALO_SOUND_CYCLE, HALO_SOUND_RELOAD,
                     HALO_SOUND_RELOAD_EMPTY, HALO_SOUND_MELEE, HALO_SOUND_READY, HALO_SOUND_OVERHEAT, HALO_SOUND_PICKUP,
                     HALO_SOUND_THROW, HALO_SOUND_EXPLODE, HALO_SOUND_ARMED,
                     // The weapon tag's zoom sounds and a looping sound held while firing (the
                     // flamethrower's fire, the plasma pistol's charge hum).
                     HALO_SOUND_ZOOM_IN, HALO_SOUND_ZOOM_OUT, HALO_SOUND_LOOP,
                     // The needles' super detonation (weapons.txt `sound supercombine`).
                     HALO_SOUND_SUPERCOMBINE, HALO_SOUND_ROLES };

// The sample tools/import_halo_audio.py decodes a sound tag to, for haloplaysound():
// "sound\sfx\weapons\needler\fire" is "tags/sfx-weapons-needler-fire".
std::string halosoundsample(const std::string &tag);

struct HaloSounds
{
    std::string samples[HALO_SOUND_ROLES];
    // The role's sample, or null when the tags name none.
    const char *get(int role) const { return role >= 0 && role < HALO_SOUND_ROLES && !samples[role].empty() ? samples[role].c_str() : nullptr; }
    // Parses the rest of a `sound <role> <tag>` line; false for an unknown role.
    bool parse(const std::string &role, const std::string &tag);
};

// A jpt! tag's damage (tools/halo_weapons.py, JPT_* layout): a full-strength hit deals a random
// amount in [upperMin, upperMax]; `lower` is what Halo scales a weakened hit down to. A hit at
// scale s (1 full strength, 0 weakest: the far end of a projectile's damage range, the outer
// edge of a blast) deals lower + (full - lower) * s.
enum { HALO_DAMAGE_MATERIALS = 33 };

struct HaloDamage
{
    float lower = 0, upperMin = 0, upperMax = 0;
    // The jpt! AOE core radius (world units; a `core` line after the damage's line: the frag and
    // plasma grenades' 0.4, the rocket's 0.6). A body within it of a blast's centre takes the blast
    // at full strength and, for a grenade, even when the centre's line of sight to it is blocked
    // (areadamage, halogrenade::explode; halo_aoecore 0 ignores it).
    float core = 0;
    // The jpt! damage modifier per retail material type (the `materials` line after the damage's
    // line in weapons.txt or equipment.txt): 12 hunter armor, 13 hunter skin, 25 sentinel,
    // 26 monitor, 32 hunter shield and so on. Without the line every modifier is 1.
    bool hasMaterials = false;
    float materials[HALO_DAMAGE_MATERIALS] = {};
    float material(int type) const { return hasMaterials && type >= 0 && type < HALO_DAMAGE_MATERIALS ? materials[type] : 1.0f; }
    // A full-strength hit.
    float roll() const;
    // [lower, upperMax]: what actors' grenades roll (the balance grenades had before the
    // jpt! layout was corrected); the player's roll the retail range.
    float rollFromLower() const;
    // A hit of full-strength amount `full` at `scale`.
    float scaled(float full, float scale) const;
};

// Halo's falloff: 1 up to `start`, 0 from `end` on, linear between; 1 when `end` is 0 (no falloff).
float halofalloff(float distance, float start, float end);

// jpt! side effects. EMP (the plasma grenade, the charged plasma bolt): the shield a hit reaches
// is drained after the hit, the overshield's layers too (halodamage), and the vehicles it reaches
// are stunned for HALO_EMP_STUN_MILLIS (halovehicleemp). The tags' stun fields are all zero, so
// the duration is the engine's. "Lethal to the unsuspecting" (every campaign weapon's melee): the
// player's melee on an actor from behind that is not fighting the player kills it (halodamage).
enum { HALO_SIDE_EFFECT_NONE = 0, HALO_SIDE_EFFECT_HARMLESS = 1, HALO_SIDE_EFFECT_UNSUSPECTING = 2, HALO_SIDE_EFFECT_EMP = 3 };
const int HALO_EMP_STUN_MILLIS = 2000;

struct HaloProjectileDef
{
    bool valid = false;
    float speed = 0, finalSpeed = 0, range = 0, gravity = 0, guided = 0;
    HaloDamage damage;
    float radius = 0, innerRadius = 0;      // area damage: full inside the inner, the lower bound at the outer
    float rangeStart = 0, rangeEnd = 0;     // air damage range: impacts fall to the lower bound across it
    int category = -1, sideEffect = 0;      // side effect 3: EMP (the charged plasma bolt)
    std::string path;
    // The needle's super detonation (a `supercombine` line): when enough of these rounds are stuck
    // in one body within `timer` seconds of each other (halo_supercombine, an engine choice: the
    // tags hold no count), they detonate together with this area damage at the body.
    struct SuperDetonation
    {
        bool valid = false;
        float timer = 0, innerRadius = 0, radius = 0;
        HaloDamage damage;
        int sideEffect = 0, category = 0;
    } super;
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
    HaloDamage melee;
    int meleeSideEffect = 0;                // the melee jpt!'s side effect: 2 lethal to the unsuspecting
    std::vector<HaloMagazineDef> magazines;
    std::vector<HaloTriggerDef> triggers;
    HaloSounds sounds;

    const char *sound(int role) const { return sounds.get(role); }
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

// Grenades and equipment from packages/halo/equipment.txt (tools/halo_weapons.py): the globals'
// grenade types and every campaign eqip tag (ammo, health packs, powerups, grenades).
enum { HALO_GRENADE_FRAG = 0, HALO_GRENADE_PLASMA = 1, HALO_GRENADE_TYPES = 2 };
enum { HALO_TIMER_IMMEDIATELY = 0, HALO_TIMER_AFTER_BOUNCE, HALO_TIMER_AT_REST, HALO_TIMER_ANY_BOUNCE };
enum HaloPowerup { HALO_POWERUP_NONE = 0, HALO_POWERUP_SPEED, HALO_POWERUP_OVERSHIELD, HALO_POWERUP_CAMO,
                   HALO_POWERUP_VISION, HALO_POWERUP_HEALTH, HALO_POWERUP_GRENADE };

struct HaloGrenadeDef
{
    int type = -1, maximum = 4;
    bool sticks = false;                    // attaches to units it touches (the plasma grenade)
    int timerStarts = HALO_TIMER_AFTER_BOUNCE;
    float arming = 0, fuse = 2;             // seconds: no detonation before arming; fuse once the timer starts
    unsigned flags = 0;
    float innerRadius = 0, outerRadius = 0; // world units
    HaloDamage blast;
    int sideEffect = 0, category = 0;       // side effect 3: EMP
    HaloDamage attached;                    // to the unit a sticky grenade is stuck to
    std::string path, equipment;
    HaloSounds sounds;
    const char *sound(int role) const { return sounds.get(role); }
};

struct HaloEquipmentAmmo { int weapon, rounds; };

struct HaloEquipmentDef
{
    int index = -1, powerup = HALO_POWERUP_NONE, grenade = 0;
    float powerupTime = 0;                  // seconds
    std::string path, name, worldmodel;
    std::vector<HaloEquipmentAmmo> ammo;    // the weapons.txt weapons whose magazine takes it
    HaloSounds sounds;
    const char *sound(int role) const { return sounds.get(role); }
    // Rounds it gives the weapon, or 0 when it is not that weapon's ammo.
    int roundsFor(int weapon) const { for(const HaloEquipmentAmmo &a : ammo) if(a.weapon == weapon) return a.rounds; return 0; }
};

bool haloequipmentload();
const HaloGrenadeDef *halogrenadedef(int type);
int haloequipmentcount();
const HaloEquipmentDef *haloequipment(int index);
const HaloEquipmentDef *haloequipmentfind(const char *path);
// The eqip a grenade type is picked up as (dropped by a killed actor), or null.
const HaloEquipmentDef *halogrenadeequipment(int type);
// Interface sounds by role (shield-low, health-low), or null.
const char *halointerfacesound(const char *role);

#endif
