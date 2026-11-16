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
    haloplayermoved("QA teleport");
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
    haloplayermoved("QA teleport");
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
    vec target = objective();
    // Markers are authored at floor height; retail collision places the eyes above the floor.
    if(halocollisionactive()) target.z += player1->eyeheight;
    player1->o = target;
    entinmap(player1);
    player1->resetinterp();
    haloplayermoved("QA teleport");
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
            haloplayermoved("QA teleport");
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
                haloplayermoved("QA teleport");
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
            if(halocollisionactive()) player1->o.z += player1->eyeheight;
            entinmap(player1);
            player1->resetinterp();
            haloplayermoved("QA teleport");
            if(stage == EXTRACTION && sectionInteraction()) execute("halo_use");
        }
    }
    addsleep(12, "qa_campaign_full", true);
});

COMMANDF(qa_timeout, "", ()
{
    if(haloactive() && escaping) escapeDeadline = lastmillis;
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
    haloplayermoved("QA teleport");
});
COMMANDF(qa_halo_feet, "fff", (float *x, float *y, float *z)
{
    player1->o = vec(*x, *y, *z + player1->eyeheight);
    player1->vel = vec();
    player1->resetinterp();
    haloplayermoved("QA teleport");
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
                t && t->projectile.valid ? t->projectile.damage.upperMax : 0.0f, d->zoomLevels);
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
    haloplayermoved("QA teleport");
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
// Combat by difficulty (the combat-scaling work): COMBAT_DIFFICULTY per team and ACTOR_COMBAT per
// living actor, the effective values the brain fires with (halo_ai.cpp haloactorcombatdump).
COMMANDF(qa_actor_combat, "s", (const char *label) { haloactorcombatdump(label); });
// The encounter grenade timeout (actions.c:2350-2373): a throw recorded for the first actor with one (GRENADE_NOTE).
COMMANDF(qa_actor_grenade_note, "s", (const char *label) { haloactorgrenadenote(label); });
COMMANDF(qa_actor_place, "s", (const char *encounter)
{
    conoutf("ACTOR_PLACE %s %d", encounter, haloscenarioplace(encounter));
    fflush(stdout);
});
// Kills every living campaign actor, as the player's fire would.
static void qaKillActors(int kind)
{
    loopv(enemies)
        if(enemies[i].bot && enemies[i].bot->state == CS_ALIVE && (kind < 0 || enemies[i].kind == kind))
            dodamage(INT_MAX, enemies[i].bot, player1, GUN_ASSAULT);
}
COMMANDF(qa_actor_kill, "i", (int *kind) { qaKillActors(*kind); });
// Halo's counts per squad of an encounter (tests/test_flood.py items 8 to 10, written while testing
// was on hold, NOT YET RUN): SQUAD_COUNT <label> <encounter>/<squad> members= living= spawned=
// spawned_living= swarms= actors= fraction= strength= nonswarm= (haloscenarioqacounts).
COMMANDF(qa_squad_counts, "ss", (const char *label, const char *encounter) { haloscenarioqacounts(label, encounter); });
// qa_squad_kill_one <encounter or encounter/squad>: the player's fire kills its first living member.
COMMANDF(qa_squad_kill_one, "s", (const char *name)
{
    if(!haloscenarioqakillone(name)) conoutf("SQUAD_KILL %s none", name);
    fflush(stdout);
});
// Living actors per body: FLOOD_QA label infection=2 carrier=0 flood_human=1 ...
COMMANDF(qa_actor_count, "s", (const char *label)
{
    int counts[ACTOR_COUNT] = {};
    loopv(enemies) if(enemies[i].bot && enemies[i].bot->state == CS_ALIVE) ++counts[enemies[i].kind];
    conoutf("FLOOD_QA %s grunt=%d elite=%d jackal=%d hunter=%d flood_elite=%d flood_human=%d infection=%d carrier=%d sentinel=%d marine=%d",
            label, counts[GRUNT], counts[ELITE], counts[JACKAL], counts[HUNTER], counts[FLOOD_ELITE], counts[FLOOD_HUMAN],
            counts[INFECTION], counts[CARRIER], counts[SENTINEL], counts[MARINE]);
    fflush(stdout);
});
// Halo's random speed increase (tests/test_flood.py, written while testing was on hold, NOT YET
// RUN): FLOOD_STAGGER <label> <name> cn=<client number> flag=<0|1> combat=<0|1> scale=<factor>
// value=<infection_forms> for every living actor (halomovementscale; combat: it fights a target).
COMMANDF(qa_flood_stagger, "s", (const char *label)
{
    loopv(enemies)
    {
        botent *bot = enemies[i].bot;
        if(!bot || bot->state != CS_ALIVE) continue;
        const HaloActorDef *def = haloactorvariant(bot);
        conoutf("FLOOD_STAGGER %s %s cn=%d flag=%d combat=%d scale=%.4f value=%.2f", label, bot->name, bot->clientnum,
                def && def->randomSpeedIncrease() ? 1 : 0, haloactorstatus(bot) == 6 && haloactortarget(bot) ? 1 : 0,
                halomovementscale(bot), halodifficultyget(HALO_DIFFICULTY_INFECTION_FORMS));
    }
    fflush(stdout);
});
// Leaves `count` dead entries in the squad list, as a long fight does.
COMMANDF(qa_squad_pad, "i", (int *count) { haloscenarioqapad(*count); });
// Places every encounter of the section one after another, killing each squad before the next
// so only the cap on living actors can stop a placement: SQUAD_QA <name> starts=<n> placed=<n>.
COMMANDF(qa_place_all, "", ()
{
    int total = 0;
    loopi(haloscenarioencountercount())
    {
        const int starts = haloscenarioencounterstarts(i);
        const int placed = starts > 0 ? max(0, haloscenarioplace(haloscenarioencountername(i))) : 0;
        total += placed;
        conoutf("SQUAD_QA %s starts=%d placed=%d", haloscenarioencountername(i), starts, placed);
        qaKillActors(-1);
    }
    conoutf("SQUAD_QA total placed=%d", total);
    fflush(stdout);
});
// Spawns an actor of a family at forward/side offsets (Halo world units) from the player, facing it.
static void qaActorSpawn(int kind, int team, float forward, float side)
{
    const float yaw = player1->yaw * RAD;
    const float s = haloscenarioscale();
    vec at = player1->o;
    at.add(vec(sinf(yaw), -cosf(yaw), 0).mul(forward * s)).add(vec(cosf(yaw), sinf(yaw), 0).mul(side * s));
    // Allies face the way the player looks; hostiles face the player. team -1: the body's own
    // side (kindTeam); 0 covenant, 1 human, 2 flood, 3 sentinel (halo_ai.h).
    const ActorKind k = ActorKind(clamp(kind, 0, int(ACTOR_COUNT) - 1));
    const bool friendly = haloteamfriendly(team >= 0 ? team : kindTeam(k));
    botent *bot = spawnEnemy(k, at, player1->yaw + (friendly ? 0 : 180), -1, -1, team);
    conoutf("ACTOR_SPAWN %s %d hp=%d", bot ? bot->name : "-", bot ? team : -1, bot ? bot->health : 0);
    fflush(stdout);
}
COMMANDF(qa_actor_spawn, "iiff", (int *kind, int *team, float *forward, float *side) { qaActorSpawn(*kind, *team, *forward, *side); });
// Calls a script AI command on a named encounter: qa_hs_ai ai_status first_wave 0
static void qaScriptAI(const char *command, const char *encounter, float second)
{
    // "encounter/squad" names a squad (a negative reference); -1 is not found.
    const int reference = haloscenarioencounter(encounter);
    const double value = reference == -1 ? -999 : haloscenariocall(command, reference, second);
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
// Script commands with several arguments (tests/test_actor_ai.py). Each token is a number, an encounter or
// squad name, `list:NAME` (ai_actors), `unit:NAME:N` (the N-th actor of NAME), `obj:NAME` (a scripted object),
// `flag:NAME` (a cutscene flag's index), `player`, `players`, `s:TEXT` (a string argument) or true/false.
// qa_hs_cmd ai_follow_target_ai "squad_a squad_b" prints HS_CMD <command> <value>.
static double qaToken(const char *token)
{
    if(!strcmp(token, "true")) return 1;
    if(!strcmp(token, "false")) return 0;
    if(!strcmp(token, "player")) return -2;
    if(!strcmp(token, "players")) return -3;
    if(!strncmp(token, "list:", 5)) return haloscenariocall("ai_actors", haloscenarioencounter(token + 5), 0);
    if(!strncmp(token, "unit:", 5))
    {
        string encounter;
        copystring(encounter, token + 5);
        char *colon = strchr(encounter, ':');
        const int n = colon ? atoi(colon + 1) : 0;
        if(colon) *colon = 0;
        const double list = haloscenariocall("ai_actors", haloscenarioencounter(encounter), 0);
        return haloscenariocall("list_get", list, n);
    }
    if(!strncmp(token, "obj:", 4)) return haloscenarioobject(token + 4);
    if(!strncmp(token, "flag:", 5)) return haloscenarioflag(token + 5);
    char *end = NULL;
    const double number = strtod(token, &end);
    if(end && !*end) return number;
    return haloscenarioencounter(token);
}
COMMANDF(qa_hs_cmd, "ss", (const char *command, const char *arguments)
{
    vector<char *> words;
    explodelist(arguments, words);
    double values[8] = {};
    const char *texts[8] = {};
    const int count = min(words.length(), 8);
    loopi(count)
    {
        // s:NAME passes a string argument (an effect or permutation name).
        if(!strncmp(words[i], "s:", 2)) texts[i] = words[i] + 2; else { texts[i] = ""; values[i] = qaToken(words[i]); }
    }
    conoutf("HS_CMD %s %.4f", command, haloscenariocallv(command, values, texts, count));
    words.deletearrays();
    fflush(stdout);
});
// qa_hs_object state obj:NAME prints the state the object commands set.
COMMANDF(qa_hs_object, "ss", (const char *what, const char *name)
{
    conoutf("HS_OBJECT %s %s %.0f", what, name, haloscenarioobjectstate(what, haloscenarioobject(!strncmp(name, "obj:", 4) ? name + 4 : name)));
    fflush(stdout);
});
COMMANDF(qa_player_vitals, "", ()
{
    conoutf("QA_VITALS health=%d shield=%.3f protected=%d", player1->health, haloplayershieldfraction(), haloplayerprotected());
    fflush(stdout);
});
COMMANDF(qa_actor_table, "", ()
{
    loopi(haloactordefcount())
    {
        const HaloActorDef *d = haloactordef(i);
        conoutf("ACTOR_TABLE %d %s %d %d %.3f %.3f %.3f", i, d->family.c_str(), d->kind, d->weapon, d->rate, d->burstMin, d->visionDistance);
        // The coll's shield model as parsed (actors.txt `resistance`, `shieldtime` and the material
        // leaks; tests/test_actor_tags.py): ACTOR_SHIELD <index> <resistance 0|1> <indirect>
        // <coll shield> <shield type> <failure fn> <threshold> <maximum> <shieldtime 0|1> <stun min>
        // <stun> <recharge> <velocity> <leaks, comma separated, - for none>
        string leaks = "";
        for(const HaloActorDef::Material &m : d->materials) concatformatstring(leaks, "%s%.3f", leaks[0] ? "," : "", m.leak);
        conoutf("ACTOR_SHIELD %d %d %d %.3f %d %d %.3f %.3f %d %.3f %.3f %.3f %.6f %s", i, d->hasResistance, d->indirectMaterial, d->collShield,
                d->shieldMaterial, d->shieldFailureFunction, d->shieldFailureThreshold, d->shieldFailureMaximum, d->hasShieldTime,
                d->shieldStunDamage, d->shieldStunTime, d->shieldRechargeTime, d->shieldRechargeVelocity, leaks[0] ? leaks : "-");
        // ACTOR_TIMER <index> <actr melee delay s> <actv encounter grenade timeout s> (-1: not in the table)
        conoutf("ACTOR_TIMER %d %.3f %.3f", i, d->meleeDelayTimer, d->grenadeTimeout);
    }
    fflush(stdout);
});
// Sets an AI switch on every campaign actor: qa_actor_flag 1 1 blinds them all.
COMMANDF(qa_actor_flag, "ii", (int *flag, int *on)
{
    loopv(enemies) if(enemies[i].bot) haloactorsetflag(enemies[i].bot, *flag, *on != 0);
});

// ---- Script pacing commands (tests/test_script_coverage.py)
// qa_hs_call hud_get_timer_ticks 0 0 prints HS_CALL <command> <value>.
COMMANDF(qa_hs_call, "sff", (const char *command, float *first, float *second)
{
    conoutf("HS_CALL %s %.4f", command, haloscenariocall(command, *first, *second));
    fflush(stdout);
});
// qa_hs_script endgame_cinematics 1 wakes (0), or reports started (1) or finished (2) for a script;
// 3 runs a static script once as a thread of its own (wake starts only startup, dormant and continuous ones).
COMMANDF(qa_hs_script, "si", (const char *name, int *action)
{
    conoutf("HS_SCRIPT %s %d %.0f", name, *action, haloscenarioqascript(name, *action));
    fflush(stdout);
});

// ---- Ending (tests/test_campaign_ending.py)
// Plays the section's exit scene, as reaching the final objective does.
COMMANDF(qa_halo_finale, "", () { if(haloactive()) finishSection(); });
COMMANDF(qa_halo_ending, "", ()
{
    conoutf("QA_ENDING ending=%d done=%d beaten=%d won=%d finished=%d cinematic=%d alive=%d title=%d",
        ending, endingDone, campaignBeaten, winPending, campaignFinished, halocinematic(), player1->state == CS_ALIVE,
        titleScreen && menuvisible());
    fflush(stdout);
});

// ---- Mission exits and script scenes (tests/test_mission_exits.py)
// qa_hs_cue cinematic_captain plays a scene as the mission script's call does (a Call cue row).
COMMANDF(qa_hs_cue, "s", (const char *script)
{
    conoutf("QA_CUE %s %d", script, haloscenarioqacue(script));
    fflush(stdout);
});
// Skips the scene playing, if one is (QA_SKIP 1), and nothing else: halo_next with no scene
// playing goes on to the next section (tests/test_mission_exits.py item 6).
COMMANDF(qa_scene_skip, "", ()
{
    conoutf("QA_SKIP %d", halocinematicskip());
    fflush(stdout);
});
// qa_device_melee melee_door_2 3 1 LABEL stands the player 3 world units before the machine (side
// 1, or -1 behind it), level and facing it, and swings the melee (halomelee): a machine placed
// "opened by melee attack" opens (Scenario::meleeDevice, the fresh-start batch).
// DEVICE_QA melee <label> object=<name> placed=<0|1> position=<device_get_position after>
COMMANDF(qa_device_melee, "sfis", (const char *name, float *distance, int *side, const char *label)
{
    vec feet;
    float yaw;
    const bool placed = haloscenarioqadevicefront(name, *distance, *side, feet, yaw);
    if(placed)
    {
        qaPlaceFeet(feet);
        player1->yaw = yaw;
        player1->pitch = 0;
        halomelee();
    }
    conoutf("DEVICE_QA melee %s object=%s placed=%d position=%.4f", label, name, placed,
            haloscenariocall("device_get_position", haloscenarioobject(name), 0));
    fflush(stdout);
});
// qa_halo_flag river_hide_flag stands the player at a cutscene flag (unchecked, as a QA teleport).
COMMANDF(qa_halo_flag, "s", (const char *flag)
{
    vec feet;
    float yaw;
    const bool found = haloscenarioflagpoint(flag, feet, yaw);
    if(found)
    {
        qaPlaceFeet(feet);
        player1->yaw = yaw;
    }
    conoutf("QA_FLAG %s %d", flag, found);
    fflush(stdout);
});
// One QA_PLAYER line: the map, the player's feet and whether a scene is playing.
COMMANDF(qa_halo_where, "", ()
{
    conoutf("QA_PLAYER map=%s feet=%.2f,%.2f,%.2f yaw=%.1f cinematic=%d alive=%d", getclientmap(), player1->o.x, player1->o.y,
            player1->o.z - player1->eyeheight, player1->yaw, halocinematic(), player1->state == CS_ALIVE);
    fflush(stdout);
});
// Queues a section as real campaign play (not a standalone preview): 1-based mission and section.
COMMANDF(qa_campaign_play, "ii", (int *m, int *s)
{
    loadProgress();
    queueSection(*m - 1, *s - 1);
});

// Difficulty (tests/test_difficulty.py): the selected level, the script's view of it, enemy_damage
// and the Covenant's (enemy) vitality, then the team values (halodifficultyteamvalue) for the
// Covenant, the marines and the player, whether actors.txt carried the globals table, and one
// DIFFICULTY_DAMAGE line of halodifficultydamagefactor per owner>target team pair ("none": -1).
static void qaDifficulty()
{
    auto team = [](int type, int t) { return halodifficultyteamvalue(type, t); };
    conoutf("DIFFICULTY %d script=%d damage=%.2f health=%.2f shield=%.2f recharge=%.2f human=%.2f,%.2f,%.2f player=%.2f,%.2f,%.2f "
            "none=%.2f table=%d", halodifficulty(), int(haloscenariocall("game_difficulty_get", 0, 0)),
            halodifficultyget(HALO_DIFFICULTY_ENEMY_DAMAGE), team(HALO_DIFFICULTY_ENEMY_VITALITY, HALO_TEAM_COVENANT),
            team(HALO_DIFFICULTY_ENEMY_SHIELD, HALO_TEAM_COVENANT), team(HALO_DIFFICULTY_ENEMY_RECHARGE, HALO_TEAM_COVENANT),
            team(HALO_DIFFICULTY_ENEMY_VITALITY, HALO_TEAM_HUMAN), team(HALO_DIFFICULTY_ENEMY_SHIELD, HALO_TEAM_HUMAN),
            team(HALO_DIFFICULTY_ENEMY_RECHARGE, HALO_TEAM_HUMAN), team(HALO_DIFFICULTY_ENEMY_VITALITY, HALO_TEAM_PLAYER),
            team(HALO_DIFFICULTY_ENEMY_SHIELD, HALO_TEAM_PLAYER), team(HALO_DIFFICULTY_ENEMY_RECHARGE, HALO_TEAM_PLAYER),
            team(HALO_DIFFICULTY_ENEMY_VITALITY, -1), halodifficultytable() ? 1 : 0);
    static const int pairs[][2] = {{HALO_TEAM_COVENANT, HALO_TEAM_PLAYER}, {HALO_TEAM_COVENANT, HALO_TEAM_HUMAN},
                                   {HALO_TEAM_COVENANT, HALO_TEAM_COVENANT}, {HALO_TEAM_COVENANT, HALO_TEAM_FLOOD},
                                   {HALO_TEAM_FLOOD, HALO_TEAM_COVENANT}, {HALO_TEAM_HUMAN, HALO_TEAM_PLAYER},
                                   {HALO_TEAM_HUMAN, HALO_TEAM_COVENANT}, {HALO_TEAM_PLAYER, HALO_TEAM_COVENANT},
                                   {HALO_TEAM_PLAYER, HALO_TEAM_PLAYER}, {-1, HALO_TEAM_PLAYER}, {HALO_TEAM_COVENANT, -1}};
    for(const auto &p : pairs)
    {
        float divisor = 1;
        const float factor = halodifficultydamagefactor(p[0], p[1], &divisor);
        conoutf("DIFFICULTY_DAMAGE %s>%s factor=%.2f divisor=%.2f", haloteamname(p[0]), haloteamname(p[1]), factor, divisor);
    }
    fflush(stdout);
}
COMMANDF(qa_difficulty, "", () { qaDifficulty(); });
// Major upgrades (tests/test_major_upgrades.py): the placed squad members' variants and the error
// diffusion state of the loaded section.
COMMANDF(qa_major_upgrades, "", () { haloscenarioqamajors(); fflush(stdout); });
// The engine's diffusion on its own, off any scenario: `squads` squads of `per` actors each,
// placed squad after squad from fresh errors and `seed` (hex), with the squad major_upgrade enum
// `kind` on the chosen difficulty. MAJOR_SIM kind level force random chance squads per seed, then
// the majors per squad and the final map-wide error.
static void qaMajorUpgradeSim(int kind, int squads, int per, const char *seedText)
{
    bool force = false, random = false;
    float chance = 0;
    halomajorupgradechance(kind, halodifficulty(), force, random, chance);
    uint32_t seed = uint32_t(strtoul(seedText && *seedText ? seedText : "deadbeef", nullptr, 16));
    float global = 0;
    std::string counts;
    for(int s = 0; s < clamp(squads, 0, 1000); ++s)
    {
        float squadError = 0;
        int majors = 0;
        for(int k = 0; k < clamp(per, 0, 1000); ++k)
            majors += random ? haloconsidermajorupgrade(global, squadError, seed, chance) : force;
        counts += " " + std::to_string(majors);
    }
    conoutf("MAJOR_SIM kind=%d level=%d force=%d random=%d chance=%.4f squads=%d per=%d majors=%s global=%.6f table=%d", kind,
            halodifficulty(), force, random, chance, squads, per, counts.c_str(), global, halodifficultytable() ? 1 : 0);
    fflush(stdout);
}
COMMANDF(qa_major_upgrade_sim, "iiis", (int *kind, int *squads, int *per, const char *seedText) { qaMajorUpgradeSim(*kind, *squads, *per, seedText); });
