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
// What halodamage made of its last hit, for the unit's pain or death line (halo_dialogue.cpp): whether
// the shield (or overshield) took damage, whether it was a headshot (Halo's killed instantly), the
// jpt! category (-1 not known) and its instantaneous acceleration. True for the target of a hit this
// frame.
struct HaloHitReport { bool shield = false, instant = false; int category = -1; float acceleration = 0; };
bool halohitreport(const playerent *target, HaloHitReport &out);
// Falling damage (halo.cpp updateFalling): physics.cpp's retailmove reports each surface an
// airborne player or actor touches (`floor`: one it can stand on), read only to project the
// Halo-equivalent fall velocity along a steep face it slides down and onto the floor it lands on.
void halofallcontact(const physent *d, const vec &normal, bool floor);
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
// Whether a hit or kill dealt in `attacker`'s name is nobody's: the player's name inside an
// ownerless script-damage scope (halo_ai.h). clientgame.cpp's dodamage and dokill and weapon.cpp's
// hit() then credit nobody: no hit sound, frag or team kill, no "you ..." kill message (the
// player's own death reads "you died", an actor's goes to the console), and -1 as the actor of the
// onHit and onKill hooks.
bool haloownerless(const playerent *attacker);
// The engine's and the scripts' forced death of the player (game_lost, game_revert, the escape
// deadline, a recovery with no safe floor): an INT_MAX kill inside an ownerless scope, so it reads
// "you died", not a suicide, and costs no frag.
void halokillplayer();
// Effect `tag`'s jpt! area damage (effects.txt `damage` line, halofxeffectdamage) at `centre`
// through haloareablast: a vehicle's death explosion (halo_vehicle.cpp) and, unless
// halo_effectdamage 0, the scripts' effect_new. False when the table gives the effect no damage.
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
// Halo's actor danger zones (halo_ai.cpp): the live projectiles whose proj tag has a danger radius
// (equipment.txt's and weapons.txt's `danger` lines): retail grenades in flight, at rest or stuck,
// and shots in flight (the Wraith mortar). `id` stays the same while the object lives; position and
// velocity are AssaultCube units (a second); radii world units; `owner` is null when it is gone;
// `stuckTo` the unit a grenade is attached to; `attached` it is stuck to a unit or a vehicle;
// `countingDown` its detonation timer runs, with `millisLeft` to detonation (-1 when it does not).
struct HaloDangerObject
{
    int id = -1;
    vec position = vec(0, 0, 0), velocity = vec(0, 0, 0);
    float dangerRadius = 0, perceptionRadius = 0;
    const playerent *owner = nullptr, *stuckTo = nullptr;
    bool attached = false, countingDown = false;
    int millisLeft = -1;
};
int halodangerobjects(HaloDangerObject *out, int capacity);
// QA (tests/test_actor_ai.py item 12): a live grenade of `type` owned by `owner` at rest at `at`, its
// timer started as if it had bounced, stuck to `stick` when that is given. False without the table.
bool haloqagrenadeplace(int type, const vec &at, playerent *owner, playerent *stick);
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
// unit_set_current_vitality and unit_set_maximum_vitality on an actor, in Halo's points against
// its stored maxima (halo.cpp); the first returns true when the body is set to nothing (the
// caller kills it). The maxima a script set (false: the variant's own), and those of a variant
// not spawned yet.
bool haloenemysetvitality(playerent *p, float body, float shield);
void haloenemysetmaximumvitality(playerent *p, float body, float shield);
bool haloenemyscriptmaximum(const playerent *p, float &body, float &shield);
void haloenemydefaultvitality(int kind, int variant, float &body, float &shield);
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
