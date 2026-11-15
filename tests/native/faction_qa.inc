// QA hooks for factions and special enemies (tests/test_factions.py). Included by halo.cpp in the
// QA build only, inside its file scope after the campaign hooks (enemies, spawnEnemy, profiles).
//
// Written while testing was on hold: none of these has been run yet.

// The first actor variant whose actv path contains `text` (spaces allowed), or -1.
static int qaVariant(const char *text)
{
    loopi(haloactordefcount())
    {
        const HaloActorDef *d = haloactordef(i);
        if(d && d->path.find(text) != std::string::npos && d->kind >= 0 && d->kind < ACTOR_COUNT) return i;
    }
    return -1;
}

// qa_variant_spawn "stealth elite plasma rifle" -1 8 0: spawns a variant at forward/side offsets
// (Halo world units) from the player on an engine team (-1: the variant's own; 0 covenant,
// 1 human, 2 flood, 3 sentinel, 4 player, 5 other). Hostiles face the player.
// FACTION_SPAWN <name> kind=<ActorKind> team=<team name> variant=<index> hp=<health> shield=<fraction>
static void qaVariantSpawn(const char *text, int team, float forward, float side)
{
    const int variant = qaVariant(text);
    const HaloActorDef *def = haloactordef(variant);
    botent *bot = nullptr;
    if(def)
    {
        const float yaw = player1->yaw * RAD, s = haloscenarioscale();
        vec at = player1->o;
        at.add(vec(sinf(yaw), -cosf(yaw), 0).mul(forward * s)).add(vec(cosf(yaw), sinf(yaw), 0).mul(side * s));
        const int resolved = team >= 0 ? team : haloactordefaultteam(def, kindTeam(ActorKind(def->kind)));
        bot = spawnEnemy(ActorKind(def->kind), at, player1->yaw + (haloteamfriendly(resolved) ? 0 : 180), -1, variant, team);
    }
    conoutf("FACTION_SPAWN %s kind=%d team=%s variant=%d hp=%d shield=%.2f", bot ? bot->name : "-", def ? def->kind : -1,
            bot ? haloteamname(haloactorteam(bot)) : "none", variant, bot ? bot->health : 0, bot ? haloenemyshieldfraction(bot) : 0.0f);
    fflush(stdout);
}
COMMANDF(qa_variant_spawn, "siff", (const char *text, int *team, float *forward, float *side) { qaVariantSpawn(text, *team, *forward, *side); });

// qa_allegiance_state <label>: every team pair. FACTION_ALLEGIANCE <label> <a>-<b> allied=<0|1> broken=<0|1>
COMMANDF(qa_allegiance_state, "s", (const char *label)
{
    loopi(HALO_TEAM_COUNT) for(int j = i + 1; j < HALO_TEAM_COUNT; ++j)
        conoutf("FACTION_ALLEGIANCE %s %s-%s allied=%d broken=%d", label, haloteamname(i), haloteamname(j),
                !haloteamhostile(i, j), haloallegiancebroken(i, j));
    fflush(stdout);
});

// qa_actor_turn <kind> <degrees>: every living actor of a kind turns so that 0 faces the player
// and 180 shows it its back, and holds still (braindead) so the facing stays.
COMMANDF(qa_actor_turn, "ii", (int *kind, int *degrees)
{
    loopv(enemies)
    {
        botent *bot = enemies[i].bot;
        if(!bot || bot->state != CS_ALIVE || enemies[i].kind != *kind) continue;
        haloactorsetflag(bot, HALO_AI_BRAINDEAD, true);
        const vec delta = vec(player1->o).sub(bot->o);
        const float facing = atan2f(delta.x, -delta.y) / RAD;
        bot->yaw = bot->targetyaw = facing + *degrees;
        bot->move = bot->strafe = 0;
        conoutf("FACTION_TURN %s yaw=%.0f", bot->name, bot->yaw);
    }
    fflush(stdout);
});

// Hits the first living actor of a kind from the player with a weapon's jpt! damage (weapons.txt
// index; its first trigger's projectile, else its melee), as a direct hit or (blast 1) as area
// damage. FACTION_HIT kind=<k> weapon=<index> blast=<0|1> amount=<a> before=<health> after=<health>
//             shield_before=<fraction> shield_after=<fraction>
static void qaActorHit(int kind, int weapon, float amount, bool blast)
{
    const HaloWeaponDef *w = haloweapon(weapon);
    const HaloTriggerDef *t = w ? w->trigger() : nullptr;
    const HaloDamage *source = t && t->projectile.valid ? &t->projectile.damage : w ? &w->melee : nullptr;
    loopv(enemies)
    {
        botent *bot = enemies[i].bot;
        if(!bot || bot->state != CS_ALIVE || enemies[i].kind != kind) continue;
        const int before = bot->health;
        const float shieldBefore = haloenemyshieldfraction(bot);
        vec dir = vec(bot->o).sub(player1->o);
        if(dir.iszero()) dir = vec(0, 1, 0);
        halohit(amount, HALO_SIDE_EFFECT_NONE, bot, player1, dir.normalize(), blast ? GUN_GRENADE : GUN_PISTOL, false, 0, source);
        conoutf("FACTION_HIT kind=%d weapon=%d blast=%d amount=%.1f before=%d after=%d shield_before=%.2f shield_after=%.2f",
                kind, weapon, blast, amount, before, bot->state == CS_ALIVE ? bot->health : 0, shieldBefore,
                bot->state == CS_ALIVE ? haloenemyshieldfraction(bot) : 0.0f);
        break;
    }
    fflush(stdout);
}
COMMANDF(qa_actor_hit, "iif", (int *kind, int *weapon, float *amount) { qaActorHit(*kind, *weapon, *amount, false); });
COMMANDF(qa_actor_blast, "iif", (int *kind, int *weapon, float *amount) { qaActorHit(*kind, *weapon, *amount, true); });

// qa_weapon_materials <index>: the jpt! modifiers the table carries for the special bodies.
// FACTION_MATERIALS <index> <path> has=<0|1> armor=<12> skin=<13> sentinel=<25> monitor=<26> shield=<32>
COMMANDF(qa_weapon_materials, "i", (int *index)
{
    const HaloWeaponDef *w = haloweapon(*index);
    const HaloTriggerDef *t = w ? w->trigger() : nullptr;
    const HaloDamage *d = t && t->projectile.valid ? &t->projectile.damage : w ? &w->melee : nullptr;
    if(d) conoutf("FACTION_MATERIALS %d %s has=%d armor=%.2f skin=%.2f sentinel=%.2f monitor=%.2f shield=%.2f", *index,
                  w->path.c_str(), d->hasMaterials, d->material(HALO_MATERIAL_HUNTER_ARMOR), d->material(HALO_MATERIAL_HUNTER_SKIN),
                  d->material(HALO_MATERIAL_SENTINEL), d->material(HALO_MATERIAL_MONITOR), d->material(HALO_MATERIAL_HUNTER_SHIELD));
    fflush(stdout);
});

// qa_actor_cloak: FACTION_CLOAK <name> cloak=<0..1> alpha=<opacity drawn, -1 opaque> camo=<actv flag>
COMMANDF(qa_actor_cloak, "", ()
{
    loopv(enemies)
    {
        botent *bot = enemies[i].bot;
        if(!bot || bot->state != CS_ALIVE) continue;
        const HaloActorDef *def = haloactorvariant(bot);
        conoutf("FACTION_CLOAK %s cloak=%.2f alpha=%.2f camo=%d", bot->name, haloactorcloak(bot), halomodelalpha(bot),
                def && def->camouflaged());
    }
    fflush(stdout);
});

// qa_actor_model: FACTION_MODEL <name> kind=<k> model=<model haloactormodel picks>
COMMANDF(qa_actor_model, "", ()
{
    loopv(enemies) if(enemies[i].bot)
    {
        const char *model = haloactormodel(enemies[i].bot);
        conoutf("FACTION_MODEL %s kind=%d model=%s", enemies[i].bot->name, int(enemies[i].kind), model ? model : "-");
    }
    fflush(stdout);
});

// qa_actor_kill_one <kind>: the player kills the first living actor of a kind (a Hunter's
// brother, an ally for the betrayal count). FACTION_KILL <name>
COMMANDF(qa_actor_kill_one, "i", (int *kind)
{
    loopv(enemies)
    {
        botent *bot = enemies[i].bot;
        if(!bot || bot->state != CS_ALIVE || enemies[i].kind != *kind) continue;
        conoutf("FACTION_KILL %s", bot->name);
        dodamage(INT_MAX, bot, player1, GUN_ASSAULT);
        break;
    }
    fflush(stdout);
});

// ATTACH_QA (tests/test_ai_attach.py): the actors ai_attach made of scripted bipeds, and those waiting.
COMMANDF(qa_attach_state, "s", (const char *label) { haloscenarioattachdump(label); });
