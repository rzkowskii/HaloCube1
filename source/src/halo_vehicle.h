#ifndef HALO_VEHICLE_H
#define HALO_VEHICLE_H

#include <string>
#include <vector>

// Campaign vehicles (halo_vehicle.cpp): the player-driven Warthog, fixed gun turrets with a
// gunner seat, and the other scenario vehicles as parked props. Definitions come from
// packages/halo/vehicles.txt (tools/halo_vehicles.py); the section maps place them as
// PLAYERSTART entities with team 102 (tools/import_halo_campaign.py). Sizes and speeds are the
// retail vehicle's in Halo units, scaled to the section (haloscenarioscale()); the handling is
// plausibly Halo, not the retail physics.

enum HaloVehicleClass { HALO_VEHICLE_CAR, HALO_VEHICLE_TURRET, HALO_VEHICLE_PROP };
enum HaloSeatRole { HALO_SEAT_DRIVER, HALO_SEAT_GUNNER, HALO_SEAT_PASSENGER };

// Map lifetime: clear on map start, add each placement entity, then restart once the section
// (or a revert) begins. `checkpoint` restores the state halovehiclescheckpoint() saved.
void halovehiclesclear();
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
bool halovehiclecamera(physent &camera);
int halovehicledamage(int damage);                     // what reaches a seated player
// The Maw: seat the player in a Warthog at a section start (an unused one nearby, or a new one).
bool halovehicleescape(const vec &feet, float yaw);
// The Maw on foot: a free Warthog within `radius` of `feet`, or a new one beside them.
void halovehicleensure(const vec &feet, float yaw, float radius);
// halo.cpp's recovery found no floor for the player on foot: if they were thrown out of a vehicle
// with no room for the body in the last 15 s, back into its seat (the vehicle righted, or moved to
// the nearest spot where it fits or its last safe spot). False otherwise.
bool halovehiclereclaim(const char *why);

struct HaloVehicleStatus
{
    const char *name = "";
    int vehicleClass = HALO_VEHICLE_CAR, role = HALO_SEAT_DRIVER;
    float speed = 0;          // Halo world units per second
    bool flipped = false, airborne = false;
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
void halovehicleenterable(int object, bool player);
void halovehicleaiboard(int object, float distance, int actorKind, bool disabled);
bool halovehicleaigoto(int object, const char *seat, playerent *const *units, int count);
void halovehicleforget(const playerent *unit);         // an actor is gone

// QA (tests/native/vehicle_qa.inc).
int halovehiclecount();
void halovehicledump(const char *label);
void halovehicleqainput(float throttle, float steer, int millis);
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

#endif
