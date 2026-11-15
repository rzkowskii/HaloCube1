// Campaign vehicles: the Warthog the player drives (The Maw's escape run and wherever the
// scenario parks one), the Covenant gun turrets with a gunner seat, the Ghost and Wraith hover
// craft, the Scorpion tank and the Banshee, which flies. See halo_vehicle.h and
// docs/macos-build.md.
//
// A vehicle is a body the size of the retail vehicle at the section's scale. It drives on the
// retail collision when the section has one (four wheel traces and two chassis capsules) and on
// the stock cube grid otherwise. The Warthog's handling is an arcade car: throttle and brakes
// along the heading, strong lateral grip with some slide, steering towards the camera (or the
// strafe keys), gravity, jumps off crests, pitch and roll following the ground, and flips when
// it lands or stands too far over; E flips it back. Hover craft float a little over the same
// ground, turn on the spot towards the camera, strafe and slide; the tank turns on its tracks
// and barely slides; neither flips. A piloted Banshee flies: no gravity, the nose follows the
// camera, the throttle pushes along it, strafe and the jump and crouch keys move it sideways and
// up and down, and with no throttle it hovers; empty, it falls and lands. Actors at the controls
// drive and fly with simple steering (aiGround, aiFlight) and fire the vehicle's gun at their
// brain's target. None of it is the retail physics.
#include "cube.h"
#include "halo.h"
#include "halo_collision.h"
#include "halo_script.h"
#include "halo_weapons.h"
#include "halo_ai.h"
#include "halo_vehicle.h"
#include "halo_state.h"
#include "halo_fx.h"
#include <algorithm>
#include <sstream>
#include <string>
#include <vector>

// 0: steer towards the camera's heading (Halo); the strafe keys always steer as well.
// 1: the strafe keys only.
VARP(halo_vehiclesteer, 0, 0, 1);
// Bodies (the player and the actors) collide with vehicles.
VAR(halo_vehiclecollide, 0, 1, 1);
// Logs vehicle spawns, seats and flips.
VAR(halo_vehicletrace, 0, 0, 1);
// Actors at the controls drive and fly their vehicles (0: they sit still, for checking).
VAR(halo_vehicleai, 0, 1, 1);
// Vehicles with a body vitality (the Wraith; the Ghost and the Banshee while someone rides them)
// take damage and are destroyed (0: none is hurt; the QA vehicle walker sets it, as the foot
// walker protects the player).
VAR(halo_vehicledamage, 0, 1, 1);

namespace
{
    // Format 2 names the hover, tank and flyer classes and says which vehicles the player may
    // drive; format 1 tables still load (their props take a class from their motion kind).
    constexpr int formatVersion = 2;
    constexpr int maxSeats = 6;
    constexpr float gravity = 40.0f;            // AssaultCube units per s², as bounce entities fall
    constexpr float minimumTopSpeed = 36.0f;    // the escape's old foot speed: the timer was tuned for it
    constexpr float stockStep = 1.3f;           // the cube grid's floors step by whole cubes
    constexpr int flipEject = 700;              // ms upside down before the riders fall out
    constexpr int safeSpotCount = 4;            // stuck recovery: safe spots kept per vehicle
    constexpr int wedgeTime = 750, stallTime = 1500, fallTime = 2000;

    struct SeatDef
    {
        std::string name, marker;
        vec at, enter;                          // model frame, Halo units
        int role = HALO_SEAT_PASSENGER;
        unsigned flags = 0;                     // retail unit seat flags (1 invisible: inside the hull)
    };

    struct VehicleDef
    {
        int index = -1;
        std::string name, path, model, weaponPath;
        int vehicleClass = HALO_VEHICLE_PROP, kind = -2;
        float modelScale = 4;
        float forward = 0, reverse = 0, acceleration = 0, deceleration = 0, turn = 0, slide = 0;
        vec low = vec(-.5f, -.5f, 0), high = vec(.5f, .5f, 1), camera = vec(0, 0, 1);
        float radius = 1;
        std::vector<SeatDef> seats;
        int modelState = 0;                     // 0 unchecked, 1 loads, -1 missing
        std::string displayName;
        bool player = true;                     // the player may drive it (not the Wraith)
        // The tag's sounds (`sound` lines): the engine's loop while someone drives (a turret's
        // while its gunner turns it), the crash and the suspension's thump on a hard landing.
        std::string engineSound, crashSound, suspensionSound;
        // The collision model (`vitality` line): body vitality (0 indestructible: the Warthog, the
        // Scorpion, the turret), the coll flags (collOnlyExplosives, collOnlyOccupied,
        // collTakesRiderDamage), the retail material type the jpt! modifiers are looked up by
        // and its body multiplier; the body depleted effect (`destroyed`); the actor variant an
        // actor at its gun fights as (`variant`: its damage modifier and firing error).
        float vitality = 0, bodyMultiplier = 1;
        unsigned collFlags = 0;
        int material = -1;
        std::string destroyedEffect, variantPath;
    };
    constexpr unsigned collTakesRiderDamage = 2, collOnlyExplosives = 32, collOnlyOccupied = 64;

    struct Vehicle
    {
        int type = 0, object = -1, flags = 0;
        vec spawnFeet;
        float spawnYaw = 0;
        bool exists = false, placed = false, escape = false;
        vec pos, vel;                           // pos: the model origin (its floor), AssaultCube units
        float yaw = 0, pitch = 0, roll = 0, pitchVel = 0, rollVel = 0, steer = 0, climb = 0;
        float smoothZ = 0;                      // drawn height: the cube grid's whole-cube steps eased
        bool grounded = false, flipped = false, sleeping = false, carried = false;
        int flippedAt = 0, stillSince = 0;
        bool enterable = true, aiDisabled = false;
        float aiDistance = 0;
        int aiKind = -1;
        playerent *riders[maxSeats] = {};
        float aimYaw = 0, aimPitch = 0;
        HaloWeaponState gun, gun2;              // the gun's first and second trigger
        int nextShot = 0, nextShot2 = 0;
        int stunnedUntil = 0;                   // EMP (halovehicleemp): no throttle, no gun
        // An actor at the controls (aiGround, aiFlight, aiFire): the Banshee's strafing run
        // phase, the way it circles a target, backing off when stuck, and its bursts.
        int aiPhase = 0, aiPhaseUntil = 0, aiOrbitUntil = 0, aiStuckSince = 0, aiReverseUntil = 0;
        int aiBurstUntil = 0, aiNextBurst = 0, aiNextSecondary = 0;
        float aiOrbit = 1;
        vec aiGoal = vec(0, 0, 0);
        // Stuck recovery (updateStuck): the last spots it stood on upright, clear and grounded on
        // structure floor (newest last), and the fall, wedge and stall timers.
        vec safePos[safeSpotCount];
        float safeYaw[safeSpotCount] = {};
        int safeCount = 0, safeChecked = 0;
        int airSince = 0, embeddedSince = 0, stalledSince = 0, lastRecovery = 0;
        vec stalledAt = vec(0, 0, 0);
        float stillZ = 0;
        int lastImpact = 0;                     // the crash and landing sounds' cooldown
        float loopYaw = 0;                      // a turret's yaw when its turning loop last checked
        int loopYawAt = 0, loopTurnedAt = 0;    // when it last checked, and last turned fast enough
        float loopLevel = 0;                    // how fast it turned then (0.3..1)
        // Body health, 1 whole to 0 (halovehiclehit; only vehicles with a body vitality lose it).
        // `destroyed`: it blew up (it no longer exists; the scripts' object is gone too).
        float health = 1;
        bool destroyed = false;
    };

    std::vector<VehicleDef> defs;
    bool defsLoaded = false;
    vector<Vehicle> vehicles;
    std::vector<Vehicle> saved;
    bool savedValid = false, restoredFromCheckpoint = false;
    int playerVehicle = -1, playerSeat = -1;
    int lastFire = 0, shotsFired = 0;
    float sectionScale = 0;                     // from the placements (attr7 / 20), 0 when none
    vec seatEye;                                // where the player was put on the seat last
    float qaThrottle = 0, qaSteer = 0;
    int qaUntil = 0;
    // Bodies part way inside a vehicle that no push could get out (depenetrate): they collide
    // with every other vehicle but may walk out of this one. `retry`: when to try again.
    struct Trap { const physent *body; int vehicle, retry; };
    std::vector<Trap> trapped;
    // The actors the player's Warthog struck this frame (run over once a frame).
    std::vector<const physent *> struck;
    int struckFrame = -1;
    // The vehicle the player was last thrown out of with no room for the body (unseatPlayer's
    // forced exit): halovehiclereclaim puts them back in it rather than leaving them wedged.
    int thrownVehicle = -1, thrownSeat = -1, thrownAt = 0;
    bool qaForceThrow = false;              // halovehicleqathrow: skip the exit spots
    // The secondary trigger, jump and crouch keys while seated, and the QA flight input.
    bool secondaryHeld = false, jumpHeld = false, crouchHeld = false;
    float qaStrafe = 0, qaClimb = 0;
    bool qaFlight = false;
    // ai_go_to_vehicle: actors walking to a seat (halo_ai.cpp haloactorgoto), seated on arrival.
    struct Boarding { playerent *unit; int vehicle, seat, since, until; };
    std::vector<Boarding> boardings;

    // What drives a vehicle for one step: the player's keys or an actor's steering. -1..1 each;
    // `aimYaw` and `aimPitch` (degrees) are where a flyer's nose turns; `piloted` keeps a
    // Banshee flying (empty, it falls).
    struct Drive { float throttle = 0, steer = 0, strafe = 0, climb = 0, aimYaw = 0, aimPitch = 0; bool piloted = false; };

    int seatRole(const std::string &name, const std::string &marker)
    {
        std::string text = name + " " + marker;
        for(char &c : text) c = char(tolower(c));
        if(text.find("driver") != std::string::npos) return HALO_SEAT_DRIVER;
        if(text.find("gunner") != std::string::npos) return HALO_SEAT_GUNNER;
        return HALO_SEAT_PASSENGER;
    }

    std::string titleCase(const std::string &name)
    {
        std::string result = name;
        bool start = true;
        for(char &c : result)
        {
            if(c == '_') c = ' ';
            if(start && c >= 'a' && c <= 'z') c -= 'a' - 'A';
            start = c == ' ';
        }
        return result;
    }

    // A Warthog-sized car, used without packages/halo/vehicles.txt (with the model the actor
    // importer has always written to packages/models/halo/warthog at 4.5 units per Halo unit).
    // Round, hand-picked numbers, not the tag's: retail data stays in the generated table.
    VehicleDef builtinWarthog()
    {
        VehicleDef d;
        d.index = 0;
        d.name = "warthog";
        d.path = "vehicles\\warthog\\warthog";
        d.model = "halo/warthog";
        d.modelScale = 4.5f;
        d.weaponPath = "vehicles\\warthog\\warthog gun";
        d.vehicleClass = HALO_VEHICLE_CAR;
        d.kind = 1;
        d.forward = .25f; d.reverse = .1f; d.acceleration = .003f; d.deceleration = .01f; d.turn = .1f;
        d.low = vec(-.95f, -.5f, 0);
        d.high = vec(.95f, .5f, 1);
        d.camera = vec(0, .15f, .6f);
        d.radius = 1.1f;
        SeatDef driver; driver.name = "W-driver"; driver.marker = "driver"; driver.at = vec(0, .15f, .4f); driver.enter = vec(0, .6f, 0);
        SeatDef passenger; passenger.name = "W-passenger"; passenger.marker = "passenger"; passenger.at = vec(-.1f, -.15f, .55f); passenger.enter = vec(0, -.6f, 0);
        SeatDef gunner; gunner.name = "W-gunner"; gunner.marker = "gunner"; gunner.at = vec(-.6f, 0, .75f); gunner.enter = vec(-1.2f, 0, 0);
        for(SeatDef *s : {&driver, &passenger, &gunner}) { s->role = seatRole(s->name, s->marker); d.seats.push_back(*s); }
        d.displayName = "Warthog";
        return d;
    }

    bool finiteValue(float f) { return std::isfinite(f); }
    bool finiteValue(const vec &v) { return finiteValue(v.x) && finiteValue(v.y) && finiteValue(v.z); }

    // The numbers the drive, collision and camera code divide by or scale with.
    bool sane(const VehicleDef &d)
    {
        if(!finiteValue(d.modelScale) || d.modelScale <= 0 || !finiteValue(d.radius) || d.radius < 0) return false;
        for(float f : {d.forward, d.reverse, d.acceleration, d.deceleration, d.turn, d.slide}) if(!finiteValue(f)) return false;
        if(!finiteValue(d.vitality) || d.vitality < 0 || !finiteValue(d.bodyMultiplier)) return false;
        if(!finiteValue(d.low) || !finiteValue(d.high) || !finiteValue(d.camera)) return false;
        loopi(3) if(d.high[i] <= d.low[i] || d.high[i] - d.low[i] > 100) return false;
        for(const SeatDef &s : d.seats) if(!finiteValue(s.at) || !finiteValue(s.enter)) return false;
        return true;
    }

    bool parse(const char *text)
    {
        std::istringstream lines(text);
        std::string line;
        VehicleDef *def = nullptr;
        bool versioned = false;
        int version = 0;
        while(std::getline(lines, line))
        {
            if(line.empty() || line[0] == '#') continue;
            std::istringstream in(line);
            std::string key;
            in >> key;
            bool ok = true;
            if(key == "format") { in >> version; ok = versioned = version == formatVersion || version == 1; }
            else if(key == "vehicle")
            {
                defs.emplace_back();
                def = &defs.back();
                ok = bool(in >> def->index >> def->name) && def->index == int(defs.size()) - 1;
            }
            else if(!def) ok = false;
            else if(key == "path") std::getline(in >> std::ws, def->path);
            else if(key == "class")
            {
                std::string kind;
                in >> kind;
                def->vehicleClass = kind == "car" ? HALO_VEHICLE_CAR : kind == "turret" ? HALO_VEHICLE_TURRET :
                                    kind == "hover" ? HALO_VEHICLE_HOVER : kind == "tank" ? HALO_VEHICLE_TANK :
                                    kind == "flyer" ? HALO_VEHICLE_FLYER : HALO_VEHICLE_PROP;
            }
            else if(key == "player") { int player = 1; ok = bool(in >> player); def->player = player != 0; }
            else if(key == "model") ok = bool(in >> def->model >> def->modelScale) && def->modelScale > 0;
            else if(key == "motion") ok = bool(in >> def->kind >> def->forward >> def->reverse >> def->acceleration
                                                >> def->deceleration >> def->turn >> def->slide);
            else if(key == "bounds") ok = bool(in >> def->low.x >> def->low.y >> def->low.z >> def->high.x >> def->high.y >> def->high.z);
            else if(key == "radius") ok = bool(in >> def->radius);
            else if(key == "weapon") { std::getline(in >> std::ws, def->weaponPath); if(def->weaponPath == "-") def->weaponPath.clear(); }
            else if(key == "seat")
            {
                SeatDef s;
                ok = bool(in >> s.name >> s.marker >> s.at.x >> s.at.y >> s.at.z >> s.enter.x >> s.enter.y >> s.enter.z);
                // The seat flags follow in newer tables.
                unsigned flags = 0;
                if(ok && in >> flags) s.flags = flags;
                s.role = seatRole(s.name, s.marker);
                if(ok && def->seats.size() < size_t(maxSeats)) def->seats.push_back(s);
            }
            else if(key == "camera") ok = bool(in >> def->camera.x >> def->camera.y >> def->camera.z);
            else if(key == "vitality")
                ok = bool(in >> def->vitality >> def->collFlags >> def->material >> def->bodyMultiplier) && def->vitality >= 0;
            else if(key == "destroyed") std::getline(in >> std::ws, def->destroyedEffect);
            else if(key == "variant") std::getline(in >> std::ws, def->variantPath);
            else if(key == "sound")
            {
                // Unknown roles are skipped, so a newer table still loads.
                std::string role, tag;
                in >> role;
                std::getline(in >> std::ws, tag);
                std::string *slot = role == "engine" ? &def->engineSound : role == "crash" ? &def->crashSound :
                                    role == "suspension" ? &def->suspensionSound : nullptr;
                if(slot && !tag.empty()) *slot = halosoundsample(tag);
            }
            else if(key == "end")
            {
                // A record whose sizes or speeds are not finite, or whose box is inside out, would
                // put NaN into every vehicle's position: refuse the table (the built-in Warthog).
                ok = sane(*def);
                // A format 1 table parked the Ghost, Scorpion, Banshee and Wraith as props: their
                // retail motion kind says what they are (4 hover, 0 tank, 5 airborne).
                if(def->vehicleClass == HALO_VEHICLE_PROP && version == 1)
                {
                    if(def->kind == 4) def->vehicleClass = HALO_VEHICLE_HOVER;
                    else if(def->kind == 0) def->vehicleClass = HALO_VEHICLE_TANK;
                    else if(def->kind == 5) def->vehicleClass = HALO_VEHICLE_FLYER;
                    if(def->name == "wraith") def->player = false;
                }
                def->displayName = def->vehicleClass == HALO_VEHICLE_TURRET ? "Covenant Turret" : titleCase(def->name);
                def = nullptr;
            }
            if(!ok)
            {
                conoutf("\f3Halo vehicles: invalid line in packages/halo/vehicles.txt: %s", line.c_str());
                defs.clear();
                return false;
            }
        }
        // A table cut short inside a record (no `end`) is refused too.
        if(def)
        {
            conoutf("\f3Halo vehicles: packages/halo/vehicles.txt ends inside the %s record", def->name.c_str());
            defs.clear();
            return false;
        }
        if(!versioned) conoutf("\f3Halo vehicles: packages/halo/vehicles.txt has no format %d line", formatVersion);
        return versioned && !defs.empty();
    }

    void loadDefs()
    {
        if(defsLoaded) return;
        defsLoaded = true;
        int size = 0;
        char *text = loadfile(path("packages/halo/vehicles.txt", true), &size);
        if(text && parse(text)) conoutf("Halo vehicles: loaded %d definitions", int(defs.size()));
        else
        {
            defs.clear();
            defs.push_back(builtinWarthog());
            if(text) conoutf("\f3Halo vehicles: packages/halo/vehicles.txt is unusable; re-run tools/halo_vehicles.py (built-in Warthog only)");
            else conoutf("Halo vehicles: packages/halo/vehicles.txt is missing; run tools/halo_vehicles.py (built-in Warthog only)");
        }
        delete[] text;
    }

    // AssaultCube units per Halo unit: the section's projection, which the placements carry, so
    // a section launched without its mission script sizes its vehicles the same.
    float vehicleScale() { return sectionScale > 0 ? sectionScale : haloscenarioscale(); }

    const VehicleDef *defOf(const Vehicle &v) { return v.type >= 0 && v.type < int(defs.size()) ? &defs[v.type] : nullptr; }

    bool hasModel(VehicleDef &d)
    {
        if(!d.modelState)
        {
            d.modelState = -1;
            if(!d.model.empty() && d.model != "-")
            {
                defformatstring(file)("packages/models/%s/tris.md3", d.model.c_str());
                if(fileexists(findfile(path(file), "rb"), "rb")) d.modelState = 1;
            }
            // The older Warthog export, if the vehicle importer has not run.
            if(d.modelState < 0 && d.name == "warthog" && fileexists(findfile(path("packages/models/halo/warthog/tris.md3", true), "rb"), "rb"))
            {
                d.model = "halo/warthog";
                d.modelScale = 4.5f;
                d.modelState = 1;
            }
            if(d.modelState < 0) conoutf("Halo vehicles: no model for %s, drawing a placeholder", d.name.c_str());
        }
        return d.modelState > 0;
    }

    bool drivenClass(int c) { return c == HALO_VEHICLE_CAR || c == HALO_VEHICLE_HOVER || c == HALO_VEHICLE_TANK || c == HALO_VEHICLE_FLYER; }
    // The driver works the gun: the Ghost's and Banshee's bolts, the Wraith's mortar, the Scorpion's cannon.
    bool driverFires(const VehicleDef &d)
    {
        return d.vehicleClass == HALO_VEHICLE_HOVER || d.vehicleClass == HALO_VEHICLE_TANK || d.vehicleClass == HALO_VEHICLE_FLYER;
    }
    // A hover craft floats this far (Halo units) over the floor under it.
    constexpr float hoverGap = .12f;
    // How far the model's origin stands above the vehicle's floor point: a hover craft's model
    // reaches below its origin (the Ghost's by 0.14 Halo units), and it floats over the ground.
    float liftOf(const VehicleDef &d, float s)
    {
        return d.vehicleClass == HALO_VEHICLE_HOVER ? (max(0.0f, -d.low.z) + hoverGap) * s : 0.0f;
    }

    // Dimensions in AssaultCube units at the section's scale.
    struct Dims
    {
        float s, length, width, height, centre, halfL, halfW, step, wheelFront, wheelRear, wheelSide, wheelbase, track, lift;
    };

    Dims dims(const Vehicle &v)
    {
        const VehicleDef &d = *defOf(v);
        Dims r;
        r.s = vehicleScale();
        r.length = max(.2f, d.high.x - d.low.x) * r.s;
        r.width = max(.2f, d.high.y - d.low.y) * r.s;
        r.lift = liftOf(d, r.s);
        r.height = max(.2f, d.high.z - max(0.0f, d.low.z)) * r.s + r.lift;
        r.centre = (d.high.x + d.low.x) * .5f * r.s;
        r.halfL = r.length * .5f;
        r.halfW = r.width * .5f;
        r.step = halocollisionactive() ? clamp(r.height * .3f, .3f, 1.3f) : stockStep;
        r.wheelFront = r.centre + r.halfL * .7f;
        r.wheelRear = r.centre - r.halfL * .7f;
        r.wheelSide = r.halfW * .8f;
        r.wheelbase = r.wheelFront - r.wheelRear;
        r.track = 2 * r.wheelSide;
        return r;
    }

    vec forwardOf(float yaw) { return vec(sinf(yaw * RAD), -cosf(yaw * RAD), 0); }
    vec rightOf(float yaw) { return vec(cosf(yaw * RAD), sinf(yaw * RAD), 0); }

    // A point of the model frame (x forward, y left, z up; Halo units) in the world, yaw only;
    // a hover craft's model stands its lift above the floor point.
    vec modelPoint(const Vehicle &v, const vec &local, float s)
    {
        const vec f = forwardOf(v.yaw), r = rightOf(v.yaw);
        const VehicleDef *d = defOf(v);
        const float lift = d ? liftOf(*d, s) : 0;
        return vec(v.pos).add(vec(f).mul(local.x * s)).sub(vec(r).mul(local.y * s)).add(vec(0, 0, local.z * s + lift));
    }

    float yawTo(const vec &from, const vec &to)
    {
        const float yaw = atan2f(to.x - from.x, -(to.y - from.y)) / RAD;
        return yaw < 0 ? yaw + 360 : yaw;
    }

    // Top speed in AssaultCube units per second: the retail speed at the section's scale, and
    // never below the Warthog's 36 (the Maw's timer was set for it) scaled by the vehicle's
    // retail speed against the Warthog's, so a Ghost still outruns the player on foot. A Banshee
    // is given a third more.
    float topSpeed(const VehicleDef &def, float s)
    {
        if(def.vehicleClass == HALO_VEHICLE_CAR) return max(minimumTopSpeed, def.forward * 30 * s);
        const float floor = minimumTopSpeed * clamp(def.forward / .255f, .3f, 1.5f) * (def.vehicleClass == HALO_VEHICLE_FLYER ? 1.3f : 1.0f);
        return max(floor, def.forward * 30 * s);
    }

    // Degrees per second a hover craft or a tank turns on the spot: the tag's turn rate when it
    // has one, else the Ghost's 150, slower for the heavier and slower ones (the Wraith about 60,
    // the Scorpion about 45).
    float turnRate(const VehicleDef &def)
    {
        if(def.turn > .001f) return def.turn * 30 / RAD;
        const float rate = 150 * powf(clamp(def.forward / .225f, .3f, 1.0f), 1.5f);
        return def.vehicleClass == HALO_VEHICLE_TANK ? rate * .6f : rate;
    }

    float wrap180(float a)
    {
        a = fmodf(a + 180.0f, 360.0f);
        if(a < 0) a += 360.0f;
        return a - 180.0f;
    }

    // Highest floor under (x, y) between `top` and `bottom`. `wall`: solid or too steep there.
    bool groundAt(float x, float y, float top, float bottom, float &z, bool &wall)
    {
        wall = false;
        if(halocollisionactive())
        {
            HaloCollisionHit hit;
            if(!haloscenariotrace(vec(x, y, top), vec(x, y, bottom), hit)) return false;
            if(hit.normal.z < .2f) { wall = true; return false; }
            z = top + (bottom - top) * hit.fraction;
            return true;
        }
        const int ix = int(floorf(x)), iy = int(floorf(y));
        if(OUTBORD(ix, iy)) { wall = true; return false; }
        const sqr *s = S(ix, iy);
        if(SOLID(s)) { wall = true; return false; }
        float f = s->floor;
        if(s->type == FHF) f -= (s->vdelta + S(ix + 1, iy)->vdelta + S(ix, iy + 1)->vdelta + S(ix + 1, iy + 1)->vdelta) / 16.0f;
        if(f > top) { wall = true; return false; }
        if(f < bottom) return false;
        z = f;
        return true;
    }

    float stockCeiling(float x, float y)
    {
        const int ix = int(floorf(x)), iy = int(floorf(y));
        if(OUTBORD(ix, iy)) return -1e9f;
        const sqr *s = S(ix, iy);
        if(SOLID(s)) return -1e9f;
        float c = s->ceil;
        if(s->type == CHF) c += (s->vdelta + S(ix + 1, iy)->vdelta + S(ix, iy + 1)->vdelta + S(ix + 1, iy + 1)->vdelta) / 16.0f;
        return c;
    }

    bool overlapsVehicle(int self, const vec &pos, float yaw, const Dims &d)
    {
        const vec f = forwardOf(yaw), r = rightOf(yaw);
        const vec centre = vec(pos).add(vec(f).mul(d.centre));
        loopv(vehicles)
        {
            if(i == self) continue;
            const Vehicle &o = vehicles[i];
            if(!o.exists || !defOf(o)) continue;
            const Dims od = dims(o);
            if(pos.z + d.height <= o.pos.z + .05f || o.pos.z + od.height <= pos.z + .05f) continue;
            const vec of = forwardOf(o.yaw), orr = rightOf(o.yaw);
            const vec ocentre = vec(o.pos).add(vec(of).mul(od.centre));
            const vec delta = vec(ocentre).sub(centre);
            // Separating axes of two boxes in the plane.
            const vec axes[4] = {f, r, of, orr};
            bool separated = false;
            for(const vec &axis : axes)
            {
                const float a = fabsf(f.dot(axis)) * d.halfL + fabsf(r.dot(axis)) * d.halfW;
                const float b = fabsf(of.dot(axis)) * od.halfL + fabsf(orr.dot(axis)) * od.halfW;
                if(fabsf(delta.dot(axis)) > a + b) { separated = true; break; }
            }
            if(!separated) return true;
        }
        return false;
    }

    // Whether the chassis can move by `delta` (horizontal) from `pos`: walls, ceilings, machines
    // and other vehicles. On retail collision `normal` gets the wall's normal.
    bool chassisBlocked(int self, const Vehicle &v, const Dims &d, const vec &pos, const vec &delta, vec &normal)
    {
        normal = vec(0, 0, 0);
        const vec to = vec(pos).add(delta);
        // Two vehicles that already overlap (one spawned on another) may move apart.
        if(overlapsVehicle(self, to, v.yaw, d) && !overlapsVehicle(self, pos, v.yaw, d)) return true;
        const vec f = forwardOf(v.yaw), r = rightOf(v.yaw);
        if(halocollisionactive())
        {
            const float radius = d.halfW * .95f, height = max(2 * radius, d.height - d.step);
            const float offset = max(0.0f, d.halfL - radius);
            bool blocked = false;
            float best = 2;
            for(float along : {offset, -offset})
            {
                const vec feet = vec(pos).add(vec(f).mul(d.centre + along)).add(vec(0, 0, d.step));
                HaloCollisionHit hit;
                if(!delta.iszero() && halocollisionsweep(feet, delta, radius, height, hit, false) && hit.normal.z < .3f)
                {
                    // Already embedded (a spawn on a seam): let it move out rather than stick.
                    HaloCollisionHit inside;
                    if(hit.fraction <= 0 && halocollisionoverlap(feet, radius, height, inside, false) && inside.penetration > .05f) continue;
                    blocked = true;
                    if(hit.fraction < best) { best = hit.fraction; normal = hit.normal; }
                }
                // Doors and other machines.
                physent probe;
                probe.type = ENT_BOUNCE;
                probe.radius = radius;
                probe.eyeheight = height;
                probe.aboveeye = 0;
                probe.o = vec(feet).add(delta).add(vec(0, 0, height));
                float hi = 1e9f, lo = -1e9f;
                if(haloscenariocollide(&probe, hi, lo))
                {
                    blocked = true;
                    if(normal.iszero()) normal = vec(delta).mul(-1).normalize();
                }
            }
            normal.z = 0;
            if(!normal.iszero()) normal.normalize();
            return blocked;
        }
        // The cube grid: the footprint's corners, edges and middle.
        for(float a : {-1.0f, 0.0f, 1.0f}) for(float b : {-1.0f, 0.0f, 1.0f})
        {
            const vec p = vec(to).add(vec(f).mul(d.centre + a * d.halfL * .95f)).add(vec(r).mul(b * d.halfW * .95f));
            float z;
            bool wall;
            groundAt(p.x, p.y, pos.z + d.step, -1e9f, z, wall);
            if(wall || stockCeiling(p.x, p.y) < pos.z + d.height * .8f) return true;
        }
        return false;
    }

    void ejectRiders(Vehicle &v, int index);

    float vehicleTop(const Vehicle &v, const Dims &d) { return v.pos.z + (v.flipped ? d.height * .6f : d.height); }

    // Whether body `b` is inside vehicle `v`'s box, as halovehiclecollide sees it (not standing on
    // its top or passing under it), and `push`, the shortest horizontal move out of the box: out
    // through its nearest face, `margin` clear.
    bool bodyInside(const Vehicle &v, const Dims &d, const physent *b, vec &push, float margin)
    {
        const vec f = forwardOf(v.yaw), r = rightOf(v.yaw);
        const vec rel = vec(b->o).sub(vec(v.pos).add(vec(f).mul(d.centre)));
        const float along = rel.dot(f), side = rel.dot(r);
        const float ex = d.halfL + b->radius, ey = d.halfW + b->radius;
        if(fabsf(along) >= ex || fabsf(side) >= ey) return false;
        const float feet = b->o.z - b->eyeheight, height = b->eyeheight + b->aboveeye;
        if(feet >= vehicleTop(v, d) - .42f || feet + height <= v.pos.z + .1f) return false;
        const float exits[4] = {ex - along, ex + along, ey - side, ey + side};
        const vec axes[4] = {f, vec(f).mul(-1), r, vec(r).mul(-1)};
        int best = 0;
        loopi(4) if(exits[i] < exits[best]) best = i;
        push = vec(axes[best]).mul(exits[best] + margin);
        push.z = 0;
        return true;
    }

    // Whether body `b` fits with its feet at `feet`: clear of the structure (or the cube grid),
    // the devices, the other bodies and the vehicles. With `floor` it must also stand on floor
    // there (on retail collision from a step above to three units below), and `feet` drops onto
    // it; without, it may be in the air (shoved over a ledge, it falls).
    bool bodyFitsAt(physent *b, vec &feet, bool floor)
    {
        const vec savedO = b->o, savedVel = b->vel;
        const bool savedFloor = b->onfloor;
        b->o = vec(feet).add(vec(0, 0, b->eyeheight + (floor && !halocollisionactive() ? .5f : 0)));
        const bool blocked = collide(b, floor);
        const vec placed = vec(b->o).sub(vec(0, 0, b->eyeheight));
        b->o = savedO;
        b->vel = savedVel;
        b->onfloor = savedFloor;
        if(blocked) return false;
        if(!floor) return true;
        // The cube grid drops a spawn onto the floor however far down it is; keep to a step.
        if(!halocollisionactive() && (placed.z < feet.z - 3 || placed.z > feet.z + stockStep)) return false;
        feet = placed;
        return true;
    }

    bool trappedIn(const physent *b, int vehicle)
    {
        for(const Trap &t : trapped) if(t.body == b && t.vehicle == vehicle) return true;
        return false;
    }

    // The bodies vehicles collide with: the player on foot and the living actors not in a seat.
    template<class F> void eachBody(F visit)
    {
        if(player1->state == CS_ALIVE && playerVehicle < 0 && !editmode) visit(static_cast<physent *>(player1), true);
        loopv(players)
        {
            playerent *p = players[i];
            if(p && p->state == CS_ALIVE && !halovehicleinside(p)) visit(static_cast<physent *>(p), false);
        }
    }

    // A moving vehicle runs into bodies: each is pushed out ahead of (or beside) the chassis when
    // it has room there, else the vehicle stops against it (false) rather than swallowing it. The
    // player's Warthog at speed also runs actors down, once a frame each, as it always has.
    bool shoveBodies(Vehicle &v, int index, const Dims &d)
    {
        if(!halo_vehiclecollide || v.carried) return true;
        const VehicleDef &def = *defOf(v);
        const float speed = vec(v.vel.x, v.vel.y, 0).magnitude();
        if(struckFrame != lastmillis) { struck.clear(); struckFrame = lastmillis; }
        bool clear = true;
        eachBody([&](physent *b, bool isPlayer)
        {
            vec push;
            if(!clear || trappedIn(b, index) || !bodyInside(v, d, b, push, .05f)) return;
            if(!isPlayer && index == playerVehicle && drivenClass(def.vehicleClass) && speed > 8 &&
               std::find(struck.begin(), struck.end(), b) == struck.end())
            {
                struck.push_back(b);
                playerent *p = static_cast<playerent *>(b);
                dodamage(speed > 20 ? INT_MAX : int(speed * 4), p, player1, GUN_KNIFE);
                if(p->state != CS_ALIVE) return;
            }
            vec feet = vec(b->o).sub(vec(0, 0, b->eyeheight)).add(push);
            if(bodyFitsAt(b, feet, false)) b->o.add(push);
            else clear = false;
        });
        return clear;
    }

    void setFlipped(Vehicle &v, int index, float rollTo)
    {
        if(v.flipped) return;
        v.flipped = true;
        v.flippedAt = lastmillis;
        v.roll = rollTo;
        v.pitchVel = v.rollVel = 0;
        if(halo_vehicletrace) conoutf("Halo vehicle %d flipped", index);
    }

    void fly(Vehicle &v, int index, float dt, const Drive &in);

    // One physics step of `dt` seconds. A piloted Banshee flies (fly); everything else drives on
    // the ground, or falls and lands.
    // A crash or a hard landing: the tag's sound where it happened (at most one in 0.4 s), and a
    // knock of the camera for the player's own vehicle.
    void impact(Vehicle &v, int index, const std::string &sample, float strength)
    {
        if(lastmillis - v.lastImpact < 400) return;
        v.lastImpact = lastmillis;
        const vec at = v.pos;
        if(!sample.empty()) haloplaysound(sample.c_str(), S_NULL, NULL, &at);
        if(index == playerVehicle) halofxshakeimpulse(clamp(strength, .2f, 1.0f), 350);
        if(halo_vehicletrace) conoutf("Halo vehicle: impact %d strength %.2f%s", index, strength, sample.empty() ? " (no sound)" : "");
    }

    void simulate(Vehicle &v, int index, float dt, const Drive &in)
    {
        const VehicleDef &def = *defOf(v);
        if(def.vehicleClass == HALO_VEHICLE_FLYER && in.piloted && !v.flipped) { fly(v, index, dt, in); return; }
        const Dims d = dims(v);
        float throttle = in.throttle, steer = in.steer, strafe = in.strafe;
        // An empty Banshee (or one whose pilot is out) only falls and lands; props and turrets stand.
        if(!drivenClass(def.vehicleClass) || def.vehicleClass == HALO_VEHICLE_FLYER) throttle = steer = strafe = 0;
        const bool car = def.vehicleClass == HALO_VEHICLE_CAR;
        const float top = topSpeed(def, d.s);
        const float reverseTop = max(top * .4f, def.reverse * 30 * d.s);
        const float accelTime = clamp(def.acceleration > 0 ? def.forward / def.acceleration / 30 : 2.0f, 1.2f, 3.0f);
        const float accel = top / accelTime, brake = accel * 2.5f, coast = accel * .35f;
        vec f = forwardOf(v.yaw), r = rightOf(v.yaw);
        float vf = v.vel.dot(f), vr = v.vel.dot(r);
        const float speedBefore = vec(v.vel.x, v.vel.y, 0).magnitude();
        if(v.flipped)
        {
            vf *= max(0.0f, 1 - dt * 4);
            vr *= max(0.0f, 1 - dt * 4);
            throttle = steer = 0;
        }
        else if(v.grounded)
        {
            if(throttle > 0) vf = vf < -.5f ? min(0.0f, vf + brake * dt) : min(max(vf, top), vf + accel * throttle * dt);
            else if(throttle < 0) vf = vf > .5f ? max(0.0f, vf - brake * dt) : max(min(vf, -reverseTop), vf + accel * .7f * throttle * dt);
            else vf = vf > 0 ? max(0.0f, vf - coast * dt) : min(0.0f, vf + coast * dt);
            if(vf > top) vf = max(top, vf - coast * dt);
            if(vf < -reverseTop) vf = min(-reverseTop, vf + coast * dt);
            // Gravity along a slope, halved: the wheels grip. Off the throttle and nearly
            // stopped on a slope it can stand on, the brakes hold it.
            if(!throttle && fabsf(vf) < .8f && fabsf(v.pitch) < 35) vf = 0;
            else vf -= gravity * .5f * sinf(v.pitch * RAD) * dt;
            const vec moving = vec(f).mul(vf).add(vec(r).mul(vr));
            if(car)
            {
                // Steering: the turn rate is the speed over the wheelbase, capped by grip at speed.
                v.steer += clamp(steer - v.steer, -dt * 5, dt * 5);
                const float speed = fabsf(vf);
                if(speed > .2f)
                {
                    const float rate = min(speed / max(d.wheelbase, .5f) * .577f, 1.25f * top / max(speed, 1.0f));
                    v.yaw = fmodf(v.yaw + v.steer * rate * (vf >= 0 ? 1 : -1) * dt / RAD + 360.0f, 360.0f);
                }
                // The body turns under its momentum: the new heading's sideways share is the slide,
                // which the tyres take out quickly.
                f = forwardOf(v.yaw);
                r = rightOf(v.yaw);
                vf = moving.dot(f);
                vr = moving.dot(r) * expf(-8.0f * dt);
            }
            else
            {
                // A hover craft or a tank turns on the spot at its own rate, whatever its speed.
                v.steer += clamp(steer - v.steer, -dt * 6, dt * 6);
                v.yaw = fmodf(v.yaw + v.steer * turnRate(def) * dt + 360.0f, 360.0f);
                f = forwardOf(v.yaw);
                r = rightOf(v.yaw);
                vf = moving.dot(f);
                vr = moving.dot(r);
                if(def.vehicleClass == HALO_VEHICLE_HOVER)
                {
                    // It strafes, and slides: what it carries sideways fades slowly.
                    if(strafe) vr += clamp(strafe * top * .6f - vr, -accel * .8f * dt, accel * .8f * dt);
                    else vr *= expf(-2.5f * dt);
                }
                else vr *= expf(-20.0f * dt);   // tracks barely slide
            }
        }
        v.vel = vec(f).mul(vf).add(vec(r).mul(vr)).add(vec(0, 0, v.vel.z));

        // Vertical motion while airborne.
        const float oldz = v.pos.z;
        if(!v.grounded)
        {
            v.vel.z -= gravity * dt;
            v.pos.z += v.vel.z * dt;
            v.pitch += v.pitchVel * dt;
            v.roll += v.rollVel * dt;
            v.pitchVel -= 15.0f * dt;
            v.pitchVel *= max(0.0f, 1 - dt * .5f);
            v.rollVel *= max(0.0f, 1 - dt * .5f);
            v.pitch = wrap180(v.pitch);
            v.roll = wrap180(v.roll);
        }

        // Horizontal motion against walls.
        vec delta(v.vel.x * dt, v.vel.y * dt, 0);
        if(!delta.iszero())
        {
            vec normal;
            if(chassisBlocked(index, v, d, v.pos, delta, normal))
            {
                if(!normal.iszero())
                {
                    const float into = v.vel.dot(normal);
                    if(into < 0) v.vel.sub(vec(normal).mul(into * 1.25f));
                    v.vel.x *= .85f;
                    v.vel.y *= .85f;
                    delta = vec(v.vel.x * dt, v.vel.y * dt, 0);
                    if(chassisBlocked(index, v, d, v.pos, delta, normal)) delta = vec(0, 0, 0);
                }
                else
                {
                    // The cube grid: keep whichever axis is free.
                    const vec dx(delta.x, 0, 0), dy(0, delta.y, 0);
                    vec n;
                    const bool freeX = !chassisBlocked(index, v, d, v.pos, dx, n), freeY = !chassisBlocked(index, v, d, v.pos, dy, n);
                    if(freeX && (!freeY || fabsf(delta.x) >= fabsf(delta.y))) { delta = dx; v.vel.y *= -.2f; }
                    else if(freeY) { delta = dy; v.vel.x *= -.2f; }
                    else { delta = vec(0, 0, 0); v.vel.x *= -.2f; v.vel.y *= -.2f; }
                }
            }
            v.pos.add(delta);
            if(!delta.iszero() && !shoveBodies(v, index, d))
            {
                // Pinned against a wall (or another body): the body stops the vehicle.
                v.pos.sub(delta);
                const vec along = vec(delta).normalize();
                const float into = v.vel.x * along.x + v.vel.y * along.y;
                if(into > 0) { v.vel.x -= along.x * into * 1.1f; v.vel.y -= along.y * into * 1.1f; }
                delta = vec(0, 0, 0);
            }
        }

        // The wheels on the ground.
        const vec wheels[4] = {
            vec(v.pos).add(vec(f).mul(d.wheelFront)).sub(vec(r).mul(d.wheelSide)),
            vec(v.pos).add(vec(f).mul(d.wheelFront)).add(vec(r).mul(d.wheelSide)),
            vec(v.pos).add(vec(f).mul(d.wheelRear)).sub(vec(r).mul(d.wheelSide)),
            vec(v.pos).add(vec(f).mul(d.wheelRear)).add(vec(r).mul(d.wheelSide)),
        };
        const float searchTop = max(oldz, v.pos.z) + d.step;
        const float searchBottom = v.grounded ? v.pos.z - d.step * 1.5f : v.pos.z - .02f;
        float heights[4];
        bool found[4], anyWall = false;
        int count = 0;
        float sum = 0;
        loopi(4)
        {
            bool wall = false;
            found[i] = groundAt(wheels[i].x, wheels[i].y, searchTop, searchBottom, heights[i], wall);
            anyWall |= wall;
            if(found[i]) { sum += heights[i]; ++count; }
        }
        if(anyWall && !delta.iszero())
        {
            // A wheel ran into a face too high or steep to climb: back off.
            v.pos.sub(delta);
            v.vel.x *= -.2f;
            v.vel.y *= -.2f;
        }
        if(!count)
        {
            if(v.grounded)
            {
                v.grounded = false;
                v.vel.z = v.climb > 0 ? min(v.climb, vec(v.vel.x, v.vel.y, 0).magnitude() * .8f) : 0;
                v.pitchVel = clamp(v.pitchVel, -120.0f, 120.0f);
                v.rollVel = clamp(v.rollVel, -120.0f, 120.0f);
            }
        }
        else
        {
            const float ground = sum / count;
            auto height = [&](int i) { return found[i] ? heights[i] : ground; };
            const float pitchTarget = atan2f((height(0) + height(1)) * .5f - (height(2) + height(3)) * .5f, max(d.wheelbase, .1f)) / RAD;
            const float rollTarget = atan2f((height(0) + height(2)) * .5f - (height(1) + height(3)) * .5f, max(d.track, .1f)) / RAD;
            // Cresting a rise at speed leaves the ground.
            const bool launch = v.grounded && v.climb > 6 && ground < v.pos.z - .15f;
            if(launch)
            {
                v.grounded = false;
                v.vel.z = min(v.climb, vec(v.vel.x, v.vel.y, 0).magnitude() * .8f);
                v.pitchVel = clamp(v.pitchVel, -120.0f, 120.0f);
                v.rollVel = clamp(v.rollVel, -120.0f, 120.0f);
            }
            else if(v.grounded || ground >= v.pos.z - .05f)
            {
                if(!v.grounded)
                {
                    // A hard landing thumps the suspension.
                    if(v.vel.z < -14) impact(v, index, def.suspensionSound, -v.vel.z / 40);
                    // Landing: on its back or side a Warthog has flipped (the others come down
                    // on their skids, hulls or tracks).
                    if(car && (fabsf(v.roll) > 75 || fabsf(v.pitch) > 75)) setFlipped(v, index, fabsf(v.roll) > 120 ? 180.0f : copysignf(100.0f, v.roll));
                    v.grounded = true;
                    v.vel.z = 0;
                }
                const float previousPitch = v.pitch, previousRoll = v.roll;
                // The rate it climbs, eased over a tenth of a second: a ramp's, not a single step's.
                const float rising = clamp((ground - v.pos.z) / max(dt, 1e-3f), -30.0f, 30.0f);
                v.climb += (rising - v.climb) * min(1.0f, dt * 10);
                v.pos.z = ground;
                if(!v.flipped)
                {
                    v.pitch += (pitchTarget - v.pitch) * min(1.0f, dt * 12);
                    v.roll += (rollTarget - v.roll) * min(1.0f, dt * 12);
                    v.pitchVel = (v.pitch - previousPitch) / max(dt, 1e-3f);
                    v.rollVel = (v.roll - previousRoll) / max(dt, 1e-3f);
                    // Standing too far over tips a Warthog onto its side.
                    if(car && (fabsf(rollTarget) > 58 || fabsf(pitchTarget) > 80)) setFlipped(v, index, copysignf(100.0f, rollTarget));
                }
                v.vel.z = 0;
            }
        }
        if(v.flipped && v.grounded) { v.vel.x *= max(0.0f, 1 - dt * 3); v.vel.y *= max(0.0f, 1 - dt * 3); }
        v.smoothZ = fabsf(v.pos.z - v.smoothZ) > 3 ? v.pos.z : v.smoothZ + (v.pos.z - v.smoothZ) * min(1.0f, dt * 18);
        // A crash: most of the speed lost in one step against a wall, a body or another vehicle.
        const float lost = speedBefore - vec(v.vel.x, v.vel.y, 0).magnitude();
        if(speedBefore > top * .25f && lost > max(8.0f, top * .35f)) impact(v, index, def.crashSound, lost / max(top, 1.0f));
    }

    // Whether a flying body can move by `delta` (any direction) from `pos`: the structure, the
    // machines and the other vehicles. `normal` gets the face it meets on retail collision, or the
    // way back on the cube grid.
    bool flightBlocked(int self, const Vehicle &v, const Dims &d, const vec &pos, const vec &delta, vec &normal)
    {
        normal = vec(0, 0, 0);
        const vec to = vec(pos).add(delta);
        vec back = vec(delta).mul(-1);
        if(!back.iszero()) back.normalize();
        if(overlapsVehicle(self, to, v.yaw, d) && !overlapsVehicle(self, pos, v.yaw, d)) { normal = back; return true; }
        const vec f = forwardOf(v.yaw), r = rightOf(v.yaw);
        const float radius = max(.3f, min(d.halfW, d.halfL) * .9f), height = max(2 * radius, d.height * .9f);
        const float offset = max(0.0f, d.halfL - radius);
        bool blocked = false;
        if(halocollisionactive())
        {
            float best = 2;
            for(float along : {offset, -offset})
            {
                const vec feet = vec(pos).add(vec(f).mul(d.centre + along)).add(vec(0, 0, .05f));
                HaloCollisionHit hit;
                if(!delta.iszero() && halocollisionsweep(feet, delta, radius, height, hit, false))
                {
                    // Already embedded (spawned on a seam): let it move out rather than stick.
                    HaloCollisionHit inside;
                    if(hit.fraction <= 0 && halocollisionoverlap(feet, radius, height, inside, false) && inside.penetration > .05f) continue;
                    blocked = true;
                    if(hit.fraction < best) { best = hit.fraction; normal = hit.normal; }
                }
                physent probe;
                probe.type = ENT_BOUNCE;
                probe.radius = radius;
                probe.eyeheight = height;
                probe.aboveeye = 0;
                probe.o = vec(feet).add(delta).add(vec(0, 0, height));
                float hi = 1e9f, lo = -1e9f;
                if(haloscenariocollide(&probe, hi, lo)) { blocked = true; if(normal.iszero()) normal = back; }
            }
            if(!normal.iszero()) normal.normalize();
            return blocked;
        }
        // The cube grid: the footprint's corners, edges and middle against solid cubes, floors
        // above the body's top and ceilings below it.
        for(float a : {-1.0f, 0.0f, 1.0f}) for(float b : {-1.0f, 0.0f, 1.0f})
        {
            const vec p = vec(to).add(vec(f).mul(d.centre + a * d.halfL * .9f)).add(vec(r).mul(b * d.halfW * .9f));
            float z;
            bool wall;
            // A floor higher than a step into the body is a wall too (a lower one it rests on).
            const bool floor = groundAt(p.x, p.y, to.z + d.height, -1e9f, z, wall);
            if(wall || (floor && z > to.z + d.step)) { normal = back; return true; }
            if(stockCeiling(p.x, p.y) < to.z + d.height) { normal = vec(0, 0, -1); return true; }
        }
        return false;
    }

    // One flight step of a piloted Banshee: no gravity; the heading and the nose follow the aim
    // at the Banshee's turn rates, it banks into turns, the throttle pushes along the nose (it
    // hovers with none), strafe and climb move it sideways and up. It slides along what it
    // meets and may rest on the floor, never sink into it. An EMP cuts its engines: it sinks.
    void fly(Vehicle &v, int index, float dt, const Drive &in)
    {
        const VehicleDef &def = *defOf(v);
        const Dims d = dims(v);
        const float top = topSpeed(def, d.s);
        const bool stunned = lastmillis < v.stunnedUntil;
        const float turn = clamp(wrap180(in.aimYaw - v.yaw), -100 * dt, 100 * dt);
        v.yaw = fmodf(v.yaw + turn + 360.0f, 360.0f);
        v.pitch += clamp(clamp(in.aimPitch, -55.0f, 55.0f) - v.pitch, -70 * dt, 70 * dt);
        const float bank = clamp(turn / max(dt, 1e-3f) * .35f + in.strafe * 25, -45.0f, 45.0f);
        v.roll += (bank - v.roll) * min(1.0f, dt * 4);
        v.pitchVel = v.rollVel = v.steer = v.climb = 0;
        const float yaw = v.yaw * RAD, pitch = v.pitch * RAD;
        const vec nose(sinf(yaw) * cosf(pitch), -cosf(yaw) * cosf(pitch), sinf(pitch));
        vec want(0, 0, 0);
        if(!stunned)
            want = vec(nose).mul(in.throttle > 0 ? in.throttle * top : in.throttle * top * .25f)
                       .add(vec(rightOf(v.yaw)).mul(in.strafe * top * .35f)).add(vec(0, 0, in.climb * top * .35f));
        vec change = vec(want).sub(v.vel);
        const float most = top / 1.6f * dt * (want.magnitude() < v.vel.magnitude() ? 1.4f : 1.0f);
        if(change.magnitude() > most) change.normalize().mul(most);
        v.vel.add(change);
        if(stunned) v.vel.z -= gravity * .5f * dt;
        vec delta = vec(v.vel).mul(dt);
        for(int pass = 0; pass < 3 && !delta.iszero(); ++pass)
        {
            vec normal;
            if(!flightBlocked(index, v, d, v.pos, delta, normal)) break;
            if(normal.iszero() || pass == 2) { delta = vec(0, 0, 0); v.vel.mul(.5f); break; }
            const float into = delta.dot(normal);
            if(into < 0) delta.sub(vec(normal).mul(into * 1.05f));
            const float against = v.vel.dot(normal);
            if(against < 0) v.vel.sub(vec(normal).mul(against));
        }
        v.pos.add(delta);
        if(!delta.iszero() && !shoveBodies(v, index, d)) { v.pos.sub(delta); v.vel.mul(.5f); }
        float ground;
        bool wall;
        if(groundAt(v.pos.x, v.pos.y, v.pos.z + d.step, v.pos.z - .05f, ground, wall) && ground > v.pos.z)
        {
            v.pos.z = ground;
            if(v.vel.z < 0) v.vel.z = 0;
        }
        v.grounded = false;
        v.smoothZ = v.pos.z;
    }

    bool outOfWorld(const Vehicle &v)
    {
        vec low, high;
        if(halocollisionactive() && halocollisionbounds(low, high)) return v.pos.z < low.z - 8;
        return v.pos.z < -130;
    }

    // Puts a vehicle down on the floor under a point: a step above it down to eight Halo units
    // below, else from higher up (never from above a low ceiling first: it would land on top).
    void settle(Vehicle &v, const vec &feet)
    {
        v.pos = feet;
        const Dims d = dims(v);
        float z;
        bool wall;
        if(groundAt(feet.x, feet.y, feet.z + d.step + .5f, feet.z - 8 * d.s, z, wall) ||
           groundAt(feet.x, feet.y, feet.z + d.height + 2, feet.z - 8 * d.s, z, wall)) v.pos.z = z;
        v.smoothZ = v.pos.z;
        v.vel = vec(0, 0, 0);
        v.pitch = v.roll = v.pitchVel = v.rollVel = v.climb = v.steer = 0;
        v.grounded = false;
        v.flipped = v.sleeping = false;
        v.stillSince = 0;
        v.stillZ = v.pos.z;
        v.airSince = v.embeddedSince = v.stalledSince = 0;
        v.stalledAt = v.pos;
    }

    // Whether the vehicle's body stands clear of walls, machines and other vehicles at `pos`.
    bool footprintFree(int self, const Vehicle &v, const vec &pos, bool player = true)
    {
        const Dims d = dims(v);
        if(overlapsVehicle(self, pos, v.yaw, d)) return false;
        // Not on the player standing there: the body would be shut inside the box.
        if(player && player1->state == CS_ALIVE && playerVehicle < 0)
        {
            const vec pf = forwardOf(v.yaw), pr = rightOf(v.yaw);
            const vec rel = vec(player1->o).sub(vec(pos).add(vec(pf).mul(d.centre)));
            const float feet = player1->o.z - player1->eyeheight;
            if(fabsf(rel.dot(pf)) < d.halfL + player1->radius && fabsf(rel.dot(pr)) < d.halfW + player1->radius &&
               feet < pos.z + d.height && feet + player1->eyeheight + player1->aboveeye > pos.z) return false;
        }
        if(halocollisionactive())
        {
            const vec f = forwardOf(v.yaw);
            const float radius = d.halfW * .95f, height = max(2 * radius, d.height - d.step);
            const float offset = max(0.0f, d.halfL - radius);
            for(float along : {offset, -offset})
            {
                HaloCollisionHit hit;
                const vec feet = vec(pos).add(vec(f).mul(d.centre + along)).add(vec(0, 0, d.step));
                if(halocollisionoverlap(feet, radius, height, hit, false) && hit.penetration > .05f) return false;
            }
            return true;
        }
        vec normal;
        return !chassisBlocked(self, v, d, pos, vec(0, 0, 0), normal);
    }

    // Settles a new vehicle at `feet`, or at the nearest spot around it where its body fits.
    void placeClear(Vehicle &v, int self, const vec &feet)
    {
        const Dims d = dims(v);
        const float yaw = v.yaw;
        for(float ring : {0.0f, 1.0f, 2.0f, 3.0f, 4.0f})
            loopi(ring > 0 ? 8 : 1)
            {
                const float angle = (v.yaw + 90 + i * 45) * RAD, reach = ring * d.halfW * 1.5f;
                settle(v, vec(feet).add(vec(cosf(angle) * reach, sinf(angle) * reach, 0)));
                v.yaw = yaw;
                if(footprintFree(self, v, v.pos)) return;
            }
        settle(v, feet);
        v.yaw = yaw;
    }

    vec seatPoint(const Vehicle &v, int seat)
    {
        const VehicleDef &d = *defOf(v);
        const float s = vehicleScale();
        if(seat >= 0 && seat < int(d.seats.size())) return modelPoint(v, d.seats[seat].at, s);
        return modelPoint(v, vec((d.low.x + d.high.x) * .5f, 0, d.high.z * .5f), s);
    }

    int seatByRole(const VehicleDef &d, int role)
    {
        for(int i = 0; i < int(d.seats.size()); ++i) if(d.seats[i].role == role) return i;
        return -1;
    }

    // A seat named by a script: W-driver, driver, gunner, ... (case-insensitive, by role first).
    int seatByName(const VehicleDef &d, const char *name)
    {
        if(!name || !*name) return d.seats.empty() ? -1 : 0;
        for(int i = 0; i < int(d.seats.size()); ++i) if(!strcasecmp(d.seats[i].name.c_str(), name) || !strcasecmp(d.seats[i].marker.c_str(), name)) return i;
        const int role = seatRole(name, "");
        if(role != HALO_SEAT_PASSENGER) return seatByRole(d, role);
        for(int i = 0; i < int(d.seats.size()); ++i) if(d.seats[i].role == HALO_SEAT_PASSENGER) return i;
        return -1;
    }

    // The rider at the controls: the first driver-role seat that is taken (the Ghost has two,
    // its own and an Elite's), alive.
    playerent *driverOf(const Vehicle &v)
    {
        const VehicleDef *d = defOf(v);
        if(!d) return nullptr;
        for(int i = 0; i < int(d->seats.size()) && i < maxSeats; ++i)
            if(d->seats[i].role == HALO_SEAT_DRIVER && v.riders[i] && v.riders[i]->state == CS_ALIVE) return v.riders[i];
        return nullptr;
    }

    // Someone flies or drives it: a Banshee with a pilot stays up.
    bool piloted(const Vehicle &v) { return driverOf(v) != nullptr; }

    // Engine loops: one per vehicle somebody drives, from the driver (or a turret's gunner while
    // it turns), its pitch rising with the speed, at the vehicle_engine class's gain. The ones
    // playing last frame that nobody drives now stop.
    struct EngineLoop { physent *owner; int sound; };
    std::vector<EngineLoop> engineLoops;

    int loopSound(const std::string &sample)
    {
        if(sample.empty()) return -1;
        defformatstring(name)("halo/%s", sample.c_str());
        defformatstring(file)("packages/audio/%s.wav", name);
        if(!fileexists(findfile(file, "rb"), "rb")) return -1;
        int id = audiomgr.findsound(name, 220, gamesounds);
        if(id < 0) id = audiomgr.addsound(name, 220, -1, true, gamesounds, true, 0);
        return id;
    }

    void stopEngineLoop(const EngineLoop &e)
    {
        physentreference ref(e.owner);
        if(location *l = audiomgr.locations.find(e.sound, &ref, gamesounds)) l->drop();
    }

    void updateEngineLoops()
    {
        std::vector<EngineLoop> now;
        if(haloactive() && !halocinematic() && !ispaused) loopv(vehicles)
        {
            Vehicle &v = vehicles[i];
            const VehicleDef *d = defOf(v);
            if(!v.exists || !d || d->engineSound.empty()) continue;
            playerent *owner = nullptr;
            float level = 0;
            if(d->vehicleClass == HALO_VEHICLE_TURRET)
            {
                // The turning rate over at least 50 ms (degrees per second), held for 250 ms, so
                // the loop does not start and stop with the frame rate.
                playerent *gunner = v.riders[0] && v.riders[0]->state == CS_ALIVE ? v.riders[0] : nullptr;
                if(!gunner || lastmillis < v.loopYawAt) { v.loopYaw = v.yaw; v.loopYawAt = lastmillis; v.loopTurnedAt = 0; }
                else if(lastmillis - v.loopYawAt >= 50)
                {
                    const float rate = fabsf(wrap180(v.yaw - v.loopYaw)) * 1000.0f / float(lastmillis - v.loopYawAt);
                    v.loopYaw = v.yaw;
                    v.loopYawAt = lastmillis;
                    if(rate > 15) { v.loopTurnedAt = lastmillis; v.loopLevel = clamp(rate / 240, .3f, 1.0f); }
                }
                if(gunner && v.loopTurnedAt && lastmillis - v.loopTurnedAt < 250) { owner = gunner; level = v.loopLevel; }
            }
            else if(drivenClass(d->vehicleClass) && !v.flipped)
            {
                owner = driverOf(v);
                level = clamp(v.vel.magnitude() / max(topSpeed(*d, dims(v).s), 1.0f), 0.0f, 1.0f);
            }
            if(!owner) continue;
            const int id = loopSound(d->engineSound);
            if(id < 0) continue;
            // Counted whether or not a channel is free (or sound is on at all): QA reads the count.
            now.push_back({owner, id});
            physentreference ref(owner);
            location *l = audiomgr.locations.find(id, &ref, gamesounds);
            if(!l) l = audiomgr._playsound(id, ref, SP_NORMAL, 0, true);
            if(!l) continue;
            l->pitch(.8f + .5f * level);
            l->setvolume(halofxclassgain(HALO_SOUND_CLASS_VEHICLE_ENGINE) * (.65f + .35f * level));
        }
        for(const EngineLoop &old : engineLoops)
        {
            bool kept = false;
            for(const EngineLoop &e : now) if(e.owner == old.owner && e.sound == old.sound) { kept = true; break; }
            if(!kept) stopEngineLoop(old);
        }
        engineLoops.swap(now);
    }

    void placePlayerOnSeat()
    {
        if(playerVehicle < 0) return;
        const Vehicle &v = vehicles[playerVehicle];
        player1->o = seatPoint(v, playerSeat).add(vec(0, 0, .45f * player1->eyeheight));
        seatEye = player1->o;
        player1->vel = vec(0, 0, 0);
        player1->onfloor = true;
        player1->timeinair = 0;
        player1->resetinterp();
    }

    void settle(Vehicle &v, const vec &feet);

    // Something moved the seated player (a QA teleport, a script's object_teleport of the
    // player): the vehicle goes with them.
    void followPlayer()
    {
        if(playerVehicle < 0 || player1->o.dist(seatEye) <= .5f) return;
        Vehicle &v = vehicles[playerVehicle];
        const float yaw = v.yaw;
        settle(v, vec(v.pos).add(vec(player1->o).sub(seatEye)));
        v.yaw = yaw;
        v.sleeping = false;
        placePlayerOnSeat();
    }

    bool seatPlayer(int index, int seat)
    {
        if(!vehicles.inrange(index) || player1->state != CS_ALIVE) return false;
        Vehicle &v = vehicles[index];
        const VehicleDef *d = defOf(v);
        if(!v.exists || v.flipped || !d || seat < 0 || seat >= int(d->seats.size()) || d->vehicleClass == HALO_VEHICLE_PROP) return false;
        if(v.riders[seat] && v.riders[seat] != player1) return false;
        if(playerVehicle >= 0 && playerVehicle != index) vehicles[playerVehicle].riders[playerSeat] = nullptr;
        else if(playerVehicle == index && playerSeat >= 0) v.riders[playerSeat] = nullptr;
        // Into the seat: the floor recovery (halo.cpp) forgets the floors walked before, keeping
        // the one the player got in from (where they go if the vehicle falls out of the world).
        if(playerVehicle < 0)
        {
            const vec boarded = vec(player1->o).sub(vec(0, 0, player1->eyeheight));
            haloplayermoved("into a vehicle seat", &boarded);
        }
        playerVehicle = index;
        playerSeat = seat;
        v.riders[seat] = player1;
        secondaryHeld = false;
        // Seated again: an earlier forced exit no longer calls for a reclaim.
        thrownVehicle = thrownSeat = -1;
        v.sleeping = false;
        v.aimYaw = player1->yaw;
        v.aimPitch = player1->pitch;
        if(!haloweapon(v.gun.def)) v.gun.reset(haloweaponfind(d->weaponPath.c_str()));
        player1->attacking = false;
        player1->stopmoving();
        placePlayerOnSeat();
        conoutf("Halo vehicle: player in the %s's %s seat", d->displayName.c_str(), d->seats[seat].name.c_str());
        return true;
    }

    // Whether the player's body fits standing with its feet near `feet`; drops `feet` to the floor.
    bool bodyFits(vec &feet) { return bodyFitsAt(player1, feet, true); }

    // Out of the seat onto the ground beside the vehicle: the seat's entry point first, then
    // either side, behind, ahead and on top. `force`: when nothing fits, out where the seat is.
    // `moved`: the floor recovery treats it as a move (false only when the vehicle fell out of the
    // world and the player goes straight back to the floor they got in from).
    bool unseatPlayer(bool force, bool moved = true)
    {
        if(playerVehicle < 0) return false;
        Vehicle &v = vehicles[playerVehicle];
        const VehicleDef &d = *defOf(v);
        const Dims dm = dims(v);
        v.riders[playerSeat] = nullptr;
        const int seat = playerSeat, index = playerVehicle;
        playerVehicle = playerSeat = -1;
        secondaryHeld = false;
        const float clear = player1->radius + .4f;
        std::vector<vec> candidates;
        const vec f = forwardOf(v.yaw), r = rightOf(v.yaw);
        const vec centre = vec(v.pos).add(vec(f).mul(dm.centre));
        if(seat >= 0 && seat < int(d.seats.size()))
        {
            vec enter = modelPoint(v, d.seats[seat].enter, dm.s);
            vec out = vec(enter).sub(v.pos);
            out.z = 0;
            if(!out.iszero()) enter.add(out.normalize().mul(clear));
            // A seat whose entry marker is inside the body (a turret's is its seat) gets out at the
            // sides instead.
            const vec rel = vec(enter).sub(centre);
            if(fabsf(rel.dot(f)) >= dm.halfL + player1->radius || fabsf(rel.dot(r)) >= dm.halfW + player1->radius)
                candidates.push_back(enter);
        }
        for(const vec &offset : {vec(r).mul(-(dm.halfW + clear)), vec(r).mul(dm.halfW + clear), vec(f).mul(-(dm.halfL + clear)),
                                 vec(f).mul(dm.halfL + clear), vec(r).mul(-(dm.halfW + clear * 3)), vec(r).mul(dm.halfW + clear * 3)})
            candidates.push_back(vec(centre).add(offset));
        candidates.push_back(vec(centre).add(vec(0, 0, dm.height + .1f)));
        for(vec feet : candidates)
        {
            if(qaForceThrow || !bodyFits(feet)) continue;
            player1->o = vec(feet).add(vec(0, 0, player1->eyeheight));
            player1->vel = vec(v.vel.x, v.vel.y, 0).mul(.02f);
            player1->resetinterp();
            if(moved) haloplayermoved("out of a vehicle seat");
            conoutf("Halo vehicle: player out of the %s", d.displayName.c_str());
            return true;
        }
        if(force)
        {
            // Onto the top. If the body is wedged there, halo.cpp's recovery puts it back in this
            // seat (halovehiclereclaim) rather than leave it in place or revert.
            player1->o = vec(centre).add(vec(0, 0, (v.flipped ? dm.height * .6f : dm.height) + .02f + player1->eyeheight));
            player1->vel = vec(0, 0, 0);
            player1->resetinterp();
            if(moved) haloplayermoved("thrown out of a vehicle");
            thrownVehicle = index;
            thrownSeat = seat;
            thrownAt = lastmillis;
            conoutf("Halo vehicle: player thrown clear of the %s", d.displayName.c_str());
            return true;
        }
        // No room: stay in.
        playerVehicle = index;
        playerSeat = seat;
        v.riders[seat] = player1;
        conoutf("Halo vehicle: no room to get out of the %s", d.displayName.c_str());
        return false;
    }

    void ejectRiders(Vehicle &v, int index)
    {
        loopi(maxSeats)
        {
            playerent *p = v.riders[i];
            if(!p) continue;
            if(p == player1 && playerVehicle == index) unseatPlayer(true);
            else v.riders[i] = nullptr;
        }
    }

    bool difficultyAllows(int flags)
    {
        // Retail not-placed flags: 2 on easy, 4 on normal, 8 on hard (Legendary follows hard).
        static const int bits[4] = {2, 4, 8, 8};
        return !(flags & bits[halodifficulty()]);
    }

    void respawn(Vehicle &v, bool exists)
    {
        for(playerent *&p : v.riders) if(p && p != player1) p = nullptr;
        v.exists = exists;
        v.health = 1;
        v.destroyed = false;
        v.carried = false;
        v.gun = HaloWeaponState();
        settle(v, v.spawnFeet);
        v.yaw = v.spawnYaw;
        v.aimYaw = v.yaw;
        v.safeCount = v.lastRecovery = 0;
    }

    int add(int type, const vec &feet, float yaw, int object, int flags)
    {
        loadDefs();
        if(type < 0 || type >= int(defs.size())) return -1;
        Vehicle v;
        v.type = type;
        v.object = object;
        v.flags = flags;
        v.spawnFeet = feet;
        v.spawnYaw = yaw;
        vehicles.add(v);
        return vehicles.length() - 1;
    }

    // Stuck recovery, as halo.cpp's updateRecovery does for the player on foot: a vehicle that
    // falls out of the world, stays wedged in the structure or hangs in mid-air goes back to the
    // nearest spot round it where it fits upright, or to the last spot it stood on safely.

    // All four wheels on structure floor near the vehicle's height: not on a lift deck or a light
    // bridge, which move or vanish. Always true on the cube grid.
    bool onStructure(const Vehicle &v, const Dims &d)
    {
        if(!halocollisionactive()) return true;
        const vec f = forwardOf(v.yaw), r = rightOf(v.yaw);
        const float reach = d.step + d.halfL * .6f;
        loopi(4)
        {
            const vec w = vec(v.pos).add(vec(f).mul(i & 1 ? d.wheelFront : d.wheelRear)).add(vec(r).mul(i & 2 ? d.wheelSide : -d.wheelSide));
            HaloCollisionHit hit;
            float ground;
            bool wall;
            if(!halocollisiontrace(vec(w.x, w.y, v.pos.z + reach), vec(w.x, w.y, v.pos.z - reach), hit) || hit.normal.z < .5f ||
               !groundAt(w.x, w.y, v.pos.z + reach, v.pos.z - reach, ground, wall)) return false;
            // The structure's floor is the highest under the wheel, not a device deck above it.
            if(v.pos.z + reach - 2 * reach * hit.fraction < ground - .1f) return false;
        }
        return true;
    }

    // Floor (structure or device) under all four wheels near the vehicle's height (a slope's
    // worth above or below: the height is the one under its middle).
    bool wheelsGrounded(const Vehicle &v, const Dims &d)
    {
        const vec f = forwardOf(v.yaw), r = rightOf(v.yaw);
        const float reach = d.step + d.halfL * .6f;
        loopi(4)
        {
            const vec w = vec(v.pos).add(vec(f).mul(i & 1 ? d.wheelFront : d.wheelRear)).add(vec(r).mul(i & 2 ? d.wheelSide : -d.wheelSide));
            float z;
            bool wall;
            if(!groundAt(w.x, w.y, v.pos.z + reach, v.pos.z - reach, z, wall)) return false;
        }
        return true;
    }

    // The chassis overlaps the structure by more than a tenth of a unit. The capsules' bottoms
    // ride up with the pitch and roll, so a vehicle on a steep ramp is not taken for wedged.
    bool chassisEmbedded(const Vehicle &v, const Dims &d)
    {
        if(!halocollisionactive() || v.flipped) return false;
        const vec f = forwardOf(v.yaw);
        const float radius = d.halfW * .8f, offset = max(0.0f, d.halfL - d.halfW * .95f);
        const float lift = d.step + fabsf(sinf(v.pitch * RAD)) * offset + fabsf(sinf(v.roll * RAD)) * radius;
        const float height = max(2 * radius, d.height - lift);
        for(float along : {offset, -offset})
        {
            HaloCollisionHit hit;
            const vec feet = vec(v.pos).add(vec(f).mul(d.centre + along)).add(vec(0, 0, lift));
            if(halocollisionoverlap(feet, radius, height, hit, false) && hit.penetration > .1f) return true;
        }
        return false;
    }

    // Every quarter second: a grounded, upright, clear spot on structure floor is safe.
    void trackSafe(Vehicle &v, int index, const Dims &d)
    {
        if(lastmillis - v.safeChecked < 250) return;
        v.safeChecked = lastmillis;
        if(!v.grounded || v.flipped || v.carried || fabsf(v.pitch) > 30 || fabsf(v.roll) > 30) return;
        if(!onStructure(v, d) || chassisEmbedded(v, d) || !footprintFree(index, v, v.pos, false)) return;
        if(v.safeCount && v.safePos[v.safeCount - 1].dist(v.pos) < max(2.0f, d.halfL))
        {
            v.safePos[v.safeCount - 1] = v.pos;
            v.safeYaw[v.safeCount - 1] = v.yaw;
            return;
        }
        if(v.safeCount == safeSpotCount)
        {
            for(int i = 1; i < safeSpotCount; ++i) { v.safePos[i - 1] = v.safePos[i]; v.safeYaw[i - 1] = v.safeYaw[i]; }
            --v.safeCount;
        }
        v.safePos[v.safeCount] = v.pos;
        v.safeYaw[v.safeCount++] = v.yaw;
    }

    void clearStuck(Vehicle &v)
    {
        v.airSince = v.embeddedSince = v.stalledSince = 0;
        v.stalledAt = v.pos;
        v.stillZ = v.pos.z;
    }

    // Puts the vehicle down upright at `pos` facing `yaw` when it fits there with floor under its
    // wheels. `player`: keep clear of the player on foot as well.
    bool tryPlace(Vehicle &v, int index, const vec &pos, float yaw, bool player)
    {
        Vehicle test = v;
        settle(test, pos);
        test.yaw = yaw;
        const Dims d = dims(test);
        if(!footprintFree(index, test, test.pos, player) || chassisEmbedded(test, d) || !wheelsGrounded(test, d)) return false;
        settle(v, pos);
        v.yaw = yaw;
        v.aimYaw = yaw;
        clearStuck(v);
        return true;
    }

    // The nearest spot round it (in place first, then rings out to two widths) unless it is
    // falling, else its safe spots from the newest. A second recovery within five seconds means
    // the newest spot did not hold: it goes further back. The seated player goes with it.
    bool recoverVehicle(Vehicle &v, int index, const char *why, bool falling, bool player = true)
    {
        const VehicleDef &def = *defOf(v);
        const Dims d = dims(v);
        if(v.lastRecovery && lastmillis - v.lastRecovery < 5000 && v.safeCount) --v.safeCount;
        v.lastRecovery = lastmillis;
        bool placed = false;
        const float yaw = v.yaw;
        if(!falling)
        {
            const vec at = v.pos;
            for(float ring : {0.0f, .5f, 1.0f, 1.5f, 2.0f})
            {
                loopi(ring > 0 ? 8 : 1)
                {
                    const float angle = i * 45 * RAD, reach = ring * max(d.width, 1.0f);
                    if(tryPlace(v, index, vec(at).add(vec(cosf(angle) * reach, sinf(angle) * reach, 0)), yaw, player)) { placed = true; break; }
                }
                if(placed) break;
            }
        }
        while(!placed && v.safeCount)
        {
            if(tryPlace(v, index, v.safePos[v.safeCount - 1], v.safeYaw[v.safeCount - 1], player)) placed = true;
            else --v.safeCount;
        }
        if(!placed) return false;
        if(index == playerVehicle) placePlayerOnSeat();
        conoutf("Halo vehicle recovery: %s, the %s put back at %.1f,%.1f,%.1f", why, def.displayName.c_str(), v.pos.x, v.pos.y, v.pos.z);
        return true;
    }

    // Falls, wedges and stalls of a vehicle that is not asleep. Turrets and the parked props
    // never move by themselves: only a fall out of the world moves them, and they stay where the
    // scenario put them however close to a railing that is.
    void updateStuck(Vehicle &v, int index)
    {
        // Hanging from a Pelican's hook (or moved by a recording) is neither a fall nor a stall.
        if(v.carried) { clearStuck(v); return; }
        const VehicleDef &def = *defOf(v);
        const Dims d = dims(v);
        // Everything that drives keeps safe spots and gets the wedge and stall checks; a flying
        // Banshee neither stalls (it hovers) nor falls for want of floor below (it flies).
        const bool car = drivenClass(def.vehicleClass);
        const bool flying = def.vehicleClass == HALO_VEHICLE_FLYER && piloted(v);
        if(car) trackSafe(v, index, d);
        const char *why = nullptr;
        bool falling = false;
        vec low, high;
        if(outOfWorld(v)) { why = "fell out of the world"; falling = true; }
        else if(halocollisionactive() && halocollisionbounds(low, high))
        {
            if(v.grounded) v.airSince = 0;
            else if(!v.airSince) v.airSince = lastmillis;
            if(flying) v.airSince = v.stalledSince = 0;
            HaloCollisionHit hit;
            // In the air for two seconds with neither structure nor device below: a hole in the mesh.
            if(v.airSince && lastmillis - v.airSince > fallTime && !haloscenariotrace(v.pos, vec(v.pos.x, v.pos.y, low.z - 8), hit))
            {
                why = "fell out of the world";
                falling = true;
            }
            else if(car)
            {
                // Wedged: the chassis in the structure, and the vehicle not getting out of it.
                if(flying || !chassisEmbedded(v, d) || vec(v.vel.x, v.vel.y, 0).magnitude() > 2) v.embeddedSince = 0;
                else if(!v.embeddedSince) v.embeddedSince = lastmillis;
                // Hung in mid-air (caught on an edge or in a crease) without moving at all.
                if(v.grounded || flying || v.pos.dist(v.stalledAt) > .05f) { v.stalledSince = 0; v.stalledAt = v.pos; }
                else if(!v.stalledSince) v.stalledSince = lastmillis;
                if(v.embeddedSince && lastmillis - v.embeddedSince >= wedgeTime) why = "wedged in the collision";
                else if(v.stalledSince && lastmillis - v.stalledSince >= stallTime) why = "stuck in mid-air";
            }
        }
        if(!why || recoverVehicle(v, index, why, falling)) return;
        // Nowhere to put it: look again in five seconds.
        v.embeddedSince = v.embeddedSince ? lastmillis + 5000 - wedgeTime : 0;
        v.stalledSince = v.stalledSince ? lastmillis + 5000 - stallTime : 0;
        v.airSince = v.airSince ? lastmillis + 5000 - fallTime : 0;
        if(index == playerVehicle)
        {
            conoutf("Halo vehicle recovery: %s, no safe spot for the %s, driver put out", why, def.displayName.c_str());
            if(!falling) { if(!unseatPlayer(false)) unseatPlayer(true); return; }
            // Out of the world with no spot for the vehicle: the player goes back to the floor
            // they got in from, or dies and reverts as before.
            unseatPlayer(true, false);
            // Not back into this one later (halovehiclereclaim): it is out of the world, and goes
            // back to its placement or away on the next frame.
            thrownVehicle = thrownSeat = -1;
            if(!halorecoverplayer(why)) dodamage(INT_MAX, player1, player1, GUN_GRENADE);
            return;
        }
        if(!falling) return;
        ejectRiders(v, index);
        if(v.placed)
        {
            Vehicle test = v;
            settle(test, v.spawnFeet);
            test.yaw = test.spawnYaw;
            if(footprintFree(index, test, test.pos))
            {
                respawn(v, true);
                conoutf("Halo vehicle recovery: the %s fell out of the world, back where it was placed", def.displayName.c_str());
                return;
            }
        }
        v.exists = false;
        conoutf("Halo vehicle recovery: the %s fell out of the world and is gone", def.displayName.c_str());
    }

    // A body part way inside a vehicle's box (the vehicle created, spawned, teleported or
    // reverted over it, an exit spot, a lift) goes out by the shortest way that has room: each
    // face at the box's edge, its corners and each face a body's width further out, nearest
    // first, then the roof when the feet are within a step of it. First onto floor, then (over a
    // ledge) anywhere the body fits. False when there is no room anywhere.
    bool pushOut(const Vehicle &v, physent *b, bool isPlayer)
    {
        const Dims d = dims(v);
        vec push;
        if(!bodyInside(v, d, b, push, .1f)) return true;
        const vec f = forwardOf(v.yaw), r = rightOf(v.yaw);
        const vec rel = vec(b->o).sub(vec(v.pos).add(vec(f).mul(d.centre)));
        const float along = rel.dot(f), side = rel.dot(r);
        const float ex = d.halfL + b->radius + .1f, ey = d.halfW + b->radius + .1f;
        const vec feet = vec(b->o).sub(vec(0, 0, b->eyeheight));
        struct Way { vec delta; float distance; bool roof; };
        std::vector<Way> ways;
        auto out = [&](float a, float s)
        {
            vec delta = vec(f).mul(a - along).add(vec(r).mul(s - side));
            delta.z = 0;
            ways.push_back({delta, delta.magnitude(), false});
        };
        for(float extra : {0.0f, 2 * b->radius + .5f})
        {
            out(ex + extra, side);
            out(-ex - extra, side);
            out(along, ey + extra);
            out(along, -ey - extra);
        }
        for(float a : {ex, -ex}) for(float s : {ey, -ey}) out(a, s);
        const float rise = vehicleTop(v, d) - feet.z;
        if(rise > 0 && rise <= 1.26f) ways.push_back({vec(0, 0, rise + .02f), rise, true});
        std::stable_sort(ways.begin(), ways.end(), [](const Way &x, const Way &y) { return x.distance < y.distance; });
        // A way may reach further than the body is wide: it must not pass through a thin wall
        // (or a closed door) into the room beyond. Checked at the body's middle, above a step.
        const float mid = max(1.3f, (b->eyeheight + b->aboveeye) * .5f);
        for(int pass = 0; pass < 2; ++pass)
            for(const Way &w : ways)
            {
                if(w.roof && pass) continue;
                vec to = vec(feet).add(w.delta);
                if(!bodyFitsAt(b, to, !pass && !w.roof)) continue;
                const vec from = vec(feet).add(vec(0, 0, mid)), at = vec(to).add(vec(0, 0, mid));
                if(from.dist(at) > .01f && !raycubelos(from, at)) continue;
                b->o = vec(to).add(vec(0, 0, b->eyeheight));
                b->vel.x = b->vel.y = 0;
                b->resetinterp();
                if(isPlayer) conoutf("Halo vehicle: player pushed out of the %s (%.1f units)", defOf(v)->displayName.c_str(), w.distance);
                else if(halo_vehicletrace) conoutf("Halo vehicle: actor pushed out of the %s (%.1f units)", defOf(v)->displayName.c_str(), w.distance);
                return true;
            }
        return false;
    }

    // Once a frame, after the vehicles moved: every body left inside a vehicle goes out of it.
    // One with no room anywhere may walk out of that vehicle (halovehiclecollide lets it) and is
    // tried again every quarter second.
    void depenetrate()
    {
        std::vector<Trap> was;
        was.swap(trapped);
        if(!halo_vehiclecollide || halocinematic()) return;
        loopv(vehicles)
        {
            const Vehicle &v = vehicles[i];
            if(!v.exists || !defOf(v) || v.carried) continue;
            const Dims d = dims(v);
            eachBody([&](physent *b, bool isPlayer)
            {
                vec push;
                if(!bodyInside(v, d, b, push, 0)) return;
                const Trap *old = nullptr;
                for(const Trap &t : was) if(t.body == b && t.vehicle == i) old = &t;
                if(old && lastmillis < old->retry) { trapped.push_back(*old); return; }
                if(pushOut(v, b, isPlayer)) return;
                if(!old && (isPlayer || halo_vehicletrace))
                    conoutf("Halo vehicle: %s inside the %s has no room to get out; it may walk out", isPlayer ? "player" : "actor",
                            defOf(v)->displayName.c_str());
                trapped.push_back({b, i, lastmillis + 250});
            });
        }
    }

    int find(int object)
    {
        if(object < 0) return -1;
        loopv(vehicles) if(vehicles[i].object == object) return i;
        return -1;
    }

    float playerReach(const Vehicle &v) { const Dims d = dims(v); return max(4.0f, d.halfW + player1->radius + 2.5f); }

    // The nearest seat the player can take, by its entry point. Returns the vehicle; `seat` the seat.
    int nearestSeat(int &seat, float &distance, bool flippedToo)
    {
        int best = -1;
        seat = -1;
        distance = 1e9f;
        const vec feet = vec(player1->o).sub(vec(0, 0, player1->eyeheight));
        loopv(vehicles)
        {
            const Vehicle &v = vehicles[i];
            const VehicleDef *d = defOf(v);
            if(!v.exists || !d || d->vehicleClass == HALO_VEHICLE_PROP || v.carried || (!v.enterable && !v.flipped)) continue;
            // The Wraith is the Covenant's alone; a vehicle a hostile drives cannot be taken (Halo
            // CE has no boarding). With a friendly driver (a marine at a Warthog's wheel) the free
            // gunner seat is still the player's; the driver's seat is taken, so the loop below
            // skips it.
            if(!d->player && !v.flipped) continue;
            const playerent *driver = driverOf(v);
            if(driver && driver != player1 && !v.flipped && !haloteamfriendly(haloactorteam(driver))) continue;
            const Dims dm = dims(v);
            if(feet.z > v.pos.z + dm.height + 2 || feet.z + player1->eyeheight < v.pos.z - 1) continue;
            const float reach = playerReach(v);
            if(v.flipped)
            {
                if(!flippedToo) continue;
                const float dd = feet.distxy(vec(v.pos).add(vec(forwardOf(v.yaw)).mul(dm.centre))) - dm.halfL;
                if(dd < reach && dd < distance) { distance = dd; best = i; seat = -1; }
                continue;
            }
            for(int j = 0; j < int(d->seats.size()); ++j)
            {
                const SeatDef &s = d->seats[j];
                if(s.role == HALO_SEAT_PASSENGER || v.riders[j]) continue;
                // One driver's seat for the player: the Ghost's second is the Elite's.
                if(s.role == HALO_SEAT_DRIVER && j != seatByRole(*d, HALO_SEAT_DRIVER)) continue;
                const float dd = feet.distxy(modelPoint(v, s.enter, dm.s));
                if(dd < reach && dd < distance) { distance = dd; best = i; seat = j; }
            }
        }
        return best;
    }

    void flipBack(Vehicle &v, int index)
    {
        v.flipped = false;
        v.roll = v.pitch = v.rollVel = v.pitchVel = 0;
        v.vel = vec(0, 0, 0);
        v.pos.z += dims(v).height * .3f;
        v.grounded = false;
        v.sleeping = false;
        audiomgr.playsound(S_ITEMAMMO, SP_HIGH);
        conoutf("Halo vehicle: %s flipped back", defOf(v)->displayName.c_str());
        // Upside down where it does not fit upright (in a crease, against a ceiling, half over a
        // ledge): beyond what flipping it back does, it goes to the nearest spot where it fits,
        // or its last safe spot. The player beside it is pushed clear if need be.
        Vehicle test = v;
        settle(test, v.pos);
        const Dims d = dims(test);
        if(!footprintFree(index, test, test.pos, false) || chassisEmbedded(test, d) || !wheelsGrounded(test, d))
            recoverVehicle(v, index, "flipped over where it cannot be righted", false, false);
    }

    playerent *aliveBot(playerent *p)
    {
        if(!p || p == player1) return nullptr;
        loopv(players) if(players[i] == p) return p->state == CS_ALIVE ? p : nullptr;
        return nullptr;
    }

    // The seats that work the vehicle's gun: a turret's or a Warthog's gunner, the driver of a
    // Ghost, Wraith, Scorpion or Banshee.
    bool armedSeat(const VehicleDef &d, int seat)
    {
        if(seat < 0 || seat >= int(d.seats.size()) || d.weaponPath.empty()) return false;
        const int role = d.seats[seat].role;
        if(d.vehicleClass == HALO_VEHICLE_TURRET) return role == HALO_SEAT_GUNNER;
        if(d.vehicleClass == HALO_VEHICLE_CAR) return role == HALO_SEAT_GUNNER;
        return driverFires(d) && role == HALO_SEAT_DRIVER;
    }

    // Guns fixed in the hull fire along the nose: the Ghost's and the Banshee's bolts (not a
    // lobbed mortar, the tank's turret or a gunner's mount).
    bool fixedGun(const VehicleDef &d, const HaloTriggerDef *t)
    {
        return (d.vehicleClass == HALO_VEHICLE_HOVER || d.vehicleClass == HALO_VEHICLE_FLYER) && !(t && t->projectile.gravity > 0);
    }

    // Milliseconds between shots of a trigger: its rate, or for a trigger fired as fast as it is
    // pulled (rate 0: the Scorpion's cannon, the Banshee's fuel rod) a pause that keeps an
    // explosive round from leaving every frame.
    int triggerDelay(const HaloWeaponState &state, const HaloTriggerDef &t)
    {
        if(t.rate + t.finalRate > .02f) return state.delay(t);
        return t.projectile.radius > 0 ? 1500 : 400;
    }

    vec noseOf(const Vehicle &v)
    {
        const float yaw = v.yaw * RAD, pitch = v.pitch * RAD;
        return vec(sinf(yaw) * cosf(pitch), -cosf(yaw) * cosf(pitch), sinf(pitch));
    }

    // Where the seat's shots leave: a turret's barrel, the front of a hover craft or a Banshee,
    // the top of the tank's turret, a Warthog gunner's mount.
    vec muzzle(const Vehicle &v, const Dims &dm, int seat, const vec &aim)
    {
        const VehicleDef &d = *defOf(v);
        if(d.vehicleClass == HALO_VEHICLE_TURRET) return vec(v.pos).add(vec(0, 0, dm.height * .75f)).add(vec(aim).mul(dm.halfL));
        if(driverFires(d) && seat >= 0 && seat < int(d.seats.size()) && d.seats[seat].role == HALO_SEAT_DRIVER)
        {
            const bool tank = d.vehicleClass == HALO_VEHICLE_TANK;
            return vec(v.pos).add(vec(forwardOf(v.yaw)).mul(dm.centre + (tank ? 0 : dm.halfL * .9f)))
                       .add(vec(0, 0, dm.height * (tank ? .8f : .45f))).add(vec(aim).mul(tank ? dm.halfL : dm.halfW * .3f));
        }
        return seatPoint(v, seat).add(vec(0, 0, dm.height * .25f)).add(vec(aim).mul(dm.halfW));
    }

    // The vehicle's gun from weapons.txt (seating the player sets it; an actor's vehicle gets it
    // on its first shot), or null.
    const HaloWeaponDef *gunOf(Vehicle &v)
    {
        if(!haloweapon(v.gun.def))
        {
            const VehicleDef *d = defOf(v);
            const HaloWeaponDef *w = d && !d->weaponPath.empty() ? haloweaponfind(d->weaponPath.c_str()) : nullptr;
            if(!w) return nullptr;
            v.gun.reset(w);
        }
        return haloweapon(v.gun.def);
    }

    // The actor variant an actor seated at this vehicle's gun fights as (actors.txt: "vehicles\
    // banshee\banshee pilot", "... c gun turret gunner"), or null. As in Halo, an actor that
    // takes a vehicle's gun seat fires with the vehicle variant's damage modifier and error, not
    // its own infantry variant's (an Elite's). Without a `variant` line (an older vehicles.txt)
    // the variant named "<vehicle tag> pilot" or "gunner" is used if actors.txt has it.
    const HaloActorDef *variantOf(const VehicleDef &d)
    {
        if(!d.variantPath.empty()) return haloactorfind(d.variantPath.c_str());
        if(d.path.empty()) return nullptr;
        for(const char *role : {" pilot", " gunner"})
            if(const HaloActorDef *a = haloactorfind((d.path + role).c_str())) return a;
        return nullptr;
    }

    // The vehicle variant when its fire block is authored (a damage modifier above 0), else null.
    // The Warthog gunner's fire block is all zeros (no modifier, rate or error): it stands for
    // nothing, so its gunner fights as the seated actor's own variant, error included (a marine
    // at the gun would otherwise never miss).
    const HaloActorDef *fightingVariant(const VehicleDef &d)
    {
        const HaloActorDef *a = variantOf(d);
        return a && a->damageScale > 0 ? a : nullptr;
    }

    // The damage modifier an actor's shot from this vehicle's gun carries: the vehicle variant's,
    // or -1 (rollshot then takes the seated actor's own; the player's shots carry 1). A variant
    // with no modifier (0: the Warthog gunner's) falls back to the actor's too.
    float gunModifier(const VehicleDef &d, const playerent *owner)
    {
        if(!owner || owner == player1) return -1;
        const HaloActorDef *a = fightingVariant(d);
        return a ? a->damageScale : -1;
    }

    // One shot of trigger `trigger` (0 or 1) of the vehicle's gun by `owner` in `seat`, towards
    // `target`. A gun fixed in the hull fires within 25 degrees of the nose, else along it.
    bool shoot(Vehicle &v, playerent *owner, int seat, int trigger, const vec &target)
    {
        const HaloWeaponDef *gun = gunOf(v);
        if(!gun) return false;
        HaloWeaponState &state = trigger ? v.gun2 : v.gun;
        int &next = trigger ? v.nextShot2 : v.nextShot;
        if(state.def != gun->index) state.reset(gun);
        const HaloTriggerDef *t = gun->trigger(trigger);
        if(!t || !t->projectile.valid || lastmillis < next || !state.canfire(*gun) || lastmillis < v.stunnedUntil) return false;
        if(!gun->usesBattery() && state.loaded <= 0 && gun->magazine()) state.loaded = max(1, gun->magazine()->loaded);
        const Dims dm = dims(v);
        const VehicleDef &d = *defOf(v);
        vec aim = vec(target).sub(v.pos);
        if(aim.iszero()) aim = noseOf(v);
        aim.normalize();
        const vec from = muzzle(v, dm, seat, aim);
        aim = vec(target).sub(from);
        if(aim.iszero()) aim = noseOf(v);
        aim.normalize();
        if(fixedGun(d, t))
        {
            // Turn the shot back inside the cone round the nose.
            const float yaw = atan2f(aim.x, -aim.y) / RAD, off = wrap180(yaw - v.yaw);
            if(fabsf(off) > 25)
            {
                const float clamped = (v.yaw + clamp(off, -25.0f, 25.0f)) * RAD, pitch = asinf(clamp(aim.z, -1.0f, 1.0f));
                aim = vec(sinf(clamped) * cosf(pitch), -cosf(clamped) * cosf(pitch), sinf(pitch));
            }
        }
        const float reach = max(from.dist(target), 1.0f);
        halofiretrigger(owner, *gun, state, trigger, from, vec(from).add(vec(aim).mul(reach)), gunModifier(d, owner));
        next = lastmillis + triggerDelay(state, *t);
        const char *sample = trigger && gun->sound(HALO_SOUND_CHARGED) ? gun->sound(HALO_SOUND_CHARGED) : gun->sound(HALO_SOUND_FIRE);
        haloplaysound(sample, d.vehicleClass == HALO_VEHICLE_TURRET || driverFires(d) ? S_SUBGUN : S_ASSAULT, owner);
        return true;
    }

    // The player's seat gun, fired at the crosshair while the trigger is held; the secondary key
    // fires its second trigger.
    void fireGun(Vehicle &v, int elapsed)
    {
        const HaloWeaponDef *gun = haloweapon(v.gun.def);
        if(!gun) return;
        v.gun.tick(*gun, elapsed);
        if(v.gun2.def == gun->index) v.gun2.tick(*gun, elapsed);
        if(halocinematic()) return;
        const float yaw = player1->yaw * RAD, pitch = player1->pitch * RAD;
        const vec aim(sinf(yaw) * cosf(pitch), -cosf(yaw) * cosf(pitch), sinf(pitch));
        const vec eye = seatPoint(v, playerSeat);
        vec to = worldpos;
        if(vec(to).sub(eye).dot(aim) <= .5f) to = vec(eye).add(vec(aim).mul(200));
        bool fired = player1->attacking && shoot(v, player1, playerSeat, 0, to);
        if(secondaryHeld && gun->trigger(1)) fired |= shoot(v, player1, playerSeat, 1, to);
        if(!fired) return;
        lastFire = lastmillis;
        ++shotsFired;
        player1->lastaction = lastmillis;
    }

    // An actor in an armed seat fires the vehicle's gun at its brain's target: within the gun's
    // range, within the nose's cone for a fixed gun, in bursts for an automatic gun, leading the
    // target and lobbing a mortar on its high arc. A Banshee's pilot adds a fuel rod now and then.
    void aiFire(Vehicle &v, playerent *unit, int seat, int elapsed)
    {
        HaloActorIntent in;
        const HaloWeaponDef *gun = gunOf(v);
        if(!gun || !haloactorintent(unit, in)) return;
        v.gun.tick(*gun, elapsed);
        if(v.gun2.def == gun->index) v.gun2.tick(*gun, elapsed);
        const HaloTriggerDef *t = gun->trigger();
        if(!in.mayFire || !in.target || !t || !t->projectile.valid || halocinematic()) return;
        const VehicleDef &d = *defOf(v);
        const Dims dm = dims(v);
        const float s = dm.s;
        vec target = vec(in.target->o).sub(vec(0, 0, in.target->eyeheight * .4f));
        const float distance = v.pos.dist(target);
        const float range = (t->projectile.range > 0 ? t->projectile.range : 100) * s;
        if(distance > range * .95f) return;
        if(fixedGun(d, t))
        {
            if(fabsf(wrap180(yawTo(v.pos, target) - v.yaw)) > 25) return;
            if(d.vehicleClass == HALO_VEHICLE_FLYER)
            {
                const float pitch = atan2f(target.z - v.pos.z, max(.01f, v.pos.distxy(target))) / RAD;
                if(fabsf(pitch - v.pitch) > 35) return;
            }
        }
        const bool automatic = t->rate + t->finalRate > .02f;
        if(automatic)
        {
            if(lastmillis >= v.aiNextBurst && lastmillis >= v.aiBurstUntil)
            {
                v.aiBurstUntil = lastmillis + 900 + rnd(900);
                v.aiNextBurst = v.aiBurstUntil + 700 + rnd(800);
            }
            if(lastmillis >= v.aiBurstUntil) return;
        }
        // Lead the target by most of the shot's flight time, and miss by the vehicle variant's
        // error (fightingVariant; the seated actor's own variant's without an authored one).
        const float speed = max(1.0f, t->projectile.speed * s);
        target.add(vec(in.target->vel).mul(.7f * distance / speed));
        const HaloActorDef *variant = fightingVariant(d);
        const float error = variant ? variant->error : in.error;
        if(error > 0)
        {
            const float spread = tanf(error) * distance;
            target.add(vec(rndscale(2 * spread) - spread, rndscale(2 * spread) - spread, (rndscale(2 * spread) - spread) * .5f));
        }
        vec to = target;
        if(t->projectile.gravity > 0)
        {
            // The mortar's launch angle for the flat distance (the height difference is ignored).
            // The engine's projectiles keep their speed and only bend under gravity * 3.22 world
            // units/s² (haloupdatebolts), so the heading turns at g cos(angle) / speed and a shot
            // comes back to its height after 2 * angle * speed² / g: the angle is the distance
            // times g over twice the speed squared (at most 80 degrees, about 85 world units for
            // the Wraith's 10 units a second).
            const float g = t->projectile.gravity * 3.22f, flat = v.pos.distxy(target) / max(s, 1e-3f);
            const float angle = clamp(flat * g / max(2 * t->projectile.speed * t->projectile.speed, 1e-3f), .05f, 1.4f);
            vec across = vec(target).sub(v.pos);
            across.z = 0;
            if(across.iszero()) across = forwardOf(v.yaw);
            across.normalize();
            to = vec(v.pos).add(vec(across).mul(cosf(angle) * 100)).add(vec(0, 0, sinf(angle) * 100 + dm.height));
        }
        // A semi-automatic gun (the mortar, the cannon) waits a little longer between rounds.
        if(shoot(v, unit, seat, 0, to) && !automatic) v.nextShot += 400 + rnd(900);
        if(d.vehicleClass == HALO_VEHICLE_FLYER && gun->trigger(1) && lastmillis >= v.aiNextSecondary && distance < 35 * s)
        {
            if(shoot(v, unit, seat, 1, target)) v.aiNextSecondary = lastmillis + 5000 + rnd(4000);
        }
    }

    // Whether the chassis would run into something (or off a drop) within `reach` along `yaw`.
    bool aheadBlocked(int index, const Vehicle &v, const Dims &d, float yaw, float reach)
    {
        vec normal;
        const vec delta = vec(forwardOf(yaw)).mul(reach);
        if(chassisBlocked(index, v, d, v.pos, delta, normal)) return true;
        const vec end = vec(v.pos).add(delta);
        float z;
        bool wall;
        return !groundAt(end.x, end.y, v.pos.z + d.step, v.pos.z - max(d.step * 3, 2 * d.s), z, wall);
    }

    // An actor at the wheel (or the controls of a hover craft or tank): towards its brain's goal
    // when it fights nobody; against a target, a Warthog runs it down, a Ghost closes to its
    // range, faces it and strafes round it, and the tank and the Wraith keep their distance and
    // circle. It steers round what blocks the way ahead and backs off when stuck.
    Drive aiGround(Vehicle &v, int index, const Dims &d, const HaloActorIntent &in)
    {
        const VehicleDef &def = *defOf(v);
        const float s = d.s;
        Drive out;
        out.piloted = true;
        const HaloWeaponDef *gun = gunOf(v);
        const HaloTriggerDef *t = gun ? gun->trigger() : nullptr;
        const bool lobs = t && t->projectile.gravity > 0;
        const bool car = def.vehicleClass == HALO_VEHICLE_CAR;
        vec goal;
        float arrive = 3 * s;
        const playerent *face = nullptr;
        if(in.target)
        {
            const vec at = vec(in.target->o).sub(vec(0, 0, in.target->eyeheight));
            const float distance = v.pos.distxy(at);
            const bool standoff = lobs || def.vehicleClass == HALO_VEHICLE_TANK;
            const float nearest = (standoff ? 12 : car ? 0 : 6) * s, farthest = (standoff ? 28 : car ? 4 : 18) * s;
            if(lastmillis >= v.aiOrbitUntil) { v.aiOrbit = rnd(2) ? 1 : -1; v.aiOrbitUntil = lastmillis + 3000 + rnd(4000); }
            vec away = vec(v.pos).sub(at);
            away.z = 0;
            if(away.iszero()) away = vec(1, 0, 0);
            away.normalize();
            if(distance > farthest) goal = at;
            else if(distance < nearest) goal = vec(at).add(vec(away).mul(nearest + 4 * s));
            else
            {
                vec round = away;
                round.rotate_around_z(35 * RAD * v.aiOrbit);
                goal = vec(at).add(round.mul(max(distance, nearest + s)));
            }
            arrive = s;
            if(def.vehicleClass == HALO_VEHICLE_HOVER && !lobs) face = in.target;
        }
        else if(in.hasGoal) { goal = in.goal; if(in.commanded) arrive = 2 * s; }
        else return out;
        const float distance = v.pos.distxy(goal);
        float heading = yawTo(v.pos, goal);
        out.throttle = distance <= arrive ? 0 : clamp(distance / (10 * s), in.commanded && !in.target ? .2f : .35f, 1.0f);
        if(face && v.pos.distxy(face->o) < 30 * s)
        {
            // In range, a Ghost faces its target (its guns are fixed) and strafes round it.
            heading = yawTo(v.pos, face->o);
            const vec rel = vec(goal).sub(v.pos);
            out.strafe = clamp(rel.dot(rightOf(heading)) / (6 * s), -1.0f, 1.0f);
            out.throttle = clamp(rel.dot(forwardOf(heading)) / (6 * s), -1.0f, 1.0f);
        }
        const float off = wrap180(heading - v.yaw);
        out.steer = clamp(off / 25, -1.0f, 1.0f);
        if(car && fabsf(off) > 100) out.throttle *= .5f;
        if(def.vehicleClass == HALO_VEHICLE_TANK && fabsf(off) > 45) out.throttle *= .3f;   // turn on the spot first
        const float speed = vec(v.vel.x, v.vel.y, 0).magnitude();
        if(out.throttle > 0)
        {
            const float reach = d.halfL + max(d.halfL, speed * .5f);
            if(aheadBlocked(index, v, d, v.yaw, reach))
            {
                const bool left = !aheadBlocked(index, v, d, v.yaw - 45, reach), right = !aheadBlocked(index, v, d, v.yaw + 45, reach);
                if(left || right) { out.steer = left && (!right || off < 0) ? -1 : 1; out.throttle = .5f; }
                else v.aiReverseUntil = max(v.aiReverseUntil, lastmillis + 900);
            }
        }
        if(out.throttle > .3f && speed < 1)
        {
            if(!v.aiStuckSince) v.aiStuckSince = lastmillis;
            else if(lastmillis - v.aiStuckSince > 1500) { v.aiReverseUntil = lastmillis + 1100; v.aiStuckSince = 0; v.aiOrbit = -v.aiOrbit; }
        }
        else v.aiStuckSince = 0;
        if(lastmillis < v.aiReverseUntil) { out.throttle = -.8f; out.steer = car ? -out.steer : out.steer; out.strafe = 0; }
        return out;
    }

    // An actor flying a Banshee. Against a target it makes strafing runs: out wide and up, a
    // dive with the nose on the target (aiFire shoots), then a pull out over and past it, and
    // round again. Otherwise it flies to its brain's goal a little above it and hovers there.
    // It keeps off the floor and climbs over what lies ahead.
    Drive aiFlight(Vehicle &v, int index, const Dims &d, const HaloActorIntent &in)
    {
        const float s = d.s;
        Drive out;
        out.piloted = true;
        out.aimYaw = v.yaw;
        vec goal;
        bool haveGoal = false, attacking = false;
        if(in.target)
        {
            const vec at = vec(in.target->o).sub(vec(0, 0, in.target->eyeheight * .4f));
            vec back = vec(v.pos).sub(at);
            back.z = 0;
            if(back.iszero()) back = vec(1, 0, 0);
            back.normalize();
            if(v.aiPhase == 0)
            {
                goal = vec(at).add(vec(back).mul(30 * s)).add(vec(0, 0, 12 * s));
                if(v.pos.dist(goal) < 8 * s || lastmillis > v.aiPhaseUntil) { v.aiPhase = 1; v.aiPhaseUntil = lastmillis + 7000; }
            }
            else if(v.aiPhase == 1)
            {
                goal = at;
                attacking = true;
                if(v.pos.distxy(at) < 9 * s || lastmillis > v.aiPhaseUntil)
                {
                    v.aiPhase = 2;
                    v.aiGoal = vec(at).add(vec(forwardOf(v.yaw)).mul(35 * s)).add(vec(0, 0, 14 * s));
                    v.aiPhaseUntil = lastmillis + 3500;
                }
            }
            else
            {
                goal = v.aiGoal;
                if(v.pos.dist(goal) < 8 * s || lastmillis > v.aiPhaseUntil) { v.aiPhase = 0; v.aiPhaseUntil = lastmillis + 10000; }
            }
            haveGoal = true;
        }
        else
        {
            v.aiPhase = 0;
            v.aiPhaseUntil = lastmillis + 10000;
            // A command list's points are where the scenario wants the Banshee (a30's lz_banshee
            // lists): it flies to them; any other goal (a position on foot) it hovers above.
            if(in.hasGoal) { goal = in.commanded ? in.goal : vec(in.goal).add(vec(0, 0, 6 * s)); haveGoal = true; }
        }
        if(haveGoal)
        {
            const vec rel = vec(goal).sub(v.pos);
            const float flat = rel.magnitudexy(), distance = rel.magnitude();
            const float arrive = (in.commanded && !in.target ? 1.5f : 4.0f) * s;
            if(flat > s) out.aimYaw = yawTo(v.pos, goal);
            out.aimPitch = clamp(atan2f(rel.z, max(flat, .01f)) / RAD, -45.0f, 45.0f);
            out.throttle = attacking ? 1 : distance < arrive ? 0 : clamp(distance / (15 * s), .2f, 1.0f);
            if(fabsf(wrap180(out.aimYaw - v.yaw)) > 60) out.throttle *= .4f;    // turn before speeding off
            if(!out.throttle) { out.climb = clamp(rel.z / (3 * s), -1.0f, 1.0f); out.aimPitch = 0; }
        }
        // Off the floor, and over what lies ahead.
        float floor;
        bool wall;
        if(groundAt(v.pos.x, v.pos.y, v.pos.z + d.step, v.pos.z - 3 * s, floor, wall) || wall)
        {
            out.aimPitch = max(out.aimPitch, 20.0f);
            out.climb = 1;
        }
        vec normal;
        const float speed = v.vel.magnitude();
        if(out.throttle > 0 && flightBlocked(index, v, d, v.pos, vec(noseOf(v)).mul(max(2 * d.halfL, speed * .6f)), normal))
        {
            out.climb = 1;
            out.aimPitch = max(out.aimPitch, 30.0f);
        }
        return out;
    }

    // What an actor at the controls does this frame (braindead: it holds where it is).
    Drive aiDrive(Vehicle &v, int index, playerent *pilot)
    {
        HaloActorIntent in;
        Drive hold;
        hold.piloted = true;
        hold.aimYaw = v.yaw;
        if(!haloactorintent(pilot, in) || !in.active) return hold;
        const Dims d = dims(v);
        Drive out = defOf(v)->vehicleClass == HALO_VEHICLE_FLYER ? aiFlight(v, index, d, in) : aiGround(v, index, d, in);
        // Stunned by EMP: the engine is out for an actor at the controls too (a Banshee sinks, fly).
        if(lastmillis < v.stunnedUntil) out.throttle = out.strafe = 0;
        return out;
    }

    // Every armed seat an actor holds fires (the player's seat is fireGun's).
    void aiFireSeats(Vehicle &v, int elapsed)
    {
        const VehicleDef &d = *defOf(v);
        loopi(min(maxSeats, int(d.seats.size())))
        {
            playerent *p = aliveBot(v.riders[i]);
            if(p && armedSeat(d, i)) aiFire(v, p, i, elapsed);
        }
    }

    // ai_go_to_vehicle: each walking actor is seated once it reaches its seat's entry point (or
    // comes close and has walked a while: a body cannot stand on a Ghost's seat marker), and
    // gives up after 30 s or when the seat is taken or the vehicle gone.
    void updateBoardings()
    {
        for(size_t i = 0; i < boardings.size();)
        {
            Boarding &b = boardings[i];
            playerent *p = aliveBot(b.unit);
            Vehicle *v = vehicles.inrange(b.vehicle) ? &vehicles[b.vehicle] : nullptr;
            const VehicleDef *d = v ? defOf(*v) : nullptr;
            bool done = !p || !v || !v->exists || !d || b.seat >= int(d->seats.size()) || (v->riders[b.seat] && v->riders[b.seat] != p) ||
                        lastmillis > b.until || halovehicleinside(p);
            if(!done)
            {
                const Dims dm = dims(*v);
                const vec entry = modelPoint(*v, d->seats[b.seat].enter, dm.s);
                const vec feet = vec(p->o).sub(vec(0, 0, p->eyeheight));
                const float reach = max(dm.halfL, dm.halfW) + p->radius + 2;
                const float distance = feet.distxy(entry);
                if(distance < reach * .5f || (lastmillis - b.since > 12000 && distance < reach * 2))
                {
                    v->riders[b.seat] = p;
                    v->sleeping = false;
                    if(halo_vehicletrace) conoutf("Halo vehicle: %s boards the %s (%s)", p->name, d->displayName.c_str(), d->seats[b.seat].name.c_str());
                    done = true;
                }
                else haloactorgoto(p, &entry);
            }
            if(done)
            {
                if(p) haloactorgoto(p, nullptr);
                boardings.erase(boardings.begin() + i);
            }
            else ++i;
        }
    }

    void updateRiders(Vehicle &v)
    {
        loopi(maxSeats)
        {
            playerent *p = v.riders[i];
            if(!p || p == player1) continue;
            if(!aliveBot(p)) { v.riders[i] = nullptr; continue; }
            // Actors ride where their seat is; the vehicle fires for one in an armed seat (aiFire).
            p->o = seatPoint(v, i).add(vec(0, 0, .45f * p->eyeheight));
            p->vel = vec(0, 0, 0);
            p->onfloor = true;
            p->resetinterp();
            // A driver with nobody to fight looks where it is going (its vision cone follows).
            const VehicleDef *d = defOf(v);
            if(d && i < int(d->seats.size()) && d->seats[i].role == HALO_SEAT_DRIVER && drivenClass(d->vehicleClass) && !haloactortarget(p))
            {
                p->yaw = v.yaw;
                if(p->type == ENT_BOT) static_cast<botent *>(p)->targetyaw = v.yaw;
            }
        }
    }

    // Covenant gunners take free turrets the script made enterable (ai_vehicle_enterable_*).
    void aiBoard(Vehicle &v)
    {
        const VehicleDef &d = *defOf(v);
        if(v.aiDisabled || v.aiDistance <= 0 || d.vehicleClass != HALO_VEHICLE_TURRET) return;
        const int seat = seatByRole(d, HALO_SEAT_GUNNER);
        if(seat < 0 || v.riders[seat]) return;
        const float range = v.aiDistance * vehicleScale();
        playerent *best = nullptr;
        float distance = range;
        loopv(players)
        {
            playerent *p = players[i];
            if(!p || p->type != ENT_BOT || p->state != CS_ALIVE || halovehicleinside(p)) continue;
            int team = -1;
            const int kind = haloenemykind(p, &team);
            if(kind < 0 || team != HALO_TEAM_COVENANT || (v.aiKind >= 0 && kind != v.aiKind)) continue;
            const float dd = p->o.dist(seatPoint(v, seat));
            if(dd < distance) { distance = dd; best = p; }
        }
        if(best)
        {
            v.riders[seat] = best;
            if(halo_vehicletrace) conoutf("Halo vehicle: %s mans a turret", best->name);
        }
    }

    float playerSpeed(const Vehicle &v) { return v.vel.magnitude() / max(.01f, vehicleScale()); }

    // Vehicle damage (halovehiclehit). Someone alive rides it: a vehicle "only damaged while
    // occupied" (the Ghost, the Banshee) takes damage only then.
    bool occupied(const Vehicle &v)
    {
        for(const playerent *p : v.riders) if(p && p->state == CS_ALIVE) return true;
        return false;
    }

    // Vehicles with a body vitality: they stop shots and can be destroyed.
    bool destructible(const Vehicle &v)
    {
        const VehicleDef *d = defOf(v);
        return v.exists && d && d->vitality > 0;
    }

    bool rides(const Vehicle &v, const playerent *p)
    {
        if(!p) return false;
        for(const playerent *r : v.riders) if(r == p) return true;
        return false;
    }

    // Someone alive rides it whom `shooter` (an actor) is hostile to. An actor's shots pass through
    // friendly bodies (haloactorhit); they pass through a vehicle of its own side, or an empty one,
    // the same way (as before vehicle damage): Grunts firing at the player past their own Wraith
    // do not wreck it.
    bool hostileRider(const Vehicle &v, const playerent *shooter)
    {
        for(const playerent *p : v.riders) if(p && p->state == CS_ALIVE && haloactorhostile(shooter, p)) return true;
        return false;
    }

    // The hull covers rider `p`: the collision model takes its children's body damage (the
    // Banshee), or the rider's seat is an invisible one, inside the hull (the Wraith's driver).
    bool coversRider(const Vehicle &v, const VehicleDef &d, const playerent *p)
    {
        if(d.collFlags & collTakesRiderDamage) return true;
        loopi(min(maxSeats, int(d.seats.size()))) if(v.riders[i] == p) return (d.seats[i].flags & 1) != 0;
        return false;
    }

    // The middle of the body box.
    vec boxCentre(const Vehicle &v, const Dims &dm)
    {
        return vec(v.pos).add(vec(forwardOf(v.yaw)).mul(dm.centre)).add(vec(0, 0, (vehicleTop(v, dm) - v.pos.z) * .5f));
    }

    // Where the segment enters the body box (yaw only), as a fraction along it, or -1.
    float boxEntry(const Vehicle &v, const Dims &dm, const vec &from, const vec &to)
    {
        const vec f = forwardOf(v.yaw), r = rightOf(v.yaw);
        const vec centre = vec(v.pos).add(vec(f).mul(dm.centre));
        const vec o = vec(from).sub(centre), dir = vec(to).sub(from);
        const float bottom = v.pos.z - centre.z, top = vehicleTop(v, dm) - centre.z;
        const float origin[3] = {o.dot(f), o.dot(r), o.z}, delta[3] = {dir.dot(f), dir.dot(r), dir.z};
        const float low[3] = {-dm.halfL, -dm.halfW, bottom}, high[3] = {dm.halfL, dm.halfW, top};
        float enter = 0, leave = 1;
        loopi(3)
        {
            if(fabsf(delta[i]) < 1e-6f)
            {
                if(origin[i] < low[i] || origin[i] > high[i]) return -1;
                continue;
            }
            float t0 = (low[i] - origin[i]) / delta[i], t1 = (high[i] - origin[i]) / delta[i];
            if(t0 > t1) std::swap(t0, t1);
            enter = max(enter, t0);
            leave = min(leave, t1);
            if(enter > leave) return -1;
        }
        return enter;
    }

    // Blown up: the body depleted effect where it stood, every rider killed (the player too,
    // unless protected), and the vehicle and the scripts' object gone.
    void destroyVehicle(Vehicle &v, int index, playerent *attacker)
    {
        const VehicleDef &d = *defOf(v);
        const Dims dm = dims(v);
        const vec centre = boxCentre(v, dm);
        v.health = 0;
        v.destroyed = true;
        conoutf("Halo vehicle: the %s is destroyed", d.displayName.c_str());
        halofxeffect(d.destroyedEffect.empty() ? "explosion" : d.destroyedEffect.c_str(), centre, !halocinematic());
        playerent *riders[maxSeats];
        loopi(maxSeats) riders[i] = v.riders[i];
        ejectRiders(v, index);
        v.exists = false;
        v.sleeping = false;
        // Nobody walks to a seat of it any more.
        for(Boarding &b : boardings) if(b.vehicle == index) b.until = 0;
        playerent *killer = attacker && (attacker == player1 || aliveBot(attacker)) ? attacker : player1;
        loopi(maxSeats)
        {
            playerent *p = riders[i];
            if(!p || p->state != CS_ALIVE || (p != player1 && !aliveBot(p))) continue;
            // A large finite amount: halodamage still honours object_cannot_take_damage and the
            // QA walker's protection.
            dodamage(100000, p, killer, GUN_GRENADE, true);
        }
        if(v.object >= 0) haloscenarioobjectdestroyed(v.object);
    }
}

void halovehiclesclear()
{
    loadDefs();
    for(const EngineLoop &e : engineLoops) stopEngineLoop(e);
    engineLoops.clear();
    vehicles.shrink(0);
    saved.clear();
    savedValid = restoredFromCheckpoint = false;
    sectionScale = 0;
    playerVehicle = playerSeat = -1;
    qaUntil = 0;
    trapped.clear();
    struck.clear();
    boardings.clear();
    thrownVehicle = thrownSeat = -1;
}

void halovehicleplace(const entity &e)
{
    // attr3: type, attr4: not-placed flags, attr5: object index + 1.
    if(e.attr7) sectionScale = e.attr7 / 20.0f;
    const int index = add(e.attr3, vec(e.x, e.y, e.z), e.attr1 / 10.0f, e.attr5 - 1, e.attr4);
    if(index >= 0) vehicles[index].placed = true;
}

void halovehiclesrestart(bool checkpoint)
{
    loadDefs();
    playerVehicle = playerSeat = -1;
    qaUntil = 0;
    trapped.clear();
    // The actors walking to seats were respawned (or are gone) with the revert.
    boardings.clear();
    secondaryHeld = jumpHeld = crouchHeld = false;
    thrownVehicle = thrownSeat = -1;
    restoredFromCheckpoint = checkpoint && savedValid;
    if(restoredFromCheckpoint)
    {
        vehicles.shrink(0);
        for(const Vehicle &v : saved) vehicles.add(v);
    }
    else
    {
        // Vehicles spawned at run time (the Maw's) go; placements return to where they stood.
        loopvrev(vehicles) if(!vehicles[i].placed) vehicles.remove(i);
        loopv(vehicles)
        {
            Vehicle &v = vehicles[i];
            for(playerent *&p : v.riders) p = nullptr;
            // What the last attempt's script set on them goes too: the script runs again. So
            // does the escape taking one over (halovehicleescape takes it again).
            v.escape = false;
            v.enterable = true;
            v.aiDisabled = false;
            v.aiDistance = 0;
            v.aiKind = -1;
            respawn(v, !(v.flags & 1) && difficultyAllows(v.flags));
        }
    }
    // The fall, wedge and stall timers start afresh; the safe spots stay (a checkpoint's are good).
    loopv(vehicles)
    {
        Vehicle &v = vehicles[i];
        v.airSince = v.embeddedSince = v.stalledSince = v.lastRecovery = v.stunnedUntil = 0;
        v.stalledAt = v.pos;
        v.aiPhase = v.aiPhaseUntil = v.aiOrbitUntil = v.aiStuckSince = v.aiReverseUntil = 0;
        v.aiBurstUntil = v.aiNextBurst = v.aiNextSecondary = v.nextShot = v.nextShot2 = 0;
        v.gun2 = HaloWeaponState();
    }
    int seated = -1, seat = -1;
    loopv(vehicles) loopj(maxSeats)
    {
        playerent *&p = vehicles[i].riders[j];
        if(p == player1 && seated < 0) { seated = i; seat = j; }
        p = nullptr;
    }
    if(seated >= 0) seatPlayer(seated, seat);
    if(halo_vehicletrace) conoutf("Halo vehicles: %d in the section%s", vehicles.length(), restoredFromCheckpoint ? " (checkpoint)" : "");
}

void halovehiclescheckpoint()
{
    saved.clear();
    loopv(vehicles)
    {
        saved.push_back(vehicles[i]);
        // Actors are respawned by a revert; only the player keeps a seat.
        for(playerent *&p : saved.back().riders) if(p != player1) p = nullptr;
    }
    savedValid = true;
}

// Saved games (halo.cpp): the in-memory checkpoint's vehicles as "v" records. A vehicle is
// named by its type (vehicles.txt name) and scenario object (index and name); the riders are
// not kept, except the player's seat. Times in milliseconds are not kept either: they start
// afresh, as the fall, wedge and stall timers do on any revert.
void halovehiclesstatesave(std::string &out)
{
    if(!savedValid) return;
    for(const Vehicle &v : saved)
    {
        const VehicleDef *d = defOf(v);
        int seat = -1;
        loopi(maxSeats) if(v.riders[i] == player1) seat = i;
        halostatef(out, "v vehicle");
        halostateword(out, d ? d->name : std::string());
        halostatef(out, " %d", v.object);
        halostateword(out, haloscenarioobjectname(v.object));
        halostatef(out, " %d %.9g %.9g %.9g %.9g %d %d %d", v.flags, v.spawnFeet.x, v.spawnFeet.y, v.spawnFeet.z, v.spawnYaw,
                   v.exists, v.placed, v.escape);
        halostatef(out, " %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g", v.pos.x, v.pos.y, v.pos.z,
                   v.vel.x, v.vel.y, v.vel.z, v.yaw, v.pitch, v.roll, v.pitchVel, v.rollVel, v.steer, v.climb, v.smoothZ);
        halostatef(out, " %d %d %d %d %d %d %.9g %d %d %.9g %.9g", v.grounded, v.flipped, v.sleeping, v.carried, v.enterable,
                   v.aiDisabled, v.aiDistance, v.aiKind, seat, v.aimYaw, v.aimPitch);
        const HaloWeaponDef *gun = haloweapon(v.gun.def);
        halostateword(out, gun ? gun->path : std::string());
        halostatef(out, " %d %d %.9g %.9g %.9g %.9g %d", v.gun.loaded, v.gun.reserve, v.gun.battery, v.gun.heat, v.gun.error,
                   v.gun.rate, v.gun.overheated);
        halostatef(out, " %.9g %.9g %.9g %.9g %d", v.stalledAt.x, v.stalledAt.y, v.stalledAt.z, v.stillZ, v.safeCount);
        loopi(clamp(v.safeCount, 0, safeSpotCount))
            halostatef(out, " %.9g %.9g %.9g %.9g", v.safePos[i].x, v.safePos[i].y, v.safePos[i].z, v.safeYaw[i]);
        // Body health and whether it was destroyed (after the safe spots: a record without them,
        // from before vehicle damage, still loads, whole).
        halostatef(out, " %.9g %d", v.health, v.destroyed);
        out += '\n';
    }
}

bool halovehiclesstaterestore(const std::vector<std::string> &records, std::string &why)
{
    loadDefs();
    std::vector<Vehicle> list;
    for(size_t line = 0; line < records.size(); ++line)
    {
        HaloStateReader r(records[line].c_str());
        Vehicle v;
        if(!r.key("vehicle")) { why = "unknown vehicle record"; return false; }
        const std::string type = r.word();
        v.object = r.i();
        const std::string object = r.word();
        v.flags = r.i();
        v.spawnFeet.x = r.f(); v.spawnFeet.y = r.f(); v.spawnFeet.z = r.f(); v.spawnYaw = r.f();
        v.exists = r.b(); v.placed = r.b(); v.escape = r.b();
        v.pos.x = r.f(); v.pos.y = r.f(); v.pos.z = r.f();
        v.vel.x = r.f(); v.vel.y = r.f(); v.vel.z = r.f();
        v.yaw = r.f(); v.pitch = r.f(); v.roll = r.f(); v.pitchVel = r.f(); v.rollVel = r.f(); v.steer = r.f(); v.climb = r.f();
        v.smoothZ = r.f();
        v.grounded = r.b(); v.flipped = r.b(); v.sleeping = r.b(); v.carried = r.b(); v.enterable = r.b(); v.aiDisabled = r.b();
        v.aiDistance = r.f(); v.aiKind = r.i();
        const int seat = r.i();
        v.aimYaw = r.f(); v.aimPitch = r.f();
        const std::string gun = r.word();
        v.gun.loaded = r.i(); v.gun.reserve = r.i(); v.gun.battery = r.f(); v.gun.heat = r.f(); v.gun.error = r.f(); v.gun.rate = r.f();
        v.gun.overheated = r.b();
        v.stalledAt.x = r.f(); v.stalledAt.y = r.f(); v.stalledAt.z = r.f(); v.stillZ = r.f();
        v.safeCount = r.i();
        if(!r.ok || v.safeCount < 0 || v.safeCount > safeSpotCount) { why = "a vehicle record is damaged"; return false; }
        loopi(v.safeCount) { v.safePos[i].x = r.f(); v.safePos[i].y = r.f(); v.safePos[i].z = r.f(); v.safeYaw[i] = r.f(); }
        if(r.ok && r.more()) { v.health = r.f(); v.destroyed = r.b(); }
        if(!r.ok || r.more() || !std::isfinite(v.health)) { why = "a vehicle record is damaged"; return false; }
        v.health = clamp(v.health, 0.0f, 1.0f);
        // A destroyed vehicle is gone; it can only be created again by the script.
        if(v.destroyed) v.exists = false;
        v.type = -1;
        for(const VehicleDef &d : defs) if(d.name == type) v.type = d.index;
        const VehicleDef *d = defOf(v);
        if(!d) { why = "a saved vehicle's type is not in vehicles.txt"; return false; }
        if(object != haloscenarioobjectname(v.object)) { why = "a saved vehicle names another scenario object"; return false; }
        if(seat >= int(d->seats.size())) { why = "a saved vehicle seat does not exist"; return false; }
        if(seat >= 0) v.riders[seat] = player1;
        const HaloWeaponDef *weapon = gun.empty() ? nullptr : haloweaponfind(gun.c_str());
        v.gun.def = weapon ? weapon->index : -1;
        if(v.flipped) v.flippedAt = lastmillis;
        list.push_back(v);
    }
    // The section's own placements, in order, as the map has them.
    std::vector<const Vehicle *> placedNow, placedSaved;
    loopv(vehicles) if(vehicles[i].placed) placedNow.push_back(&vehicles[i]);
    for(const Vehicle &v : list) if(v.placed) placedSaved.push_back(&v);
    if(placedNow.size() != placedSaved.size()) { why = "the section's vehicle placements changed"; return false; }
    for(size_t i = 0; i < placedNow.size(); ++i)
        if(placedNow[i]->type != placedSaved[i]->type || placedNow[i]->object != placedSaved[i]->object)
        {
            why = "the section's vehicle placements changed";
            return false;
        }
    saved = std::move(list);
    savedValid = true;
    return true;
}

void halovehiclessync()
{
    if(restoredFromCheckpoint) return;
    loopv(vehicles)
    {
        Vehicle &v = vehicles[i];
        if((v.flags & 1) && v.object >= 0 && !v.exists && haloscenarioobjectvisible(v.object) && difficultyAllows(v.flags))
            respawn(v, true);
    }
}

bool halovehicleseated() { return playerVehicle >= 0 && vehicles.inrange(playerVehicle) && player1->state == CS_ALIVE; }
int halovehicleplayervehicle() { return halovehicleseated() ? playerVehicle : -1; }

bool halovehicleuse(float weaponDistance)
{
    if(!haloactive() || halocinematic() || player1->state != CS_ALIVE) return false;
    if(halovehicleseated())
    {
        Vehicle &v = vehicles[playerVehicle];
        const VehicleDef &d = *defOf(v);
        // A flying Banshee's pilot stays in until it is near the floor: land first.
        if(d.vehicleClass == HALO_VEHICLE_FLYER && !v.flipped && driverOf(v) == player1)
        {
            float floor;
            bool wall;
            if(!groundAt(v.pos.x, v.pos.y, v.pos.z + .1f, v.pos.z - 1.5f * vehicleScale(), floor, wall))
            {
                conoutf("Halo vehicle: the %s is in the air, land to get out", d.displayName.c_str());
                return true;
            }
        }
        unseatPlayer(v.flipped);
        return true;
    }
    int seat;
    float distance;
    const int index = nearestSeat(seat, distance, true);
    if(index < 0 || distance > weaponDistance) return false;
    if(vehicles[index].flipped) { flipBack(vehicles[index], index); return true; }
    return seatPlayer(index, seat);
}

const char *halovehicleprompt(float weaponDistance)
{
    if(!haloactive() || halovehicleseated() || player1->state != CS_ALIVE) return nullptr;
    int seat;
    float distance;
    const int index = nearestSeat(seat, distance, true);
    if(index < 0 || distance > weaponDistance) return nullptr;
    static string prompt;
    const VehicleDef &d = *defOf(vehicles[index]);
    if(vehicles[index].flipped) formatstring(prompt)("E  Flip the %s", d.displayName.c_str());
    else if(d.seats[seat].role == HALO_SEAT_GUNNER)
        formatstring(prompt)(d.vehicleClass == HALO_VEHICLE_TURRET ? "E  Man the %s" : "E  %s gunner", d.displayName.c_str());
    else formatstring(prompt)(d.vehicleClass == HALO_VEHICLE_FLYER ? "E  Fly the %s" : "E  Drive the %s", d.displayName.c_str());
    return prompt;
}

void halovehicledrive(int curtime)
{
    if(!halovehicleseated() || curtime <= 0) return;
    followPlayer();
    Vehicle &v = vehicles[playerVehicle];
    const VehicleDef &d = *defOf(v);
    const int role = d.seats[playerSeat].role;
    Drive in;
    const bool qa = lastmillis < qaUntil;
    if(role == HALO_SEAT_DRIVER && !v.flipped)
    {
        in.piloted = true;
        in.throttle = clamp(float(player1->move), -1.0f, 1.0f);
        const float keys = -clamp(float(player1->strafe), -1.0f, 1.0f);
        // Turn towards where the camera looks, as the retail vehicles do.
        const float camera = clamp(wrap180(player1->yaw - v.yaw) / 30.0f, -1.0f, 1.0f);
        const float forwardSpeed = v.vel.dot(forwardOf(v.yaw));
        if(d.vehicleClass == HALO_VEHICLE_FLYER)
        {
            // The nose follows the camera; A and D strafe, jump climbs and crouch descends.
            in.strafe = keys;
            in.climb = float(jumpHeld) - float(crouchHeld);
            in.aimYaw = player1->yaw;
            in.aimPitch = player1->pitch;
            if(qa && qaFlight) { in.throttle = qaThrottle; in.strafe = qaStrafe; in.climb = qaClimb; }
        }
        else if(d.vehicleClass == HALO_VEHICLE_HOVER)
        {
            // A hover craft turns to the camera even standing still; A and D strafe.
            in.strafe = keys;
            in.steer = halo_vehiclesteer ? 0 : clamp(wrap180(player1->yaw - v.yaw) / 20.0f, -1.0f, 1.0f);
            if(halo_vehiclesteer) { in.steer = keys; in.strafe = 0; }
            if(qa) { in.throttle = qaThrottle; if(fabsf(qaSteer) <= 1) in.steer = qaSteer; }
        }
        else
        {
            // The Warthog and the tank: A and D steer; with neither, towards the camera when
            // moving, reversing included (the tank turns on the spot with the keys alone).
            in.steer = keys;
            if(!keys && !halo_vehiclesteer && (in.throttle || fabsf(forwardSpeed) > 2))
                in.steer = camera * (forwardSpeed < -1 ? -1 : 1);
            // QA input; a steer outside -1..1 keeps the camera steering.
            if(qa) { in.throttle = qaThrottle; if(fabsf(qaSteer) <= 1) in.steer = qaSteer; }
        }
        // Stunned by EMP: the engine is out, the car coasts (a Banshee sinks, fly).
        // A hover craft's strafe is its engine too.
        if(lastmillis < v.stunnedUntil) in.throttle = in.strafe = 0;
    }
    else if(playerent *pilot = aliveBot(driverOf(v)))
    {
        // A Warthog's gunner or passenger with an actor at the wheel: it drives.
        if(halo_vehicleai) in = aiDrive(v, playerVehicle, pilot);
        else in.piloted = true;
    }
    player1->strafe = 0;
    player1->jumpnext = false;
    player1->trycrouch = false;
    if(role != HALO_SEAT_DRIVER) { v.aimYaw = player1->yaw; v.aimPitch = player1->pitch; }
    if(drivenClass(d.vehicleClass) || !v.grounded)
    {
        // Fixed 8 ms steps keep wall contacts and landings stable at any frame rate.
        for(int left = min(curtime, 100); left > 0; left -= 8)
            simulate(v, playerVehicle, min(left, 8) / 1000.0f, in);
    }
    if(d.vehicleClass == HALO_VEHICLE_TURRET) v.yaw = v.aimYaw;
    // Falls, wedges and stalls; actors in the way were shoved or run down in simulate.
    if(!halocinematic()) updateStuck(v, playerVehicle);
    if(!halovehicleseated()) return;
    placePlayerOnSeat();
    player1->yaw = fmodf(player1->yaw + 360.0f, 360.0f);
}

void halovehiclesupdate(int elapsed)
{
    if(!haloactive() || elapsed <= 0) return;
    if(playerVehicle >= 0 && player1->state != CS_ALIVE)
    {
        // Killed in the seat: the vehicle stays where it is.
        if(vehicles.inrange(playerVehicle)) vehicles[playerVehicle].riders[playerSeat] = nullptr;
        playerVehicle = playerSeat = -1;
    }
    static int lastBoard = 0;
    const bool boardTick = lastmillis - lastBoard >= 500;
    if(boardTick) lastBoard = lastmillis;
    loopv(vehicles)
    {
        Vehicle &v = vehicles[i];
        const VehicleDef *d = defOf(v);
        if(!v.exists || !d) continue;
        // Carried by a scripted object (a Pelican's cargo hook) or moved by a recording.
        vec carriedAt;
        float carriedYaw, carriedPitch = 0;
        if(v.object >= 0 && haloscenarioobjectcarried(v.object, carriedAt, carriedYaw, &carriedPitch))
        {
            // A recorded flight (a Banshee's flyby) climbs and dives with the recording; when it
            // ends, the vehicle carries on from there (an actor at the controls takes over).
            v.carried = true;
            v.pos = carriedAt;
            v.smoothZ = v.pos.z;
            v.yaw = carriedYaw;
            v.pitch = d->vehicleClass == HALO_VEHICLE_FLYER ? clamp(carriedPitch, -60.0f, 60.0f) : 0;
            v.roll = 0;
            v.vel = vec(0, 0, 0);
            v.grounded = v.sleeping = false;
            updateRiders(v);
            continue;
        }
        v.carried = false;
        if(i == playerVehicle)
        {
            followPlayer();
            if(armedSeat(*d, playerSeat)) fireGun(v, elapsed);
            if(v.flipped && lastmillis - v.flippedAt > flipEject)
            {
                conoutf("Halo vehicle: the %s is over, player thrown out", d->displayName.c_str());
                // No room for the body beside it: the vehicle goes back on its wheels with the
                // driver still in it (where it lies if it fits, else nearby or its last safe spot);
                // failing that, out onto it as before.
                if(!unseatPlayer(false) && !recoverVehicle(v, i, "over with no room to get out", false)) unseatPlayer(true);
            }
        }
        else if(playerent *pilot = halo_vehicleai && !halocinematic() && drivenClass(d->vehicleClass) && !v.flipped ? aliveBot(driverOf(v)) : nullptr)
        {
            // An actor at the controls drives or flies it (aiGround, aiFlight).
            v.sleeping = false;
            v.stillSince = 0;
            const Drive in = aiDrive(v, i, pilot);
            for(int left = min(elapsed, 100); left > 0; left -= 8) simulate(v, i, min(left, 8) / 1000.0f, in);
            updateStuck(v, i);
            if(!v.exists) continue;
            v.stillZ = v.pos.z;
        }
        else if(!v.sleeping && !halocinematic())
        {
            for(int left = min(elapsed, 100); left > 0; left -= 8) simulate(v, i, min(left, 8) / 1000.0f, Drive());
            updateStuck(v, i);
            if(!v.exists) continue;
            // Asleep once still. Not on a lift deck or a light bridge (it moves or vanishes): a
            // vehicle on one keeps following its floor, so lifts carry vehicles as they do bodies.
            const Dims dm = dims(v);
            if(v.grounded && v.vel.magnitude() < .05f && fabsf(v.pos.z - v.stillZ) < .02f && onStructure(v, dm))
            {
                if(!v.stillSince) v.stillSince = lastmillis;
                else if(lastmillis - v.stillSince > 600) v.sleeping = true;
            }
            else { v.stillSince = 0; v.stillZ = v.pos.z; }
        }
        if(d->vehicleClass == HALO_VEHICLE_TURRET && v.riders[0] && v.riders[0] != player1) v.yaw = v.riders[0]->yaw;
        updateRiders(v);
        if(!halocinematic()) aiFireSeats(v, elapsed);
        if(boardTick) aiBoard(v);
    }
    if(boardTick) updateBoardings();
    depenetrate();
    updateEngineLoops();
}

bool halovehiclecamera(physent &camera)
{
    if(!halovehicleseated() || editmode) return false;
    const Vehicle &v = vehicles[playerVehicle];
    const VehicleDef &d = *defOf(v);
    const Dims dm = dims(v);
    const int role = d.seats[playerSeat].role;
    // A Banshee's pilot looks further up and down (the nose follows the camera).
    const bool flyer = d.vehicleClass == HALO_VEHICLE_FLYER && role == HALO_SEAT_DRIVER;
    const float lowest = flyer ? -70.0f : -40.0f, highest = flyer ? 70.0f : 35.0f;
    const float yaw = player1->yaw * RAD, pitch = clamp(player1->pitch, lowest, highest) * RAD;
    const vec view(sinf(yaw) * cosf(pitch), -cosf(yaw) * cosf(pitch), sinf(pitch));
    vec pivot;
    float distance;
    if(d.vehicleClass == HALO_VEHICLE_TURRET)
    {
        pivot = modelPoint(v, d.camera, dm.s).add(vec(0, 0, dm.height * .35f + 1));
        distance = dm.length * 1.4f + 3;
    }
    else if(role == HALO_SEAT_GUNNER)
    {
        pivot = seatPoint(v, playerSeat).add(vec(0, 0, dm.height * .6f + 1));
        distance = dm.length * .9f + 3;
    }
    else
    {
        pivot = vec(v.pos.x, v.pos.y, v.smoothZ).add(vec(forwardOf(v.yaw)).mul(dm.centre)).add(vec(0, 0, dm.height * .9f + 1));
        distance = dm.length * 2.2f + 4;
    }
    vec target = vec(pivot).sub(vec(view).mul(distance));
    intersectgeometry(pivot, target);
    // Keep a little off the wall the line stopped at.
    vec back = vec(target).sub(pivot);
    const float length = back.magnitude();
    if(length > 1) target = vec(pivot).add(back.mul((length - .5f) / length));
    camera.o = target;
    camera.yaw = player1->yaw;
    camera.pitch = clamp(player1->pitch, lowest, highest) - (role == HALO_SEAT_DRIVER ? 6 : 0);
    camera.roll = 0;
    return true;
}

float halovehicledamage(float damage)
{
    if(!halovehicleseated() || damage <= 0) return damage;
    const VehicleDef &d = *defOf(vehicles[playerVehicle]);
    // The Warthog's body and the Banshee's canopy take some of the fire and the tank's hull more;
    // a turret's gunner and a Ghost's rider sit in the open.
    switch(d.vehicleClass)
    {
        case HALO_VEHICLE_CAR: case HALO_VEHICLE_FLYER: return damage * .8f;
        case HALO_VEHICLE_TANK: return damage * .5f;
        default: return damage;
    }
}

int halovehicleemp(const vec &centre, float radius, int stuck, int millis)
{
    if(!haloactive() || millis <= 0) return 0;
    int count = 0;
    loopv(vehicles)
    {
        Vehicle &v = vehicles[i];
        if(!v.exists || !defOf(v)) continue;
        bool reached = i == stuck;
        if(!reached && radius > 0)
        {
            // The nearest point of the body's box (yaw only) to the centre.
            const Dims dm = dims(v);
            const vec f = forwardOf(v.yaw), r = rightOf(v.yaw);
            const vec rel = vec(centre).sub(vec(v.pos).add(vec(f).mul(dm.centre)));
            const float along = max(0.0f, fabsf(rel.dot(f)) - dm.halfL), side = max(0.0f, fabsf(rel.dot(r)) - dm.halfW);
            const float top = v.pos.z + (v.flipped ? dm.height * .6f : dm.height);
            const float up = centre.z > top ? centre.z - top : centre.z < v.pos.z ? v.pos.z - centre.z : 0;
            reached = along * along + side * side + up * up <= radius * radius;
        }
        if(!reached) continue;
        v.stunnedUntil = max(v.stunnedUntil, lastmillis + millis);
        v.sleeping = false;
        ++count;
        if(halo_vehicletrace) conoutf("Halo vehicle %d stunned by EMP for %d ms", i, millis);
    }
    return count;
}

int halovehicletrace(const vec &from, const vec &to, float &fraction, const playerent *shooter, const playerent *met)
{
    if(!haloactive() || !halo_vehicledamage) return -1;
    int best = -1;
    float nearest = 2;
    loopv(vehicles)
    {
        const Vehicle &v = vehicles[i];
        if(!destructible(v) || rides(v, shooter)) continue;
        if(shooter && shooter != player1 && !hostileRider(v, shooter)) continue;
        const Dims dm = dims(v);
        const float t = boxEntry(v, dm, from, to);
        if(t < 0 || t >= nearest) continue;
        // A rider in an open seat is what the shot reached.
        if(met && rides(v, met) && !coversRider(v, *defOf(v), met)) continue;
        nearest = t;
        best = i;
    }
    if(best >= 0) fraction = nearest;
    return best;
}

int halovehiclesnear(const vec &centre, float radius, int *out, float *distances, int capacity)
{
    if(!haloactive() || !halo_vehicledamage || radius <= 0) return 0;
    int count = 0;
    loopv(vehicles)
    {
        const Vehicle &v = vehicles[i];
        if(count >= capacity || !destructible(v)) continue;
        // The nearest point of the body's box (yaw only), as halovehicleemp.
        const Dims dm = dims(v);
        const vec f = forwardOf(v.yaw), r = rightOf(v.yaw);
        const vec rel = vec(centre).sub(vec(v.pos).add(vec(f).mul(dm.centre)));
        const float along = max(0.0f, fabsf(rel.dot(f)) - dm.halfL), side = max(0.0f, fabsf(rel.dot(r)) - dm.halfW);
        const float top = vehicleTop(v, dm);
        const float up = centre.z > top ? centre.z - top : centre.z < v.pos.z ? v.pos.z - centre.z : 0;
        const float distance = sqrtf(along * along + side * side + up * up);
        if(distance >= radius) continue;
        out[count] = i;
        distances[count] = distance;
        ++count;
    }
    return count;
}

float halovehiclehit(int index, float damage, const HaloDamage *source, bool explosive, playerent *attacker)
{
    if(!haloactive() || !halo_vehicledamage || !vehicles.inrange(index) || !(damage > 0) || halocinematic()) return 0;
    Vehicle &v = vehicles[index];
    const VehicleDef *d = defOf(v);
    if(!destructible(v) || v.destroyed) return 0;
    if((d->collFlags & collOnlyExplosives) && !explosive) return 0;
    if((d->collFlags & collOnlyOccupied) && !occupied(v)) return 0;
    float amount = damage * (source ? source->material(d->material) : 1.0f) * d->bodyMultiplier;
    // The player's own vehicle takes enemy fire as the player does on this difficulty; any other
    // vehicle has the difficulty's enemy toughness.
    if(index == playerVehicle) { if(attacker != player1) amount *= halodifficultydamage(); }
    else amount /= max(.1f, halodifficultyhealth());
    if(!(amount > 0)) return 0;
    v.health = max(0.0f, v.health - amount / d->vitality);
    v.sleeping = false;
    if(halo_vehicletrace)
        conoutf("Halo vehicle %d (%s) takes %.2f%s from %s: health %.3f", index, d->displayName.c_str(), amount, explosive ? " (blast)" : "",
                attacker ? (attacker == player1 ? "the player" : attacker->name) : "-", v.health);
    if(v.health <= 0) destroyVehicle(v, index, attacker);
    return amount;
}

float halovehiclehealth(int object)
{
    const int index = find(object);
    if(index < 0 || !vehicles[index].exists) return -1;
    const Vehicle &v = vehicles[index];
    const VehicleDef *d = defOf(v);
    return d && d->vitality > 0 ? clamp(v.health, 0.0f, 1.0f) : 1.0f;
}

bool halovehicleriderstunned(const playerent *unit)
{
    if(!unit) return false;
    loopv(vehicles)
    {
        const Vehicle &v = vehicles[i];
        const VehicleDef *d = defOf(v);
        if(!v.exists || !d || lastmillis >= v.stunnedUntil) continue;
        // Only the gun seat: a passenger fires its own weapon, which an EMP on the car leaves alone.
        loopj(maxSeats) if(v.riders[j] == unit) return j < int(d->seats.size()) && d->seats[j].role == HALO_SEAT_GUNNER;
    }
    return false;
}

namespace
{
    int carType()
    {
        loadDefs();
        for(int i = 0; i < int(defs.size()); ++i) if(defs[i].vehicleClass == HALO_VEHICLE_CAR) return i;
        return -1;
    }

    // The scenario's own Warthog nearest `feet` within `radius` that the escape can take: placed
    // in this section and allowed on this difficulty, upright, its driver's seat free, standing
    // there already or (placed by a script, d40's trench_jeep1 to 3 and asspain_1 to 3) not
    // created yet. Taking it, rather than spawning another, leaves no second Warthog for the
    // script to create on top of the player's, and the script's seat tests name it.
    int placedWarthog(int type, const vec &feet, float radius)
    {
        const int driver = seatByRole(defs[type], HALO_SEAT_DRIVER);
        int index = -1;
        float best = radius;
        loopv(vehicles)
        {
            const Vehicle &v = vehicles[i];
            if(!v.placed || v.type != type || v.carried || !difficultyAllows(v.flags) || (driver >= 0 && v.riders[driver])) continue;
            if(v.exists ? v.flipped : !(v.flags & 1)) continue;
            const float dd = (v.exists ? v.pos : v.spawnFeet).dist(feet);
            if(dd < best) { best = dd; index = i; }
        }
        return index;
    }

    // Creates a placement the script has not created yet, where it was placed.
    void takeOver(int index)
    {
        Vehicle &v = vehicles[index];
        if(!v.exists)
        {
            respawn(v, true);
            if(halo_vehicletrace) conoutf("Halo vehicle %d created for the escape (object %d)", index, v.object);
        }
        v.escape = true;
    }
}

bool halovehicleescape(const vec &feet, float yaw)
{
    const int type = carType();
    if(type < 0) return false;
    const int driver = seatByRole(defs[type], HALO_SEAT_DRIVER);
    int index = -1;
    float best = 1e9f;
    loopv(vehicles)
    {
        const Vehicle &v = vehicles[i];
        if(!v.exists || v.flipped || v.type != type || v.carried || (driver >= 0 && v.riders[driver])) continue;
        const float dd = v.pos.dist(feet);
        if(dd < dims(v).length * 3 && dd < best) { best = dd; index = i; }
    }
    if(index < 0)
    {
        // The scenario's Warthog parked near the start (section 6's six at the elevator landing).
        Vehicle probe;
        probe.type = type;
        index = placedWarthog(type, feet, max(40.0f, dims(probe).length * 10));
    }
    if(index >= 0) takeOver(index);
    else
    {
        // None in the scenario here (sections 7 and 8): a Warthog of the engine's own.
        index = add(type, feet, yaw, -1, 0);
        if(index < 0) return false;
        Vehicle &v = vehicles[index];
        respawn(v, true);
        placeClear(v, index, feet);
        v.spawnFeet = v.pos;
        v.escape = true;
    }
    if(!seatPlayer(index, driver)) return false;
    player1->yaw = vehicles[index].yaw;
    conoutf("Halo vehicle: escape Warthog at %.1f,%.1f,%.1f (object %d)", vehicles[index].pos.x, vehicles[index].pos.y,
            vehicles[index].pos.z, vehicles[index].object);
    return true;
}

void halovehicleensure(const vec &feet, float yaw, float radius)
{
    const int type = carType();
    if(type < 0) return;
    loopv(vehicles)
    {
        const Vehicle &v = vehicles[i];
        if(v.exists && !v.flipped && v.type == type && v.pos.dist(feet) < radius) return;
    }
    // A parked Warthog the script has not created (a section launched on its own, before the
    // script got there): it appears where it was placed.
    int index = placedWarthog(type, feet, radius);
    if(index >= 0)
    {
        takeOver(index);
        const Vehicle &v = vehicles[index];
        conoutf("Halo vehicle: a Warthog for the escape at %.1f,%.1f,%.1f (object %d)", v.pos.x, v.pos.y, v.pos.z, v.object);
        return;
    }
    index = add(type, feet, yaw, -1, 0);
    if(index < 0) return;
    Vehicle &v = vehicles[index];
    v.spawnYaw = yaw;
    const Dims d = dims(v);
    // Beside the player, to the right, clear of the body (or the nearest spot that is clear).
    v.spawnFeet = vec(feet).add(vec(rightOf(yaw)).mul(d.halfW + player1->radius + 1.5f));
    respawn(v, true);
    placeClear(v, index, v.spawnFeet);
    v.spawnFeet = v.pos;
    v.escape = true;
    conoutf("Halo vehicle: a Warthog for the escape at %.1f,%.1f,%.1f", v.pos.x, v.pos.y, v.pos.z);
}

bool halovehiclereclaim(const char *why)
{
    const int index = thrownVehicle, seat = thrownSeat;
    thrownVehicle = thrownSeat = -1;
    if(!haloactive() || index < 0 || !vehicles.inrange(index) || lastmillis - thrownAt > 15000 || halovehicleseated() ||
       player1->state != CS_ALIVE) return false;
    Vehicle &v = vehicles[index];
    const VehicleDef *d = defOf(v);
    if(!v.exists || !d || v.carried || seat < 0 || seat >= int(d->seats.size()) || (v.riders[seat] && v.riders[seat] != player1)) return false;
    // Back on its wheels where it lies if it fits there, else nearby or at its last safe spot.
    if(v.flipped || chassisEmbedded(v, dims(v)))
    {
        Vehicle test = v;
        settle(test, v.pos);
        const Dims dm = dims(test);
        if(footprintFree(index, test, test.pos, false) && !chassisEmbedded(test, dm) && wheelsGrounded(test, dm))
        {
            const float yaw = v.yaw;
            settle(v, v.pos);
            v.yaw = yaw;
        }
        else if(!recoverVehicle(v, index, why, false, false)) return false;
    }
    if(!seatPlayer(index, seat)) return false;
    conoutf("Halo vehicle recovery: %s, player put back in the %s at %.1f,%.1f,%.1f", why, d->displayName.c_str(), v.pos.x, v.pos.y, v.pos.z);
    return true;
}

bool halovehiclestatus(HaloVehicleStatus &status)
{
    if(!halovehicleseated()) return false;
    const Vehicle &v = vehicles[playerVehicle];
    const VehicleDef &d = *defOf(v);
    status.name = d.displayName.c_str();
    status.vehicleClass = d.vehicleClass;
    status.role = d.seats[playerSeat].role;
    status.speed = playerSpeed(v);
    status.flipped = v.flipped;
    status.airborne = !v.grounded && (d.vehicleClass == HALO_VEHICLE_CAR || d.vehicleClass == HALO_VEHICLE_HOVER || d.vehicleClass == HALO_VEHICLE_TANK);
    status.driverFires = driverFires(d);
    const HaloWeaponDef *gun = armedSeat(d, playerSeat) ? haloweapon(v.gun.def) : nullptr;
    status.weapon = gun ? gun->index : -1;
    status.secondary = gun && gun->trigger(1) && gun->trigger(1)->projectile.valid;
    if(d.vehicleClass == HALO_VEHICLE_FLYER)
    {
        float floor;
        bool wall;
        const float s = vehicleScale(), reach = 60 * s;
        if(groundAt(v.pos.x, v.pos.y, v.pos.z + .1f, v.pos.z - reach, floor, wall)) status.altitude = (v.pos.z - floor) / max(s, 1e-3f);
    }
    status.heat = gun && gun->heatOverheat > 0 ? clamp(v.gun.heat / gun->heatOverheat, 0.0f, 1.0f) : 0;
    status.overheated = v.gun.overheated;
    status.firing = gun && lastmillis - lastFire < 150;
    return true;
}

void halovehiclesrender()
{
    if(!haloactive()) return;
    loopv(vehicles)
    {
        Vehicle &v = vehicles[i];
        VehicleDef *d = v.type >= 0 && v.type < int(defs.size()) ? &defs[v.type] : nullptr;
        if(!v.exists || !d || !hasModel(*d)) continue;
        const float s = vehicleScale();
        if(camera1->o.dist(v.pos) > 3200) continue;
        // Orientation as the scene renderer takes it: the retail frame (y flipped) angles.
        const float yawRad = v.yaw * RAD, p = v.pitch * RAD, r = v.roll * RAD;
        const vec f(sinf(yawRad) * cosf(p), -cosf(yawRad) * cosf(p), sinf(p));
        const vec upLevel(-sinf(yawRad) * sinf(p), cosf(yawRad) * sinf(p), cosf(p)), right = rightOf(v.yaw);
        const vec up = vec(upLevel).mul(cosf(r)).add(vec(right).mul(sinf(r)));
        const vec rf(f.x, -f.y, f.z), ru(up.x, -up.y, up.z);
        const float heading = atan2f(rf.y, rf.x), pitch = atan2f(rf.z, sqrtf(rf.x * rf.x + rf.y * rf.y));
        const vec rr(sinf(heading), -cosf(heading), 0), lu(-cosf(heading) * sinf(pitch), -sinf(heading) * sinf(pitch), cosf(pitch));
        const float roll = atan2f(ru.dot(rr), ru.dot(lu));
        // Turrets turn with their gunner.
        const float renderHeading = d->vehicleClass == HALO_VEHICLE_TURRET ? (90 - v.yaw) * RAD : heading;
        // A hover craft floats over its floor point, bobbing a little.
        const float lift = liftOf(*d, s), bob = lift > 0 ? sinf(lastmillis / 300.0f + i) * .02f * s : 0;
        const vec at(v.pos.x, v.pos.y, v.smoothZ + lift + bob);
        rendermodel(d->model.c_str(), ANIM_MAPMODEL, 0, -1, at, roll / RAD, 180 - renderHeading / RAD, pitch / RAD, 0, 0,
                    nullptr, nullptr, s / d->modelScale);
    }
}

void halovehiclesrenderplaceholders()
{
    if(!haloactive()) return;
    bool any = false;
    loopv(vehicles)
    {
        Vehicle &v = vehicles[i];
        VehicleDef *d = v.type >= 0 && v.type < int(defs.size()) ? &defs[v.type] : nullptr;
        if(!v.exists || !d || hasModel(*d)) continue;
        if(!any) { glDisable(GL_TEXTURE_2D); glColor3f(.55f, .8f, .45f); any = true; }
        const Dims dm = dims(v);
        const vec f = forwardOf(v.yaw), r = rightOf(v.yaw), c = vec(v.pos).add(vec(f).mul(dm.centre));
        vec corners[8];
        loopj(8)
            corners[j] = vec(c).add(vec(f).mul(j & 1 ? dm.halfL : -dm.halfL)).add(vec(r).mul(j & 2 ? dm.halfW : -dm.halfW))
                         .add(vec(0, 0, j & 4 ? dm.height : 0));
        static const int edges[12][2] = {{0,1},{2,3},{4,5},{6,7},{0,2},{1,3},{4,6},{5,7},{0,4},{1,5},{2,6},{3,7}};
        glBegin(GL_LINES);
        for(const auto &e : edges) { glVertex3fv(corners[e[0]].v); glVertex3fv(corners[e[1]].v); }
        glEnd();
    }
    if(any) { glColor3f(1, 1, 1); glEnable(GL_TEXTURE_2D); }
}

bool halovehiclecollide(physent *d, float &hi, float &lo)
{
    if(!halo_vehiclecollide || !haloactive() || (d->type != ENT_PLAYER && d->type != ENT_BOT) || vehicles.empty()) return false;
    if(d == player1 && halovehicleseated()) return false;
    const float feet = d->o.z - d->eyeheight, height = d->eyeheight + d->aboveeye;
    loopv(vehicles)
    {
        const Vehicle &v = vehicles[i];
        if(!v.exists || !defOf(v)) continue;
        bool rider = false;
        for(playerent *p : v.riders) rider |= p == d;
        if(rider) continue;
        const Dims dm = dims(v);
        const vec f = forwardOf(v.yaw), r = rightOf(v.yaw);
        const vec rel = vec(d->o).sub(vec(v.pos).add(vec(f).mul(dm.centre)));
        if(fabsf(rel.dot(f)) >= dm.halfL + d->radius || fabsf(rel.dot(r)) >= dm.halfW + d->radius) continue;
        const float bottom = v.pos.z, top = vehicleTop(v, dm);
        // A body inside that no push could get out (depenetrate) may walk out of this vehicle.
        if(trappedIn(d, i)) continue;
        if(feet >= top - .42f) lo = max(lo, top);
        else if(feet + height <= bottom + .1f) hi = min(hi, bottom);
        else return true;
        if(hi - lo < height) return true;
    }
    return false;
}

int halovehicleat(const vec &point, float reach)
{
    if(!haloactive()) return -1;
    loopv(vehicles)
    {
        const Vehicle &v = vehicles[i];
        if(!v.exists || !defOf(v)) continue;
        const Dims dm = dims(v);
        const vec f = forwardOf(v.yaw), r = rightOf(v.yaw);
        const vec rel = vec(point).sub(vec(v.pos).add(vec(f).mul(dm.centre)));
        const float top = v.pos.z + (v.flipped ? dm.height * .6f : dm.height);
        if(fabsf(rel.dot(f)) <= dm.halfL + reach && fabsf(rel.dot(r)) <= dm.halfW + reach &&
           point.z >= v.pos.z - reach && point.z <= top + reach) return i;
    }
    return -1;
}

bool halovehicleattach(int index, const vec &point, vec &local)
{
    if(!vehicles.inrange(index) || !vehicles[index].exists) return false;
    const Vehicle &v = vehicles[index];
    const vec rel = vec(point).sub(v.pos);
    local = vec(rel.dot(forwardOf(v.yaw)), rel.dot(rightOf(v.yaw)), rel.z);
    return true;
}

bool halovehicleattached(int index, const vec &local, vec &point)
{
    if(!vehicles.inrange(index) || !vehicles[index].exists) return false;
    const Vehicle &v = vehicles[index];
    point = vec(v.pos).add(vec(forwardOf(v.yaw)).mul(local.x)).add(vec(rightOf(v.yaw)).mul(local.y)).add(vec(0, 0, local.z));
    return true;
}

bool halovehiclebound(int object) { return find(object) >= 0; }

void halovehicleobject(int object, bool exists, bool reset, bool bulk)
{
    const int index = find(object);
    if(index < 0) return;
    Vehicle &v = vehicles[index];
    if(!exists)
    {
        // Bulk clean-ups (object_destroy_all) leave the player's own vehicle alone.
        if(!v.exists || (bulk && index == playerVehicle)) return;
        ejectRiders(v, index);
        v.exists = false;
        if(halo_vehicletrace) conoutf("Halo vehicle %d destroyed by the script", index);
        return;
    }
    if(index == playerVehicle) return;
    if(!v.exists || reset)
    {
        ejectRiders(v, index);
        respawn(v, true);
        if(halo_vehicletrace) conoutf("Halo vehicle %d created by the script", index);
    }
}

void halovehicleteleport(int object, const vec &feet, float yaw)
{
    const int index = find(object);
    if(index < 0) return;
    Vehicle &v = vehicles[index];
    const bool keepPlayer = index == playerVehicle;
    settle(v, feet);
    // A Banshee keeps the flag's height (a30's lz_banshee flags are in the air): with a pilot it
    // flies from there, empty it falls.
    if(defOf(v)->vehicleClass == HALO_VEHICLE_FLYER) { v.pos = feet; v.smoothZ = feet.z; v.stillZ = feet.z; v.grounded = false; }
    v.yaw = yaw;
    if(keepPlayer) placePlayerOnSeat();
}

bool halovehicleseattest(int object, const char *seat, const playerent *unit)
{
    const int index = find(object);
    if(index < 0 || !unit) return false;
    const Vehicle &v = vehicles[index];
    const VehicleDef &d = *defOf(v);
    if(!v.exists) return false;
    if(!seat || !*seat)
    {
        for(const playerent *p : v.riders) if(p == unit) return true;
        return false;
    }
    const int s = seatByName(d, seat);
    return s >= 0 && v.riders[s] == unit;
}

bool halovehicleload(int object, const char *seat, playerent *unit)
{
    const int index = find(object);
    if(index < 0 || !unit || unit->state != CS_ALIVE) return false;
    Vehicle &v = vehicles[index];
    const VehicleDef &d = *defOf(v);
    // A vehicle that blew up (halovehiclehit) takes nobody until the script creates it again
    // (object_create respawns it whole, halovehicleobject); as in Halo, loading into it fails.
    if(!v.exists && v.destroyed) return false;
    if(!v.exists) respawn(v, true);
    int s = seatByName(d, seat);
    if(s < 0) return false;
    if(unit == player1) return seatPlayer(index, s);
    halovehicleunseat(unit);
    if(v.riders[s] == player1) return false;
    v.riders[s] = unit;
    return true;
}

int halovehicleunload(int object, const char *seat)
{
    const int index = find(object);
    if(index < 0) return 0;
    Vehicle &v = vehicles[index];
    const VehicleDef &d = *defOf(v);
    int released = 0;
    loopi(maxSeats)
    {
        if(!v.riders[i] || (seat && *seat && i < int(d.seats.size()) && seatByName(d, seat) != i)) continue;
        if(v.riders[i] == player1) { if(unseatPlayer(true)) ++released; }
        else { v.riders[i] = nullptr; ++released; }
    }
    return released;
}

bool halovehicleescapeseat(int object, const char *seat, const playerent *unit)
{
    if(unit != player1 || !halovehicleseated()) return false;
    const Vehicle &v = vehicles[playerVehicle];
    const VehicleDef &d = *defOf(v);
    if(!v.escape || d.vehicleClass != HALO_VEHICLE_CAR) return false;
    // A scenario Warthog the escape took (d40's trench_jeep1) is itself, and stands in for the
    // scenario's Warthogs that are not here (in sections 7 and 8, trench_jeep1 to 3 and
    // asspain_1 to 3), but not for one standing here that the player is not in. The engine's
    // own (no scenario object) stands in for every one, as before.
    if(v.object >= 0 && v.object != object)
    {
        const int other = find(object);
        if(other >= 0 && vehicles[other].exists) return false;
    }
    return !seat || !*seat || seatByName(d, seat) == playerSeat;
}

bool halovehicleunseat(const playerent *unit)
{
    if(!unit) return false;
    if(unit == player1) return halovehicleseated() && unseatPlayer(true);
    bool found = false;
    loopv(vehicles) for(playerent *&p : vehicles[i].riders) if(p == unit) { p = nullptr; found = true; }
    return found;
}

bool halovehicleinside(const playerent *unit)
{
    if(!unit) return false;
    if(unit == player1) return halovehicleseated();
    loopv(vehicles) for(const playerent *p : vehicles[i].riders) if(p == unit) return true;
    return false;
}

void halovehicleenterable(int object, bool player)
{
    const int index = find(object);
    if(index >= 0) vehicles[index].enterable = player;
}

void halovehicleaiboard(int object, float distance, int actorKind, bool disabled)
{
    const int index = find(object);
    if(index < 0) return;
    Vehicle &v = vehicles[index];
    if(distance >= 0) v.aiDistance = distance;
    if(actorKind >= -1 && !disabled) v.aiKind = actorKind;
    if(disabled) v.aiDisabled = true;
}

bool halovehicleaigoto(int object, const char *seat, playerent *const *units, int count)
{
    const int index = find(object);
    if(index < 0) return false;
    Vehicle &v = vehicles[index];
    const VehicleDef &d = *defOf(v);
    const int s = seatByName(d, seat);
    if(!v.exists || s < 0 || v.riders[s]) return true;
    for(const Boarding &b : boardings) if(b.vehicle == index && b.seat == s) return true;
    // The nearest of the squad boards at once if it is within the enterable distance; else the
    // nearest one walks there (haloactorgoto) and boards on arrival (updateBoardings).
    const float range = max(v.aiDistance, 3.0f) * vehicleScale() + dims(v).halfL;
    playerent *best = nullptr;
    float distance = 1e9f;
    loopi(count)
    {
        playerent *p = units[i];
        if(!p || p->state != CS_ALIVE || halovehicleinside(p)) continue;
        bool walking = false;
        for(const Boarding &b : boardings) walking |= b.unit == p;
        if(walking) continue;
        const float dd = p->o.dist(seatPoint(v, s));
        if(dd < distance) { distance = dd; best = p; }
    }
    if(!best) return true;
    if(distance <= range)
    {
        v.riders[s] = best;
        v.sleeping = false;
        return true;
    }
    boardings.push_back({best, index, s, lastmillis, lastmillis + 30000});
    const vec entry = modelPoint(v, d.seats[s].enter, vehicleScale());
    haloactorgoto(best, &entry);
    if(halo_vehicletrace) conoutf("Halo vehicle: %s walks to the %s (%s)", best->name, d.displayName.c_str(), d.seats[s].name.c_str());
    return true;
}

void halovehicleforget(const playerent *unit)
{
    if(!unit || unit == player1) return;
    loopv(vehicles) for(playerent *&p : vehicles[i].riders) if(p == unit) p = nullptr;
    for(Vehicle &v : saved) for(playerent *&p : v.riders) if(p == unit) p = nullptr;
    boardings.erase(std::remove_if(boardings.begin(), boardings.end(), [unit](const Boarding &b) { return b.unit == unit; }), boardings.end());
    trapped.erase(std::remove_if(trapped.begin(), trapped.end(), [unit](const Trap &t) { return t.body == unit; }), trapped.end());
    struck.erase(std::remove(struck.begin(), struck.end(), static_cast<const physent *>(unit)), struck.end());
}

bool halovehiclearmedseat(const playerent *unit)
{
    if(!unit || unit == player1) return false;
    loopv(vehicles)
    {
        const Vehicle &v = vehicles[i];
        const VehicleDef *d = defOf(v);
        if(!v.exists || !d) continue;
        loopj(min(maxSeats, int(d->seats.size()))) if(v.riders[j] == unit) return armedSeat(*d, j);
    }
    return false;
}

playerent *halovehicledriverof(int object)
{
    const int index = find(object);
    return index >= 0 && vehicles[index].exists ? driverOf(vehicles[index]) : nullptr;
}

int halovehicleaigoing(int object)
{
    const int index = find(object);
    int count = 0;
    for(const Boarding &b : boardings) count += b.vehicle == index && index >= 0;
    return count;
}

void halovehiclesecondary(bool down) { secondaryHeld = down && halovehicleseated(); }
void halovehiclejump(bool down) { jumpHeld = down; }
void halovehiclecrouch(bool down) { crouchHeld = down; }

int halovehicleensuretype(const char *name, const vec &feet, float yaw)
{
    loadDefs();
    int type = -1;
    for(const VehicleDef &d : defs) if(d.name == name) type = d.index;
    if(type < 0 || !defs[type].player) return -1;
    const int driver = seatByRole(defs[type], HALO_SEAT_DRIVER);
    // One standing in the section that the player may take. Placements the script has not
    // created yet are left to the script: it gives some of them to its own pilots (c40's
    // c_banshee_1), so the flight gets a Banshee of the engine's own instead.
    loopv(vehicles)
    {
        const Vehicle &v = vehicles[i];
        if(v.exists && v.type == type && !v.carried && v.enterable && !v.flipped && !driverOf(v) && !(driver >= 0 && v.riders[driver]))
            return i;
    }
    const int index = add(type, feet, yaw, -1, 0);
    if(index < 0) return -1;
    Vehicle &v = vehicles[index];
    const Dims d = dims(v);
    v.spawnFeet = vec(feet).add(vec(rightOf(yaw)).mul(d.halfW + player1->radius + 1.5f));
    respawn(v, true);
    placeClear(v, index, v.spawnFeet);
    v.spawnFeet = v.pos;
    conoutf("Halo vehicle: a %s for the flight at %.1f,%.1f,%.1f", defs[type].displayName.c_str(), v.pos.x, v.pos.y, v.pos.z);
    return index;
}

int halovehicleengineloops() { return int(engineLoops.size()); }
void halovehiclessounds() { updateEngineLoops(); }

int halovehiclecount() { return vehicles.length(); }
int halovehicledefcount() { loadDefs(); return int(defs.size()); }

void halovehicledump(const char *label)
{
    fflush(stdout);
    HaloVehicleStatus st;
    const bool seated = halovehiclestatus(st);
    conoutf("VEHICLE_QA %s count=%d seated=%d vehicle=%d seat=%d role=%d gun=%d shots=%d heat=%.2f scale=%.2f alive=%d "
            "player=%.2f,%.2f,%.2f inside=%d trapped=%d", label, vehicles.length(), seated, playerVehicle, playerSeat, seated ? st.role : -1,
            seated ? st.weapon : -1, shotsFired, seated ? st.heat : 0.0f, vehicleScale(), player1->state == CS_ALIVE,
            player1->o.x, player1->o.y, player1->o.z, halovehicleqainside(player1), int(trapped.size()));
    loopv(vehicles)
    {
        const Vehicle &v = vehicles[i];
        const VehicleDef *d = defOf(v);
        int riders = 0;
        for(const playerent *p : v.riders) riders += p != nullptr;
        const playerent *pilot = driverOf(v);
        conoutf("VEHICLE_QA %s vehicle=%d type=%s object=%d exists=%d pos=%.2f,%.2f,%.2f yaw=%.1f pitch=%.1f roll=%.1f "
                "speed=%.2f grounded=%d flipped=%d riders=%d escape=%d placed=%d safe=%d length=%.2f stunned=%d class=%d "
                "pilot=%s phase=%d target=%s carried=%d shots=%d",
                label, i, d ? d->name.c_str() : "?", v.object, v.exists, v.pos.x, v.pos.y, v.pos.z, v.yaw, v.pitch, v.roll,
                playerSpeed(v), v.grounded, v.flipped, riders, v.escape, v.placed, v.safeCount, d ? dims(v).length : 0.0f,
                max(0, v.stunnedUntil - lastmillis), d ? d->vehicleClass : -1, pilot ? (pilot == player1 ? "player" : pilot->name) : "-",
                v.aiPhase, pilot && pilot != player1 && haloactortarget(pilot) ? haloactortarget(pilot)->name : "-", v.carried,
                v.gun.lastShot ? 1 : 0);
        // A line of its own (conoutf cuts lines at 260 characters): body health, vitality and the
        // variant its gun fires as (halovehiclehit, variantOf).
        const HaloActorDef *variant = d ? variantOf(*d) : nullptr;
        const char *object = haloscenarioobjectname(v.object);
        conoutf("VEHICLE_HEALTH %s vehicle=%d name=%s health=%.3f destroyed=%d vitality=%g flags=%u material=%d occupied=%d "
                "variant=%d modifier=%g error=%g", label, i, object && *object ? object : "-", v.health, v.destroyed,
                d ? d->vitality : 0.0f, d ? d->collFlags : 0u, d ? d->material : -1, occupied(v), variant ? variant->index : -1,
                variant ? variant->damageScale : 0.0f, variant ? variant->error : 0.0f);
    }
    fflush(stdout);
}

float halovehicleqadamage(int vehicle, float amount, bool explosive)
{
    return halovehiclehit(vehicle, amount, nullptr, explosive, player1);
}

bool halovehicleqapose(vec &pos, float &yaw)
{
    if(!halovehicleseated()) return false;
    pos = vehicles[playerVehicle].pos;
    yaw = vehicles[playerVehicle].yaw;
    return true;
}

int halovehicleqafind(const char *name, const vec &near, vec &entry)
{
    int best = -1;
    float distance = 1e9f;
    loopv(vehicles)
    {
        const Vehicle &v = vehicles[i];
        const VehicleDef *d = defOf(v);
        if(!v.exists || !d || d->name != name || v.carried || !v.enterable) continue;
        const int seat = seatByRole(*d, HALO_SEAT_DRIVER);
        if(seat < 0 || v.riders[seat]) continue;
        const vec at = modelPoint(v, d->seats[seat].enter, dims(v).s);
        const float dd = at.dist(near);
        if(dd < distance) { distance = dd; best = i; entry = vec(at.x, at.y, v.pos.z); }
    }
    return best;
}

bool halovehiclecentre(int vehicle, vec &centre)
{
    if(!vehicles.inrange(vehicle) || !vehicles[vehicle].exists || !defOf(vehicles[vehicle])) return false;
    centre = boxCentre(vehicles[vehicle], dims(vehicles[vehicle]));
    return true;
}

void halovehicleqainput(float throttle, float steer, int millis)
{
    qaThrottle = clamp(throttle, -1.0f, 1.0f);
    qaSteer = fabsf(steer) > 1.5f ? 2.0f : clamp(steer, -1.0f, 1.0f);
    qaUntil = lastmillis + max(0, millis);
    qaFlight = false;
}

void halovehicleqaflight(float throttle, float strafe, float climb, int millis)
{
    qaThrottle = clamp(throttle, -1.0f, 1.0f);
    qaStrafe = clamp(strafe, -1.0f, 1.0f);
    qaClimb = clamp(climb, -1.0f, 1.0f);
    qaSteer = 2;
    qaUntil = lastmillis + max(0, millis);
    qaFlight = true;
}

bool halovehicleqapilot(int vehicle, playerent *unit, const char *seat)
{
    if(!vehicles.inrange(vehicle) || !vehicles[vehicle].exists || !unit || unit == player1 || unit->state != CS_ALIVE) return false;
    Vehicle &v = vehicles[vehicle];
    const VehicleDef &d = *defOf(v);
    const int s = seat && *seat ? seatByName(d, seat) : seatByRole(d, HALO_SEAT_DRIVER);
    if(s < 0 || (v.riders[s] && v.riders[s] != unit)) return false;
    halovehicleunseat(unit);
    v.riders[s] = unit;
    v.sleeping = false;
    return true;
}

bool halovehicleqaenter(int vehicle, int role)
{
    if(!vehicles.inrange(vehicle)) return false;
    const VehicleDef *d = defOf(vehicles[vehicle]);
    return d && seatPlayer(vehicle, seatByRole(*d, role));
}

bool halovehicleqaflip(int vehicle)
{
    if(!vehicles.inrange(vehicle) || !vehicles[vehicle].exists) return false;
    Vehicle &v = vehicles[vehicle];
    if(v.flipped) { flipBack(v, vehicle); return true; }
    setFlipped(v, vehicle, 180);
    v.grounded = true;
    return true;
}

bool halovehicleqaentry(int vehicle, int role, vec &feet)
{
    if(!vehicles.inrange(vehicle) || !vehicles[vehicle].exists) return false;
    const Vehicle &v = vehicles[vehicle];
    const VehicleDef &d = *defOf(v);
    const int seat = seatByRole(d, role);
    if(seat < 0) return false;
    feet = modelPoint(v, d.seats[seat].enter, vehicleScale());
    return true;
}

int halovehicleqaspawn(int type, float forward)
{
    const float yaw = player1->yaw;
    const vec feet = vec(player1->o).sub(vec(0, 0, player1->eyeheight)).add(vec(forwardOf(yaw)).mul(forward));
    const int index = add(type, feet, yaw, -1, 0);
    if(index >= 0) respawn(vehicles[index], true);
    return index;
}

int halovehicleqainside(const physent *body)
{
    if(!body || (body == player1 && halovehicleseated())) return -1;
    loopv(vehicles)
    {
        const Vehicle &v = vehicles[i];
        if(!v.exists || !defOf(v)) continue;
        bool rider = false;
        for(const playerent *p : v.riders) rider |= p == body;
        vec push;
        if(!rider && bodyInside(v, dims(v), body, push, 0)) return i;
    }
    return -1;
}

bool halovehicleqaplace(int vehicle, physent *body, float along, float side)
{
    // A vehicle the script has not created yet stands at its placement: a body put there is
    // inside it once the script creates it.
    if(!body || !vehicles.inrange(vehicle) || (body == player1 && halovehicleseated())) return false;
    const Vehicle &v = vehicles[vehicle];
    const Dims d = dims(v);
    const vec f = forwardOf(v.yaw), r = rightOf(v.yaw);
    const vec feet = vec(v.pos).add(vec(f).mul(d.centre + along * d.halfL)).add(vec(r).mul(side * d.halfW)).add(vec(0, 0, .05f));
    body->o = vec(feet).add(vec(0, 0, body->eyeheight));
    body->vel = vec(0, 0, 0);
    body->resetinterp();
    if(body == player1) haloplayermoved("QA teleport into a vehicle");
    return true;
}

bool halovehicleqapush(int vehicle, float speed)
{
    if(!vehicles.inrange(vehicle) || !vehicles[vehicle].exists) return false;
    Vehicle &v = vehicles[vehicle];
    v.vel = vec(forwardOf(v.yaw)).mul(speed);
    v.sleeping = false;
    v.stillSince = 0;
    return true;
}

bool halovehicleqastuck(int vehicle, const char *kind)
{
    if(!vehicles.inrange(vehicle) || !vehicles[vehicle].exists || !kind) return false;
    Vehicle &v = vehicles[vehicle];
    vec low, high;
    if(!strcmp(kind, "fall")) v.pos.z = halocollisionactive() && halocollisionbounds(low, high) ? low.z - 20 : -200;
    else if(!strcmp(kind, "sink")) v.pos.z -= dims(v).height * .5f;
    else return false;
    v.smoothZ = v.pos.z;
    v.vel = vec(0, 0, 0);
    v.grounded = v.sleeping = false;
    v.stillSince = 0;
    if(vehicle == playerVehicle) placePlayerOnSeat();
    return true;
}

bool halovehicleqathrow(int vehicle)
{
    if(!vehicles.inrange(vehicle) || !vehicles[vehicle].exists) return false;
    const VehicleDef *d = defOf(vehicles[vehicle]);
    if(!d || (!halovehicleseated() && !seatPlayer(vehicle, seatByRole(*d, HALO_SEAT_DRIVER)))) return false;
    setFlipped(vehicles[playerVehicle], playerVehicle, 180);
    vehicles[playerVehicle].grounded = true;
    qaForceThrow = true;
    const bool thrown = unseatPlayer(true);
    qaForceThrow = false;
    return thrown && thrownVehicle >= 0;
}
