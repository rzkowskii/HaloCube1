#include "cube.h"
#include "bot/bot.h"
#include "halo.h"
#include "halo_collision.h"
#include "halo_script.h"
#include "halo_campaign.h"
#include "halo_weapons.h"
#include "halo_ai.h"
#include "halo_hud.h"

extern void startgame(const char *name, int mode, int minutes, bool notify);

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
    bool driving = false;
    int escapeDeadline = 0, incomingEscapeTime = 0;
    const int escapeTime = 180000;
    const int playerShield = 75, playerHealth = 75;
    const int shieldDelay = 6000, shieldRecharge = 4000;
    const int eliteDelay = 12000, eliteRecharge = 10000;
    const float markerRadius = 8.0f, firstContactRadius = 62.0f;

    enum MissionStage { INSERTION, FIRST_CONTACT, REACH_PASS, SECOND_CONTACT, EXTRACTION, COMPLETE };
    enum ActorKind { GRUNT, ELITE, JACKAL, HUNTER, FLOOD_ELITE, FLOOD_HUMAN, INFECTION, CARRIER, SENTINEL, MARINE, ACTOR_COUNT };

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
    };

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

    bool tagWeapons() { return haloweapon(carried[0].def) && haloweapon(carried[1].def); }

    // Two carried weapons need distinct AssaultCube slots; projectile weapons
    // share the carbine and submachine-gun slots between them.
    void assignGuns()
    {
        loopi(2)
        {
            const HaloWeaponDef *d = haloweapon(carried[i].def);
            carriedGun[i] = d ? d->acgun() : (i ? GUN_PISTOL : GUN_ASSAULT);
        }
        if(carriedGun[0] == carriedGun[1]) carriedGun[1] = carriedGun[0] == GUN_CARBINE ? GUN_SUBGUN : GUN_CARBINE;
    }

    // Copy the slot's live rounds back into the carried weapon.
    void syncCarried()
    {
        if(!tagWeapons()) return;
        loopi(2)
        {
            const HaloWeaponDef *d = haloweapon(carried[i].def);
            if(d->usesBattery()) continue;
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
        info.damage = t && t->projectile.valid ? int(t->projectile.damageUpper + .5f) : 10;
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
        player1->setnextprimary(carriedGun[0]);
        player1->setprimary(carriedGun[0]);
        selectCarried(currentCarried, immediate);
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
        // Reverting restores the weapons lying in the world as they were saved;
        // before the first checkpoint that is the section's placements.
        groundWeapons.setsize(0);
        loopv(checkpointGround) groundWeapons.add(checkpointGround[i]);
        shield = shieldCap;
        shieldHit = 0;
        shieldRecharging = false;
    }

    void saveCheckpoint(bool scripted = false)
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
        checkpointItems.setsize(0);
        loopv(ents) checkpointItems.add(ents[i].spawned);
        haloscenariocheckpoint();
        conoutf("Halo: checkpoint reached");
    }

    botent *spawnEnemy(ActorKind kind, const vec &origin, float yaw, int weaponIndex = -1, int actorIndex = -1,
                       int team = HALO_TEAM_COVENANT)
    {
        const ActorProfile &profile = profiles[kind];
        // Script-spawned actors arrive without a variant; give them their usual weapon.
        static const char *const usual[ACTOR_COUNT] = {
            "weapons\\plasma pistol\\plasma pistol", "weapons\\plasma rifle\\plasma rifle",
            "weapons\\plasma pistol\\plasma pistol", "weapons\\fuel rod gun\\hunter fuel rod",
            "weapons\\plasma rifle\\plasma rifle", "weapons\\assault rifle\\assault rifle", NULL, NULL,
            "characters\\sentinel\\sentinel", "weapons\\assault rifle\\assault rifle",
        };
        const HaloActorDef *actor = haloactordef(actorIndex);
        if(actor && actor->weapon >= 0) weaponIndex = actor->weapon;
        const HaloWeaponDef *def = profile.melee ? nullptr :
            weaponIndex >= 0 ? haloweapon(weaponIndex) : usual[kind] ? haloweaponfind(usual[kind]) : nullptr;
        if(!actor) actor = haloactordefault(kind, def ? def->index : -1);
        defformatstring(name)("%.12s %d", profile.name, enemies.length() + 1);
        const bool human = team == HALO_TEAM_HUMAN;
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
        const int gun = profile.melee ? GUN_KNIFE : GUN_SUBGUN;
        bot->setprimary(gun);
        bot->setnextprimary(gun);
        bot->selectweapon(gun);
        bot->nextweaponsel = bot->weaponsel;
        bot->weaponchanging = 0;
        loopj(NUMGUNS) bot->mag[j] = bot->ammo[j] = 0;
        // Damage is the retail projectile's, scaled by the variant's damage modifier;
        // the variant's rate of fire sets the cadence inside a burst.
        const HaloTriggerDef *t = def ? def->trigger() : nullptr;
        const float damageScale = actor && actor->damageScale > 0 ? actor->damageScale : aiDamageScale;
        const int damage = t && t->projectile.valid ? max(1, int(t->projectile.damageUpper * damageScale + .5f)) : profile.damage;
        const int delay = actor && actor->rate > 0 ? int(1000 / actor->rate) :
                          t ? max(profile.delay, enemies.last().weapon.delay(*t)) : profile.delay;
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
            const ActorKind kind = start.attr4 < ACTOR_COUNT ? ActorKind(start.attr4) : GRUNT;
            spawnEnemy(kind, position(start), start.attr1 / 10.0f, start.attr5 - 1);
        }
        if(!waveStarts[wave].empty()) conoutf("Halo: encounter %d", wave + 1);
    }

    int remaining()
    {
        int count = 0;
        loopv(enemies) if(enemies[i].bot && enemies[i].bot->state == CS_ALIVE && enemies[i].team != HALO_TEAM_HUMAN) ++count;
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
        resetLoadout();
        driving = escapeRun();
        player1->maxspeed = driving ? 36.0f : 16.0f;
        escapeDeadline = driving ? lastmillis + (incomingEscapeTime > 0 ? incomingEscapeTime : escapeTime) : 0;
        incomingEscapeTime = 0;
        loopv(ents) if(isitem(ents[i].type))
        {
            bool available = !atCheckpoint || (checkpointItems.inrange(i) && checkpointItems[i]);
            setpickupspawn(i, available);
            ents[i].lastmillis = 0;
        }
        intermission = false;
        if(driving) setStage(EXTRACTION);
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
            const int placed = spawnRing(spawn.kind, spawn.o, spawn.yaw, spawn.count, HALO_TEAM_COVENANT);
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

    void updateWeapons(int elapsed)
    {
        if(!tagWeapons()) return;
        syncCarried();
        const int selected = player1->weaponsel->type;
        if(selected == carriedGun[0] || selected == carriedGun[1]) currentCarried = selected == carriedGun[1];
        loopi(2)
        {
            const HaloWeaponDef *d = haloweapon(carried[i].def);
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
            audiomgr.playsound(S_ITEMAMMO, SP_HIGH);
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
        dropWeapon(carried[currentCarried], feetOf(player1), player1->yaw);
        carried[currentCarried] = picked.state;
        equipCarried(false);
        audiomgr.playsound(S_ITEMAMMO, SP_HIGH);
        const HaloWeaponDef *d = haloweapon(picked.state.def);
        showPickup(d ? d->name.c_str() : "Weapon");
        return true;
    }

    void loadProgress()
    {
        if(progressLoaded) return;
        progressLoaded = true;
        FILE *file = fopen(findfile(progressFile, "rb"), "rb");
        if(!file) return;
        int version = 0, m = 0, s = 0;
        int beaten = 0;
        const int fields = fscanf(file, "%d %d %d %d", &version, &m, &s, &beaten);
        if(fields >= 3 && version == 1 && m >= 0 && m < haloMissionCount && s >= 0 && s < haloMissions[m].sections)
        {
            savedMission = m;
            savedSection = s;
            campaignBeaten = fields == 4 && beaten == 1;
        }
        fclose(file);
    }

    void saveProgress()
    {
        string destination, temporary;
        copystring(destination, findfile(progressFile, "wb"));
        formatstring(temporary)("%s.tmp", destination);
        FILE *file = fopen(temporary, "wb");
        if(!file) { conoutf("Halo: unable to open campaign progress"); return; }
        bool written = (campaignBeaten ? fprintf(file, "1 %d %d 1\n", savedMission, savedSection)
                                       : fprintf(file, "1 %d %d\n", savedMission, savedSection)) > 0;
        if(fclose(file) != 0 || !written || rename(temporary, destination) != 0)
        {
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
        saveProgress();
        player1->attacking = false;
        player1->stopmoving();
        conoutf("Halo ending reached: %s complete", haloMissions[mission].title);
    }

    // Credits are over (or skipped): leave the finished campaign at the Halo menu.
    void finishEnding()
    {
        if(!ending || endingDone) return;
        endingDone = true;
        conoutf("Halo credits finished");
        showmenu("Halo Campaign");
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
        int remainingTime = driving ? max(1, escapeDeadline - lastmillis) : 0;
        queueSection(m, s);
        if(launchPending && m == mission)
        {
            incomingEscapeTime = remainingTime;
            pendingContinuation = true;
        }
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
            if(driving) for(int i = 0; i < 2; ++i)
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

void halomapstart(const char *name)
{
    haloscenariostop();
    halocollisionmap(nullptr);
    authoredTransition = false;
    ending = endingDone = winPending = false;
    active = startPending = driving = false;
    player1->maxspeed = 16.0f;
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
        }
    }
    if(insertion < 0 || checkpoint < 0 || extraction < 0)
    {
        conoutf("Halo: incomplete campaign section markers");
        return;
    }
    loopv(groundWeapons) checkpointGround.add(groundWeapons[i]);
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
int haloescapemillis() { return driving ? max(0, escapeDeadline - lastmillis) : -1; }
float haloplayershieldfraction() { return clamp(shield / float(playerShield), 0.0f, 1.0f); }

float haloenemyshieldfraction(const playerent *p)
{
    HaloEnemy *enemy = enemyState(p);
    return enemy && enemy->shieldMax > 0 ? enemy->shield / enemy->shieldMax : 0;
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
    if(haloactive() && player1->state == CS_ALIVE && !driving && stage != FIRST_CONTACT &&
       stage != SECOND_CONTACT && stage != COMPLETE) saveCheckpoint(true);
}

bool halorespawn()
{
    if(!haloactive()) return false;
    if(player1->state == CS_DEAD && lastmillis - player1->lastdeath >= 1000)
    {
        resetEncounter();
        haloscenariorevert();
        resetcamera();
    }
    return true;
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
        return;
    }
    if(startPending)
    {
        if(getclientnum() < 0) return;
        startPending = false;
        resetEncounter();
        resetcamera();
        conoutf("Halo: %s, section %d", haloMissions[mission].title, section + 1);
        haloscenariostart(mission, section, preview, pendingContinuation);
        if(!preview && (mission > savedMission || (mission == savedMission && section > savedSection)))
        {
            savedMission = mission;
            savedSection = section;
            saveProgress();
        }
    }
    haloscenariotick(elapsed);
    if(haloscenariocomplete()) finishSection(true);
    if(haloscenariogamewon())
    {
        if(!preview && mission == haloMissionCount - 1 && section == haloMissions[mission].sections - 1) winPending = true;
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
    if(halocinematic() || player1->state != CS_ALIVE || stage == COMPLETE) return;
    if(driving && lastmillis >= escapeDeadline)
    {
        if(haloscenarioescapefailed() || !haloscenarioescapetimeout())
            dodamage(INT_MAX, player1, player1, GUN_GRENADE);
        return;
    }
    if(!retailCartographer() && (stage == INSERTION || stage == REACH_PASS || stage == EXTRACTION))
    {
        const vector<int> &markers = route[stage == INSERTION ? 0 : stage == REACH_PASS ? 1 : 2];
        if(driving)
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
            if(nextRouteMarker >= route[2].length() + (driving ? route[0].length() + route[1].length() : 0) &&
               near(extraction, markerRadius) && !sectionInteraction()) finishSection();
            break;
        case COMPLETE: break;
    }
}

void halodamage(int &damage, playerent *target, playerent *attacker, int gun)
{
    if(damage <= 0 || damage == INT_MAX) return;
    // Playfighting actors go through the motions without hurting anyone.
    if(haloactorflags(attacker) & HALO_AI_PLAYFIGHT) { damage = 0; return; }
    // object_cannot_take_damage and unit_impervious.
    if((target == player1 && playerProtected) || (haloactorflags(target) & (HALO_AI_NO_DAMAGE | HALO_AI_IMPERVIOUS))) { damage = 0; return; }
    if(target == player1)
    {
        // Enemy fire hurts more on the higher difficulties.
        if(attacker != player1 && halodifficulty() != 1) damage = max(1, int(damage * halodifficultydamage() + .5f));
        shieldHit = lastmillis;
        shieldRecharging = false;
        const bool hadShield = shield > 0;
        int factor = attacker != player1 && gun == GUN_SUBGUN ? 2 : 1;
        float absorbed = min(shield, float(damage * factor));
        shield -= absorbed;
        if(hadShield) haloplaysound(shield > 0 ? "shield-hit" : "shield-depleted", S_NULL);
        damage = max(0, int(ceilf(damage - absorbed / factor - 0.001f)));
        return;
    }
    HaloEnemy *enemy = enemyState(target);
    if(!enemy || enemy->shieldMax <= 0) return;
    enemy->hitTime = lastmillis;
    float modifier = gun == GUN_ASSAULT ? 0.7f : 1.0f;
    float absorbed = min(enemy->shield, damage * modifier);
    if(absorbed > 0)
    {
        enemy->shield -= absorbed;
        damage = max(0, int(ceilf(damage - absorbed / modifier - 0.001f)));
        particle_splash(PART_PLASMA, 6, 220, target->o);
    }
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
            if(grenades >= 4) return true;
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
    return profiles[enemy->kind].model;
}

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
    box(right-318, 72, 318, 120, 7, 24, 44, 160);
    box(right-299, 123, 278, 16, 18, 60, 81, 210);
    box(right-299, 123, 278 * shield / playerShield, 16, 91, 216, 255, 235);
    loopi(8) box(right-299 + i*35, 151, 30, 13,
        player1->health * 8 > i * playerHealth ? 114 : 24,
        player1->health * 8 > i * playerHealth ? 217 : 61,
        player1->health * 8 > i * playerHealth ? 232 : 86, 225);

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
        const bool ally = enemies[i].team == HALO_TEAM_HUMAN;
        box(trackerX + delta.x*2-7, trackerY + delta.y*2-7, 14, 14, 255, ally ? 220 : 74, ally ? 60 : 81, 250);
    }
    }
    if(stage != COMPLETE)
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
    if(!tagHud) label("SHIELD", right-299, 79);
    if(driving && stage != COMPLETE)
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
    if(driving)
    {
        label("WARTHOG", right, VIRTH-155, 0.70f, true);
        return;
    }
    const weapon *current = player1->weaponsel;
    const HaloWeaponDef *def = tagWeapons() ? haloweapon(carried[currentCarried].def) : NULL;
    if(def && tagHud)
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
        HaloHudUnit u;
        u.shield = clamp(shield / playerShield, 0.0f, 1.0f);
        u.health = clamp(player1->health / float(playerHealth), 0.0f, 1.0f);
        u.grenades = player1->mag[GUN_GRENADE];
        HaloHudBlip blips[64];
        int count = 0;
        const float scale = haloscenarioscale();
        loopv(enemies)
        {
            const botent *bot = enemies[i].bot;
            // The sensor shows contacts that move or fire, as the retail tracker does.
            if(!bot || bot->state != CS_ALIVE || count >= 64 || (bot->vel.sqrxy() < 0.01f && lastmillis - bot->lastaction > 500)) continue;
            vec delta = vec(bot->o).sub(player1->o);
            delta.rotate_around_z(-yaw);
            blips[count++] = HaloHudBlip{delta.x / scale, delta.y / scale, enemies[i].team == HALO_TEAM_HUMAN};
        }
        halohuddraw(w, u, blips, count);
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
    else
    {
        const char *name = current->type == GUN_PISTOL ? "M6D MAGNUM" : current->type == GUN_KNIFE ? "MELEE" : "MA5B ASSAULT RIFLE";
        label(name, right, VIRTH-212, 0.60f, true);
        defformatstring(ammo)("%02d / %03d", current->mag, current->ammo);
        label(ammo, right, VIRTH-173, 0.96f, true);
    }
    if(groundWeapons.inrange(nearbyWeapon))
    {
        const HaloWeaponDef *d = haloweapon(groundWeapons[nearbyWeapon].state.def);
        defformatstring(prompt)("E  Pick up %s", d ? d->name.c_str() : "weapon");
        label(prompt, VIRTW/2-200, VIRTH/2+90, 0.6f);
    }
    if(pickupMessage && lastmillis - pickupMessage < 1600) label(pickupText, VIRTW/2-200, VIRTH/2+130, 0.55f);
    if(!tagHud)
    {
        defformatstring(grenades)("FRAG  %d", player1->mag[GUN_GRENADE]);
        label(grenades, right, VIRTH-100, 0.57f, true);
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
COMMANDF(halo_restart, "", () { if(haloactive()) queueSection(mission, section, preview); });
COMMANDF(halo_next, "", ()
{
    if(ending) { finishEnding(); return; }
    if(!halocinematicskip()) nextSection();
});
COMMANDF(halo_use, "", ()
{
    if(haloscenariouse()) return;
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
COMMANDF(halo_switch, "d", (bool down) { if(down) haloswitchweapon(); });

void haloremoveactor(const playerent *p)
{
    loopv(enemies) if(enemies[i].bot == p) enemies[i].bot = NULL;
    haloactordetach(p);
    haloscenarioremovebot(p);
}

COMMANDF(halo_frag, "d", (bool down) { if(down) halofrag(); });
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
    if(position) audiomgr.playsound(id, position);
    else if(actor && actor != player1) audiomgr.playsound(id, actor);
    else audiomgr.playsound(id, SP_HIGH);
}

COMMANDF(halo_pause, "", ()
{
    if(haloactive())
    {
        player1->attacking = false;
        player1->stopmoving();
        showmenu("Halo Campaign");
    }
    else execute("key_escape");
});


bool halodriving() { return haloactive() && driving && !editmode && player1->state == CS_ALIVE; }

void halorendervehicle()
{
    halorenderweapons();
    if(!halodriving()) return;
    vec origin = player1->o;
    origin.z -= player1->eyeheight;
    rendermodel("halo/warthog", ANIM_MAPMODEL, 0, -1, origin, 0, player1->yaw + 90, 0, 0, 0);
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
    return gun == carriedGun[0] || gun == carriedGun[1];
}

void haloswitchweapon()
{
    if(!haloactive() || halocinematic() || player1->state != CS_ALIVE) return;
    if(!tagWeapons())
    {
        execute(player1->weaponsel->type == GUN_ASSAULT ? "weapon pistol" : "weapon assault");
        return;
    }
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
    return d && d->meleeUpper > 0 ? int(d->meleeUpper + .5f) : 50;
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
        if(!bot || bot->state != CS_ALIVE || enemies[i].team == HALO_TEAM_HUMAN) continue;
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
    haloactordeath(victim, killer);
    HaloEnemy *enemy = enemyState(victim);
    if(!enemy) return;
    const vec feet = feetOf(victim);
    if(enemy->kind == CARRIER) floodSpawns.add(FloodSpawn{feet, victim->yaw, INFECTION, 4 + rnd(5), lastmillis});
    else if((enemy->kind == MARINE || enemy->kind == ELITE) && corpses.length() < 32)
        corpses.add(Corpse{feet, victim->yaw, enemy->kind, lastmillis + corpseLife});
    const HaloWeaponDef *d = haloweapon(enemy->weapon.def);
    // unit_doesnt_drop_items leaves nothing behind.
    if(!keepsItems || !d || !d->playable()) return;
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
}

void halozoomstep()
{
    if(!haloactive() || !tagWeapons() || halocinematic() || player1->state != CS_ALIVE) return;
    HaloWeaponState &s = carried[currentCarried];
    const HaloWeaponDef *d = haloweapon(s.def);
    if(!d || d->zoomLevels <= 0 || player1->weaponsel->reloading) return;
    s.zoom = (s.zoom + 1) % (d->zoomLevels + 1);
    if(player1->weaponsel->type == GUN_SNIPER) ((sniperrifle *)player1->weaponsel)->setscope(s.zoom > 0);
    audiomgr.playsound(S_NOAMMO, SP_LOW);
}
COMMANDF(halo_zoom, "d", (bool down) { if(down) halozoomstep(); });

#ifdef HALO_QA
#include "halo_ai.h"
// Verification hooks live in tests/native. They read this file's internal
// campaign state, so they compile into the same translation unit.
#include "campaign_qa.inc"
#include "b30_qa.inc"
#include "retail_qa.inc"
#endif
