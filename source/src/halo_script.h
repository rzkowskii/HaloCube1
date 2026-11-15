#ifndef HALO_SCRIPT_H
#define HALO_SCRIPT_H

#include <string>
#include <vector>

// Selected difficulty (0 Easy to 3 Impossible) and its enemy damage and health multipliers.
int halodifficulty();
float halodifficultydamage();
float halodifficultyhealth();
void haloscenariostart(int mission, int section, bool preview, bool continuing);
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
const char *haloscenarioencountername(int index);
int haloscenarioencounterstarts(int index);
// QA probes: an encounter's script reference, and a script command called directly.
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
bool haloscenariocollide(physent *entity, float &hi, float &lo);
// How far below a lift deck top `lo` (world units) a grounded body may step onto it; 0 if `lo` is not one.
float haloscenarioliftboard(physent *entity, float lo);
const char *haloscenarioblockingdevice(physent *entity);
struct HaloCollisionHit;
bool haloscenariotrace(const vec &from, const vec &to, HaloCollisionHit &hit);
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
void haloscenarioremovebot(const playerent *bot);

#endif
