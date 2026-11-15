#ifndef HALO_AI_H
#define HALO_AI_H

// Halo-style actor brains for campaign bots (halo_ai.cpp). Definitions come
// from packages/halo/actors.txt, written by tools/halo_actors.py from the
// retail actr and actv tags. Distances are Halo world units, times seconds,
// angles radians; the brain converts with haloscenarioscale().

#include <string>
#include <vector>

class CBot;

// Engine teams. Covenant and human keep their old values (0 and 1: the QA hooks and saved
// squads use them); HALO_TEAM_OTHER stands for the retail teams 6 to 9, hostile to everyone.
// HALO_TEAM_DEFAULT asks the spawn to take the variant's (or the body's) own team.
enum { HALO_TEAM_DEFAULT = -1, HALO_TEAM_COVENANT = 0, HALO_TEAM_HUMAN = 1, HALO_TEAM_FLOOD = 2, HALO_TEAM_SENTINEL = 3,
       HALO_TEAM_PLAYER = 4, HALO_TEAM_OTHER = 5, HALO_TEAM_COUNT = 6 };
// Retail game teams (the scenario's encounter team, the unit tag's default team, the script's
// team literals): 0 default, 1 player, 2 human, 3 covenant, 4 flood, 5 sentinel, 6-9 unused.
int haloteamfromretail(int retail, int fallback = HALO_TEAM_COVENANT);
const char *haloteamname(int team);
enum HaloPosture { HALO_ATTACKING = 0, HALO_DEFENDING, HALO_PURSUING };
// Scripted switches, set by the ai_* commands.
enum { HALO_AI_BLIND = 1, HALO_AI_DEAF = 2, HALO_AI_BRAINDEAD = 4, HALO_AI_PLAYFIGHT = 8, HALO_AI_DISREGARD = 16,
       HALO_AI_MAGIC_SIGHT = 32, HALO_AI_FOLLOW_PLAYER = 64, HALO_AI_FORCE_ACTIVE = 128, HALO_AI_FLEE = 256,
       HALO_AI_NO_DAMAGE = 512, HALO_AI_IMPERVIOUS = 1024, HALO_AI_NO_DROPS = 2048, HALO_AI_NO_DORMANT = 4096,
       HALO_AI_NO_MANEUVER = 8192 };

struct HaloActorDef
{
    int index = -1, kind = -1, type = -1, weapon = -1;
    unsigned actrFlags = 0, actvFlags = 0;
    std::string path, family;
    float visionDistance = 0, visionCentral = 0, visionMax = 0, peripheralAngle = 0, peripheralDistance = 0, hearing = 0;
    float maxRange = 0, rate = 0, error = 0, firstDelayMin = 0, firstDelayMax = 0, combatMin = 0, combatMax = 0, lead = 0, damageScale = 0;
    float burstMin = 0, burstMax = 0, separationMin = 0, separationMax = 0;
    int grenadeType = 0, grenadeMin = 0, grenadeMax = 0;
    float grenadeChance = 0, grenadeRangeMin = 0, grenadeRangeMax = 0;
    float body = 0, shield = 0;
    float hideShield = 0, emergeAttack = 0, emergePursue = 0;
    int casualtyTrigger = 0;
    float friendKilled = 0, leaderKilled = 0;
    float berserkAmount = 0, berserkThreshold = 0, meleeRange = 0;
    float guardMin = 0, guardMax = 0, positionMin = 0, positionMax = 0;
    // actors.txt format 2: the unit's default team (retail numbering, -1 unknown), its melee
    // jpt! (lower, upper min, upper max) and its collision materials.
    int team = -1;
    float unitMelee[3] = {0, 0, 0};
    struct Material { int type; unsigned flags; float body, shield; };
    std::vector<Material> materials;
    bool flying() const { return (actrFlags >> 21) & 1; }
    bool camouflaged() const { return (actvFlags >> 4) & 1; }
    bool superCamouflage() const { return (actvFlags >> 5) & 1; }
    bool meleeOnly() const { return (actvFlags >> 6) & 1; }
    // Body damage multiplier of the first material of a retail material type, or -1 when the
    // collision model has none.
    float bodyMultiplier(int type) const;
};

// Retail material types (the coll materials and the 33 jpt! damage modifiers) the engine uses.
enum { HALO_MATERIAL_HUNTER_ARMOR = 12, HALO_MATERIAL_HUNTER_SKIN = 13, HALO_MATERIAL_ENGINEER_SKIN = 17,
       HALO_MATERIAL_SENTINEL = 25, HALO_MATERIAL_MONITOR = 26, HALO_MATERIAL_HUNTER_SHIELD = 32, HALO_MATERIALS = 33 };

// Allegiances between engine teams (ai_allegiance, ai_allegiance_remove, ai_allegiance_broken).
// Every team is hostile to every other except its own and the allegiances set: the player and
// the humans by default (the missions also set it). Killing allies breaks a breakable allegiance.
bool haloteamhostile(int a, int b);
void haloallegianceset(int a, int b, bool allied);
bool haloallegiancebroken(int a, int b);
void haloallegiancereset();
// The allegiance state as two bit masks over team pairs (for checkpoints and saved games): the
// current one, or with `checkpoint` the copy the last checkpoint keeps for a revert.
void haloallegianceget(unsigned &allied, unsigned &broken, bool checkpoint = false);
void haloallegianceput(unsigned allied, unsigned broken);
void haloallegiancecheckpoint();
void haloallegiancerevert();
// Whether an actor's team, as of now, is on the player's side.
bool haloteamfriendly(int team);
// The engine team a variant spawns on when nothing else decides it.
int haloactordefaultteam(const HaloActorDef *def, int fallback);

// A combat position of an encounter, already in AssaultCube coordinates.
struct HaloFiringPosition { vec point; int group; };

// A scripted command list with its points in AssaultCube coordinates.
struct HaloCommand { int type; float first, second; int point, otherPoint, script; };
struct HaloCommandProgram { unsigned flags = 0; std::vector<HaloCommand> commands; std::vector<vec> points; };

bool haloactorsload();
int haloactordefcount();
const HaloActorDef *haloactordef(int index);
// The variant to use when only an actor family and weapon are known (wave sections, scripts).
const HaloActorDef *haloactordefault(int kind, int weaponIndex);
// The variant whose actv tag path is `path` (case-insensitive), or null: the vehicle variants
// (vehicles.txt `variant` lines: "vehicles\banshee\banshee pilot") an actor seated at a
// vehicle's gun fights as (halo_vehicle.cpp).
const HaloActorDef *haloactorfind(const char *path);

void haloactorattach(botent *bot, const HaloActorDef *def, int team);
void haloactordetach(const playerent *bot);
void haloactorsclear();
bool haloisactor(const playerent *p);
int haloactorteam(const playerent *p);
bool haloactorhostile(const playerent *a, const playerent *b);
// Replaces CBot::MainAI for campaign actors. Returns false for other bots.
bool haloactorthink(CBot *bot);

void haloactorsetpositions(const playerent *bot, const std::vector<HaloFiringPosition> *positions, const unsigned masks[7]);
void haloactorsetposture(const playerent *bot, int posture);
int haloactorposture(const playerent *bot);
void haloactorsetstate(const playerent *bot, int state);
void haloactorsetflag(const playerent *bot, int flag, bool on);
void haloactorcommand(const playerent *bot, const HaloCommandProgram *program);
int haloactorcommandstatus(const playerent *bot);
void haloactorcommandadvance(const playerent *bot);
// ai_status scale: 0 gone, 1 asleep, 2 idle, 4 alert, 6 in combat.
int haloactorstatus(const playerent *bot);
playerent *haloactortarget(const playerent *bot);
// QA (tests/native/damage_qa.inc): puts an actor in combat with `target` (status 6) or unaware of
// everyone (status 2), as its brain would have it, for the melee from behind.
void haloactorforceawareness(const playerent *bot, bool combat, playerent *target);
// A vehicle's AI driver or gunner (halo_vehicle.cpp drives and fires the vehicle for it): whom
// its brain fights (null when nobody: not in combat, or the target is gone), whether it may fire
// now (bots shoot, it has reacted to the target, it is not panicked or passive), where it is
// headed when not fighting (the current command list point, its combat or search position, the
// unit it follows, the last place it knew of a target) and whether it thinks at all (not
// braindead). False for anything that is not an actor.
struct HaloActorIntent
{
    playerent *target = nullptr;
    bool mayFire = false, active = false, hasGoal = false;
    bool commanded = false;     // the goal is a command list's point (reached within 4 world units in a seat)
    vec goal = vec(0, 0, 0);
    float error = 0;            // the variant's firing error (radians)
};
bool haloactorintent(const playerent *bot, HaloActorIntent &intent);
// ai_go_to_vehicle: the actor walks to `point` (a seat's entry point), fighting on the way,
// until it is cleared (null) or boards. Cleared by a revert with the actor.
void haloactorgoto(const playerent *bot, const vec *point);
// Current health over the health the actor spawned with; the player uses 75.
float haloactorhealthfraction(const playerent *bot);
int haloactorflags(const playerent *bot);
// The grenade type the actor throws (and drops when killed), or -1 for anything that is not an actor.
int haloactorgrenadetype(const playerent *bot);
// ai_grenades: whether any actor may throw grenades (on by default; the scenario resets it per section).
void haloaisetgrenades(bool on);
bool haloaigrenades();
// ai_berserk: charge into melee until switched off.
void haloactorberserk(const playerent *bot, bool on);
// ai_follow_target_ai / _disable / ai_follow_distance: stay near `target` (null clears) while nothing is fought.
void haloactorfollow(const playerent *bot, const playerent *target);
void haloactorfollowdistance(const playerent *bot, float distance);
const playerent *haloactorfollowing(const playerent *bot);
// ai_prefer_target: actors weigh preferred units as closer when choosing whom to fight.
void haloactorprefer(const playerent *target, bool on);
// Sets health to a fraction of what the actor spawned with.
void haloactorsethealthfraction(playerent *bot, float fraction);
int haloactorstarthealth(const playerent *bot);
// ai_renew: full health, shield and the actor's grenade allowance.
void haloactorrenew(playerent *bot);

// Damage the scripts deal in the player's name (ai_erase, ai_kill, damage_object) while one of
// these lives: it does not count as the player betraying allies.
void haloactorscriptdamage(bool on);
struct HaloScriptDamage
{
    HaloScriptDamage() { haloactorscriptdamage(true); }
    ~HaloScriptDamage() { haloactorscriptdamage(false); }
    HaloScriptDamage(const HaloScriptDamage &) = delete;
    HaloScriptDamage &operator=(const HaloScriptDamage &) = delete;
};

// Events from the game.
void haloactornoise(const playerent *source);
void haloactordeath(const playerent *victim, const playerent *killer);
// The closest hostile combatant a shot from `owner` meets between from and to.
playerent *haloactorhit(const playerent *owner, const vec &from, const vec &to, vec &contact);

// One console line per actor for the QA client.
void haloactordump(const char *label);

// A living flyer (actr "flying": Sentinels, the Monitor, Engineers): physics.cpp gives it no
// gravity and no ground snap; the brain sets its climb rate in vel.z.
bool halohovering(const playerent *p);
// The Monitor and Engineers: they never fire, throw or fight.
bool haloactorpassive(const playerent *p);
// The variant an actor was spawned as, or null.
const HaloActorDef *haloactorvariant(const playerent *p);

// Callbacks the scenario provides.
void haloscenariowakescript(int script);
float haloenemyshieldfraction(const playerent *p);
// How strongly an actor's active camouflage hides it (0 none, 1 fully cloaked; it weakens while
// the actor fires or is hit). halo.cpp; the renderer draws the actor that translucent.
float haloactorcloak(const playerent *p);

#endif
