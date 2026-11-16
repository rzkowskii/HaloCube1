#ifndef HALO_VEHICLE_H
#define HALO_VEHICLE_H

#include <string>
#include <vector>

// Campaign vehicles (halo_vehicle.cpp): the Warthog (a car), fixed gun turrets with a gunner
// seat, the Ghost and the Wraith (hover craft), the Scorpion (a tank) and the Banshee (a flyer).
// Definitions come from packages/halo/vehicles.txt (tools/halo_vehicles.py); the section maps
// place them as PLAYERSTART entities with team 102 (tools/import_halo_campaign.py). Sizes and
// speeds are the retail vehicle's in Halo units, scaled to the section (haloscenarioscale());
// the handling is plausibly Halo, not the retail physics. The player drives all but the Wraith;
// actors drive and fly them when a script seats them at the controls (AI drivers below).

// The values of the first three are kept (saved progress and the HUD use them).
enum HaloVehicleClass { HALO_VEHICLE_CAR, HALO_VEHICLE_TURRET, HALO_VEHICLE_PROP, HALO_VEHICLE_HOVER, HALO_VEHICLE_TANK,
                        HALO_VEHICLE_FLYER };
enum HaloSeatRole { HALO_SEAT_DRIVER, HALO_SEAT_GUNNER, HALO_SEAT_PASSENGER };

// Map lifetime: clear on map start, add each placement entity, then restart once the section
// (or a revert) begins. `checkpoint` restores the state halovehiclescheckpoint() saved.
void halovehiclesclear();
// Jump governors (halo_vehiclegovern lines of a map .cfg): resetmap drops the ones a .cfg left
// pending, before the next map's .cfg runs; startmap makes the pending ones the map's (every load
// path, the edit-mode xmap restore included), replacing the last map's.
void halovehiclegovernorsreset();
void halovehiclegovernorsadopt();
void halovehicleplace(const entity &e);
void halovehiclesrestart(bool checkpoint);
void halovehiclescheckpoint();
// After the mission script started (or resumed) on this section: script-created vehicles that
// the script has already created exist.
void halovehiclessync();
// Saved games (halo.cpp, config/halo-progress.txt version 3): the vehicles the last checkpoint
// keeps, as "v ..." records; and back into that checkpoint from the records (without their
// "v " prefix), so that halovehiclesrestart(true) puts them there, the player in their seat. False,
// with the reason in `why` and the checkpoint untouched, when the records do not fit the section.
void halovehiclesstatesave(std::string &out);
bool halovehiclesstaterestore(const std::vector<std::string> &records, std::string &why);

// The player. halodriving() is true while the player is in any seat.
bool halovehicleseated();
bool halovehicleuse(float weaponDistance);             // E: exit, flip a vehicle back, or get in
const char *halovehicleprompt(float weaponDistance);   // the E prompt on foot, or null
void halovehicledrive(int curtime);                    // movelocalplayer while seated
void halovehiclesupdate(int elapsed);                  // haloupdate: other vehicles, riders, AI gunners
void halovehiclessounds();                             // the engine loops (halovehiclesupdate; stops them while paused)
bool halovehiclecamera(physent &camera);
float halovehicledamage(float damage);                 // what reaches a seated player
// EMP (jpt! side effect 3: a plasma grenade's blast, the charged plasma bolt): vehicles whose body
// lies within `radius` of `centre`, and vehicle `stuck` (one a plasma grenade is stuck to, or -1),
// are stunned for `millis`: a car's engine cuts out (it coasts and steers, the throttle does
// nothing) and the seat's gun cannot fire, the player's or an actor gunner's. Returns how many.
int halovehicleemp(const vec &centre, float radius, int stuck, int millis);
bool halovehicleriderstunned(const playerent *unit);    // `unit` sits in a stunned vehicle's gun seat
// Vehicle damage (vehicles.txt `vitality`, the retail collision model's body vitality and flags).
// Only vehicles with a body vitality stop shots and take damage: the Wraith, and the Ghost and the
// Banshee while someone rides them (their coll's "only damaged while occupied"). The Warthog, the
// Scorpion and the turret have none (indestructible in Halo) and shots pass through them as before.
// halovehicletrace: the first such vehicle whose body box (turned by its yaw and tilted by its pitch
// and roll, as it is drawn; a flipped one level) the segment passes through,
// with the fraction along it, or -1; `shooter`'s own vehicle is left out, and for an actor shooter
// so is any vehicle no one hostile to it rides (its shots pass friends, as haloactorhit). `met` is the body the
// shot reached at `to`, or null: a rider of that vehicle in an open seat (the Ghost's) takes the
// shot itself (-1); one the hull covers does not, and the vehicle takes it: a rider in an invisible
// seat (seat flag 1: the Wraith's driver) or of a vehicle whose coll "takes body damage for its
// children" (flag 2: the Banshee's pilot, the player included).
struct HaloDamage;
int halovehicletrace(const vec &from, const vec &to, float &fraction, const playerent *shooter, const playerent *met);
// `unit` rides a vehicle with a body vitality in a seat its hull covers (the rule above: the
// Wraith's driver, the Banshee's pilot, the player included): a blast (areadamage, grenades,
// haloareablast) leaves it alone and the hull takes the blast instead, so a rider is not hurt twice
// (in its own right and through the hull) and dies with the vehicle. The Ghost's open seat is not
// covered. False with vehicle damage off (halo_vehicledamage 0).
bool halovehicleridercovered(const playerent *unit);
// The vehicles with a body vitality a blast at `centre` reaches within `radius` (AssaultCube
// units, to the nearest point of the box), with those distances; returns how many.
int halovehiclesnear(const vec &centre, float radius, int *vehicles, float *distances, int capacity);
// `damage` (jpt! points, the hit's falloff applied) from `attacker` to vehicle `index`: times the
// jpt! modifier for the vehicle's material (`source`, may be null) and its body multiplier, over
// its body vitality. `explosive`: area damage (a vehicle "only damaged by explosives" takes only
// that). Enemy fire on the player's vehicle is scaled by the difficulty as on the player; damage
// to any other vehicle by the difficulty's enemy health. At no health it explodes (its body
// depleted effect), its riders die, the effect's jpt! area damage strikes round it
// (halo_vehicleexplosion, haloeffectblast), it is gone and so is the scripts' object
// (unit_get_health -1); a grounded one leaves its wreck, a box bodies collide with, when
// halo_vehiclewreck is on (off by default).
// Returns the damage taken (0: none, for an indestructible, empty or protected vehicle).
float halovehiclehit(int index, float damage, const HaloDamage *source, bool explosive, playerent *attacker);
// unit_get_health of scenario object `object`'s vehicle: its body health 0..1 (1 for one with no
// vitality), -1 when it does not exist (destroyed, or not created yet).
float halovehiclehealth(int object);
// The middle of vehicle `vehicle`'s body box (a grenade's line to it), false when it does not exist.
bool halovehiclecentre(int vehicle, vec &centre);
// The secondary trigger while seated (halo_zoom, Mouse 2 or Z): the second trigger of the seat's
// gun, the Banshee's fuel rod and the Scorpion's machine gun.
void halovehiclesecondary(bool down);
// The jump and crouch keys held (physics.cpp jumpn, crouch): a piloted Banshee climbs and descends.
void halovehiclejump(bool down);
void halovehiclecrouch(bool down);
// The Maw: seat the player in a Warthog at a section start (an unused one nearby, or a new one).
bool halovehicleescape(const vec &feet, float yaw);
// The Maw on foot: a free Warthog within `radius` of `feet`, or a new one beside them.
void halovehicleensure(const vec &feet, float yaw, float radius);
// Flown sections (c40 sections 1 and 4): a vehicle of vehicles.txt type `name` the player may
// take, anywhere in the section (standing, or placed and not created by the script yet), else a
// new one beside the player. Returns the vehicle, or -1.
int halovehicleensuretype(const char *name, const vec &feet, float yaw);
// halo.cpp's recovery found no floor for the player on foot: if they were thrown out of a vehicle
// with no room for the body in the last 15 s, back into its seat (the vehicle righted, or moved to
// the nearest spot where it fits or its last safe spot). False otherwise.
bool halovehiclereclaim(const char *why);

struct HaloVehicleStatus
{
    const char *name = "";
    int vehicleClass = HALO_VEHICLE_CAR, role = HALO_SEAT_DRIVER;
    float speed = 0;          // Halo world units per second
    float governed = 0;       // a jump governor holds the car at this speed (Halo units/s; 0: none)
    float altitude = -1;      // a flying Banshee's height over the floor below, Halo units (-1: none in reach)
    bool flipped = false, airborne = false;
    bool driverFires = false; // the driver works the gun (Ghost, Banshee, Scorpion)
    bool secondary = false;   // the gun has a second trigger (fuel rod, machine gun)
    int weapon = -1;          // packages/halo/weapons.txt index of the seat's gun, or -1
    float heat = 0;
    bool overheated = false, firing = false;
};
bool halovehiclestatus(HaloVehicleStatus &status);

// Rendering (inside the model batch) and the placeholder boxes for vehicles without a model.
void halovehiclesrender();
void halovehiclesrenderplaceholders();

// Bodies collide with vehicles (physics.cpp collidedynamic); `hi`/`lo` as mmcollide. A body left
// inside a vehicle is pushed out of it each frame (halovehiclesupdate); a moving vehicle shoves
// the bodies it runs into, or stops against them.
bool halovehiclecollide(physent *d, float &hi, float &lo);
// Sticky grenades (weapon.cpp): the vehicle whose body holds `point` (within `reach`), or -1;
// a point in that vehicle's frame (yaw only) and back to the world while the vehicle exists.
int halovehicleat(const vec &point, float reach);
bool halovehicleattach(int index, const vec &point, vec &local);
bool halovehicleattached(int index, const vec &local, vec &point);

// Mission script hooks. `object` is the .hscb object index (the scenario object name index).
bool halovehiclebound(int object);                      // an engine vehicle stands for it here
void halovehicleobject(int object, bool exists, bool reset, bool bulk);
void halovehicleteleport(int object, const vec &feet, float yaw);
bool halovehicleseattest(int object, const char *seat, const playerent *unit);
bool halovehicleload(int object, const char *seat, playerent *unit);
int halovehicleunload(int object, const char *seat);
// The player in `seat` of the Warthog the Maw's escape put them in, for the scripts' seat test of
// scenario Warthog `object` (d40's trench_jeep_test). One the engine spawned (no scenario object)
// stands in for every scenario Warthog; a scenario Warthog the escape took over (trench_jeep1) is
// itself and stands in for those not standing in this section.
bool halovehicleescapeseat(int object, const char *seat, const playerent *unit);
bool halovehicleunseat(const playerent *unit);         // ai_exit_vehicle, unit_exit_vehicle
bool halovehicleinside(const playerent *unit);
// `unit` (an actor) sits in a seat that works the vehicle's gun: the driver of a Ghost, Wraith,
// Scorpion or Banshee, a Warthog's gunner, a turret's gunner. The vehicle fires for it, aimed by
// its brain's target (halo_ai.cpp keeps the actor's own weapon quiet).
bool halovehiclearmedseat(const playerent *unit);
// Whoever sits in the driver's seat of the vehicle standing for scenario object `object`, or null.
playerent *halovehicledriverof(int object);
// ai_going_to_vehicle: actors still walking to a seat of the vehicle for `object`.
int halovehicleaigoing(int object);
void halovehicleenterable(int object, bool player);
void halovehicleaiboard(int object, float distance, int actorKind, bool disabled);
bool halovehicleaigoto(int object, const char *seat, playerent *const *units, int count);
void halovehicleforget(const playerent *unit);         // an actor is gone

// QA (tests/native/vehicle_qa.inc).
int halovehiclecount();
int halovehicledefcount();                              // vehicles.txt records (1: the built-in Warthog)
int halovehicleengineloops();                           // engine loops playing now (presentation)
void halovehicledump(const char *label);
// QA: damage vehicle `vehicle` as the player would (no jpt! source).
float halovehicleqadamage(int vehicle, float amount, bool explosive);
// QA (tests/test_vehicle_damage.py): the unit at vehicle `vehicle`'s controls, or null; the point
// `side` units out past the right side of its body box at the box's middle height; its pitch and
// roll set (and held: asleep) for the tilted hit box; where a level segment across it from side to
// side, `along` units forward of its floor point and `up` above it, enters its hit box (`fraction`,
// -1 for a miss), with the level box's back, front, half width and top (AssaultCube units, from the
// floor point); and how many wrecks the section holds (halo_vehiclewreck).
playerent *halovehicleqadriver(int vehicle);
bool halovehicleqabeside(int vehicle, float side, vec &point);
bool halovehicleqatilt(int vehicle, float pitch, float roll);
bool halovehicleqaprobe(int vehicle, float along, float up, float &fraction, float box[4]);
int halovehicleqawrecks();
bool halovehicleqawreckat(int vehicle, vec &centre);   // the middle of vehicle `vehicle`'s wreck box
// QA vehicle walker (tests/native/retail_qa.inc): the player's vehicle's position (its floor point
// or a flyer's origin, as the importer's vehicle rows give it) and yaw; and the existing vehicle of
// vehicles.txt type `name` nearest `near` whose driver's seat is free, with that seat's entry point.
bool halovehicleqapose(vec &pos, float &yaw);
int halovehicleqafind(const char *name, const vec &near, vec &entry);
void halovehicleqainput(float throttle, float steer, int millis);
// Flight input for a piloted Banshee: throttle, strafe and climb (-1..1) held for `millis`; the
// camera's yaw and pitch still aim the nose.
void halovehicleqaflight(float throttle, float strafe, float climb, int millis);
// Seats actor `unit` at the controls of vehicle `vehicle` (its driver's seat, else `seat` by name).
bool halovehicleqapilot(int vehicle, playerent *unit, const char *seat);
bool halovehicleqaenter(int vehicle, int role);
bool halovehicleqaflip(int vehicle);
int halovehicleqaspawn(int type, float forward);
bool halovehicleqaentry(int vehicle, int role, vec &feet);
int halovehicleplayervehicle();
// The vehicle whose box `body` is inside (not standing on it, not seated), or -1.
int halovehicleqainside(const physent *body);
// `body`'s feet inside vehicle `vehicle`, `along` and `side` its half-length and half-width from
// the box's centre (0, 0 the middle; 0.8 most of the way to the front or the right side). The
// vehicle need not exist yet (where the script will create it).
bool halovehicleqaplace(int vehicle, physent *body, float along, float side);
bool halovehicleqapush(int vehicle, float speed);       // rolls it forward at `speed` units/s
bool halovehicleqastuck(int vehicle, const char *kind); // fall: below the mesh; sink: half into the floor
// Seats the player at the wheel, turns it over and throws them out with no room for the body.
bool halovehicleqathrow(int vehicle);
// The player's vehicle put on the floor at `feet` heading `yaw` (the camera too) at `speed` units/s
// forward, for the jump checks (tests/test_vehicle_jump.py); and its motion: position (floor
// point), velocity, whether it is on the ground or flipped, its airborne gravity (vehicleGravity)
// and the surface's vertical rate it would leave the ground with.
bool halovehicleqajump(const vec &feet, float yaw, float speed);
// QA input (the route walker's, the autopilot's, qa_vehicle_input) stops accelerating at `speed`
// units/s while section map `map` is loaded ("*": any; an empty map clears every cap, speed 0 one).
void halovehicleqacruise(const char *map, float speed);
bool halovehicleqamotion(vec &pos, vec &vel, bool &grounded, bool &flipped, float &gravity, float &tangent);
// The section's jump governors (halo_vehiclegovern from its map .cfg, halo_vehiclegovernor): how
// many there are, and governor `index`'s take-off point, radius and speed (units, units/s).
int halovehicleqagovernors();
bool halovehicleqagovernor(int index, vec &at, float &radius, float &speed);

#endif
