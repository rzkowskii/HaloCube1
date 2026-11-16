// Vehicle verification hooks (tests/test_vehicles.py, tests/test_maw_drive.py). Compiled into
// halo.cpp only for halo-ac-qa; they read the campaign state there and drive halo_vehicle.cpp
// through its QA entry points. Lines start with VEHICLE_QA.

COMMANDF(qa_vehicle_state, "s", (const char *label) { halovehicledump(label); });
COMMANDF(qa_vehicle_enter, "ii", (int *vehicle, int *role)
{
    conoutf("VEHICLE_QA enter vehicle=%d role=%d ok=%d", *vehicle, *role, halovehicleqaenter(*vehicle, *role));
    fflush(stdout);
});
COMMANDF(qa_vehicle_use, "", ()
{
    execute("halo_use");
    conoutf("VEHICLE_QA use seated=%d vehicle=%d", halodriving(), halovehicleplayervehicle());
    fflush(stdout);
});
// Throttle and steering (-1..1) held for `ms`, in place of the keys.
COMMANDF(qa_vehicle_input, "ffi", (float *throttle, float *steer, int *ms) { halovehicleqainput(*throttle, *steer, *ms); });
COMMANDF(qa_vehicle_flip, "i", (int *vehicle)
{
    conoutf("VEHICLE_QA flip vehicle=%d ok=%d", *vehicle, halovehicleqaflip(*vehicle));
    fflush(stdout);
});
COMMANDF(qa_vehicle_spawn, "if", (int *type, float *forward)
{
    conoutf("VEHICLE_QA spawn type=%d vehicle=%d", *type, halovehicleqaspawn(*type, *forward));
    fflush(stdout);
});
// Feet at a vehicle seat's entry point (role 0 driver, 1 gunner), on foot, for the E tests.
// Vehicle -1: the player's own, else the nearest vehicle with such a seat.
COMMANDF(qa_vehicle_approach, "ii", (int *vehicle, int *role)
{
    int index = *vehicle < 0 ? halovehicleplayervehicle() : *vehicle;
    if(halodriving()) halovehicleuse(0);
    vec feet;
    if(index < 0)
    {
        float best = 1e9f;
        loopi(halovehiclecount())
        {
            vec at;
            if(!halovehicleqaentry(i, *role, at) || at.dist(player1->o) >= best) continue;
            best = at.dist(player1->o);
            index = i;
        }
    }
    const bool found = index >= 0 && halovehicleqaentry(index, *role, feet);
    if(found)
    {
        player1->o = vec(feet).add(vec(0, 0, player1->eyeheight + .2f));
        player1->vel = vec(0, 0, 0);
        player1->resetinterp();
        haloplayermoved("QA teleport");
    }
    const char *prompt = halovehicleprompt(1e9f);
    conoutf("VEHICLE_QA approach vehicle=%d found=%d prompt=%s", index, found, prompt ? prompt : "-");
    fflush(stdout);
});
COMMANDF(qa_vehicle_prompt, "", ()
{
    const char *prompt = halovehicleprompt(1e9f);
    conoutf("VEHICLE_QA prompt=%s", prompt ? prompt : "-");
    fflush(stdout);
});
COMMANDF(qa_vehicle_escape, "", ()
{
    const vec target = objective();
    HaloVehicleStatus st;
    const bool seated = halovehiclestatus(st);
    conoutf("VEHICLE_QA escape escaping=%d remaining=%d mission=%d section=%d stage=%d marker=%d/%d seated=%d speed=%.2f "
            "flipped=%d hp=%d pos=%.1f,%.1f,%.1f target=%.1f,%.1f,%.1f distance=%.1f",
            escaping, haloescapemillis(), mission + 1, section + 1, stage, nextRouteMarker,
            route[0].length() + route[1].length() + route[2].length(), seated, seated ? st.speed : 0.0f, seated && st.flipped,
            player1->health, player1->o.x, player1->o.y, player1->o.z, target.x, target.y, target.z, player1->o.distxy(target));
    fflush(stdout);
});

// Drives the seated vehicle at the current objective (the escape's route markers) for a while:
// full throttle, steering at the marker, backing off for a second when it stalls. Logs once a
// second. Not a racing line: it shows the run can be driven, not how fast.
static int qaAutopilotUntil = 0, qaAutopilotStalled = 0, qaAutopilotReverse = 0, qaAutopilotLog = 0;
COMMANDF(qa_vehicle_autopilot_step, "", ()
{
    if(!qaAutopilotUntil) return;
    if(!haloactive() || lastmillis >= qaAutopilotUntil || stage == COMPLETE)
    {
        conoutf("VEHICLE_QA autopilot done stage=%d section=%d", stage, section + 1);
        fflush(stdout);
        qaAutopilotUntil = 0;
        return;
    }
    if(!halocinematic() && player1->state == CS_ALIVE)
    {
        if(!halodriving()) halovehicleuse(1e9f);
        HaloVehicleStatus st;
        if(halovehiclestatus(st))
        {
            const vec target = objective();
            const vec delta = vec(target).sub(player1->o);
            float desired = atan2f(delta.x, -delta.y) / RAD;
            if(desired < 0) desired += 360;
            if(st.flipped) halovehicleuse(0);
            else if(lastmillis < qaAutopilotReverse) halovehicleqainput(-1, 0, 120);
            else
            {
                // The camera looks at the marker; the Warthog steers towards the camera.
                player1->yaw = desired;
                halovehicleqainput(1, 2, 120);
                if(st.speed < .5f) { if(!qaAutopilotStalled) qaAutopilotStalled = lastmillis; }
                else qaAutopilotStalled = 0;
                if(qaAutopilotStalled && lastmillis - qaAutopilotStalled > 1500)
                {
                    qaAutopilotStalled = 0;
                    qaAutopilotReverse = lastmillis + 900;
                }
            }
        }
        if(lastmillis - qaAutopilotLog >= 1000)
        {
            qaAutopilotLog = lastmillis;
            execute("qa_vehicle_escape");
        }
    }
    addsleep(50, "qa_vehicle_autopilot_step", true);
});
COMMANDF(qa_vehicle_autopilot, "i", (int *seconds)
{
    qaAutopilotUntil = lastmillis + max(1, *seconds) * 1000;
    qaAutopilotStalled = qaAutopilotReverse = qaAutopilotLog = 0;
    execute("qa_vehicle_autopilot_step");
});

// Bodies inside vehicles (tests/test_vehicles.py). qa_vehicle_place_player and
// qa_vehicle_place_actor put the player, or the nearest living actor not in a seat, with their
// feet inside vehicle N (along and side in half-lengths and half-widths from the box's centre);
// halovehiclesupdate should push them out within a frame or two.
static playerent *qaVehicleActor = NULL;
COMMANDF(qa_vehicle_place_player, "iff", (int *vehicle, float *along, float *side)
{
    if(halodriving()) halovehicleuse(0);
    const bool ok = halovehicleqaplace(*vehicle, player1, *along, *side);
    conoutf("VEHICLE_QA place player vehicle=%d ok=%d inside=%d", *vehicle, ok, halovehicleqainside(player1));
    fflush(stdout);
});
COMMANDF(qa_vehicle_place_actor, "iff", (int *vehicle, float *along, float *side)
{
    qaVehicleActor = NULL;
    float best = 1e9f;
    loopv(players)
    {
        playerent *p = players[i];
        if(!p || p->state != CS_ALIVE || halovehicleinside(p) || p->o.dist(player1->o) >= best) continue;
        best = p->o.dist(player1->o);
        qaVehicleActor = p;
    }
    const bool ok = qaVehicleActor && halovehicleqaplace(*vehicle, qaVehicleActor, *along, *side);
    conoutf("VEHICLE_QA place actor vehicle=%d ok=%d name=%s inside=%d", *vehicle, ok, qaVehicleActor ? qaVehicleActor->name : "-",
            qaVehicleActor ? halovehicleqainside(qaVehicleActor) : -1);
    fflush(stdout);
});
// Where the player and the last placed actor are: the vehicle each is inside, or -1.
COMMANDF(qa_vehicle_inside, "s", (const char *label)
{
    bool actorAlive = false;
    loopv(players) actorAlive |= players[i] == qaVehicleActor && qaVehicleActor->state == CS_ALIVE;
    conoutf("VEHICLE_QA inside %s player=%d actor=%d actoralive=%d feet=%.2f,%.2f,%.2f", label, halovehicleqainside(player1),
            actorAlive ? halovehicleqainside(qaVehicleActor) : -1, actorAlive, player1->o.x, player1->o.y, player1->o.z - player1->eyeheight);
    fflush(stdout);
});
// Rolls vehicle N forward at `speed` units/s with nobody at the wheel.
COMMANDF(qa_vehicle_push, "if", (int *vehicle, float *speed)
{
    conoutf("VEHICLE_QA push vehicle=%d ok=%d", *vehicle, halovehicleqapush(*vehicle, *speed));
    fflush(stdout);
});
// Samples, every frame for `ms`, whether the player's body is inside any vehicle.
static int qaWatchUntil = 0, qaWatchSamples = 0, qaWatchInside = 0;
COMMANDF(qa_vehicle_watch_step, "", ()
{
    if(!qaWatchUntil) return;
    ++qaWatchSamples;
    qaWatchInside += halovehicleqainside(player1) >= 0;
    if(lastmillis >= qaWatchUntil)
    {
        conoutf("VEHICLE_QA watch samples=%d inside=%d", qaWatchSamples, qaWatchInside);
        fflush(stdout);
        qaWatchUntil = 0;
        return;
    }
    addsleep(5, "qa_vehicle_watch_step", true);
});
COMMANDF(qa_vehicle_watch, "i", (int *ms)
{
    qaWatchUntil = lastmillis + max(1, *ms);
    qaWatchSamples = qaWatchInside = 0;
    execute("qa_vehicle_watch_step");
});
// Jumps (tests/test_vehicle_jump.py): the player's vehicle put down at X Y Z (its floor point)
// heading YAW at SPEED units/s, full throttle straight ahead for MS, and its motion logged every
// 50 ms (`VEHICLE_QA jump t=... pos=... vel=... grounded=... flipped=... gravity=... tangent=...`).
static int qaJumpStarted = 0, qaJumpUntil = 0;
// (A function, not the command's body: COMMANDF's body may hold no bare commas.)
static void qaVehicleJumpStep()
{
    if(!qaJumpUntil) return;
    vec pos, vel;
    bool grounded = false, flipped = false;
    float gravity = 0, tangent = 0;
    if(halovehicleqamotion(pos, vel, grounded, flipped, gravity, tangent))
        conoutf("VEHICLE_QA jump t=%d pos=%.2f,%.2f,%.2f vel=%.2f,%.2f,%.2f grounded=%d flipped=%d gravity=%.2f tangent=%.2f",
                lastmillis - qaJumpStarted, pos.x, pos.y, pos.z, vel.x, vel.y, vel.z, grounded, flipped, gravity, tangent);
    else conoutf("VEHICLE_QA jump t=%d seated=0", lastmillis - qaJumpStarted);
    fflush(stdout);
    if(lastmillis >= qaJumpUntil) { qaJumpUntil = 0; return; }
    addsleep(50, "qa_vehicle_jump_step", true);
}
COMMANDF(qa_vehicle_jump_step, "", () { qaVehicleJumpStep(); });
// The planned drive's cruising speed for section MAP (`*` any; tests/test_retail_collision.py,
// tests/test_maw_drive.py and tests/test_vehicle_jump.py read it from the section's metrics,
// `vehicle_route.drive.speed`): QA throttle input stops at SPEED units/s there. SPEED 0 removes it.
COMMANDF(qa_vehicle_cruise, "sf", (const char *map, float *speed)
{
    halovehicleqacruise(map, *speed);
    conoutf("VEHICLE_QA cruise map=%s speed=%.2f", map, *speed);
    fflush(stdout);
});
// The section's jump governors (halo_vehiclegovern lines of its map .cfg; tests/test_vehicle_jump.py):
// `VEHICLE_QA governors count=N enabled=0|1`, then `VEHICLE_QA governor at=X,Y,Z radius=R speed=S`.
static void qaVehicleGovernors()
{
    extern int halo_vehiclegovernor;
    const int count = halovehicleqagovernors();
    conoutf("VEHICLE_QA governors count=%d enabled=%d", count, halo_vehiclegovernor);
    loopi(count)
    {
        vec at;
        float radius = 0, speed = 0;
        if(halovehicleqagovernor(i, at, radius, speed))
            conoutf("VEHICLE_QA governor at=%.2f,%.2f,%.2f radius=%.2f speed=%.2f", at.x, at.y, at.z, radius, speed);
    }
    fflush(stdout);
}
COMMANDF(qa_vehicle_governors, "", () { qaVehicleGovernors(); });
COMMANDF(qa_vehicle_jump, "ffffffi", (float *x, float *y, float *z, float *yaw, float *speed, int *ms)
{
    const bool ok = halovehicleqajump(vec(*x, *y, *z), *yaw, *speed);
    conoutf("VEHICLE_QA jumpstart ok=%d", ok);
    fflush(stdout);
    if(!ok) return;
    halovehicleqainput(1, 0, max(1, *ms));
    qaJumpStarted = lastmillis;
    qaJumpUntil = lastmillis + max(1, *ms);
    execute("qa_vehicle_jump_step");
});
// Stuck vehicles: `fall` drops vehicle N 20 units below the section's mesh, `sink` sinks it half
// its height into the floor (the seated player goes with it).
COMMANDF(qa_vehicle_stuck, "is", (int *vehicle, const char *kind)
{
    conoutf("VEHICLE_QA stuck vehicle=%d kind=%s ok=%d", *vehicle, kind, halovehicleqastuck(*vehicle, kind));
    fflush(stdout);
});
// The player at the wheel of vehicle N, turned over and thrown out onto it with no room.
COMMANDF(qa_vehicle_throw, "i", (int *vehicle)
{
    conoutf("VEHICLE_QA throw vehicle=%d ok=%d", *vehicle, halovehicleqathrow(*vehicle));
    fflush(stdout);
});

// Enemy and flying vehicles (tests/test_enemy_vehicles.py). qa_vehicle_flight holds a piloted
// Banshee's throttle, strafe and climb (-1..1) for `ms` (the camera still aims the nose:
// qa_vehicle_look sets it); qa_vehicle_pilot puts a new actor of KIND (0 Grunt, 1 Elite, ...,
// halo.cpp ActorKind) on TEAM (0 Covenant) at the controls of vehicle N (its driver's seat, or
// SEAT by name), or with KIND -1 the nearest living actor not in a seat; qa_vehicle_secondary
// holds or releases the secondary trigger.
COMMANDF(qa_vehicle_flight, "fffi", (float *throttle, float *strafe, float *climb, int *ms) { halovehicleqaflight(*throttle, *strafe, *climb, *ms); });
COMMANDF(qa_vehicle_look, "ff", (float *yaw, float *pitch)
{
    player1->yaw = *yaw;
    player1->pitch = clamp(*pitch, -89.0f, 89.0f);
});
COMMANDF(qa_vehicle_secondary, "i", (int *on) { halovehiclesecondary(*on != 0); });
COMMANDF(qa_vehicle_pilot, "iiis", (int *vehicle, int *kind, int *team, const char *seat)
{
    playerent *unit = NULL;
    if(*kind >= 0)
    {
        vec feet;
        if(halovehicleqaentry(*vehicle, HALO_SEAT_DRIVER, feet) || halovehicleqaentry(*vehicle, HALO_SEAT_GUNNER, feet))
        {
            const ActorKind k = ActorKind(clamp(*kind, 0, int(ACTOR_COUNT) - 1));
            unit = spawnEnemy(k, vec(feet).add(vec(0, 0, profiles[k].eyeheight + .5f)), player1->yaw + 180, -1, -1, *team);
        }
    }
    else
    {
        float best = 1e9f;
        loopv(players)
        {
            playerent *p = players[i];
            if(!p || p->state != CS_ALIVE || halovehicleinside(p) || p->o.dist(player1->o) >= best) continue;
            best = p->o.dist(player1->o);
            unit = p;
        }
    }
    const bool ok = unit && halovehicleqapilot(*vehicle, unit, seat);
    if(ok) haloactorsetflag(unit, HALO_AI_MAGIC_SIGHT, true);
    conoutf("VEHICLE_QA pilot vehicle=%d ok=%d name=%s armed=%d", *vehicle, ok, unit ? unit->name : "-", ok && halovehiclearmedseat(unit));
    fflush(stdout);
});
// Dropships and Pelicans (halo_script_ai.inc): CARGO_QA lines, boarding with E (the hook calls
// what E does on foot) and the player put beside a script vehicle's first rider seat.
COMMANDF(qa_cargo_state, "s", (const char *label) { haloscenariocargodump(label); });
COMMANDF(qa_cargo_board, "", ()
{
    const char *prompt = haloscenarioboardprompt();
    const bool ok = haloscenarioboard();
    conoutf("CARGO_QA board prompt=%s ok=%d boarded=%d driving=%d", prompt ? prompt : "-", ok, haloscenarioboarded(), halodriving());
    fflush(stdout);
});
COMMANDF(qa_cargo_approach, "s", (const char *object)
{
    vec feet;
    const bool ok = haloscenarioqaboardpoint(object, feet);
    if(ok)
    {
        if(halodriving()) execute("halo_use");
        player1->o = vec(feet).add(vec(1.5f * haloscenarioscale(), 0, player1->eyeheight + .2f));
        player1->vel = vec(0, 0, 0);
        player1->resetinterp();
        haloplayermoved("QA teleport");
    }
    const char *prompt = haloscenarioboardprompt();
    conoutf("CARGO_QA approach object=%s ok=%d prompt=%s", object, ok, prompt ? prompt : "-");
    fflush(stdout);
});
// The flown sections' Banshee (halo.cpp flownSection): whether the engine keeps one.
COMMANDF(qa_vehicle_flown, "", ()
{
    conoutf("VEHICLE_QA flown mission=%d section=%d flown=%d", mission + 1, section + 1, flownSection());
    fflush(stdout);
});

// Vehicle damage (tests/test_vehicle_damage.py). qa_vehicle_damage deals AMOUNT points to vehicle N
// as the player would (no jpt! material modifier; EXPLOSIVE 1 as a blast), logging what it took and
// its VEHICLE_HEALTH row; qa_vehicle_aim turns the player to face vehicle N's body box (to fire the
// player's own weapon at it); qa_vehicle_protect 1 keeps every vehicle from harm (halo_vehicledamage 0).
COMMANDF(qa_vehicle_damage, "ifi", (int *vehicle, float *amount, int *explosive)
{
    const float taken = halovehicleqadamage(*vehicle, *amount, *explosive != 0);
    conoutf("VEHICLE_QA damage vehicle=%d amount=%.2f explosive=%d taken=%.3f", *vehicle, *amount, *explosive, taken);
    halovehicledump("damage");
});
COMMANDF(qa_vehicle_aim, "i", (int *vehicle)
{
    vec centre;
    const bool ok = halovehiclecentre(*vehicle, centre);
    if(ok)
    {
        const vec delta = vec(centre).sub(player1->o);
        float yaw = atan2f(delta.x, -delta.y) / RAD;
        if(yaw < 0) yaw += 360;
        player1->yaw = yaw;
        player1->pitch = clamp(atan2f(delta.z, max(.01f, delta.magnitudexy())) / RAD, -89.0f, 89.0f);
    }
    conoutf("VEHICLE_QA aim vehicle=%d ok=%d distance=%.2f", *vehicle, ok, ok ? centre.dist(player1->o) : -1.0f);
    fflush(stdout);
});
COMMANDF(qa_vehicle_protect, "i", (int *on)
{
    extern int halo_vehicledamage;
    halo_vehicledamage = *on ? 0 : 1;
    conoutf("VEHICLE_QA protect=%d", halo_vehicledamage ? 0 : 1);
    fflush(stdout);
});

// Covered riders, tilted hit boxes, death explosions and wrecks (tests/test_vehicle_damage.py items
// 5 to 8; batch 5's critic, gaps 8 and 11).
// qa_vehicle_blast N TYPE SIDE: a grenade of TYPE (0 frag, 1 plasma), the player's, goes off SIDE
// units out past vehicle N's right side at its box's middle height, at once. Logs
//   VEHICLE_QA blast vehicle=<n> ok=<0|1> pilot=<name|-> covered=<0|1> before=<health>,<shield>
//              after=<health>,<shield> alive=<0|1>
// (the driver's health in points and shield fraction), then the VEHICLE_HEALTH rows ("blast").
static void qaVehicleBlast(int vehicle, int type, float side)
{
    playerent *pilot = halovehicleqadriver(vehicle);
    const bool covered = pilot && halovehicleridercovered(pilot);
    const int health = pilot ? pilot->health : -1;
    const float shield = pilot ? haloenemyshieldfraction(pilot) : -1;
    vec at;
    const bool ok = halovehicleqabeside(vehicle, side, at) && haloqagrenadeat(type, at, player1);
    const bool alive = pilot && pilot->state == CS_ALIVE;
    conoutf("VEHICLE_QA blast vehicle=%d ok=%d pilot=%s covered=%d before=%d,%.3f after=%d,%.3f alive=%d", vehicle, ok,
            pilot ? pilot->name : "-", covered, health, shield, alive ? pilot->health : 0, alive ? haloenemyshieldfraction(pilot) : 0.0f, alive);
    halovehicledump("blast");
}
COMMANDF(qa_vehicle_blast, "iif", (int *vehicle, int *type, float *side) { qaVehicleBlast(*vehicle, *type, *side); });

// qa_vehicle_tilt N PITCH ROLL: vehicle N held at that pitch and roll (degrees; asleep, so the
// ground does not level it). qa_vehicle_probe N ALONG UP: a level segment across vehicle N from side
// to side, ALONG units forward of its floor point and UP above it:
//   VEHICLE_QA probe vehicle=<n> ok=<0|1> along=<a> up=<u> hit=<0|1> fraction=<f> back=<b> front=<f>
//              halfwidth=<w> top=<t>
// (the level box's extent from the floor point, AssaultCube units).
COMMANDF(qa_vehicle_tilt, "iff", (int *vehicle, float *pitch, float *roll)
{
    conoutf("VEHICLE_QA tilt vehicle=%d ok=%d pitch=%.1f roll=%.1f", *vehicle, halovehicleqatilt(*vehicle, *pitch, *roll), *pitch, *roll);
    fflush(stdout);
});
static void qaVehicleProbe(int vehicle, float along, float up)
{
    float fraction = -1, box[4] = {0, 0, 0, 0};
    const bool ok = halovehicleqaprobe(vehicle, along, up, fraction, box);
    conoutf("VEHICLE_QA probe vehicle=%d ok=%d along=%.3f up=%.3f hit=%d fraction=%.3f back=%.3f front=%.3f halfwidth=%.3f top=%.3f",
            vehicle, ok, along, up, ok && fraction >= 0, fraction, box[0], box[1], box[2], box[3]);
    fflush(stdout);
}
COMMANDF(qa_vehicle_probe, "iff", (int *vehicle, float *along, float *up) { qaVehicleProbe(*vehicle, *along, *up); });

// qa_vehicle_wrecks N: how many wrecks the section holds (halo_vehiclewreck) and whether vehicle
// N's wreck stops the player's body put at its box's middle (halovehiclecollide; the player is put
// back at once): VEHICLE_QA wrecks count=<n> vehicle=<n> wreck=<0|1> blocked=<0|1|-1>.
static void qaVehicleWrecks(int vehicle)
{
    vec centre;
    int blocked = -1;
    const bool wreck = halovehicleqawreckat(vehicle, centre);
    if(wreck)
    {
        const vec was = player1->o;
        player1->o = vec(centre.x, centre.y, centre.z + player1->eyeheight * .5f);
        float hi = 1e9f, lo = -1e9f;
        blocked = halovehiclecollide(player1, hi, lo) ? 1 : 0;
        player1->o = was;
    }
    conoutf("VEHICLE_QA wrecks count=%d vehicle=%d wreck=%d blocked=%d", halovehicleqawrecks(), vehicle, wreck, blocked);
    fflush(stdout);
}
COMMANDF(qa_vehicle_wrecks, "i", (int *vehicle) { qaVehicleWrecks(*vehicle); });
