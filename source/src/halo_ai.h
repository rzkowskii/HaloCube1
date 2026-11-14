#ifndef HALO_AI_H
#define HALO_AI_H

// Halo-style actor brains for campaign bots (halo_ai.cpp). Definitions come
// from packages/halo/actors.txt, written by tools/halo_actors.py from the
// retail actr and actv tags. Distances are Halo world units, times seconds,
// angles radians; the brain converts with haloscenarioscale().

#include <string>
#include <vector>

class CBot;

enum { HALO_TEAM_COVENANT = 0, HALO_TEAM_HUMAN = 1 };
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
};

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
// Current health over the health the actor spawned with; the player uses 75.
float haloactorhealthfraction(const playerent *bot);
int haloactorflags(const playerent *bot);
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

// Events from the game.
void haloactornoise(const playerent *source);
void haloactordeath(const playerent *victim, const playerent *killer);
// The closest hostile combatant a shot from `owner` meets between from and to.
playerent *haloactorhit(const playerent *owner, const vec &from, const vec &to, vec &contact);

// One console line per actor for the QA client.
void haloactordump(const char *label);

// Callbacks the scenario provides.
void haloscenariowakescript(int script);
float haloenemyshieldfraction(const playerent *p);

#endif
