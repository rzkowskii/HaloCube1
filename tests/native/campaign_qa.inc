// Verification hooks for tests/. Compiled into halo.cpp only when HALO_QA is
// defined (the halo-ac-qa target), because they read that file's internal state.
#include <string>

// ---- Combat, shield and checkpoint replay (tests/qa_replay.py)

// Local verification hooks compiled only into the temporary QA executable.
COMMANDF(qa_halo_status, "s", (const char *name)
{
    fflush(stdout);
    conoutf("QA %s stage=%d live=%d pos=%.2f,%.2f,%.2f yaw=%.2f hp=%d shield=%.2f mag=%d reserve=%d frags=%d checkpoint=%d",
        name, stage, remaining(), player1->o.x, player1->o.y, player1->o.z, player1->yaw,
        player1->health, shield, player1->weaponsel->mag, player1->weaponsel->ammo, player1->mag[GUN_GRENADE], atCheckpoint);
    fflush(stdout);
});
COMMANDF(qa_halo_position, "fff", (float *x, float *y, float *yaw)
{
    const sqr *cell = S(int(*x), int(*y));
    player1->o = vec(*x, *y, cell->floor + player1->eyeheight - (cell->type == FHF ? cell->vdelta / 4.0f : 0));
    player1->yaw = *yaw;
    player1->pitch = 0;
    player1->vel = vec();
    entinmap(player1);
    player1->resetinterp();
});
COMMANDF(qa_halo_damage, "i", (int *amount) { dodamage(*amount, player1, player1, GUN_ASSAULT); });
COMMANDF(qa_halo_revert, "", () { tryrespawn(); });
COMMANDF(qa_halo_clear, "", ()
{
    loopv(enemies) if(enemies[i].bot && enemies[i].bot->state == CS_ALIVE)
        dodamage(INT_MAX, enemies[i].bot, player1, GUN_ASSAULT);
});
COMMANDF(qa_halo_fire, "i", (int *on) { attack(*on != 0); });
COMMANDF(qa_halo_move, "i", (int *on) { player1->move = *on; });
COMMANDF(qa_halo_aim, "", ()
{
    playerent *target = NULL;
    float best = 1e16f;
    loopv(enemies)
    {
        auto &enemy = enemies[i];
        if(!enemy.bot || enemy.bot->state != CS_ALIVE || enemy.kind != GRUNT) continue;
        float distance = player1->o.squareddist(enemy.bot->o);
        if(distance < best && raycubelos(player1->o, enemy.bot->o)) { best = distance; target = enemy.bot; }
    }
    if(!target) return;
    vec direction = vec(target->o).sub(player1->o);
    direction.z -= target->eyeheight * .40f;
    player1->yaw = atan2f(direction.x, -direction.y) / RAD;
    if(player1->yaw < 0) player1->yaw += 360.0f;
    player1->pitch = atan2f(direction.z, direction.magnitudexy()) / RAD;
});

// ---- Campaign progression, devices and the 55-section replay
static vec qaLibraryExit(int part)
{
    const vec exits[] = {vec(121.79f, 462.20f, 97.f), vec(825.98f, 555.53f, 126.f), vec(157.95f, 83.31f, 116.4f)};
    return exits[part];
}

static void qaPlaceFeet(const vec &feet)
{
    player1->o = vec(feet.x, feet.y, feet.z + player1->eyeheight);
    player1->vel = vec();
    player1->resetinterp();
}

// Runs a command once the queued section is in live gameplay, skipping any
// cinematic on the way. Sleeps inside the command are relative to that moment,
// so tests don't depend on how long loading or waypoint generation takes.
static std::string qaAfterGameplay;
static int qaAfterGameplayStable = 0, qaAfterGameplayStarted = 0;
COMMANDF(qa_after_gameplay_poll, "", ()
{
    if(qaAfterGameplay.empty()) return;
    const bool live = haloactive() && !launchPending && !startPending && !halocinematic() && player1->state == CS_ALIVE;
    if(halocinematic()) halocinematicskip();
    qaAfterGameplayStable = live ? qaAfterGameplayStable + 1 : 0;
    if(qaAfterGameplayStable >= 5)
    {
        conoutf("QA_GAMEPLAY %s after %d ms", getclientmap(), lastmillis - qaAfterGameplayStarted);
        fflush(stdout);
        const std::string body = qaAfterGameplay;
        qaAfterGameplay.clear();
        execute(body.c_str());
        return;
    }
    addsleep(20, "qa_after_gameplay_poll", true);
});
COMMANDF(qa_after_gameplay, "s", (const char *body)
{
    qaAfterGameplay = body;
    qaAfterGameplayStable = 0;
    qaAfterGameplayStarted = lastmillis;
    addsleep(20, "qa_after_gameplay_poll", true);
});

COMMANDF(qa_campaign_load, "ii", (int *m, int *s)
{
    loadProgress();
    queueSection(*m - 1, *s - 1, true);
});
COMMANDF(qa_campaign_state, "", ()
{
    conoutf("CAMPAIGN_QA map=%s stage=%d alive=%d spawn=%d,%d,%d",
        getclientmap(), stage, player1->state == CS_ALIVE, int(player1->o.x), int(player1->o.y), int(player1->o.z));
    fflush(stdout);
});
COMMANDF(qa_campaign_advance, "", ()
{
    if(!haloactive()) return;
    if(stage == FIRST_CONTACT || stage == SECOND_CONTACT)
    {
        loopv(enemies) if(enemies[i].bot && enemies[i].bot->state == CS_ALIVE)
            dodamage(INT_MAX, enemies[i].bot, player1, GUN_ASSAULT);
        return;
    }
    if(stage == COMPLETE) return;
    const vec target = objective();
    player1->o = target;
    entinmap(player1);
    player1->resetinterp();
});

// Steps through the current stage's route markers, one per poll, until the
// stage changes. Teleporting straight to a goal skips the markers it requires.
static int qaReachStage = -1, qaReachTries = 0;
COMMANDF(qa_campaign_reach_step, "", ()
{
    if(stage != qaReachStage || ++qaReachTries > 400)
    {
        conoutf("QA_REACH from=%d stage=%d tries=%d", qaReachStage, stage, qaReachTries);
        fflush(stdout);
        return;
    }
    execute("qa_campaign_advance");
    addsleep(20, "qa_campaign_reach_step", true);
});
COMMANDF(qa_campaign_reach, "", ()
{
    qaReachStage = stage;
    qaReachTries = 0;
    execute("qa_campaign_reach_step");
});

COMMANDF(qa_campaign_full, "", ()
{
    if(!haloactive())
    {
        addsleep(20, "qa_campaign_full", true);
        return;
    }
    static int ticks = 0; static int lastMission = -1; static int lastSection = -1; static int sectionTicks = 0; static bool sectionReported = false;
    if(mission != lastMission || section != lastSection)
    {
        if(lastMission >= 0 && lastMission == mission && section == lastSection + 1 && !sectionReported)
            conoutf("CAMPAIGN_QA completed via authored transition %d %d", lastMission + 1, lastSection + 1);
        sectionReported = false;
        lastMission = mission;
        lastSection = section;
        sectionTicks = 0;
    }
    if(++ticks > 8000 || ++sectionTicks > 650)
    {
        conoutf("CAMPAIGN_QA stuck %s stage %d marker %d", getclientmap(), stage, nextRouteMarker);
        fflush(stdout);
        execute("quit");
        return;
    }
    if(halocinematic())
    {
        if(sectionTicks >= 10) halocinematicskip();
        addsleep(12, "qa_campaign_full", true);
        return;
    }
    if(stage == COMPLETE)
    {
        conoutf("CAMPAIGN_QA completed %s", getclientmap());
        sectionReported = true;
        fflush(stdout);
        if(campaignFinished) { execute("quit"); return; }
        nextSection();
    }
    else
    {
        if(player1->state == CS_DEAD) resetEncounter();
        const vec target = objective();
        if(mission == 6 && section < 3 && stage == EXTRACTION && nextRouteMarker + 10 >= route[2].length())
        {
            // Put the test player inside the original BSP exit above the lift.
            const vec transition = qaLibraryExit(section);
            player1->o = transition;
            player1->resetinterp();
            addsleep(50, "qa_campaign_full", true);
            return;
        }
        if(stage == FIRST_CONTACT || stage == SECOND_CONTACT)
        {
            loopv(enemies) if(enemies[i].bot && enemies[i].bot->state == CS_ALIVE)
                dodamage(INT_MAX, enemies[i].bot, player1, GUN_ASSAULT);
        }
        else if(retailCartographer())
        {
            // Retail collision has no AssaultCube spawn spots at the adapted
            // markers, so drive the authored triggers instead: the Cartographer
            // control, then each BSP switch volume, then the Pelican ramp.
            vec point;
            bool mapped = false;
            if(halocartographertarget(point, mapped) && !mapped)
            {
                qaPlaceFeet(point);
                execute("halo_use");
            }
            else if(haloscenarioexitpoint(point))
            {
                // testVolume samples the player at 0.45 of eye height.
                player1->o = point;
                player1->o.z += .45f * player1->eyeheight;
                player1->vel = vec();
                player1->resetinterp();
            }
            else if(halopelicanready())
            {
                qaPlaceFeet(position(ents[extraction]));
                execute("halo_use");
            }
            // The Pelican's recorded approach outlasts the usual section budget.
            if(section == 2 && !halopelicanready()) sectionTicks = min(sectionTicks, 300);
        }
        else
        {
            player1->o = target;
            entinmap(player1);
            player1->resetinterp();
            if(stage == EXTRACTION && sectionInteraction()) execute("halo_use");
        }
    }
    addsleep(12, "qa_campaign_full", true);
});

COMMANDF(qa_timeout, "", ()
{
    if(haloactive() && driving) escapeDeadline = lastmillis;
});
COMMANDF(qa_maw_route, "", ()
{
    loadProgress();
    queueSection(9, 5);
});
COMMANDF(qa_scenario_state, "", ()
{
    conoutf("RETAIL_QA mission=%d section=%d alive=%d cutscene=%d stage=%d",
        mission, section, player1->state == CS_ALIVE, halocinematic(), stage);
    fflush(stdout);
});

COMMANDF(qa_halo_warp, "fff", (float *x, float *y, float *z)
{
    player1->o = vec(*x, *y, *z);
    player1->vel = vec();
    player1->resetinterp();
});
COMMANDF(qa_halo_feet, "fff", (float *x, float *y, float *z)
{
    player1->o = vec(*x, *y, *z + player1->eyeheight);
    player1->vel = vec();
    player1->resetinterp();
});
COMMANDF(qa_halo_block, "fff", (float *x, float *y, float *z)
{
    const vec old = player1->o;
    player1->o = vec(*x, *y, *z + player1->eyeheight);
    float hi = 127; float lo = -128;
    bool blocked = haloscenariocollide(player1, hi, lo);
    player1->o = old;
    conoutf("DEVICE_QA blocked=%d floor=%.2f", blocked, lo);
    fflush(stdout);
});

COMMANDF(qa_campaign_resume, "ii", (int *m, int *s)
{
    loadProgress();
    queueSection(*m - 1, *s - 1);
});

// ---- Tag-driven weapons (tests/test_weapon_tags.py, tests/test_weapon_fire.py)
static const HaloWeaponDef *qaWeaponByLabel(const char *label)
{
    loopi(haloweaponcount())
    {
        const HaloWeaponDef *d = haloweapon(i);
        if(d && d->playable() && d->label == label) return d;
    }
    return NULL;
}

COMMANDF(qa_weapon_table, "", ()
{
    loopi(haloweaponcount())
    {
        const HaloWeaponDef *d = haloweapon(i);
        const HaloMagazineDef *m = d->magazine();
        const HaloTriggerDef *t = d->trigger();
        conoutf("WEAPON_TABLE %d %s %d %d %d %d %.3f %.3f %.3f %d", i, d->label.c_str(), int(d->kind),
                m ? m->loaded : 0, m ? m->maximum : 0, int(d->triggers.size()),
                t ? t->rate : 0.0f, t && t->projectile.valid ? t->projectile.speed : 0.0f,
                t && t->projectile.valid ? t->projectile.damageUpper : 0.0f, d->zoomLevels);
    }
    fflush(stdout);
});
COMMANDF(qa_weapon_give, "s", (const char *label)
{
    const HaloWeaponDef *d = qaWeaponByLabel(label);
    if(!d || !tagWeapons()) { conoutf("WEAPON_QA give-failed %s", label); fflush(stdout); return; }
    carried[currentCarried].reset(d);
    equipCarried(true);
});
static void qaWeaponState(const char *name)
{
    const HaloWeaponState &s = carried[currentCarried];
    const HaloWeaponDef *d = haloweapon(s.def);
    const HaloWeaponDef *other = haloweapon(carried[currentCarried ^ 1].def);
    const HaloTriggerDef *t = d ? d->trigger() : NULL;
    extern float dynfov();
    extern float fov;
    conoutf("WEAPON_QA %s weapon=%s other=%s gun=%d mag=%d ammo=%d battery=%.3f heat=%.3f overheated=%d charged=%d "
            "zoom=%d fov=%.3f basefov=%.3f delay=%d shots=%d ground=%d nearby=%d",
            name, d ? d->label.c_str() : "-", other ? other->label.c_str() : "-", player1->weaponsel->type,
            player1->weaponsel->mag, player1->weaponsel->ammo, s.battery, s.heat, s.overheated, s.charged,
            s.zoom, dynfov(), float(fov), t ? s.delay(*t) : 0, player1->pstatshots[player1->weaponsel->type],
            groundWeapons.length(), nearbyWeapon);
    conoutf("WEAPON_POS %s %.1f %.1f %.1f", name, player1->o.x, player1->o.y, player1->o.z - player1->eyeheight);
    fflush(stdout);
}
COMMANDF(qa_weapon_state, "s", (const char *name) { qaWeaponState(name); });
COMMANDF(qa_weapon_release, "", () { player1->attacking = false; });
COMMANDF(qa_weapon_fire, "i", (int *ms)
{
    player1->attacking = true;
    defformatstring(release)("qa_weapon_release");
    addsleep(*ms, release, true);
});
COMMANDF(qa_weapon_charge_release, "", () { haloattackinput(false); player1->attacking = false; });
COMMANDF(qa_weapon_charge, "i", (int *ms)
{
    haloattackinput(true);
    player1->attacking = true;
    addsleep(*ms, "qa_weapon_charge_release", true);
});
COMMANDF(qa_weapon_goto_ground, "", ()
{
    // Stand on the closest ground weapon that differs from both carried ones.
    float best = 1e9f;
    int found = -1;
    loopv(groundWeapons)
    {
        const GroundWeapon &g = groundWeapons[i];
        if(g.state.def == carried[0].def || g.state.def == carried[1].def) continue;
        const float d = player1->o.dist(g.o);
        if(d < best) { best = d; found = i; }
    }
    if(found < 0)
    {
        loopv(groundWeapons) conoutf("WEAPON_GROUND carried %s", haloweapon(groundWeapons[i].state.def)->label.c_str());
        conoutf("WEAPON_QA no-ground-weapon");
        fflush(stdout);
        return;
    }
    const vec &o = groundWeapons[found].o;
    conoutf("WEAPON_GROUND %s at %.1f %.1f %.1f", haloweapon(groundWeapons[found].state.def)->label.c_str(), o.x, o.y, o.z);
    fflush(stdout);
    player1->o = vec(groundWeapons[found].o).add(vec(0, 0, player1->eyeheight));
    player1->vel = vec();
    player1->resetinterp();
});
COMMANDF(qa_weapon_switch, "", () { haloswitchweapon(); });
COMMANDF(qa_weapon_zoom, "", () { halozoomstep(); });
COMMANDF(qa_enemy_weapons, "", ()
{
    loopv(enemies)
    {
        const HaloWeaponDef *d = haloweapon(enemies[i].weapon.def);
        conoutf("ENEMY_WEAPON %s %s damage=%d", profiles[enemies[i].kind].name, d ? d->label.c_str() : "-",
                enemies[i].bot ? enemies[i].bot->weapons[GUN_SUBGUN]->info.damage : 0);
    }
    fflush(stdout);
});

// ---- Actor AI (tests/test_actor_ai.py)
COMMANDF(qa_actor_state, "s", (const char *label) { haloactordump(label); });
COMMANDF(qa_actor_place, "s", (const char *encounter)
{
    conoutf("ACTOR_PLACE %s %d", encounter, haloscenarioplace(encounter));
    fflush(stdout);
});
// Spawns an actor of a family at forward/side offsets (Halo world units) from the player, facing it.
static void qaActorSpawn(int kind, int team, float forward, float side)
{
    const float yaw = player1->yaw * RAD;
    const float s = haloscenarioscale();
    vec at = player1->o;
    at.add(vec(sinf(yaw), -cosf(yaw), 0).mul(forward * s)).add(vec(cosf(yaw), sinf(yaw), 0).mul(side * s));
    // Allies face the way the player looks; hostiles face the player.
    botent *bot = spawnEnemy(ActorKind(clamp(kind, 0, int(ACTOR_COUNT) - 1)), at, player1->yaw + (team ? 0 : 180), -1, -1, team);
    conoutf("ACTOR_SPAWN %s %d", bot ? bot->name : "-", bot ? team : -1);
    fflush(stdout);
}
COMMANDF(qa_actor_spawn, "iiff", (int *kind, int *team, float *forward, float *side) { qaActorSpawn(*kind, *team, *forward, *side); });
// Calls a script AI command on a named encounter: qa_hs_ai ai_status first_wave 0
static void qaScriptAI(const char *command, const char *encounter, float second)
{
    const int reference = haloscenarioencounter(encounter);
    const double value = reference < 0 ? -999 : haloscenariocall(command, reference, second);
    conoutf("HS_AI %s %s %.4f", command, encounter, value);
    fflush(stdout);
}
COMMANDF(qa_hs_ai, "ssf", (const char *command, const char *encounter, float *second) { qaScriptAI(command, encounter, *second); });
// ai_actors on an encounter, then list_count and list_get through the script commands.
static void qaScriptList(const char *encounter)
{
    const int reference = haloscenarioencounter(encounter);
    const double list = haloscenariocall("ai_actors", reference, 0);
    const int count = int(haloscenariocall("list_count", list, 0));
    int units = 0;
    loopi(count) if(haloscenariocall("list_get", list, i) <= -1000) ++units;
    conoutf("HS_LIST %s list=%.0f count=%d units=%d", encounter, list, count, units);
    fflush(stdout);
}
COMMANDF(qa_hs_list, "s", (const char *encounter) { qaScriptList(encounter); });
// ai_command_list_status across an encounter's actors (0 none, 1 running, 2 finished).
static void qaCommandStatus(const char *encounter)
{
    const int reference = haloscenarioencounter(encounter);
    const double list = haloscenariocall("ai_actors", reference, 0);
    conoutf("HS_COMMAND %s %.0f", encounter, haloscenariocall("ai_command_list_status", list, 0));
    fflush(stdout);
}
COMMANDF(qa_hs_command, "s", (const char *encounter) { qaCommandStatus(encounter); });
COMMANDF(qa_actor_table, "", ()
{
    loopi(haloactordefcount())
    {
        const HaloActorDef *d = haloactordef(i);
        conoutf("ACTOR_TABLE %d %s %d %d %.3f %.3f %.3f", i, d->family.c_str(), d->kind, d->weapon, d->rate, d->burstMin, d->visionDistance);
    }
    fflush(stdout);
});
// Sets an AI switch on every campaign actor: qa_actor_flag 1 1 blinds them all.
COMMANDF(qa_actor_flag, "ii", (int *flag, int *on)
{
    loopv(enemies) if(enemies[i].bot) haloactorsetflag(enemies[i].bot, *flag, *on != 0);
});
