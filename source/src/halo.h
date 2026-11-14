// Offline Halo campaign hooks into the AssaultCube client.
bool haloactive();
void halomapstart(const char *name);
bool halorespawn();
void haloupdate(int elapsed);
void halodrawhud();
void halodamage(int &damage, playerent *target, playerent *attacker, int gun);
bool halopickup(int index, playerent *player);
const char *haloactormodel(const playerent *player);
const char *halohudmodel(int gun);
void haloupdatecounter(int rounds);
Texture *halocountertexture(const char *mesh, Texture *fallback);
void haloremoveactor(const playerent *player);
void halofrag();
void halomelee();
void haloupdatebolts(int elapsed);
void haloclearbolts();
void haloaddbolt(playerent *owner, const vec &position, const vec &direction, int damage);
void haloplaysound(const char *sample, int fallback, physent *actor = NULL, const vec *position = NULL);
bool halodriving();
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
