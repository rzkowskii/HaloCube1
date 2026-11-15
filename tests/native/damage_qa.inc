// Damage model verification hooks (tests/test_damage_model.py): jpt! range and blast falloff to
// the lower bound, actors' grenade rolls, exact (fractional) hits, EMP on shields and vehicles.
// Compiled into halo.cpp only for halo-ac-qa; they read the campaign state there. Lines start with
// DAMAGE_QA, DAMAGE_TABLE, DAMAGE_GRENADE or DAMAGE_ACTOR (and DAMAGE_TRACE with halo_damagetrace 1).

// DAMAGE_TABLE weapon=<index> trigger=<n> lower= umin= umax= start= end= inner= outer= side= cat=
//              at0= atStart= atMid= atEnd= atFar=   (the upper min's hit at those distances, world units)
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
                    "at0=%.2f atStart=%.2f atMid=%.2f atEnd=%.2f atFar=%.2f path=%s",
                    i, j, g.lower, g.upperMin, g.upperMax, p.rangeStart, p.rangeEnd, p.innerRadius, p.radius, p.sideEffect, p.category,
                    at(0), at(p.rangeStart), at((p.rangeStart + p.rangeEnd) * .5f), at(p.rangeEnd), at(p.rangeEnd * 2 + 1), d->path.c_str());
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
                "actor_min=%.2f actor_max=%.2f player_min=%.2f player_max=%.2f attached_actor_min=%.2f attached_actor_max=%.2f",
                i, d->innerRadius, d->outerRadius, b.lower, b.upperMin, b.upperMax, d->sideEffect, at(0),
                at((d->innerRadius + d->outerRadius) * .5f), at(d->outerRadius), actorMin, actorMax, playerMin, playerMax,
                attachedMin, attachedMax);
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
