// Damage model verification hooks (tests/test_damage_model.py): jpt! range and blast falloff to
// the lower bound, actors' grenade rolls, exact (fractional) hits, EMP on shields and vehicles.
// Compiled into halo.cpp only for halo-ac-qa; they read the campaign state there. Lines start with
// DAMAGE_QA, DAMAGE_TABLE, DAMAGE_GRENADE or DAMAGE_ACTOR (and DAMAGE_TRACE with halo_damagetrace 1).

// DAMAGE_TABLE weapon=<index> trigger=<n> lower= umin= umax= start= end= inner= outer= side= cat=
//              at0= atStart= atMid= atEnd= atFar=   (the upper min's hit at those distances, world units)
//              core=<AOE core radius> super=<0|1> super_timer= super_inner= super_outer= super_umin=
//              super_side= melee_side=<the weapon's melee side effect>
// DAMAGE_GRENADE type=<n> inner= outer= lower= umin= umax= side= centre= mid= edge=   (the retail roll)
//              actor_min= actor_max= player_min= player_max=   (400 rolls each: actors [lower, umax])
//              attached_actor_min= attached_actor_max=
// DAMAGE_QA falloff <distance> <start> <end> = <scale>   for a few fixed cases
static void qaDamageTable()
{
    loopi(haloweaponcount())
    {
        const HaloWeaponDef *d = haloweapon(i);
        loopj(int(d->triggers.size()))
        {
            const HaloProjectileDef &p = d->triggers[j].projectile;
            if(!p.valid) continue;
            const HaloDamage &g = p.damage;
            auto at = [&](float distance) { return g.scaled(g.upperMin, halofalloff(distance, p.rangeStart, p.rangeEnd)); };
            conoutf("DAMAGE_TABLE weapon=%d trigger=%d lower=%.2f umin=%.2f umax=%.2f start=%.2f end=%.2f inner=%.2f outer=%.2f side=%d cat=%d "
                    "at0=%.2f atStart=%.2f atMid=%.2f atEnd=%.2f atFar=%.2f core=%.2f super=%d super_timer=%.2f super_inner=%.2f "
                    "super_outer=%.2f super_umin=%.2f super_side=%d melee_side=%d path=%s",
                    i, j, g.lower, g.upperMin, g.upperMax, p.rangeStart, p.rangeEnd, p.innerRadius, p.radius, p.sideEffect, p.category,
                    at(0), at(p.rangeStart), at((p.rangeStart + p.rangeEnd) * .5f), at(p.rangeEnd), at(p.rangeEnd * 2 + 1), g.core,
                    int(p.super.valid), p.super.timer, p.super.innerRadius, p.super.radius, p.super.damage.upperMin, p.super.sideEffect,
                    d->meleeSideEffect, d->path.c_str());
        }
    }
    loopi(HALO_GRENADE_TYPES)
    {
        const HaloGrenadeDef *d = halogrenadedef(i);
        if(!d) continue;
        float actorMin = 1e9f, actorMax = -1e9f, playerMin = 1e9f, playerMax = -1e9f, attachedMin = 1e9f, attachedMax = -1e9f;
        loopj(400)
        {
            const float a = d->blast.rollFromLower(), p = d->blast.roll(), s = d->attached.rollFromLower();
            actorMin = min(actorMin, a); actorMax = max(actorMax, a);
            playerMin = min(playerMin, p); playerMax = max(playerMax, p);
            attachedMin = min(attachedMin, s); attachedMax = max(attachedMax, s);
        }
        const HaloDamage &b = d->blast;
        auto at = [&](float distance) { return b.scaled(b.upperMin, halofalloff(distance, d->innerRadius, d->outerRadius)); };
        conoutf("DAMAGE_GRENADE type=%d inner=%.2f outer=%.2f lower=%.2f umin=%.2f umax=%.2f side=%d centre=%.2f mid=%.2f edge=%.2f "
                "actor_min=%.2f actor_max=%.2f player_min=%.2f player_max=%.2f attached_actor_min=%.2f attached_actor_max=%.2f core=%.2f",
                i, d->innerRadius, d->outerRadius, b.lower, b.upperMin, b.upperMax, d->sideEffect, at(0),
                at((d->innerRadius + d->outerRadius) * .5f), at(d->outerRadius), actorMin, actorMax, playerMin, playerMax,
                attachedMin, attachedMax, b.core);
    }
    static const float cases[][3] = {{0, 1.5f, 3}, {1.5f, 1.5f, 3}, {2.25f, 1.5f, 3}, {3, 1.5f, 3}, {9, 1.5f, 3}, {50, 0, 0}, {999, 1000, 1000}, {1000, 1000, 1000}};
    for(const auto &c : cases) conoutf("DAMAGE_QA falloff %.3f %.3f %.3f = %.4f", c[0], c[1], c[2], halofalloff(c[0], c[1], c[2]));
    fflush(stdout);
}
COMMANDF(qa_damage_table, "", () { qaDamageTable(); });

static void qaDamageVitals(const char *label)
{
    conoutf("DAMAGE_QA %s hp=%d shield=%.3f overshield=%.3f carry=%.3f alive=%d", label, player1->health, shield, overshield,
            playerHealthCarry, player1->state == CS_ALIVE);
    fflush(stdout);
}

// The nearest living hostile actor, with a shield when `shielded` (an Elite or a Jackal).
static HaloEnemy *qaDamageActor(bool shielded)
{
    HaloEnemy *found = NULL;
    float best = 1e16f;
    loopv(enemies)
    {
        HaloEnemy &e = enemies[i];
        if(!e.bot || e.bot->state != CS_ALIVE || haloteamfriendly(e.team) || (shielded && e.shieldMax <= 0)) continue;
        const float d = player1->o.squareddist(e.bot->o);
        if(d < best) { best = d; found = &e; }
    }
    return found;
}

// Sets the player's health and shield (no overshield, no carried fraction), for the hits below.
COMMANDF(qa_damage_vitals, "if", (int *health, float *value)
{
    player1->health = clamp(*health, 1, playerHealth);
    shield = clamp(*value, 0.0f, shieldCap);
    overshield = overshieldTarget = 0;
    playerHealthCarry = 0;
    qaDamageVitals("vitals");
});
// An exact hit of `amount` with jpt! side effect `side` on the player, from the nearest hostile
// actor (`actor` 1; the plain enemy gun, GUN_SUBGUN, when `plasma` is 1) or from the player.
COMMANDF(qa_damage_hit, "fiii", (float *amount, int *side, int *actor, int *plasma)
{
    HaloEnemy *from = *actor ? qaDamageActor(false) : NULL;
    playerent *attacker = from ? (playerent *)from->bot : (playerent *)player1;
    if(*actor && !from) { conoutf("DAMAGE_QA hit no-actor"); fflush(stdout); return; }
    halohit(*amount, *side, player1, attacker, vec(0, 0, 1), *plasma ? GUN_SUBGUN : GUN_GRENADE, false, 0);
    qaDamageVitals("hit");
});
// The player's exact hit of `amount` with side effect `side` on the nearest shielded hostile actor.
// DAMAGE_ACTOR <label> name= kind= shield= shield_max= health= carry= alive=
COMMANDF(qa_damage_actor, "fis", (float *amount, int *side, const char *label)
{
    HaloEnemy *e = qaDamageActor(true);
    if(!e) { conoutf("DAMAGE_ACTOR %s no-actor", label); fflush(stdout); return; }
    botent *bot = e->bot;
    if(*amount > 0) halohit(*amount, *side, bot, player1, vec(0, 0, 1), GUN_GRENADE, false, 0);
    conoutf("DAMAGE_ACTOR %s name=%s kind=%d shield=%.3f shield_max=%.3f health=%d carry=%.3f alive=%d", label, bot->name, int(e->kind),
            e->shield, e->shieldMax, bot->health, e->healthCarry, bot->state == CS_ALIVE);
    fflush(stdout);
});
// Every living actor's weapon and damage modifier, for bounding its traced shots:
// DAMAGE_ACTOR weapon name= kind= team= weapon=<weapons.txt index> scale=<modifier>
static void qaDamageActors()
{
    loopv(enemies)
    {
        const HaloEnemy &e = enemies[i];
        if(!e.bot || e.bot->state != CS_ALIVE) continue;
        conoutf("DAMAGE_ACTOR weapon name=%s kind=%d team=%d weapon=%d scale=%.3f", e.bot->name, int(e.kind), e.team,
                e.weapon.def, e.damageScale);
    }
    fflush(stdout);
}
COMMANDF(qa_damage_actors, "", () { qaDamageActors(); });
// EMP on vehicle `vehicle` alone (as a plasma grenade stuck to it), for `ms`.
COMMANDF(qa_damage_emp_vehicle, "ii", (int *vehicle, int *ms)
{
    conoutf("DAMAGE_QA emp vehicle=%d stunned=%d", *vehicle, halovehicleemp(vec(0, 0, -1e6f), 0, *vehicle, *ms));
    fflush(stdout);
});

// Damage targets (tests/test_damage_targets.py; d40's manifold detectors, halo_script.cpp
// traceTargets/damageTarget). qa_target_state NAME LABEL prints
// TARGET_QA LABEL name= found= health= damageable= centre=x,y,z
static void qaTargetState(const char *name, const char *label)
{
    vec centre(0, 0, 0);
    float health = -1;
    bool damageable = false;
    const bool found = haloscenariotargetstate(name, centre, health, damageable);
    conoutf("TARGET_QA %s name=%s found=%d health=%.3f damageable=%d centre=%.2f,%.2f,%.2f", label, name, found, health,
            damageable, centre.x, centre.y, centre.z);
    fflush(stdout);
}
COMMANDF(qa_target_state, "ss", (const char *name, const char *label) { qaTargetState(name, label); });
// qa_target_trace NAME: traces from the player's eye to the target's centre, as a shot would, and
// prints which target the trace meets first (TARGET_QA trace name= hit=<name or -> fraction=).
COMMANDF(qa_target_trace, "s", (const char *name)
{
    vec centre;
    float health;
    bool damageable;
    if(!haloscenariotargetstate(name, centre, health, damageable)) { conoutf("TARGET_QA trace name=%s hit=- missing=1", name); fflush(stdout); return; }
    const vec end = vec(centre).sub(player1->o).mul(1.5f).add(player1->o);
    float fraction = 1;
    const int hit = haloscenariotargettrace(player1->o, end, fraction);
    conoutf("TARGET_QA trace name=%s hit=%s fraction=%.3f", name, hit >= 0 ? haloscenarioobjectname(hit) : "-", fraction);
    fflush(stdout);
});
// qa_target_damage NAME AMOUNT: the player's damage dealt straight to the target, as a hit would.
COMMANDF(qa_target_damage, "sf", (const char *name, float *amount)
{
    const int id = haloscenarioobject(name);
    const bool dealt = id >= 0 && haloscenariotargetdamage(id, *amount);
    conoutf("TARGET_QA damage name=%s amount=%.1f dealt=%d", name, *amount, dealt);
    fflush(stdout);
});
// qa_target_look NAME: turns the player to face the target's centre (for a real shot with qa_halo_fire).
COMMANDF(qa_target_look, "s", (const char *name)
{
    vec centre;
    float health;
    bool damageable;
    if(!haloscenariotargetstate(name, centre, health, damageable)) return;
    const vec look = vec(centre).sub(player1->o);
    player1->yaw = fmodf(atan2f(look.y, look.x) / RAD + 90.0f + 360.0f, 360.0f);
    player1->pitch = atan2f(look.z, max(look.magnitudexy(), .01f)) / RAD;
    conoutf("TARGET_QA look name=%s distance=%.2f", name, look.magnitude());
    fflush(stdout);
});
// qa_target_aim X Y Z: turns the player's eye to a world point (a route's shoot row aim point).
COMMANDF(qa_target_aim, "fff", (float *x, float *y, float *z)
{
    const vec look = vec(*x, *y, *z).sub(player1->o);
    player1->yaw = fmodf(atan2f(look.y, look.x) / RAD + 90.0f + 360.0f, 360.0f);
    player1->pitch = atan2f(look.z, max(look.magnitudexy(), .01f)) / RAD;
    conoutf("TARGET_QA aim distance=%.2f yaw=%.1f pitch=%.1f", look.magnitude(), player1->yaw, player1->pitch);
    fflush(stdout);
});

// ---- The fidelity gaps of batch 9 (tests/test_melee_materials.py)

// qa_damage_keep 1: the hooks below leave the actor as it is instead of renewing it (a second hit
// on a drained or half shield, tests/test_melee_materials.py items 8 and 10).
static bool qaDamageKeep = false;
COMMANDF(qa_damage_keep, "i", (int *on) { qaDamageKeep = *on != 0; });

// The nearest living hostile actor of body kind `kind` (ActorKind; -1 any), renewed: full health
// and shield, nothing carried (unless qa_damage_keep 1). `friendly`: the nearest of the player's
// allies instead.
static HaloEnemy *qaDamageRenewed(int kind, bool friendly = false)
{
    HaloEnemy *found = NULL;
    float best = 1e16f;
    loopv(enemies)
    {
        HaloEnemy &e = enemies[i];
        if(!e.bot || e.bot->state != CS_ALIVE || haloteamfriendly(e.team) != friendly || (kind >= 0 && int(e.kind) != kind)) continue;
        const float d = player1->o.squareddist(e.bot->o);
        if(d < best) { best = d; found = &e; }
    }
    if(found && !qaDamageKeep)
    {
        haloactorrenew(found->bot);
        found->shield = found->shieldMax;
        found->healthCarry = 0;
    }
    return found;
}

// The player's feet `gap` world units from the actor at `bearing` degrees from where it faces
// (0 in front, 180 behind), facing the actor's middle.
static void qaDamageStandBy(const playerent *bot, float bearing, float gap)
{
    const float yaw = (bot->yaw + bearing) * RAD, s = haloscenarioscale();
    const vec feet = vec(bot->o).sub(vec(0, 0, bot->eyeheight)).add(vec(sinf(yaw), -cosf(yaw), 0).mul(gap * s));
    player1->o = vec(feet).add(vec(0, 0, player1->eyeheight));
    player1->vel = vec(0, 0, 0);
    player1->resetinterp();
    haloplayermoved("QA teleport");
    vec body = bot->o;
    body.z -= bot->eyeheight * .5f;
    const vec look = vec(body).sub(player1->o);
    player1->yaw = fmodf(atan2f(look.y, look.x) / RAD + 90.0f + 360.0f, 360.0f);
    player1->pitch = atan2f(look.z, max(look.magnitudexy(), .01f)) / RAD;
}

static void qaDamageReport(const char *label, const HaloEnemy *e)
{
    const botent *bot = e->bot;
    conoutf("DAMAGE_REGION %s name=%s kind=%d shield=%.3f shield_max=%.3f health=%d alive=%d status=%d", label, bot ? bot->name : "-",
            int(e->kind), e->shield, e->shieldMax, bot ? bot->health : 0, bot && bot->state == CS_ALIVE, bot ? haloactorstatus(bot) : 0);
    fflush(stdout);
}

// qa_damage_melee BEHIND AWARE LABEL: the nearest hostile actor, renewed, with the player put
// 0.6 world units behind it (BEHIND 1) or in front of it, facing it; AWARE 1 puts it in combat
// with the player, 0 unaware of everyone; then the player's melee (halomelee, with its jpt!
// side effect: lethal to the unsuspecting). DAMAGE_REGION <label> ... alive=<0|1>
COMMANDF(qa_damage_melee, "iis", (int *behind, int *aware, const char *label)
{
    HaloEnemy *e = qaDamageRenewed(-1);
    if(!e) { conoutf("DAMAGE_REGION %s no-actor", label); fflush(stdout); return; }
    qaDamageStandBy(e->bot, *behind ? 180.0f : 0.0f, .6f);
    haloactorforceawareness(e->bot, *aware != 0, player1);
    halomelee();
    qaDamageReport(label, e);
});

// qa_damage_melee_friend LABEL: as qa_damage_melee 1 0 on the nearest of the player's allies (a
// marine; a50's Keyes once attached): the player's melee from behind on an ally that is not in
// combat. Batch 5's critic: allies and attached story figures are spared the kill from behind
// (DAMAGE_UNSUSPECTING_SPARED), the melee's normal damage still applies. DAMAGE_REGION <label> ...
COMMANDF(qa_damage_melee_friend, "s", (const char *label)
{
    HaloEnemy *e = qaDamageRenewed(-1, true);
    if(!e) { conoutf("DAMAGE_REGION %s no-actor", label); fflush(stdout); return; }
    qaDamageStandBy(e->bot, 180.0f, .6f);
    haloactorforceawareness(e->bot, false, player1);
    halomelee();
    qaDamageReport(label, e);
});

// qa_damage_hunter_act FIRED: the nearest hostile Hunter has just fired its cannon (FIRED 1:
// haloactorfired, as halofiretrigger records it) or just smashed (0: its lastaction set, as the
// smash does, and nothing else): the belly window opens only after a cannon shot (gap 11).
COMMANDF(qa_damage_hunter_act, "i", (int *fired)
{
    HaloEnemy *e = qaDamageRenewed(HUNTER);
    if(e && *fired) haloactorfired(e->bot);
    else if(e) e->bot->lastaction = lastmillis;
    conoutf("DAMAGE_HUNTER_ACT ok=%d fired=%d", e != NULL, *fired);
    fflush(stdout);
});

// qa_damage_region KIND BEARING HEIGHT WEAPON AMOUNT LABEL: the nearest hostile actor of body kind
// KIND (3 Hunter, -1 any), renewed, takes the player's exact hit of AMOUNT with the jpt! damage of
// weapons.txt weapon WEAPON's first projectile, from 3 world units at BEARING degrees off where
// it faces, struck at HEIGHT of its body (0 feet, 1 top). With halo_damagetrace 1 the hit's
// DAMAGE_MATERIAL line names the material it landed on. DAMAGE_REGION <label> ...
COMMANDF(qa_damage_region, "iffifs", (int *kind, float *bearing, float *height, int *weapon, float *amount, const char *label)
{
    HaloEnemy *e = qaDamageRenewed(*kind);
    const HaloWeaponDef *w = haloweapon(*weapon);
    const HaloTriggerDef *t = w ? w->trigger(0) : NULL;
    if(!e || !t || !t->projectile.valid) { conoutf("DAMAGE_REGION %s no-actor-or-weapon", label); fflush(stdout); return; }
    botent *bot = e->bot;
    qaDamageStandBy(bot, *bearing, 3);
    const float feet = bot->o.z - bot->eyeheight;
    const float tall = bot->eyeheight + bot->aboveeye;
    const vec at(bot->o.x, bot->o.y, feet + clamp(*height, 0.0f, 1.0f) * tall);
    const vec dir = vec(at).sub(player1->o).normalize();
    halohit(*amount, HALO_SIDE_EFFECT_NONE, bot, player1, dir, GUN_PISTOL, false, 0, &t->projectile.damage, &at);
    qaDamageReport(label, e);
});

// qa_damage_needles COUNT LABEL: COUNT of the player's needles strike the nearest hostile actor
// at once (haloqaneedles). DAMAGE_NEEDLES <label> needles= detonations= then its DAMAGE_REGION line.
COMMANDF(qa_damage_needles, "is", (int *count, const char *label)
{
    HaloEnemy *e = qaDamageRenewed(-1);
    if(!e) { conoutf("DAMAGE_NEEDLES %s no-actor", label); fflush(stdout); return; }
    const int detonations = haloqaneedles(e->bot, *count);
    conoutf("DAMAGE_NEEDLES %s needles=%d detonations=%d", label, *count, detonations);
    qaDamageReport(label, e);
});

// ---- Per-material shield leak and per-unit shield timing (tests/test_melee_materials.py items 8-10)

// qa_damage_blast KIND WEAPON AMOUNT LABEL: the nearest hostile actor of body kind KIND (2 Jackal,
// -1 any), renewed, takes the player's exact blast (GUN_GRENADE: the coll's indirect damage
// material) of AMOUNT with weapons.txt weapon WEAPON's first projectile jpt!, struck at its middle.
// With halo_damagetrace 1 the DAMAGE_SHIELD line names the material. DAMAGE_REGION <label> ...
COMMANDF(qa_damage_blast, "iifs", (int *kind, int *weapon, float *amount, const char *label)
{
    HaloEnemy *e = qaDamageRenewed(*kind);
    const HaloWeaponDef *w = haloweapon(*weapon);
    const HaloTriggerDef *t = w ? w->trigger(0) : NULL;
    if(!e || !t || !t->projectile.valid) { conoutf("DAMAGE_REGION %s no-actor-or-weapon", label); fflush(stdout); return; }
    botent *bot = e->bot;
    qaDamageStandBy(bot, 0, 3);
    const vec at(bot->o.x, bot->o.y, bot->o.z - bot->eyeheight * .5f);
    halohit(*amount, HALO_SIDE_EFFECT_NONE, bot, player1, vec(0, 0, 1), GUN_GRENADE, false, 0, &t->projectile.damage, &at);
    qaDamageReport(label, e);
});

// qa_damage_state KIND LABEL: the nearest hostile actor of body kind KIND as it is now (not
// renewed), for watching its shield come back. DAMAGE_REGION <label> ... then
// DAMAGE_SHIELD_STATE <label> since_hit=<ms since the stun last restarted>
COMMANDF(qa_damage_state, "is", (int *kind, const char *label)
{
    HaloEnemy *found = NULL;
    float best = 1e16f;
    loopv(enemies)
    {
        HaloEnemy &e = enemies[i];
        if(!e.bot || e.bot->state != CS_ALIVE || haloteamfriendly(e.team) || (*kind >= 0 && int(e.kind) != *kind)) continue;
        const float d = player1->o.squareddist(e.bot->o);
        if(d < best) { best = d; found = &e; }
    }
    if(!found) { conoutf("DAMAGE_REGION %s no-actor", label); fflush(stdout); return; }
    qaDamageReport(label, found);
    conoutf("DAMAGE_SHIELD_STATE %s since_hit=%d", label, lastmillis - found->hitTime);
    fflush(stdout);
});

// qa_shield_timing: one line per living actor (shield_max 0 for one without a shield), the stun and recharge it uses
// (enemyShieldTiming, before and after its team's recharge scale) and its variant's coll values:
// SHIELD_TIMING name= kind= team= variant= shield_max= delay= recharge= scaled= timed=<0|1>
//               stun_min= stun= recharge_s= velocity= leaks=<per material, comma separated> indirect=
//               shield_type= failure=<function>,<threshold>,<maximum>
static void qaShieldTiming()
{
    loopv(enemies)
    {
        const HaloEnemy &e = enemies[i];
        if(!e.bot || e.bot->state != CS_ALIVE) continue;
        int delay = 0, recharge = 0;
        enemyShieldTiming(e, delay, recharge);
        const HaloActorDef *def = haloactorvariant(e.bot);
        string leaks = "";
        if(def) for(const HaloActorDef::Material &m : def->materials) concatformatstring(leaks, "%s%.3g", leaks[0] ? "," : "", m.leak);
        conoutf("SHIELD_TIMING name=%s kind=%d team=%d variant=%d shield_max=%.3f delay=%d recharge=%d scaled=%d timed=%d stun_min=%.3f "
                "stun=%.3f recharge_s=%.3f velocity=%.6f leaks=%s indirect=%d shield_type=%d failure=%d,%.3f,%.3f",
                e.bot->name, int(e.kind), e.team, def ? def->index : -1, e.shieldMax, delay, recharge,
                recharge > 0 ? rechargeTime(recharge, haloactorteam(e.bot)) : 0, def && def->hasShieldTime,
                def ? def->shieldStunDamage : 0.0f, def ? def->shieldStunTime : 0.0f, def ? def->shieldRechargeTime : 0.0f,
                def ? def->shieldRechargeVelocity : 0.0f, leaks[0] ? leaks : "-", def ? def->indirectMaterial : -1,
                def ? def->shieldMaterial : -1, def ? def->shieldFailureFunction : 0, def ? def->shieldFailureThreshold : 0.0f,
                def ? def->shieldFailureMaximum : 0.0f);
    }
    fflush(stdout);
}
COMMANDF(qa_shield_timing, "", () { qaShieldTiming(); });
