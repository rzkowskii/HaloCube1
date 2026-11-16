#ifndef HALO_SCRIPT_H
#define HALO_SCRIPT_H

#include <string>
#include <vector>

// Selected difficulty (0 Easy to 3 Impossible). Its damage, vitality and recharge scaling by team
// is in halo_ai.h (halodifficultyteamvalue, halodifficultydamagefactor).
int halodifficulty();
void haloscenariostart(int mission, int section, bool preview, bool continuing);
// A player starting profile of the scenario (.hscb version 9, tools/import_halo_scripts.py
// profile_rows): the health and shield modifiers (0 leaves them as they are), each weapon's retail
// tag path ("" none) with its rounds loaded and total, and the grenade counts. Profile 0 is what
// a mission starts the player with (a10's carries no weapon); scripts add others with
// player_add_equipment.
struct HaloStartingProfile
{
    std::string name;
    float health = 0, shield = 0;
    std::string weapon[2];
    int loaded[2] = {}, total[2] = {};
    int frags = 0, plasmas = 0;
};
// The loaded mission script's profile `index` (by name when `name` is given and found); false when
// there is none (no script, or an .hscb older than version 9).
bool haloscenarioprofile(int index, const char *name, HaloStartingProfile &out);
// halo.cpp: puts a profile on the player. `reset` replaces what they carry (an empty slot of the
// profile empties theirs); otherwise its weapons fill empty slots or top up a weapon carried (as
// player_add_equipment with reset false), and its grenades are added up to the maximum.
void haloplayeraddequipment(const HaloStartingProfile &profile, bool reset);
void haloscenariostop();
void haloscenariotick(int elapsed);
void haloscenariorevert();
bool haloscenarioexit();
bool halopelicanready();
bool haloscenariocomplete();
bool haloscenariogamewon();   // the script called game_won (consumed)
bool haloscenariogamelost();  // the script called game_lost (consumed)
bool haloscenariotransition();
bool haloscenariouse();
bool halocartographertarget(vec &point, bool &mapped);
// Center of the authored trigger volume that switches to the next section's BSP.
bool haloscenarioexitpoint(vec &point);
// AssaultCube units per Halo world unit for the current section.
float haloscenarioscale();
// Places every squad of a named encounter; returns how many actors spawned, or -1.
int haloscenarioplace(const char *encounter);
void haloscenarioqapad(int count);
int haloscenarioencountercount();
// QA (tests/test_major_upgrades.py): the living squad members' major upgrades and the diffusion state.
void haloscenarioqamajors();
// QA (tests/test_frontend_progress.py item 16): the living squad members where they stand with
// their vitality (SQUAD_STOOD), and a way to hurt them all (not the attached actors).
void haloscenarioqasquads(const char *label);
void haloscenarioqasquadhurt(float health, float shield);
const char *haloscenarioencountername(int index);
int haloscenarioencounterstarts(int index);
// QA probes: an encounter's script reference ("encounter/squad" a squad's, negative; -1 when not found),
// and a script command called directly.
int haloscenarioencounter(const char *name);
double haloscenariocall(const char *command, double first, double second);
double haloscenariocallv(const char *command, const double *values, const char *const *texts, int count);
int haloscenarioobject(const char *name);
int haloscenarioflag(const char *name);           // a cutscene flag's index, -1 when the section has none by that name
double haloscenarioobjectstate(const char *what, int id);
double haloscenarioqascript(const char *name, int action);
double haloscenarioqaglobal(const char *name, bool set, double value);
void haloscenarioqastate(int &tick, int &started, int &visible);
// QA (tests/test_mission_exits.py): plays a scene the way a script's call does (its Call cue row);
// and a cutscene flag's place in the section's map (feet) and the player's yaw facing its way.
bool haloscenarioqacue(const char *script);
bool haloscenarioflagpoint(const char *flag, vec &feet, float &yaw);
// QA: where to stand `distance` world units before a machine (`side` -1 behind it), facing it.
bool haloscenarioqadevicefront(const char *name, float distance, int side, vec &feet, float &yaw);
bool haloscenariocollide(physent *entity, float &hi, float &lo);
// How far below a lift deck top `lo` (world units) a grounded body may step onto it; 0 if `lo` is not one.
float haloscenarioliftboard(physent *entity, float lo);
const char *haloscenarioblockingdevice(physent *entity);
struct HaloCollisionHit;
bool haloscenariotrace(const vec &from, const vec &to, HaloCollisionHit &hit);
// A player's melee along this segment (world units) opens a machine placed "opened by melee attack"
// that it meets first (a10's melee_door_2): Halo's machine_try_to_open_with_damage. True if it did.
bool haloscenariomeleedevice(const vec &from, const vec &to);
// Damage targets: scripted units with no AI the player hurts by shooting them (d40's manifold
// detectors, which enc5_1_destroyed_poll reads with unit_get_health; tools/halo_cast.py damage_target).
// The first target the segment passes through (its fraction along it in `fraction`), or -1; the
// player's damage to it (false when it cannot take damage: object_cannot_take_damage, destroyed);
// the targets a blast at `centre` within `radius` world units sees, with their distances; and a QA
// probe of a target by name (its box centre in world units, health 0 to 1, whether it takes damage).
int haloscenariotargettrace(const vec &from, const vec &to, float &fraction);
bool haloscenariotargetdamage(int target, float damage);
int haloscenariotargetsnear(const vec &centre, float radius, int *targets, float *distances, int capacity);
bool haloscenariotargetstate(const char *name, vec &centre, float &health, bool &damageable);
// A scripted object's state, for the vehicles bound to it (halo_vehicle.cpp): created by the script,
// and carried by another object or a recording (with its world position and AssaultCube yaw).
bool haloscenarioobjectvisible(int object);
// A vehicle the engine drives for `object` blew up (halo_vehicle.cpp halovehiclehit): the object
// is destroyed for the scripts, as object_destroy does.
void haloscenarioobjectdestroyed(int object);
// `pitch`, when given, gets the carried pose's pitch in degrees (nose up positive): a Banshee on a
// recorded flight climbs and dives with it.
bool haloscenarioobjectcarried(int object, vec &feet, float &yaw, float *pitch = nullptr);
// Dropships and Pelicans (the scenario's vehicles the engine does not drive): actors the scripts
// load into them (vehicle_load_magic, unit_enter_vehicle, ai_go_to_vehicle) ride at their seat
// markers until vehicle_unload sets them down. Whether `unit` rides in one now.
bool haloscenariocarried(const playerent *unit);
// The player boarded one with E (a30's extraction Pelicans, once the script makes them enterable):
// E gets out again. haloscenarioboardprompt is the prompt on foot, or null.
bool haloscenarioboard();
const char *haloscenarioboardprompt();
bool haloscenarioboarded();
// The player is held in a seat by the script (unit_enter_vehicle of the player into a vehicle the
// engine does not drive: a10's cryotube, the insertion Pelicans when a script seats the player
// outside a scene): E does not get out; unit_exit_vehicle (or halo_scriptseatmax) lets them out.
bool haloscenarioscriptseated();
// QA (tests/native/vehicle_qa.inc): one CARGO_QA line per rider or walker and per scripted vehicle
// object that is visible (drawn, boardable, on a recording); and the feet beside the best rider
// seat of `object` for a boarding test.
void haloscenariocargodump(const char *label);
bool haloscenarioqaboardpoint(const char *object, vec &feet);
bool haloscenarioescapetimeout();
bool haloscenarioescapefailed();
bool halocinematicskip();
bool halocinematic();
bool halocinematiccamera(physent &camera);
bool halocinematicground(const vec &point, float &ground);
const char *halocinematicworld();
float halocinematicfov();
float halocinematicnear();
void haloscenariodraw();
const char *haloscenarioobjective();
// The nav points the script shows (activate_nav_point_*), in world coordinates; returns how many
// were written (at most `capacity`). None during a cinematic.
struct HaloHudNav;
int haloscenarionavpoints(HaloHudNav *out, int capacity);
// HUD parts the script hid (hud_show_* false) and, in `blinking`, those it flashes (HALO_HUD_* bits).
int haloscenariohudhidden(int *blinking);
void haloscenariorender();
void haloscenariopausestate(bool paused);
void haloscenariocheckpoint();
void haloscenarioplayersave();
// team: an engine team (halo_ai.h HALO_TEAM_*), or -1 (HALO_TEAM_DEFAULT) for the variant's own.
playerent *haloscenariospawnenemy(int kind, const vec &point, float yaw, int actorIndex = -1, int team = -1);
// Where haloscenariospawnenemy would put that body's feet for `feet` on retail collision, without
// spawning it (a revert's check of where a squad member stood); false with no floor, or off retail collision.
bool haloscenariospawnfloor(int kind, int actorIndex, vec &feet);
// Saved games (halo.cpp, config/halo-progress.txt version 3). haloscenariostatesave appends the
// mission script's state as "s ..." records: what the last checkpoint keeps for a revert, or with
// `live` the state now (a section's start, the script carried in from the section before); false
// when there is none (no checkpoint, a cutscene running). haloscenariostaterestore takes those
// records without their "s " prefix, checks them against the loaded .hscb (names, table sizes,
// node datums) and, with `commit`, puts them in the checkpoint's slots, so that
// haloscenariorevert() resumes there; false with the reason in `why` when they do not fit.
bool haloscenariostatesave(std::string &out, bool live);
bool haloscenariostaterestore(const std::vector<std::string> &records, bool commit, std::string &why);
unsigned haloscenarioprogramhash();               // FNV-1a of the loaded .hscb file, 0 when none
unsigned haloscenariofilehash(int mission);       // of packages/halo/<cache>.hscb on disk, 0 when missing
const char *haloscenarioobjectname(int object);   // a script object's name, "" when none
bool haloscenariobotsuspended(const playerent *bot);
// `bot` is an actor ai_attach made of a scripted object (a50's Keyes and freed marines, c10's
// marine_suicidal): story figures the player's melee from behind does not kill outright (halodamage).
bool haloscenarioattachedactor(const playerent *bot);
// QA probes (tests/native): the actors ai_attach made (ATTACH_QA lines), the script's audio
// (AUDIO_QA: loops, conversations, the loops a Call scene restarts), and a conversation's status.
void haloscenarioattachdump(const char *label);
bool haloscenarioqaattachmove(const char *object, float forward, float side);
void haloscenarioaudiodump(const char *label);
int haloscenarioconversationstatus(const char *name);
void haloscenarioremovebot(const playerent *bot);

#endif
