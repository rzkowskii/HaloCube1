// Halo-style actor brains for campaign bots. AssaultCube's CBot stays the body:
// it turns towards targetyaw/targetpitch, walks with move/strafe and follows
// flood waypoints. This file decides where to stand, whom to fight and when to
// fire, from the retail actor definitions and the encounter data the scenario
// passes in.

#include "cube.h"
#include "bot/bot.h"
#include "halo.h"
#include "halo_ai.h"
#include "halo_collision.h"
#include "halo_script.h"
#include "halo_vehicle.h"
#include "halo_weapons.h"
#include <algorithm>
#include <map>
#include <sstream>

extern int halo_damagetrace;

namespace
{
    // Format 2 adds the team, unitmelee and material lines; format 1 tables still load (their
    // variants take the team of their family and have no materials). Format 3 adds the `major`
    // line per variant and the globals' `difficulty` lines before the variants; format 1 and 2
    // tables still load (no major upgrades, and halodifficultyvalue answers its fallback).
    constexpr int formatVersion = 3;
    std::vector<HaloActorDef> definitions;
    float difficultyValues[HALO_DIFFICULTY_VALUES][4];
    bool difficultySeen[HALO_DIFFICULTY_VALUES] = {};
    bool attempted = false, loaded = false;

    enum Awareness { UNAWARE, ALERT, COMBAT };
    enum Morale { STEADY, PANICKED, BERSERK };
    // Retail actr types used for leader and swarm rules.
    enum { TYPE_ELITE = 0, TYPE_JACKAL = 1, TYPE_GRUNT = 2, TYPE_HUNTER = 3, TYPE_ENGINEER = 4, TYPE_MARINE = 7,
           TYPE_INFECTION = 10, TYPE_MONITOR = 12, TYPE_SENTINEL = 13 };
    // Retail actr flags used by the brain: always charge at enemies, always berserk in attacking
    // mode, flying, always charge in attacking mode.
    enum { ACTR_ALWAYS_CHARGE = 1 << 11, ACTR_BERSERK_ATTACKING = 1 << 19, ACTR_FLYING = 1 << 21, ACTR_CHARGE_ATTACKING = 1 << 24 };
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
        // The attack delay its weapon slot was given at spawn (spawnEnemy's campaignWeapon, just
        // before haloactorattach): a melee-only actor's swing cadence, which fire() scales by the
        // melee delay values on each swing (the slot's delay is rewritten there, so it is read once).
        int baseDelay = 0;
        bool hiding = false, berserkTried = false;
        int startHealth = 0;
        const HaloCommandProgram *program = nullptr;
        size_t cursor = 0;
        int commandUntil = 0, commandStatus = 0;
        // The loop atom the list last reached: Halo reports such a list as waiting for a
        // stimulus (ai_command_list_status 2) and ai_command_list_advance steps past the loop.
        int loopAt = -1;
        float radius = 0;
        waypoint_s *goal = nullptr;
        int thinks = 0, shots = 0, destinationGroup = -1;
        float homeYaw = 0;
        int nextGlance = 0;
        int nextLeap = 0, leaps = 0;
        const playerent *follow = nullptr;
        float followDistance = 0;
        // Hunters: the bond brother fought beside, and when the next smash may land.
        const playerent *partner = nullptr;
        int nextSmash = 0, smashes = 0;
        // Flyers (Sentinels, the Monitor, Engineers): the height held over the target or the
        // floor, in Halo world units, and when the strafe direction next changes.
        float hoverHeight = 2.5f;
        int nextStrafe = 0, strafeDir = 0;
        int floorAt = 0;
        float floorZ = 0, ceilingZ = 0;
        vec floorFrom;
        // ai_go_to_vehicle (haloactorgoto): walking to a vehicle seat's entry point.
        bool boarding = false;
        vec boardAt;
    };
    std::vector<Actor> actors;
    // Halo's per-encounter last_grenade_throw_time (../Halo3DS ai/encounters.h:77, set when any member
    // starts a throw, actions.c:3600-3607): lastmillis of the last throw, by the encounter index
    // haloscenariosquadof gives. Cleared with the actors (a section start, or a revert, where Halo's
    // checkpoint would restore it: the engine forgets it, judgement call).
    std::map<int, int> encounterThrows;

    // Allegiances between engine teams over pairs a < b (15 pairs for 6 teams): allied and
    // broken bits, the checkpoint's copy, and the allies the player has killed per pair.
    int pairBit(int a, int b)
    {
        if(a < 0 || b < 0 || a >= HALO_TEAM_COUNT || b >= HALO_TEAM_COUNT || a == b) return -1;
        if(a > b) std::swap(a, b);
        return a * (2 * HALO_TEAM_COUNT - a - 1) / 2 + (b - a - 1);
    }
    unsigned defaultAlliances() { return 1u << pairBit(HALO_TEAM_PLAYER, HALO_TEAM_HUMAN); }
    unsigned allianceBits = defaultAlliances(), brokenBits = 0, savedAllianceBits = defaultAlliances(), savedBrokenBits = 0;
    int betrayals[HALO_TEAM_COUNT] = {};
    // The player turns a breakable allegiance by killing this many of the allied team (Halo's
    // allies turn on a player who keeps killing them; the count is the engine's choice).
    const int betrayalKills = 2;
    int scriptDamage = 0;           // HaloScriptDamage scopes open
    int ownerlessDamage = 0;        // of those, the ones dealing damage nobody owns (haloareablast, no owner)
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

    // The coll lines actors.txt and player.txt share: `material`, `resistance` and `shieldtime`
    // (tools/halo_actors.py). Whether the line parsed.
    bool parseColl(const std::string &key, std::istringstream &in, HaloActorDef &d)
    {
        bool ok = true;
        if(key == "material")
        {
            HaloActorDef::Material m{};
            ok = bool(in >> m.type >> m.flags >> m.body >> m.shield);
            // The optional shield leak (tools/halo_actors.py); a table without it leaves -1.
            float leak = -1;
            if(ok && (in >> leak) && std::isfinite(leak)) m.leak = clamp(leak, 0.0f, 1.0f);
            if(ok) d.materials.push_back(m);
        }
        else if(key == "resistance")
        {
            // resistance <indirect material> <coll body> <coll shield> <shield material type>
            //            <failure function> <failure threshold> <maximum failure>
            ok = bool(in >> d.indirectMaterial >> d.collBody >> d.collShield >> d.shieldMaterial >> d.shieldFailureFunction >>
                      d.shieldFailureThreshold >> d.shieldFailureMaximum);
            for(float v : {d.collBody, d.collShield, d.shieldFailureThreshold, d.shieldFailureMaximum}) if(ok && !std::isfinite(v)) ok = false;
            d.hasResistance = ok;
        }
        else if(key == "shieldtime")
        {
            // shieldtime <minimum stun damage> <stun s> <recharge s> <runtime recharge velocity>
            ok = bool(in >> d.shieldStunDamage >> d.shieldStunTime >> d.shieldRechargeTime >> d.shieldRechargeVelocity);
            for(float v : {d.shieldStunDamage, d.shieldStunTime, d.shieldRechargeTime, d.shieldRechargeVelocity})
                if(ok && !(std::isfinite(v) && v >= 0)) ok = false;
            d.hasShieldTime = ok;
        }
        return ok;
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
            if(key == "format") { int v = 0; in >> v; ok = versioned = v >= 1 && v <= formatVersion; }
            else if(key == "difficulty" && !d)
            {
                // difficulty <value index> <name> <easy> <normal> <hard> <impossible>
                int index = -1;
                std::string name;
                float v[4];
                ok = bool(in >> index >> name >> v[0] >> v[1] >> v[2] >> v[3]) && index >= 0 && index < HALO_DIFFICULTY_VALUES;
                loopi(4) if(ok && !std::isfinite(v[i])) ok = false;
                if(ok)
                {
                    loopi(4) difficultyValues[index][i] = v[i];
                    difficultySeen[index] = true;
                }
            }
            else if(key == "actor") { definitions.emplace_back(); d = &definitions.back(); in >> d->index; ok = d->index == int(definitions.size()) - 1; }
            else if(!d) ok = false;
            else if(key == "path") std::getline(in >> std::ws, d->path);
            else if(key == "family") ok = bool(in >> d->family >> d->kind >> d->type >> d->actrFlags >> d->actvFlags);
            else if(key == "weapon") ok = bool(in >> d->weapon);
            else if(key == "vision") ok = bool(in >> d->visionDistance >> d->visionCentral >> d->visionMax >> d->peripheralAngle >> d->peripheralDistance >> d->hearing);
            else if(key == "fire") ok = bool(in >> d->maxRange >> d->rate >> d->error >> d->firstDelayMin >> d->firstDelayMax >> d->combatMin >> d->combatMax >> d->lead >> d->damageScale);
            else if(key == "burst") ok = bool(in >> d->burstMin >> d->burstMax >> d->separationMin >> d->separationMax);
            else if(key == "grenade")
            {
                ok = bool(in >> d->grenadeType >> d->grenadeChance >> d->grenadeRangeMin >> d->grenadeRangeMax >> d->grenadeMin >> d->grenadeMax);
                // The optional encounter grenade timeout (tools/halo_actors.py); without it -1.
                float timeout = -1;
                if(ok && (in >> timeout) && std::isfinite(timeout) && timeout >= 0) d->grenadeTimeout = timeout;
            }
            else if(key == "vitality") ok = bool(in >> d->body >> d->shield);
            else if(key == "defense") ok = bool(in >> d->hideShield >> d->emergeAttack >> d->emergePursue);
            else if(key == "morale") ok = bool(in >> d->casualtyTrigger >> d->friendKilled >> d->leaderKilled);
            else if(key == "berserk")
            {
                ok = bool(in >> d->berserkAmount >> d->berserkThreshold >> d->meleeRange);
                // The optional actr melee attack delay (tools/halo_actors.py); without it -1.
                float timer = -1;
                if(ok && (in >> timer) && std::isfinite(timer) && timer >= 0) d->meleeDelayTimer = timer;
            }
            else if(key == "positions") ok = bool(in >> d->guardMin >> d->guardMax >> d->positionMin >> d->positionMax);
            else if(key == "team") ok = bool(in >> d->team);
            else if(key == "unitmelee") ok = bool(in >> d->unitMelee[0] >> d->unitMelee[1] >> d->unitMelee[2]);
            else if(key == "material" || key == "resistance" || key == "shieldtime") ok = parseColl(key, in, *d);
            // unitmeleematerials <33 modifiers>: the unit melee jpt!'s (optional; older engines skip it).
            else if(key == "unitmeleematerials")
            {
                loopi(33) if(ok && !(bool(in >> d->unitMeleeMaterials[i]) && std::isfinite(d->unitMeleeMaterials[i]))) ok = false;
                d->hasUnitMeleeMaterials = ok;
            }
            else if(key == "major") std::getline(in >> std::ws, d->majorPath);
            // Optional format 3 lines (tools/halo_actors.py; older engines skip them).
            else if(key == "unit") ok = bool(in >> d->unitFlags >> d->bipedFlags);
            else if(key == "spawn")
            {
                // spawn <actor index> <lower> <upper> <throw velocity>: the index is checked once
                // the table is read; bounds Halo could not draw from (negative, or upper below lower:
                // seed_random_range's unsigned span, random_math.c:203-209) leave no spawn.
                ok = bool(in >> d->spawnVariant >> d->spawnLower >> d->spawnUpper >> d->spawnVelocity) &&
                     std::isfinite(d->spawnVelocity);
                if(ok && (d->spawnLower < 0 || d->spawnUpper < d->spawnLower)) d->spawnVariant = -1;
            }
            else if(key == "pop")
            {
                ok = bool(in >> d->popInner >> d->popOuter >> d->popLower >> d->popUpperMin >> d->popUpperMax >> d->popFlags);
                for(float v : {d->popInner, d->popOuter, d->popLower, d->popUpperMin, d->popUpperMax}) if(ok && !std::isfinite(v)) ok = false;
            }
            // destroyed <effe tag>: the body destroyed effect set off when the unit is destroyed.
            else if(key == "destroyed") std::getline(in >> std::ws, d->destroyedEffect);
            else if(key == "end") d = nullptr;
            if(!ok)
            {
                conoutf("\f3Halo actors: invalid line in packages/halo/actors.txt: %s", line.c_str());
                definitions.clear();
                loopi(HALO_DIFFICULTY_VALUES) difficultySeen[i] = false;
                return false;
            }
        }
        if(d)
        {
            conoutf("\f3Halo actors: packages/halo/actors.txt ends inside variant %d; re-run tools/halo_actors.py", d->index);
            definitions.clear();
            loopi(HALO_DIFFICULTY_VALUES) difficultySeen[i] = false;
            return false;
        }
        if(!versioned) definitions.clear();
        if(definitions.empty()) loopi(HALO_DIFFICULTY_VALUES) difficultySeen[i] = false;
        // The major upgrade references, by path (one level, as actor_place takes it: actors.c:2687).
        // A path not in the table, or naming the variant itself, gives no upgrade.
        for(HaloActorDef &def : definitions)
        {
            def.major = -1;
            if(def.majorPath.empty()) continue;
            for(const HaloActorDef &other : definitions)
                if(!strcasecmp(other.path.c_str(), def.majorPath.c_str())) { def.major = other.index; break; }
            if(def.major == def.index) def.major = -1;
        }
        // A spawned variant outside the table, or the variant itself (a unit releasing its own kind
        // would recurse), gives no spawn.
        for(HaloActorDef &def : definitions)
            if(def.spawnVariant < 0 || def.spawnVariant >= int(definitions.size()) || def.spawnVariant == def.index) def.spawnVariant = -1;
        return versioned && !definitions.empty();
    }

    // --- perception -------------------------------------------------------

    vec eyeOf(const playerent *p) { return p->o; }
    vec centreOf(const playerent *p) { return vec(p->o).sub(vec(0, 0, p->eyeheight * 0.4f)); }

    bool alive(const playerent *p) { return p && p->state == CS_ALIVE; }

    // The Monitor and Engineers never fight; nobody fights the Monitor (it cannot be hurt).
    bool passive(const Actor &a) { return a.def->type == TYPE_MONITOR || a.def->type == TYPE_ENGINEER; }
    bool untargetable(const Actor *a) { return a && a->def->type == TYPE_MONITOR; }
    bool flyer(const Actor &a) { return a.def->flying(); }
    // Charges into melee: the actr's always-charge flag, or charging (or berserking) while attacking.
    bool charges(const Actor &a)
    {
        const unsigned f = a.def->actrFlags;
        return (f & ACTR_ALWAYS_CHARGE) || (a.posture == HALO_ATTACKING && (f & (ACTR_CHARGE_ATTACKING | ACTR_BERSERK_ATTACKING)));
    }

    bool sees(const Actor &a, CBot *body, const playerent *t)
    {
        if(a.flags & HALO_AI_BLIND) return false;
        const Actor *other = find(t);
        if(other && ((other->flags & HALO_AI_DISREGARD) || untargetable(other))) return false;
        if(a.flags & HALO_AI_MAGIC_SIGHT) return true;
        const HaloActorDef &d = *a.def;
        vec delta = vec(centreOf(t)).sub(eyeOf(a.bot));
        const float distance = delta.magnitude();
        const float s = scale();
        const float yaw = a.bot->yaw * RAD;
        const vec facing(sinf(yaw), -cosf(yaw), 0);
        const float angle = acosf(clamp(vec(delta.x, delta.y, 0).normalize().dot(facing), -1.0f, 1.0f));
        // Active camouflage hides the player beyond a short range, a little further while they
        // fire; actors already fighting them lose track (perceive drops to alert after 4 s).
        // A camouflaged actor (stealth Elites) is hidden the same way; its cloak already
        // flickers weaker while it fires or is hit (haloactorcloak).
        const float camo = t == player1 ? haloplayercamo() : haloactorcloak(t);
        const bool revealed = t == player1 && lastmillis - player1->lastaction < 1000;
        const float reach = s * (1 - camo * (revealed ? .6f : .88f));
        const bool inCone = distance <= d.visionDistance * reach && angle <= d.visionMax * 0.5f + 0.3f;
        const bool inPeriphery = distance <= d.peripheralDistance * reach && angle <= d.peripheralAngle * 0.5f;
        // Actors already fighting keep tracking a target they know about.
        const bool tracking = a.awareness == COMBAT && a.target == t && distance <= d.visionDistance * reach * 1.5f;
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
        // The Monitor pays no attention to fights (its scripts lead it); Engineers notice only
        // enough to keep away (fleeing is decided in think).
        if(a.def->type == TYPE_MONITOR) { a.target = nullptr; a.awareness = UNAWARE; return; }
        // A target whose team the scripts just allied with (ai_allegiance) is dropped at once,
        // rather than fired at until it goes unseen for 4 s.
        if(a.target && !haloactorhostile(a.bot, a.target))
        {
            a.target = nullptr;
            if(a.awareness == COMBAT) { a.awareness = ALERT; a.positionUntil = 0; }
        }
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

    // --- flight -----------------------------------------------------------
    //
    // Flyers (the actr "flying" flag: Sentinels, the Monitor, Engineers) have no gravity and no
    // ground snap (physics.cpp asks halohovering); the brain sets their climb rate in vel.z.

    // The floor under the flyer (AssaultCube units), sampled a few times a second.
    float floorUnder(Actor &a)
    {
        botent *e = a.bot;
        if(lastmillis >= a.floorAt || fabsf(e->o.x - a.floorFrom.x) + fabsf(e->o.y - a.floorFrom.y) > 2.0f)
        {
            const float reach = 60.0f * scale();
            // On retail collision the section's BSP is the floor (the cube map under it is not);
            // on stock geometry the cubes are.
            auto probe = [&](float dz, float &distance)
            {
                if(halocollisionactive())
                {
                    HaloCollisionHit hit;
                    if(!halocollisiontrace(e->o, vec(e->o).add(vec(0, 0, dz * reach)), hit) || hit.fraction >= 1) return false;
                    distance = hit.fraction * reach;
                    return true;
                }
                vec normal;
                distance = raycube(e->o, vec(0, 0, dz), normal);
                return distance >= 0 && distance <= reach;
            };
            float d = 0, c = 0;
            // No floor within reach (over a chasm): hold the height the flyer is at.
            a.floorZ = probe(-1, d) ? e->o.z - d : e->o.z - e->eyeheight - a.hoverHeight * 0.5f * scale();
            a.ceilingZ = probe(1, c) ? e->o.z + c : e->o.z + reach;
            a.floorAt = lastmillis + 200;
            a.floorFrom = e->o;
        }
        return a.floorZ;
    }

    // Sets the climb rate that brings the flyer's feet towards `feetZ`, kept a little above the
    // floor and below the ceiling.
    void holdAltitude(Actor &a, float feetZ)
    {
        botent *e = a.bot;
        const float s = scale(), feet = e->o.z - e->eyeheight;
        const float lowest = floorUnder(a) + 0.4f * s, highest = a.ceilingZ - e->eyeheight - e->aboveeye - 0.3f * s;
        const float want = highest > lowest ? clamp(feetZ, lowest, highest) : lowest;
        e->vel.z = clamp((want - feet) / (1.5f * s), -0.6f, 0.6f);
    }

    // Flies straight at a point (no waypoints: flyers clear the floor's obstacles), holding the
    // point's height or the actor's hover height over the floor, whichever is higher.
    bool flyTo(Actor &a, CBot *body, const vec &to, float radius, bool faceTravel)
    {
        botent *e = a.bot;
        const float s = scale(), feet = e->o.z - e->eyeheight;
        holdAltitude(a, max(to.z, floorUnder(a) + a.hoverHeight * s * 0.5f));
        a.goal = nullptr;
        if(vec(to).sub(e->o).magnitudexy() <= radius && fabsf(to.z - feet) < 3.0f * s) { e->move = e->strafe = 0; return true; }
        if(faceTravel) body->AimToVec(vec(to).add(vec(0, 0, e->eyeheight)));
        steer(e, to);
        body->CheckStuck();
        return false;
    }

    // Returns true once the actor stands within `radius` of the point.
    bool walkTo(Actor &a, CBot *body, const vec &to, float radius, bool faceTravel)
    {
        if(flyer(a)) return flyTo(a, body, to, radius, faceTravel);
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

    // --- difficulty -------------------------------------------------------
    //
    // An actor's combat numbers on the selected difficulty: halocombatscaling on its current team
    // (a team hostile to the player takes the level's values, the player's side Normal's), applied
    // to the variant's actv values at use as Halo does; actors.txt keeps the tag values. Shared by
    // the brain and the QA probe (haloactorcombatdump), so the probe prints what the brain uses.
    struct CombatValues
    {
        HaloCombatScaling scale;
        float error = 0;        // aim error (radians), before the camouflage term
        float separation = 1;   // on the pause between bursts: burst_separation, times 1.7 playfighting
        float duration = 1;     // on a burst's length: 0.6 playfighting (actor_combat.c:1259-1262)
        bool leads = false;     // the lead gate (actor_combat.c:2194-2199)
        int fireDelay = 0;      // ms between trigger pulls in a burst; 0: the weapon's own cadence
        int meleeDelay = 0;     // ms between a melee-only actor's swings; 0: not melee-only
    };

    // The wait in ms from one melee attack to the next, `attack` being the engine's own interval for
    // the attack. Halo starts a melee charge once melee_delay_scale times the actr's
    // melee_attack_delay_timer (0 while berserk) plus melee_delay_base seconds have passed since
    // last_melee_attack_time (actions.c:2563-2568, 2593-2601), which the charge refreshes on every
    // tick until its attack ends (action_charge.c:400-402): the delay follows the attack. The engine
    // plays no melee animation, so `attack` stands for the attack itself (the Hunter's 1.6 to 2.2 s
    // against its 61-frame, 2.03 s `stand unarmed melee`; a sword swing's 1.2 s against the Elite's
    // 28 and 35-frame sword melees) and is not scaled (judgement call). Halo's rule skips swarm actors
    // (actions.c:2541, the infection forms), and an older actors.txt has no timer: both keep the
    // engine's former rule, the whole interval scaled (halomeleedelay).
    int meleeInterval(const Actor &a, int attack)
    {
        const HaloActorDef &d = *a.def;
        if(d.meleeDelayTimer < 0 || d.swarm()) return halomeleedelay(attack);
        const float timer = a.morale == BERSERK ? 0.0f : d.meleeDelayTimer;
        const float delay = halodifficultyget(HALO_DIFFICULTY_MELEE_DELAY_SCALE) * timer +
                            halodifficultyget(HALO_DIFFICULTY_MELEE_DELAY_BASE);
        return max(1, attack + max(0, int(delay * 1000 + .5f)));
    }

    // Halo's encounter grenade timeout for `a` (actions.c:2350-2373): the variant's
    // encounter_grenade_timeout times grenade_timer_scale on its team (the encounter's, which the
    // engine's actors share), in whole ticks (throw_delay_ticks), counted from the encounter's last
    // throw. `wait` gets the scaled timeout in ms and `encounter` the actor's encounter (-1 none);
    // returns the ms left (0: no wait). An actor outside any encounter, or a table without the
    // timeout, has none (Halo skips the test for encounter_index NONE).
    int grenadeTimeoutLeft(const Actor &a, float timerScale, int &wait, int &encounter)
    {
        wait = 0;
        encounter = -1;
        int squad = -1;
        if(a.def->grenadeTimeout < 0 || !haloscenariosquadof(a.bot, encounter, squad)) { encounter = -1; return 0; }
        // Doubled while the actor plays at fighting (actions.c:2361-2362).
        const float playfight = (a.flags & HALO_AI_PLAYFIGHT) ? 2.0f : 1.0f;
        const long ticks = long(a.def->grenadeTimeout * timerScale * playfight * 30);
        wait = int(ticks * 1000 / 30);
        const auto last = encounterThrows.find(encounter);
        // A throw time ahead of now (the clock went back) counts as long past.
        if(last == encounterThrows.end() || last->second > lastmillis) return 0;
        return max(0, last->second + wait - lastmillis);
    }

    CombatValues combatValues(const Actor &a)
    {
        CombatValues v;
        v.scale = halocombatscaling(a.team);
        const HaloActorDef &d = *a.def;
        // actor_combat.c:1268-1272: burst_error = projectile_error_angle * projectile_error (the
        // error every projectile of the burst takes).
        v.error = d.error * v.scale.projectileError;
        v.separation = v.scale.burstSeparation;
        // ai_playfight (actor_external_orders.playfighting, the encounter's flag): the error doubled
        // plus one degree (actor_combat.c:1278-1281), the pause between bursts 1.7 times as long
        // (612-615) and the bursts 0.6 as long (1259-1262).
        if(a.flags & HALO_AI_PLAYFIGHT)
        {
            v.error = 2 * v.error + RAD;
            v.separation *= 1.7f;
            v.duration = .6f;
        }
        // actor_combat.c:2194-2210: target_leading is added to the variant's lead fraction only
        // for the test MIN(1, sum) > 0; the lead itself (point_from_line3d) is the variant's own
        // fraction. Followed as written (the conservative reading): no difficulty leads harder,
        // and since every exported lead is 0 or more the gate never closes on a lead that exists.
        v.leads = min(1.0f, d.lead + v.scale.targetLeading) > 0;
        const weapon *w = a.bot ? a.bot->weaponsel : nullptr;
        const bool melee = w && w->type == GUN_KNIFE;
        // actor_combat.c:2380-2404: a variant rate of 0 holds the trigger (the weapon's own
        // cadence, unscaled); otherwise a pull every TICKS_PER_SECOND / (rate * rate_of_fire)
        // ticks, at least 2. The engine's slot delay (spawnEnemy) is 1000 / rate ms; the same
        // formula with the factor, floored at two ticks (66 ms: no exported rate reaches it on Normal).
        // Both delays are kept within the slot's short attackdelay (guninfo, weapon.h).
        if(!melee && d.rate > 0) v.fireDelay = clamp(int(1000 / max(d.rate * v.scale.rateOfFire, 1e-3f)), 66, 32767);
        // actions.c:2563-2568: the swing (the slot delay, at least 1.2 s for a sword) then the
        // actr's melee delay on the level (meleeInterval).
        if(melee && a.baseDelay > 0) v.meleeDelay = min(meleeInterval(a, a.baseDelay), 32767);
        return v;
    }

    // --- firing -----------------------------------------------------------

    void fire(Actor &a, CBot *body)
    {
        botent *e = a.bot;
        playerent *t = a.target;
        e->attacking = false;
        // `botsshoot 0` holds every bot's fire, as it does for AssaultCube's own AI.
        if(!BotManager.BotsShoot()) return;
        if(!t || !alive(t) || lastmillis < a.reactUntil || a.morale == PANICKED || a.awareness != COMBAT || passive(a)) return;
        // A gunner on a turret an EMP stunned holds its fire until the stun wears off.
        if(halovehicleriderstunned(e)) return;
        const HaloActorDef &d = *a.def;
        const float s = scale();
        const float distance = e->o.dist(t->o);
        const bool melee = e->weaponsel->type == GUN_KNIFE;
        if(!melee && d.maxRange > 0 && distance > d.maxRange * s * 1.25f) return;
        // Difficulty (combatValues): error, lead gate, trigger and swing delays, burst separation.
        const CombatValues combat = combatValues(a);
        // Lead moving targets by the variant's lead fraction of the travel time.
        const HaloWeaponDef *w = haloweapon(d.weapon);
        const HaloTriggerDef *trigger = w ? w->trigger() : nullptr;
        const float speed = trigger && trigger->projectile.valid ? max(1.0f, trigger->projectile.speed * s) : 1000.0f;
        vec aim = centreOf(t);
        if(combat.leads) aim.add(vec(t->vel).mul(d.lead * distance / speed));
        // A camouflaged player is hard to hit: the shots spread wider (the engine's own term,
        // not scaled by difficulty).
        const float camoError = (t == player1 ? haloplayercamo() : haloactorcloak(t)) * 0.12f;
        if(combat.error > 0 || camoError > 0)
        {
            const float spread = tanf(combat.error + camoError) * distance;
            aim.add(vec(frand(-spread, spread), frand(-spread, spread), frand(-spread, spread) * 0.5f));
        }
        body->AimToVec(aim);
        if(lastmillis >= a.nextBurst && lastmillis >= a.burstUntil)
        {
            a.burstUntil = lastmillis + millisFor(frand(d.burstMin, d.burstMax) * combat.duration);
            // actor_combat.c:599-605: the separation times burst_separation (and playfight's 1.7).
            a.nextBurst = a.burstUntil + millisFor(frand(d.separationMin, d.separationMax) * combat.separation);
        }
        if(lastmillis >= a.burstUntil && !melee) return;
        // Fire only once roughly facing the target.
        const float yawError = fabsf(e->targetyaw - e->yaw);
        if(min(yawError, 360 - yawError) > 25) return;
        if(melee && distance > max(d.meleeRange * s, 5.0f)) return;
        // In a seat that works a vehicle's gun (a Ghost's or Banshee's pilot, a turret's or a
        // Warthog's gunner) the vehicle fires instead (halo_vehicle.cpp); the aim above still
        // turns a turret with its gunner.
        if(halovehiclearmedseat(e)) return;
        // The cadence the weapon code waits between pulls (weapon.cpp reads the slot's
        // attackdelay for a bot): the variant's rate times rate_of_fire, or the swing delay.
        if(combat.fireDelay > 0) e->weaponsel->info.attackdelay = combat.fireDelay;
        else if(combat.meleeDelay > 0) e->weaponsel->info.attackdelay = combat.meleeDelay;
        e->attacking = true;
        shoot(e, aim);
    }

    void throwGrenade(Actor &a, CBot *body)
    {
        const HaloActorDef &d = *a.def;
        botent *e = a.bot;
        if(!grenadesAllowed || !BotManager.BotsShoot() || !a.target || passive(a) || lastmillis < a.nextGrenade || e->mag[GUN_GRENADE] <= 0 || d.grenadeChance <= 0) return;
        // Nobody throws from a vehicle seat (or a dropship's hold).
        if(halovehicleinside(e) || haloscenariocarried(e)) return;
        const int wait = 4000 + rnd(4000);
        a.nextGrenade = lastmillis + wait;
        const float distance = e->o.dist(a.target->o), s = scale();
        if(distance < max(d.grenadeRangeMin, 2.0f) * s || distance > max(d.grenadeRangeMax, 8.0f) * s) return;
        // Difficulty on the actor's current team: the throw chance times grenade_chance_scale
        // (actions.c:3642-3647), then Halo's encounter-wide timeout times grenade_timer_scale after
        // any member's throw (actions.c:2350-2373, grenadeTimeoutLeft). The 4 to 8 s between checks
        // stays the engine's own and unscaled: it stands for Halo's throw_grenade_delay between
        // chance rolls (actions.c:3636-3641) together with the target and trajectory tests the
        // engine does not make (judgement call). Without the actv timeout (an older actors.txt) the
        // engine's former stand-in: its per-actor wait after its own throw times grenade_timer_scale.
        const HaloCombatScaling scaling = halocombatscaling(a.team);
        if(rndscale(1.0f) > d.grenadeChance * scaling.grenadeChance) return;
        int timeout = 0, encounter = -1;
        // Playing at fighting doubles the timeout (grenadeTimeoutLeft), and its stand-in too.
        const bool playfight = (a.flags & HALO_AI_PLAYFIGHT) != 0;
        if(d.grenadeTimeout < 0) a.nextGrenade = lastmillis + max(1, int(wait * scaling.grenadeTimer * (playfight ? 2 : 1) + .5f));
        else if(grenadeTimeoutLeft(a, scaling.grenadeTimer, timeout, encounter) > 0) return;
        // Playing at fighting, the grenade is aimed 1.5 world units off the target in a random
        // direction (actor_combat.c:834-837, actor_combat_find_nearby_target at 519-580). Judgement
        // call: only the horizontal miss; Halo's aim point also sits 1.5 units above the target and
        // is pulled in short of a wall, which the engine's distance-based throw has no use for.
        vec at = a.target->o;
        if(playfight)
        {
            const float heading = rndscale(2 * PI);
            at.add(vec(cosf(heading), sinf(heading), 0).mul(1.5f * s));
        }
        vec direction = vec(at).sub(e->o);
        const float flight = clamp(direction.magnitude() / 60.0f, 0.4f, 1.6f);
        direction.normalize().mul(1.6f);
        direction.z += 0.25f * flight;
        // The retail grenade of the variant's type (the Covenant throw plasma); AssaultCube's
        // frag grenade without packages/halo/equipment.txt.
        if(!halothrowgrenade(e, haloactorgrenadetype(e), direction))
        {
            grenadeent *g = new grenadeent(e);
            bounceents.add(g);
            g->activate(e->o, at);
            g->moveoutsidebbox(direction, e);
            g->_throw(g->o, direction);
        }
        --e->mag[GUN_GRENADE];
        // The throw starts the encounter's timeout for every member (actions.c:3604-3607).
        if(encounter >= 0) encounterThrows[encounter] = lastmillis;
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
                    // At the wheel the vehicle carries the actor there (halo_vehicle.cpp), which
                    // cannot stop on a point as a body does: within 4 world units counts.
                    if(point && !walkTo(a, body, *point, halovehicleinside(e) ? max(a.radius * s, 4.0f * s) : max(a.radius * s, 2.5f), true))
                        return true;
                    if(c.type == ATOM_GO_TO_FACE && c.otherPoint >= 0 && c.otherPoint < int(p.points.size()))
                        body->AimToVec(vec(p.points[c.otherPoint]).add(vec(0, 0, e->eyeheight)));
                    break;
                case ATOM_LOOK: if(point) body->AimToVec(vec(*point).add(vec(0, 0, e->eyeheight))); break;
                case ATOM_LOOK_PLAYER: body->AimToVec(player1->o); break;
                case ATOM_CROUCH: e->trycrouch = c.first > 0; break;
                case ATOM_SET_RADIUS: a.radius = c.first; break;
                case ATOM_LOOP: a.loopAt = int(a.cursor); a.cursor = 0; continue;
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

    // An ally the player walks into steps aside, as Halo's marines and crewmen do, so a
    // corridor they stand in does not stop the player.
    bool giveWay(Actor &a)
    {
        botent *e = a.bot;
        if(!haloteamfriendly(a.team) || flyer(a) || !alive(player1)) return false;
        vec gap = vec(e->o).sub(player1->o);
        gap.z = 0;
        const float reach = e->radius + player1->radius + 1.5f;
        const vec heading(player1->vel.x, player1->vel.y, 0);
        if(gap.squaredlen() > reach * reach || heading.squaredlen() < .04f || gap.dot(heading) <= 0) return false;
        // Step off the player's line, to whichever side the actor already stands on.
        vec side(-heading.y, heading.x, 0);
        side.normalize();
        if(side.dot(gap) < 0) side.mul(-1);
        steer(e, vec(e->o).add(vec(side).mul(4.0f)));
        return true;
    }

    // --- Hunters ----------------------------------------------------------

    // A Hunter's smash: the unit's melee jpt! (35 to 45 with a strong shove) against whatever it
    // fights once it stands within reach and roughly faces it, every 1.6 to 2.2 s plus its melee delay.
    void smash(Actor &a)
    {
        botent *e = a.bot;
        playerent *t = a.target;
        if(!t || !alive(t) || lastmillis < a.nextSmash || !BotManager.BotsShoot()) return;
        const float s = scale();
        const float reach = e->radius + t->radius + 0.6f * s;
        if(vec(t->o).sub(e->o).magnitudexy() > reach || fabsf(t->o.z - e->o.z) > max(e->eyeheight, t->eyeheight)) return;
        const float yawError = fabsf(e->targetyaw - e->yaw);
        if(min(yawError, 360 - yawError) > 40) return;
        // The wait between smashes on the selected difficulty, for every team alike (actions.c:
        // 2563-2568, game_difficulty_get_value): the engine's 1.6 to 2.2 s for the smash itself,
        // then melee_delay_scale times the actr's melee_attack_delay_timer (the Hunters' 0.1 s; 0
        // while berserk) plus melee_delay_base (meleeInterval; without the timer the whole 1.6 to
        // 2.2 s scaled, as before).
        a.nextSmash = lastmillis + meleeInterval(a, 1600 + rnd(600));
        const float *m = a.def->unitMelee;
        const float full = m[2] > 0 ? frand(m[1], m[2]) : 40.0f;
        vec dir = vec(t->o).sub(e->o);
        dir.z = 0;
        if(dir.iszero()) dir = vec(0, 1, 0);
        dir.normalize();
        e->lastattackweapon = e->weaponsel;
        e->lastaction = lastmillis;
        // The smash carries its jpt!'s material modifiers (actors.txt `unitmeleematerials`), as any
        // damage does in Halo (../Halo3DS objects/damage.c:898-899, 1174): 1 on the player's
        // cyborg armour and shield, 0 on another Hunter. Without the line, none, as before.
        HaloDamage melee;
        melee.lower = m[0];
        melee.upperMin = m[1];
        melee.upperMax = m[2];
        melee.hasMaterials = a.def->hasUnitMeleeMaterials;
        loopi(HALO_DAMAGE_MATERIALS) melee.materials[i] = a.def->unitMeleeMaterials[i];
        halohit(full, HALO_SIDE_EFFECT_NONE, t, e, dir, GUN_KNIFE, false, 0, melee.hasMaterials ? &melee : nullptr);
        // The jpt!'s instantaneous acceleration throws the victim back.
        if(t->state == CS_ALIVE) { t->vel.add(vec(dir).mul(1.2f)); t->vel.z += 0.5f; }
        ++a.smashes;
    }

    // Charge distance: inside it a Hunter closes in to smash instead of firing the cannon.
    float chargeDistance(const Actor &a) { return 1.6f * scale(); }

    // Hunters pair off with the nearest unpartnered Hunter of their team.
    void pairHunter(Actor &a)
    {
        if(a.def->type != TYPE_HUNTER || a.partner) return;
        Actor *best = nullptr;
        float bestDistance = 20.0f * scale();
        for(Actor &other : actors)
        {
            if(&other == &a || other.def->type != TYPE_HUNTER || other.partner || other.team != a.team || !alive(other.bot)) continue;
            const float d = other.bot->o.dist(a.bot->o);
            if(d < bestDistance) { bestDistance = d; best = &other; }
        }
        if(best) { a.partner = best->bot; best->partner = a.bot; }
    }

    // --- flying combat -----------------------------------------------------

    // Sentinels hold a height over their target, keep their combat range and strafe across the
    // target's line of fire, changing direction and height now and then.
    void flyCombat(Actor &a, CBot *body)
    {
        botent *e = a.bot;
        const float s = scale();
        const playerent *t = a.target;
        if(lastmillis >= a.nextStrafe)
        {
            a.nextStrafe = lastmillis + 900 + rnd(1400);
            a.strafeDir = rnd(2) ? 1 : -1;
            a.hoverHeight = frand(1.5f, 3.5f);
        }
        const float targetFeet = (t ? t->o.z - t->eyeheight : a.lastKnown.z);
        holdAltitude(a, targetFeet + a.hoverHeight * s * 0.6f);
        const float d = vec(a.lastKnown).sub(e->o).magnitudexy();
        const float wantMin = a.def->combatMin * s, wantMax = max(a.def->combatMax, a.def->combatMin + 1) * s;
        vec away = vec(e->o).sub(a.lastKnown);
        away.z = 0;
        if(away.iszero()) away = vec(1, 0, 0);
        away.normalize();
        if(d > wantMax) steer(e, a.lastKnown);
        else if(d < wantMin) steer(e, vec(e->o).add(vec(away).mul(20)));
        else { e->move = 0; e->strafe = a.strafeDir; }
        body->CheckStuck();
    }

    void think(Actor &a, CBot *body)
    {
        ++a.thinks;
        botent *e = a.bot;
        e->move = e->strafe = 0;
        e->attacking = false;
        // Flyers keep their hover height unless something below asks for another one.
        if(flyer(a)) holdAltitude(a, floorUnder(a) + a.hoverHeight * scale() * 0.5f);
        if(a.flags & HALO_AI_BRAINDEAD) return;
        perceive(a, body);
        updateMorale(a);
        pairHunter(a);
        if(giveWay(a)) return;

        if(a.boarding)
        {
            // ai_go_to_vehicle: on the way to the seat, fighting what it meets; the vehicle
            // seats it on arrival (halo_vehicle.cpp).
            walkTo(a, body, a.boardAt, max(1.5f * scale(), 2.0f), a.awareness != COMBAT);
            if(a.awareness == COMBAT && a.target) { body->AimToVec(centreOf(a.target)); fire(a, body); }
            return;
        }
        const bool commanded = runCommands(a, body);
        const bool initiative = !a.program || a.commandStatus != 1 || (a.program->flags & COMMAND_ALLOW_INITIATIVE);
        if(commanded && !initiative)
        {
            if(a.program->flags & COMMAND_ALLOW_TARGETING) fire(a, body);
            return;
        }

        const float s = scale();
        if(seekCorpse(a, body)) return;
        // Engineers never fight: they float away from a threat and otherwise hover in place.
        if(passive(a) && a.def->type == TYPE_ENGINEER && a.awareness == COMBAT && a.target)
        {
            vec away = vec(e->o).sub(a.target->o);
            away.z = 0;
            if(!away.iszero() && away.magnitude() < 12.0f * s) steer(e, vec(e->o).add(away.normalize().mul(20)));
            return;
        }
        if(a.awareness == COMBAT && a.target && !passive(a))
        {
            const bool hunter = a.def->type == TYPE_HUNTER;
            const float distance = e->o.dist(a.target->o);
            if(hunter && (a.morale == BERSERK || distance < chargeDistance(a)))
            {
                // Hunters close in and smash; a Hunter whose brother fell does nothing else.
                walkTo(a, body, a.target->o, e->radius + a.target->radius, false);
                body->AimToVec(centreOf(a.target));
                smash(a);
                if(a.morale != BERSERK) fire(a, body);
                return;
            }
            if(flyer(a))
            {
                flyCombat(a, body);
                fire(a, body);
                return;
            }
            if(hunter && alive(a.partner) && e->o.dist(a.partner->o) > 10.0f * s)
            {
                // Bond brothers fight side by side: rejoin a partner that drifted off.
                walkTo(a, body, a.partner->o, 4.0f * s, false);
                fire(a, body);
                return;
            }
            if(a.morale == BERSERK || charges(a) || e->weaponsel->type == GUN_KNIFE)
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
            // Playfighting actors throw too, with a doubled timeout and a miss (throwGrenade).
            throwGrenade(a, body);
            return;
        }
        if(passive(a) && a.awareness == COMBAT)
        {
            // An Engineer that lost its threat drifts back to calm.
            a.awareness = ALERT;
        }

        if(alive(a.follow) && a.awareness != ALERT)
        {
            // Squads told to follow a unit trail it at their follow distance.
            const float want = max(a.followDistance, 4.0f) * s;
            if(e->o.dist(a.follow->o) > want * 1.5f) walkTo(a, body, a.follow->o, want, true);
            return;
        }
        if((a.flags & HALO_AI_FOLLOW_PLAYER) ||
           ((a.team == HALO_TEAM_HUMAN || a.team == HALO_TEAM_PLAYER) && haloteamfriendly(a.team) && !a.positions))
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
    else conoutf("\f3Halo actors: packages/halo/actors.txt is unusable; actors use the built-in profiles");
    return loaded;
}

int haloactordefcount() { haloactorsload(); return int(definitions.size()); }

namespace
{
    HaloActorDef playerUnit;
    bool playerAttempted = false, playerLoaded = false;

    // packages/halo/player.txt (tools/halo_actors.py PLAYER_FIELDS): `format 1`, then `player <unit
    // tag>`, its `material`, `resistance` and `shieldtime` lines and `end`. Keys it does not know are
    // skipped (a newer file still loads). Usable only with a material, a resistance line whose
    // shield material type is a retail material type and an indirect material that is one of them
    // or none (-1).
    bool parsePlayer(const char *text)
    {
        std::istringstream lines(text);
        std::string line;
        bool versioned = false, open = false, closed = false;
        HaloActorDef d;
        while(std::getline(lines, line))
        {
            if(line.empty() || line[0] == '#') continue;
            std::istringstream in(line);
            std::string key;
            in >> key;
            bool ok = true;
            if(key == "format") { int v = 0; in >> v; ok = versioned = v == 1; }
            else if(key == "player") { ok = !open && !closed; open = true; std::getline(in >> std::ws, d.path); }
            else if(key == "end") { ok = open; open = false; closed = true; }
            else if(open && (key == "material" || key == "resistance" || key == "shieldtime")) ok = parseColl(key, in, d);
            if(!ok) { conoutf("\f3Halo player: invalid line in packages/halo/player.txt: %s", line.c_str()); return false; }
        }
        if(!versioned || open || !closed || d.materials.empty() || !d.hasResistance || d.shieldMaterial < 0 || d.shieldMaterial >= 33 ||
           d.indirectMaterial < -1 || d.indirectMaterial >= int(d.materials.size()))
            return false;
        for(const HaloActorDef::Material &m : d.materials) if(m.type < 0 || m.type >= 33) return false;
        playerUnit = d;
        return true;
    }
}

const HaloActorDef *haloplayerunit()
{
    if(!playerAttempted)
    {
        playerAttempted = true;
        int size = 0;
        // Optional: without it the player takes damage by the engine's built-in rule (halodamage).
        if(char *text = loadfile(path("packages/halo/player.txt", true), &size))
        {
            playerLoaded = parsePlayer(text);
            delete[] text;
            if(playerLoaded) conoutf("Halo player: %s, %d collision materials", playerUnit.path.c_str(), int(playerUnit.materials.size()));
            else conoutf("\f3Halo player: packages/halo/player.txt is unusable; re-run tools/halo_actors.py");
        }
    }
    return playerLoaded ? &playerUnit : nullptr;
}

float halodifficultyvalue(int type, int level, float fallback)
{
    haloactorsload();
    if(type < 0 || type >= HALO_DIFFICULTY_VALUES || !difficultySeen[type]) return fallback;
    return difficultyValues[type][clamp(level, 0, 3)];
}

bool halodifficultytable()
{
    haloactorsload();
    loopi(HALO_DIFFICULTY_VALUES) if(difficultySeen[i]) return true;
    return false;
}

namespace
{
    // ../Halo3DS game/game_globals.c:167-196 game_difficulty_get_value_by_difficulty: the table's
    // value (the level clamped to 0..3). Without the table (actors.txt format 1 or 2) the engine's
    // constants from before it, where it had one, else Halo's own 1.0 for a missing table (:171),
    // except for the three values Halo adds rather than multiplies (target_tracking and
    // target_leading, actor_combat.c:2175 and 2196; melee_delay_base, actions.c:2567): 0, so an
    // older actors.txt leaves the engine's combat exactly as it was before the combat scaling.
    float difficultyAt(int type, int level)
    {
        static const float damage[4] = {0.6f, 1.0f, 1.3f, 1.7f}, vitality[4] = {0.75f, 1.0f, 1.25f, 1.5f};
        level = clamp(level, 0, 3);
        const bool additive = type == HALO_DIFFICULTY_TARGET_TRACKING || type == HALO_DIFFICULTY_TARGET_LEADING ||
                              type == HALO_DIFFICULTY_MELEE_DELAY_BASE;
        // infection_forms has no engine constant and its neutral value is 0 (Normal's: no speed
        // stagger, no pop toughness), where Halo's missing-table answer would be 1.0; an older
        // actors.txt has no pop or stagger data anyway.
        const float fallback = type == HALO_DIFFICULTY_ENEMY_DAMAGE ? damage[level] :
                               type == HALO_DIFFICULTY_ENEMY_VITALITY || type == HALO_DIFFICULTY_ENEMY_SHIELD ? vitality[level] :
                               additive || type == HALO_DIFFICULTY_INFECTION_FORMS ? 0.0f : 1.0f;
        return halodifficultyvalue(type, level, fallback);
    }
}

float halodifficultyget(int type) { return difficultyAt(type, halodifficulty()); }

float halodifficultyteamvalue(int type, int team)
{
    // game_globals.c:204-225. game_team_is_enemy(_game_team_player, team) is true for an invalid
    // team (game_allegiance.c:192-206), so no team takes the enemy value; the player's own team is
    // friendly to itself (every team is, game_allegiance.c:171-180), so the player takes the
    // friend value. global_difficulty_friend_settings (game_globals.c:138-148) maps types 0 to 3 to
    // 4 to 7; any other type on the player's side is read at Normal. (game_engine_running, the
    // multiplayer engines' Normal, does not arise in the single-player campaign.)
    if(team >= 0 && !haloteamhostile(HALO_TEAM_PLAYER, team))
    {
        if(type >= HALO_DIFFICULTY_ENEMY_DAMAGE && type <= HALO_DIFFICULTY_ENEMY_RECHARGE)
            return difficultyAt(type + HALO_DIFFICULTY_FRIEND_DAMAGE, halodifficulty());
        return difficultyAt(type, 1);
    }
    return difficultyAt(type, halodifficulty());
}

float halodifficultydamagefactor(int ownerTeam, int targetTeam, float *divisor)
{
    if(divisor) *divisor = 1;
    // damage.c:1415-1419: only damage with an owner team hostile to the player is multiplied.
    if(ownerTeam < 0 || !haloteamhostile(ownerTeam, HALO_TEAM_PLAYER)) return 1;
    const float scale = halodifficultyget(HALO_DIFFICULTY_ENEMY_DAMAGE);
    // damage.c:1603-1612: the target's team is not the owner's enemy (its own team or an ally):
    // the shield (881-887) and the body (1159-1165) divide the multiplier back out, when positive.
    if(scale > 0 && !haloteamhostile(targetTeam, ownerTeam))
    {
        if(divisor) *divisor = scale;
        return 1;
    }
    return scale;
}

HaloCombatScaling halocombatscaling(int team)
{
    HaloCombatScaling c;
    c.team = team;
    c.level = halodifficulty();
    // game_difficulty_get_team_value with the actor's team (actor->meta.team_index in
    // actor_combat.c; the encounter's team for the grenade timer, actions.c:2356-2358, which the
    // engine's actors share with their encounter): Normal's value on the player's side.
    auto teamValue = [team](int type) { return halodifficultyteamvalue(type, team); };
    c.rateOfFire = teamValue(HALO_DIFFICULTY_RATE_OF_FIRE);              // actor_combat.c:2394-2396
    c.projectileError = teamValue(HALO_DIFFICULTY_PROJECTILE_ERROR);     // :1268-1272
    c.burstError = teamValue(HALO_DIFFICULTY_BURST_ERROR);               // :1400-1408
    c.newTargetDelay = teamValue(HALO_DIFFICULTY_NEW_TARGET_DELAY);      // :1230-1235
    c.burstSeparation = teamValue(HALO_DIFFICULTY_BURST_SEPARATION);     // :599-605
    c.targetTracking = teamValue(HALO_DIFFICULTY_TARGET_TRACKING);       // :2174-2178
    c.targetLeading = teamValue(HALO_DIFFICULTY_TARGET_LEADING);         // :2195-2199
    c.overchargeChance = teamValue(HALO_DIFFICULTY_OVERCHARGE_CHANCE);   // :1785-1787 (never read after)
    c.specialFireDelay = teamValue(HALO_DIFFICULTY_SPECIAL_FIRE_DELAY);  // :1776-1778 (never read after)
    c.grenadeChance = teamValue(HALO_DIFFICULTY_GRENADE_CHANCE_SCALE);   // actions.c:3644-3647
    c.grenadeTimer = teamValue(HALO_DIFFICULTY_GRENADE_TIMER_SCALE);     // actions.c:2356-2358
    // game_difficulty_get_value: the selected level whatever the team (actions.c:2565-2568,
    // projectiles.c:802).
    c.meleeDelayScale = halodifficultyget(HALO_DIFFICULTY_MELEE_DELAY_SCALE);
    c.meleeDelayBase = halodifficultyget(HALO_DIFFICULTY_MELEE_DELAY_BASE);
    c.guidanceVsPlayer = halodifficultyget(HALO_DIFFICULTY_GUIDANCE_VS_PLAYER);
    return c;
}

int halomeleedelay(int millis)
{
    // actions.c:2565-2568: charge_delay = melee_delay_scale * delay + melee_delay_base.
    const float scaled = halodifficultyget(HALO_DIFFICULTY_MELEE_DELAY_SCALE) * millis +
                         halodifficultyget(HALO_DIFFICULTY_MELEE_DELAY_BASE) * 1000.0f;
    return max(1, int(scaled + .5f));
}

float haloguidancevsplayer() { return halodifficultyget(HALO_DIFFICULTY_GUIDANCE_VS_PLAYER); }

int halodamageteam(const playerent *attacker)
{
    if(!attacker) return -1;
    if(attacker == player1 || find(attacker)) return haloactorteam(attacker);
    // A dead actor's record is gone (haloactordeath), but the grenades and projectiles it fired
    // still carry the team they were fired with (Halo's damage keeps its owner_team_index,
    // ../Halo3DS objects/damage.c:1415): the team it spawned on (halo.cpp's HaloEnemy), not
    // haloactorteam's Covenant default, so a dead marine's frag is not taken for Covenant fire.
    int team = -1;
    if(haloenemykind(attacker, &team) >= 0 && team >= 0 && team < HALO_TEAM_COUNT) return team;
    return haloactorteam(attacker);
}

void halomajorupgradechance(int majorUpgrade, int level, bool &force, bool &random, float &chance)
{
    // ../Halo3DS ai/ai.c:615-650. Without the globals table (an actors.txt older than format 3)
    // the chances are the retail globals' (the same in all ten caches; checked by
    // tests/test_major_upgrades.py against the tags).
    static const float normal[4] = {0.15f, 0.25f, 0.5f, 0.75f}, few[4] = {0.05f, 0.1f, 0.3f, 0.5f},
                       many[4] = {0.25f, 0.5f, 0.75f, 1.0f};
    level = clamp(level, 0, 3);
    force = false;
    random = false;
    chance = 0;
    switch(majorUpgrade)
    {
        case 3: break;                                   // _actor_major_upgrade_none
        case 4: force = true; break;                     // _actor_major_upgrade_all
        case 1: random = true; chance = halodifficultyvalue(HALO_DIFFICULTY_MAJOR_UPGRADE_1, level, few[level]); break;
        case 2: random = true; chance = halodifficultyvalue(HALO_DIFFICULTY_MAJOR_UPGRADE_2, level, many[level]); break;
        default: random = true; chance = halodifficultyvalue(HALO_DIFFICULTY_MAJOR_UPGRADE, level, normal[level]); break;
    }
}

bool haloconsidermajorupgrade(float &globalError, float &squadError, uint32_t &seed, float chance)
{
    // ../Halo3DS ai/ai.c:1875-1893, with real_seed_random (math/random_math.c:211-216) on its own
    // seed: seed = seed * 1664525 + 1013904223 (RANDOM_A, RANDOM_C, :69-70), (seed >> 16) / 65535.
    const float global = globalError * -(1.0f / 3.0f), squad = -squadError;
    const float error = fabsf(global) > fabsf(squad) ? global : squad;
    seed = seed * 1664525u + 1013904223u;
    const float draw = float(seed >> 16) / 65535.0f;
    const bool upgrade = draw < error + chance;
    const float delta = (upgrade ? 1.0f : 0.0f) - chance;
    squadError += delta;
    globalError += delta;
    return upgrade;
}

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

const HaloActorDef *haloactorfind(const char *path)
{
    haloactorsload();
    if(!path || !*path) return nullptr;
    for(const HaloActorDef &d : definitions) if(!strcasecmp(d.path.c_str(), path)) return &d;
    return nullptr;
}

void haloactorattach(botent *bot, const HaloActorDef *def, int team)
{
    if(!bot || !def) return;
    haloactordetach(bot);
    Actor a;
    a.bot = bot;
    a.def = def;
    a.team = team >= 0 && team < HALO_TEAM_COUNT ? team : haloactordefaultteam(def, HALO_TEAM_COVENANT);
    a.hoverHeight = def->type == TYPE_MONITOR ? 1.5f : def->type == TYPE_ENGINEER ? 1.0f : frand(1.5f, 3.5f);
    a.lastKnown = bot->o;
    a.startHealth = bot->health;
    a.homeYaw = bot->yaw;
    a.baseDelay = bot->weaponsel ? bot->weaponsel->info.attackdelay : 0;
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
        if(a.partner == bot) a.partner = nullptr;
    }
    preferred.erase(std::remove(preferred.begin(), preferred.end(), bot), preferred.end());
}

void haloactorsclear() { actors.clear(); noises.clear(); encounterThrows.clear(); }
bool haloisactor(const playerent *p) { return find(p) != nullptr; }

int haloactorteam(const playerent *p)
{
    if(p == player1) return HALO_TEAM_PLAYER;
    const Actor *a = find(p);
    return a ? a->team : HALO_TEAM_COVENANT;
}

bool haloactorhostile(const playerent *a, const playerent *b)
{
    if(!a || !b || a == b) return false;
    return haloteamhostile(haloactorteam(a), haloactorteam(b));
}

// --- teams and allegiances ---------------------------------------------------

int haloteamfromretail(int retail, int fallback)
{
    switch(retail)
    {
        case 1: return HALO_TEAM_PLAYER;
        case 2: return HALO_TEAM_HUMAN;
        case 3: return HALO_TEAM_COVENANT;
        case 4: return HALO_TEAM_FLOOD;
        case 5: return HALO_TEAM_SENTINEL;
        case 6: case 7: case 8: case 9: return HALO_TEAM_OTHER;
        default: return fallback;
    }
}

const char *haloteamname(int team)
{
    static const char *const names[HALO_TEAM_COUNT] = {"covenant", "human", "flood", "sentinel", "player", "other"};
    return team >= 0 && team < HALO_TEAM_COUNT ? names[team] : "none";
}

bool haloteamhostile(int a, int b)
{
    if(a == b) return false;
    const int bit = pairBit(a, b);
    return bit < 0 || !(allianceBits >> bit & 1);
}

bool haloteamfriendly(int team) { return !haloteamhostile(team, HALO_TEAM_PLAYER); }

void haloallegianceset(int a, int b, bool allied)
{
    const int bit = pairBit(a, b);
    if(bit < 0) return;
    if(allied) allianceBits |= 1u << bit;
    else allianceBits &= ~(1u << bit);
    brokenBits &= ~(1u << bit);
    if(a == HALO_TEAM_PLAYER || b == HALO_TEAM_PLAYER) betrayals[a == HALO_TEAM_PLAYER ? b : a] = 0;
}

bool haloallegiancebroken(int a, int b)
{
    const int bit = pairBit(a, b);
    return bit >= 0 && (brokenBits >> bit & 1);
}

// The player's kills of a team's allies counted towards a betrayal since the last reset (QA,
// tests/test_vehicle_damage.py item 9).
int halobetrayalcount(int team) { return team >= 0 && team < HALO_TEAM_COUNT ? betrayals[team] : 0; }

void haloallegiancereset()
{
    allianceBits = savedAllianceBits = defaultAlliances();
    brokenBits = savedBrokenBits = 0;
    loopi(HALO_TEAM_COUNT) betrayals[i] = 0;
}

void haloallegianceget(unsigned &allied, unsigned &broken, bool checkpoint)
{
    allied = checkpoint ? savedAllianceBits : allianceBits;
    broken = checkpoint ? savedBrokenBits : brokenBits;
}

void haloallegianceput(unsigned allied, unsigned broken)
{
    const unsigned mask = (1u << pairBit(HALO_TEAM_COUNT - 2, HALO_TEAM_COUNT - 1) << 1) - 1;
    allianceBits = savedAllianceBits = allied & mask;
    brokenBits = savedBrokenBits = broken & mask;
    loopi(HALO_TEAM_COUNT) betrayals[i] = 0;
}

void haloallegiancecheckpoint() { savedAllianceBits = allianceBits; savedBrokenBits = brokenBits; }

void haloallegiancerevert()
{
    allianceBits = savedAllianceBits;
    brokenBits = savedBrokenBits;
    loopi(HALO_TEAM_COUNT) betrayals[i] = 0;
}

int haloactordefaultteam(const HaloActorDef *def, int fallback)
{
    if(!def) return fallback;
    const int retail = haloteamfromretail(def->team, -1);
    if(retail >= 0) return retail;
    // Format 1 tables: the family decides, as the unit tags' default teams do.
    switch(def->type)
    {
        case TYPE_MARINE: case 8: return HALO_TEAM_HUMAN;                 // marines, crew
        case 9: case TYPE_INFECTION: case 11: return HALO_TEAM_FLOOD;     // combat, infection, carrier forms
        case TYPE_MONITOR: case TYPE_SENTINEL: return HALO_TEAM_SENTINEL;
        default: return def->type >= 0 ? HALO_TEAM_COVENANT : fallback;
    }
}

float HaloActorDef::bodyMultiplier(int materialType) const
{
    for(const Material &m : materials) if(m.type == materialType) return m.body;
    return -1;
}

bool HaloActorDef::hasLeaks() const
{
    if(materials.empty() || !hasResistance) return false;
    for(const Material &m : materials) if(m.leak < 0) return false;
    return true;
}

bool halohovering(const playerent *p)
{
    if(!p || p->state != CS_ALIVE || !haloactive()) return false;
    const Actor *a = find(p);
    return a && a->def->flying();
}

float halomovementscale(const playerent *p)
{
    // ../Halo3DS units/bipeds.c:3580-3590: movement_scale = infection_forms * (biped_index % 137) *
    // 0.00729927 + 1 when the biped definition has _biped_random_speed_increase_bit and the unit's
    // aiming speed is alert (0); the infection swarm sets alert only while it closes on or backs off
    // a combat target (ai/actor_type_infection.c:470-474), casual otherwise (:334). The retail tags
    // set the flag on characters\flood_infection\flood_infection nopop alone (c10). The engine's
    // ordinal is the bot's client number (Halo's is the object datum index). physics.cpp scales
    // the bot's whole horizontal step by it, so a leap or a throw in flight is scaled too, where
    // Halo scales only the movement the biped asks for (judgement call: a bot's velocity in the
    // engine does not keep its own movement apart from momentum). Vertical motion is untouched.
    if(!p || p->type != ENT_BOT || p->state != CS_ALIVE || !haloactive()) return 1;
    const Actor *a = find(p);
    if(!a || !a->def->randomSpeedIncrease() || a->awareness != COMBAT || !alive(a->target)) return 1;
    const float value = halodifficultyget(HALO_DIFFICULTY_INFECTION_FORMS);
    const unsigned ordinal = unsigned(max(0, p->clientnum)) % 137;
    return max(0.05f, value * float(ordinal) * 0.00729927f + 1.0f);
}

bool haloactorpassive(const playerent *p) { const Actor *a = find(p); return a && passive(*a); }
void haloactorscriptdamage(bool on, bool ownerless)
{
    scriptDamage = max(0, scriptDamage + (on ? 1 : -1));
    if(ownerless) ownerlessDamage = max(0, ownerlessDamage + (on ? 1 : -1));
}
bool haloactorownerlessdamage() { return ownerlessDamage > 0; }
const HaloActorDef *haloactorvariant(const playerent *p) { const Actor *a = find(p); return a ? a->def : nullptr; }

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
    a->loopAt = -1;
    a->goal = nullptr;
}

// 0 no list, 1 running, 2 finished or waiting at a loop for the script to advance it.
int haloactorcommandstatus(const playerent *bot)
{
    const Actor *a = find(bot);
    if(!a) return 0;
    return a->commandStatus == 1 && a->loopAt >= 0 ? 2 : a->commandStatus;
}

void haloactorcommandadvance(const playerent *bot)
{
    Actor *a = find(bot);
    if(!a || a->commandStatus != 1) return;
    if(a->loopAt >= 0)
    {
        // Leave the loop: carry on with the atoms after it.
        a->cursor = size_t(a->loopAt) + 1;
        a->loopAt = -1;
    }
    else ++a->cursor;
    a->commandUntil = 0;
    a->goal = nullptr;
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

void haloactorforceawareness(const playerent *bot, bool combat, playerent *target)
{
    Actor *a = find(bot);
    if(!a) return;
    a->awareness = combat && target ? COMBAT : UNAWARE;
    a->target = combat ? target : nullptr;
    if(combat && target) a->lastKnown = target->o;
}

bool haloactorintent(const playerent *bot, HaloActorIntent &intent)
{
    intent = HaloActorIntent();
    const Actor *a = find(bot);
    if(!a || !alive(a->bot)) return false;
    intent.active = !(a->flags & HALO_AI_BRAINDEAD);
    // Difficulty on the seated actor's current team (halocombatscaling): Halo scales a vehicle
    // gunner's firing variant the same way as an infantry one's (actor_combat.c reads the firing
    // variant and the actor's team alike).
    const HaloCombatScaling scaling = halocombatscaling(a->team);
    intent.errorScale = scaling.projectileError;
    intent.separationScale = scaling.burstSeparation;
    intent.rateScale = max(scaling.rateOfFire, 1e-3f);
    intent.error = a->def->error * intent.errorScale;
    // Playing at fighting, as combatValues has it for the actor's own weapon.
    intent.playfight = (a->flags & HALO_AI_PLAYFIGHT) != 0;
    if(intent.playfight)
    {
        intent.error = 2 * intent.error + RAD;
        intent.separationScale *= 1.7f;
    }
    if(!intent.active) return true;
    if(a->awareness == COMBAT && alive(a->target) && haloactorhostile(a->bot, a->target)) intent.target = a->target;
    intent.mayFire = intent.target && BotManager.BotsShoot() && lastmillis >= a->reactUntil && a->morale != PANICKED && !passive(*a);
    // Where it is headed: the command list's current point first, as the brain walks it.
    if(a->program && a->commandStatus == 1 && a->cursor < a->program->commands.size())
    {
        const HaloCommand &c = a->program->commands[a->cursor];
        if((c.type == ATOM_GO_TO || c.type == ATOM_GO_TO_FACE || c.type == ATOM_MOVE_IMMEDIATE) && c.point >= 0 &&
           c.point < int(a->program->points.size()))
        {
            intent.goal = a->program->points[c.point];
            intent.hasGoal = intent.commanded = true;
            return true;
        }
    }
    if(a->hasDestination) { intent.goal = a->destination; intent.hasGoal = true; }
    else if(alive(a->follow)) { intent.goal = a->follow->o; intent.hasGoal = true; }
    else if(a->awareness != UNAWARE) { intent.goal = a->lastKnown; intent.hasGoal = true; }
    return true;
}

void haloactorgoto(const playerent *bot, const vec *point)
{
    Actor *a = find(bot);
    if(!a) return;
    a->boarding = point != nullptr;
    if(point) a->boardAt = *point;
    a->goal = nullptr;
}

float haloactorhealthfraction(const playerent *bot)
{
    if(!bot) return 0;
    if(bot == player1) return bot->health / 75.0f;
    const Actor *a = find(bot);
    return a && a->startHealth > 0 ? float(max(0, bot->health)) / a->startHealth : 0;
}

int haloactorflags(const playerent *bot) { const Actor *a = find(bot); return a ? a->flags : 0; }

float haloactordamagefactor(const playerent *attacker)
{
    // ai_adjust_damage (../Halo3DS ai/ai.c:654-680), which object_cause_damage calls for damage
    // owned by a unit with an actor, its gunner's or its swarm actor's (objects/damage.c:1386-1406),
    // before difficulty and the materials: times 0.3 while that actor plays at fighting, whoever is
    // hit (the player too). Its weapon_damage_modifier (664-669) is the spawn's damageScale here.
    // A dead actor's record is gone, so its grenades still in flight count in full: Halo's dead
    // unit has no actor either (actor_detach_from_unit).
    return haloactorflags(attacker) & HALO_AI_PLAYFIGHT ? .3f : 1.0f;
}

int haloactorgrenadetype(const playerent *bot)
{
    const Actor *a = find(bot);
    if(!a) return -1;
    // The variant's grenade type; the Covenant's plasma or the humans' frag when it names none known.
    if(halogrenadedef(a->def->grenadeType)) return a->def->grenadeType;
    return a->team == HALO_TEAM_HUMAN || a->team == HALO_TEAM_PLAYER ? HALO_GRENADE_FRAG : HALO_GRENADE_PLASMA;
}
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

// unit_set_maximum_vitality (haloenemysetmaximumvitality): a new full health, which health
// fractions, unit_get_health and ai_renew then count against.
void haloactorsetstarthealth(playerent *bot, int health)
{
    Actor *a = find(bot);
    if(!a || !alive(bot) || health <= 0) return;
    a->startHealth = health;
    bot->health = health;
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
    // Damage nobody owns (a vehicle's death explosion, effect_new's jpt! damage: haloareablast with
    // no owner) is dealt in the player's name, because the stock engine needs an attacker; the kill
    // is nobody's, as in Halo, where the blast's owner is the vehicle or NONE and no player: no
    // betrayal, and the dead actor's hostile neighbours are not alerted to the player.
    const bool ownerless = ownerlessDamage && killer == player1;
    if(ownerless) killer = nullptr;
    int alerted = 0;
    const Actor *dead = find(victim);
    const int team = haloactorteam(victim);
    const bool leader = dead && dead->def->type == TYPE_ELITE;
    const float s = scale();
    // Killing allies breaks the player's allegiance with their team (ai_allegiance_broken).
    if(dead && killer == player1 && !scriptDamage && team != HALO_TEAM_PLAYER && !haloteamhostile(team, HALO_TEAM_PLAYER) &&
       ++betrayals[team] >= betrayalKills)
    {
        const int bit = pairBit(team, HALO_TEAM_PLAYER);
        if(bit >= 0)
        {
            allianceBits &= ~(1u << bit);
            brokenBits |= 1u << bit;
            conoutf("Halo: the %s turn on the player", haloteamname(team));
        }
    }
    for(Actor &a : actors)
    {
        // A Hunter whose bond brother falls goes berserk for good.
        if(dead && dead->def->type == TYPE_HUNTER && a.partner == victim && alive(a.bot))
        {
            a.morale = BERSERK;
            a.moraleUntil = INT_MAX;
            a.berserkTried = true;
            a.hiding = false;
            a.positionUntil = 0;
            if(killer && haloactorhostile(a.bot, killer)) { a.target = (playerent *)killer; a.awareness = COMBAT; a.lastKnown = killer->o; a.lastSeen = lastmillis; }
        }
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
            ++alerted;
        }
    }
    // DEATH_TRACE victim=<name> killer=<player|name|-> ownerless=<0|1> alerted=<neighbours alerted to the killer>
    if(dead && halo_damagetrace)
        conoutf("DEATH_TRACE victim=%s killer=%s ownerless=%d alerted=%d", victim->name,
                killer ? (killer == player1 ? "player" : killer->name) : "-", ownerless, alerted);
    haloactordetach(victim);
}

playerent *haloactorhit(const playerent *owner, const vec &from, const vec &to, vec &contact)
{
    playerent *best = nullptr;
    float bestDistance = 1e9f;
    eachCombatant([&](playerent *p)
    {
        if(p == owner || !haloactorhostile(owner, p) || untargetable(find(p))) return;
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
        // ACTOR_FACTION <label> cn= side=<team name> friendly=<0|1> flying=<0|1> passive=<0|1>
        //   cloak=<0..1> partner=<name|-> smashes=<n> altitude=<feet above the floor, AC units>
        conoutf("ACTOR_FACTION %s cn=%d side=%s friendly=%d flying=%d passive=%d cloak=%.2f partner=%s smashes=%d altitude=%.2f",
                label, e->clientnum, haloteamname(a.team), haloteamfriendly(a.team), a.def->flying(), passive(a), haloactorcloak(e),
                a.partner ? a.partner->name : "-", a.smashes, a.def->flying() ? e->o.z - e->eyeheight - a.floorZ : 0.0f);
        vec delta = vec(player1->o).sub(e->o);
        const float yaw = e->yaw * RAD;
        const float angle = acosf(clamp(vec(delta.x, delta.y, 0).normalize().dot(vec(sinf(yaw), -cosf(yaw), 0)), -1.0f, 1.0f)) / RAD;
        conoutf("ACTOR_VIEW %s cn=%d name=%s yaw=%.0f angle=%.0f distance=%.1f range=%.1f visible=%d thinks=%d", label, e->clientnum, e->name, e->yaw,
                angle, delta.magnitude(), a.def->visionDistance * scale(), ::IsVisible(e->o, centreOf(player1), NULL), a.thinks);
    }
    fflush(stdout);
}

void haloactorcombatdump(const char *label)
{
    // Two lines per team (conoutf cuts a line at 260 characters), from halocombatscaling:
    // COMBAT_DIFFICULTY <label> level= table= team=<name> rof= perror= burst_error= new_target=
    //   separation= tracking= leading=
    // COMBAT_DIFFICULTY_MORE <label> team=<name> overcharge= special= grenade_chance=
    //   grenade_timer= melee_scale= melee_base= guidance=
    static const int teams[] = {HALO_TEAM_COVENANT, HALO_TEAM_FLOOD, HALO_TEAM_SENTINEL, HALO_TEAM_HUMAN, HALO_TEAM_PLAYER, -1};
    for(int team : teams)
    {
        const HaloCombatScaling c = halocombatscaling(team);
        const char *name = haloteamname(team);
        conoutf("COMBAT_DIFFICULTY %s level=%d table=%d team=%s rof=%.3f perror=%.3f burst_error=%.3f new_target=%.3f separation=%.3f "
                "tracking=%.3f leading=%.3f", label, c.level, halodifficultytable() ? 1 : 0, name, c.rateOfFire, c.projectileError,
                c.burstError, c.newTargetDelay, c.burstSeparation, c.targetTracking, c.targetLeading);
        conoutf("COMBAT_DIFFICULTY_MORE %s team=%s overcharge=%.3f special=%.3f grenade_chance=%.3f grenade_timer=%.3f melee_scale=%.3f "
                "melee_base=%.3f guidance=%.3f", label, name, c.overchargeChance, c.specialFireDelay, c.grenadeChance, c.grenadeTimer,
                c.meleeDelayScale, c.meleeDelayBase, c.guidanceVsPlayer);
    }
    // ACTOR_COMBAT <label> cn= variant= team= rate=<actv> delay=<ms, 0 weapon's own> slot=<ms now>
    //   error=<actv> scaled_error= lead=<actv> leads=<0|1> separation=<min>,<max> (scaled s)
    //   grenade=<actv chance> grenade_scaled= melee=<ms, 0 not melee-only> smash=<min>,<max> (ms: the
    //   interval meleeInterval gives this actor after a 1.6 and a 2.199 s smash)
    for(const Actor &a : actors)
    {
        const botent *e = a.bot;
        if(!alive(e)) continue;
        const CombatValues v = combatValues(a);
        const HaloActorDef &d = *a.def;
        conoutf("ACTOR_COMBAT %s cn=%d variant=%d team=%s rate=%.3f delay=%d slot=%d error=%.4f scaled_error=%.4f lead=%.3f leads=%d "
                "separation=%.3f,%.3f grenade=%.3f grenade_scaled=%.3f melee=%d smash=%d,%d", label, e->clientnum, d.index,
                haloteamname(a.team), d.rate, v.fireDelay, e->weaponsel ? int(e->weaponsel->info.attackdelay) : -1, d.error, v.error,
                d.lead, v.leads, d.separationMin * v.separation, d.separationMax * v.separation, d.grenadeChance,
                d.grenadeChance * v.scale.grenadeChance, v.meleeDelay, meleeInterval(a, 1600), meleeInterval(a, 2199));
        // ACTOR_TIMERS <label> cn= variant= berserk=<0|1> swarm=<0|1> melee_timer=<actr s, -1 none>
        //   grenade_timeout=<actv s, -1 none> encounter=<index, -1 none> grenade_wait=<scaled ms>
        //   grenade_left=<ms of the encounter's timeout left> playfight=<0|1> burst=<min>,<max> (s, scaled)
        int wait = 0, encounter = -1;
        const int left = grenadeTimeoutLeft(a, v.scale.grenadeTimer, wait, encounter);
        conoutf("ACTOR_TIMERS %s cn=%d variant=%d berserk=%d swarm=%d melee_timer=%.3f grenade_timeout=%.3f encounter=%d "
                "grenade_wait=%d grenade_left=%d playfight=%d burst=%.3f,%.3f", label, e->clientnum, d.index, a.morale == BERSERK ? 1 : 0,
                d.swarm() ? 1 : 0, d.meleeDelayTimer, d.grenadeTimeout, encounter, wait, left, (a.flags & HALO_AI_PLAYFIGHT) ? 1 : 0,
                d.burstMin * v.duration, d.burstMax * v.duration);
    }
    fflush(stdout);
}

void haloactorgrenadenote(const char *label)
{
    // As throwGrenade records a throw: the first living actor with an encounter and a positive
    // timeout starts its encounter's timeout now.
    for(const Actor &a : actors)
    {
        if(!alive(a.bot) || a.def->grenadeTimeout <= 0) continue;
        int wait = 0, encounter = -1;
        grenadeTimeoutLeft(a, halocombatscaling(a.team).grenadeTimer, wait, encounter);
        if(encounter < 0) continue;
        encounterThrows[encounter] = lastmillis;
        conoutf("GRENADE_NOTE %s cn=%d variant=%d encounter=%d timeout=%.3f wait=%d", label, a.bot->clientnum, a.def->index,
                encounter, a.def->grenadeTimeout, wait);
        fflush(stdout);
        return;
    }
    conoutf("GRENADE_NOTE %s none", label);
    fflush(stdout);
}
