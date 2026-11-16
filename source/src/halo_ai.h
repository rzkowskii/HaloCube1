#ifndef HALO_AI_H
#define HALO_AI_H

// Halo-style actor brains for campaign bots (halo_ai.cpp). Definitions come
// from packages/halo/actors.txt, written by tools/halo_actors.py from the
// retail actr and actv tags. Distances are Halo world units, times seconds,
// angles radians; the brain converts with haloscenarioscale().

#include <cstdint>
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
    // actors.txt format 3: the actv's major upgrade reference (../Halo3DS actor_definitions.h:408),
    // the variant a squad places instead when its major upgrade roll succeeds (encounters.c:3040,
    // actors.c:2687): its path and its index here, -1 when it has none or it is not in the table.
    std::string majorPath;
    int major = -1;
    // Optional format 3 lines (an engine before them skips the keys; a table without them leaves
    // these at their defaults). `unit`: the unit tag's flags (../Halo3DS units/unit_definitions.h:
    // 21-48) and a biped's flags (units/biped_definitions.h:25-37). `spawn`: the actors the unit
    // releases when destroyed (unit_definitions.h:152-155, units.c:1666-1693): the variant's index
    // here (-1 none, or not in the table), the count bounds and the throw velocity in world units a
    // second. `pop`: the body destroyed effect's jpt! when it carries the infection-form pop flag
    // (objects/damage.c:2343-2375): its falloff and cutoff radii (world units), damage bounds and
    // jpt! damage flags; popOuter 0 for none.
    unsigned unitFlags = 0, bipedFlags = 0;
    int spawnVariant = -1, spawnLower = 0, spawnUpper = 0;
    float spawnVelocity = 0;
    float popInner = 0, popOuter = 0, popLower = 0, popUpperMin = 0, popUpperMax = 0;
    unsigned popFlags = 0;
    bool inconsequential() const { return (unitFlags >> 19) & 1; }     // _unit_is_inconsequential_bit
    bool randomSpeedIncrease() const { return (bipedFlags >> 11) & 1; } // _biped_random_speed_increase_bit
    bool pops() const { return popOuter > 0 && ((popFlags >> 12) & 1); } // _damage_infection_form_pop_bit
    bool popHitsOne() const { return (popFlags >> 10) & 1; }          // _damage_does_not_hurt_infection_forms_bit
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
int halobetrayalcount(int team);
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

// The globals (matg) difficulty table (actors.txt format 3 `difficulty` lines, from
// difficulty_information, ../Halo3DS game/game_globals.h:63-99 and 189): the value `type` on
// `level` (0 Easy .. 3 Impossible), or `fallback` when actors.txt has no such line (format 1 or 2).
enum
{
    HALO_DIFFICULTY_ENEMY_DAMAGE = 0,        // _game_difficulty_value_enemy_damage
    HALO_DIFFICULTY_ENEMY_VITALITY = 1,      // _game_difficulty_value_enemy_vitality
    HALO_DIFFICULTY_ENEMY_SHIELD = 2,        // _game_difficulty_value_enemy_shield
    HALO_DIFFICULTY_ENEMY_RECHARGE = 3,      // _game_difficulty_value_enemy_recharge
    HALO_DIFFICULTY_FRIEND_DAMAGE = 4,       // the friend_* values game_difficulty_get_team_value
    HALO_DIFFICULTY_FRIEND_VITALITY = 5,     // answers for the player's side in place of 0 to 3
    HALO_DIFFICULTY_FRIEND_SHIELD = 6,       // (game_globals.c:138-148)
    HALO_DIFFICULTY_FRIEND_RECHARGE = 7,
    // _game_difficulty_value_infection_forms (game_globals.h:73): the speed stagger of bipeds with
    // the random speed increase flag (bipeds.c:3580-3590) and infection forms' toughness to the
    // pop (damage.c:2353-2371). Without the table the engine answers 0, Normal's neutral value.
    HALO_DIFFICULTY_INFECTION_FORMS = 8,
    // The combat values (game_globals.h:75-89), read by halocombatscaling.
    HALO_DIFFICULTY_RATE_OF_FIRE = 10,       // _game_difficulty_value_rate_of_fire
    HALO_DIFFICULTY_PROJECTILE_ERROR = 11,   // _game_difficulty_value_projectile_error
    HALO_DIFFICULTY_BURST_ERROR = 12,        // _game_difficulty_value_burst_error
    HALO_DIFFICULTY_NEW_TARGET_DELAY = 13,   // _game_difficulty_value_new_target_delay
    HALO_DIFFICULTY_BURST_SEPARATION = 14,   // _game_difficulty_value_burst_separation
    HALO_DIFFICULTY_TARGET_TRACKING = 15,    // _game_difficulty_value_target_tracking (added, not multiplied)
    HALO_DIFFICULTY_TARGET_LEADING = 16,     // _game_difficulty_value_target_leading (added)
    HALO_DIFFICULTY_OVERCHARGE_CHANCE = 17,  // _game_difficulty_value_overcharge_chance
    HALO_DIFFICULTY_SPECIAL_FIRE_DELAY = 18, // _game_difficulty_value_special_fire_delay
    HALO_DIFFICULTY_GUIDANCE_VS_PLAYER = 19, // _game_difficulty_value_guidance_vs_player
    HALO_DIFFICULTY_MELEE_DELAY_BASE = 20,   // _game_difficulty_value_melee_delay_base (seconds, added)
    HALO_DIFFICULTY_MELEE_DELAY_SCALE = 21,  // _game_difficulty_value_melee_delay_scale
    HALO_DIFFICULTY_GRENADE_CHANCE_SCALE = 23, // _game_difficulty_value_grenade_chance_scale
    HALO_DIFFICULTY_GRENADE_TIMER_SCALE = 24,  // _game_difficulty_value_grenade_timer_scale
    HALO_DIFFICULTY_MAJOR_UPGRADE = 28,      // _game_difficulty_value_major_upgrade (squads "normal")
    HALO_DIFFICULTY_MAJOR_UPGRADE_1 = 29,    // _game_difficulty_value_major_upgrade_1 ("few")
    HALO_DIFFICULTY_MAJOR_UPGRADE_2 = 30,    // _game_difficulty_value_major_upgrade_2 ("many")
    HALO_DIFFICULTY_VALUES = 35
};
float halodifficultyvalue(int type, int level, float fallback);
bool halodifficultytable();   // actors.txt carried the table

// Halo's difficulty team rules (../Halo3DS objects/damage.c, game/game_globals.c), on the selected
// level (halodifficulty). Teams are engine teams; -1 is no team (ownerless damage, a vehicle
// nobody has sat in: retail team 0 or NONE, hostile to every team).
// game_difficulty_get_value (game_globals.c:198): the table's value, else the engine's constants
// from before the table (enemy damage 0.6/1/1.3/1.7, enemy vitality and shield 0.75/1/1.25/1.5),
// 0 for the values Halo adds rather than multiplies (target_tracking, target_leading and
// melee_delay_base, so the engine's combat stays as it was) and 1 for every other value.
float halodifficultyget(int type);
// game_difficulty_get_team_value (game_globals.c:204-225): `type` for a team hostile to the
// player (or no team); for the player's own team and its allies the matching friend_* value
// (types 0 to 3), any other type at Normal.
float halodifficultyteamvalue(int type, int team);
// The difficulty factor on one hit: damage owned by a team hostile to the player is multiplied by
// enemy_damage whoever it hits (damage.c:1415-1419); on a target whose team is friendly to the
// owner's it is divided back out (damage.c:1603-1612 sets the flag; the shield divides at
// 881-887, the body at 1159-1165). Returns the multiplier over both; `divisor` gets the division
// (enemy_damage when it applies, else 1): a hit that passes a shield is divided by it once more,
// as Halo's body step divides the shield's excess again.
float halodifficultydamagefactor(int ownerTeam, int targetTeam, float *divisor = nullptr);
// The owner team of a hit by `attacker` (null: none, -1; a dead actor: the team it spawned on).
int halodamageteam(const playerent *attacker);

// Halo's difficulty scaling of actor combat on the selected level, for an actor on `team` (engine
// team; -1 none). The team values come from game_difficulty_get_team_value, so a team hostile to
// the player takes the level's value and the player's side Normal's (game_globals.c:204-225); the
// melee and guidance values from game_difficulty_get_value, every team alike. Where each is read
// in ../Halo3DS and what the engine does with it is in docs/macos-build.md (Difficulty, Combat):
// rate_of_fire divides the trigger delay (actor_combat.c:2380-2404), projectile_error multiplies
// the aim error (:1268-1272), burst_separation the pause between bursts (:599-605),
// grenade_chance_scale the throw chance (actions.c:3642-3647), grenade_timer_scale the wait after
// a throw (actions.c:2353-2368), the melee pair the wait between melee attacks (actions.c:2563-2568)
// and guidance_vs_player a homing projectile's turn towards the player (projectiles.c:798-803).
// target_leading only gates the lead (actor_combat.c:2194-2210 adds it to the variant's fraction for
// the test and then leads by the variant's own); burst_error, new_target_delay, target_tracking,
// overcharge_chance and special_fire_delay have no engine counterpart (or, the last two, no effect
// in the port) and are carried for the QA probe only.
struct HaloCombatScaling
{
    int team = -1, level = 1;
    float rateOfFire = 1, projectileError = 1, burstError = 1, newTargetDelay = 1, burstSeparation = 1;
    float targetTracking = 0, targetLeading = 0, overchargeChance = 1, specialFireDelay = 1;
    float grenadeChance = 1, grenadeTimer = 1;
    float meleeDelayScale = 1, meleeDelayBase = 0, guidanceVsPlayer = 1;
};
HaloCombatScaling halocombatscaling(int team);
// A wait between melee attacks of `millis` on the selected level: melee_delay_scale times it plus
// melee_delay_base seconds (actions.c:2563-2568), at least 1 ms.
int halomeleedelay(int millis);
// The factor on a guided projectile's turn rate when its target is the player (projectiles.c:798-803).
float haloguidancevsplayer();
// QA (tests/test_actor_ai.py): COMBAT_DIFFICULTY and _MORE lines per team, then one ACTOR_COMBAT line per
// living actor with the effective values its brain uses.
void haloactorcombatdump(const char *label);

// Halo's major upgrade of a squad's actors at placement (../Halo3DS ai/ai.c:615
// ai_get_major_upgrade_chance): from the squad's major_upgrade enum (0 normal, 1 few, 2 many,
// 3 none, 4 all; anything else normal) on `level`, whether the choice is random and, if so, its
// chance, else whether every actor is upgraded.
void halomajorupgradechance(int majorUpgrade, int level, bool &force, bool &random, float &chance);
// One error-diffusion choice (ai.c:1875 ai_consider_major_upgrade): the larger in magnitude of
// the map-wide error times -1/3 and the squad's error, negated, is added to the chance and a draw
// from Halo's seeded generator (math/random_math.c:211 real_seed_random, the LCG of :69) is
// compared with it; both errors then grow by (upgraded - chance). Returns whether to upgrade.
bool haloconsidermajorupgrade(float &globalError, float &squadError, uint32_t &seed, float chance);

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
    float error = 0;            // the variant's firing error (radians), times projectile_error
    // The actor's difficulty factors (halocombatscaling on its current team) for the vehicle's own
    // firing: on a vehicle variant's error, the pause between bursts and a semi-automatic gun's
    // wait between rounds (divided by rateScale).
    float errorScale = 1, separationScale = 1, rateScale = 1;
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
// these lives: it does not count as the player betraying allies. An ownerless scope (haloareablast
// with no owner: a vehicle's death explosion, effect_new's jpt! damage; and since the final
// integration review of the ownerless-kill batch ai_kill, ai_kill_silent, ai_erase, ai_erase_all,
// damage_object and damage_new, which Halo deals with no owner) also makes its kills nobody's:
// haloactordeath gets no killer, so no hostile neighbour is alerted to the player.
void haloactorscriptdamage(bool on, bool ownerless = false);
struct HaloScriptDamage
{
    explicit HaloScriptDamage(bool ownerless = false) : ownerless(ownerless) { haloactorscriptdamage(true, ownerless); }
    ~HaloScriptDamage() { haloactorscriptdamage(false, ownerless); }
    const bool ownerless;
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
// The factor on a living actor's ground movement from Halo's random speed increase (bipeds.c:
// 3580-3590): for a variant whose biped carries the flag, while it moves at the alert aiming speed
// (in combat with a target: actor_type_infection.c:470-474), infection_forms * (ordinal % 137) *
// 0.00729927 + 1, the ordinal being the bot's client number for Halo's biped datum index. 1 for
// everything else, the player included (physics.cpp applies it to bots only).
float halomovementscale(const playerent *p);
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
