// Weapon and bot traces across stacked retail BSP floors in b30 (qa_b30_traces).
#include "cube.h"
#include "bot/bot.h"
#include "halo_script.h"

#include "halo_collision.h"

extern void kickbot(const char *name);

static void qa_b30_traces()
{
    if(!halocollisionactive() || halocinematic() || player1->state != CS_ALIVE)
    {
        conoutf("B30_TRACE_QA unavailable");
        return;
    }

    const vec upper(242, 74, 5), lower(242, 74, -37), sameRoom(242, 79, -37);
    vec down = lower, up = upper, clear = sameRoom;
    const bool weaponDown = intersectgeometry(upper, down);
    const bool weaponUp = intersectgeometry(lower, up);
    const bool weaponClear = !intersectgeometry(lower, clear);
    const bool botDown = !IsVisible(upper, lower, nullptr);
    const bool botUp = !IsVisible(lower, upper, nullptr);
    const bool botClear = IsVisible(lower, sameRoom, nullptr);
    conoutf("B30_TRACE_QA weaponDown=%d z=%.3f weaponUp=%d z=%.3f weaponClear=%d botDown=%d botUp=%d botClear=%d",
            weaponDown, down.z, weaponUp, up.z, weaponClear, botDown, botUp, botClear);

    botent *target = BotManager.CreateBot("CLA", "bad", "b30_trace_target");
    if(!target || !target->pBot) return;
    target->state = CS_ALIVE;
    target->health = 100;
    target->armour = 0;
    target->eyeheight = 2.6f;
    target->radius = 0.9f;
    target->yaw = target->pitch = 0;
    target->head = vec(-1, 0, 0);
    target->o = lower;
    const vec oldPosition = player1->o, oldVelocity = player1->vel;
    const float oldPitchVelocity = player1->pitchvel;
    const int oldLastAction = player1->lastaction;
    weapon *oldWeapon = player1->weaponsel;
    weapon *pistol = player1->weapons[GUN_PISTOL];
    player1->weaponsel = pistol;
    pistol->mag = 12;
    pistol->info.spread = 1;

    auto fire = [&](const vec &origin)
    {
        player1->o = origin;
        vec aim = vec(target->o).add(vec(0, 0, -0.7f));
        const vec muzzle = vec(origin).add(vec(0, 0, -weapon::weaponbeloweye));
        const bool intersectsTarget = intersect(target, muzzle, aim) != 0;
        pistol->gunwait = 0;
        player1->lastaction = 0;
        player1->attacking = true;
        const bool fired = pistol->attack(aim);
        conoutf("B30_TRACE_QA originZ=%.3f targetIntersects=%d fired=%d targetHealth=%d ammo=%d",
                origin.z, intersectsTarget, fired, target->health, pistol->mag);
    };

    fire(upper);
    fire(sameRoom);
    player1->attacking = false;
    player1->weaponsel = oldWeapon;
    player1->lastaction = oldLastAction;
    player1->o = oldPosition;
    player1->vel = oldVelocity;
    player1->pitchvel = oldPitchVelocity;
    player1->resetinterp();
    kickbot(target->name);
    fflush(stdout);
}
COMMAND(qa_b30_traces, "");
