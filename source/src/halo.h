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
// Exact damage (weapon.cpp): deals `damage` (not rounded) with a jpt! side effect through hit();
// halodamage applies the exact amount (shields, difficulty, the health fraction carried to the
// next hit) and the side effect (3, EMP, drains the shield). halopendinghit is its hand-off.
// `source` is the jpt! damage dealt, whose material modifiers halodamage applies to the bodies
// with retail collision materials (Hunters, Sentinels, the Monitor, Engineers); null for none.
struct HaloDamage;
// `at`, when given, is where the hit struck the body (shots and the player's melee): the Hunter's
// head and belly and the general material model's head go by its height.
void halohit(float damage, int sideEffect, playerent *victim, playerent *owner, const vec &dir, int gun, bool gib, int info,
             const HaloDamage *source = NULL, const vec *at = NULL);
void halopendinghit(const playerent *target, float damage, int sideEffect, const HaloDamage *source = NULL, const vec *at = NULL);
// A jpt! blast that is not a weapon's (weapon.cpp): full strength inside `inner`, falling to the
// lower bound at `outer` (world units, scaled to the section), none beyond; bodies need the line
// from the centre to their middle clear unless inside the jpt! AOE core radius, as for grenades;
// riders the hull covers are left to the hull (halovehicleridercovered); the vehicles it reaches
// take it with the tag's material modifier. `owner` deals it (null: nobody, dealt in the player's
// name inside an ownerless script-damage scope: no betrayal, its kills reach haloactordeath with no
// killer; the player's own hurt as self-inflicted).
void haloareablast(const vec &centre, const HaloDamage &damage, float inner, float outer, int sideEffect, playerent *owner);
// Effect `tag`'s jpt! area damage (effects.txt `damage` line, halofxeffectdamage) at `centre`
// through haloareablast: a vehicle's death explosion (halo_vehicle.cpp) and, with
// halo_effectdamage 1, the scripts' effect_new. False when the table gives the effect no damage.
bool haloeffectblast(const char *tag, const vec &centre, playerent *owner);
// An actor fired a weapon trigger (halofiretrigger): the Hunter's belly window (halodamage) opens
// from its own cannon fire, not from its smash.
void haloactorfired(const playerent *p);
// QA (tests/test_vehicle_damage.py): a grenade of `type` thrown by `owner` goes off at `at` at once.
bool haloqagrenadeat(int type, const vec &at, playerent *owner);
// Opacity a camouflaged actor is drawn with (rendermodel.cpp), or a negative value for a body
// drawn as usual.
float halomodelalpha(const playerent *p);
// An actor's damage modifier on its weapon's jpt! damage (the variant's, else the default share).
float haloactordamagescale(const playerent *p);
bool halopickup(int index, playerent *player);
const char *haloactormodel(const playerent *player);
// `name` when that model loads, else `fallback` (tried once per name).
const char *halomodelor(const char *name, const char *fallback);
const char *halohudmodel(int gun);
// The current viewmodel's placement away from its retail animation's (right, forward, up in the
// view; AssaultCube units), when it has one (the rocket launcher).
bool halohudmodeloffset(int gun, vec &offset);
void haloupdatecounter(int rounds);
Texture *halocountertexture(const char *mesh, Texture *fallback);
void haloremoveactor(const playerent *player);
void halofrag();
void halomelee();
// QA (tests/native/damage_qa.inc): `count` of the player's needles strike `victim` at once, each
// as a needle that hit it (its damage, then the stuck count of the supercombine); the number of
// super detonations they set off, or -1 without a weapon whose projectile super-detonates.
int haloqaneedles(playerent *victim, int count);
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
// A decoded tag sample looped from the camera while `active` (a weapon's firing loop); one at a time.
void haloloopsound(const char *sample, bool active);
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
// A script moving the player (object_teleport, volume_teleport_players_not_inside, a scene's
// handoff): to `feet` facing `yaw` (or, with no feet, only turned to it) when the body fits on a
// floor there (retail collision: standing room on the mesh; stock geometry: clear of the cubes).
// False, and the player left where they are, otherwise or while seated, dead or in the ending.
bool haloplayerplace(const vec *feet, float yaw, const char *why);
// The floor under `from` the player's body fits on (`ground`: the feet there), straight down the
// retail mesh or the cube column; false when there is none (the scripted seat's guard).
bool haloplayerfloorbelow(const vec &from, vec &ground);
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
// Removes an actor from the world at once, with no death, body or drops (object_destroy of a
// scripted biped that ai_attach turned into an actor, halo_script.cpp).
void haloerasebot(playerent *p);
// An actor's body kind (halo.cpp ActorKind) and team, or -1 for anything else.
int haloenemykind(const playerent *p, int *team = NULL);

// Tag-driven weapons (halo.cpp loadout, weapon.cpp firing and projectiles).
struct HaloWeaponDef;
struct HaloWeaponState;
// The retail weapon behind an AssaultCube gun slot for the player or an enemy.
const HaloWeaponDef *haloweaponof(const playerent *p, int gun, HaloWeaponState **state = NULL);
bool halocarriesgun(int gun);
// The player holds no weapon (a slot left empty by a starting profile: a10 before the bridge): the
// fire button does nothing and no viewmodel or fallback reticle is drawn; melee still strikes.
bool haloplayerunarmed();
void haloswitchweapon();
void haloattackinput(bool down);
float halozoom();
void halounzoom();
int halomeleedamage();
// The enemy the weapon's aim assist locks onto along a direction, if any.
playerent *haloaimtarget(const vec &from, const vec &direction, const HaloWeaponDef &def);
// `modifier`: the damage modifier an actor's shots carry instead of its own variant's (a vehicle
// gun's variant, halo_vehicle.cpp); -1 for the shooter's own (1 for the player).
void halofiretrigger(playerent *owner, const HaloWeaponDef &def, HaloWeaponState &state, int trigger,
                     const vec &from, const vec &to, float modifier = -1);
void halokilled(playerent *victim, playerent *killer);
// The nearest marine or Elite body an infection form could reach within `range`.
void haloclearflood();
bool halonearestcorpse(const vec &from, float range, vec &body);
void halorenderweapons();
