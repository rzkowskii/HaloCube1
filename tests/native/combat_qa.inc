// Combat verification hooks (tests/test_combat_equipment.py): the two grenade types, sticky
// plasma grenades, scenario equipment, the overshield, active camouflage and the tag-driven
// weapon sounds. Compiled into halo.cpp only for halo-ac-qa; they read the campaign state there.
// Lines start with COMBAT_QA, ITEM_QA, SOUND_QA, COMBAT_ACTOR or GRENADE (halogrenadedump).

static void qaCombatState(const char *label)
{
    int items = 0;
    loopv(groundItems) if(groundItems[i].exists) ++items;
    conoutf("COMBAT_QA %s frag=%d plasma=%d type=%d hp=%d shield=%.2f overshield=%.2f camo=%.2f items=%d placed=%d "
            "alive=%d pos=%.1f,%.1f,%.1f",
            label, grenadeCount(HALO_GRENADE_FRAG), grenadeCount(HALO_GRENADE_PLASMA), grenadeType, player1->health, shield,
            overshield, haloplayercamo(), items, groundItems.length(), player1->state == CS_ALIVE,
            player1->o.x, player1->o.y, player1->o.z - player1->eyeheight);
    fflush(stdout);
}
COMMANDF(qa_combat_state, "s", (const char *label) { qaCombatState(label); });

// ITEM_QA <label> index=<n> equipment=<n> powerup=<n> grenade=<n> exists=<0|1> created=<0|1> object=<n> flags=<n>
//         pos=<x,y,z> distance=<feet distance>
COMMANDF(qa_combat_items, "s", (const char *label)
{
    const vec feet = feetOf(player1);
    loopv(groundItems)
    {
        const GroundItem &item = groundItems[i];
        const HaloEquipmentDef *d = haloequipment(item.def);
        conoutf("ITEM_QA %s index=%d equipment=%d powerup=%d grenade=%d exists=%d created=%d object=%d flags=%d pos=%.1f,%.1f,%.1f distance=%.1f",
                label, i, item.def, d ? d->powerup : -1, d ? d->grenade : -1, item.exists, item.created, item.object, item.flags,
                item.o.x, item.o.y, item.o.z, feet.dist(item.o));
    }
    fflush(stdout);
});

COMMANDF(qa_grenade_give, "ii", (int *type, int *count)
{
    if(*type < 0 || *type >= HALO_GRENADE_TYPES || !grenadeKnown(*type)) return;
    grenadeCount(*type) = clamp(*count, 0, grenadeMaximum(*type));
});
COMMANDF(qa_grenade_select, "i", (int *type) { if(*type >= 0 && *type < HALO_GRENADE_TYPES && grenadeKnown(*type)) grenadeType = *type; });
COMMANDF(qa_grenade_switch, "s", (const char *label) { switchGrenade(); qaCombatState(label); });
// The player takes no damage (as object_cannot_take_damage), so actors' grenades cannot end a run.
COMMANDF(qa_combat_protect, "i", (int *on) { playerProtected = *on != 0; });
COMMANDF(qa_grenade_throw, "", ()
{
    haloclearbolts();       // also ends the last throw's animation lock
    halofrag();
    qaCombatState("throw");
});
COMMANDF(qa_grenade_state, "s", (const char *label)
{
    const int count = halogrenadedump(label);
    conoutf("COMBAT_QA %s grenades=%d", label, count);
    fflush(stdout);
});

// Faces the nearest living hostile actor in sight and stands `distance` units from it (stock
// geometry or retail, wherever the actor stands), for a grenade thrown straight at it.
COMMANDF(qa_grenade_face_actor, "f", (float *distance)
{
    playerent *target = NULL;
    float best = 1e16f;
    loopv(enemies)
    {
        botent *bot = enemies[i].bot;
        if(!bot || bot->state != CS_ALIVE || haloteamfriendly(enemies[i].team)) continue;
        const float d = player1->o.squareddist(bot->o);
        if(d < best) { best = d; target = bot; }
    }
    if(!target) { conoutf("COMBAT_QA face no-actor"); fflush(stdout); return; }
    vec away = vec(player1->o).sub(target->o);
    away.z = 0;
    if(away.iszero()) away = vec(1, 0, 0);
    away.normalize().mul(*distance);
    player1->o = vec(target->o).add(away);
    player1->o.z = target->o.z - target->eyeheight + player1->eyeheight;
    player1->vel = vec(0, 0, 0);
    player1->resetinterp();
    haloplayermoved("QA teleport");
    vec direction = vec(target->o).sub(player1->o);
    direction.z -= target->eyeheight * .5f;
    player1->yaw = atan2f(direction.x, -direction.y) / RAD;
    if(player1->yaw < 0) player1->yaw += 360.0f;
    player1->pitch = atan2f(direction.z, direction.magnitudexy()) / RAD - 8;
    conoutf("COMBAT_QA face actor=%s distance=%.1f", target->name, player1->o.dist(target->o));
    fflush(stdout);
});

// The first equipment definition of a powerup kind (and grenade type), or for ammunition the one
// the current weapon takes.
static const HaloEquipmentDef *qaEquipmentOf(int powerup, int grenade)
{
    loopi(haloequipmentcount())
    {
        const HaloEquipmentDef *d = haloequipment(i);
        if(d->powerup != powerup || (powerup == HALO_POWERUP_GRENADE && d->grenade != grenade)) continue;
        if(powerup == HALO_POWERUP_NONE && !d->roundsFor(carried[currentCarried].def)) continue;
        return d;
    }
    return NULL;
}
// Drops a piece of equipment `forward` units ahead of the player's feet.
COMMANDF(qa_item_drop, "iif", (int *powerup, int *grenade, float *forward)
{
    const HaloEquipmentDef *d = qaEquipmentOf(*powerup, *grenade);
    const float yaw = player1->yaw * RAD;
    dropItem(d, vec(feetOf(player1)).add(vec(sinf(yaw), -cosf(yaw), 0).mul(*forward)), player1->yaw);
    conoutf("COMBAT_QA drop powerup=%d grenade=%d equipment=%d", *powerup, *grenade, d ? d->index : -1);
    fflush(stdout);
});
// Stands on the nearest placed equipment of a powerup kind (-1 any) that is on the ground.
COMMANDF(qa_item_goto, "i", (int *powerup)
{
    float best = 1e16f;
    int found = -1;
    loopv(groundItems)
    {
        const HaloEquipmentDef *d = haloequipment(groundItems[i].def);
        if(!groundItems[i].exists || !d || (*powerup >= 0 && d->powerup != *powerup)) continue;
        const float distance = player1->o.squareddist(groundItems[i].o);
        if(distance < best) { best = distance; found = i; }
    }
    if(found >= 0)
    {
        player1->o = vec(groundItems[found].o).add(vec(0, 0, player1->eyeheight + .1f));
        player1->vel = vec(0, 0, 0);
        player1->resetinterp();
        haloplayermoved("QA teleport");
    }
    conoutf("COMBAT_QA goto powerup=%d item=%d", *powerup, found);
    fflush(stdout);
});
COMMANDF(qa_camo_give, "i", (int *ms) { camoUntil = lastmillis + *ms; qaCombatState("camo"); });

// The nearest hostile actor throws its own grenade type at the player (as halo_ai.cpp's throw does).
COMMANDF(qa_enemy_throw, "", ()
{
    botent *thrower = NULL;
    float best = 1e16f;
    loopv(enemies)
    {
        botent *bot = enemies[i].bot;
        if(!bot || bot->state != CS_ALIVE || haloteamfriendly(enemies[i].team)) continue;
        const float d = player1->o.squareddist(bot->o);
        if(d < best) { best = d; thrower = bot; }
    }
    if(!thrower) { conoutf("COMBAT_QA enemy-throw none"); fflush(stdout); return; }
    vec direction = vec(player1->o).sub(thrower->o);
    direction.normalize().mul(1.6f);
    direction.z += .3f;
    const int type = haloactorgrenadetype(thrower);
    conoutf("COMBAT_QA enemy-throw actor=%s type=%d thrown=%d", thrower->name, type, halothrowgrenade(thrower, type, direction));
    fflush(stdout);
});
// COMBAT_ACTOR <label> name=<bot> kind=<ActorKind> team=<n> grenade_type=<n> grenades=<n>
COMMANDF(qa_actor_grenades, "s", (const char *label)
{
    loopv(enemies)
    {
        botent *bot = enemies[i].bot;
        if(!bot || bot->state != CS_ALIVE) continue;
        conoutf("COMBAT_ACTOR %s name=%s kind=%d team=%d grenade_type=%d grenades=%d", label, bot->name, int(enemies[i].kind),
                enemies[i].team, haloactorgrenadetype(bot), bot->mag[GUN_GRENADE]);
    }
    fflush(stdout);
});

// SOUND_QA <owner> <role> <sample> <0|1 decoded file present>, for every sound the tables name.
static void qaSound(const char *owner, const char *role, const char *sample)
{
    if(!sample) return;
    defformatstring(file)("packages/audio/halo/%s.wav", sample);
    conoutf("SOUND_QA %s %s %s %d", owner, role, sample, fileexists(findfile(file, "rb"), "rb"));
}
static const char *const qaSoundRoles[HALO_SOUND_ROLES] = {
    "fire", "charged", "empty", "cycle", "reload", "reload-empty", "melee", "ready", "overheat", "pickup",
    "throw", "explode", "armed", "zoom-in", "zoom-out", "loop",
};
COMMANDF(qa_sound_table, "", ()
{
    const char *const *roles = qaSoundRoles;
    loopi(haloweaponcount())
    {
        const HaloWeaponDef *d = haloweapon(i);
        if(!d->playable()) continue;
        loopj(HALO_SOUND_ROLES) qaSound(d->label.c_str(), roles[j], d->sound(j));
    }
    loopi(HALO_GRENADE_TYPES) if(const HaloGrenadeDef *g = halogrenadedef(i))
    {
        defformatstring(owner)("grenade-%d", i);
        loopj(HALO_SOUND_ROLES) qaSound(owner, roles[j], g->sound(j));
    }
    loopi(haloequipmentcount())
    {
        defformatstring(owner)("equipment-%d", i);
        qaSound(owner, "pickup", haloequipment(i)->sound(HALO_SOUND_PICKUP));
    }
    qaSound("interface", "shield-low", halointerfacesound("shield-low"));
    qaSound("interface", "health-low", halointerfacesound("health-low"));
    fflush(stdout);
});
