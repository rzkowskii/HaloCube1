// Halo-style actor brains for campaign bots. AssaultCube's CBot stays the body:
// it turns towards targetyaw/targetpitch, walks with move/strafe and follows
// flood waypoints. This file decides where to stand, whom to fight and when to
// fire, from the retail actor definitions and the encounter data the scenario
// passes in.

#include "cube.h"
#include "bot/bot.h"
#include "halo.h"
#include "halo_ai.h"
#include "halo_script.h"
#include "halo_weapons.h"
#include <algorithm>
#include <sstream>

namespace
{
    constexpr int formatVersion = 1;
    std::vector<HaloActorDef> definitions;
    bool attempted = false, loaded = false;

    enum Awareness { UNAWARE, ALERT, COMBAT };
    enum Morale { STEADY, PANICKED, BERSERK };
    // Retail actr types used for leader and swarm rules.
    enum { TYPE_ELITE = 0, TYPE_JACKAL = 1, TYPE_GRUNT = 2, TYPE_HUNTER = 3, TYPE_MARINE = 7, TYPE_INFECTION = 10 };
    // Retail squad states (none, sleep, alert, the move states, guard, search, flee).
    enum { STATE_SLEEP = 1, STATE_MOVE_FIRST = 3, STATE_MOVE_LAST = 7, STATE_GUARD = 8, STATE_GUARD_AT = 9,
           STATE_SEARCH = 10, STATE_FLEE = 11 };
    // Command-list atoms the brain runs; the rest are skipped.
    enum { ATOM_PAUSE = 0, ATOM_GO_TO = 1, ATOM_GO_TO_FACE = 2, ATOM_LOOK = 4, ATOM_CROUCH = 6, ATOM_SCRIPT = 12,
           ATOM_WAIT = 19, ATOM_LOOP = 20, ATOM_DIE = 21, ATOM_MOVE_IMMEDIATE = 22, ATOM_LOOK_PLAYER = 24,
           ATOM_SET_RADIUS = 26, ATOM_TELEPORT = 27 };
    enum { COMMAND_ALLOW_INITIATIVE = 1, COMMAND_ALLOW_TARGETING = 2 };

    struct Actor
    {
        botent *bot = nullptr;
        const HaloActorDef *def = nullptr;
        int team = HALO_TEAM_COVENANT, flags = 0, state = 0, posture = HALO_ATTACKING;
        Awareness awareness = UNAWARE;
        Morale morale = STEADY;
        int moraleUntil = 0, reactUntil = 0;
        playerent *target = nullptr;
        vec lastKnown;
        int lastSeen = 0, heardAt = 0;
        const std::vector<HaloFiringPosition> *positions = nullptr;
        unsigned masks[7] = {};
        vec destination;
        bool hasDestination = false;
        int positionUntil = 0, nextWander = 0;
        int burstUntil = 0, nextBurst = 0, nextGrenade = 0;
        bool hiding = false, berserkTried = false;
        int startHealth = 0;
        const HaloCommandProgram *program = nullptr;
        size_t cursor = 0;
        int commandUntil = 0, commandStatus = 0;
        float radius = 0;
        waypoint_s *goal = nullptr;
        int thinks = 0, shots = 0, destinationGroup = -1;
        float homeYaw = 0;
        int nextGlance = 0;
        int nextLeap = 0, leaps = 0;
        const playerent *follow = nullptr;
        float followDistance = 0;
    };
    std::vector<Actor> actors;
    std::vector<const playerent *> preferred;
    bool grenadesAllowed = true;
    struct Noise { const playerent *source; vec at; int millis; };
    std::vector<Noise> noises;

    float scale() { return haloscenarioscale(); }
    float frand(float a, float b) { return a + rndscale(max(0.0f, b - a)); }
    int millisFor(float seconds) { return int(seconds * 1000); }

    Actor *find(const playerent *p)
    {
        if(!p) return nullptr;
        for(Actor &a : actors) if(a.bot == p) return &a;
        return nullptr;
    }

    bool parse(const char *text)
    {
        std::istringstream lines(text);
        std::string line;
        HaloActorDef *d = nullptr;
        bool versioned = false;
        while(std::getline(lines, line))
        {
            if(line.empty() || line[0] == '#') continue;
            std::istringstream in(line);
            std::string key;
            in >> key;
            bool ok = true;
            if(key == "format") { int v = 0; in >> v; ok = versioned = v == formatVersion; }
            else if(key == "actor") { definitions.emplace_back(); d = &definitions.back(); in >> d->index; ok = d->index == int(definitions.size()) - 1; }
            else if(!d) ok = false;
            else if(key == "path") std::getline(in >> std::ws, d->path);
            else if(key == "family") ok = bool(in >> d->family >> d->kind >> d->type >> d->actrFlags >> d->actvFlags);
            else if(key == "weapon") ok = bool(in >> d->weapon);
            else if(key == "vision") ok = bool(in >> d->visionDistance >> d->visionCentral >> d->visionMax >> d->peripheralAngle >> d->peripheralDistance >> d->hearing);
            else if(key == "fire") ok = bool(in >> d->maxRange >> d->rate >> d->error >> d->firstDelayMin >> d->firstDelayMax >> d->combatMin >> d->combatMax >> d->lead >> d->damageScale);
            else if(key == "burst") ok = bool(in >> d->burstMin >> d->burstMax >> d->separationMin >> d->separationMax);
            else if(key == "grenade") ok = bool(in >> d->grenadeType >> d->grenadeChance >> d->grenadeRangeMin >> d->grenadeRangeMax >> d->grenadeMin >> d->grenadeMax);
            else if(key == "vitality") ok = bool(in >> d->body >> d->shield);
            else if(key == "defense") ok = bool(in >> d->hideShield >> d->emergeAttack >> d->emergePursue);
            else if(key == "morale") ok = bool(in >> d->casualtyTrigger >> d->friendKilled >> d->leaderKilled);
            else if(key == "berserk") ok = bool(in >> d->berserkAmount >> d->berserkThreshold >> d->meleeRange);
            else if(key == "positions") ok = bool(in >> d->guardMin >> d->guardMax >> d->positionMin >> d->positionMax);
            else if(key == "end") d = nullptr;
            if(!ok)
            {
                conoutf("\f3Halo actors: invalid line in packages/halo/actors.txt: %s", line.c_str());
                definitions.clear();
                return false;
            }
        }
        return versioned && !definitions.empty();
    }

    // --- perception -------------------------------------------------------

    vec eyeOf(const playerent *p) { return p->o; }
    vec centreOf(const playerent *p) { return vec(p->o).sub(vec(0, 0, p->eyeheight * 0.4f)); }

    bool alive(const playerent *p) { return p && p->state == CS_ALIVE; }

    bool sees(const Actor &a, CBot *body, const playerent *t)
    {
        if(a.flags & HALO_AI_BLIND) return false;
        const Actor *other = find(t);
        if(other && (other->flags & HALO_AI_DISREGARD)) return false;
        if(a.flags & HALO_AI_MAGIC_SIGHT) return true;
        const HaloActorDef &d = *a.def;
        vec delta = vec(centreOf(t)).sub(eyeOf(a.bot));
        const float distance = delta.magnitude();
        const float s = scale();
        const float yaw = a.bot->yaw * RAD;
        const vec facing(sinf(yaw), -cosf(yaw), 0);
        const float angle = acosf(clamp(vec(delta.x, delta.y, 0).normalize().dot(facing), -1.0f, 1.0f));
        const bool inCone = distance <= d.visionDistance * s && angle <= d.visionMax * 0.5f + 0.3f;
        const bool inPeriphery = distance <= d.peripheralDistance * s && angle <= d.peripheralAngle * 0.5f;
        // Actors already fighting keep tracking a target they know about.
        const bool tracking = a.awareness == COMBAT && a.target == t && distance <= d.visionDistance * s * 1.5f;
        if(!inCone && !inPeriphery && !tracking) return false;
        return body->IsVisible(centreOf(t));
    }

    bool hears(const Actor &a, vec &where)
    {
        if(a.flags & HALO_AI_DEAF) return false;
        const float range = a.def->hearing * scale();
        for(const Noise &n : noises)
        {
            if(lastmillis - n.millis > 600 || !haloactorhostile(a.bot, n.source)) continue;
            if(n.at.dist(a.bot->o) <= range) { where = n.at; return true; }
        }
        return false;
    }

    // Everyone who takes part in fights: the player and every actor bot.
    template<class F> void eachCombatant(F f)
    {
        if(alive(player1)) f(player1);
        for(Actor &a : actors) if(alive(a.bot)) f(a.bot);
    }

    void perceive(Actor &a, CBot *body)
    {
        playerent *best = nullptr;
        float bestDistance = 1e9f;
        eachCombatant([&](playerent *p)
        {
            if(p == a.bot || !haloactorhostile(a.bot, p) || !sees(a, body, p)) return;
            float d = p->o.dist(a.bot->o) * (p == a.target ? 0.8f : 1.0f);
            if(std::find(preferred.begin(), preferred.end(), p) != preferred.end()) d *= 0.5f;
            if(d < bestDistance) { bestDistance = d; best = p; }
        });
        if(best)
        {
            if(a.awareness != COMBAT || a.target != best)
            {
                // Reaction time before the first burst at a newly noticed target.
                if(a.awareness != COMBAT) a.reactUntil = lastmillis + millisFor(frand(a.def->firstDelayMin, a.def->firstDelayMax));
                a.target = best;
                a.positionUntil = 0;
            }
            a.awareness = COMBAT;
            a.lastKnown = best->o;
            a.lastSeen = lastmillis;
            // Nearby squadmates are put on alert towards the same target.
            const float shout = 15.0f * scale();
            for(Actor &friendActor : actors)
                if(&friendActor != &a && friendActor.team == a.team && friendActor.awareness == UNAWARE && alive(friendActor.bot) &&
                   friendActor.bot->o.dist(a.bot->o) <= shout)
                {
                    friendActor.awareness = ALERT;
                    friendActor.lastKnown = best->o;
                    friendActor.heardAt = lastmillis;
                    friendActor.positionUntil = 0;
                }
            return;
        }
        vec heard;
        if(hears(a, heard))
        {
            if(a.awareness == UNAWARE) { a.awareness = ALERT; a.positionUntil = 0; }
            a.lastKnown = heard;
            a.heardAt = lastmillis;
        }
        if(a.awareness == COMBAT && lastmillis - a.lastSeen > 4000) { a.awareness = ALERT; a.positionUntil = 0; }
        if(a.target && !alive(a.target)) { a.target = nullptr; a.awareness = ALERT; }
        if(a.awareness == ALERT && lastmillis - max(a.lastSeen, a.heardAt) > 20000) a.awareness = UNAWARE;
    }

    // --- movement ---------------------------------------------------------

    // Walk towards a point while looking elsewhere, splitting the direction
    // into AssaultCube's forward (sin yaw, -cos yaw) and strafe (-cos yaw, -sin yaw).
    void steer(botent *e, const vec &to)
    {
        vec dir = vec(to).sub(e->o);
        dir.z = 0;
        if(dir.squaredlen() < 0.01f) { e->move = e->strafe = 0; return; }
        dir.normalize();
        const float yaw = e->yaw * RAD;
        const float forward = dir.x * sinf(yaw) - dir.y * cosf(yaw);
        const float side = -dir.x * cosf(yaw) - dir.y * sinf(yaw);
        e->move = forward > 0.38f ? 1 : forward < -0.38f ? -1 : 0;
        e->strafe = side > 0.38f ? 1 : side < -0.38f ? -1 : 0;
    }

    // Returns true once the actor stands within `radius` of the point.
    bool walkTo(Actor &a, CBot *body, const vec &to, float radius, bool faceTravel)
    {
        botent *e = a.bot;
        if(vec(to).sub(e->o).magnitudexy() <= radius && fabsf(to.z - (e->o.z - e->eyeheight)) < 12.0f)
        {
            e->move = e->strafe = 0;
            a.goal = nullptr;
            return true;
        }
        if(body->IsVisible(vec(to).add(vec(0, 0, e->eyeheight * 0.5f))))
        {
            if(faceTravel) body->AimToVec(vec(to).add(vec(0, 0, e->eyeheight)));
            steer(e, to);
            body->CheckStuck();
            return false;
        }
        // Out of sight: follow the flood waypoints to the waypoint nearest the point.
        if(!a.goal || a.goal->pNode->v_origin.dist(to) > 24.0f)
        {
            a.goal = body->GetNearestWaypoint(to, 24.0f);
            if(a.goal)
            {
                body->ResetWaypointVars();
                body->SetCurrentGoalWaypoint(a.goal);
            }
        }
        if(a.goal && body->HeadToGoal()) return false;
        a.goal = nullptr;
        body->AimToVec(vec(to).add(vec(0, 0, e->eyeheight)));
        steer(e, to);
        body->CheckStuck();
        return false;
    }

    int maskFor(const Actor &a)
    {
        if(a.hiding || a.morale == PANICKED) return 3;                  // defending
        if(a.awareness == UNAWARE) return a.posture == HALO_DEFENDING ? 5 : 2;   // guard positions
        if(a.awareness == ALERT) return a.posture == HALO_DEFENDING ? 4 : 1;     // search positions
        return a.posture == HALO_DEFENDING ? 3 : a.posture == HALO_PURSUING ? 6 : 0;
    }

    // Choose a combat position from the squad's groups for its current posture.
    bool choosePosition(Actor &a)
    {
        if(!a.positions || a.positions->empty()) return false;
        const unsigned mask = a.masks[maskFor(a)];
        const float s = scale();
        const float wantMin = a.def->combatMin * s, wantMax = max(a.def->combatMax, a.def->combatMin + 1) * s;
        const bool fleeing = a.morale == PANICKED || (a.flags & HALO_AI_FLEE);
        float best = 1e9f;
        const HaloFiringPosition *pick = nullptr;
        const int count = int(a.positions->size());
        const int start = rnd(count), samples = min(count, 48);
        loopi(samples)
        {
            const HaloFiringPosition &p = (*a.positions)[(start + i * 7) % count];
            if(mask && !(mask & (1u << p.group))) continue;
            float score = p.point.dist(a.bot->o) * 0.25f + rndscale(6.0f);
            if(a.target || a.awareness != UNAWARE)
            {
                const float d = p.point.dist(a.lastKnown);
                if(fleeing) score -= d * 2;
                else if(a.hiding) score += (::IsVisible(vec(p.point).add(vec(0, 0, a.bot->eyeheight)), a.lastKnown) ? 60 : 0) - d * 0.2f;
                else score += d < wantMin ? (wantMin - d) * 2 : d > wantMax ? (d - wantMax) * 1.5f : 0;
            }
            if(score < best) { best = score; pick = &p; }
        }
        if(!pick) return false;
        a.destination = pick->point;
        a.destinationGroup = pick->group;
        a.hasDestination = true;
        const float hold = a.awareness == UNAWARE ? frand(a.def->guardMin, a.def->guardMax) : frand(a.def->positionMin, a.def->positionMax);
        a.positionUntil = lastmillis + millisFor(max(hold, 2.0f));
        return true;
    }

    // Without authored positions, keep the variant's combat range from the target.
    void keepRange(Actor &a, CBot *body)
    {
        const float s = scale();
        const float d = a.bot->o.dist(a.lastKnown);
        vec away = vec(a.bot->o).sub(a.lastKnown);
        away.z = 0;
        if(away.iszero()) away = vec(1, 0, 0);
        away.normalize();
        if(a.morale == PANICKED || a.hiding) steer(a.bot, vec(a.bot->o).add(vec(away).mul(20)));
        else if(d > a.def->combatMax * s) walkTo(a, body, a.lastKnown, a.def->combatMax * s, false);
        else if(d < a.def->combatMin * s) steer(a.bot, vec(a.bot->o).add(vec(away).mul(20)));
        else
        {
            // Sidestep while in range, changing direction now and then.
            if(lastmillis >= a.nextWander) { a.nextWander = lastmillis + 700 + rnd(1300); a.bot->strafe = rnd(3) - 1; }
            a.bot->move = 0;
        }
    }

    // --- firing -----------------------------------------------------------

    void fire(Actor &a, CBot *body)
    {
        botent *e = a.bot;
        playerent *t = a.target;
        e->attacking = false;
        // `botsshoot 0` holds every bot's fire, as it does for AssaultCube's own AI.
        if(!BotManager.BotsShoot()) return;
        if(!t || !alive(t) || lastmillis < a.reactUntil || a.morale == PANICKED || a.awareness != COMBAT) return;
        const HaloActorDef &d = *a.def;
        const float s = scale();
        const float distance = e->o.dist(t->o);
        const bool melee = e->weaponsel->type == GUN_KNIFE;
        if(!melee && d.maxRange > 0 && distance > d.maxRange * s * 1.25f) return;
        // Lead moving targets by the variant's lead fraction of the travel time.
        const HaloWeaponDef *w = haloweapon(d.weapon);
        const HaloTriggerDef *trigger = w ? w->trigger() : nullptr;
        const float speed = trigger && trigger->projectile.valid ? max(1.0f, trigger->projectile.speed * s) : 1000.0f;
        vec aim = centreOf(t);
        aim.add(vec(t->vel).mul(d.lead * distance / speed));
        if(d.error > 0)
        {
            const float spread = tanf(d.error) * distance;
            aim.add(vec(frand(-spread, spread), frand(-spread, spread), frand(-spread, spread) * 0.5f));
        }
        body->AimToVec(aim);
        if(lastmillis >= a.nextBurst && lastmillis >= a.burstUntil)
        {
            a.burstUntil = lastmillis + millisFor(frand(d.burstMin, d.burstMax));
            a.nextBurst = a.burstUntil + millisFor(frand(d.separationMin, d.separationMax));
        }
        if(lastmillis >= a.burstUntil && !melee) return;
        // Fire only once roughly facing the target.
        const float yawError = fabsf(e->targetyaw - e->yaw);
        if(min(yawError, 360 - yawError) > 25) return;
        if(melee && distance > max(d.meleeRange * s, 5.0f)) return;
        e->attacking = true;
        shoot(e, aim);
    }

    void throwGrenade(Actor &a, CBot *body)
    {
        const HaloActorDef &d = *a.def;
        botent *e = a.bot;
        if(!grenadesAllowed || !BotManager.BotsShoot() || !a.target || lastmillis < a.nextGrenade || e->mag[GUN_GRENADE] <= 0 || d.grenadeChance <= 0) return;
        a.nextGrenade = lastmillis + 4000 + rnd(4000);
        const float distance = e->o.dist(a.target->o), s = scale();
        if(distance < max(d.grenadeRangeMin, 2.0f) * s || distance > max(d.grenadeRangeMax, 8.0f) * s) return;
        if(rndscale(1.0f) > d.grenadeChance) return;
        vec direction = vec(a.target->o).sub(e->o);
        const float flight = clamp(direction.magnitude() / 60.0f, 0.4f, 1.6f);
        direction.normalize().mul(1.6f);
        direction.z += 0.25f * flight;
        grenadeent *g = new grenadeent(e);
        bounceents.add(g);
        g->activate(e->o, a.target->o);
        g->moveoutsidebbox(direction, e);
        g->_throw(g->o, direction);
        --e->mag[GUN_GRENADE];
    }

    // --- morale and defence -----------------------------------------------

    void updateMorale(Actor &a)
    {
        if(a.morale != STEADY && lastmillis >= a.moraleUntil) a.morale = STEADY;
        const HaloActorDef &d = *a.def;
        const float shield = haloenemyshieldfraction(a.bot);
        if(d.shield > 0 && d.hideShield > 0)
        {
            if(!a.hiding && shield < d.hideShield && a.awareness == COMBAT) { a.hiding = true; a.positionUntil = 0; }
            else if(a.hiding && shield >= max(d.emergeAttack, d.hideShield)) { a.hiding = false; a.positionUntil = 0; }
        }
        // Heavy damage can tip shielded fighters into a berserk charge.
        if(!a.berserkTried && d.berserkThreshold > 0 && a.startHealth > 0 &&
           a.bot->health < a.startHealth * (1 - d.berserkThreshold) && shield <= 0)
        {
            a.berserkTried = true;
            if(rndscale(1.0f) < d.berserkAmount) { a.morale = BERSERK; a.moraleUntil = lastmillis + 8000; a.hiding = false; }
        }
    }

    // --- command lists ----------------------------------------------------

    // Runs the current atom. Returns true while the list controls movement.
    bool runCommands(Actor &a, CBot *body)
    {
        if(!a.program || a.commandStatus != 1) return false;
        const HaloCommandProgram &p = *a.program;
        botent *e = a.bot;
        const float s = scale();
        for(int guard = 0; guard < 8; ++guard)
        {
            if(a.cursor >= p.commands.size()) { a.commandStatus = 2; e->move = e->strafe = 0; return false; }
            const HaloCommand &c = p.commands[a.cursor];
            const vec *point = c.point >= 0 && c.point < int(p.points.size()) ? &p.points[c.point] : nullptr;
            switch(c.type)
            {
                case ATOM_PAUSE: case ATOM_WAIT:
                    if(!a.commandUntil) a.commandUntil = lastmillis + millisFor(c.first);
                    if(lastmillis < a.commandUntil) { e->move = e->strafe = 0; return true; }
                    a.commandUntil = 0;
                    break;
                case ATOM_GO_TO: case ATOM_GO_TO_FACE: case ATOM_MOVE_IMMEDIATE:
                    if(point && !walkTo(a, body, *point, max(a.radius * s, 2.5f), true)) return true;
                    if(c.type == ATOM_GO_TO_FACE && c.otherPoint >= 0 && c.otherPoint < int(p.points.size()))
                        body->AimToVec(vec(p.points[c.otherPoint]).add(vec(0, 0, e->eyeheight)));
                    break;
                case ATOM_LOOK: if(point) body->AimToVec(vec(*point).add(vec(0, 0, e->eyeheight))); break;
                case ATOM_LOOK_PLAYER: body->AimToVec(player1->o); break;
                case ATOM_CROUCH: e->trycrouch = c.first > 0; break;
                case ATOM_SET_RADIUS: a.radius = c.first; break;
                case ATOM_LOOP: a.cursor = 0; continue;
                case ATOM_TELEPORT: if(point) { e->o = vec(*point).add(vec(0, 0, e->eyeheight)); entinmap(e); e->resetinterp(); } break;
                case ATOM_DIE: dodamage(INT_MAX, e, e, GUN_KNIFE); return true;
                case ATOM_SCRIPT: if(c.script >= 0) haloscenariowakescript(c.script); break;
                default: break;
            }
            ++a.cursor;
        }
        return true;
    }

    // Infection forms lunge at a target a few strides away: a hop with a burst of
    // forward speed, then a cooldown while they scuttle.
    void leap(Actor &a)
    {
        botent *e = a.bot;
        const playerent *t = a.target;
        if(a.def->type != TYPE_INFECTION || !t || !alive(t) || lastmillis < a.nextLeap || !e->onfloor) return;
        const float s = scale(), distance = e->o.dist(t->o);
        if(distance < 2.5f * s || distance > 7.0f * s || !::IsVisible(e->o, centreOf(t), NULL)) return;
        vec dir = vec(t->o).sub(e->o);
        dir.z = 0;
        if(dir.iszero()) return;
        dir.normalize();
        e->vel.x = dir.x * 1.1f;
        e->vel.y = dir.y * 1.1f;
        e->jumpnext = true;
        a.nextLeap = lastmillis + 1500 + rnd(1500);
        ++a.leaps;
    }

    // Infection forms head for a fallen marine or Elite when it is closer than their target.
    bool seekCorpse(Actor &a, CBot *body)
    {
        if(a.def->type != TYPE_INFECTION) return false;
        const float s = scale();
        vec corpse;
        if(!halonearestcorpse(a.bot->o, 40.0f * s, corpse)) return false;
        if(a.awareness == COMBAT && a.target && a.bot->o.dist(a.target->o) < a.bot->o.dist(corpse)) return false;
        walkTo(a, body, corpse, 1.5f, true);
        return true;
    }

    void think(Actor &a, CBot *body)
    {
        ++a.thinks;
        botent *e = a.bot;
        e->move = e->strafe = 0;
        e->attacking = false;
        if(a.flags & HALO_AI_BRAINDEAD) return;
        perceive(a, body);
        updateMorale(a);

        const bool commanded = runCommands(a, body);
        const bool initiative = !a.program || a.commandStatus != 1 || (a.program->flags & COMMAND_ALLOW_INITIATIVE);
        if(commanded && !initiative)
        {
            if(a.program->flags & COMMAND_ALLOW_TARGETING) fire(a, body);
            return;
        }

        const float s = scale();
        if(seekCorpse(a, body)) return;
        if(a.awareness == COMBAT && a.target)
        {
            if(a.morale == BERSERK || (a.def->actrFlags & 1) || e->weaponsel->type == GUN_KNIFE)
            {
                // Charge into melee range.
                walkTo(a, body, a.target->o, max(a.def->meleeRange * s, 3.0f), false);
                body->AimToVec(centreOf(a.target));
            }
            else if(lastmillis >= a.positionUntil || !a.hasDestination)
            {
                if(!choosePosition(a)) { a.hasDestination = false; keepRange(a, body); }
            }
            else if(walkTo(a, body, a.destination, 3.0f, false))
            {
                // At the position: sidestep a little as the retail actors do.
                if(lastmillis >= a.nextWander) { a.nextWander = lastmillis + 900 + rnd(1500); e->strafe = rnd(3) - 1; }
            }
            if(!a.hasDestination && a.morale != BERSERK) keepRange(a, body);
            leap(a);
            fire(a, body);
            if(!(a.flags & HALO_AI_PLAYFIGHT)) throwGrenade(a, body);
            return;
        }

        if(alive(a.follow) && a.awareness != ALERT)
        {
            // Squads told to follow a unit trail it at their follow distance.
            const float want = max(a.followDistance, 4.0f) * s;
            if(e->o.dist(a.follow->o) > want * 1.5f) walkTo(a, body, a.follow->o, want, true);
            return;
        }
        if((a.flags & HALO_AI_FOLLOW_PLAYER) || (a.team == HALO_TEAM_HUMAN && !a.positions))
        {
            // Stay near the player when following and nothing is to be fought.
            if(alive(player1) && e->o.dist(player1->o) > 9.0f * s) walkTo(a, body, player1->o, 6.0f * s, true);
            return;
        }

        if(a.awareness == ALERT)
        {
            // Search towards where the target was last seen or heard.
            if(lastmillis >= a.positionUntil || !a.hasDestination)
            {
                if(!choosePosition(a)) { a.destination = a.lastKnown; a.hasDestination = true; a.positionUntil = lastmillis + 6000; }
            }
            // Searching actors stop at their combat range from the last known position.
            const float stop = a.destination.dist(a.lastKnown) < 1.0f ? max(a.def->combatMin * s, 3.0f) : 3.0f;
            if(walkTo(a, body, a.destination, stop, true)) body->AimToVec(vec(a.lastKnown).add(vec(0, 0, e->eyeheight)));
            return;
        }

        // Unaware actors glance around from where they first faced, as the retail
        // idle looking does, so a player in the open is eventually noticed.
        if(a.state != STATE_SLEEP && lastmillis >= a.nextGlance)
        {
            a.nextGlance = lastmillis + 2500 + rnd(3500);
            e->targetyaw = a.homeYaw + frand(-120, 120);
            e->targetpitch = 0;
        }
        // Sleepers and guards hold still; the move states wander between guard positions.
        if(a.state == STATE_SLEEP || a.state == STATE_GUARD || a.state == STATE_GUARD_AT || a.state == 0) return;
        if(a.state >= STATE_MOVE_FIRST && a.state <= STATE_MOVE_LAST) {}
        else if(a.state != STATE_SEARCH && a.state != STATE_FLEE) return;
        if(lastmillis >= a.positionUntil || !a.hasDestination) choosePosition(a);
        if(a.hasDestination) walkTo(a, body, a.destination, 3.0f, true);
    }
}

bool haloactorsload()
{
    if(attempted) return loaded;
    attempted = true;
    int size = 0;
    char *text = loadfile(path("packages/halo/actors.txt", true), &size);
    if(!text)
    {
        conoutf("\f3Halo actors: packages/halo/actors.txt is missing; run tools/halo_actors.py");
        return false;
    }
    loaded = parse(text);
    delete[] text;
    if(loaded) conoutf("Halo actors: loaded %d variants", int(definitions.size()));
    return loaded;
}

int haloactordefcount() { haloactorsload(); return int(definitions.size()); }

const HaloActorDef *haloactordef(int index)
{
    haloactorsload();
    return index >= 0 && index < int(definitions.size()) ? &definitions[index] : nullptr;
}

const HaloActorDef *haloactordefault(int kind, int weaponIndex)
{
    haloactorsload();
    const HaloActorDef *sameKind = nullptr;
    for(const HaloActorDef &d : definitions)
    {
        if(d.kind != kind) continue;
        // Prefer the minor rank, which the campaign places most often.
        const bool minor = d.path.find("minor") != std::string::npos || d.path.find("major") == std::string::npos;
        if(d.weapon == weaponIndex && minor) return &d;
        if(d.weapon == weaponIndex && !sameKind) sameKind = &d;
    }
    if(sameKind) return sameKind;
    for(const HaloActorDef &d : definitions) if(d.kind == kind) return &d;
    return nullptr;
}

void haloactorattach(botent *bot, const HaloActorDef *def, int team)
{
    if(!bot || !def) return;
    haloactordetach(bot);
    Actor a;
    a.bot = bot;
    a.def = def;
    a.team = team;
    a.lastKnown = bot->o;
    a.startHealth = bot->health;
    a.homeYaw = bot->yaw;
    bot->mag[GUN_GRENADE] = def->grenadeMax > 0 ? def->grenadeMin + rnd(def->grenadeMax - def->grenadeMin + 1) : 0;
    actors.push_back(a);
}

void haloactordetach(const playerent *bot)
{
    for(size_t i = 0; i < actors.size(); ++i) if(actors[i].bot == bot) { actors.erase(actors.begin() + i); break; }
    for(Actor &a : actors)
    {
        if(a.target == bot) { a.target = nullptr; if(a.awareness == COMBAT) a.awareness = ALERT; }
        if(a.follow == bot) a.follow = nullptr;
    }
    preferred.erase(std::remove(preferred.begin(), preferred.end(), bot), preferred.end());
}

void haloactorsclear() { actors.clear(); noises.clear(); }
bool haloisactor(const playerent *p) { return find(p) != nullptr; }

int haloactorteam(const playerent *p)
{
    if(p == player1) return HALO_TEAM_HUMAN;
    const Actor *a = find(p);
    return a ? a->team : HALO_TEAM_COVENANT;
}

bool haloactorhostile(const playerent *a, const playerent *b)
{
    if(!a || !b || a == b) return false;
    return haloactorteam(a) != haloactorteam(b);
}

bool haloactorthink(CBot *body)
{
    if(!haloactive() || !body) return false;
    Actor *a = find(body->m_pMyEnt);
    if(!a) return false;
    think(*a, body);
    return true;
}

void haloactorsetpositions(const playerent *bot, const std::vector<HaloFiringPosition> *positions, const unsigned masks[7])
{
    Actor *a = find(bot);
    if(!a) return;
    a->positions = positions;
    loopi(7) a->masks[i] = masks ? masks[i] : 0;
    a->positionUntil = 0;
    a->hasDestination = false;
}

void haloactorsetposture(const playerent *bot, int posture)
{
    if(Actor *a = find(bot)) if(a->posture != posture && !(posture == HALO_PURSUING && (a->flags & HALO_AI_NO_MANEUVER))) { a->posture = posture; a->positionUntil = 0; }
}

int haloactorposture(const playerent *bot) { const Actor *a = find(bot); return a ? a->posture : HALO_ATTACKING; }
void haloactorsetstate(const playerent *bot, int state) { if(Actor *a = find(bot)) a->state = state; }

void haloactorsetflag(const playerent *bot, int flag, bool on)
{
    Actor *a = find(bot);
    if(!a) return;
    a->flags = on ? a->flags | flag : a->flags & ~flag;
    if(on && (flag & (HALO_AI_MAGIC_SIGHT | HALO_AI_FORCE_ACTIVE)) && a->awareness == UNAWARE)
    {
        a->awareness = ALERT;
        a->lastKnown = player1->o;
    }
}

void haloactorcommand(const playerent *bot, const HaloCommandProgram *program)
{
    Actor *a = find(bot);
    if(!a) return;
    a->program = program;
    a->cursor = 0;
    a->commandUntil = 0;
    a->commandStatus = program ? 1 : 0;
    a->goal = nullptr;
}

int haloactorcommandstatus(const playerent *bot) { const Actor *a = find(bot); return a ? a->commandStatus : 0; }

void haloactorcommandadvance(const playerent *bot)
{
    Actor *a = find(bot);
    if(!a || a->commandStatus != 1) return;
    ++a->cursor;
    a->commandUntil = 0;
}

int haloactorstatus(const playerent *bot)
{
    const Actor *a = find(bot);
    if(!a || !alive(a->bot)) return 0;
    if(a->awareness == COMBAT) return 6;
    if(a->awareness == ALERT) return 4;
    return a->state == STATE_SLEEP ? 1 : 2;
}

playerent *haloactortarget(const playerent *bot) { const Actor *a = find(bot); return a ? a->target : nullptr; }

float haloactorhealthfraction(const playerent *bot)
{
    if(!bot) return 0;
    if(bot == player1) return bot->health / 75.0f;
    const Actor *a = find(bot);
    return a && a->startHealth > 0 ? float(max(0, bot->health)) / a->startHealth : 0;
}

int haloactorflags(const playerent *bot) { const Actor *a = find(bot); return a ? a->flags : 0; }
void haloaisetgrenades(bool on) { grenadesAllowed = on; }
bool haloaigrenades() { return grenadesAllowed; }

void haloactorberserk(const playerent *bot, bool on)
{
    Actor *a = find(bot);
    if(!a) return;
    if(on) { a->morale = BERSERK; a->moraleUntil = INT_MAX; a->hiding = false; a->berserkTried = true; }
    else if(a->morale == BERSERK) a->morale = STEADY;
    a->positionUntil = 0;
}

void haloactorfollow(const playerent *bot, const playerent *target)
{
    if(Actor *a = find(bot)) a->follow = target;
}
void haloactorfollowdistance(const playerent *bot, float distance) { if(Actor *a = find(bot)) a->followDistance = distance; }
const playerent *haloactorfollowing(const playerent *bot) { const Actor *a = find(bot); return a ? a->follow : nullptr; }

void haloactorprefer(const playerent *target, bool on)
{
    if(!target) return;
    const bool present = std::find(preferred.begin(), preferred.end(), target) != preferred.end();
    if(on && !present) preferred.push_back(target);
    else if(!on && present) preferred.erase(std::remove(preferred.begin(), preferred.end(), target), preferred.end());
}

int haloactorstarthealth(const playerent *bot) { const Actor *a = find(bot); return a ? a->startHealth : 0; }

void haloactorrenew(playerent *bot)
{
    const Actor *a = find(bot);
    if(!a || !alive(bot)) return;
    haloenemyrenew(bot, a->startHealth, a->def->grenadeMax);
}

void haloactorsethealthfraction(playerent *bot, float fraction)
{
    Actor *a = find(bot);
    if(!a || a->startHealth <= 0 || !alive(bot)) return;
    bot->health = max(1, int(clamp(fraction, 0.0f, 1.0f) * a->startHealth + .5f));
}

void haloactornoise(const playerent *source)
{
    if(!source) return;
    if(Actor *a = find(source)) ++a->shots;
    for(Noise &n : noises) if(n.source == source) { n.at = source->o; n.millis = lastmillis; return; }
    noises.push_back(Noise{source, source->o, lastmillis});
}

void haloactordeath(const playerent *victim, const playerent *killer)
{
    const Actor *dead = find(victim);
    const int team = haloactorteam(victim);
    const bool leader = dead && dead->def->type == TYPE_ELITE;
    const float s = scale();
    for(Actor &a : actors)
    {
        if(a.bot == victim || !alive(a.bot) || a.team != team) continue;
        if(a.bot->o.dist(victim->o) > 15.0f * s) continue;
        // Seeing a friend or a leader fall can break an actor's nerve.
        const float chance = leader && a.def->type != TYPE_ELITE ? max(a.def->leaderKilled, a.def->friendKilled) : a.def->friendKilled;
        if(a.morale == STEADY && rndscale(1.0f) < chance * 0.5f)
        {
            a.morale = PANICKED;
            a.moraleUntil = lastmillis + 3000 + rnd(3000);
            a.positionUntil = 0;
        }
        if(killer && haloactorhostile(a.bot, killer) && a.awareness != COMBAT)
        {
            a.awareness = ALERT;
            a.lastKnown = killer->o;
        }
    }
    haloactordetach(victim);
}

playerent *haloactorhit(const playerent *owner, const vec &from, const vec &to, vec &contact)
{
    playerent *best = nullptr;
    float bestDistance = 1e9f;
    eachCombatant([&](playerent *p)
    {
        if(p == owner || !haloactorhostile(owner, p)) return;
        vec end;
        if(!intersect(p, from, to, &end)) return;
        const float d = end.dist(from);
        if(d < bestDistance) { bestDistance = d; best = p; contact = end; }
    });
    return best;
}

void haloactordump(const char *label)
{
    static const char *const awareness[] = {"unaware", "alert", "combat"}, *const morale[] = {"steady", "panicked", "berserk"};
    for(const Actor &a : actors)
    {
        const botent *e = a.bot;
        conoutf("ACTOR_QA %s cn=%d name=%s family=%s team=%d alive=%d awareness=%s morale=%s posture=%d hiding=%d status=%d "
                "target=%s pos=%.1f,%.1f,%.1f dest=%d group=%d mask=%u positions=%d command=%d health=%d shots=%d flags=%d leaps=%d",
                label, e->clientnum, e->name, a.def->family.c_str(), a.team, e->state == CS_ALIVE, awareness[a.awareness], morale[a.morale],
                a.posture, a.hiding, haloactorstatus(e), a.target ? (a.target == player1 ? "player" : a.target->name) : "-",
                e->o.x, e->o.y, e->o.z, a.hasDestination, a.destinationGroup, a.masks[maskFor(a)],
                a.positions ? int(a.positions->size()) : 0, a.commandStatus,
                e->health, a.shots, a.flags, a.leaps);
        conoutf("ACTOR_EXTRA %s cn=%d follow=%d grenades=%d grenades_on=%d shield=%.3f", label, e->clientnum, a.follow != nullptr, e->mag[GUN_GRENADE], grenadesAllowed, haloenemyshieldfraction(e));
        vec delta = vec(player1->o).sub(e->o);
        const float yaw = e->yaw * RAD;
        const float angle = acosf(clamp(vec(delta.x, delta.y, 0).normalize().dot(vec(sinf(yaw), -cosf(yaw), 0)), -1.0f, 1.0f)) / RAD;
        conoutf("ACTOR_VIEW %s cn=%d name=%s yaw=%.0f angle=%.0f distance=%.1f range=%.1f visible=%d thinks=%d", label, e->clientnum, e->name, e->yaw,
                angle, delta.magnitude(), a.def->visionDistance * scale(), ::IsVisible(e->o, centreOf(player1), NULL), a.thinks);
    }
    fflush(stdout);
}
