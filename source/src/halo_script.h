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
double haloscenarioobjectstate(const char *what, int id);
double haloscenarioqascript(const char *name, int action);
double haloscenarioqaglobal(const char *name, bool set, double value);
void haloscenarioqastate(int &tick, int &started, int &visible);
bool haloscenariocollide(physent *entity, float &hi, float &lo);
// How far below a lift deck top `lo` (world units) a grounded body may step onto it; 0 if `lo` is not one.
float haloscenarioliftboard(physent *entity, float lo);
const char *haloscenarioblockingdevice(physent *entity);
struct HaloCollisionHit;
bool haloscenariotrace(const vec &from, const vec &to, HaloCollisionHit &hit);
// A scripted object's state, for the vehicles bound to it (halo_vehicle.cpp): created by the script,
// and carried by another object or a recording (with its world position and AssaultCube yaw).
bool haloscenarioobjectvisible(int object);
bool haloscenarioobjectcarried(int object, vec &feet, float &yaw);
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
playerent *haloscenariospawnenemy(int kind, const vec &point, float yaw, int actorIndex = -1, int team = 0);
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
