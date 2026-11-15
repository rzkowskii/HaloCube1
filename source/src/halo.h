// Offline Halo campaign hooks into the AssaultCube client.
bool haloactive();
void halomapstart(const char *name);
bool halorespawn();
void haloupdate(int elapsed);
void halodrawhud();
// The title screen behind the title menu (halo_title): whether it shows, and drawing it in the 2D HUD pass.
bool halotitleactive();
void halodrawtitle();
void halodamage(int &damage, playerent *target, playerent *attacker, int gun);
bool halopickup(int index, playerent *player);
const char *haloactormodel(const playerent *player);
const char *halohudmodel(int gun);
void haloupdatecounter(int rounds);
Texture *halocountertexture(const char *mesh, Texture *fallback);
void haloremoveactor(const playerent *player);
void halofrag();
void halomelee();
// Retail grenades (weapon.cpp, from packages/halo/equipment.txt): thrown by the player and the
// actors; plasma grenades stick to units and vehicles. False without the table.
bool halothrowgrenade(playerent *owner, int type, const vec &velocity);
void halogrenadesupdate();
void halogrenadeforget(const playerent *unit);        // a unit is gone: grenades stuck to it fall
int halogrenadedump(const char *label);               // one console line per grenade (QA); the count
// The player's grenades (halo.cpp): count of a type (-1 all types), the selected type, and
// taking one to throw (the selected type, or the other when it has run out; -1 when none).
int haloplayergrenades(int type);
int haloplayergrenadetype();
int haloplayerusegrenade();
// Active camouflage on the player: 0 off, 1 fully cloaked (halo_ai.cpp: enemies lose track and miss).
float haloplayercamo();
// Scenario equipment the mission script creates (object_create) or removes, by .hscb object
// index; after the script started or resumed, equipment it already created is placed.
void haloequipmentobject(int object, bool exists, bool reset);
void haloequipmentsync();
void haloupdatebolts(int elapsed);
void haloclearbolts();
void haloaddbolt(playerent *owner, const vec &position, const vec &direction, int damage);
void haloplaysound(const char *sample, int fallback, physent *actor = NULL, const vec *position = NULL);
bool halodriving();
// Retail collision: the player on foot back on the newest of the last floors they stood on that is
// still clear (logged with `why`), as the fall recovery does; false when there is none.
bool halorecoverplayer(const char *why);
// Every move of the player that is not walking (a spawn or revert, Continue, a section start, a
// cutscene that puts them somewhere, getting into or out of a vehicle seat, a QA teleport) goes
// through here: the recovery above forgets the floors recorded before the move, restarts its wedge
// and stall timers and gives the body a second before the wedge check may nudge it. `kept`, when
// given, is a floor to keep as the one safe spot (where the player got into a seat from).
void haloplayermoved(const char *why, const vec *kept = NULL);
// FNV-1a of a file's bytes (the progress file's check), 0 when it cannot be read.
unsigned halofilehash(const char *path);
int haloescapemillis();
float haloplayershieldfraction();
// Script-driven unit state: vitality, damage immunity, renewal and teleports.
void haloplayersetvitality(float health, float shield, bool maximum);
void haloplayerprotect(bool on);
bool haloplayerprotected();
void haloenemysetshield(const playerent *p, float fraction);
void haloenemyrenew(playerent *p, int health, int grenades);
bool haloenemymove(playerent *p, const vec &point);
void halorendervehicle();
// An actor's body kind (halo.cpp ActorKind) and team, or -1 for anything else.
int haloenemykind(const playerent *p, int *team = NULL);

// Tag-driven weapons (halo.cpp loadout, weapon.cpp firing and projectiles).
struct HaloWeaponDef;
struct HaloWeaponState;
// The retail weapon behind an AssaultCube gun slot for the player or an enemy.
const HaloWeaponDef *haloweaponof(const playerent *p, int gun, HaloWeaponState **state = NULL);
bool halocarriesgun(int gun);
void haloswitchweapon();
void haloattackinput(bool down);
float halozoom();
void halounzoom();
int halomeleedamage();
// The enemy the weapon's aim assist locks onto along a direction, if any.
playerent *haloaimtarget(const vec &from, const vec &direction, const HaloWeaponDef &def);
void halofiretrigger(playerent *owner, const HaloWeaponDef &def, HaloWeaponState &state, int trigger,
                     const vec &from, const vec &to);
void halokilled(playerent *victim, playerent *killer);
// The nearest marine or Elite body an infection form could reach within `range`.
void haloclearflood();
bool halonearestcorpse(const vec &from, float range, vec &body);
void halorenderweapons();
