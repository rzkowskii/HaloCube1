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
