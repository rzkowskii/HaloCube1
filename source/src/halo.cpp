#include "cube.h"
#include "bot/bot.h"
#include "halo.h"
#include "halo_collision.h"
#include "halo_script.h"
#include "halo_campaign.h"
#include "halo_weapons.h"
#include "halo_ai.h"
#include "halo_hud.h"
#include "halo_fx.h"
#include "halo_vehicle.h"
#include "halo_state.h"
#include <cmath>
#include <string>
#include <utility>
#include <vector>

extern void startgame(const char *name, int mode, int minutes, bool notify);
extern int halo_scripttrace;     // halo_script.cpp: trace logging

namespace
{
    const char *const progressFile = "config/halo-progress.txt";
    const int noMission = -1;
    int mission = noMission, section = 0;
    int pendingMission = 0, pendingSection = 0;
    bool preview = false, pendingPreview = false;
    bool requestedSection = false, pendingContinuation = false;
    int savedMission = 0, savedSection = 0;
    bool progressLoaded = false;
    bool campaignFinished = false, authoredTransition = false;
    // Ending: set once The Maw's finale plays out (game_won, or the finale cue ending the last section).
    bool campaignBeaten = false, ending = false, endingDone = false, winPending = false;
    int endingStart = 0;
    const int creditsTime = 34000;
    // The Maw's timed escape (sections 6 to 8), driven in a Warthog (halo_vehicle.cpp).
    bool escaping = false, escapeOffered = false;
    int escapeDeadline = 0, incomingEscapeTime = 0, escapeStarted = 0;
    const int escapeTime = 180000;
    const int playerShield = 75, playerHealth = 75;
    const int shieldDelay = 6000, shieldRecharge = 4000;

    // What the player carries: the two weapons, grenades, health and shield. Carried from one
    // section to the next within a mission, restored by Continue, and the loadout a revert before
    // the section's first checkpoint returns to.
    struct Loadout
    {
        bool valid = false;
        HaloWeaponState carried[2];
        int current = 0;
        int weapon = GUN_ASSAULT;                          // the AssaultCube slot, without weapons.txt
        int mag[2] = {60, 12}, ammo[2] = {180, 48};        // assault rifle and pistol, without weapons.txt
        int health = playerHealth;
        float shield = playerShield;
        int frags = 2, plasmas = 0, grenadeType = HALO_GRENADE_FRAG;
    };
    Loadout sectionLoadout, carryLoadout;
    int sectionLoadoutMission = -1, sectionLoadoutSection = -1;

    // The game Continue resumes (progress file version 2): the section and, once one was reached,
    // the last checkpoint in it (position, mission stage and route marker), with the loadout,
    // the difficulty and the escape countdown left at the section's start (The Maw's run).
    struct SavedGame
    {
        bool valid = false, checkpoint = false;
        int mission = 0, section = 0, difficulty = 1;
        vec position;
        float yaw = 0;
        int stage = 0, marker = 0, escapeMillis = 0;
        Loadout loadout;
        string weaponPath[2];                               // retail weapon tags, resolved on restore
        bool slotEmpty[2] = {false, false};                 // an `empty <slot>` line: no weapon there (a10's unarmed start)
        // Version 3: the state block (its `state` header and the s, v, w and p records, verbatim):
        // the mission script, vehicles, ground items and player limits at the checkpoint, or the
        // script carried into the section at its start. Empty when there is none.
        std::string state;
    };
    SavedGame savedGame;
    bool restorePending = false;                            // Continue: apply savedGame once the section starts
    // The section's script was carried in (from the section before, or restored by Continue), so
    // a save at its start keeps the script's state; pendingStartState: that save waits for the
    // section's opening scene to end.
    bool scriptCarried = false;
    bool pendingStartState = false;
    string progressNote = "";                               // the last load's warning (corrupt file), for the menus
    bool titleScreen = false;                               // the title menu (halo_title) is up
    bool keepSectionLoadout = false;                        // halo_restart: keep what the player brought in
    const int eliteDelay = 12000, eliteRecharge = 10000;
    const float markerRadius = 8.0f, firstContactRadius = 62.0f;

    enum MissionStage { INSERTION, FIRST_CONTACT, REACH_PASS, SECOND_CONTACT, EXTRACTION, COMPLETE };
    // MONITOR and ENGINEER (tools/halo_actors.py KINDS 10 and 11) fly and never fight.
    enum ActorKind { GRUNT, ELITE, JACKAL, HUNTER, FLOOD_ELITE, FLOOD_HUMAN, INFECTION, CARRIER, SENTINEL, MARINE, MONITOR, ENGINEER,
                     ACTOR_COUNT };

    struct ActorProfile
    {
        const char *name, *model;
        int health, shield, delay, damage;
        float eyeheight, radius;
        bool melee;
    };

    const ActorProfile profiles[ACTOR_COUNT] = {
        {"Grunt", "halo/grunt", 40, 0, 400, 5, 2.6f, 0.9f, false},
        {"Elite", "halo/elite", 100, 100, 180, 8, 4.0f, 1.1f, false},
        {"Jackal", "halo/jackal", 50, 45, 420, 6, 3.3f, 0.9f, false},
        {"Hunter", "halo/hunter", 240, 0, 960, 26, 5.6f, 1.4f, false},
        {"Elite Flood", "halo/flood-elite", 75, 0, 265, 9, 4.0f, 1.1f, false},
        {"Human Flood", "halo/flood-human", 65, 0, 270, 9, 3.7f, 1.1f, false},
        {"Infection Form", "halo/infection", 15, 0, 550, 18, 1.0f, 0.45f, true},
        {"Carrier Form", "halo/carrier", 50, 0, 900, 36, 2.7f, 0.95f, true},
        {"Sentinel", "halo/sentinel", 65, 0, 225, 7, 3.8f, 0.95f, false},
        {"Marine", "halo/marine", 45, 24, 300, 6, 3.6f, 1.0f, false},
        {"343 Guilty Spark", "halo/monitor", 200, 0, 1000, 0, 1.4f, 0.7f, true},
        {"Engineer", "halo/engineer", 30, 0, 1000, 0, 2.4f, 1.0f, true},
    };

    struct HaloEnemy
    {
        botent *bot;
        ActorKind kind;
        float shield;
        int hitTime;
        HaloWeaponState weapon;
        int team;
        float shieldMax;
        // The variant's damage modifier on its weapon's jpt! damage (halofiretrigger), and the
        // fraction of a health point the exact hits on it have left over (halodamage).
        float damageScale = 0.5f, healthCarry = 0;
        // When the actor was last hurt, which makes active camouflage flicker (haloactorcloak).
        int hurtAt = -100000;
        // When it last fired its weapon (haloactorfired, from halofiretrigger): a Hunter's belly
        // window opens after its own cannon shot, not after its smash (which sets lastaction too).
        int firedAt = -100000;
    };

    // The hit weapon.cpp is dealing (halohit): its exact amount and jpt! side effect, which
    // halodamage uses in place of the whole number hit() carries.
    // `at` is where it struck the body, when the caller knows (shots and the player's melee): the
    // Hunter's head and belly and the head material of the general model go by its height.
    struct PendingHit
    {
        const playerent *target = NULL;
        float damage = 0;
        int sideEffect = 0;
        const HaloDamage *source = NULL;
        bool hasAt = false;
        vec at = vec(0, 0, 0);
    };
    PendingHit pendingHit;
    // The fraction of a health point exact hits on the player have left over.
    float playerHealthCarry = 0;

    // A weapon lying in the world: scenario placements and drops.
    struct GroundWeapon
    {
        vec o;
        float yaw;
        HaloWeaponState state;
    };

    // Enemies deal a share of the retail projectile damage, standing in for
    // the AI damage modifiers that the actor definitions apply on Normal.
    const float aiDamageScale = 0.5f;
    const char *const defaultWeapons[2] = {"weapons\\assault rifle\\assault rifle", "weapons\\pistol\\pistol"};

    bool active = false, startPending = false, launchPending = false, controlsLoaded = false;
    MissionStage stage = INSERTION;
    int insertion = -1, checkpoint = -1, extraction = -1;
    int shieldHit = 0;
    bool shieldRecharging = false;
    float shield = playerShield;
    // Script-set limits: unit_set_maximum_vitality and unit_impervious on the player.
    float shieldCap = playerShield, maxVitalityHealth = 100, maxVitalityShield = 75;
    bool playerProtected = false;
    bool atCheckpoint = false;
    int checkpointHealth = playerHealth, checkpointWeapon = GUN_ASSAULT;
    int checkpointMag[NUMGUNS], checkpointAmmo[NUMGUNS];
    vector<uchar> checkpointItems;
    vec checkpointPosition;
    float checkpointYaw = 0;
    MissionStage checkpointStage = SECOND_CONTACT;
    int checkpointMarker = 0;
    vector<int> waveStarts[2], route[3];
    int nextRouteMarker = 0;
    vector<HaloEnemy> enemies;
    // Flood life cycle: carriers burst into infection forms, infection forms
    // reaching a marine or Elite body turn it into a combat form after a delay.
    struct Corpse { vec o; float yaw; ActorKind kind; int expires; };
    struct FloodSpawn { vec o; float yaw; ActorKind kind; int count; int at; };
    vector<Corpse> corpses;
    vector<FloodSpawn> floodSpawns;
    const int corpseLife = 90000, infectionDelay = 2500;
    const float infectionReach = 4.0f;

    void clearFlood() { corpses.setsize(0); floodSpawns.setsize(0); }
    // The player carries two retail weapons, each bound to an AssaultCube slot.
    HaloWeaponState carried[2], checkpointCarried[2];
    int carriedGun[2] = {GUN_ASSAULT, GUN_PISTOL}, currentCarried = 0, checkpointCurrent = 0;
    vector<GroundWeapon> groundWeapons, checkpointGround;
    int nearbyWeapon = -1, pickupMessage = 0;
    string pickupText = "";

    // Grenades. The frag count stays player1->mag[GUN_GRENADE], which the scripts and
    // AssaultCube read; the plasma count and the selected type live here.
    int plasmaGrenades = 0, grenadeType = HALO_GRENADE_FRAG, checkpointPlasma = 0, checkpointGrenadeType = HALO_GRENADE_FRAG;
    // Powerups. The overshield is shield beyond the cap (up to two more layers, tripling it):
    // it charges over overshieldCharge ms, then drains over the powerup's time. Active
    // camouflage lasts its powerup time and fades over the last camoFade ms.
    float overshield = 0, overshieldTarget = 0;
    int overshieldCharged = 0, overshieldDrain = 60000, camoUntil = 0;
    const int overshieldCharge = 1500, camoFade = 3000;
    int lowShieldSound = 0, lowHealthSound = 0;

    // Equipment lying in the world: scenario placements (ammo, health packs, grenades, the
    // overshield and active camouflage) and the grenades killed actors drop.
    struct GroundItem
    {
        vec o;
        float yaw;
        int def, object, flags;             // equipment.txt index, scenario object (-1 none), not-placed flags
        bool exists, created;               // on the ground now; the script has created it (flag 1)
    };
    vector<GroundItem> groundItems, checkpointItemsHalo;

    int &grenadeCount(int type) { return type == HALO_GRENADE_PLASMA ? plasmaGrenades : player1->mag[GUN_GRENADE]; }
    int grenadeMaximum(int type) { const HaloGrenadeDef *g = halogrenadedef(type); return g ? g->maximum : 4; }
    // Without packages/halo/equipment.txt only the frag grenade exists.
    bool grenadeKnown(int type) { return type == HALO_GRENADE_FRAG || halogrenadedef(type); }

    // Retail not-placed flags: 1 the script creates it, 2 not on Easy, 4 not on Normal, 8 not on
    // Heroic (Legendary follows Heroic), as for the scenario vehicles.
    bool itemDifficultyAllows(int flags)
    {
        static const int bits[4] = {2, 4, 8, 8};
        return !(flags & bits[halodifficulty()]);
    }

    vec position(const entity &e) { return vec(e.x, e.y, e.z); }
    bool near(int i, float radius)
    {
        const entity &e = ents[i];
        return fabsf(player1->o.z - e.z) < 14.0f && player1->o.distxy(position(e)) <= radius;
    }

    HaloEnemy *enemyState(const playerent *p)
    {
        loopv(enemies) if(enemies[i].bot == p) return &enemies[i];
        return NULL;
    }

    void campaignWeapon(playerent *p, int gun, int mag, int delay, int reload, int damage, int spread)
    {
        guninfo &info = p->weapons[gun]->info;
        info.magsize = mag;
        info.attackdelay = delay;
        info.reloadtime = reload;
        info.damage = damage;
        info.spread = spread;
        info.recoil = 3;
        info.mdl_kick_back = info.mdl_kick_rot = 0;
        info.recoilincrease = 32;
        info.recoilbase = 3;
        info.maxrecoil = 8;
    }

    // The carried weapons come from packages/halo/weapons.txt when it loads and has the default
    // assault rifle and pistol; a slot may then be empty (a weapon state with no definition: a10
    // starts unarmed, its starting profile carrying none). Without the table the player has
    // AssaultCube's own assault rifle and pistol.
    bool tagWeapons()
    {
        static int usable = -1;
        if(usable < 0) usable = haloweaponsload() && haloweaponfind(defaultWeapons[0]) && haloweaponfind(defaultWeapons[1]) ? 1 : 0;
        return usable == 1;
    }
    bool slotArmed(int i) { return haloweapon(carried[i].def) != NULL; }
    // Unarmed: the selected slot holds no weapon (its AssaultCube slot is the knife, which neither
    // fires nor draws a viewmodel here; melee still strikes).
    bool unarmed() { return tagWeapons() && !slotArmed(currentCarried); }

    // Two carried weapons need distinct AssaultCube slots; projectile weapons
    // share the carbine and submachine-gun slots between them. An empty slot is the knife.
    void assignGuns()
    {
        loopi(2)
        {
            const HaloWeaponDef *d = haloweapon(carried[i].def);
            carriedGun[i] = d ? d->acgun() : tagWeapons() ? GUN_KNIFE : (i ? GUN_PISTOL : GUN_ASSAULT);
        }
        if(carriedGun[0] == carriedGun[1] && carriedGun[0] != GUN_KNIFE) carriedGun[1] = carriedGun[0] == GUN_CARBINE ? GUN_SUBGUN : GUN_CARBINE;
    }

    // Copy the slot's live rounds back into the carried weapon.
    void syncCarried()
    {
        if(!tagWeapons()) return;
        loopi(2)
        {
            const HaloWeaponDef *d = haloweapon(carried[i].def);
            if(!d || d->usesBattery()) continue;
            carried[i].loaded = player1->mag[carriedGun[i]];
            carried[i].reserve = player1->ammo[carriedGun[i]];
        }
    }

    void applyCarried(int i)
    {
        const HaloWeaponDef *d = haloweapon(carried[i].def);
        if(!d) return;
        const int gun = carriedGun[i];
        const HaloMagazineDef *m = d->magazine();
        const HaloTriggerDef *t = d->trigger();
        guninfo &info = player1->weapons[gun]->info;
        info.magsize = d->usesBattery() ? 1 : max(1, m->loaded);
        info.attackdelay = t ? carried[i].delay(*t) : 100;
        info.reloadtime = m ? int(m->reload * 1000) : 0;
        // Nominal only: the tag weapon's shots roll their own damage in halofiretrigger.
        info.damage = t && t->projectile.valid ? int(t->projectile.damage.upperMin + .5f) : 10;
        info.spread = 0;
        info.recoil = 3;
        info.mdl_kick_back = info.mdl_kick_rot = 0;
        info.recoilincrease = 32;
        info.recoilbase = 3;
        info.maxrecoil = 8;
        info.isauto = t && max(t->rate, t->finalRate) >= 5;
        player1->mag[gun] = d->usesBattery() ? (carried[i].canfire(*d) ? 1 : 0) : carried[i].loaded;
        player1->ammo[gun] = d->usesBattery() ? 0 : carried[i].reserve;
    }

    void selectCarried(int i, bool immediate)
    {
        currentCarried = i;
        carried[0].zoom = carried[1].zoom = 0;
        weapon *w = player1->weapons[carriedGun[i]];
        if(immediate)
        {
            player1->selectweapon(carriedGun[i]);
            player1->nextweaponsel = player1->weaponsel;
            player1->weaponchanging = 0;
        }
        else if(w != player1->weaponsel) player1->weaponswitch(w);
    }

    void equipCarried(bool immediate)
    {
        assignGuns();
        loopi(2) applyCarried(i);
        // With an armed slot the selection never rests on an empty one.
        if(!slotArmed(currentCarried) && slotArmed(currentCarried ^ 1)) currentCarried ^= 1;
        const int primary = slotArmed(0) || !slotArmed(1) ? carriedGun[0] : carriedGun[1];
        player1->setnextprimary(primary);
        player1->setprimary(primary);
        selectCarried(currentCarried, immediate);
    }

    void captureLoadout(Loadout &l)
    {
        syncCarried();
        l.valid = true;
        loopi(2) l.carried[i] = carried[i];
        l.current = currentCarried;
        l.weapon = player1->weaponsel ? player1->weaponsel->type : GUN_ASSAULT;
        l.mag[0] = player1->mag[GUN_ASSAULT]; l.ammo[0] = player1->ammo[GUN_ASSAULT];
        l.mag[1] = player1->mag[GUN_PISTOL]; l.ammo[1] = player1->ammo[GUN_PISTOL];
        l.health = player1->health;
        l.shield = shield;
        l.frags = grenadeCount(HALO_GRENADE_FRAG);
        l.plasmas = plasmaGrenades;
        l.grenadeType = grenadeType;
    }

    // Puts a saved loadout on the player over the default one. Weapons the table no longer has
    // keep the default in that slot.
    void applyLoadout(const Loadout &l)
    {
        if(!l.valid) return;
        // With the weapon table an empty slot stays empty (a10's unarmed start, carried on).
        loopi(2) if(haloweapon(l.carried[i].def) || (tagWeapons() && l.carried[i].def < 0))
        {
            carried[i] = l.carried[i];
            carried[i].heat = 0;
            carried[i].overheated = carried[i].charged = false;
            carried[i].lastShot = carried[i].chargeStart = carried[i].zoom = 0;
        }
        currentCarried = l.current ? 1 : 0;
        if(tagWeapons())
        {
            loopi(NUMGUNS) if(i != GUN_GRENADE && i != GUN_KNIFE) player1->mag[i] = player1->ammo[i] = 0;
            equipCarried(true);
        }
        else
        {
            player1->mag[GUN_ASSAULT] = l.mag[0]; player1->ammo[GUN_ASSAULT] = l.ammo[0];
            player1->mag[GUN_PISTOL] = l.mag[1]; player1->ammo[GUN_PISTOL] = l.ammo[1];
            player1->selectweapon(l.weapon == GUN_PISTOL ? GUN_PISTOL : GUN_ASSAULT);
            player1->nextweaponsel = player1->weaponsel;
            player1->weaponchanging = 0;
        }
        player1->health = clamp(l.health, 1, playerHealth);
        shield = clamp(l.shield, 0.0f, shieldCap);
        grenadeCount(HALO_GRENADE_FRAG) = clamp(l.frags, 0, grenadeMaximum(HALO_GRENADE_FRAG));
        plasmaGrenades = grenadeKnown(HALO_GRENADE_PLASMA) ? clamp(l.plasmas, 0, grenadeMaximum(HALO_GRENADE_PLASMA)) : 0;
        grenadeType = grenadeKnown(l.grenadeType) && grenadeCount(l.grenadeType) > 0 ? l.grenadeType : HALO_GRENADE_FRAG;
    }

    void resetLoadout()
    {
        if(atCheckpoint)
        {
            loopi(2) carried[i] = checkpointCarried[i];
            currentCarried = checkpointCurrent;
        }
        else
        {
            loopi(2) carried[i].reset(haloweaponfind(defaultWeapons[i]));
            currentCarried = 0;
        }
        if(tagWeapons())
        {
            loopi(NUMGUNS) if(i != GUN_GRENADE && i != GUN_KNIFE) player1->mag[i] = player1->ammo[i] = 0;
            equipCarried(true);
        }
        else
        {
            // Without packages/halo/weapons.txt keep the original MA5B and M6D numbers.
            campaignWeapon(player1, GUN_ASSAULT, 60, 67, 3400, 10, 60);
            campaignWeapon(player1, GUN_PISTOL, 12, 286, 2170, 25, 14);
            player1->setnextprimary(GUN_ASSAULT);
            player1->setprimary(GUN_ASSAULT);
            player1->selectweapon(atCheckpoint ? checkpointWeapon : GUN_ASSAULT);
            player1->nextweaponsel = player1->weaponsel;
            player1->weaponchanging = 0;
            player1->mag[GUN_ASSAULT] = 60;
            player1->ammo[GUN_ASSAULT] = 180;
            player1->mag[GUN_PISTOL] = 12;
            player1->ammo[GUN_PISTOL] = 48;
            if(atCheckpoint)
            {
                memcpy(player1->mag, checkpointMag, sizeof(checkpointMag));
                memcpy(player1->ammo, checkpointAmmo, sizeof(checkpointAmmo));
            }
        }
        player1->health = atCheckpoint ? checkpointHealth : playerHealth;
        player1->armour = 0;
        player1->mag[GUN_GRENADE] = atCheckpoint ? checkpointMag[GUN_GRENADE] : 2;
        plasmaGrenades = atCheckpoint ? checkpointPlasma : 0;
        grenadeType = atCheckpoint ? checkpointGrenadeType : HALO_GRENADE_FRAG;
        overshield = overshieldTarget = 0;
        camoUntil = 0;
        // Reverting restores the weapons and equipment lying in the world as they were saved;
        // before the first checkpoint that is the section's placements.
        groundWeapons.setsize(0);
        loopv(checkpointGround) groundWeapons.add(checkpointGround[i]);
        groundItems.setsize(0);
        loopv(checkpointItemsHalo) groundItems.add(checkpointItemsHalo[i]);
        shield = shieldCap;
        shieldHit = 0;
        shieldRecharging = false;
        playerHealthCarry = 0;
        // Before the section's first checkpoint a revert returns to what the player brought into
        // the section (from the section before, or a continued game), not the default loadout.
        if(!atCheckpoint) applyLoadout(sectionLoadout);
    }

    void recordProgress(bool atCheckpointNow);

    // Starting profiles a fresh start of a later section adds to the mission's profile 0, for what
    // its script gives the player in an earlier section and does not give again when started
    // afresh there: a10's pistol at the bridge (mission_shoot, just past cinematic_x20 at the start
    // of section 3). The mission script's counterpart is freshScripts in halo_script.cpp (a10
    // sections 5 and 6: mission_tunnel and cryo_search woken, the bridge flags set).
    struct FreshProfile { int mission, section; const char *profile; };
    const FreshProfile freshProfiles[] = { {0, 2, "bridge0_profile"} };

    // A fresh start's loadout: the scenario's player starting profile 0 (tools/import_halo_scripts.py,
    // .hscb version 9), which also becomes what a revert before the section's first checkpoint
    // returns to. An older .hscb has no profiles: the default assault rifle, pistol and two frags.
    void applyStartingProfile()
    {
        HaloStartingProfile profile;
        if(!haloscenarioprofile(0, NULL, profile)) return;
        haloplayeraddequipment(profile, true);
        for(const FreshProfile &f : freshProfiles)
            if(f.mission == mission && section >= f.section && haloscenarioprofile(-1, f.profile, profile)) haloplayeraddequipment(profile, false);
        captureLoadout(sectionLoadout);
        sectionLoadoutMission = mission;
        sectionLoadoutSection = section;
    }

    // Takes the checkpoint a revert returns to (without saving it to the progress file).
    void takeCheckpoint(bool scripted)
    {
        atCheckpoint = true;
        checkpointStage = scripted ? stage : SECOND_CONTACT;
        checkpointMarker = scripted ? nextRouteMarker : 0;
        checkpointPosition = scripted ? player1->o : position(ents[checkpoint]);
        checkpointYaw = scripted ? player1->yaw : ents[checkpoint].attr1 / 10.0f;
        checkpointHealth = player1->health;
        checkpointWeapon = player1->weaponsel->type;
        memcpy(checkpointMag, player1->mag, sizeof(checkpointMag));
        memcpy(checkpointAmmo, player1->ammo, sizeof(checkpointAmmo));
        syncCarried();
        loopi(2) checkpointCarried[i] = carried[i];
        checkpointCurrent = currentCarried;
        checkpointGround.setsize(0);
        loopv(groundWeapons) checkpointGround.add(groundWeapons[i]);
        checkpointPlasma = plasmaGrenades;
        checkpointGrenadeType = grenadeType;
        checkpointItemsHalo.setsize(0);
        loopv(groundItems) checkpointItemsHalo.add(groundItems[i]);
        checkpointItems.setsize(0);
        loopv(ents) checkpointItems.add(ents[i].spawned);
        haloscenariocheckpoint();
        halovehiclescheckpoint();
    }

    void saveCheckpoint(bool scripted = false)
    {
        pendingStartState = false;
        takeCheckpoint(scripted);
        conoutf("Halo: checkpoint reached");
        recordProgress(true);
    }

    // The side a body fights on when neither the scenario nor the variant names one.
    int kindTeam(ActorKind kind)
    {
        switch(kind)
        {
            case MARINE: return HALO_TEAM_HUMAN;
            case FLOOD_ELITE: case FLOOD_HUMAN: case INFECTION: case CARRIER: return HALO_TEAM_FLOOD;
            case SENTINEL: case MONITOR: return HALO_TEAM_SENTINEL;
            default: return HALO_TEAM_COVENANT;
        }
    }

    // team HALO_TEAM_DEFAULT: the variant's unit team (actors.txt), else the body's.
    botent *spawnEnemy(ActorKind kind, const vec &origin, float yaw, int weaponIndex = -1, int actorIndex = -1,
                       int team = HALO_TEAM_DEFAULT)
    {
        const ActorProfile &profile = profiles[kind];
        // Script-spawned actors arrive without a variant; give them their usual weapon.
        static const char *const usual[ACTOR_COUNT] = {
            "weapons\\plasma pistol\\plasma pistol", "weapons\\plasma rifle\\plasma rifle",
            "weapons\\plasma pistol\\plasma pistol", "weapons\\fuel rod gun\\hunter fuel rod",
            "weapons\\plasma rifle\\plasma rifle", "weapons\\assault rifle\\assault rifle", NULL, NULL,
            "characters\\sentinel\\sentinel", "weapons\\assault rifle\\assault rifle", NULL, NULL,
        };
        const HaloActorDef *actor = haloactordef(actorIndex);
        if(actor && actor->weapon >= 0) weaponIndex = actor->weapon;
        const HaloWeaponDef *def = profile.melee ? nullptr :
            weaponIndex >= 0 ? haloweapon(weaponIndex) : usual[kind] ? haloweaponfind(usual[kind]) : nullptr;
        if(!actor) actor = haloactordefault(kind, def ? def->index : -1);
        if(team < 0 || team >= HALO_TEAM_COUNT) team = haloactordefaultteam(actor, kindTeam(kind));
        defformatstring(name)("%.12s %d", profile.name, enemies.length() + 1);
        const bool human = team == HALO_TEAM_HUMAN || team == HALO_TEAM_PLAYER;
        botent *bot = BotManager.CreateBot(human ? "RVSF" : "CLA", kind == ELITE ? "worse" : "bad", name);
        if(!bot) return nullptr;
        // The variant's vitality overrides the body defaults when it sets one.
        // Difficulty toughens or softens the Covenant and the Flood; allies are unchanged.
        const float vitality = human ? 1.0f : halodifficultyhealth();
        const float shieldMax = (actor && actor->shield > 0 ? actor->shield : float(profile.shield)) * vitality;
        enemies.add(HaloEnemy{bot, kind, shieldMax, lastmillis, HaloWeaponState(), team, shieldMax});
        enemies.last().weapon.reset(def);
        bot->health = max(1, int((actor && actor->body > 0 ? int(actor->body + .5f) : profile.health) * vitality + .5f));
        bot->maxeyeheight = bot->eyeheight = profile.eyeheight;
        bot->radius = profile.radius;
        bot->armour = 0;
        bot->o = origin;
        bot->yaw = yaw;
        bot->pitch = bot->roll = 0;
        entinmap(bot);
        bot->targetyaw = bot->yaw;
        // Variants that cannot use ranged weapons (the energy sword Elites) fight in melee.
        const bool meleeOnly = profile.melee || (actor && actor->meleeOnly());
        const int gun = meleeOnly ? GUN_KNIFE : GUN_SUBGUN;
        bot->setprimary(gun);
        bot->setnextprimary(gun);
        bot->selectweapon(gun);
        bot->nextweaponsel = bot->weaponsel;
        bot->weaponchanging = 0;
        loopj(NUMGUNS) bot->mag[j] = bot->ammo[j] = 0;
        // Each shot rolls the retail projectile's full-strength range (the jpt! upper bound) with
        // its range falloff, times the variant's damage modifier, in halofiretrigger (exact, not
        // rounded per shot). The slot keeps the nominal damage (the upper bound's min times the
        // modifier) for the plain bolt of an actor without weapon data. The variant's rate of
        // fire sets the cadence inside a burst.
        const HaloTriggerDef *t = def ? def->trigger() : nullptr;
        const float damageScale = actor && actor->damageScale > 0 ? actor->damageScale : aiDamageScale;
        enemies.last().damageScale = damageScale;
        // A melee-only variant strikes with its weapon's melee jpt! (the sword's 151) times the modifier.
        const int damage = meleeOnly && def && def->melee.upperMin > 0 ? max(1, int(def->melee.upperMin * damageScale + .5f)) :
                           t && t->projectile.valid ? max(1, int(t->projectile.damage.upperMin * damageScale + .5f)) : profile.damage;
        int delay = actor && actor->rate > 0 ? int(1000 / actor->rate) :
                    t ? max(profile.delay, enemies.last().weapon.delay(*t)) : profile.delay;
        // A sword swing every 1.2 s at most (the slot's ranged cadence would swing five times a second).
        if(meleeOnly && !profile.melee) delay = max(delay, 1200);
        campaignWeapon(bot, gun, 100, delay, 0, damage, kind == ELITE ? 36 : 56);
        bot->mag[gun] = 100;
        haloactorattach(bot, actor, team);
        return bot;
    }

    void spawnWave(int wave)
    {
        loopv(waveStarts[wave])
        {
            entity &start = ents[waveStarts[wave][i]];
            const ActorKind kind = start.attr4 >= 0 && start.attr4 < ACTOR_COUNT ? ActorKind(start.attr4) : GRUNT;
            spawnEnemy(kind, position(start), start.attr1 / 10.0f, start.attr5 - 1);
        }
        if(!waveStarts[wave].empty()) conoutf("Halo: encounter %d", wave + 1);
    }

    int remaining()
    {
        int count = 0;
        loopv(enemies) if(enemies[i].bot && enemies[i].bot->state == CS_ALIVE && !haloteamfriendly(enemies[i].team) &&
                          !haloactorpassive(enemies[i].bot)) ++count;
        return count;
    }

    vec contactPoint()
    {
        if(waveStarts[0].empty()) return position(ents[checkpoint]);
        vec midpoint;
        loopv(waveStarts[0]) midpoint.add(position(ents[waveStarts[0][i]]));
        return midpoint.div(waveStarts[0].length());
    }

    void setStage(MissionStage next)
    {
        stage = next;
        nextRouteMarker = 0;
        if(next == FIRST_CONTACT) spawnWave(0);
        if(next == SECOND_CONTACT) spawnWave(1);
        if(next == COMPLETE)
        {
            player1->attacking = false;
            player1->stopmoving();
            intermission = true;
            conoutf("Halo: mission complete");
        }
    }

    bool escapeRun() { return mission == 9 && section >= 5; }
    // Two Betrayals' sections 1 and 4 on retail collision are flown, not walked: their way on is
    // up to a pulse generator or a landing pad (DRIVEN_SECTIONS in tools/import_halo_campaign.py).
    bool flownSection() { return mission == 7 && (section == 0 || section == 3) && halocollisionactive(); }
    bool retailCartographer() { return mission == 3 && halocollisionactive(); }

    const char *sectionInteraction()
    {
        if(preview) return NULL;
        if(mission == 2 && section == 3) return "Free Captain Keyes";
        if(mission == 3 && section == 1) return "Access the Cartographer";
        if(mission == 3 && section == 2 && halocollisionactive()) return "Board the Pelican";
        if(mission == 4 && section == 6) return "Access the control room";
        if(mission == 5 && section == 4) return "Investigate the containment lab";
        if(mission == 6 && section == 3) return "Retrieve the Index";
        if(mission == 7 && section == 4) return "Disable the final pulse generator";
        if(mission == 8 && section == 4) return "Recover the Captain's implants";
        if(mission == 9 && section == 4) return "Sabotage the reactor controls";
        return NULL;
    }

    void finishSection(bool completedScene = false)
    {
        if(!completedScene && haloscenarioexit()) return;
        setStage(COMPLETE);
        campaignFinished = !preview && mission == haloMissionCount - 1 && section == haloMissions[mission].sections - 1;
    }

    void clearRecovery();   // fall and wedge recovery on retail collision (below)

    void resetEncounter()
    {
        haloclearbolts();
        enemies.setsize(0);
        clearFlood();
        haloactorsclear();
        kickallbots();
        clearbounceents();
        showscores(false);
        player1->team = TEAM_RVSF;
        spawnplayer(player1, false);
        gotoplayerstart(player1, &ents[insertion]);
        if(atCheckpoint)
        {
            player1->o = checkpointPosition;
            player1->yaw = checkpointYaw;
        }
        else if(halocollisionactive()) player1->o.z += player1->eyeheight;
        entinmap(player1);
        player1->resetinterp();
        haloplayermoved(atCheckpoint ? "revert to the checkpoint" : "spawn");
        resetLoadout();
        halovehiclesrestart(atCheckpoint);
        escaping = escapeRun();
        player1->maxspeed = 16.0f;
        escapeDeadline = escaping ? lastmillis + (incomingEscapeTime > 0 ? incomingEscapeTime : escapeTime) : 0;
        incomingEscapeTime = 0;
        escapeStarted = lastmillis;
        // A checkpoint revert may already have put the player back in their seat.
        escapeOffered = escaping && halodriving();
        // The run is driven. Past its first section, and on the retail collision (where the
        // walking body does not fit the floors), the player starts at the wheel of a Warthog.
        if(escaping && !escapeOffered && (section > 5 || halocollisionactive()))
            escapeOffered = halovehicleescape(position(ents[insertion]), ents[insertion].attr1 / 10.0f);
        loopv(ents) if(isitem(ents[i].type))
        {
            bool available = !atCheckpoint || (checkpointItems.inrange(i) && checkpointItems[i]);
            setpickupspawn(i, available);
            ents[i].lastmillis = 0;
        }
        intermission = false;
        if(escaping) setStage(EXTRACTION);
        else if(atCheckpoint)
        {
            setStage(checkpointStage);
            nextRouteMarker = checkpointMarker;
        }
        else setStage(retailCartographer() ? EXTRACTION : INSERTION);
    }

    vec feetOf(const playerent *p) { return vec(p->o).sub(vec(0, 0, p->eyeheight)); }

    // A new actor beside a body: on retail collision it must find floor, otherwise
    // it only needs open space, as entinmap settles it.
    bool spawnNear(ActorKind kind, const vec &feet, float yaw, int team)
    {
        if(halocollisionactive()) return haloscenariospawnenemy(kind, feet, yaw, -1, team) != nullptr;
        const int x = int(feet.x), y = int(feet.y);
        if(halocinematic() || editmode || OUTBORD(x, y) || SOLID(S(x, y))) return false;
        return spawnEnemy(kind, vec(feet).add(vec(0, 0, profiles[kind].eyeheight)), yaw, -1, -1, team) != nullptr;
    }

    // Spawns `count` actors of a kind in a ring around a point, as many as fit.
    int spawnRing(ActorKind kind, const vec &feet, float yaw, int count, int team)
    {
        int placed = 0;
        for(int attempt = 0; attempt < count * 4 && placed < count; ++attempt)
        {
            const float angle = (attempt * 360.0f / max(count, 1) + rnd(30)) * RAD, r = count > 1 ? 1.0f + rndscale(1.8f) : 0.0f;
            const vec at = vec(feet).add(vec(cosf(angle) * r, sinf(angle) * r, 0));
            if(spawnNear(kind, at, yaw + rnd(360), team)) ++placed;
        }
        return placed;
    }

    void updateFlood()
    {
        // Infection forms touching a body convert it; the form is spent.
        loopv(enemies)
        {
            botent *bot = enemies[i].bot;
            if(!bot || bot->state != CS_ALIVE || enemies[i].kind != INFECTION) continue;
            loopvj(corpses)
            {
                if(fabsf(corpses[j].o.z - feetOf(bot).z) > 4.0f || corpses[j].o.distxy(bot->o) > infectionReach) continue;
                const Corpse &c = corpses[j];
                floodSpawns.add(FloodSpawn{c.o, c.yaw, c.kind == ELITE ? FLOOD_ELITE : FLOOD_HUMAN, 1, lastmillis + infectionDelay});
                conoutf("Halo: infection form takes a %s body", profiles[c.kind].name);
                corpses.remove(j);
                dodamage(INT_MAX, bot, bot, GUN_KNIFE);
                break;
            }
        }
        loopvrev(corpses) if(lastmillis >= corpses[i].expires) corpses.remove(i);
        loopvrev(floodSpawns)
        {
            if(lastmillis < floodSpawns[i].at) continue;
            const FloodSpawn spawn = floodSpawns.remove(i);
            const int placed = spawnRing(spawn.kind, spawn.o, spawn.yaw, spawn.count, HALO_TEAM_FLOOD);
            if(placed) conoutf("Halo: flood %s rises", profiles[spawn.kind].name);
        }
    }

    void showPickup(const char *text)
    {
        copystring(pickupText, text);
        pickupMessage = lastmillis;
    }

    void dropWeapon(const HaloWeaponState &state, const vec &feet, float yaw)
    {
        const HaloWeaponDef *def = haloweapon(state.def);
        if(!def || !def->playable() || def->worldmodel == "-") return;
        GroundWeapon &g = groundWeapons.add();
        g.o = feet;
        g.yaw = yaw;
        g.state = state;
        g.state.zoom = 0;
        g.state.charged = false;
        g.state.chargeStart = 0;
    }

    // The weapon's firing loop (weapons.txt `sound loop`): the flamethrower's fire while the trigger
    // is held and it can fire, the plasma pistol's hum once a charge has built for a moment.
    void updateFiringLoop()
    {
        const char *sample = NULL;
        bool active = false;
        if(tagWeapons() && !halocinematic() && !halodriving() && player1->state == CS_ALIVE && stage != COMPLETE && !ispaused)
        {
            const HaloWeaponState &st = carried[currentCarried];
            const HaloWeaponDef *d = haloweapon(st.def);
            const HaloTriggerDef *t = d ? d->trigger() : NULL;
            sample = d ? d->sound(HALO_SOUND_LOOP) : NULL;
            if(sample && t)
            {
                if(t->charge > 0) active = st.chargeStart && lastmillis - st.chargeStart > 150;
                else active = player1->attacking && st.canfire(*d) && !player1->weaponsel->reloading &&
                              (d->usesBattery() || player1->weaponsel->mag > 0);
            }
        }
        haloloopsound(sample, active);
    }

    void updateWeapons(int elapsed)
    {
        if(!tagWeapons()) return;
        syncCarried();
        const int selected = player1->weaponsel->type;
        if(selected == carriedGun[0] || selected == carriedGun[1]) currentCarried = selected == carriedGun[1];
        loopi(2)
        {
            const HaloWeaponDef *d = haloweapon(carried[i].def);
            if(!d) continue;
            carried[i].tick(*d, elapsed);
            if(d->usesBattery()) player1->mag[carriedGun[i]] = carried[i].canfire(*d) ? 1 : 0;
            const HaloTriggerDef *t = d->trigger();
            if(t) player1->weapons[carriedGun[i]]->info.attackdelay = carried[i].delay(*t);
            // Holding the trigger on a charging weapon builds its second-trigger shot.
            if(i == currentCarried && carried[i].chargeStart && t && t->charge > 0 && d->trigger(1))
                carried[i].charged = lastmillis - carried[i].chargeStart >= int(t->charge * 1000);
        }
        loopv(enemies) if(enemies[i].bot && haloweapon(enemies[i].weapon.def))
            enemies[i].weapon.tick(*haloweapon(enemies[i].weapon.def), elapsed);

        nearbyWeapon = -1;
        float best = 1e9f;
        const vec feet = feetOf(player1);
        loopv(groundWeapons)
        {
            GroundWeapon &g = groundWeapons[i];
            const HaloWeaponDef *d = haloweapon(g.state.def);
            const float distance = feet.dist(g.o);
            if(!d || distance > 5.0f) continue;
            int holder = carried[0].def == g.state.def ? 0 : carried[1].def == g.state.def ? 1 : -1;
            if(holder < 0)
            {
                if(distance < best) { best = distance; nearbyWeapon = i; }
                continue;
            }
            // Walking over a weapon you already carry takes its rounds.
            const HaloMagazineDef *m = d->magazine();
            if(d->usesBattery() || !m) continue;
            HaloWeaponState &c = carried[holder];
            const int room = max(0, m->maximum - c.loaded - c.reserve);
            const int take = min(room, g.state.loaded + g.state.reserve);
            if(take <= 0) continue;
            c.reserve += take;
            const int fromReserve = min(take, g.state.reserve);
            g.state.reserve -= fromReserve;
            g.state.loaded -= take - fromReserve;
            player1->ammo[carriedGun[holder]] = c.reserve;
            haloplaysound(d->sound(HALO_SOUND_PICKUP), S_ITEMAMMO);
            defformatstring(text)("%s ammo", d->name.c_str());
            showPickup(text);
            if(g.state.loaded + g.state.reserve <= 0) groundWeapons.remove(i--);
        }
    }

    bool swapWeapon()
    {
        if(!tagWeapons() || !groundWeapons.inrange(nearbyWeapon) || player1->weaponsel->reloading || player1->weaponchanging) return false;
        GroundWeapon picked = groundWeapons.remove(nearbyWeapon);
        nearbyWeapon = -1;
        syncCarried();
        // An empty slot takes the weapon (and is selected), as in Halo: the selected one first,
        // else the other; with both armed the selected weapon is dropped for it.
        if(slotArmed(currentCarried) && !slotArmed(currentCarried ^ 1)) currentCarried ^= 1;
        else dropWeapon(carried[currentCarried], feetOf(player1), player1->yaw);
        carried[currentCarried] = picked.state;
        const int slot = carriedGun[currentCarried];
        equipCarried(false);
        const HaloWeaponDef *d = haloweapon(picked.state.def);
        // A new AssaultCube slot readies the weapon with its own sound (weapon::onselecting);
        // a weapon taking over the same slot plays it here.
        if(carriedGun[currentCarried] == slot) haloplaysound(d ? d->sound(HALO_SOUND_READY) : NULL, S_ITEMAMMO);
        showPickup(d ? d->name.c_str() : "Weapon");
        return true;
    }

    // How far the weapon the player would pick up lies from their feet; a vehicle seat nearer than it wins E.
    float nearbyWeaponDistance()
    {
        return groundWeapons.inrange(nearbyWeapon) ? feetOf(player1).dist(groundWeapons[nearbyWeapon].o) : 1e9f;
    }

    void dropItem(const HaloEquipmentDef *d, const vec &feet, float yaw)
    {
        if(!d) return;
        GroundItem &item = groundItems.add();
        item.o = feet;
        item.yaw = yaw;
        item.def = d->index;
        item.object = -1;
        item.flags = 0;
        item.exists = item.created = true;
    }

    // Equipment the player walks over is taken when it does them any good, as in Halo.
    bool takeItem(const HaloEquipmentDef &d)
    {
        switch(d.powerup)
        {
            case HALO_POWERUP_NONE:
            {
                // Ammunition for a carried weapon, up to the weapon's tag maximum.
                if(!tagWeapons()) return false;
                syncCarried();
                loopi(2)
                {
                    const HaloWeaponDef *w = haloweapon(carried[i].def);
                    const HaloMagazineDef *m = w ? w->magazine() : NULL;
                    const int rounds = w ? d.roundsFor(w->index) : 0;
                    if(!m || rounds <= 0) continue;
                    const int room = m->maximum - carried[i].loaded - carried[i].reserve;
                    if(room <= 0) continue;
                    carried[i].reserve += min(room, rounds);
                    player1->ammo[carriedGun[i]] = carried[i].reserve;
                    defformatstring(text)("%s ammo", w->name.c_str());
                    showPickup(text);
                    return true;
                }
                return false;
            }
            case HALO_POWERUP_HEALTH:
                if(player1->health >= playerHealth) return false;
                player1->health = playerHealth;
                showPickup("Health pack");
                return true;
            case HALO_POWERUP_GRENADE:
            {
                if(!grenadeKnown(d.grenade)) return false;
                int &count = grenadeCount(d.grenade);
                if(count >= grenadeMaximum(d.grenade)) return false;
                ++count;
                // With none of the selected type left, the pickup becomes the selection.
                if(grenadeCount(grenadeType) <= 0) grenadeType = d.grenade;
                showPickup(d.grenade == HALO_GRENADE_PLASMA ? "Plasma grenade" : "Fragmentation grenade");
                return true;
            }
            case HALO_POWERUP_OVERSHIELD:
                if(shieldCap <= 0 || (overshieldTarget > 0 && overshield >= overshieldTarget * .99f)) return false;
                shield = shieldCap;
                overshieldTarget = 2 * shieldCap;
                overshieldCharged = lastmillis + overshieldCharge;
                overshieldDrain = max(1000, int(d.powerupTime * 1000));
                showPickup("Overshield");
                return true;
            case HALO_POWERUP_CAMO:
                camoUntil = lastmillis + max(1000, int(d.powerupTime * 1000));
                showPickup("Active camouflage");
                return true;
            // Double speed and full-spectrum vision are not campaign pickups.
            default: return false;
        }
    }

    void updateItems(int elapsed)
    {
        const vec feet = feetOf(player1);
        // A seated player rides over equipment without taking it, as in Halo.
        if(!halodriving()) loopv(groundItems)
        {
            GroundItem &item = groundItems[i];
            const HaloEquipmentDef *d = haloequipment(item.def);
            if(!item.exists || !d || feet.dist(item.o) > 2.5f || !takeItem(*d)) continue;
            item.exists = false;
            haloplaysound(d->sound(HALO_SOUND_PICKUP), S_ITEMAMMO);
        }
        if(overshieldTarget > 0)
        {
            if(lastmillis < overshieldCharged) overshield = min(overshieldTarget, overshield + overshieldTarget * elapsed / overshieldCharge);
            else overshield -= overshieldTarget * elapsed / float(overshieldDrain);
            if(overshield <= 0 && lastmillis >= overshieldCharged) overshield = overshieldTarget = 0;
        }
        // The retail warnings: the low-shield alarm while the shield is nearly gone and not yet
        // recharging, the heartbeat while health is low with no shield left.
        const bool lowShield = shieldCap > 0 && shield + overshield < shieldCap * .25f && !shieldRecharging;
        if(lowShield && lastmillis - lowShieldSound >= 3700)
        {
            lowShieldSound = lastmillis;
            haloplaysound(halointerfacesound("shield-low"), S_NULL);
        }
        if(shield <= 0 && player1->health * 3 <= playerHealth && lastmillis - lowHealthSound >= 1750)
        {
            lowHealthSound = lastmillis;
            haloplaysound(halointerfacesound("health-low"), S_NULL);
        }
    }

    void switchGrenade()
    {
        if(!haloactive() || halocinematic() || halodriving() || player1->state != CS_ALIVE) return;
        const int next = grenadeType ^ 1;
        if(!grenadeKnown(next) || grenadeCount(next) <= 0) return;
        grenadeType = next;
        audiomgr.playsound(S_GUNCHANGE, SP_HIGH);
    }

    // Campaign progress, config/halo-progress.txt in the profile. Version 2 is line based:
    //   halo-progress 2
    //   furthest <mission> <section>          the furthest section reached (unlocks missions)
    //   beaten <0|1>
    //   game <mission> <section> <difficulty> <checkpoint 0|1> <escape ms>
    //   checkpoint <x> <y> <z> <yaw> <stage> <marker>
    //   vitals <health> <shield>
    //   grenades <frag> <plasma> <selected type>
    //   weapon <slot> <loaded> <reserve> <battery> <weapon tag path>
    //   empty <slot>                          a slot with no weapon (a starting profile's: a10 begins unarmed);
    //                                         a slot with neither line (older saves) gets the default weapon
    //   current <slot>
    //   stock <weapon> <assault mag> <assault ammo> <pistol mag> <pistol ammo>
    //   state <format> <mission> <section> <checkpoint 0|1> <.hscb hash> <section map hash> <records> <records' FNV-1a>
    //   s ... / v ... / w ... / p ...           version 3: the state block's records (see below)
    //   check <FNV-1a of everything above, hex>
    // The game lines are present only while there is a game to continue. Version 2 files (no
    // state block) and version 1 ("1 m s [1]") still load. Each save first moves a valid file to
    // halo-progress.txt.bak; a file that fails its check is moved to halo-progress.txt.corrupt
    // and the backup restored, with a warning.
    //
    // The state block makes Continue resume the mission script where the checkpoint left it,
    // instead of starting it afresh on the section: "s" records are the script's state
    // (halo_script.cpp), "v" the vehicles (halo_vehicle.cpp), "w" the weapons and equipment on the
    // ground and the stock items, "p" the script's limits on the player and the powerups. At a
    // checkpoint it holds what the in-memory checkpoint keeps for a revert; at a section's start,
    // only when the script was carried in from the section before (its "s" and "p" records).
    // Continue loads it into the in-memory checkpoint and reverts to it, as a death would. When
    // it does not fit (another .hscb, a damaged block, a record that names nothing) the script
    // restarts on the section as before, with a warning in the log and the menus; when only the
    // section map changed, the script's state is kept and the vehicles and ground items return to
    // their placements.
    const int progressVersion = 3, stateFormat = 1;
    const size_t maxProgressSize = 32u << 20;

    struct ProgressData
    {
        int furthestMission = 0, furthestSection = 0;
        bool beaten = false;
        SavedGame game;
    };

    unsigned progressHash(const char *text, size_t length)
    {
        unsigned hash = 2166136261u;
        loopi(int(length)) { hash ^= uchar(text[i]); hash *= 16777619u; }
        return hash;
    }

    bool validSection(int m, int s) { return m >= 0 && m < haloMissionCount && s >= 0 && s < haloMissions[m].sections; }

    bool parseProgress(const char *text, ProgressData &out)
    {
        out = ProgressData();
        int version = 0, m = 0, sec = 0, beaten = 0;
        if(strncmp(text, "halo-progress ", 14))
        {
            // Version 1: "1 <mission> <section> [1]".
            const int fields = sscanf(text, "%d %d %d %d", &version, &m, &sec, &beaten);
            if(fields < 3 || version != 1 || !validSection(m, sec)) return false;
            out.furthestMission = m;
            out.furthestSection = sec;
            out.beaten = fields == 4 && beaten == 1;
            if(!out.beaten)
            {
                // The section reached is where Continue starts, from its beginning.
                out.game.valid = true;
                out.game.mission = m;
                out.game.section = sec;
                out.game.difficulty = halodifficulty();
            }
            return true;
        }
        if(sscanf(text, "halo-progress %d", &version) != 1 || version < 2 || version > progressVersion) return false;
        const char *check = strstr(text, "\ncheck ");
        unsigned stored = 0;
        if(!check || sscanf(check + 7, "%x", &stored) != 1 || stored != progressHash(text, check - text + 1)) return false;
        bool furthest = false;
        int slot = 0;
        for(const char *line = text; line && line < check + 1; line = strchr(line, '\n') ? strchr(line, '\n') + 1 : NULL)
        {
            SavedGame &g = out.game;
            Loadout &l = g.loadout;
            int a = 0, b = 0, c = 0, d = 0, e = 0;
            float x = 0, y = 0, z = 0, w = 0;
            // The state block's lines are kept verbatim; Continue checks them (restoreSavedState).
            if(!strncmp(line, "state ", 6) || (strchr("svwp", line[0]) && line[0] && line[1] == ' '))
            {
                const char *eol = strchr(line, '\n');
                g.state.append(line, eol ? size_t(eol - line + 1) : strlen(line));
            }
            else if(sscanf(line, "furthest %d %d", &a, &b) == 2) { furthest = validSection(a, b); out.furthestMission = a; out.furthestSection = b; }
            else if(sscanf(line, "beaten %d", &a) == 1) out.beaten = a == 1;
            else if(sscanf(line, "game %d %d %d %d %d", &a, &b, &c, &d, &e) == 5)
            {
                if(!validSection(a, b) || c < 0 || c > 3) return false;
                g.valid = l.valid = true;
                g.mission = a; g.section = b; g.difficulty = c; g.checkpoint = d != 0; g.escapeMillis = max(e, 0);
            }
            else if(sscanf(line, "checkpoint %f %f %f %f %d %d", &x, &y, &z, &w, &a, &b) == 6)
            {
                // sscanf reads "nan" and "inf": a checkpoint there would put the player nowhere.
                if(!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z) || !std::isfinite(w) ||
                   fabsf(x) > 1e6f || fabsf(y) > 1e6f || fabsf(z) > 1e6f) return false;
                g.position = vec(x, y, z); g.yaw = w; g.stage = clamp(a, 0, 4); g.marker = max(b, 0);
            }
            else if(sscanf(line, "vitals %d %f", &a, &x) == 2 && std::isfinite(x)) { l.health = clamp(a, 1, playerHealth); l.shield = clamp(x, 0.0f, float(playerShield)); }
            else if(sscanf(line, "grenades %d %d %d", &a, &b, &c) == 3) { l.frags = clamp(a, 0, 9); l.plasmas = clamp(b, 0, 9); l.grenadeType = c == HALO_GRENADE_PLASMA ? c : HALO_GRENADE_FRAG; }
            else if(sscanf(line, "current %d", &a) == 1) l.current = a ? 1 : 0;
            else if(sscanf(line, "empty %d", &a) == 1 && a >= 0 && a <= 1) g.slotEmpty[a] = true;
            else if(sscanf(line, "stock %d %d %d %d %d", &a, &b, &c, &d, &e) == 5)
            {
                l.weapon = a == GUN_PISTOL ? GUN_PISTOL : GUN_ASSAULT;
                l.mag[0] = max(b, 0); l.ammo[0] = max(c, 0); l.mag[1] = max(d, 0); l.ammo[1] = max(e, 0);
            }
            else if(!strncmp(line, "weapon ", 7) && slot < 2)
            {
                int consumed = 0;
                if(sscanf(line, "weapon %d %d %d %f %n", &a, &b, &c, &x, &consumed) < 4 || consumed <= 0 || a < 0 || a > 1) return false;
                HaloWeaponState &st = l.carried[a];
                st = HaloWeaponState();
                st.loaded = max(b, 0);
                st.reserve = max(c, 0);
                st.battery = std::isfinite(x) ? clamp(x, 0.0f, 1.0f) : 1.0f;
                const char *path = line + consumed, *eol = strchr(path, '\n');
                copystring(g.weaponPath[a], path, min(size_t(MAXSTRLEN), size_t((eol ? eol : path + strlen(path)) - path + 1)));
                ++slot;
            }
        }
        if(!furthest) return false;
        if(out.game.valid && out.game.checkpoint && out.game.position.iszero()) out.game.checkpoint = false;
        if(!out.game.valid) out.game.state.clear();
        return true;
    }

    // 1: read and valid, 0: no file, -1: there but unreadable or failing its check.
    int readProgress(const char *path, ProgressData &out)
    {
        FILE *file = fopen(path, "rb");
        if(!file) return 0;
        // Version 3 files carry the script's state: a few hundred kilobytes.
        std::string text;
        char chunk[65536];
        size_t length;
        while((length = fread(chunk, 1, sizeof(chunk), file)) > 0 && text.size() <= maxProgressSize) text.append(chunk, length);
        const bool failed = ferror(file) != 0 || text.size() > maxProgressSize;
        fclose(file);
        if(failed || text.empty() || strlen(text.c_str()) != text.size()) return -1;
        return parseProgress(text.c_str(), out) ? 1 : -1;
    }

    void applyProgress(const ProgressData &d)
    {
        savedMission = d.furthestMission;
        savedSection = d.furthestSection;
        campaignBeaten = d.beaten;
        savedGame = d.game;
    }

    void saveProgress();

    void loadProgress()
    {
        if(progressLoaded) return;
        progressLoaded = true;
        progressNote[0] = '\0';
        string mainPath, homePath, backup, broken, temporary;
        copystring(mainPath, findfile(progressFile, "rb"));
        copystring(homePath, findfile(progressFile, "wb"));
        formatstring(backup)("%s.bak", homePath);
        formatstring(broken)("%s.corrupt", homePath);
        formatstring(temporary)("%s.tmp", homePath);
        ProgressData d;
        const int state = readProgress(mainPath, d);
        if(state > 0) { applyProgress(d); return; }
        if(state == 0)
        {
            // A save interrupted between its two renames leaves only the finished temporary file.
            if(readProgress(temporary, d) > 0 && rename(temporary, homePath) == 0)
            {
                applyProgress(d);
                conoutf("Halo: campaign progress recovered from an interrupted save");
            }
            return;
        }
        // The file is there but damaged: keep it aside and fall back to the backup.
        rename(mainPath, broken);
        if(readProgress(backup, d) > 0)
        {
            applyProgress(d);
            copystring(progressNote, "Campaign progress was corrupt and has been restored from the backup");
            conoutf("\f3Halo: campaign progress was corrupt (kept as %s); restored from the backup", broken);
            saveProgress();
        }
        else
        {
            copystring(progressNote, "Campaign progress was corrupt and no backup could be read; progress was reset");
            conoutf("\f3Halo: campaign progress was corrupt (kept as %s) and the backup is unusable; progress reset", broken);
        }
    }

    void appendf(std::string &out, const char *fmt, ...) PRINTFARGS(2, 3);
    void appendf(std::string &out, const char *fmt, ...)
    {
        char line[600];
        va_list args;
        va_start(args, fmt);
        vsnprintf(line, sizeof(line), fmt, args);
        va_end(args);
        out += line;
    }

    void saveProgress()
    {
        loadProgress();
        std::string text;
        appendf(text, "halo-progress %d\nfurthest %d %d\nbeaten %d\n", progressVersion, savedMission, savedSection, campaignBeaten ? 1 : 0);
        const SavedGame &g = savedGame;
        if(g.valid)
        {
            const Loadout &l = g.loadout;
            appendf(text, "game %d %d %d %d %d\n", g.mission, g.section, g.difficulty, g.checkpoint ? 1 : 0, g.escapeMillis);
            if(g.checkpoint) appendf(text, "checkpoint %.4f %.4f %.4f %.3f %d %d\n", g.position.x, g.position.y, g.position.z, g.yaw, g.stage, g.marker);
            appendf(text, "vitals %d %.3f\ngrenades %d %d %d\n", l.health, l.shield, l.frags, l.plasmas, l.grenadeType);
            loopi(2)
            {
                const HaloWeaponDef *d = haloweapon(l.carried[i].def);
                if(d && !strchr(d->path.c_str(), '\n'))
                    appendf(text, "weapon %d %d %d %.4f %s\n", i, l.carried[i].loaded, l.carried[i].reserve, l.carried[i].battery, d->path.c_str());
                else if(!d && tagWeapons()) appendf(text, "empty %d\n", i);
            }
            appendf(text, "current %d\nstock %d %d %d %d %d\n", l.current, l.weapon, l.mag[0], l.ammo[0], l.mag[1], l.ammo[1]);
            text += g.state;
        }
        appendf(text, "check %08x\n", progressHash(text.c_str(), text.length()));

        string destination, temporary, backup;
        copystring(destination, findfile(progressFile, "wb"));
        formatstring(temporary)("%s.tmp", destination);
        formatstring(backup)("%s.bak", destination);
        FILE *file = fopen(temporary, "wb");
        if(!file) { conoutf("Halo: unable to open campaign progress"); return; }
        const bool written = fwrite(text.data(), 1, text.length(), file) == text.length();
        if(fclose(file) != 0 || !written)
        {
            remove(temporary);
            conoutf("Halo: unable to save campaign progress");
            return;
        }
        // Keep the last good file as the backup; a damaged one never replaces it.
        ProgressData previous;
        const bool backedUp = readProgress(destination, previous) > 0 && rename(destination, backup) == 0;
        if(rename(temporary, destination) != 0)
        {
            if(backedUp) rename(backup, destination);
            remove(temporary);
            conoutf("Halo: unable to save campaign progress");
        }
    }

    void beginEnding()
    {
        if(ending || preview) return;
        ending = true;
        endingDone = winPending = false;
        endingStart = lastmillis;
        campaignBeaten = true;
        savedMission = mission;
        savedSection = section;
        // The campaign is over: nothing to continue, every mission is unlocked.
        savedGame.valid = false;
        saveProgress();
        player1->attacking = false;
        player1->stopmoving();
        conoutf("Halo ending reached: %s complete", haloMissions[mission].title);
    }

    // The title menu (halo_title): on launch, from the pause menu (the campaign stays paused
    // under it) and after the credits.
    void showtitle()
    {
        loadProgress();
        if(!controlsLoaded) controlsLoaded = execfile("config/halo.cfg");
        titleScreen = true;
        if(haloactive())
        {
            player1->attacking = false;
            player1->stopmoving();
        }
        closemenu(NULL);
        showmenu("Halo");
    }

    // Credits are over (or skipped): leave the finished campaign at the title menu.
    void finishEnding()
    {
        if(!ending || endingDone) return;
        endingDone = true;
        conoutf("Halo credits finished");
        // Back to the title menu, where New Game and Mission select (every mission now) start again.
        showtitle();
    }

    bool installed(int m, int s)
    {
        string name;
        halosectionname(m, s, name, sizeof(name));
        defformatstring(path)("packages/maps/%s.cgz", name);
        return fileexists(findfile(path, "rb"), "rb");
    }

    void queueSection(int m, int s, bool standalone = false)
    {
        if(multiplayer("halo_campaign")) return;
        if(!installed(m, s))
        {
            conoutf("Halo: import %s, section %d to continue", haloMissions[m].title, s + 1);
            return;
        }
        closemenu(NULL);
        incomingEscapeTime = 0;
        pendingMission = m;
        pendingSection = s;
        pendingPreview = standalone;
        pendingContinuation = false;
        launchPending = true;
    }

    void nextSection()
    {
        if(!haloactive() || stage != COMPLETE || preview || campaignFinished) return;
        int m = mission, s = section + 1;
        if(s == haloMissions[m].sections) { ++m; s = 0; }
        if(m >= haloMissionCount) return;
        int remainingTime = escaping ? max(1, escapeDeadline - lastmillis) : 0;
        // Within a mission the player keeps what they carry into the next section.
        Loadout carry;
        captureLoadout(carry);
        queueSection(m, s);
        if(launchPending && m == mission)
        {
            incomingEscapeTime = remainingTime;
            pendingContinuation = true;
            carryLoadout = carry;
        }
    }

    // Saves the game Continue resumes: this section, with the checkpoint just taken or (at the
    // section's start) none, the loadout, the difficulty and the escape countdown.
    // FNV-1a of the section's map file (the state block is only good for the map it was saved on).
    unsigned sectionMapHash()
    {
        static string cachedName = "";
        static unsigned cachedHash = 0;
        string name;
        halosectionname(mission, section, name, sizeof(name));
        if(strcmp(name, cachedName))
        {
            defformatstring(file)("packages/maps/%s.cgz", name);
            cachedHash = halofilehash(file);
            copystring(cachedName, name);
        }
        return cachedHash;
    }

    void writeWeaponState(std::string &out, const HaloWeaponState &w)
    {
        const HaloWeaponDef *d = haloweapon(w.def);
        halostateword(out, d ? d->path : std::string());
        halostatef(out, " %d %d %.9g %.9g %.9g %.9g %d", w.loaded, w.reserve, w.battery, w.heat, w.error, w.rate, w.overheated);
    }

    bool readWeaponState(HaloStateReader &r, HaloWeaponState &w)
    {
        const std::string path = r.word();
        w = HaloWeaponState();
        w.loaded = r.i(); w.reserve = r.i(); w.battery = r.f(); w.heat = r.f(); w.error = r.f(); w.rate = r.f(); w.overheated = r.b();
        const HaloWeaponDef *d = path.empty() ? nullptr : haloweaponfind(path.c_str());
        w.def = d ? d->index : -1;
        return r.ok;
    }

    // The "p" records: what the scripts set on the player (unit_set_maximum_vitality, unit_impervious),
    // which a revert keeps, and the powerups running now (the time left on each).
    void writePlayerState(std::string &out)
    {
        halostatef(out, "p vitality %.9g %.9g %.9g %d\n", maxVitalityHealth, maxVitalityShield, shieldCap, playerProtected);
        halostatef(out, "p powerup %.9g %.9g %d %d %d\n", overshield, overshieldTarget, max(0, overshieldCharged - lastmillis),
                   overshieldDrain, max(0, camoUntil - lastmillis));
    }

    // The "w" records: the in-memory checkpoint's weapons and equipment on the ground and the
    // stock items' spawned flags (halo.cpp takeCheckpoint), for the section map they belong to.
    void writeWorldState(std::string &out)
    {
        loopv(checkpointGround)
        {
            const GroundWeapon &g = checkpointGround[i];
            halostatef(out, "w weapon %.9g %.9g %.9g %.9g", g.o.x, g.o.y, g.o.z, g.yaw);
            writeWeaponState(out, g.state);
            out += '\n';
        }
        loopv(checkpointItemsHalo)
        {
            const GroundItem &item = checkpointItemsHalo[i];
            const HaloEquipmentDef *d = haloequipment(item.def);
            halostatef(out, "w item %.9g %.9g %.9g %.9g", item.o.x, item.o.y, item.o.z, item.yaw);
            halostateword(out, d ? d->path : std::string());
            halostatef(out, " %d", item.object);
            halostateword(out, haloscenarioobjectname(item.object));
            halostatef(out, " %d %d %d\n", item.flags, item.exists, item.created);
        }
        halostatef(out, "w items %d =", checkpointItems.length());
        loopv(checkpointItems) out += checkpointItems[i] ? '1' : '0';
        out += '\n';
    }

    // Puts the state block on the saved game: at a checkpoint, what the in-memory checkpoint
    // keeps; at a section's start, the script carried into it. None during a cutscene, or when
    // the section's script starts afresh anyway.
    void buildSavedState(SavedGame &g)
    {
        g.state.clear();
        if(halocinematic() || (!g.checkpoint && !scriptCarried)) return;
        std::string body;
        if(!haloscenariostatesave(body, !g.checkpoint)) return;
        writePlayerState(body);
        if(g.checkpoint)
        {
            writeWorldState(body);
            halovehiclesstatesave(body);
        }
        int records = 0;
        for(char c : body) records += c == '\n';
        halostatef(g.state, "state %d %d %d %d %08x %08x %d %08x\n", stateFormat, mission, section, g.checkpoint ? 1 : 0,
                   haloscenarioprogramhash(), sectionMapHash(), records, halostatehash(body.data(), body.size()));
        g.state += body;
    }

    void recordProgress(bool atCheckpointNow)
    {
        if(preview || ending || !haloactive()) return;
        loadProgress();
        SavedGame &g = savedGame;
        g.valid = true;
        g.mission = mission;
        g.section = section;
        g.difficulty = halodifficulty();
        g.checkpoint = atCheckpointNow && atCheckpoint;
        if(g.checkpoint)
        {
            g.position = checkpointPosition;
            g.yaw = checkpointYaw;
            g.stage = checkpointStage;
            g.marker = checkpointMarker;
        }
        g.escapeMillis = !g.checkpoint && escaping ? max(0, escapeDeadline - lastmillis) : 0;
        captureLoadout(g.loadout);
        loopi(2)
        {
            const HaloWeaponDef *d = haloweapon(g.loadout.carried[i].def);
            copystring(g.weaponPath[i], d ? d->path.c_str() : "");
            g.slotEmpty[i] = !d && tagWeapons();
        }
        buildSavedState(g);
        saveProgress();
        if(halo_scripttrace)
            conoutf("Halo: game saved on %s, section %d (%s%s)", haloMissions[mission].title, section + 1,
                    g.checkpoint ? "checkpoint" : "section start", g.state.empty() ? "" : ", with the mission script");
    }

    // Continue: once the saved section has started, put the player back at the saved checkpoint
    // (the loadout is already on them). The mission script starts afresh on the section, as it
    // does for a section started on its own; its opening scene is skipped. The restored spot is
    // taken as the checkpoint, so a death reverts there instead of restarting the script.
    void restoreSavedCheckpoint()
    {
        const SavedGame &g = savedGame;
        if(!g.checkpoint) return;
        if(halocinematic()) halocinematicskip();
        player1->o = g.position;
        player1->yaw = g.yaw;
        player1->pitch = 0;
        player1->vel = vec(0, 0, 0);
        entinmap(player1);
        player1->resetinterp();
        haloplayermoved("continue");
        if(!escaping && g.stage != COMPLETE)
        {
            setStage(MissionStage(g.stage));
            nextRouteMarker = g.marker;
        }
        takeCheckpoint(true);
        conoutf("Halo: continued from the checkpoint on %s, section %d", haloMissions[mission].title, section + 1);
    }

    // The state block split into its records (without their "x " prefixes).
    struct StateBlock
    {
        int format = 0, mission = -1, section = -1, checkpoint = 0;
        unsigned script = 0, map = 0;
        std::vector<std::string> s, v, w, p;
    };

    bool splitState(const std::string &text, StateBlock &b)
    {
        const char *header = text.c_str();
        unsigned hash = 0;
        int count = -1;
        if(sscanf(header, "state %d %d %d %d %x %x %d %x", &b.format, &b.mission, &b.section, &b.checkpoint, &b.script, &b.map,
                  &count, &hash) != 8 || b.format != stateFormat) return false;
        const char *body = strchr(header, '\n');
        if(!body) return false;
        ++body;
        if(halostatehash(body, text.size() - size_t(body - header)) != hash) return false;
        int lines = 0;
        for(const char *line = body; *line; ++lines)
        {
            const char *eol = strchr(line, '\n');
            if(!eol || eol - line < 2 || line[1] != ' ') return false;
            std::string record(line + 2, eol);
            switch(line[0])
            {
                case 's': b.s.push_back(record); break;
                case 'v': b.v.push_back(record); break;
                case 'w': b.w.push_back(record); break;
                case 'p': b.p.push_back(record); break;
                default: return false;
            }
            line = eol + 1;
        }
        return lines == count;
    }

    struct SavedPlayer
    {
        float maxHealth = 100, maxShield = 75, cap = playerShield;
        bool protectedPlayer = false;
        float overshield = 0, overshieldTarget = 0;
        int chargeLeft = 0, drain = 60000, camoLeft = 0;
    };

    bool parsePlayerState(const std::vector<std::string> &records, SavedPlayer &p)
    {
        bool vitality = false, powerup = false;
        for(const std::string &record : records)
        {
            HaloStateReader r(record.c_str());
            if(r.key("vitality"))
            {
                p.maxHealth = r.f(); p.maxShield = r.f(); p.cap = r.f(); p.protectedPlayer = r.b();
                vitality = r.ok && p.maxHealth >= 1 && p.maxShield >= 0 && p.cap >= 0 && p.cap <= playerShield;
            }
            else if(r.key("powerup"))
            {
                p.overshield = r.f(); p.overshieldTarget = r.f(); p.chargeLeft = r.i(); p.drain = r.i(); p.camoLeft = r.i();
                powerup = r.ok && p.overshield >= 0 && p.overshieldTarget >= 0 && p.chargeLeft >= 0 && p.drain > 0 && p.camoLeft >= 0;
            }
            else return false;
            if(!r.ok || r.more()) return false;
        }
        return vitality && powerup;
    }

    struct SavedWorld
    {
        vector<GroundWeapon> weapons;
        vector<GroundItem> items;
        vector<uchar> spawned;
    };

    bool parseWorldState(const std::vector<std::string> &records, SavedWorld &w, std::string &why)
    {
        bool spawned = false;
        for(const std::string &record : records)
        {
            HaloStateReader r(record.c_str());
            if(r.key("weapon"))
            {
                GroundWeapon g;
                g.o.x = r.f(); g.o.y = r.f(); g.o.z = r.f(); g.yaw = r.f();
                if(!readWeaponState(r, g.state)) { why = "a ground weapon record is damaged"; return false; }
                // A weapon the table no longer has is left out.
                if(haloweapon(g.state.def)) w.weapons.add(g);
            }
            else if(r.key("item"))
            {
                GroundItem item;
                item.o.x = r.f(); item.o.y = r.f(); item.o.z = r.f(); item.yaw = r.f();
                const std::string path = r.word();
                item.object = r.i();
                const std::string name = r.word();
                item.flags = r.i(); item.exists = r.b(); item.created = r.b();
                if(!r.ok) { why = "an equipment record is damaged"; return false; }
                if(name != haloscenarioobjectname(item.object)) { why = "an equipment record names another scenario object"; return false; }
                const HaloEquipmentDef *d = haloequipmentfind(path.c_str());
                if(!d) continue;
                item.def = d->index;
                w.items.add(item);
            }
            else if(r.key("items"))
            {
                const int count = r.i();
                const std::string bits = r.word();
                if(!r.ok || count != ents.length() || int(bits.size()) != count) { why = "the stock items do not match the section map"; return false; }
                loopi(count) w.spawned.add(bits[i] == '1');
                spawned = true;
            }
            else { why = "unknown ground record"; return false; }
            if(!r.ok || r.more()) { why = "malformed ground record"; return false; }
        }
        if(!spawned) { why = "the stock items are missing"; return false; }
        return true;
    }

    void warnState(const char *what, const std::string &why)
    {
        formatstring(progressNote)("%s (%s)", what, why.c_str());
        conoutf("\f3Halo: %s", progressNote);
    }

    // Continue with a version 3 state block: the saved checkpoint (or the section's start with
    // the script carried in) becomes the in-memory checkpoint, and the section reverts to it as
    // it does after a death: the mission script resumes with its threads, globals, devices,
    // objects and squads as they were, the vehicles (the player in their seat), the weapons and
    // equipment on the ground and the stock items as they were, and the loadout. Called once
    // the section has started, with the saved loadout already on the player. False (nothing
    // changed, the opening scene perhaps skipped) when the block is missing or does not fit;
    // the caller then falls back to restarting the script.
    bool restoreSavedState()
    {
        const SavedGame &g = savedGame;
        if(g.state.empty())
        {
            conoutf("Halo: the saved game holds no mission script state; the script starts afresh on this section");
            return false;
        }
        StateBlock block;
        std::string why;
        SavedPlayer player;
        if(!splitState(g.state, block)) why = "the saved state is damaged";
        else if(block.mission != mission || block.section != section || (block.checkpoint != 0) != g.checkpoint) why = "the saved state is for another section";
        else if(!haloscenarioprogramhash() || block.script != haloscenarioprogramhash()) why = "the mission scripts were re-imported since the game was saved";
        else if(!parsePlayerState(block.p, player)) why = "the saved player state is damaged";
        else haloscenariostaterestore(block.s, false, why);
        if(!why.empty())
        {
            warnState("The checkpoint's mission script state could not be restored; the script restarted on this section", why);
            return false;
        }
        // The section map: the ground items and vehicles are only good for the map they were saved on.
        SavedWorld world;
        std::string worldWhy, vehicleWhy;
        bool worldOk = g.checkpoint, vehiclesOk = g.checkpoint;
        if(g.checkpoint && block.map != sectionMapHash()) { worldOk = vehiclesOk = false; worldWhy = "the section map was re-imported"; }
        if(worldOk && !parseWorldState(block.w, world, worldWhy)) worldOk = vehiclesOk = false;
        // The opening scene has to end before the script's state goes in (it wakes the threads).
        if(halocinematic()) halocinematicskip();
        if(!haloscenariostaterestore(block.s, true, why))
        {
            warnState("The checkpoint's mission script state could not be restored; the script restarted on this section", why);
            return false;
        }
        if(vehiclesOk && !halovehiclesstaterestore(block.v, vehicleWhy)) vehiclesOk = false;
        maxVitalityHealth = player.maxHealth;
        maxVitalityShield = player.maxShield;
        shieldCap = player.cap;
        playerProtected = player.protectedPlayer;
        if(g.checkpoint)
        {
            // The loadout the section started with is the saved one: it is the checkpoint's.
            checkpointPosition = g.position;
            checkpointYaw = g.yaw;
            checkpointStage = MissionStage(clamp(g.stage, int(INSERTION), int(EXTRACTION)));
            checkpointMarker = g.marker;
            checkpointHealth = player1->health;
            checkpointWeapon = player1->weaponsel->type;
            memcpy(checkpointMag, player1->mag, sizeof(checkpointMag));
            memcpy(checkpointAmmo, player1->ammo, sizeof(checkpointAmmo));
            syncCarried();
            loopi(2) checkpointCarried[i] = carried[i];
            checkpointCurrent = currentCarried;
            checkpointPlasma = plasmaGrenades;
            checkpointGrenadeType = grenadeType;
            checkpointGround.setsize(0);
            checkpointItemsHalo.setsize(0);
            checkpointItems.setsize(0);
            if(worldOk)
            {
                loopv(world.weapons) checkpointGround.add(world.weapons[i]);
                loopv(world.items) checkpointItemsHalo.add(world.items[i]);
                loopv(world.spawned) checkpointItems.add(world.spawned[i]);
            }
            else
            {
                // The section's placements, as the map has them now.
                loopv(groundWeapons) checkpointGround.add(groundWeapons[i]);
                loopv(groundItems) checkpointItemsHalo.add(groundItems[i]);
                loopv(ents) checkpointItems.add(ents[i].spawned);
            }
            atCheckpoint = true;
        }
        // The escape countdown keeps the time it had at the section's start.
        incomingEscapeTime = escaping ? max(1, escapeDeadline - lastmillis) : 0;
        resetEncounter();
        haloscenariorevert();
        halovehiclessync();
        haloequipmentsync();
        resetcamera();
        // Without the saved vehicles the placements (and what the script created) are the checkpoint's.
        if(g.checkpoint && !vehiclesOk) halovehiclescheckpoint();
        if(player.overshieldTarget > 0)
        {
            overshieldTarget = min(player.overshieldTarget, 2 * max(shieldCap, 1.0f));
            overshield = min(player.overshield, overshieldTarget);
            overshieldCharged = lastmillis + min(player.chargeLeft, overshieldCharge);
            overshieldDrain = player.drain;
        }
        if(player.camoLeft > 0) camoUntil = lastmillis + player.camoLeft;
        if(!worldOk && g.checkpoint) warnState("The vehicles and items on the ground went back to their placements", worldWhy);
        else if(!vehiclesOk && g.checkpoint) warnState("The vehicles went back to their placements", vehicleWhy);
        if(g.checkpoint)
            conoutf("Halo: continued from the checkpoint on %s, section %d, with the mission script where it was", haloMissions[mission].title, section + 1);
        else conoutf("Halo: continued %s, section %d, with the mission script carried in from the section before", haloMissions[mission].title, section + 1);
        return true;
    }

    void shieldTick(float &value, float capacity, int hit, int delay, int recharge, int elapsed)
    {
        int regenTime = min(elapsed, lastmillis - hit - delay);
        if(value < capacity && regenTime > 0) value = min(capacity, value + capacity * regenTime / recharge);
    }

    void box(float x, float y, float w, float h, int r, int g, int b, int a)
    {
        glColor4ub(r, g, b, a);
        glBegin(GL_TRIANGLE_STRIP);
        glVertex2f(x, y); glVertex2f(x+w, y);
        glVertex2f(x, y+h); glVertex2f(x+w, y+h);
        glEnd();
    }

    void ring(float x, float y, float radius, int r, int g, int b, int a)
    {
        glColor4ub(r, g, b, a);
        glBegin(GL_LINE_LOOP);
        loopi(40)
        {
            float angle = i * PI2 / 40.0f;
            glVertex2f(x + cosf(angle)*radius, y + sinf(angle)*radius);
        }
        glEnd();
    }

    vec objective()
    {
        vec cartographer;
        bool mapped;
        if(halocartographertarget(cartographer, mapped)) return cartographer;
        if(retailCartographer()) return position(ents[extraction]);
        if(stage == INSERTION || stage == REACH_PASS)
        {
            const vector<int> &markers = route[stage == REACH_PASS];
            if(markers.inrange(nextRouteMarker)) return position(ents[markers[nextRouteMarker]]);
            if(stage == INSERTION) return contactPoint();
            return position(ents[checkpoint]);
        }
        if(stage == EXTRACTION)
        {
            int marker = nextRouteMarker;
            if(escaping) for(int i = 0; i < 2; ++i)
            {
                if(route[i].inrange(marker)) return position(ents[route[i][marker]]);
                marker -= route[i].length();
            }
            if(route[2].inrange(marker)) return position(ents[route[2][marker]]);
            return position(ents[extraction]);
        }
        vec closest = position(ents[stage == FIRST_CONTACT ? checkpoint : extraction]);
        float distance = 1e16f;
        loopv(enemies)
        {
            botent *bot = enemies[i].bot;
            if(!bot || bot->state != CS_ALIVE) continue;
            float d = player1->o.squareddist(bot->o);
            if(d < distance) { closest = bot->o; distance = d; }
        }
        return closest;
    }

    void label(const char *text, int x, int y, float scale = 0.56f, bool alignRight = false)
    {
        glPushMatrix();
        glTranslatef(x, y, 0);
        glScalef(scale, scale, 1);
        draw_text(text, alignRight ? -text_width(text) : 0, 0, 146, 231, 250, 245);
        glPopMatrix();
    }

    HaloHudUnit hudUnit()
    {
        HaloHudUnit u;
        u.shield = clamp(shield / playerShield, 0.0f, 1.0f);
        u.health = clamp(player1->health / float(playerHealth), 0.0f, 1.0f);
        u.grenadeType = grenadeType;
        u.grenades = grenadeCount(grenadeType);
        u.overshield = shieldCap > 0 ? clamp(overshield / shieldCap, 0.0f, 2.0f) : 0;
        u.flashlight = halofxflashlight(&u.flashlightCharge);
        u.hidden = haloscenariohudhidden(&u.blinking);
        return u;
    }

    // The motion sensor's contacts: actors that move or fire, as the retail tracker shows them.
    int sensorBlips(HaloHudBlip *blips, int capacity, float yaw)
    {
        int count = 0;
        const float scale = haloscenarioscale();
        loopv(enemies)
        {
            const botent *bot = enemies[i].bot;
            if(!bot || bot->state != CS_ALIVE || count >= capacity || (bot->vel.sqrxy() < 0.01f && lastmillis - bot->lastaction > 500)) continue;
            vec delta = vec(bot->o).sub(player1->o);
            delta.rotate_around_z(-yaw);
            blips[count++] = HaloHudBlip{delta.x / scale, delta.y / scale, haloteamfriendly(enemies[i].team)};
        }
        return count;
    }

    // In a vehicle seat: the unit panel and sensor, the vehicle, the seat, speed or the gun's heat.
    void drawVehicleHud(bool tagHud, int right, float yaw)
    {
        HaloVehicleStatus st;
        if(!halovehiclestatus(st))
        {
            // Aboard a Pelican (haloscenarioboard): it flies, the player rides.
            if(haloscenarioboarded() && !haloscenarioscriptseated()) label("E  Get out", VIRTW/2-200, VIRTH/2+90, 0.6f);
            return;
        }
        // Without the HUD tags the seat's gun still gets a reticle.
        if(!tagHud && st.weapon >= 0 && !(haloscenariohudhidden(NULL) & HALO_HUD_CROSSHAIR)) halohuddrawreticle(false);
        if(tagHud)
        {
            // The seat's gun has a retail HUD of its own when the HUD tags carry one (a plain
            // reticle when they carry none for it).
            HaloHudWeapon w;
            w.weapon = st.weapon;
            w.battery_weapon = true;
            w.heat = st.heat;
            w.overheated = st.overheated;
            w.firing = st.firing;
            HaloHudBlip blips[64];
            const int count = sensorBlips(blips, 64, yaw);
            halohuddraw(w, hudUnit(), blips, count);
        }
        string title;
        copystring(title, st.name);
        for(char *c = title; *c; ++c) *c = toupper(*c);
        label(title, right, VIRTH-212, 0.60f, true);
        const char *seat = st.role == HALO_SEAT_DRIVER ? "DRIVER" : st.role == HALO_SEAT_GUNNER ? "GUNNER" : "PASSENGER";
        if(st.vehicleClass != HALO_VEHICLE_TURRET && st.vehicleClass != HALO_VEHICLE_PROP && st.role == HALO_SEAT_DRIVER)
        {
            // Halo world units are 10 feet; a Banshee's pilot also sees the height over the floor.
            string extra = "";
            if(st.vehicleClass == HALO_VEHICLE_FLYER) { if(st.altitude >= 0) formatstring(extra)("  ALT %d M", int(st.altitude * 3.048f + .5f)); }
            else if(st.airborne) copystring(extra, "  AIR");
            defformatstring(speed)("%s  %3d KM/H%s", seat, int(st.speed * 3.048f * 3.6f + .5f), extra);
            label(speed, right, VIRTH-173, 0.70f, true);
        }
        else label(seat, right, VIRTH-173, 0.70f, true);
        if(st.weapon >= 0)
        {
            glDisable(GL_TEXTURE_2D);
            box(right-260, VIRTH-140, 260, 10, 18, 60, 81, 210);
            box(right-260, VIRTH-140, 260 * st.heat, 10, 255, st.overheated ? 60 : 160, 40, 235);
            glEnable(GL_TEXTURE_2D);
            if(st.overheated) label("OVERHEATED", right-270, VIRTH-146, 0.5f, true);
        }
        label(st.flipped ? "Upside down  -  E  Get out" : "E  Get out", VIRTW/2-200, VIRTH/2+90, 0.6f);
    }

    void drawEnding()
    {
        const char *const credits[] = {
            "HALO", "Combat Evolved", "",
            "Pillar of Autumn  to  The Maw", "Campaign complete", "",
            "Original game and campaign", "Bungie", "",
            "Engine", "AssaultCube 1.3.0.2", "",
            "Rebuilt on the AssaultCube engine", "from the original Xbox maps", "",
            "Thank you for playing", "",
            "Enter  Skip"
        };
        const int count = sizeof(credits) / sizeof(credits[0]);
        const int elapsed = lastmillis - endingStart;
        glDisable(GL_TEXTURE_2D);
        box(0, 0, VIRTW, VIRTH, 0, 0, 0, clamp(elapsed / 6, 0, 235));
        glEnable(GL_TEXTURE_2D);
        const float scroll = clamp(elapsed / float(creditsTime), 0.0f, 1.0f);
        const float top = VIRTH + 40 - scroll * (VIRTH + 40 + count * 52);
        loopi(count)
        {
            const float y = top + i * 52;
            if(credits[i][0] && y > -40 && y < VIRTH) label(credits[i], VIRTW/2 - int(text_width(credits[i]) * (i < 2 ? 0.45f : 0.3f)), int(y), i < 2 ? 0.9f : 0.6f);
        }
    }
}

bool haloactive() { return active && gamemode == GMODE_BOTTEAMDEATHMATCH && !multiplayer(NULL); }

unsigned halofilehash(const char *path)
{
    stream *file = openfile(path, "rb");
    if(!file) return 0;
    unsigned hash = 2166136261u;
    char chunk[65536];
    for(int length; (length = file->read(chunk, sizeof(chunk))) > 0;) hash = halostatehash(chunk, size_t(length), hash);
    delete file;
    return hash;
}

void halomapstart(const char *name)
{
    haloscenariostop();
    halofxreset();
    haloloopsound(NULL, false);
    halocollisionmap(nullptr);
    haloplayermoved("section start");
    authoredTransition = false;
    ending = endingDone = winPending = false;
    active = startPending = escaping = false;
    pendingStartState = false;
    player1->maxspeed = 16.0f;
    halovehiclesclear();
    haloclearbolts();
    enemies.setsize(0);
    clearFlood();
    haloactorsclear();
    shield = shieldCap = playerShield;
    maxVitalityHealth = 100;
    maxVitalityShield = 75;
    playerProtected = false;
    groundWeapons.setsize(0);
    checkpointGround.setsize(0);
    groundItems.setsize(0);
    checkpointItemsHalo.setsize(0);
    nearbyWeapon = -1;
    loopi(NUMGUNS) player1->weapons[i]->info = guns[i];
    if(gamemode != GMODE_BOTTEAMDEATHMATCH || multiplayer(NULL) || !haloparsemap(behindpath(name), mission, section)) return;
    if(!requestedSection) preview = true;
    insertion = checkpoint = extraction = -1;
    loopi(2) waveStarts[i].setsize(0);
    loopi(3) route[i].setsize(0);
    loopv(ents)
    {
        const entity &e = ents[i];
        if(e.type != PLAYERSTART) continue;
        switch(e.attr2)
        {
            case TEAM_RVSF:
                if(insertion < 0) insertion = i;
                else if(checkpoint < 0) checkpoint = i;
                break;
            case TEAM_CLA:
                waveStarts[e.attr3 == 1].add(i);
                break;
            case 100:
                if(e.attr3 >= 1 && e.attr3 <= 3) route[e.attr3 - 1].add(i);
                else if(e.attr3 == 0) extraction = i;
                break;
            case 101:
            {
                // Scenario weapon placement; attr5 is the weapons.txt index + 1.
                const HaloWeaponDef *def = haloweapon(e.attr5 - 1);
                if(!def || !def->playable()) break;
                GroundWeapon &g = groundWeapons.add();
                g.o = position(e);
                g.yaw = e.attr1 / 10.0f;
                g.state.reset(def);
                break;
            }
            case 102:
                // Scenario vehicle placement (halo_vehicle.cpp).
                halovehicleplace(e);
                break;
            case 103:
            {
                // Scenario equipment placement: attr3 the equipment.txt index, attr4 the
                // not-placed flags, attr5 the scenario object index + 1.
                const HaloEquipmentDef *def = haloequipment(e.attr3);
                if(!def) break;
                GroundItem &item = groundItems.add();
                item.o = position(e);
                item.yaw = e.attr1 / 10.0f;
                item.def = def->index;
                item.object = e.attr5 - 1;
                item.flags = e.attr4;
                item.created = !(item.flags & 1);
                item.exists = item.created && itemDifficultyAllows(item.flags);
                break;
            }
        }
    }
    if(insertion < 0 || checkpoint < 0 || extraction < 0)
    {
        conoutf("Halo: incomplete campaign section markers");
        return;
    }
    loopv(groundWeapons) checkpointGround.add(groundWeapons[i]);
    loopv(groundItems) checkpointItemsHalo.add(groundItems[i]);
    atCheckpoint = false;
    active = startPending = true;
    halocollisionmap(behindpath(name));
    stage = INSERTION;
    checkpointItems.setsize(0);
}

// Retail floor for a body at an authored point. Retail starts were placed for Halo's own
// bodies, so an AssaultCube body can overlap a console or a cryo tube there: try the point,
// then a ring of nearby spots, and move the point onto the first floor found.
static bool retailfloorspot(vec &point, float radius, float height)
{
    static const float ring[][2] = {{0, 0}, {.6f, 0}, {-.6f, 0}, {0, .6f}, {0, -.6f}, {.45f, .45f}, {-.45f, .45f}, {.45f, -.45f},
        {-.45f, -.45f}, {1.2f, 0}, {-1.2f, 0}, {0, 1.2f}, {0, -1.2f}, {.85f, .85f}, {-.85f, .85f}, {.85f, -.85f}, {-.85f, -.85f}};
    for(const auto &offset : ring)
    {
        const vec at(point.x + offset[0], point.y + offset[1], point.z);
        HaloCollisionHit hit;
        if(!halocollisionsweep(vec(at).add(vec(0, 0, 1.26f)), vec(0, 0, -4.26f), radius, height, hit) ||
           hit.normal.z < .65f || hit.fraction <= 0) continue;
        point = vec(at.x, at.y, at.z + 1.26f - 4.26f * hit.fraction + .015f);
        return true;
    }
    return false;
}

playerent *haloscenariospawnenemy(int kind, const vec &point, float yaw, int actorIndex, int team)
{
    if(const HaloActorDef *actor = haloactordef(actorIndex)) if(kind < 0) kind = actor->kind;
    if(kind < 0 || kind >= ACTOR_COUNT || !haloactive() || halocinematic() || editmode) return nullptr;
    if(halocollisionactive())
    {
        const ActorProfile &profile = profiles[kind];
        vec feet = point;
        if(!retailfloorspot(feet, profile.radius, profile.eyeheight + .7f)) return nullptr;
        return spawnEnemy(ActorKind(kind), vec(feet).add(vec(0, 0, profile.eyeheight)), yaw, -1, actorIndex, team);
    }
    const int x = int(point.x), y = int(point.y);
    if(OUTBORD(x, y) || SOLID(S(x, y)) || fabsf(S(x, y)->floor - point.z) > 5.0f) return nullptr;
    return spawnEnemy(ActorKind(kind), point, yaw, -1, actorIndex, team);
}

// Milliseconds left on the escape countdown, or -1 outside the escape run.
int haloescapemillis() { return escaping ? max(0, escapeDeadline - lastmillis) : -1; }
float haloplayershieldfraction() { return clamp(shield / float(playerShield), 0.0f, 1.0f); }

float haloenemyshieldfraction(const playerent *p)
{
    HaloEnemy *enemy = enemyState(p);
    return enemy && enemy->shieldMax > 0 ? enemy->shield / enemy->shieldMax : 0;
}

// A starting profile's weapon as a carried weapon state. Judgement call, not checked against the
// retail executable: the profile's "rounds total" is the reserve beyond the magazine (a30's assault
// rifle starts 60 + 120), clamped to the magazine's maximum; a battery weapon starts charged.
static bool profileWeapon(const HaloStartingProfile &p, int i, HaloWeaponState &out)
{
    const HaloWeaponDef *d = p.weapon[i].empty() ? NULL : haloweaponfind(p.weapon[i].c_str());
    if(!d || !d->playable()) return false;
    out.reset(d);
    if(const HaloMagazineDef *m = d->usesBattery() ? NULL : d->magazine())
    {
        out.loaded = clamp(p.loaded[i], 0, max(1, m->loaded));
        out.reserve = clamp(p.total[i], 0, max(0, m->maximum - out.loaded));
    }
    return true;
}

void haloplayeraddequipment(const HaloStartingProfile &profile, bool reset)
{
    if(!haloactive()) return;
    if(tagWeapons())
    {
        syncCarried();
        const bool wasArmed = slotArmed(currentCarried);
        if(reset)
        {
            // Replaces what the player carries: an empty profile slot empties theirs.
            loopi(2) if(!profileWeapon(profile, i, carried[i])) carried[i] = HaloWeaponState();
            currentCarried = 0;
        }
        else loopi(2)
        {
            HaloWeaponState given;
            if(!profileWeapon(profile, i, given)) continue;
            int slot = carried[0].def == given.def ? 0 : carried[1].def == given.def ? 1 : -1;
            if(slot >= 0)
            {
                // A weapon already carried takes the rounds, up to its maximum.
                const HaloWeaponDef *d = haloweapon(given.def);
                const HaloMagazineDef *m = d ? d->magazine() : NULL;
                if(m && !d->usesBattery())
                    carried[slot].reserve = clamp(carried[slot].reserve + given.loaded + given.reserve, 0, max(0, m->maximum - carried[slot].loaded));
                else carried[slot].battery = 1;
                continue;
            }
            // Otherwise an empty slot, the selected one first; with both armed it is not taken.
            slot = !slotArmed(currentCarried) ? currentCarried : !slotArmed(currentCarried ^ 1) ? currentCarried ^ 1 : -1;
            if(slot < 0) continue;
            carried[slot] = given;
            if(!wasArmed) currentCarried = slot;
        }
        loopi(NUMGUNS) if(i != GUN_GRENADE && i != GUN_KNIFE) player1->mag[i] = player1->ammo[i] = 0;
        equipCarried(true);
        conoutf("Halo: starting profile %s %s: %s / %s", profile.name.empty() ? "(unnamed)" : profile.name.c_str(),
                    reset ? "replaces the loadout" : "added", haloweapon(carried[0].def) ? haloweapon(carried[0].def)->name.c_str() : "-",
                    haloweapon(carried[1].def) ? haloweapon(carried[1].def)->name.c_str() : "-");
    }
    // Grenades: set by a reset, else added; health and shield by their modifiers (0 keeps them).
    const int frags = reset ? profile.frags : grenadeCount(HALO_GRENADE_FRAG) + profile.frags;
    grenadeCount(HALO_GRENADE_FRAG) = clamp(frags, 0, grenadeMaximum(HALO_GRENADE_FRAG));
    if(grenadeKnown(HALO_GRENADE_PLASMA))
        plasmaGrenades = clamp(reset ? profile.plasmas : plasmaGrenades + profile.plasmas, 0, grenadeMaximum(HALO_GRENADE_PLASMA));
    if(grenadeCount(grenadeType) <= 0 && grenadeKnown(grenadeType ^ 1) && grenadeCount(grenadeType ^ 1) > 0) grenadeType ^= 1;
    if(profile.health > 0 && player1->state == CS_ALIVE) player1->health = clamp(int(playerHealth * min(profile.health, 1.0f) + .5f), 1, playerHealth);
    if(profile.shield > 0) shield = clamp(shieldCap * min(profile.shield, 1.0f), 0.0f, shieldCap);
}

// Script control of the player's and the enemies' vitality (unit_set_*_vitality, unit_impervious, ai_renew, ai_teleport_*).
// Values are in the script's units: the player's maximum defaults to 100 health and 75 shield, an actor's to 1.
void haloplayersetvitality(float health, float shieldValue, bool maximum)
{
    if(maximum)
    {
        maxVitalityHealth = max(health, 1.0f);
        maxVitalityShield = max(shieldValue, 0.0f);
        shieldCap = maxVitalityShield > 0 ? float(playerShield) : 0.0f;
        shield = min(shield, shieldCap);
        return;
    }
    if(player1->state == CS_ALIVE)
        player1->health = clamp(int(health / maxVitalityHealth * playerHealth + .5f), 1, playerHealth);
    shield = maxVitalityShield > 0 ? clamp(shieldValue / maxVitalityShield, 0.0f, 1.0f) * shieldCap : 0.0f;
    shieldHit = lastmillis;
    shieldRecharging = false;
}
void haloplayerprotect(bool on) { playerProtected = on; }
bool haloplayerprotected() { return playerProtected; }

void haloenemysetshield(const playerent *p, float fraction)
{
    if(HaloEnemy *enemy = enemyState(p)) enemy->shield = clamp(fraction, 0.0f, 1.0f) * enemy->shieldMax;
}

// Refills health, shield and grenades (ai_renew).
void haloenemyrenew(playerent *p, int health, int grenades)
{
    HaloEnemy *enemy = enemyState(p);
    if(!enemy || p->state != CS_ALIVE) return;
    enemy->shield = enemy->shieldMax;
    p->health = max(p->health, health);
    p->mag[GUN_GRENADE] = max(p->mag[GUN_GRENADE], grenades);
}

// Puts an enemy on the ground at a world point, as when it was placed.
bool haloenemymove(playerent *p, const vec &point)
{
    HaloEnemy *enemy = enemyState(p);
    if(!enemy || p->state != CS_ALIVE) return false;
    const ActorProfile &profile = profiles[enemy->kind];
    if(halocollisionactive())
    {
        vec feet = point;
        if(!retailfloorspot(feet, profile.radius, profile.eyeheight + .7f)) return false;
        p->o = vec(feet).add(vec(0, 0, profile.eyeheight));
    }
    else p->o = point;
    p->vel = vec(0, 0, 0);
    entinmap(p);
    return true;
}

void haloscenarioplayersave()
{
    if(haloactive() && player1->state == CS_ALIVE && !escaping && stage != FIRST_CONTACT &&
       stage != SECOND_CONTACT && stage != COMPLETE) saveCheckpoint(true);
}

bool halorespawn()
{
    if(!haloactive()) return false;
    if(player1->state == CS_DEAD && lastmillis - player1->lastdeath >= 1000)
    {
        resetEncounter();
        haloscenariorevert();
        halovehiclessync();
        haloequipmentsync();
        resetcamera();
    }
    return true;
}

// Recovery on retail collision. The retail structure has holes and seams the stock cube grid
// never had: a body can fall out of the world below the section's mesh, or end up wedged where
// no move gets it out (a door or a lift deck closing on it, a crease between two slopes too
// steep to stand on). The player is put back on the last retail floor they stood on, or, when
// none of those is clear any more, reverts to the checkpoint; actors that fall out of the world
// die, as they would in Halo. Ordinary movement never trips these checks.
namespace
{
    struct SafeSpot { vec feet; int millis; };
    vector<SafeSpot> safeSpots;            // the newest last, each at least safeSpacing from the one before
    const int safeSpotCount = 8, safeSpotInterval = 100;
    const float safeSpacing = 2.0f;
    const float killDepth = 8.0f;          // world units below the mesh's lowest point
    const int fallTime = 2000;             // airborne with nothing at all below for this long: out of the world
    const int wedgeTime = 750, stallTime = 1500;
    const int moveGrace = 1000;            // after a move that is not walking, before the wedge check may nudge
    int safeSpotChecked = 0, embeddedSince = 0, stalledSince = 0, lastRecovery = 0, movedAt = 0;
    bool stoodSafe = false;                // a safe spot was recorded since the last move that was not walking
    vec stalledAt;

    float bodyHeight(const physent *d) { return max(d->eyeheight + d->aboveeye, 2 * d->radius); }

    // Forgets the floors recorded before a move that was not walking (haloplayermoved).
    void clearRecovery()
    {
        safeSpots.setsize(0);
        safeSpotChecked = embeddedSince = stalledSince = lastRecovery = 0;
        stoodSafe = false;
        movedAt = lastmillis;
        stalledAt = vec(player1->o).sub(vec(0, 0, player1->eyeheight));
    }

    bool inMoveGrace() { return unsigned(lastmillis - movedAt) < unsigned(moveGrace); }

    // Whether the player's body fits with its feet near `feet`, on retail floor (not a device
    // top: lift decks and light bridges move or vanish), clear of the structure, of every
    // device box and (unless `vehicles` is false) of every vehicle (one may have been parked
    // there since; a body put back part way into a vehicle's box could not move). `ground` gets
    // the feet on that floor.
    bool standable(const vec &feet, vec &ground, bool vehicles = true)
    {
        const float height = bodyHeight(player1), radius = player1->radius;
        const vec top = vec(feet).add(vec(0, 0, .5f));
        HaloCollisionHit hit;
        if(halocollisionoverlap(top, radius, height, hit) && hit.penetration > .015f) return false;
        if(!halocollisionsweep(top, vec(0, 0, -2.5f), radius, height, hit) || hit.normal.z < .65f) return false;
        ground = vec(top).add(vec(0, 0, -2.5f * hit.fraction + .015f));
        if(vehicles && halovehicleat(vec(ground).add(vec(0, 0, .5f)), radius) >= 0) return false;
        const vec saved = player1->o;
        player1->o = vec(ground).add(vec(0, 0, player1->eyeheight));
        float hi = 1e9f, lo = -1e9f;
        const bool blocked = haloscenariocollide(player1, hi, lo) || ground.z + height > hi || lo > ground.z + .1f;
        player1->o = saved;
        return !blocked;
    }

    void placePlayer(const vec &ground, const char *why)
    {
        player1->o = vec(ground).add(vec(0, 0, player1->eyeheight));
        player1->vel = vec(0, 0, 0);
        player1->timeinair = 0;
        player1->onfloor = true;
        player1->resetinterp();
        embeddedSince = stalledSince = 0;
        stalledAt = ground;
        conoutf("Halo recovery: %s, player put back at %.1f,%.1f,%.1f", why, ground.x, ground.y, ground.z);
    }

    void trackSafeSpot(const vec &feet)
    {
        if(!player1->onfloor || player1->timeinair || lastmillis - safeSpotChecked < safeSpotInterval) return;
        safeSpotChecked = lastmillis;
        vec ground;
        if(!standable(feet, ground) || ground.dist(feet) > .3f) return;
        stoodSafe = true;
        if(!safeSpots.empty() && safeSpots.last().feet.dist(ground) < safeSpacing)
        {
            safeSpots.last() = SafeSpot{ground, lastmillis};
            return;
        }
        if(safeSpots.length() >= safeSpotCount) safeSpots.remove(0);
        safeSpots.add(SafeSpot{ground, lastmillis});
    }

    // Back to the newest safe spot that is still clear. A second recovery within five seconds
    // means that spot did not hold (a ledge the body slides off again): drop it and go further back.
    // `falling`: the body is falling out of the world, not wedged or stalled where it stands.
    bool restoreSafeSpot(const char *why)
    {
        while(!safeSpots.empty())
        {
            vec ground;
            if(standable(safeSpots.last().feet, ground)) { placePlayer(ground, why); return true; }
            safeSpots.pop();
        }
        return false;
    }

    void recoverPlayer(const char *why, bool falling)
    {
        if(lastRecovery && lastmillis - lastRecovery < 5000 && !safeSpots.empty()) safeSpots.pop();
        lastRecovery = lastmillis;
        // Just thrown out of a vehicle with no room for the body (a Warthog over on its back in
        // the Maw's run, where the walking body fits almost nowhere): back into its seat, rather
        // than onto a floor walked long before boarding or a revert.
        if(halovehiclereclaim(why))
        {
            embeddedSince = stalledSince = 0;
            return;
        }
        if(restoreSafeSpot(why)) return;
        if(!falling && !stoodSafe)
        {
            // Wedged since the spawn or the revert, before standing on any retail floor (a
            // checkpoint on a lift deck): reverting would put the body back in the same place
            // and loop. Leave it, and look again in five seconds.
            conoutf("Halo recovery: %s, no safe floor since the checkpoint, player left in place", why);
            embeddedSince = embeddedSince ? lastmillis + 5000 - wedgeTime : 0;
            stalledSince = stalledSince ? lastmillis + 5000 - stallTime : 0;
            return;
        }
        conoutf("Halo recovery: %s, no safe floor left, reverting to the checkpoint", why);
        dodamage(INT_MAX, player1, player1, GUN_GRENADE);
    }

    // A wedged body is first moved to the nearest clear floor within three units.
    bool nudgePlayer(const vec &feet, const char *why)
    {
        for(float radius : {0.0f, .5f, 1.0f, 1.5f, 2.25f, 3.0f})
            loopi(radius > 0 ? 12 : 1)
            {
                const float angle = i * 30 * RAD;
                for(float up : {0.0f, 1.26f})
                {
                    vec ground;
                    if(!standable(vec(feet).add(vec(cosf(angle) * radius, sinf(angle) * radius, up)), ground)) continue;
                    placePlayer(ground, why);
                    return true;
                }
            }
        return false;
    }

    void updateRecovery()
    {
        // A seated player rides the vehicle, which keeps itself out of the walls (halo_vehicle.cpp).
        if(!halocollisionactive() || halocinematic() || player1->state != CS_ALIVE || stage == COMPLETE || ending || halodriving())
        {
            embeddedSince = stalledSince = 0;
            return;
        }
        vec low, high;
        if(!halocollisionbounds(low, high)) return;
        const float kill = low.z - killDepth;
        loopv(enemies)
        {
            botent *bot = enemies[i].bot;
            if(!bot || bot->state != CS_ALIVE || bot->o.z - bot->eyeheight >= kill) continue;
            conoutf("Halo recovery: %s fell out of the world", bot->name);
            dodamage(INT_MAX, bot, bot, GUN_GRENADE);
        }
        const vec feet = vec(player1->o).sub(vec(0, 0, player1->eyeheight));
        trackSafeSpot(feet);
        if(feet.z < kill) { recoverPlayer("fell below the retail collision", true); return; }
        if(!player1->onfloor && player1->timeinair > fallTime)
        {
            // Nothing below at all (structure or device) down to the kill depth: a hole in the mesh.
            HaloCollisionHit hit;
            if(!haloscenariotrace(feet, vec(feet.x, feet.y, kill), hit)) { recoverPlayer("fell out of the world", true); return; }
        }
        HaloCollisionHit hit;
        float hi = 1e9f, lo = -1e9f;
        const bool embedded = (halocollisionoverlap(feet, player1->radius, bodyHeight(player1), hit) && hit.penetration > .1f) ||
                              haloscenariocollide(player1, hi, lo);
        if(!embedded) embeddedSince = 0;
        else if(!embeddedSince) embeddedSince = lastmillis;
        // Just moved there by a teleport, a revert or a seat: the body settles (entinmap, a
        // door finishing its swing) before the wedge and stall checks start counting.
        if(inMoveGrace())
        {
            embeddedSince = stalledSince = 0;
            stalledAt = feet;
            return;
        }
        // Airborne yet not moving at all: caught in a crease too steep to stand on.
        if(player1->onfloor || feet.dist(stalledAt) > .05f)
        {
            stalledSince = 0;
            stalledAt = feet;
        }
        else if(!stalledSince) stalledSince = lastmillis;
        const bool wedged = embeddedSince && lastmillis - embeddedSince >= wedgeTime;
        if(!wedged && !(stalledSince && lastmillis - stalledSince >= stallTime)) return;
        const char *why = wedged ? "wedged in the collision" : "stuck in mid-air";
        if(!nudgePlayer(feet, why)) recoverPlayer(why, false);
    }
}

bool halorecoverplayer(const char *why)
{
    if(!haloactive() || !halocollisionactive() || player1->state != CS_ALIVE) return false;
    return restoreSafeSpot(why);
}

void haloplayermoved(const char *why, const vec *kept)
{
    const int dropped = safeSpots.length();
    clearRecovery();
    // The floor the player got into a seat from: retail floor they walked to, clear of the
    // structure and the devices (the vehicle beside it is not counted: it is about to move off,
    // and restoreSafeSpot checks the vehicles again before using the spot).
    vec ground;
    if(kept && halocollisionactive() && standable(*kept, ground, false) && ground.dist(*kept) <= .3f)
        safeSpots.add(SafeSpot{ground, lastmillis});
    if(halo_scripttrace && haloactive())
        conoutf("Halo moved: %s, %d safe floor%s dropped%s", why ? why : "?", dropped, dropped == 1 ? "" : "s",
                safeSpots.empty() ? "" : ", the boarding floor kept");
}

bool haloplayerplace(const vec *feet, float yaw, const char *why)
{
    if(!haloactive() || editmode || ending || player1->state != CS_ALIVE || halodriving()) return false;
    if(!feet)
    {
        player1->yaw = yaw;
        player1->resetinterp();
        return true;
    }
    // Flags sit on the floor or a little above it; the body is tried there and a little higher.
    vec ground;
    bool found = false;
    for(float up : {0.0f, .5f, 1.26f})
    {
        const vec at = vec(*feet).add(vec(0, 0, up));
        if(halocollisionactive())
        {
            if(standable(at, ground)) { found = true; break; }
            continue;
        }
        if(OUTBORD(int(at.x), int(at.y))) break;
        // collide's spawn test drops the body onto the floor of the cube column it stands in; a
        // floor far below the flag (a flag in the air over the map, such as a50's lift arrival
        // in the ship) is not one it stands on.
        const vec old = player1->o;
        const bool wasOnFloor = player1->onfloor;
        player1->o = vec(at).add(vec(0, 0, player1->eyeheight));
        const bool blocked = collide(player1, true);
        if(!blocked) ground = vec(player1->o).sub(vec(0, 0, player1->eyeheight));
        player1->o = old;
        player1->onfloor = wasOnFloor;
        if(!blocked && at.z - ground.z <= 4.0f) { found = true; break; }
    }
    if(!found) return false;
    player1->o = vec(ground).add(vec(0, 0, player1->eyeheight));
    player1->vel = vec(0, 0, 0);
    player1->timeinair = 0;
    player1->yaw = yaw;
    player1->attacking = false;
    player1->resetinterp();
    haloplayermoved(why);
    return true;
}

// The floor under `from` the player's body fits on (gap 10: the scripted seat's guard lets the
// player out of a Pelican that may be in mid-air): on retail collision the first floor straight
// down the mesh, at `from` or at the spawn helper's ring of nearby spots (retailfloorspot), that
// standable accepts; on stock geometry the floor of the cube column. False when there is none.
bool haloplayerfloorbelow(const vec &from, vec &ground)
{
    if(!haloactive() || !std::isfinite(from.x) || !std::isfinite(from.y) || !std::isfinite(from.z)) return false;
    if(!halocollisionactive())
    {
        const int x = int(from.x), y = int(from.y);
        if(OUTBORD(x, y) || SOLID(S(x, y)) || S(x, y)->floor > from.z + 1.26f) return false;
        ground = vec(from.x, from.y, S(x, y)->floor);
        return true;
    }
    vec low, high;
    if(!halocollisionbounds(low, high)) return false;
    static const float ring[][2] = {{0, 0}, {.6f, 0}, {-.6f, 0}, {0, .6f}, {0, -.6f}, {1.2f, 0}, {-1.2f, 0}, {0, 1.2f}, {0, -1.2f},
                                    {2.4f, 0}, {-2.4f, 0}, {0, 2.4f}, {0, -2.4f}};
    const float height = bodyHeight(player1), radius = player1->radius;
    for(const auto &offset : ring)
    {
        const vec top(from.x + offset[0], from.y + offset[1], from.z + 1.26f);
        const float depth = top.z - (low.z - 1.0f);
        if(depth <= 0) continue;
        HaloCollisionHit hit;
        if(!halocollisionsweep(top, vec(0, 0, -depth), radius, height, hit) || hit.normal.z < .65f || hit.fraction <= 0) continue;
        if(standable(vec(top.x, top.y, top.z - depth * hit.fraction + .015f), ground)) return true;
    }
    return false;
}

bool haloscenariotransition()
{
    if(!haloactive() || (preview && !retailCartographer()) || authoredTransition || launchPending || halocinematic() ||
       stage == COMPLETE || section + 1 >= haloMissions[mission].sections) return false;
    if(!installed(mission, section + 1)) return false;
    authoredTransition = true;
    finishSection();
    return true;
}

void haloupdate(int elapsed)
{
    if(launchPending && getclientnum() >= 0)
    {
        launchPending = false;
        preview = pendingPreview;
        campaignFinished = ending = endingDone = winPending = false;
        string name;
        halosectionname(pendingMission, pendingSection, name, sizeof(name));
        requestedSection = true;
        startgame(name, GMODE_BOTTEAMDEATHMATCH, 60, true);
        requestedSection = false;
        if(haloactive() && !controlsLoaded) controlsLoaded = execfile("config/halo.cfg");
    }
    if(!haloactive() || editmode) return;
    if(ispaused || elapsed <= 0)
    {
        haloscenariopausestate(ispaused || menuvisible());
        // AssaultCube keeps its sounds playing while paused: the loops stop.
        if(ispaused) { haloloopsound(NULL, false); halovehiclessounds(); }
        return;
    }
    if(startPending)
    {
        if(getclientnum() < 0) return;
        startPending = false;
        // What the player brings into the section: from the section before (within a mission),
        // from a continued game, or (restarting the section) what they brought in last time.
        const bool restoring = restorePending && !preview && savedGame.valid && savedGame.mission == mission && savedGame.section == section;
        restorePending = false;
        if(preview) sectionLoadout.valid = false;
        else if(pendingContinuation && carryLoadout.valid) sectionLoadout = carryLoadout;
        else if(restoring)
        {
            sectionLoadout = savedGame.loadout;
            loopi(2)
            {
                // An `empty` line: an empty slot. A weapon the table no longer has, or neither line
                // (a save from before empty slots, or one made without weapons.txt): the default
                // weapon for that slot, with its own rounds.
                const HaloWeaponDef *d = savedGame.weaponPath[i][0] ? haloweaponfind(savedGame.weaponPath[i]) : NULL;
                if(d) sectionLoadout.carried[i].def = d->index;
                else if(savedGame.slotEmpty[i] && !savedGame.weaponPath[i][0]) sectionLoadout.carried[i] = HaloWeaponState();
                else sectionLoadout.carried[i].reset(haloweaponfind(defaultWeapons[i]));
            }
        }
        else if(!keepSectionLoadout || sectionLoadoutMission != mission || sectionLoadoutSection != section) sectionLoadout.valid = false;
        carryLoadout.valid = keepSectionLoadout = false;
        sectionLoadoutMission = mission;
        sectionLoadoutSection = section;
        resetEncounter();
        resetcamera();
        conoutf("Halo: %s, section %d", haloMissions[mission].title, section + 1);
        // A fresh start (nothing carried in, restored or kept by a restart) takes the mission's
        // starting profile once its script is loaded (below).
        const bool freshLoadout = !preview && !restoring && !sectionLoadout.valid;
        haloscenariostart(mission, section, preview, pendingContinuation);
        if(freshLoadout) applyStartingProfile();
        halovehiclessync();
        haloequipmentsync();
        // A save at this section's start keeps the script's state only when the script was
        // carried in; Continue (with a state block that fits) restores it where it was saved.
        scriptCarried = !preview && pendingContinuation;
        if(restoring)
        {
            if(restoreSavedState()) scriptCarried = true;
            else restoreSavedCheckpoint();
        }
        if(!preview) loadProgress();
        if(!preview && (mission > savedMission || (mission == savedMission && section > savedSection)))
        {
            savedMission = mission;
            savedSection = section;
        }
        if(!preview) recordProgress(atCheckpoint);
        // Carried in under an opening scene: the save that keeps the script waits for it to end.
        pendingStartState = !preview && scriptCarried && !atCheckpoint && halocinematic();
        // A carried-in script is also the in-memory checkpoint until the section's first one, so
        // a death before it reverts to the script as it came in, in live play as after Continue
        // (which restores the saved section start the same way), instead of restarting it.
        if(!preview && scriptCarried && !atCheckpoint && !halocinematic()) haloscenariocheckpoint();
    }
    haloscenariotick(elapsed);
    if(pendingStartState && !halocinematic())
    {
        pendingStartState = false;
        if(!atCheckpoint && stage != COMPLETE && player1->state == CS_ALIVE)
        {
            haloscenariocheckpoint();
            recordProgress(false);
        }
    }
    if(haloscenariocomplete()) finishSection(true);
    if(haloscenariogamewon())
    {
        if(!preview && mission == haloMissionCount - 1 && section == haloMissions[mission].sections - 1) winPending = true;
        else if(haloscenarioboarded() && stage != COMPLETE)
        {
            // The player boarded the extraction Pelican and the script ends the mission (a30).
            conoutf("Halo: game_won aboard the extraction, %s section %d complete", haloMissions[mission].title, section + 1);
            finishSection();
        }
        else conoutf("Halo: game_won ignored on %s, section %d", haloMissions[mission].title, section + 1);
    }
    if(haloscenariogamelost() && !ending && !halocinematic() && player1->state == CS_ALIVE)
    {
        conoutf("Halo game over: game_lost, reverting");
        dodamage(INT_MAX, player1, player1, GUN_GRENADE);
    }
    if(!ending && !halocinematic() && (winPending || (campaignFinished && stage == COMPLETE))) beginEnding();
    if(ending)
    {
        // The credits run over the live level; nothing may interrupt them.
        player1->attacking = false;
        player1->stopmoving();
        if(player1->state == CS_ALIVE) { player1->health = max(player1->health, playerHealth); shield = playerShield; }
        if(lastmillis - endingStart >= creditsTime) finishEnding();
        return;
    }
    if(authoredTransition && stage == COMPLETE)
    {
        nextSection();
        return;
    }
    halovehiclesupdate(elapsed);
    halofxupdate(elapsed);
    updateFiringLoop();
    if(halocinematic() || player1->state != CS_ALIVE || stage == COMPLETE) return;
    // The run's first section on stock geometry starts on foot by the parked Warthogs; when the
    // script has not created them (a section launched on its own runs no script), one waits there.
    if(escaping && !escapeOffered && !halodriving() && lastmillis - escapeStarted > 2500)
    {
        escapeOffered = true;
        halovehicleensure(feetOf(player1), player1->yaw, 80.0f);
    }
    // A flown section always has a Banshee the player may take (the scenario's, or one beside
    // them): checked after the opening and every five seconds on foot, so one lost is replaced.
    static int flightChecked = 0;
    if(flownSection() && !halodriving() && lastmillis - escapeStarted > 2500 && lastmillis - flightChecked > 5000)
    {
        flightChecked = lastmillis;
        halovehicleensuretype("banshee", feetOf(player1), player1->yaw);
    }
    updateRecovery();
    if(player1->state != CS_ALIVE) return;
    if(escaping && lastmillis >= escapeDeadline)
    {
        if(haloscenarioescapefailed() || !haloscenarioescapetimeout())
            dodamage(INT_MAX, player1, player1, GUN_GRENADE);
        return;
    }
    if(!retailCartographer() && (stage == INSERTION || stage == REACH_PASS || stage == EXTRACTION))
    {
        const vector<int> &markers = route[stage == INSERTION ? 0 : stage == REACH_PASS ? 1 : 2];
        if(escaping)
        {
            const vec target = objective();
            if(player1->o.distxy(target) <= 13.0f && nextRouteMarker < route[0].length() + route[1].length() + route[2].length())
                ++nextRouteMarker;
        }
        else if(markers.inrange(nextRouteMarker) && near(markers[nextRouteMarker], 13.0f)) ++nextRouteMarker;
    }
    if(shield < shieldCap && !shieldRecharging && lastmillis - shieldHit >= shieldDelay)
    {
        haloplaysound("shield-charge", S_NULL);
        shieldRecharging = true;
    }
    shieldTick(shield, shieldCap, shieldHit, shieldDelay, shieldRecharge, elapsed);
    updateWeapons(elapsed);
    updateItems(elapsed);
    updateFlood();
    loopv(enemies)
    {
        HaloEnemy &enemy = enemies[i];
        if(enemy.shieldMax > 0 && enemy.bot && enemy.bot->state == CS_ALIVE)
            shieldTick(enemy.shield, enemy.shieldMax, enemy.hitTime, eliteDelay, eliteRecharge, elapsed);
    }
    if(retailCartographer()) return;
    switch(stage)
    {
        case INSERTION:
            if(!route[0].inrange(nextRouteMarker) && player1->o.distxy(contactPoint()) <= firstContactRadius) setStage(FIRST_CONTACT);
            break;
        case FIRST_CONTACT:
            if(!remaining()) setStage(REACH_PASS);
            break;
        case REACH_PASS:
            if(!route[1].inrange(nextRouteMarker) && near(checkpoint, markerRadius)) { saveCheckpoint(); setStage(SECOND_CONTACT); }
            break;
        case SECOND_CONTACT:
            if(!remaining()) setStage(EXTRACTION);
            break;
        case EXTRACTION:
            if(nextRouteMarker >= route[2].length() + (escaping ? route[0].length() + route[1].length() : 0) &&
               near(extraction, markerRadius) && !sectionInteraction()) finishSection();
            break;
        case COMPLETE: break;
    }
}

// halo_damagetrace 1 also logs the material a special body was hit on (materialScale):
// DAMAGE_MATERIAL target=<name> kind=<ActorKind> material=<retail type> scale=<multiplier>
// halo_damagetrace 1 logs every hit halodamage sees (tests/test_damage_model.py):
// DAMAGE_TRACE attacker=<name> target=<name> exact=<0|1> amount=<before shields> side=<n> gun=<n>
//              health=<whole points taken> shield=<left> carry=<fraction left over>
VAR(halo_damagetrace, 0, 0, 1);
// "Lethal to the unsuspecting" (the melee jpt!'s side effect 2): halo_meleeunsuspecting 0 turns
// the kill from behind off. halo_damagetrace 1 logs each one:
// DAMAGE_UNSUSPECTING target=<name> kind=<ActorKind> status=<ai_status> facing=<cosine> amount=<dealt>
VAR(halo_meleeunsuspecting, 0, 1, 1);

void halopendinghit(const playerent *target, float damage, int sideEffect, const HaloDamage *source, const vec *at)
{
    pendingHit.target = target;
    pendingHit.damage = damage;
    pendingHit.sideEffect = sideEffect;
    pendingHit.source = target ? source : NULL;
    pendingHit.hasAt = target && at && std::isfinite(at->x) && std::isfinite(at->y) && std::isfinite(at->z);
    pendingHit.at = pendingHit.hasAt ? *at : vec(0, 0, 0);
}

// The Hunter's head and belly (materialScale): halo_hunterregions 0 leaves every hit that is not
// on the shield arm or the back on the armour, as before.
VAR(halo_hunterregions, 0, 1, 1);
// The retail material model for every other actor (materialScale): off by default, for a later
// balance pass (it moves every time to kill: the sniper rifle's modifier on Flood combat forms is
// 0.05, the assault rifle's on an Elite's shield 0.7, the plasma rifle's 2).
VAR(halo_actormaterials, 0, 0, 1);

void haloactorfired(const playerent *p)
{
    HaloEnemy *enemy = enemyState(p);
    if(enemy) enemy->firedAt = lastmillis;
}

float haloactordamagescale(const playerent *p)
{
    const HaloEnemy *enemy = enemyState(p);
    return enemy ? enemy->damageScale : aiDamageScale;
}

namespace
{
    // Whole health points of an exact amount; the fraction is carried to the next hit.
    int wholeDamage(float amount, float &carry)
    {
        // A little slack so float products that should be whole (13 on Heroic is 1.3 x 10, which
        // comes out at 12.999999) are not cut a point short with 0.999999 carried.
        const float total = max(0.0f, amount) + carry;
        const int whole = int(total + 1e-3f);
        carry = max(0.0f, total - whole);
        return whole;
    }

    // Where on the body a hit struck, 0 the feet and 1 the top, or -1 when the hit carries no point.
    float hitHeight(const playerent *target, const PendingHit *hit)
    {
        if(!hit || !hit->hasAt) return -1;
        const float feet = target->o.z - target->eyeheight, height = max(target->eyeheight + target->aboveeye, .1f);
        return clamp((hit->at.z - feet) / height, 0.0f, 1.0f);
    }

    // The retail material model for an actor that has no special body (halo_actormaterials 1): the
    // hit material is the variant's head-flagged material for a hit at head height (the top fifth),
    // else its first other material; the hit takes that material's body multiplier times the jpt!
    // damage's modifier for its type. An Elite's energy shield takes the modifier for the elite
    // energy shield (30) in `shieldScale` (in place of the assault rifle's 0.7). Without the
    // weapon's `materials` line or the variant's `material` lines nothing changes.
    float generalMaterial(const HaloEnemy &enemy, const playerent *target, const HaloDamage *source, const PendingHit *hit,
                          int &region, float &shieldScale)
    {
        region = -1;
        const HaloActorDef *def = haloactorvariant(target);
        if(!halo_actormaterials || !source || !source->hasMaterials || !def || def->materials.empty()) return 1;
        if(enemy.kind == ELITE) shieldScale = source->material(30);
        const bool head = hitHeight(target, hit) >= .8f;
        const HaloActorDef::Material *chosen = NULL;
        for(const HaloActorDef::Material &m : def->materials) if(((m.flags & 1) != 0) == head) { chosen = &m; break; }
        if(!chosen) chosen = &def->materials[0];
        region = chosen->type;
        return max(chosen->body, 0.0f) * source->material(chosen->type);
    }

    // Bodies whose retail collision materials decide what hurts them: the hit material's body
    // multiplier (the unit's coll tag, actors.txt) times the jpt! damage's modifier for that
    // material type (weapons.txt and equipment.txt `materials` lines). Every other body takes its
    // damage as before unless halo_actormaterials is on (generalMaterial).
    //   Hunter: a direct hit from within 60 degrees of where it faces lands on the shield arm
    //   (hunter shield: body multiplier 0, and every campaign weapon but the energy sword has
    //   modifier 0 for it); from behind (beyond 130 degrees) on the exposed skin (hunter skin);
    //   anything else, blasts included, on the armour plates (hunter armor). With
    //   halo_hunterregions (on by default) two more hits reach the skin, Halo's weak spots in
    //   front of the back: the head (the coll's head-flagged material is hunter skin), a hit in the
    //   top fifth of the body from outside the shield arm's 60 degrees; and the belly, a hit from
    //   the front between 30% and 60% of its height while it fires its cannon (the 0.6 s after a
    //   cannon shot, when the shield arm swings aside: HaloEnemy::firedAt, so a smash, which sets
    //   lastaction too, does not open it). Engine judgements: the retail coll tag's regions are not
    //   traced, the body is a cylinder here.
    //   Sentinel, Monitor and Engineer have one material each (sentinel, monitor, engineer skin);
    //   the Monitor's body multiplier is 0, so nothing hurts it.
    // `region` returns the material type used (-1: none applies); `shieldScale` multiplies what an
    // energy shield takes of the hit (generalMaterial).
    float materialScale(const HaloEnemy &enemy, const playerent *target, const playerent *attacker, int gun,
                        const HaloDamage *source, int &region, const PendingHit *hit, float &shieldScale)
    {
        region = -1;
        shieldScale = 1;
        switch(enemy.kind)
        {
            case HUNTER:
                region = HALO_MATERIAL_HUNTER_ARMOR;
                if(attacker && attacker != target && gun != GUN_GRENADE)
                {
                    vec delta = vec(attacker->o).sub(target->o);
                    delta.z = 0;
                    if(!delta.iszero())
                    {
                        const float yaw = target->yaw * RAD;
                        const float facing = delta.normalize().dot(vec(sinf(yaw), -cosf(yaw), 0));
                        const float height = halo_hunterregions ? hitHeight(target, hit) : -1;
                        const bool firing = lastmillis - enemy.firedAt >= 0 && lastmillis - enemy.firedAt < 600;
                        if(facing > cosf(60 * RAD))
                            region = height >= .3f && height <= .6f && firing ? HALO_MATERIAL_HUNTER_SKIN : HALO_MATERIAL_HUNTER_SHIELD;
                        else if(facing < cosf(130 * RAD) || height >= .8f) region = HALO_MATERIAL_HUNTER_SKIN;
                    }
                }
                break;
            case SENTINEL: region = HALO_MATERIAL_SENTINEL; break;
            case MONITOR: region = HALO_MATERIAL_MONITOR; break;
            case ENGINEER: region = HALO_MATERIAL_ENGINEER_SKIN; break;
            default: return generalMaterial(enemy, target, source, hit, region, shieldScale);
        }
        const HaloActorDef *def = haloactorvariant(target);
        float body = def ? def->bodyMultiplier(region) : -1;
        // Without actors.txt format 2: the retail multipliers of these bodies.
        if(body < 0) body = region == HALO_MATERIAL_HUNTER_SHIELD || region == HALO_MATERIAL_MONITOR ? 0 : 1;
        const bool tagged = source && source->hasMaterials;
        float modifier = tagged ? source->material(region) : 1;
        // Hits that carry no jpt! modifiers (actors' melee, the stock weapons, a weapon table
        // written before the `materials` lines): the armour's usual 0.5.
        if(!tagged && region == HALO_MATERIAL_HUNTER_ARMOR) modifier = .5f;
        return body * modifier;
    }
}

float haloactorcloak(const playerent *p)
{
    if(!haloactive() || !p || p == player1 || p->state != CS_ALIVE) return 0;
    const HaloActorDef *def = haloactorvariant(p);
    if(!def || !def->camouflaged()) return 0;
    const HaloEnemy *enemy = enemyState(p);
    // Super camouflage (the stealth majors) hides completely; plain camouflage nearly.
    float cloak = def->superCamouflage() ? 1.0f : .9f;
    // Firing and being hit make it flicker: weaker at once, recovering over a second.
    const int since = min(lastmillis - p->lastaction, enemy ? lastmillis - enemy->hurtAt : 100000);
    if(since >= 0 && since < 1000) cloak *= .35f + .65f * since / 1000.0f;
    return cloak;
}

float halomodelalpha(const playerent *p)
{
    const float cloak = haloactorcloak(p);
    if(cloak <= 0) return -1;
    // A faint refracting shimmer that shows more of the body as the cloak flickers weaker,
    // the same fading the player's own camouflage uses for its screen tint.
    const float shimmer = .5f + .5f * sinf(lastmillis * .013f + p->clientnum * 1.7f);
    float alpha = .05f + .08f * shimmer + (1 - cloak) * .6f;
    // The flicker is a function of time, not rnd(): the renderer must not draw on the game's
    // random sequence (it runs a different number of times per tick at every frame rate).
    if(cloak < .85f && ((lastmillis / 40 + p->clientnum) % 3) == 0) alpha += .2f;
    return clamp(alpha, .04f, .85f);
}

void halodamage(int &damage, playerent *target, playerent *attacker, int gun)
{
    if(damage <= 0 || damage == INT_MAX) return;
    // A hit from halohit carries its exact amount and side effect; any other is the whole number.
    const bool exact = pendingHit.target == target;
    float amount = exact ? pendingHit.damage : float(damage);
    const bool emp = exact && pendingHit.sideEffect == HALO_SIDE_EFFECT_EMP;
    const int side = exact ? pendingHit.sideEffect : 0;
    const HaloDamage *pendingSource = pendingHit.source;
    const PendingHit pendingAt = pendingHit;
    pendingHit.target = NULL;
    pendingHit.source = NULL;
    const float given = amount;
    auto trace = [&](float shieldLeft, float carry)
    {
        if(halo_damagetrace)
            conoutf("DAMAGE_TRACE attacker=%s target=%s exact=%d amount=%.3f side=%d gun=%d health=%d shield=%.2f carry=%.3f",
                    attacker ? (attacker == player1 ? "player" : attacker->name) : "-", target == player1 ? "player" : target->name,
                    exact, given, side, gun, damage, shieldLeft, carry);
    };
    // Playfighting actors go through the motions without hurting anyone.
    if(haloactorflags(attacker) & HALO_AI_PLAYFIGHT) { damage = 0; trace(-1, 0); return; }
    // object_cannot_take_damage and unit_impervious (traced with shield -1).
    if((target == player1 && playerProtected) || (haloactorflags(target) & (HALO_AI_NO_DAMAGE | HALO_AI_IMPERVIOUS))) { damage = 0; trace(-1, 0); return; }
    if(target == player1)
    {
        // Enemy fire hurts more on the higher difficulties.
        if(attacker != player1) amount *= halodifficultydamage();
        // In a Warthog the body takes some of the fire.
        if(halodriving()) amount = halovehicledamage(amount);
        shieldHit = lastmillis;
        shieldRecharging = false;
        const float factor = attacker != player1 && gun == GUN_SUBGUN ? 2 : 1;
        // The overshield's extra layers take the hit first.
        if(overshield > 0)
        {
            const float layer = min(overshield, amount * factor);
            overshield -= layer;
            amount = max(0.0f, amount - layer / factor);
            if(emp) overshield = overshieldTarget = 0;
            if(amount <= 1e-4f)
            {
                if(emp) shield = 0;
                haloplaysound(emp ? "shield-depleted" : "shield-hit", S_NULL);
                if(attacker && attacker != player1) halofxdamage(attacker->o, given, true);
                damage = 0;
                trace(shield, playerHealthCarry);
                return;
            }
        }
        const bool hadShield = shield > 0;
        const float absorbed = min(shield, amount * factor);
        shield -= absorbed;
        amount = max(0.0f, amount - absorbed / factor);
        if(emp) { shield = 0; overshield = overshieldTarget = 0; }
        if(hadShield) haloplaysound(shield > 0 ? "shield-hit" : "shield-depleted", S_NULL);
        damage = wholeDamage(amount, playerHealthCarry);
        // The damage-direction arc points at the attacker, for a hit the shield took too.
        if(attacker && attacker != player1) halofxdamage(attacker->o, given, damage <= 0);
        trace(shield, playerHealthCarry);
        return;
    }
    HaloEnemy *enemy = enemyState(target);
    if(!enemy) { trace(0, 0); return; }
    enemy->hurtAt = lastmillis;
    int region = -1;
    float shieldScale = 1;
    const float material = materialScale(*enemy, target, attacker, gun, exact ? pendingSource : NULL, region,
                                         exact ? &pendingAt : NULL, shieldScale);
    // The general material model's body multiplier (generalMaterial) is for the body: an energy
    // shield takes the hit at its own modifier (shieldScale), and only what passes it is scaled by
    // the body's. The special bodies (no shields) take theirs on the whole hit.
    const bool special = enemy->kind == HUNTER || enemy->kind == SENTINEL || enemy->kind == MONITOR || enemy->kind == ENGINEER;
    float bodyScale = 1;
    if(region >= 0)
    {
        if(special) amount *= material;
        else bodyScale = material;
        if(halo_damagetrace) conoutf("DAMAGE_MATERIAL target=%s kind=%d material=%d scale=%.3f", target->name, int(enemy->kind), region, material);
    }
    // Lethal to the unsuspecting (side effect 2, every campaign weapon's melee): the player's melee
    // on an actor from behind (beyond 120 degrees of where it faces) that is not fighting the player
    // (not in combat, or fighting someone else) takes the actor's whole shield and health, whatever
    // the shield's modifier (applied below, after the shield). A body its material leaves unhurt
    // (the Monitor, a Hunter's shield arm) stays unhurt; the actors' own melee is not affected.
    // Judgement call (batch 5's critic): only an actor hostile to the player now. The player's
    // allies (the marines, until a betrayal turns them) and the actors ai_attach made of story
    // figures (a50's Keyes and freed marines, c10's marine_suicidal, whatever their team) take
    // the melee's normal damage instead: the tags give the side effect to every weapon's melee and
    // say nothing of allies, and whether Halo CE's own check spares an ally that is not in combat
    // is not confirmed here, while one stray tap from behind would otherwise end a50 (Keyes's
    // death is game_lost_script's loss). halo_damagetrace logs DAMAGE_UNSUSPECTING_SPARED for them.
    bool unsuspecting = false;
    if(side == HALO_SIDE_EFFECT_UNSUSPECTING && halo_meleeunsuspecting && attacker == player1 && amount > 0 &&
       (region < 0 || material > 0))
    {
        vec delta = vec(attacker->o).sub(target->o);
        delta.z = 0;
        const float yaw = target->yaw * RAD;
        const float facing = delta.iszero() ? 1.0f : delta.normalize().dot(vec(sinf(yaw), -cosf(yaw), 0));
        const int status = haloactorstatus(target);
        if(facing < cosf(120 * RAD) && (status < 6 || haloactortarget(target) != attacker))
        {
            const bool attached = haloscenarioattachedactor(target);
            if(attached || !haloactorhostile(target, player1))
            {
                if(halo_damagetrace)
                    conoutf("DAMAGE_UNSUSPECTING_SPARED target=%s kind=%d team=%d attached=%d facing=%.3f", target->name, int(enemy->kind),
                            haloactorteam(target), attached, facing);
            }
            else
            {
                unsuspecting = true;
                if(halo_damagetrace)
                    conoutf("DAMAGE_UNSUSPECTING target=%s kind=%d status=%d facing=%.3f amount=%.2f", target->name, int(enemy->kind),
                            status, facing, max(amount, enemy->shield + float(max(target->health, 0)) + 1));
            }
        }
    }
    if(unsuspecting && enemy->shieldMax > 0 && enemy->shield > 0)
    {
        enemy->hitTime = lastmillis;
        enemy->shield = 0;
        particle_splash(PART_PLASMA, 6, 220, target->o);
    }
    if(enemy->shieldMax > 0)
    {
        enemy->hitTime = lastmillis;
        // The assault rifle's 0.7 on shields is its jpt! modifier for the elite energy shield; with
        // the general material model the shield takes the weapon's own modifier (generalMaterial).
        const float modifier = shieldScale != 1 ? shieldScale : gun == GUN_ASSAULT ? 0.7f : 1.0f;
        const float absorbed = modifier > 0 ? min(enemy->shield, amount * modifier) : 0;
        if(absorbed > 0 || (emp && enemy->shield > 0) || (modifier <= 0 && enemy->shield > 0))
        {
            enemy->shield -= absorbed;
            // A modifier of 0 (a weapon the shield is immune to): the shield stops the hit whole.
            amount = modifier > 0 ? max(0.0f, amount - absorbed / modifier) : 0;
            if(emp) enemy->shield = 0;
            particle_splash(PART_PLASMA, emp ? 12 : 6, 220, target->o);
        }
    }
    amount *= bodyScale;
    if(unsuspecting) amount = max(amount, float(max(target->health, 0)) + 1);
    damage = wholeDamage(amount, enemy->healthCarry);
    trace(enemy->shield, enemy->healthCarry);
}

bool halopickup(int index, playerent *p)
{
    if(!haloactive()) return false;
    if(p != player1) return true;
    entity &e = ents[index];
    int &grenades = p->mag[GUN_GRENADE];
    // Ammunition boxes refill the carried weapon they belong to, up to its tag maximum.
    auto refill = [&](const char *path, int fallbackGun, int fallbackRounds, int fallbackMax) -> bool
    {
        if(tagWeapons())
        {
            loopi(2)
            {
                const HaloWeaponDef *d = haloweapon(carried[i].def);
                const HaloMagazineDef *m = d ? d->magazine() : NULL;
                if(!d || d->path != path || !m) continue;
                syncCarried();
                const int room = m->maximum - carried[i].loaded - carried[i].reserve;
                if(room <= 0) return false;
                carried[i].reserve += min(room, m->loaded);
                p->ammo[carriedGun[i]] = carried[i].reserve;
                return true;
            }
            return false;
        }
        int &ammo = p->ammo[fallbackGun];
        if(ammo >= fallbackMax) return false;
        ammo = min(ammo + fallbackRounds, fallbackMax);
        return true;
    };
    switch(e.type)
    {
        case I_HEALTH:
            if(p->health == playerHealth) return true;
            p->health = playerHealth;
            break;
        case I_AMMO:
            if(!refill(defaultWeapons[0], GUN_ASSAULT, 60, 600)) return true;
            break;
        case I_CLIPS:
            if(!refill(defaultWeapons[1], GUN_PISTOL, 12, 120)) return true;
            break;
        case I_GRENADE:
            if(grenades >= grenadeMaximum(HALO_GRENADE_FRAG)) return true;
            ++grenades;
            break;
        default: return true;
    }
    e.spawned = false;
    itemstat *stats = p->itemstats(e.type);
    if(stats) audiomgr.playsound(stats->sound, SP_HIGH);
    return true;
}

const char *haloactormodel(const playerent *p)
{
    if(!haloactive()) return NULL;
    if(p == player1) return "halo/cyborg";
    HaloEnemy *enemy = enemyState(p);
    if(!enemy) return NULL;
    // Named bodies: Captain Keyes (the captain and captain_ingame variants, which fight as
    // marines) has his own model; a body whose model was not imported keeps a stand-in.
    const HaloActorDef *def = haloactorvariant(p);
    if(def && (def->family == "captain" || def->family == "captain_ingame")) return halomodelor("halo/keyes", profiles[enemy->kind].model);
    if(enemy->kind == MONITOR) return halomodelor(profiles[MONITOR].model, profiles[SENTINEL].model);
    if(enemy->kind == ENGINEER) return halomodelor(profiles[ENGINEER].model, profiles[GRUNT].model);
    return profiles[enemy->kind].model;
}

// `name` when its model loads, else `fallback` (each name is tried once).
const char *halomodelor(const char *name, const char *fallback)
{
    static std::vector<std::pair<std::string, bool>> tried;
    for(const auto &t : tried) if(t.first == name) return t.second ? name : fallback;
    const bool ok = loadmodel(name) != NULL;
    tried.emplace_back(name, ok);
    return ok ? name : fallback;
}

// First-person placement: where a viewmodel sits away from the place its retail animation holds
// it, in the view's frame (right, forward, up; AssaultCube units). The rocket launcher's tube rides
// the right shoulder with its top above eye height and its rear behind the eye; in AssaultCube's
// wider view (75 degrees vertical, where Halo's is 70 horizontal) it filled the right of the screen
// and crowded the reticle, so it sits lower, further right and further forward. A judgement call
// made without seeing it: `halo_viewmodeloffset <right> <forward> <up>` changes the current
// weapon's in the game (and with no arguments prints it), for tuning.
namespace
{
    struct ViewOffset { std::string path; vec offset; };
    std::vector<ViewOffset> viewOffsets = {
        {"weapons\\rocket launcher\\rocket launcher", vec(0.5f, 1.0f, -1.5f)},
    };

    ViewOffset *viewOffsetFor(const std::string &path, bool create)
    {
        for(ViewOffset &v : viewOffsets) if(v.path == path) return &v;
        if(!create) return NULL;
        viewOffsets.push_back({path, vec(0, 0, 0)});
        return &viewOffsets.back();
    }
}

bool halohudmodeloffset(int gun, vec &offset)
{
    if(!haloactive() || !tagWeapons()) return false;
    loopi(2) if(carriedGun[i] == gun)
    {
        const HaloWeaponDef *d = haloweapon(carried[i].def);
        const ViewOffset *v = d ? viewOffsetFor(d->path, false) : NULL;
        if(!v) return false;
        offset = v->offset;
        return true;
    }
    return false;
}

COMMANDF(halo_viewmodeloffset, "sss", (const char *x, const char *y, const char *z)
{
    const HaloWeaponDef *d = haloactive() && tagWeapons() ? haloweapon(carried[currentCarried].def) : NULL;
    if(!d) return;
    if(*x || *y || *z) viewOffsetFor(d->path, true)->offset = vec(float(atof(x)), float(atof(y)), float(atof(z)));
    const ViewOffset *v = viewOffsetFor(d->path, false);
    const vec o = v ? v->offset : vec(0, 0, 0);
    conoutf("Halo viewmodel offset for %s: %.2f %.2f %.2f (right, forward, up)", d->name.c_str(), o.x, o.y, o.z);
});

const char *halohudmodel(int gun)
{
    if(!haloactive()) return NULL;
    if(tagWeapons()) loopi(2) if(carriedGun[i] == gun)
    {
        const HaloWeaponDef *d = haloweapon(carried[i].def);
        if(d && d->viewmodel != "-") return d->viewmodel.c_str();
    }
    if(gun == GUN_ASSAULT) return "halo/ma5b";
    if(gun == GUN_PISTOL) return "halo/pistol";
    return NULL;
}

void halodrawhud()
{
    const char *const objectives[] = {
        "Follow the distress beacon", "Clear the Covenant patrol", "Reach the pass",
        "Clear the extraction zone", "Reach the extraction beacon", "Mission complete"
    };
    const int right = VIRTW - 74, left = 74;
    const float trackerX = 205, trackerY = VIRTH - 210, trackerRadius = 124;
    const float yaw = player1->yaw * RAD;
    const vec target = objective();
    vec bearing = vec(target).sub(player1->o);
    bearing.rotate_around_z(-yaw);
    // The script's nav points (halo_hud.cpp draws them) stand in for the route marker.
    HaloHudNav navs[16];
    const int navCount = haloscenarionavpoints(navs, 16);
    const int hidden = haloscenariohudhidden(NULL);

    glDisable(GL_TEXTURE_2D);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    if(lastmillis - shieldHit < 230 && shieldHit > 0)
    {
        int alpha = (230 - (lastmillis-shieldHit))/3;
        box(0, 0, VIRTW, 25, 70, 168, 255, alpha);
        box(0, VIRTH-25, VIRTW, 25, 70, 168, 255, alpha);
        box(0, 0, 25, VIRTH, 70, 168, 255, alpha);
        box(VIRTW-25, 0, 25, VIRTH, 70, 168, 255, alpha);
    }
    // The retail HUD tags draw the shield, health and motion sensor when present;
    // the boxes below are the fallback without packages/halo/hud.txt.
    const bool tagHud = halohudready() && tagWeapons();
    if(!tagHud)
    {
    if((hidden & (HALO_HUD_SHIELD | HALO_HUD_HEALTH)) != (HALO_HUD_SHIELD | HALO_HUD_HEALTH)) box(right-318, 72, 318, 120, 7, 24, 44, 160);
    if(!(hidden & HALO_HUD_SHIELD))
    {
        box(right-299, 123, 278, 16, 18, 60, 81, 210);
        box(right-299, 123, 278 * shield / playerShield, 16, 91, 216, 255, 235);
    }
    if(!(hidden & HALO_HUD_HEALTH)) loopi(8) box(right-299 + i*35, 151, 30, 13,
        player1->health * 8 > i * playerHealth ? 114 : 24,
        player1->health * 8 > i * playerHealth ? 217 : 61,
        player1->health * 8 > i * playerHealth ? 232 : 86, 225);
    if(!(hidden & HALO_HUD_MOTION))
    {
    glColor4ub(8, 30, 48, 164);
    glBegin(GL_TRIANGLE_FAN);
    glVertex2f(trackerX, trackerY);
    loopi(41)
    {
        float angle = i * PI2 / 40.0f;
        glVertex2f(trackerX + cosf(angle)*trackerRadius, trackerY + sinf(angle)*trackerRadius);
    }
    glEnd();
    ring(trackerX, trackerY, trackerRadius, 79, 183, 216, 190);
    ring(trackerX, trackerY, trackerRadius*0.5f, 70, 144, 169, 120);
    box(trackerX-4, trackerY-4, 8, 8, 111, 220, 244, 255);
    loopv(enemies)
    {
        const botent *bot = enemies[i].bot;
        if(!bot || bot->state != CS_ALIVE || (bot->vel.sqrxy() < 0.01f && lastmillis-bot->lastaction > 500)) continue;
        vec delta = vec(bot->o).sub(player1->o);
        if(delta.sqrxy() > 58.0f*58.0f) continue;
        delta.rotate_around_z(-yaw);
        // Allies show yellow on the motion tracker, hostiles red.
        const bool ally = haloteamfriendly(enemies[i].team);
        box(trackerX + delta.x*2-7, trackerY + delta.y*2-7, 14, 14, 255, ally ? 220 : 74, ally ? 60 : 81, 250);
    }
    }
    }
    if(stage != COMPLETE && !navCount)
    {
        const float direction = atan2f(bearing.x, -bearing.y);
        float x = VIRTW * 0.5f + clamp(direction / PI, -0.9f, 0.9f) * VIRTW * 0.42f;
        float y = 170.0f;
        if(!preview)
        {
            vec4 clip;
            mvpmatrix.transform(vec(target).add(vec(0, 0, 2.5f)), clip);
            if(clip.w > 0.01f)
            {
                x = VIRTW * clamp(0.5f + 0.5f * clip.x / clip.w, 0.045f, 0.955f);
                y = VIRTH * clamp(0.5f - 0.5f * clip.y / clip.w, 0.14f, 0.83f);
            }
        }
        glColor4ub(105, 218, 255, 240);
        glBegin(GL_LINE_LOOP);
        glVertex2f(x, y-12); glVertex2f(x+12, y); glVertex2f(x, y+12); glVertex2f(x-12, y);
        glEnd();
    }
    glEnable(GL_TEXTURE_2D);
    if(ending)
    {
        drawEnding();
        return;
    }
    if(stage != COMPLETE) halohuddrawnav(navs, navCount);
    if(!tagHud && !(hidden & HALO_HUD_SHIELD)) label("SHIELD", right-299, 79);
    if(escaping && stage != COMPLETE)
    {
        int seconds = max(0, (escapeDeadline - lastmillis + 999) / 1000);
        defformatstring(timer)("ESCAPE  %d:%02d", seconds / 60, seconds % 60);
        label(timer, right, 203, 0.61f, true);
    }
    if(!preview || mission != 1 || section != 0)
    {
        defformatstring(title)("%s  %d/%d", haloMissions[mission].title, section + 1, haloMissions[mission].sections);
        label(title, left, 262, 0.56f);
    }
    // Mission text sits below the retail weapon and grenade panels in the top-left corner.
    const char *task = objectives[stage];
    if(mission != 1 || section != 0 || !preview)
    {
        const char *const sectionObjectives[] = {
            "Follow the route", "Clear the hostiles", "Reach the checkpoint",
            "Clear the hostiles", "Reach the next sector", "Section complete"
        };
        task = campaignFinished ? "Campaign complete" : sectionObjectives[stage];
        if(stage == INSERTION || stage == REACH_PASS)
        {
            if(const char *authored = haloscenarioobjective()) task = authored;
        }
        if(stage == EXTRACTION)
        {
            const char *interaction = sectionInteraction();
            if(interaction) task = interaction;
            else if(section + 1 == haloMissions[mission].sections) task = haloMissions[mission].finalObjective;
            else if(retailCartographer() && section == 0) task = "Reach the Cartographer entrance";
        }
    }
    vec control;
    bool mapped = false;
    const bool cartographer = stage != COMPLETE && halocartographertarget(control, mapped);
    if(cartographer) task = mapped ? "Return to the surface" : "Access the Cartographer";
    if(retailCartographer() && section == 2 && stage != COMPLETE && !halopelicanready()) task = "Reach the Pelican landing zone";
    label(task, left, 301, 0.72f);
    if(player1->state == CS_DEAD)
    {
        label("Click to revert to checkpoint", VIRTW/2-240, VIRTH/2+100, 0.65f);
        return;
    }
    if(stage == COMPLETE)
    {
        label(preview || campaignFinished ? "F5  Restart section" : "Enter  Continue   /   F5  Restart section", left, 350);
        return;
    }
    if((cartographer && !mapped && player1->o.dist(control) < 13.0f) ||
       (stage == EXTRACTION && sectionInteraction() &&
        (retailCartographer() ? section == 2 && halopelicanready() : !route[2].inrange(nextRouteMarker)) &&
        near(extraction, markerRadius)))
        label("E  Interact", left, 353);
    else
    {
        defformatstring(distance)("%d m", int(bearing.magnitudexy()/2.75f + 0.5f));
        label(distance, left, 353);
    }
    if(halodriving())
    {
        drawVehicleHud(tagHud, right, yaw);
        return;
    }
    const weapon *current = player1->weaponsel;
    const HaloWeaponDef *def = tagWeapons() ? haloweapon(carried[currentCarried].def) : NULL;
    if(!def && tagHud)
    {
        // Unarmed (a10 before the bridge): the retail HUD's unit panels, no weapon or reticle.
        HaloHudWeapon w;
        HaloHudBlip blips[64];
        const int count = sensorBlips(blips, 64, yaw);
        halohuddraw(w, hudUnit(), blips, count);
    }
    else if(def && tagHud)
    {
        // Feed the retail HUD: weapon state, shields and health, grenades and contacts.
        const HaloWeaponState &st = carried[currentCarried];
        HaloHudWeapon w;
        w.weapon = def->index;
        w.loaded = current->mag;
        w.reserve = current->ammo;
        w.magazine = def->magazine() ? def->magazine()->loaded : 0;
        w.maximum = def->magazine() ? def->magazine()->maximum : 0;
        w.battery_weapon = def->usesBattery();
        w.heat = def->heatOverheat > 0 ? clamp(st.heat / def->heatOverheat, 0.0f, 1.0f) : 0;
        w.battery = st.battery;
        w.overheated = st.overheated;
        w.reloading = current->reloading != 0;
        w.charging = st.chargeStart != 0;
        w.zoom = st.zoom;
        vec from = player1->o, aim = vec(worldpos).sub(from);
        w.locked = !aim.iszero() && haloaimtarget(from, aim.normalize(), *def);
        HaloHudBlip blips[64];
        const int count = sensorBlips(blips, 64, yaw);
        halohuddraw(w, hudUnit(), blips, count);
        // A weapon whose HUD tags show no ammunition (the flamethrower): its fuel as text.
        bool reticle, ammo;
        halohudweaponparts(def->index, reticle, ammo);
        if(!ammo)
        {
            if(def->usesBattery())
            {
                defformatstring(battery)("%s %d%%", st.overheated ? "OVERHEATED" : "BATTERY", int(st.battery * 100 + .5f));
                label(battery, right, VIRTH-173, 0.70f, true);
            }
            else
            {
                defformatstring(rounds)("%02d / %03d", current->mag, current->ammo);
                label(rounds, right, VIRTH-173, 0.96f, true);
            }
        }
    }
    else if(def)
    {
        const HaloWeaponState &s = carried[currentCarried];
        string title;
        copystring(title, def->name.c_str());
        for(char *c = title; *c; ++c) *c = toupper(*c);
        label(title, right, VIRTH-212, 0.60f, true);
        if(def->usesBattery())
        {
            // Covenant weapons show battery and heat instead of rounds.
            glDisable(GL_TEXTURE_2D);
            box(right-260, VIRTH-178, 260, 14, 18, 60, 81, 210);
            box(right-260, VIRTH-178, 260 * s.battery, 14, 120, 230, 120, 235);
            const float heat = def->heatOverheat > 0 ? clamp(s.heat / def->heatOverheat, 0.0f, 1.0f) : 0;
            box(right-260, VIRTH-158, 260, 10, 18, 60, 81, 210);
            box(right-260, VIRTH-158, 260 * heat, 10, 255, s.overheated ? 60 : 160, 40, 235);
            glEnable(GL_TEXTURE_2D);
            defformatstring(battery)("%d%%", int(s.battery * 100 + .5f));
            label(s.overheated ? "OVERHEATED" : s.charged ? "CHARGED" : battery, right-270, VIRTH-182, 0.6f, true);
        }
        else
        {
            defformatstring(ammo)("%02d / %03d", current->mag, current->ammo);
            label(ammo, right, VIRTH-173, 0.96f, true);
        }
        const HaloWeaponDef *other = haloweapon(carried[currentCarried ^ 1].def);
        if(other) label(other->name.c_str(), right, VIRTH-240, 0.45f, true);
        if(s.zoom > 0)
        {
            defformatstring(zoom)("%.0fx", def->magnification(s.zoom));
            label(zoom, VIRTW/2+40, VIRTH/2+30, 0.55f);
        }
    }
    else if(def || !tagWeapons())
    {
        // Without the retail HUD (or without weapons.txt): the plain weapon labels.
        const char *name = current->type == GUN_PISTOL ? "M6D MAGNUM" : current->type == GUN_KNIFE ? "MELEE" : "MA5B ASSAULT RIFLE";
        label(name, right, VIRTH-212, 0.60f, true);
        defformatstring(ammo)("%02d / %03d", current->mag, current->ammo);
        label(ammo, right, VIRTH-173, 0.96f, true);
    }
    if(const char *vehicle = halovehicleprompt(nearbyWeaponDistance())) label(vehicle, VIRTW/2-200, VIRTH/2+90, 0.6f);
    else if(const char *board = haloscenarioboardprompt()) label(board, VIRTW/2-200, VIRTH/2+90, 0.6f);
    else if(groundWeapons.inrange(nearbyWeapon))
    {
        const HaloWeaponDef *d = haloweapon(groundWeapons[nearbyWeapon].state.def);
        defformatstring(prompt)("E  Pick up %s", d ? d->name.c_str() : "weapon");
        label(prompt, VIRTW/2-200, VIRTH/2+90, 0.6f);
    }
    if(pickupMessage && lastmillis - pickupMessage < 1600) label(pickupText, VIRTW/2-200, VIRTH/2+130, 0.55f);
    // Both grenade counts, the selected type in brackets (H switches); the retail panel above
    // shows the selected type's count.
    const int frags = grenadeCount(HALO_GRENADE_FRAG), plasmas = grenadeCount(HALO_GRENADE_PLASMA);
    defformatstring(grenades)(grenadeType == HALO_GRENADE_PLASMA ? "FRAG %d   [PLASMA %d]" : "[FRAG %d]   PLASMA %d", frags, plasmas);
    if(tagHud) label(grenades, left, 212, 0.5f);
    else label(grenades, right, VIRTH-100, 0.57f, true);
    if(overshield > 0 && !tagHud)
    {
        glDisable(GL_TEXTURE_2D);
        box(right-299, 141, 278 * clamp(overshield / max(shieldCap, 1.0f) * .5f, 0.0f, 1.0f), 6, 120, 255, 120, 235);
        glEnable(GL_TEXTURE_2D);
    }
    const float camo = haloplayercamo();
    if(camo > 0)
    {
        glDisable(GL_TEXTURE_2D);
        box(0, 0, VIRTW, VIRTH, 150, 190, 230, int(22 * camo));
        glEnable(GL_TEXTURE_2D);
        defformatstring(cloak)("ACTIVE CAMOUFLAGE  %d", max(0, (camoUntil - lastmillis + 999) / 1000));
        label(cloak, VIRTW/2 - 230, 120, 0.5f);
    }
}

COMMANDF(halo_campaign, "", ()
{
    loadProgress();
    if(installed(savedMission, savedSection)) queueSection(savedMission, savedSection);
    else if(!installed(0, 0) && installed(1, 0)) queueSection(1, 0, true);
    else queueSection(0, 0);
});
COMMANDF(halo_preview, "", () { queueSection(1, 0, true); });
COMMANDF(halo_section, "ii", (int *m, int *s)
{
    if(*m >= 1 && *m <= haloMissionCount && *s >= 1 && *s <= haloMissions[*m - 1].sections)
        queueSection(*m - 1, *s - 1, true);
});
COMMANDF(halo_restart, "", ()
{
    if(!haloactive()) return;
    keepSectionLoadout = true;
    queueSection(mission, section, preview);
});
COMMANDF(halo_next, "", ()
{
    if(ending) { finishEnding(); return; }
    if(!halocinematicskip()) nextSection();
});
COMMANDF(halo_use, "", ()
{
    // In a seat E gets out (or, upside down, is thrown clear); aboard a Pelican, out of it.
    if(halodriving()) { if(haloscenarioboarded()) haloscenarioboard(); else halovehicleuse(0); return; }
    if(haloscenariouse()) return;
    // A Pelican the script made enterable (a30's extraction): aboard.
    if(haloactive() && !halocinematic() && player1->state == CS_ALIVE && !halovehicleprompt(nearbyWeaponDistance()) && haloscenarioboard()) return;
    // A vehicle seat or a weapon on the ground, whichever is nearer.
    if(haloactive() && !halocinematic() && player1->state == CS_ALIVE && halovehicleuse(nearbyWeaponDistance())) return;
    if(haloactive() && !halocinematic() && player1->state == CS_ALIVE && swapWeapon()) return;
    if(retailCartographer() && (section != 2 || !halopelicanready())) return;
    if(haloactive() && !halocinematic() && player1->state == CS_ALIVE && stage == EXTRACTION && sectionInteraction() &&
       (retailCartographer() || !route[2].inrange(nextRouteMarker)) && near(extraction, markerRadius))
    {
        audiomgr.playsound(S_ITEMAMMO, SP_HIGH);
        finishSection();
    }
});
COMMANDF(halo_mission, "i", (int *index)
{
    loadProgress();
    if(*index >= 1 && *index <= haloMissionCount) queueSection(*index - 1, 0);
});

// ---- Title menu and saved games (config/halo.cfg builds the menus from these)

// Missions the player may pick: up to the furthest reached, every one once the campaign is beaten.
static int halounlockedmissions()
{
    loadProgress();
    return campaignBeaten ? haloMissionCount : clamp(savedMission + 1, 1, haloMissionCount);
}

// Shows the title menu: on launch, or from the pause menu (the campaign stays paused under it).
static void halotitle() { showtitle(); }
COMMANDN(halo_title, halotitle, "");

// New Game: the difficulty, then The Pillar of Autumn from its start. Unlocked missions stay.
static void halonewgame(int *difficulty)
{
    loadProgress();
    defformatstring(set)("halo_difficulty %d", clamp(*difficulty, 0, 3));
    execute(set);
    sectionLoadout.valid = carryLoadout.valid = restorePending = false;
    if(!installed(0, 0) && installed(1, 0)) queueSection(1, 0, true);
    else queueSection(0, 0);
    // A warning about the old progress file (a corrupt file restored) is done with once a game starts.
    if(launchPending) progressNote[0] = '\0';
}
COMMANDN(halo_new_game, halonewgame, "i");

// Continue: the saved section at the saved checkpoint, with the saved loadout and difficulty.
static void halocontinue()
{
    loadProgress();
    if(!savedGame.valid || !installed(savedGame.mission, savedGame.section))
    {
        conoutf("Halo: no saved game to continue");
        return;
    }
    defformatstring(set)("halo_difficulty %d", savedGame.difficulty);
    execute(set);
    carryLoadout.valid = false;
    queueSection(savedGame.mission, savedGame.section);
    if(!launchPending) return;
    // The last warning is cleared; restoring the saved state sets a new one if it does not fit.
    progressNote[0] = '\0';
    restorePending = true;
    incomingEscapeTime = savedGame.escapeMillis;
}
COMMANDN(halo_continue, halocontinue, "");

static void halocancontinue()
{
    loadProgress();
    intret(savedGame.valid && installed(savedGame.mission, savedGame.section) ? 1 : 0);
}
COMMANDN(halo_can_continue, halocancontinue, "");

static const char *const difficultyNames[4] = {"Easy", "Normal", "Heroic", "Legendary"};

// "The Pillar of Autumn  2/7  Normal  (checkpoint)" for the Continue entry.
static void halocontinuelabel()
{
    loadProgress();
    if(!savedGame.valid) { result(""); return; }
    // A state block saved with another .hscb cannot resume the script: say so before Continue.
    unsigned script = 0;
    int format = 0;
    const bool stale = !savedGame.state.empty() && sscanf(savedGame.state.c_str(), "state %d %*d %*d %*d %x", &format, &script) == 2 &&
                       script != haloscenariofilehash(savedGame.mission);
    defformatstring(label)("%s  %d/%d  %s%s%s", haloMissions[savedGame.mission].title, savedGame.section + 1,
        haloMissions[savedGame.mission].sections, difficultyNames[clamp(savedGame.difficulty, 0, 3)],
        savedGame.checkpoint ? "  (checkpoint)" : "", stale ? "  (scripts changed: the section's script restarts)" : "");
    result(label);
}
COMMANDN(halo_continue_label, halocontinuelabel, "");

static void halounlocked() { intret(halounlockedmissions()); }
COMMANDN(halo_unlocked, halounlocked, "");

// 1-based mission: 1 can be played, 0 locked, 2 not imported.
static void halomissionstate(int *index)
{
    const int m = *index - 1;
    if(m < 0 || m >= haloMissionCount) { intret(0); return; }
    if(!installed(m, 0)) { intret(2); return; }
    intret(m < halounlockedmissions() ? 1 : 0);
}
COMMANDN(halo_mission_state, halomissionstate, "i");

static void halomissiontitle(int *index)
{
    result(*index >= 1 && *index <= haloMissionCount ? haloMissions[*index - 1].title : "");
}
COMMANDN(halo_mission_title, halomissiontitle, "i");

static void halodifficultyname() { result(difficultyNames[halodifficulty()]); }
COMMANDN(halo_difficulty_name, halodifficultyname, "");

// The warning from loading the progress file (a corrupt file restored from its backup), or "".
static void haloprogressnote() { loadProgress(); result(progressNote); }
COMMANDN(halo_progress_note, haloprogressnote, "");
COMMANDF(halo_switch, "d", (bool down) { if(down) haloswitchweapon(); });
// X: the flashlight (halo_fx.cpp).
COMMANDF(halo_flashlight, "d", (bool down) { if(down) halofxflashlighttoggle(); });

int haloenemykind(const playerent *p, int *team)
{
    HaloEnemy *enemy = p ? enemyState(p) : NULL;
    if(team) *team = enemy ? enemy->team : -1;
    return enemy ? int(enemy->kind) : -1;
}

void haloerasebot(playerent *p)
{
    if(!p || p->type != ENT_BOT) return;
    loopv(bots) if(bots[i] == p)
    {
        // The grenades it threw go with it: a live one would explode with a deleted owner
        // (haloactornoise reads it). Its bolts in flight are dropped by haloupdatebolts.
        removebounceents(p);
        botent *d = bots.remove(i);
        delete d->pBot;
        d->pBot = NULL;
        freebotent(d);
        return;
    }
}

void haloremoveactor(const playerent *p)
{
    loopv(enemies) if(enemies[i].bot == p) enemies[i].bot = NULL;
    halogrenadeforget(p);
    halovehicleforget(p);
    haloactordetach(p);
    haloscenarioremovebot(p);
}

COMMANDF(halo_frag, "d", (bool down) { if(down) halofrag(); });
COMMANDF(halo_grenade_switch, "d", (bool down) { if(down) switchGrenade(); });

int haloplayergrenades(int type)
{
    if(type < 0) return grenadeCount(HALO_GRENADE_FRAG) + grenadeCount(HALO_GRENADE_PLASMA);
    return type < HALO_GRENADE_TYPES ? grenadeCount(type) : 0;
}

int haloplayergrenadetype() { return grenadeType; }

int haloplayerusegrenade()
{
    if(grenadeCount(grenadeType) <= 0)
    {
        // Halo throws the other type once the selected one has run out.
        if(!grenadeKnown(grenadeType ^ 1) || grenadeCount(grenadeType ^ 1) <= 0) return -1;
        grenadeType ^= 1;
    }
    const int type = grenadeType;
    --grenadeCount(type);
    if(grenadeCount(type) <= 0 && grenadeKnown(type ^ 1) && grenadeCount(type ^ 1) > 0) grenadeType ^= 1;
    return type;
}

float haloplayercamo()
{
    if(!haloactive() || player1->state != CS_ALIVE || lastmillis >= camoUntil) return 0;
    return clamp((camoUntil - lastmillis) / float(camoFade), 0.0f, 1.0f);
}

// Script hooks for scenario equipment with a name: object_create shows it, object_destroy takes it away.
void haloequipmentobject(int object, bool exists, bool reset)
{
    if(object < 0) return;
    loopv(groundItems)
    {
        GroundItem &item = groundItems[i];
        if(item.object != object) continue;
        if(!exists) { item.exists = false; continue; }
        if(item.exists && !reset) continue;
        item.exists = itemDifficultyAllows(item.flags);
        item.created = true;
    }
}

// After the mission script started (or resumed) on this section: equipment the script has already created is there.
void haloequipmentsync()
{
    loopv(groundItems)
    {
        GroundItem &item = groundItems[i];
        if(!item.created && item.object >= 0 && haloscenarioobjectvisible(item.object))
        {
            item.created = true;
            item.exists = itemDifficultyAllows(item.flags);
        }
    }
}
COMMANDF(halo_melee, "d", (bool down) { if(down) halomelee(); });

COMMANDF(halo_slot, "i", (int *slot)
{
    if(!haloactive() || halocinematic() || !keypressed) return;
    if(!tagWeapons()) execute(*slot == 2 ? "weapon pistol" : "weapon assault");
    else if((*slot == 2) != (currentCarried == 1)) haloswitchweapon();
});

void haloplaysound(const char *sample, int fallback, physent *actor, const vec *position)
{
    int id = fallback;
    if(haloactive() && sample)
    {
        defformatstring(name)("halo/%s", sample);
        defformatstring(path)("packages/audio/%s.wav", name);
        if(fileexists(findfile(path, "rb"), "rb"))
        {
            id = audiomgr.findsound(name, 220, gamesounds);
            if(id < 0) id = audiomgr.addsound(name, 220, -1, false, gamesounds, true, 0);
        }
    }
    if(id < 0 || id == S_NULL) return;
    // A decoded tag sample plays at its retail sound class's gain (sound_class_set_gain).
    const float gain = haloactive() && sample ? halofxclassgain(halofxsampleclass(sample)) : 1;
    if(gain <= .01f) return;
    location *played = NULL;
    if(position) played = audiomgr._playsound(id, staticreference(*position), SP_NORMAL);
    else if(actor && actor != player1) played = audiomgr._playsound(id, physentreference(actor), SP_NORMAL);
    else played = audiomgr._playsound(id, camerareference(), SP_HIGH);
    if(played && gain < .99f) played->setvolume(gain);
}

// A decoded tag sample looped from the camera while `active` (the flamethrower's fire, the plasma
// pistol's charge hum), at its sound class's gain; one at a time: another sample stops the last.
void haloloopsound(const char *sample, bool active)
{
    static int current = -1;
    int id = -1;
    if(sample && active)
    {
        defformatstring(name)("halo/%s", sample);
        defformatstring(path)("packages/audio/%s.wav", name);
        if(fileexists(findfile(path, "rb"), "rb"))
        {
            id = audiomgr.findsound(name, 220, gamesounds);
            if(id < 0) id = audiomgr.addsound(name, 220, -1, true, gamesounds, true, 0);
        }
    }
    if(current >= 0 && current != id) audiomgr.updateloopsound(current, false);
    current = id;
    // The class gain through setvolume (the sample's configured volume times it, set every frame so
    // a fade back to 1 restores it): updateloopsound's own volume sets the raw source gain.
    if(id >= 0)
        if(location *l = audiomgr.updateloopsound(id, true)) l->setvolume(halofxclassgain(halofxsampleclass(sample)));
}

COMMANDF(halo_pause, "", ()
{
    if(haloactive())
    {
        player1->attacking = false;
        player1->stopmoving();
        showmenu("Halo Campaign");
    }
    // Before a campaign has started, Escape brings the title menu back.
    else if(titleScreen) showmenu("Halo");
    else execute("key_escape");
});

// The title screen behind the title menu: before a campaign starts it hides the stock level the
// engine boots into; over a paused campaign it stays until the title menu is closed.
bool halotitleactive()
{
    if(!titleScreen) return false;
    if(haloactive() && !menuvisible()) titleScreen = false;
    return titleScreen;
}

void halodrawtitle()
{
    glDisable(GL_TEXTURE_2D);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glBegin(GL_TRIANGLE_STRIP);
    glColor4ub(4, 10, 22, 255); glVertex2f(0, 0); glVertex2f(VIRTW, 0);
    glColor4ub(10, 32, 52, 255); glVertex2f(0, VIRTH); glVertex2f(VIRTW, VIRTH);
    glEnd();
    glEnable(GL_TEXTURE_2D);
    label("HALO", VIRTW/2 - int(text_width("HALO") * 1.6f), VIRTH/8, 3.2f);
    label("Combat Evolved", VIRTW/2 - int(text_width("Combat Evolved") * 0.45f), VIRTH/8 + 230, 0.9f);
    if(progressNote[0])
    {
        glPushMatrix();
        glTranslatef(VIRTW/2 - text_width(progressNote) * 0.3f, VIRTH - 260, 0);
        glScalef(0.6f, 0.6f, 1);
        draw_text(progressNote, 0, 0, 255, 120, 90, 255);
        glPopMatrix();
    }
    if(!menuvisible()) label("Esc  Menu", VIRTW/2 - int(text_width("Esc  Menu") * 0.3f), VIRTH - 160, 0.6f);
}


// The player is in a vehicle seat (halo_vehicle.cpp): its controls replace walking and the carried weapons.
// In a vehicle seat, or aboard a Pelican (halo_script_ai.inc): no walking, firing or pickups.
bool halodriving() { return haloactive() && !editmode && (halovehicleseated() || haloscenarioboarded()); }

void halorendervehicle()
{
    halorenderweapons();
    halovehiclesrender();
}

const HaloWeaponDef *haloweaponof(const playerent *p, int gun, HaloWeaponState **state)
{
    if(!haloactive() || !p) return NULL;
    if(p == player1)
    {
        if(!tagWeapons()) return NULL;
        loopi(2) if(carriedGun[i] == gun)
        {
            if(state) *state = &carried[i];
            return haloweapon(carried[i].def);
        }
        return NULL;
    }
    HaloEnemy *enemy = enemyState(p);
    if(!enemy || gun != GUN_SUBGUN) return NULL;
    if(state) *state = &enemy->weapon;
    return haloweapon(enemy->weapon.def);
}

bool halocarriesgun(int gun)
{
    if(!tagWeapons()) return gun == GUN_ASSAULT || gun == GUN_PISTOL;
    // An empty slot's knife is not a weapon to select while the other slot is armed.
    loopi(2) if(gun == carriedGun[i] && (slotArmed(i) || !slotArmed(i ^ 1))) return true;
    return false;
}

bool haloplayerunarmed() { return haloactive() && unarmed(); }

void haloswitchweapon()
{
    if(!haloactive() || halocinematic() || halodriving() || player1->state != CS_ALIVE) return;
    if(!tagWeapons())
    {
        execute(player1->weaponsel->type == GUN_ASSAULT ? "weapon pistol" : "weapon assault");
        return;
    }
    // With one weapon (or none) there is nothing to switch to.
    if(!slotArmed(currentCarried ^ 1)) return;
    if(!player1->weaponsel->deselectable() || player1->weaponchanging) return;
    syncCarried();
    selectCarried(currentCarried ^ 1, false);
}

void haloattackinput(bool down)
{
    if(!haloactive() || !tagWeapons() || player1->state != CS_ALIVE) return;
    HaloWeaponState &s = carried[currentCarried];
    const HaloWeaponDef *d = haloweapon(s.def);
    const HaloTriggerDef *t = d ? d->trigger() : NULL;
    if(!t || t->charge <= 0 || !d->trigger(1)) return;
    if(down)
    {
        if(s.canfire(*d)) s.chargeStart = lastmillis;
        return;
    }
    // Releasing a charged trigger fires the second trigger's shot.
    const bool charged = s.charged && s.canfire(*d);
    s.chargeStart = 0;
    s.charged = false;
    if(!charged || halocinematic()) return;
    vec from = player1->o, to = worldpos;
    from.z -= weapon::weaponbeloweye;
    halofiretrigger(player1, *d, s, 1, from, to);
    haloplaysound(d->sound(HALO_SOUND_CHARGED) ? d->sound(HALO_SOUND_CHARGED) : d->sound(HALO_SOUND_FIRE), S_NULL);
    player1->weaponsel->updatetimers(lastmillis);
    player1->lastaction = lastmillis;
}

float halozoom()
{
    if(!haloactive() || !tagWeapons() || player1->state != CS_ALIVE) return 1;
    const HaloWeaponState &s = carried[currentCarried];
    const HaloWeaponDef *d = haloweapon(s.def);
    return d && s.zoom > 0 ? d->magnification(s.zoom) : 1;
}

void halounzoom()
{
    if(!tagWeapons()) return;
    loopi(2) carried[i].zoom = 0;
    if(player1->weaponsel->type == GUN_SNIPER) ((sniperrifle *)player1->weaponsel)->setscope(false);
}

int halomeleedamage()
{
    const HaloWeaponDef *d = tagWeapons() ? haloweapon(carried[currentCarried].def) : NULL;
    // A full-strength melee hit rolls the jpt! upper bound (50-60 for the human weapons).
    return d && d->melee.upperMax > 0 ? int(d->melee.roll() + .5f) : 50;
}

playerent *haloaimtarget(const vec &from, const vec &direction, const HaloWeaponDef &def)
{
    if(def.autoaimAngle <= 0 || def.autoaimRange <= 0) return NULL;
    const float range = def.autoaimRange * haloscenarioscale(), cosine = cosf(def.autoaimAngle);
    playerent *best = NULL;
    float bestCosine = cosine;
    loopv(enemies)
    {
        botent *bot = enemies[i].bot;
        if(!bot || bot->state != CS_ALIVE || haloteamfriendly(enemies[i].team) || haloactorpassive(bot)) continue;
        // Aim at the upper body rather than the eyes.
        vec target = bot->o;
        target.z -= bot->eyeheight * 0.35f;
        vec delta = vec(target).sub(from);
        const float distance = delta.magnitude();
        if(distance < 0.01f || distance > range) continue;
        const float c = delta.dot(direction) / distance;
        if(c < bestCosine) continue;
        vec end = target;
        if(intersectgeometry(from, end)) continue;
        bestCosine = c;
        best = bot;
    }
    return best;
}

void halokilled(playerent *victim, playerent *killer)
{
    if(!haloactive() || !victim || victim == player1) return;
    const bool keepsItems = !(haloactorflags(victim) & HALO_AI_NO_DROPS);
    const int grenadeKind = haloactorgrenadetype(victim);
    haloactordeath(victim, killer);
    HaloEnemy *enemy = enemyState(victim);
    if(!enemy) return;
    const vec feet = feetOf(victim);
    if(enemy->kind == CARRIER) floodSpawns.add(FloodSpawn{feet, victim->yaw, INFECTION, 4 + rnd(5), lastmillis});
    else if((enemy->kind == MARINE || enemy->kind == ELITE) && corpses.length() < 32)
        corpses.add(Corpse{feet, victim->yaw, enemy->kind, lastmillis + corpseLife});
    // unit_doesnt_drop_items leaves nothing behind. Actors drop the grenades they still
    // carry (at most two), beside the body, then their weapon.
    if(!keepsItems) return;
    const HaloEquipmentDef *grenade = grenadeKind >= 0 ? halogrenadeequipment(grenadeKind) : NULL;
    loopi(min(victim->mag[GUN_GRENADE], 2))
    {
        const float angle = (victim->yaw + 90 + i * 140) * RAD;
        dropItem(grenade, vec(feet).add(vec(cosf(angle), sinf(angle), 0).mul(.8f)), victim->yaw + i * 60);
    }
    const HaloWeaponDef *d = haloweapon(enemy->weapon.def);
    if(!d || !d->playable()) return;
    // A dropped weapon keeps part of its battery or magazine.
    HaloWeaponState dropped;
    dropped.reset(d);
    if(d->usesBattery()) dropped.battery = 0.3f + rndscale(0.6f);
    else
    {
        dropped.loaded = max(1, int(dropped.loaded * (0.3f + rndscale(0.7f))));
        dropped.reserve = int(dropped.reserve * rndscale(0.5f));
    }
    dropWeapon(dropped, feetOf(victim), victim->yaw);
}

// Scripted mass kills (ai_erase_all) are cleanup, so they leave no burst or bodies behind.
void haloclearflood() { clearFlood(); }

bool halonearestcorpse(const vec &from, float range, vec &body)
{
    float best = range;
    bool found = false;
    loopv(corpses)
    {
        const float d = corpses[i].o.dist(from);
        if(d < best) { best = d; body = corpses[i].o; found = true; }
    }
    return found;
}

void halorenderweapons()
{
    if(!haloactive()) return;
    loopv(groundWeapons)
    {
        const GroundWeapon &g = groundWeapons[i];
        const HaloWeaponDef *d = haloweapon(g.state.def);
        if(!d || d->worldmodel == "-") continue;
        // Placed weapons sway slowly so they read as pickups.
        vec o = g.o;
        o.z += 0.35f + 0.12f * sinf(lastmillis / 400.0f + i);
        rendermodel(d->worldmodel.c_str(), ANIM_MAPMODEL|ANIM_LOOP, 0, -1, o, 0, g.yaw + 90, 0, 0, 0);
    }
    loopv(groundItems)
    {
        const GroundItem &item = groundItems[i];
        const HaloEquipmentDef *d = haloequipment(item.def);
        if(!item.exists || !d || d->worldmodel == "-") continue;
        // Powerups float and turn; ammunition, grenades and health packs lie on the floor.
        const bool powerup = d->powerup == HALO_POWERUP_OVERSHIELD || d->powerup == HALO_POWERUP_CAMO;
        vec o = item.o;
        o.z += powerup ? 1.2f + 0.15f * sinf(lastmillis / 500.0f + i) : 0.1f;
        const float yaw = powerup ? fmodf(lastmillis / 20.0f, 360.0f) : item.yaw + 90;
        rendermodel(d->worldmodel.c_str(), ANIM_MAPMODEL|ANIM_LOOP, 0, -1, o, 0, yaw, 0, 0, 0);
    }
}

void halozoomstep()
{
    if(!haloactive() || !tagWeapons() || halocinematic() || halodriving() || player1->state != CS_ALIVE) return;
    HaloWeaponState &s = carried[currentCarried];
    const HaloWeaponDef *d = haloweapon(s.def);
    if(!d || d->zoomLevels <= 0 || player1->weaponsel->reloading) return;
    s.zoom = (s.zoom + 1) % (d->zoomLevels + 1);
    if(player1->weaponsel->type == GUN_SNIPER) ((sniperrifle *)player1->weaponsel)->setscope(s.zoom > 0);
    // The weapon tag's zoom-in sound for each step in, its zoom-out sound back to no zoom.
    if(const char *zoom = d->sound(s.zoom > 0 ? HALO_SOUND_ZOOM_IN : HALO_SOUND_ZOOM_OUT)) haloplaysound(zoom, S_NOAMMO);
    else audiomgr.playsound(S_NOAMMO, SP_LOW);
}
// In a vehicle seat the zoom key is the secondary trigger (the Banshee's fuel rod, the Scorpion's
// machine gun).
COMMANDF(halo_zoom, "d", (bool down)
{
    if(halovehicleseated()) halovehiclesecondary(down);
    else
    {
        halovehiclesecondary(false);
        if(down) halozoomstep();
    }
});

#ifdef HALO_QA
#include "halo_ai.h"
// Verification hooks live in tests/native. They read this file's internal
// campaign state, so they compile into the same translation unit.
#include "campaign_qa.inc"
#include "b30_qa.inc"
#include "retail_qa.inc"
#include "vehicle_qa.inc"
#include "combat_qa.inc"
#include "damage_qa.inc"
#include "frontend_qa.inc"
#include "faction_qa.inc"
#include "presentation_qa.inc"
#include "soak_qa.inc"
#endif
