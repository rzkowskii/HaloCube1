// Campaign vehicles: the Warthog the player drives (The Maw's escape run and wherever the
// scenario parks one), the Covenant gun turrets with a gunner seat, and the other scenario
// vehicles standing where they were placed. See halo_vehicle.h and docs/macos-build.md.
//
// A vehicle is a body the size of the retail vehicle at the section's scale. It drives on the
// retail collision when the section has one (four wheel traces and two chassis capsules) and on
// the stock cube grid otherwise. The handling is an arcade car: throttle and brakes along the
// heading, strong lateral grip with some slide, steering towards the camera (or the strafe
// keys), gravity, jumps off crests, pitch and roll following the ground, and flips when it lands
// or stands too far over; E flips it back. None of it is the retail physics.
#include "cube.h"
#include "halo.h"
#include "halo_collision.h"
#include "halo_script.h"
#include "halo_weapons.h"
#include "halo_ai.h"
#include "halo_vehicle.h"
#include "halo_state.h"
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

namespace
{
    constexpr int formatVersion = 1;
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
    };

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
        HaloWeaponState gun;
        int nextShot = 0;
        // Stuck recovery (updateStuck): the last spots it stood on upright, clear and grounded on
        // structure floor (newest last), and the fall, wedge and stall timers.
        vec safePos[safeSpotCount];
        float safeYaw[safeSpotCount] = {};
        int safeCount = 0, safeChecked = 0;
        int airSince = 0, embeddedSince = 0, stalledSince = 0, lastRecovery = 0;
        vec stalledAt = vec(0, 0, 0);
        float stillZ = 0;
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

    bool parse(const char *text)
    {
        std::istringstream lines(text);
        std::string line;
        VehicleDef *def = nullptr;
        bool versioned = false;
        while(std::getline(lines, line))
        {
            if(line.empty() || line[0] == '#') continue;
            std::istringstream in(line);
            std::string key;
            in >> key;
            bool ok = true;
            if(key == "format") { int version = 0; in >> version; ok = versioned = version == formatVersion; }
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
                def->vehicleClass = kind == "car" ? HALO_VEHICLE_CAR : kind == "turret" ? HALO_VEHICLE_TURRET : HALO_VEHICLE_PROP;
            }
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
                s.role = seatRole(s.name, s.marker);
                if(ok && def->seats.size() < size_t(maxSeats)) def->seats.push_back(s);
            }
            else if(key == "camera") ok = bool(in >> def->camera.x >> def->camera.y >> def->camera.z);
            else if(key == "end")
            {
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
            conoutf("Halo vehicles: packages/halo/vehicles.txt is missing; run tools/halo_vehicles.py (built-in Warthog only)");
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

    // Dimensions in AssaultCube units at the section's scale.
    struct Dims
    {
        float s, length, width, height, centre, halfL, halfW, step, wheelFront, wheelRear, wheelSide, wheelbase, track;
    };

    Dims dims(const Vehicle &v)
    {
        const VehicleDef &d = *defOf(v);
        Dims r;
        r.s = vehicleScale();
        r.length = max(.2f, d.high.x - d.low.x) * r.s;
        r.width = max(.2f, d.high.y - d.low.y) * r.s;
        r.height = max(.2f, d.high.z - max(0.0f, d.low.z)) * r.s;
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

    // A point of the model frame (x forward, y left, z up; Halo units) in the world, yaw only.
    vec modelPoint(const Vehicle &v, const vec &local, float s)
    {
        const vec f = forwardOf(v.yaw), r = rightOf(v.yaw);
        return vec(v.pos).add(vec(f).mul(local.x * s)).sub(vec(r).mul(local.y * s)).add(vec(0, 0, local.z * s));
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
                if(!delta.iszero() && halocollisionsweep(feet, delta, radius, height, hit) && hit.normal.z < .3f)
                {
                    // Already embedded (a spawn on a seam): let it move out rather than stick.
                    HaloCollisionHit inside;
                    if(hit.fraction <= 0 && halocollisionoverlap(feet, radius, height, inside) && inside.penetration > .05f) continue;
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
            if(!isPlayer && index == playerVehicle && def.vehicleClass == HALO_VEHICLE_CAR && speed > 8 &&
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

    // One physics step of `dt` seconds. `throttle` and `steer` are -1..1.
    void simulate(Vehicle &v, int index, float dt, float throttle, float steer)
    {
        const VehicleDef &def = *defOf(v);
        const Dims d = dims(v);
        if(def.vehicleClass != HALO_VEHICLE_CAR) throttle = steer = 0;
        const float top = max(minimumTopSpeed, def.forward * 30 * d.s);
        const float reverseTop = max(top * .4f, def.reverse * 30 * d.s);
        const float accelTime = clamp(def.acceleration > 0 ? def.forward / def.acceleration / 30 : 2.0f, 1.2f, 3.0f);
        const float accel = top / accelTime, brake = accel * 2.5f, coast = accel * .35f;
        vec f = forwardOf(v.yaw), r = rightOf(v.yaw);
        float vf = v.vel.dot(f), vr = v.vel.dot(r);
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
            // Steering: the turn rate is the speed over the wheelbase, capped by grip at speed.
            v.steer += clamp(steer - v.steer, -dt * 5, dt * 5);
            const float speed = fabsf(vf);
            const vec moving = vec(f).mul(vf).add(vec(r).mul(vr));
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
                    // Landing: on its back or side it has flipped.
                    if(fabsf(v.roll) > 75 || fabsf(v.pitch) > 75) setFlipped(v, index, fabsf(v.roll) > 120 ? 180.0f : copysignf(100.0f, v.roll));
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
                    // Standing too far over tips it onto its side.
                    if(fabsf(rollTarget) > 58 || fabsf(pitchTarget) > 80) setFlipped(v, index, copysignf(100.0f, rollTarget));
                }
                v.vel.z = 0;
            }
        }
        if(v.flipped && v.grounded) { v.vel.x *= max(0.0f, 1 - dt * 3); v.vel.y *= max(0.0f, 1 - dt * 3); }
        v.smoothZ = fabsf(v.pos.z - v.smoothZ) > 3 ? v.pos.z : v.smoothZ + (v.pos.z - v.smoothZ) * min(1.0f, dt * 18);
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
                if(halocollisionoverlap(feet, radius, height, hit) && hit.penetration > .05f) return false;
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
            if(halocollisionoverlap(feet, radius, height, hit) && hit.penetration > .1f) return true;
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
        const bool car = def.vehicleClass == HALO_VEHICLE_CAR;
        if(car) trackSafe(v, index, d);
        const char *why = nullptr;
        bool falling = false;
        vec low, high;
        if(outOfWorld(v)) { why = "fell out of the world"; falling = true; }
        else if(halocollisionactive() && halocollisionbounds(low, high))
        {
            if(v.grounded) v.airSince = 0;
            else if(!v.airSince) v.airSince = lastmillis;
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
                if(!chassisEmbedded(v, d) || vec(v.vel.x, v.vel.y, 0).magnitude() > 2) v.embeddedSince = 0;
                else if(!v.embeddedSince) v.embeddedSince = lastmillis;
                // Hung in mid-air (caught on an edge or in a crease) without moving at all.
                if(v.grounded || v.pos.dist(v.stalledAt) > .05f) { v.stalledSince = 0; v.stalledAt = v.pos; }
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

    // The seat's gun, fired at the crosshair while the player holds the trigger.
    void fireGun(Vehicle &v, int elapsed)
    {
        const VehicleDef &d = *defOf(v);
        const HaloWeaponDef *gun = haloweapon(v.gun.def);
        if(!gun) return;
        v.gun.tick(*gun, elapsed);
        const HaloTriggerDef *t = gun->trigger();
        if(!t || !player1->attacking || lastmillis < v.nextShot || !v.gun.canfire(*gun) || halocinematic()) return;
        if(!gun->usesBattery() && v.gun.loaded <= 0 && gun->magazine()) v.gun.loaded = max(1, gun->magazine()->loaded);
        const Dims dm = dims(v);
        const float yaw = player1->yaw * RAD, pitch = player1->pitch * RAD;
        const vec aim(sinf(yaw) * cosf(pitch), -cosf(yaw) * cosf(pitch), sinf(pitch));
        vec from = d.vehicleClass == HALO_VEHICLE_TURRET ? vec(v.pos).add(vec(0, 0, dm.height * .75f)).add(vec(aim).mul(dm.halfL))
                                                        : seatPoint(v, playerSeat).add(vec(0, 0, dm.height * .25f)).add(vec(aim).mul(dm.halfW));
        vec to = worldpos;
        if(vec(to).sub(from).dot(aim) <= .5f) to = vec(from).add(vec(aim).mul(200));
        halofiretrigger(player1, *gun, v.gun, 0, from, to);
        v.nextShot = lastmillis + v.gun.delay(*t);
        lastFire = lastmillis;
        ++shotsFired;
        player1->lastaction = lastmillis;
        audiomgr.playsound(d.vehicleClass == HALO_VEHICLE_TURRET ? S_SUBGUN : S_ASSAULT, SP_HIGH);
    }

    playerent *aliveBot(playerent *p)
    {
        if(!p || p == player1) return nullptr;
        loopv(players) if(players[i] == p) return p->state == CS_ALIVE ? p : nullptr;
        return nullptr;
    }

    void updateRiders(Vehicle &v)
    {
        loopi(maxSeats)
        {
            playerent *p = v.riders[i];
            if(!p || p == player1) continue;
            if(!aliveBot(p)) { v.riders[i] = nullptr; continue; }
            // Actors ride where their seat is (a turret gunner keeps firing its own weapon).
            p->o = seatPoint(v, i).add(vec(0, 0, .45f * p->eyeheight));
            p->vel = vec(0, 0, 0);
            p->onfloor = true;
            p->resetinterp();
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
}

void halovehiclesclear()
{
    loadDefs();
    vehicles.shrink(0);
    saved.clear();
    savedValid = restoredFromCheckpoint = false;
    sectionScale = 0;
    playerVehicle = playerSeat = -1;
    qaUntil = 0;
    trapped.clear();
    struck.clear();
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
    loopv(vehicles) { vehicles[i].airSince = vehicles[i].embeddedSince = vehicles[i].stalledSince = vehicles[i].lastRecovery = 0; vehicles[i].stalledAt = vehicles[i].pos; }
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
        if(!r.ok || r.more()) { why = "a vehicle record is damaged"; return false; }
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
    if(halovehicleseated()) { unseatPlayer(vehicles[playerVehicle].flipped); return true; }
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
    else formatstring(prompt)("E  Drive the %s", d.displayName.c_str());
    return prompt;
}

void halovehicledrive(int curtime)
{
    if(!halovehicleseated() || curtime <= 0) return;
    followPlayer();
    Vehicle &v = vehicles[playerVehicle];
    const VehicleDef &d = *defOf(v);
    const int role = d.seats[playerSeat].role;
    float throttle = 0, steer = 0;
    if(role == HALO_SEAT_DRIVER && !v.flipped)
    {
        throttle = clamp(float(player1->move), -1.0f, 1.0f);
        const float keys = -clamp(float(player1->strafe), -1.0f, 1.0f);
        steer = keys;
        if(!keys && !halo_vehiclesteer)
        {
            // Turn towards where the camera looks, as the retail Warthog does, reversing included.
            const float forwardSpeed = v.vel.dot(forwardOf(v.yaw));
            if(throttle || fabsf(forwardSpeed) > 2)
                steer = clamp(wrap180(player1->yaw - v.yaw) / 30.0f, -1.0f, 1.0f) * (forwardSpeed < -1 ? -1 : 1);
        }
        // QA input; a steer outside -1..1 keeps the camera steering.
        if(lastmillis < qaUntil) { throttle = qaThrottle; if(fabsf(qaSteer) <= 1) steer = qaSteer; }
    }
    player1->strafe = 0;
    player1->jumpnext = false;
    player1->trycrouch = false;
    if(role != HALO_SEAT_DRIVER) { v.aimYaw = player1->yaw; v.aimPitch = player1->pitch; }
    if(d.vehicleClass == HALO_VEHICLE_CAR || !v.grounded)
    {
        // Fixed 8 ms steps keep wall contacts and landings stable at any frame rate.
        for(int left = min(curtime, 100); left > 0; left -= 8)
            simulate(v, playerVehicle, min(left, 8) / 1000.0f, throttle, steer);
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
        float carriedYaw;
        if(v.object >= 0 && haloscenarioobjectcarried(v.object, carriedAt, carriedYaw))
        {
            v.carried = true;
            v.pos = carriedAt;
            v.smoothZ = v.pos.z;
            v.yaw = carriedYaw;
            v.vel = vec(0, 0, 0);
            v.grounded = v.sleeping = false;
            continue;
        }
        v.carried = false;
        if(i == playerVehicle)
        {
            followPlayer();
            if(d->seats[playerSeat].role != HALO_SEAT_DRIVER || d->vehicleClass == HALO_VEHICLE_TURRET) fireGun(v, elapsed);
            if(v.flipped && lastmillis - v.flippedAt > flipEject)
            {
                conoutf("Halo vehicle: the %s is over, player thrown out", d->displayName.c_str());
                // No room for the body beside it: the vehicle goes back on its wheels with the
                // driver still in it (where it lies if it fits, else nearby or its last safe spot);
                // failing that, out onto it as before.
                if(!unseatPlayer(false) && !recoverVehicle(v, i, "over with no room to get out", false)) unseatPlayer(true);
            }
        }
        else if(!v.sleeping && !halocinematic())
        {
            for(int left = min(elapsed, 100); left > 0; left -= 8) simulate(v, i, min(left, 8) / 1000.0f, 0, 0);
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
        if(boardTick) aiBoard(v);
    }
    depenetrate();
}

bool halovehiclecamera(physent &camera)
{
    if(!halovehicleseated() || editmode) return false;
    const Vehicle &v = vehicles[playerVehicle];
    const VehicleDef &d = *defOf(v);
    const Dims dm = dims(v);
    const int role = d.seats[playerSeat].role;
    const float yaw = player1->yaw * RAD, pitch = clamp(player1->pitch, -40.0f, 35.0f) * RAD;
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
    camera.pitch = clamp(player1->pitch, -40.0f, 35.0f) - (role == HALO_SEAT_DRIVER ? 6 : 0);
    camera.roll = 0;
    return true;
}

int halovehicledamage(int damage)
{
    if(!halovehicleseated() || damage <= 0 || damage == INT_MAX) return damage;
    const VehicleDef &d = *defOf(vehicles[playerVehicle]);
    // The Warthog's body takes some of the fire; a turret's gunner sits in the open.
    return d.vehicleClass == HALO_VEHICLE_CAR ? max(1, int(damage * .8f + .5f)) : damage;
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
    status.airborne = !v.grounded && d.vehicleClass == HALO_VEHICLE_CAR;
    const HaloWeaponDef *gun = status.role != HALO_SEAT_DRIVER || d.vehicleClass == HALO_VEHICLE_TURRET ? haloweapon(v.gun.def) : nullptr;
    status.weapon = gun ? gun->index : -1;
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
        const vec at(v.pos.x, v.pos.y, v.smoothZ);
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
    // No pathing: the nearest of the squad boards if it is within the enterable distance.
    const float range = max(v.aiDistance, 3.0f) * vehicleScale() + dims(v).halfL;
    playerent *best = nullptr;
    float distance = range;
    loopi(count)
    {
        playerent *p = units[i];
        if(!p || p->state != CS_ALIVE || halovehicleinside(p)) continue;
        const float dd = p->o.dist(seatPoint(v, s));
        if(dd < distance) { distance = dd; best = p; }
    }
    if(best) v.riders[s] = best;
    return true;
}

void halovehicleforget(const playerent *unit)
{
    if(!unit || unit == player1) return;
    loopv(vehicles) for(playerent *&p : vehicles[i].riders) if(p == unit) p = nullptr;
    for(Vehicle &v : saved) for(playerent *&p : v.riders) if(p == unit) p = nullptr;
    trapped.erase(std::remove_if(trapped.begin(), trapped.end(), [unit](const Trap &t) { return t.body == unit; }), trapped.end());
    struck.erase(std::remove(struck.begin(), struck.end(), static_cast<const physent *>(unit)), struck.end());
}

int halovehiclecount() { return vehicles.length(); }

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
        conoutf("VEHICLE_QA %s vehicle=%d type=%s object=%d exists=%d pos=%.2f,%.2f,%.2f yaw=%.1f pitch=%.1f roll=%.1f "
                "speed=%.2f grounded=%d flipped=%d riders=%d escape=%d placed=%d safe=%d length=%.2f",
                label, i, d ? d->name.c_str() : "?", v.object, v.exists, v.pos.x, v.pos.y, v.pos.z, v.yaw, v.pitch, v.roll,
                playerSpeed(v), v.grounded, v.flipped, riders, v.escape, v.placed, v.safeCount, d ? dims(v).length : 0.0f);
    }
    fflush(stdout);
}

void halovehicleqainput(float throttle, float steer, int millis)
{
    qaThrottle = clamp(throttle, -1.0f, 1.0f);
    qaSteer = fabsf(steer) > 1.5f ? 2.0f : clamp(steer, -1.0f, 1.0f);
    qaUntil = lastmillis + max(0, millis);
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
