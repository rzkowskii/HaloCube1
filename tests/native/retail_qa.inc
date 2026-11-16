// Retail-collision walk harness for any section (tests/test_retail_collision.py).
// Compiled into halo.cpp only when HALO_QA is defined, after b30_qa.inc.
//
// qa_retail_start <mission> <section> loads the section as a real campaign start, audits
// its spawn and markers against the retail collision surfaces, then walks the importer's
// route (HALOQA_RETAIL_ROUTE, x y z feet rows) with ordinary movement input until the
// section completes. Enemies in the contact stages are removed and the player cannot be
// hurt, so only movement and the exit are measured.
#include "halo_collision.h"

extern int halo_vehicledamage;   // halo_vehicle.cpp: 0 keeps every vehicle from harm

namespace
{
    vector<vec> qaRetailRoute;
    std::string qaRetailMap;
    std::vector<std::string> qaRetailChain; // route files of the sections still to walk
    bool qaRetailAwaitNext = false;          // left the section through its exit scene
    int qaRetailNudged = 0;
    vector<bool> qaRetailCrouch; // route steps only passable crouching
    vector<bool> qaRetailJump;   // route steps reached by jumping (a ledge, an obstacle, a gap)
    int qaRetailMission = 0, qaRetailSection = 0, qaRetailNext = 0, qaRetailStarted = 0;
    int qaRetailProgress = 0, qaRetailLastJump = 0, qaRetailStable = 0, qaRetailLive = 0;
    float qaRetailClosest = 1e9f;
    int qaRetailWaited = 0, qaRetailWaitLimit = 150000;
    bool qaRetailWalking = false;
    int qaRetailStuckAt = 0, qaRetailCrouched = 0, qaRetailJumps = 0;
    int qaRetailRetreat = -1, qaRetailHoldUntil = 0; // stepping back from a closed device
    int qaRetailCrouchUntil = -1; // crouching under a part-open device up to this waypoint
    vector<vec> qaRetailDetour;   // feet points through the gap of a part-open door
    int qaRetailDetourStart = 0, qaRetailWaitAt = -1;
    int qaRetailSeatedSince = 0;  // held in a scripted seat since (a10's cryotube during the tutorial)

    // Lifts and light bridges (route flags 16, 32, 64 and 128, with the device's name and position
    // in a fifth column, name@position; tools/import_halo_campaign.py LIFT_RIDES and LightBridges).
    // At a lift's deck row the walker stops on the landing until the deck is there (calling it
    // with the control in reach if no script brings it, flag 32), walks onto the deck, uses the
    // control beside it if the deck has one (flag 64) and rides: to the end of the section, or,
    // when more rows follow (b40's c1 tower), until the lift stops and the next row (flag 4) is
    // walked. Before the first row of a light bridge the walker stops two rows short until the
    // bridge is extended, using its control there first if the row has flag 32.
    vector<int> qaRetailFlags;
    std::vector<std::string> qaRetailDevice;
    vector<float> qaRetailValue;
    enum { RIDE_NONE, RIDE_WAIT, RIDE_BOARD, RIDE_ON };
    int qaRide = RIDE_NONE, qaRideStep = -1, qaRideSince = 0, qaRideUses = 0, qaRideUsed = 0, qaRideStill = 0;
    float qaRideLast = -1;
    int qaBridgeStep = -1, qaBridgeCleared = -1, qaBridgeSince = 0, qaBridgeUses = 0, qaBridgeUsed = 0;
    int qaRetailRecoveries = 0;
    // Action rows (tools/import_halo_campaign.py UseControl, ShootTarget, ThrowTarget and DOOR_WAITS):
    // 256 uses the control in reach until the named device stands at the position after the @, 512
    // shoots the named damage target (aim point in the sixth to eighth columns) until its health is at
    // most the value after the @, 1024 waits at a scripted door until it is open that far, 2048 throws
    // frag grenades from the row's spot at the aim point (sixth to eighth columns, seen from the eye
    // above the row) until the target's health is at most the value after the @. Each follows the
    // step row it stands on; a run of 2048 rows is one target's aims, best first (each may name
    // another of its detectors).
    vector<vec> qaRetailAim;
    int qaAct = -1, qaActDone = -1, qaActSince = 0, qaActUsed = 0, qaActLogged = 0, qaActShots = 0, qaActFire = 0;
    // 2048: frags thrown at this row, when the last went, whether its spot was logged, and the
    // throws at the target over all its rows.
    int qaThrows = 0, qaThrowAt = 0, qaThrowTotal = 0;
    bool qaThrowDumped = false;
    vec qaRetailLastFeet;
    bool qaRetailHaveFeet = false;

    // Vehicle rows (tools/import_halo_campaign.py VEHICLE_ROUTES: the driven and flown sections), with
    // the vehicles.txt type in the fifth column: 4096 drive or fly to the row's point (the vehicle's
    // floor point when driven, a flyer's origin when flown; with 2 as well, a jump's landing), 8192
    // board that type of vehicle from where the row stands (walk to the nearest one's driver seat and
    // press use), 16384 get out at the row (a flyer hovers down over it first). The vehicle walker
    // holds full throttle as the foot walker holds forward, steering at a row ahead.
    enum { QA_ROUTE_VEHICLE = 4096, QA_ROUTE_BOARD = 8192, QA_ROUTE_GET_OUT = 16384,
           QA_ROUTE_VEHICLE_ANY = QA_ROUTE_VEHICLE | QA_ROUTE_BOARD | QA_ROUTE_GET_OUT };
    int qaVehRow = -1, qaVehSince = 0, qaVehUsed = 0, qaVehProgress = 0, qaVehStalled = 0, qaVehReverse = 0, qaVehChanged = 0;
    int qaVehLogged = 0, qaVehBoards = 0, qaVehJumps = 0, qaVehUnflips = 0;
    bool qaVehSeated = false;

    // Movement feel. Once per section the walk stops at the first long straight, flat stretch
    // of the route and runs it twice from a standstill with a fixed heading: on the retail
    // collision, then on the stock AssaultCube geometry of the same map. Each run reports the
    // sustained ground speed, the time to reach 90% of it and a standing jump, so the test can
    // hold retail movement to the stock feel. Straight stretches walked by the route follower
    // report their speed as well.
    struct QaRetailRun
    {
        float speed = 0, accel = -1, apex = 0;
        int air = 0;
        bool done = false, ok = false;
    };
    enum { TRIAL_NONE, TRIAL_RUN, TRIAL_STOP, TRIAL_JUMP, TRIAL_DONE };
    int qaTrialStart = -1, qaTrialLength = 0, qaTrialPhase = TRIAL_NONE, qaTrialPass = 0, qaTrialClock = 0;
    int qaTrialFrom = 0, qaTrialLastMillis = 0, qaTrialAirStart = 0;
    float qaTrialFromDist = 0, qaTrialGround = 0, qaTrialYaw = 0;
    vec qaTrialOrigin, qaTrialDir, qaTrialLast;
    bool qaTrialLeft = false;
    QaRetailRun qaTrialRuns[2];
    int qaWalkStretchFrom = -1, qaWalkStretchEnd = -1, qaWalkStretchMillis = 0, qaWalkStretchJumps = 0;
    bool qaWalkStretchHeld = false;
    vec qaWalkStretchFeet;
    float qaWalkSpeedMin = 1e9f;
    int qaWalkStretches = 0;

    // Prints a section's result. The map is the section walked, which a transition has
    // already replaced by the next one.
    void qaRetailReport(const char *label)
    {
        forward(false);
        jumpn(false);
        const vec feet = vec(player1->o).sub(vec(0, 0, player1->eyeheight));
        if(qaWalkStretches) conoutf("RETAIL_QA walk-speed-min %.2f over %d stretches", qaWalkSpeedMin, qaWalkStretches);
        if(qaRetailCrouched) conoutf("RETAIL_QA crouched %d times", qaRetailCrouched);
        if(qaRetailJumps) conoutf("RETAIL_QA jumped %d times", qaRetailJumps);
        if(qaRetailRecoveries) conoutf("RETAIL_QA recoveries %d", qaRetailRecoveries);
        if(qaVehBoards || qaVehJumps || qaVehUnflips)
            conoutf("RETAIL_QA vehicle boarded %d times, %d jump rows passed, flipped back %d times", qaVehBoards, qaVehJumps, qaVehUnflips);
        player1->trycrouch = false;
        player1->strafe = 0;
        conoutf("RETAIL_QA %s map=%s waypoint=%d/%d feet=%.2f,%.2f,%.2f stage=%d alive=%d",
                label, qaRetailMap.c_str(), qaRetailNext, int(qaRetailRoute.length()), feet.x, feet.y, feet.z,
                stage, player1->state == CS_ALIVE);
        fflush(stdout);
    }

    void qaRetailStop(const char *label)
    {
        qaRetailReport(label);
        execute("halo_script_status");
        haloactordump("retail");
        loopv(enemies) if(enemies[i].bot) conoutf("RETAIL_QA actor %d team=%d alive=%d at %.1f,%.1f,%.1f", i, enemies[i].team, enemies[i].bot->state == CS_ALIVE, enemies[i].bot->o.x, enemies[i].bot->o.y, enemies[i].bot->o.z);
        execute("quit");
    }

    // Distance from the feet down to retail floor, or -1 when there is none within `reach`.
    float qaRetailFloorGap(const vec &feet, float reach)
    {
        HaloCollisionHit hit;
        if(!halocollisionsweep(feet, vec(0, 0, -reach), player1->radius, player1->eyeheight + player1->aboveeye, hit) ||
           hit.normal.z < .65f) return -1;
        return hit.fraction * reach;
    }

    void qaRetailAudit()
    {
        int checked = 0, bad = 0;
        loopv(ents)
        {
            const entity &e = ents[i];
            if(e.type != PLAYERSTART && !isitem(e.type)) continue;
            // Vehicle placements (102) stand at their retail point, which may hover (Banshees,
            // Ghosts) or sit on a slope; the engine settles them itself (halo_vehicle.cpp).
            if(e.type == PLAYERSTART && e.attr2 == 102) continue;
            // A flown section's route markers follow its planned flight (VEHICLE_ROUTES): in the air.
            if(e.type == PLAYERSTART && e.attr2 == 100 && e.attr3 != 0 && flownSection()) continue;
            // Markers, spawns and items are authored at floor height. Spawns (the player's and
            // the enemy waves') need room for a body; route markers (100), weapon placements
            // (101), equipment placements (103) and items only need floor under a point.
            const vec feet(e.x, e.y, e.z);
            HaloCollisionHit hit;
            const bool body = e.type == PLAYERSTART && e.attr2 != 100 && e.attr2 != 101 && e.attr2 != 103;
            const float radius = body ? player1->radius : .35f;
            ++checked;
            if(!halocollisionsweep(vec(feet).add(vec(0, 0, 1.26f)), vec(0, 0, -4.26f), radius,
                                   player1->eyeheight + player1->aboveeye, hit) || hit.normal.z < .65f)
            {
                ++bad;
                conoutf("RETAIL_QA floorless entity %d type=%d at %d,%d,%d", i, e.type, e.x, e.y, e.z);
            }
        }
        conoutf("RETAIL_QA audit entities=%d floorless=%d", checked, bad);
    }

    // Whether a standing body fits with its feet here: no retail surface and no device in the way.
    bool qaRetailFits(const vec &feet)
    {
        HaloCollisionHit hit;
        if(halocollisionoverlap(vec(feet).add(vec(0, 0, .05f)), player1->radius, player1->eyeheight + player1->aboveeye, hit)) return false;
        const vec saved = player1->o;
        player1->o = vec(feet).add(vec(0, 0, player1->eyeheight + .05f));
        float hi = 127, lo = -128;
        const bool blocked = haloscenariocollide(player1, hi, lo);
        player1->o = saved;
        return !blocked;
    }

    // Length (in route steps) of the straight, flat run starting at route index i.
    int qaRetailStraight(int i)
    {
        if(i + 1 >= qaRetailRoute.length() || (qaRetailFlags[i] & QA_ROUTE_VEHICLE_ANY) || (qaRetailFlags[i + 1] & QA_ROUTE_VEHICLE_ANY)) return 0;
        const vec step = vec(qaRetailRoute[i + 1]).sub(qaRetailRoute[i]);
        if(fabsf(step.z) > .01f || fabsf(step.magnitudexy() - 1) > .01f || qaRetailCrouch[i] || qaRetailCrouch[i + 1]) return 0;
        int j = i + 1;
        while(j + 1 < qaRetailRoute.length())
        {
            const vec next = vec(qaRetailRoute[j + 1]).sub(qaRetailRoute[j]);
            if(next.dist(step) > .01f || fabsf(qaRetailRoute[j + 1].z - qaRetailRoute[i].z) > .01f || qaRetailCrouch[j + 1] ||
               (qaRetailFlags[j + 1] & QA_ROUTE_VEHICLE_ANY)) break;
            ++j;
        }
        return j - i;
    }

    // The trial needs room for the body on both sides of the straight, on retail floor, so a
    // fixed heading cannot brush a wall. Stock geometry is checked by the run itself.
    bool qaTrialClear(int start, int length)
    {
        const vec dir = vec(qaRetailRoute[start + 1]).sub(qaRetailRoute[start]), side(-dir.y, dir.x, 0);
        HaloCollisionHit hit;
        for(int k = 0; k <= length; ++k)
        {
            const vec feet = vec(qaRetailRoute[start + k]).add(vec(0, 0, .05f));
            for(float offset : {-.25f, .25f})
            {
                const vec at = vec(feet).add(vec(side).mul(offset));
                if(halocollisionoverlap(vec(at).add(vec(0, 0, .3f)), player1->radius, player1->eyeheight + player1->aboveeye, hit)) return false;
            }
        }
        return true;
    }

    void qaTrialPlace()
    {
        const vec feet = qaRetailRoute[qaTrialStart];
        player1->o = vec(feet.x, feet.y, feet.z + player1->eyeheight + .05f);
        player1->vel = vec();
        player1->yaw = qaTrialYaw;
        player1->pitch = 0;
        player1->resetinterp();
        haloplayermoved("QA teleport");
        forward(false);
        jumpn(false);
        player1->trycrouch = false;
        player1->strafe = 0;
        qaTrialOrigin = feet;
        qaTrialLast = feet;
        qaTrialLastMillis = lastmillis;
        qaTrialClock = lastmillis;
        qaTrialFrom = 0;
        qaTrialLeft = false;
        qaTrialPhase = TRIAL_STOP;  // settle on the floor first, then run
        qaTrialRuns[qaTrialPass] = QaRetailRun();
    }

    void qaTrialBegin(int start, int length)
    {
        qaTrialStart = start;
        // 30 steps: well past the acceleration, and short enough to stay clear of doors.
        qaTrialLength = min(length, 30);
        qaTrialDir = vec(qaRetailRoute[start + 1]).sub(qaRetailRoute[start]);
        qaTrialDir.z = 0;
        qaTrialDir.normalize();
        qaTrialYaw = fmodf(atan2f(qaTrialDir.y, qaTrialDir.x) / RAD + 90.0f + 360.0f, 360.0f);
        qaTrialPass = 0;
        qaWalkStretchFrom = -1;
        conoutf("RETAIL_QA trial straight=%d..%d from=%.0f,%.0f,%.2f", start, start + qaTrialLength,
                qaRetailRoute[start].x, qaRetailRoute[start].y, qaRetailRoute[start].z);
        qaTrialPlace();
    }

    void qaTrialFinish(const char *why)
    {
        QaRetailRun &run = qaTrialRuns[qaTrialPass];
        run.done = true;
        forward(false);
        jumpn(false);
        conoutf("RETAIL_QA trial %s %s speed=%.2f accel=%d apex=%.2f air=%d",
                qaTrialPass ? "stock" : "retail", why, run.speed, int(run.accel), run.apex, run.air);
        if(qaTrialPass == 0)
        {
            // The same straight on the stock cube geometry of this map.
            halocollisionmap(nullptr);
            qaTrialPass = 1;
            qaTrialPlace();
            return;
        }
        halocollisionmap(getclientmap());
        const QaRetailRun &retail = qaTrialRuns[0], &stock = qaTrialRuns[1];
        conoutf("RETAIL_QA feel retail_speed=%.2f stock_speed=%.2f retail_accel=%d stock_accel=%d "
                "retail_apex=%.2f stock_apex=%.2f retail_air=%d stock_air=%d retail_ok=%d stock_ok=%d",
                retail.speed, stock.speed, int(retail.accel), int(stock.accel), retail.apex, stock.apex,
                retail.air, stock.air, retail.ok, stock.ok);
        fflush(stdout);
        // Resume the walk at the end of the straight, back on retail collision.
        const int resume = min(qaTrialStart + qaTrialLength, int(qaRetailRoute.length()) - 1);
        const vec feet = qaRetailRoute[resume];
        player1->o = vec(feet.x, feet.y, feet.z + player1->eyeheight + .05f);
        player1->vel = vec();
        player1->resetinterp();
        haloplayermoved("QA teleport");
        qaRetailNext = resume;
        qaRetailClosest = 1e9f;
        qaRetailProgress = lastmillis;
        qaRetailHaveFeet = false;
        qaTrialPhase = TRIAL_DONE;
    }

    // One tick of the trial: settle, run the straight, settle, jump.
    void qaTrialStep()
    {
        QaRetailRun &run = qaTrialRuns[qaTrialPass];
        const vec feet = vec(player1->o).sub(vec(0, 0, player1->eyeheight));
        const float along = vec(feet).sub(qaTrialOrigin).dot(qaTrialDir);
        const int elapsed = lastmillis - qaTrialClock;
        player1->yaw = qaTrialYaw;
        player1->pitch = 0;
        if(qaTrialPhase == TRIAL_STOP)
        {
            // Stand still on the floor for a moment before each run or jump.
            if(elapsed > 2500) { qaTrialFinish("never-settled"); return; }
            if(!player1->onfloor || player1->vel.magnitude() > .02f || elapsed < 300) return;
            if(!run.ok && run.speed == 0 && qaTrialFrom == 0)
            {
                qaTrialPhase = TRIAL_RUN;
                qaTrialOrigin = feet;
                qaTrialClock = lastmillis;
                qaTrialLast = feet;
                qaTrialLastMillis = lastmillis;
                forward(true);
                return;
            }
            qaTrialPhase = TRIAL_JUMP;
            qaTrialGround = feet.z;
            qaTrialClock = lastmillis;
            qaTrialAirStart = 0;
            qaTrialLeft = false;
            jumpn(true);
            return;
        }
        if(qaTrialPhase == TRIAL_RUN)
        {
            // Acceleration: the time to cover the first 4 units from a standstill, interpolated
            // between ticks so frame timing on a loaded machine does not quantise it.
            const float before = vec(qaTrialLast).sub(qaTrialOrigin).dot(qaTrialDir);
            if(run.accel < 0 && along >= 4.0f && along > before)
                run.accel = qaTrialLastMillis - qaTrialClock + (4.0f - before) / (along - before) * (lastmillis - qaTrialLastMillis);
            qaTrialLast = feet;
            qaTrialLastMillis = lastmillis;
            // Sustained speed between 6 and length-4 cells, well past the acceleration.
            if(!qaTrialFrom && along >= 6.0f)
            {
                qaTrialFrom = lastmillis;
                qaTrialFromDist = along;
            }
            if(qaTrialFrom && along >= qaTrialLength - 4.0f)
            {
                const int span = lastmillis - qaTrialFrom;
                run.speed = span > 0 ? (along - qaTrialFromDist) * 1000.0f / span : 0;
                run.ok = true;
                forward(false);
                qaTrialPhase = TRIAL_STOP;
                qaTrialClock = lastmillis;
                return;
            }
            if(elapsed > int(qaTrialLength * 1000 / 16.0f) + 2000)
            {
                run.speed = along * 1000.0f / max(elapsed, 1);
                qaTrialFinish("stalled");
            }
            return;
        }
        if(qaTrialPhase == TRIAL_JUMP)
        {
            if(lastmillis - qaTrialClock > 150) jumpn(false);
            run.apex = max(run.apex, feet.z - qaTrialGround);
            if(!player1->onfloor && !qaTrialLeft) { qaTrialLeft = true; qaTrialAirStart = lastmillis; }
            if(qaTrialLeft && player1->onfloor)
            {
                run.air = lastmillis - qaTrialAirStart;
                qaTrialFinish("ok");
                return;
            }
            if(lastmillis - qaTrialClock > 3000) qaTrialFinish("jump-timeout");
        }
    }

    // Route-follower speed on straight stretches of 16 or more steps (from step 7 to length-2).
    void qaWalkStretch(const vec &feet)
    {
        if(qaWalkStretchFrom >= 0)
        {
            if(player1->jumpnext || !player1->onfloor) ++qaWalkStretchJumps;
            // A door or another body just ahead (an automatic door still opening as the player
            // reaches it, a crewman in the corridor) holds a player up whatever the collision.
            if(!qaWalkStretchHeld && qaRetailNext + 1 < qaRetailRoute.length())
            {
                const vec dir = vec(qaRetailRoute[qaRetailNext + 1]).sub(qaRetailRoute[qaRetailNext]).normalize();
                const vec saved = player1->o;
                player1->o.add(vec(dir).mul(1.5f));
                float hi = 127, lo = -128;
                qaWalkStretchHeld = haloscenariocollide(player1, hi, lo);
                player1->o = saved;
                loopv(players) if(players[i] && players[i] != player1 && players[i]->state == CS_ALIVE &&
                                  players[i]->o.dist(player1->o) < 2 * player1->radius + 2.0f) qaWalkStretchHeld = true;
            }
            if(qaRetailNext >= qaWalkStretchEnd)
            {
                const int span = lastmillis - qaWalkStretchMillis;
                const float distance = vec(feet).sub(qaWalkStretchFeet).magnitudexy();
                if(span > 0 && !qaWalkStretchJumps)
                {
                    const float speed = distance * 1000.0f / span;
                    conoutf("RETAIL_QA walk-speed waypoints=%d..%d speed=%.2f held=%d", qaWalkStretchFrom, qaWalkStretchEnd, speed, qaWalkStretchHeld);
                    if(!qaWalkStretchHeld)
                    {
                        qaWalkSpeedMin = min(qaWalkSpeedMin, speed);
                        ++qaWalkStretches;
                    }
                }
                qaWalkStretchFrom = -1;
            }
            return;
        }
        // Start measuring seven steps into a straight stretch, once the follower is lined up
        // and already up to speed (not just set down after the feel trial or a detour).
        if(player1->vel.magnitudexy() < .9f) return;
        for(int back = 7; back <= 10; ++back)
        {
            const int start = qaRetailNext - back;
            if(start < 0) break;
            const int length = qaRetailStraight(start);
            if(length < 16 || start + length - 2 <= qaRetailNext) continue;
            qaWalkStretchFrom = qaRetailNext;
            qaWalkStretchEnd = start + length - 2;
            qaWalkStretchMillis = lastmillis;
            qaWalkStretchFeet = feet;
            qaWalkStretchJumps = 0;
            qaWalkStretchHeld = false;
            return;
        }
    }

    bool qaRetailLoad(const std::string &file)
    {
        qaRetailRoute.setsize(0);
        qaRetailAim.setsize(0);
        qaRetailCrouch.setsize(0);
        qaRetailJump.setsize(0);
        qaRetailFlags.setsize(0);
        qaRetailValue.setsize(0);
        qaRetailDevice.clear();
        std::ifstream input(file);
        std::string line;
        while(std::getline(input, line))
        {
            // x y z [flags [device@position [ax ay az]]]: 1 crouch, 2 jump, 16 lift deck, 128 light
            // bridge, 256 use a control, 512 shoot a target at the aim point, 1024 wait at a door,
            // 2048 throw frags at the aim point
            vec point, aim(0, 0, 0);
            int flags = 0;
            char device[96] = "";
            const int fields = sscanf(line.c_str(), "%f %f %f %d %95s %f %f %f", &point.x, &point.y, &point.z, &flags, device,
                                      &aim.x, &aim.y, &aim.z);
            if(fields < 3)
            {
                if(line.find_first_not_of(" \t\r") != std::string::npos) return false;
                continue;
            }
            if(fields < 4) flags = 0;
            // sscanf takes "nan" and "inf": a damaged row would aim the walker at nothing.
            if(!std::isfinite(point.x) || !std::isfinite(point.y) || !std::isfinite(point.z) ||
               !std::isfinite(aim.x) || !std::isfinite(aim.y) || !std::isfinite(aim.z) || flags < 0) return false;
            float value = 0;
            if(char *at = strrchr(device, '@')) { value = float(atof(at + 1)); *at = 0; }
            if((flags & (16 | 128 | 256 | 512 | 1024 | 2048 | QA_ROUTE_VEHICLE_ANY)) && !*device) return false;
            if((flags & (512 | 2048)) && fields < 8) return false;
            qaRetailRoute.add(point);
            qaRetailAim.add(aim);
            qaRetailCrouch.add((flags & 1) != 0);
            qaRetailJump.add((flags & 2) != 0);
            qaRetailFlags.add(flags);
            qaRetailDevice.push_back(device);
            qaRetailValue.add(value);
        }
        return !qaRetailRoute.empty();
    }

    float qaDevicePosition(const std::string &name)
    {
        const int id = haloscenarioobject(name.c_str());
        return id < 0 ? -1.0f : float(haloscenariocall("device_get_position", id, 0));
    }

    float qaRetailHeading(const vec &to) { return fmodf(atan2f(to.y, to.x) / RAD + 90.0f + 360.0f, 360.0f); }

    void qaRetailHold()
    {
        forward(false);
        jumpn(false);
        player1->strafe = 0;
        player1->trycrouch = false;
        qaRetailProgress = lastmillis;
    }

    // The first route row from the follower's waypoint on that it must not count as reached by
    // walking near it: a lift deck, or the first row of a light bridge that is not yet extended.
    int qaRetailHoldRow()
    {
        for(int i = qaRetailNext; i < qaRetailRoute.length() && i <= qaRetailNext + 10; ++i)
        {
            if(qaRetailFlags[i] & (16 | QA_ROUTE_VEHICLE_ANY)) return i;
            if((qaRetailFlags[i] & (256 | 512 | 1024 | 2048)) && i > qaActDone) return i;
            if((qaRetailFlags[i] & 128) && (!i || !(qaRetailFlags[i - 1] & 128)) && i != qaBridgeCleared) return i;
        }
        return INT_MAX;
    }

    // One tick of a lift ride; false when the follower walks on as usual.
    bool qaRetailRide(const vec &feet)
    {
        const int n = qaRetailRoute.length();
        if(qaRide == RIDE_NONE)
        {
            int board = -1;
            for(int i = qaRetailNext; i < n && i <= qaRetailNext + 8 && board < 0; ++i) if(qaRetailFlags[i] & 16) board = i;
            if(board < 0) return false;
            // Wait on the landing: the row before the deck.
            if(board != qaRetailNext && vec(qaRetailRoute[max(board - 1, 0)]).sub(feet).magnitudexy() > 1.5f) return false;
            qaRide = RIDE_WAIT;
            qaRideStep = board;
            qaRideSince = qaRideStill = lastmillis;
            qaRideUses = qaRideUsed = 0;
            qaRideLast = qaDevicePosition(qaRetailDevice[board]);
            conoutf("RETAIL_QA ride waiting for %s at waypoint %d: position %.2f, boards at %.2f", qaRetailDevice[board].c_str(),
                    board, qaRideLast, qaRetailValue[board]);
        }
        const int board = qaRideStep;
        const char *lift = qaRetailDevice[board].c_str();
        const float position = qaDevicePosition(qaRetailDevice[board]), target = qaRetailValue[board];
        if(fabsf(position - qaRideLast) > .001f) { qaRideLast = position; qaRideStill = lastmillis; }
        const bool still = lastmillis - qaRideStill > 500, there = fabsf(position - target) < .01f;
        auto use = [&](const char *why)
        {
            execute("halo_use");
            ++qaRideUses;
            qaRideUsed = lastmillis;
            conoutf("RETAIL_QA ride uses the control to %s %s (position %.2f)", why, lift, position);
        };
        if(qaRide == RIDE_WAIT)
        {
            qaRetailHold();
            if(there && still)
            {
                qaRide = RIDE_BOARD;
                qaRideSince = lastmillis;
                conoutf("RETAIL_QA ride boarding %s", lift);
            }
            // Call it from the landing (flag 32) once it has stood still somewhere else for 3 s.
            else if((qaRetailFlags[board] & 32) && still && lastmillis - qaRideStill > 3000 && qaRideUses < 3 &&
                    (!qaRideUsed || lastmillis - qaRideUsed > 15000)) use("call");
            if(lastmillis - qaRideSince > qaRetailWaitLimit) qaRetailStop("FAIL ride-never-arrived");
            return true;
        }
        if(qaRide == RIDE_BOARD)
        {
            const vec to = vec(qaRetailRoute[board]).sub(feet);
            qaRetailProgress = lastmillis;
            player1->strafe = 0;
            jumpn(false);
            if(to.magnitudexy() > .5f)
            {
                player1->yaw = qaRetailHeading(to);
                player1->pitch = 0;
                forward(true);
                if(lastmillis - qaRideSince > 10000) qaRetailStop("FAIL ride-could-not-board");
                return true;
            }
            forward(false);
            qaRide = RIDE_ON;
            qaRideSince = qaRideStill = lastmillis;
            conoutf("RETAIL_QA ride on %s at %.2f,%.2f,%.2f", lift, feet.x, feet.y, feet.z);
            return true;
        }
        // On the deck. A lift with a control on its deck (flag 64) leaves once that is used.
        qaRetailHold();
        if((qaRetailFlags[board] & 64) && there && lastmillis - qaRideSince > 1000 && qaRideUses < 4 &&
           (!qaRideUsed || lastmillis - qaRideUsed > 8000)) use("send");
        if(board + 1 < n && !there && still)
        {
            // It stopped at the other end: walk on from the step after the ride (flag 4).
            conoutf("RETAIL_QA ride done: %s at %.2f, feet %.2f,%.2f,%.2f", lift, position, feet.x, feet.y, feet.z);
            qaRetailNext = board + 1;
            qaRide = RIDE_NONE;
            qaRideStep = -1;
            qaRetailClosest = 1e9f;
            return false;
        }
        if(lastmillis - qaRideSince > 90000) qaRetailStop("FAIL ride-timeout");
        return true;
    }

    // Waiting for a light bridge to extend; false when the follower walks on as usual.
    bool qaRetailBridge(const vec &feet)
    {
        int entry = -1;
        for(int i = qaRetailNext; i < qaRetailRoute.length() && i <= qaRetailNext + 6 && entry < 0; ++i)
            if((qaRetailFlags[i] & 128) && (!i || !(qaRetailFlags[i - 1] & 128))) entry = i;
        if(entry < 0 || entry == qaBridgeCleared) return false;
        const char *bridge = qaRetailDevice[entry].c_str();
        const float position = qaDevicePosition(qaRetailDevice[entry]), target = qaRetailValue[entry];
        if(target > .5f ? position >= target - .02f : position <= target + .02f)
        {
            if(qaBridgeStep == entry) conoutf("RETAIL_QA bridge %s extended after %d ms", bridge, lastmillis - qaBridgeSince);
            qaBridgeStep = -1;
            qaBridgeCleared = entry;
            return false;
        }
        // Not extended: stop two rows short of it, stepping back there after any overshoot.
        if(qaRetailNext < entry - 3 && entry > 3) return false;
        if(qaBridgeStep != entry)
        {
            qaBridgeStep = entry;
            qaBridgeSince = lastmillis;
            qaBridgeUses = qaBridgeUsed = 0;
            conoutf("RETAIL_QA bridge waiting for %s at waypoint %d: position %.2f, extended at %.2f", bridge, entry, position, target);
        }
        const vec back = vec(qaRetailRoute[max(entry - 2, 0)]).sub(feet);
        qaRetailHold();
        if(back.magnitudexy() > .6f)
        {
            player1->yaw = qaRetailHeading(back);
            forward(true);
        }
        else if((qaRetailFlags[entry] & 32) && qaBridgeUses < 3 && (!qaBridgeUsed || lastmillis - qaBridgeUsed > 6000))
        {
            // The control is beside the bridge's end, in reach of the rows before it.
            execute("halo_use");
            ++qaBridgeUses;
            qaBridgeUsed = lastmillis;
            conoutf("RETAIL_QA bridge uses the control of %s", bridge);
        }
        if(lastmillis - qaBridgeSince > qaRetailWaitLimit) qaRetailStop("FAIL bridge-never-extended");
        return true;
    }

    float qaRetailHealth(const std::string &name)
    {
        const int id = haloscenarioobject(name.c_str());
        return id < 0 ? -1.0f : float(haloscenariocall("unit_get_health", id, 0));
    }

    // One tick of an action row (256, 512, 1024, 2048) at the follower's waypoint; false when there is none.
    bool qaRetailAct(const vec &feet)
    {
        const int row = qaRetailNext;
        if(row >= qaRetailRoute.length() || !(qaRetailFlags[row] & (256 | 512 | 1024 | 2048)) || row <= qaActDone) return false;
        const int flags = qaRetailFlags[row];
        const char *what = qaRetailDevice[row].c_str();
        const float value = qaRetailValue[row];
        const char *kind = flags & 256 ? "use" : flags & 512 ? "shoot" : flags & 2048 ? "throw" : "door";
        if(qaAct != row)
        {
            // The first of a run of throw rows starts the count; the later rows (other aims) carry it on.
            if((flags & 2048) && !(row > 0 && (qaRetailFlags[row - 1] & 2048))) qaThrowTotal = 0;
            qaAct = row;
            qaActSince = qaActLogged = lastmillis;
            qaActUsed = qaActShots = qaActFire = 0;
            qaThrows = qaThrowAt = 0;
            qaThrowDumped = false;
            conoutf("RETAIL_QA act %s %s at waypoint %d (position %.2f, health %.2f)", kind, what, row, qaDevicePosition(what), qaRetailHealth(what));
        }
        qaRetailHold();
        auto done = [&](const char *how)
        {
            attack(false);
            conoutf("RETAIL_QA act %s %s %s after %d ms", kind, what, how, lastmillis - qaActSince);
            qaActDone = row;
            qaRetailNext = row + 1;
            qaRetailClosest = 1e9f;
            qaRetailProgress = lastmillis;
            qaWalkStretchFrom = -1;
            return true;
        };
        // Stand on the row first (it repeats the step the follower just reached). A throw is planned
        // from the row's spot exactly: walk onto it in short taps once close, and let the body stop.
        const vec to = vec(qaRetailRoute[row]).sub(feet);
        if((flags & 2048) && !qaThrows && lastmillis - qaActSince < 10000 &&
           (to.magnitudexy() > .15f || vec(player1->vel.x, player1->vel.y, 0).magnitude() > .05f))
        {
            player1->yaw = qaRetailHeading(to);
            player1->pitch = 0;
            forward(to.magnitudexy() > .6f || (to.magnitudexy() > .15f && (lastmillis / 40) % 4 == 0));
            return true;
        }
        if(!(flags & 2048) && to.magnitudexy() > .6f && lastmillis - qaActSince < 6000)
        {
            player1->yaw = qaRetailHeading(to);
            player1->pitch = 0;
            forward(true);
            return true;
        }
        const int elapsed = lastmillis - qaActSince;
        // Covenant, Flood and Sentinels near the spot are fought as the player would (removed), so
        // they neither stop the action nor take the aim assist.
        if(elapsed < 200 || (elapsed / 1000) % 5 == 0)
            loopv(enemies) if(enemies[i].bot && enemies[i].bot->state == CS_ALIVE && !haloteamfriendly(enemies[i].team) &&
                              !haloactorpassive(enemies[i].bot) && enemies[i].bot->o.dist(player1->o) < 30.0f)
                dodamage(INT_MAX, enemies[i].bot, player1, GUN_ASSAULT);
        if(flags & 256)
        {
            const float position = qaDevicePosition(what);
            if(value > .5f ? position >= value - .02f : position <= value + .02f) return done("reached");
            if(!qaActUsed || lastmillis - qaActUsed > 1500)
            {
                execute("halo_use");
                qaActUsed = lastmillis;
                conoutf("RETAIL_QA act use presses use for %s (position %.2f)", what, position);
            }
            // The script may hold the control (a manifold already destroyed): carry on.
            if(elapsed > 20000) return done("timed-out");
            return true;
        }
        if(flags & 512)
        {
            const float health = qaRetailHealth(what);
            if(health >= 0 && health <= value) return done("destroyed");
            // Aim at the point the importer saw the target at, from the eye.
            const vec look = vec(qaRetailAim[row]).sub(player1->o);
            player1->yaw = qaRetailHeading(look);
            player1->pitch = atan2f(look.z, max(look.magnitudexy(), .01f)) / RAD;
            // Short bursts keep the spread down; the magazine is refilled as it runs dry, as a
            // player would reload and pick up ammunition from the many weapons on the floor.
            if(player1->weaponsel && player1->weaponsel->mag <= 0)
            {
                player1->weaponsel->ammo = max(player1->weaponsel->ammo, 120);
                tryreload(player1);
            }
            const bool firing = elapsed > 400 && (lastmillis / 100) % 5 < 2;
            if(firing && !qaActFire) ++qaActShots;
            qaActFire = firing;
            attack(firing);
            if(lastmillis - qaActLogged > 5000)
            {
                qaActLogged = lastmillis;
                bool damageable = false;
                vec centre;
                float probe = -1;
                haloscenariotargetstate(what, centre, probe, damageable);
                conoutf("RETAIL_QA act shoot %s health %.2f damageable %d bursts %d", what, health, damageable, qaActShots);
            }
            if(elapsed > 45000) { attack(false); qaRetailStop("FAIL target-not-destroyed"); }
            return true;
        }
        if(flags & 2048)
        {
            // Frags from the planned spot at the planned aim (from the eye above the row, so the yaw and
            // pitch are the planner's whatever the body's last centimetres). Damage adds up, so throws
            // that only reach the blast's edge still count. Four a row, the second and third a
            // half-degree either side; then the next aim row, and after the target's last row a failure.
            // A run of throw rows is one target's aims; its rows may name either of its detectors (each
            // destroys the manifold, enc5_1_destroyed_poll), so any of them down counts.
            int first = row, last = row;
            while(first > 0 && (qaRetailFlags[first - 1] & 2048)) --first;
            while(last + 1 < qaRetailRoute.length() && (qaRetailFlags[last + 1] & 2048)) ++last;
            for(int k = first; k <= last; ++k)
            {
                const float down = qaRetailHealth(qaRetailDevice[k]);
                if(down >= 0 && down <= qaRetailValue[k]) return done("destroyed");
            }
            const float health = qaRetailHealth(what);
            if(qaThrows && !qaThrowDumped && lastmillis - qaThrowAt > 1400)
            {
                qaThrowDumped = true;
                halogrenadedump("throw");   // where the frag lies just before it goes off
            }
            if(qaThrows && lastmillis - qaThrowAt < 2800) return true;
            if(qaThrows)
            {
                bool damageable = false;
                vec centre;
                float probe = -1;
                haloscenariotargetstate(what, centre, probe, damageable);
                conoutf("RETAIL_QA act throw %s health %.2f damageable %d after throw %d (%d at this target)", what, health,
                        damageable, qaThrows, qaThrowTotal);
            }
            const bool lastRow = row == last;
            if(qaThrows >= 4)
            {
                if(lastRow) { qaRetailStop("FAIL target-not-destroyed"); return true; }
                return done("missed");
            }
            static const float jitter[4] = { 0, .5f, -.5f, 0 };
            const vec eye = vec(qaRetailRoute[row]).add(vec(0, 0, player1->eyeheight));
            const vec look = vec(qaRetailAim[row]).sub(eye);
            player1->yaw = fmodf(qaRetailHeading(look) + jitter[qaThrows & 3] + 360.0f, 360.0f);
            player1->pitch = atan2f(look.z, max(look.magnitudexy(), .01f)) / RAD;
            // The walker carries the frags it needs, as a player would pick them up on the way.
            if(grenadeCount(HALO_GRENADE_FRAG) <= 0) grenadeCount(HALO_GRENADE_FRAG) = 1;
            grenadeType = HALO_GRENADE_FRAG;
            const vec at = vec(player1->o).sub(vec(0, 0, player1->eyeheight));
            const int before = grenadeCount(HALO_GRENADE_FRAG);
            halofrag();
            // Not thrown (a throw or melee still playing, a weapon switch or reload): try next tick,
            // for as long as the shoot rows try (halofrag refusing for good would hang the walk).
            if(grenadeCount(HALO_GRENADE_FRAG) == before)
            {
                if(elapsed > 45000) qaRetailStop("FAIL frag-never-thrown");
                return true;
            }
            ++qaThrows;
            ++qaThrowTotal;
            qaThrowAt = lastmillis;
            qaThrowDumped = false;
            conoutf("RETAIL_QA act throw %s at waypoint %d: throw %d from %.2f,%.2f,%.2f yaw %.1f pitch %.1f", what, row, qaThrows,
                    at.x, at.y, at.z, player1->yaw, player1->pitch);
            return true;
        }
        // 1024: a scripted door. The follower stands at it, inside its opening radius.
        const float position = qaDevicePosition(what);
        if(position >= value) return done("opened");
        if(elapsed > 3000) player1->yaw = fmodf(player1->yaw + 1.5f, 360.0f);   // tutorial scripts wait for a look
        if(!qaActUsed || lastmillis - qaActUsed > 6000)
        {
            execute("halo_use");
            qaActUsed = lastmillis;
        }
        if(lastmillis - qaActLogged > 20000)
        {
            qaActLogged = lastmillis;
            const int id = haloscenarioobject(what);
            conoutf("RETAIL_QA act door %s waiting: power %.2f position %.2f", what,
                    id < 0 ? -1.0f : float(haloscenariocall("device_get_power", id, 0)), position);
            execute("halo_script_status");
        }
        if(elapsed > qaRetailWaitLimit)
        {
            const int id = haloscenarioobject(what);
            conoutf("RETAIL_QA act door %s never opened: power %.2f position %.2f", what,
                    id < 0 ? -1.0f : float(haloscenariocall("device_get_power", id, 0)), position);
            qaRetailStop("FAIL door-never-opened");
        }
        return true;
    }

    void qaVehicleState(const char *label)
    {
        vec pos(0, 0, 0);
        float yaw = 0;
        halovehicleqapose(pos, yaw);
        HaloVehicleStatus st;
        const bool seated = halovehiclestatus(st);
        conoutf("RETAIL_QA vehicle %s row=%d/%d seated=%d pos=%.2f,%.2f,%.2f yaw=%.1f speed=%.2f altitude=%.2f flipped=%d", label,
                qaRetailNext, int(qaRetailRoute.length()), seated, pos.x, pos.y, pos.z, yaw, seated ? st.speed : 0.0f,
                seated ? st.altitude : -1.0f, seated && st.flipped);
    }

    // One tick at a vehicle row; false when the follower's row is not one (it walks as usual).
    bool qaRetailVehicle(const vec &feet)
    {
        const int n = qaRetailRoute.length(), row = qaRetailNext;
        if(row >= n || !(qaRetailFlags[row] & QA_ROUTE_VEHICLE_ANY)) return false;
        const int flags = qaRetailFlags[row];
        const char *type = qaRetailDevice[row].c_str();
        if(qaVehRow != row)
        {
            const bool kind = qaVehRow < 0 || (qaRetailFlags[qaVehRow] & QA_ROUTE_VEHICLE_ANY) != (flags & QA_ROUTE_VEHICLE_ANY);
            if(kind) conoutf("RETAIL_QA vehicle %s %s at waypoint %d", flags & QA_ROUTE_BOARD ? "board" : flags & QA_ROUTE_GET_OUT ? "get-out" : "drive",
                             type, row);
            if(kind) qaVehSince = lastmillis;
            qaVehRow = row;
            qaVehProgress = lastmillis;
        }
        player1->strafe = 0;
        jumpn(false);
        player1->trycrouch = false;
        const int elapsed = lastmillis - qaVehSince;
        auto next = [&](const char *how)
        {
            if(how) conoutf("RETAIL_QA vehicle %s at waypoint %d after %d ms", how, row, elapsed);
            qaRetailNext = row + 1;
            qaRetailClosest = 1e9f;
            qaRetailProgress = qaVehProgress = lastmillis;
            qaVehChanged = lastmillis;
            qaWalkStretchFrom = -1;
            return true;
        };
        auto use = [&](int every)
        {
            if(lastmillis - qaVehUsed < every) return;
            qaVehUsed = lastmillis;
            execute("halo_use");
        };
        // Contact-stage and nearby hostiles are removed, as on foot.
        if(lastmillis - qaVehLogged > 5000)
        {
            qaVehLogged = lastmillis;
            qaVehicleState("at");
            loopv(enemies) if(enemies[i].bot && enemies[i].bot->state == CS_ALIVE && !haloteamfriendly(enemies[i].team) &&
                              !haloactorpassive(enemies[i].bot) && enemies[i].bot->o.dist(player1->o) < 40.0f)
                dodamage(INT_MAX, enemies[i].bot, player1, GUN_ASSAULT);
        }
        if(flags & QA_ROUTE_BOARD)
        {
            if(halodriving()) { ++qaVehBoards; qaVehSeated = true; return next("boarded"); }
            vec entry;
            if(halovehicleqafind(type, feet, entry) < 0)
            {
                // c40 section 4's Banshee appears beside the player a few seconds in.
                qaRetailHold();
                if(elapsed > 15000) qaRetailStop("FAIL vehicle-not-found");
                return true;
            }
            const vec to = vec(entry).sub(feet);
            const char *prompt = halovehicleprompt(1e9f);
            if(prompt && to.magnitudexy() < 6.0f) { forward(false); use(1000); }
            else
            {
                player1->yaw = qaRetailHeading(to);
                player1->pitch = 0;
                forward(to.magnitudexy() > .5f);
                if(to.magnitudexy() <= .5f) use(1000);
            }
            qaRetailProgress = lastmillis;
            if(elapsed > 30000) qaRetailStop("FAIL vehicle-never-boarded");
            return true;
        }
        forward(false);
        if(!halodriving())
        {
            if(flags & QA_ROUTE_GET_OUT) return next("out");
            // A driven section seats the player at its start (the Maw's escape); otherwise, or after
            // being thrown out, use gets in (or flips the vehicle back first).
            qaRetailProgress = lastmillis;
            use(1500);
            if(qaVehSeated) { ++qaVehUnflips; qaVehSeated = false; conoutf("RETAIL_QA vehicle out of the seat at waypoint %d: using it again", row); }
            if(elapsed > 20000) qaRetailStop("FAIL vehicle-not-seated");
            return true;
        }
        qaVehSeated = true;
        HaloVehicleStatus st;
        halovehiclestatus(st);
        vec pos;
        float yaw;
        if(!halovehicleqapose(pos, yaw)) return true;
        const bool flyer = st.vehicleClass == HALO_VEHICLE_FLYER;
        const float s = max(haloscenarioscale(), .1f);
        if(st.flipped) { use(1500); qaVehProgress = lastmillis; return true; }
        // Rows reached: any of the run's next 40 within reach of the vehicle (corners may be cut).
        if(flags & QA_ROUTE_VEHICLE)
        {
            const float reach = flyer ? 1.6f * s : 1.2f * s + 1.5f;
            int reached = -1;
            for(int k = row; k < n && k <= row + 40 && (qaRetailFlags[k] & QA_ROUTE_VEHICLE); ++k)
            {
                const vec d = vec(qaRetailRoute[k]).sub(pos);
                if(flyer ? d.magnitude() <= reach : d.magnitudexy() <= reach && fabsf(d.z) < 3 * s) reached = k;
            }
            if(reached >= row)
            {
                for(int k = row; k <= reached; ++k) if(qaRetailFlags[k] & 2) ++qaVehJumps;
                if(reached / 100 != row / 100) conoutf("RETAIL_QA progress %d/%d", reached, n);
                qaRetailNext = reached + 1;
                qaVehRow = reached;
                qaVehProgress = qaRetailProgress = lastmillis;
                return true;
            }
        }
        // Steer at a row a little ahead (the last of the run, or the get-out row).
        int aim = row;
        const float look = flyer ? 2.5f * s : max(8.0f, 2.0f * s);
        while(aim + 1 < n && (qaRetailFlags[aim] & QA_ROUTE_VEHICLE) && (qaRetailFlags[aim + 1] & (QA_ROUTE_VEHICLE | QA_ROUTE_GET_OUT)) &&
              vec(qaRetailRoute[aim]).sub(pos).magnitude() < look) ++aim;
        const vec to = vec(qaRetailRoute[aim]).sub(pos);
        const float heading = qaRetailHeading(to);
        if(flags & QA_ROUTE_GET_OUT)
        {
            // Over the row: stop and come down until the engine lets the pilot out.
            const vec over = vec(qaRetailRoute[row]).sub(pos);
            const bool there = over.magnitudexy() < .6f * s;
            player1->yaw = heading;
            player1->pitch = 0;
            if(flyer) halovehicleqaflight(there ? 0 : clamp(over.magnitudexy() / (3 * s), .15f, .6f), 0, there ? -1 : clamp(over.z / (2 * s), -1.0f, 1.0f), 150);
            else halovehicleqainput(there ? 0 : .5f, 2, 150);
            if(there && (!flyer || st.altitude < 0 || st.altitude < 1.3f)) use(1000);
            if(elapsed > 30000) qaRetailStop("FAIL vehicle-could-not-get-out");
            return true;
        }
        player1->yaw = heading;
        if(flyer)
        {
            // The nose follows the camera: pitch at the row, full throttle, the climb trimming the height.
            player1->pitch = clamp(atan2f(to.z, max(to.magnitudexy(), .01f)) / RAD, -45.0f, 45.0f);
            const float distance = to.magnitude();
            const bool lastRow = !(aim + 1 < n && (qaRetailFlags[aim + 1] & QA_ROUTE_VEHICLE));
            halovehicleqaflight(lastRow ? clamp(distance / (4 * s), .2f, 1.0f) : 1.0f, 0, clamp(to.z / (2 * s), -1.0f, 1.0f), 150);
        }
        else
        {
            player1->pitch = 0;
            // Full throttle, steering at the camera; backing off for a second when it stalls.
            if(lastmillis < qaVehReverse) halovehicleqainput(-1, 0, 150);
            else
            {
                halovehicleqainput(fabsf(fmodf(heading - yaw + 540.0f, 360.0f) - 180.0f) > 100 ? .5f : 1.0f, 2, 150);
                if(st.speed < .3f) { if(!qaVehStalled) qaVehStalled = lastmillis; }
                else qaVehStalled = 0;
                if(qaVehStalled && lastmillis - qaVehStalled > 1500)
                {
                    qaVehStalled = 0;
                    qaVehReverse = lastmillis + 900;
                    conoutf("RETAIL_QA vehicle stalled at waypoint %d: backing off", row);
                }
            }
        }
        qaRetailProgress = lastmillis;
        if(lastmillis - qaVehProgress > 25000)
        {
            qaVehicleState("stuck");
            conoutf("RETAIL_QA vehicle stuck target=%.2f,%.2f,%.2f", qaRetailRoute[row].x, qaRetailRoute[row].y, qaRetailRoute[row].z);
            qaRetailStop("FAIL vehicle-stuck");
        }
        return true;
    }

    // The engine put the player back after a fall or a wedge (Halo recovery): carry on from the
    // route row nearest to where they now stand, and wait for a bridge again if one is ahead.
    void qaRetailResync(const vec &feet)
    {
        int best = qaRetailNext;
        float distance = 1e9f;
        for(int i = max(qaRetailNext - 400, 0); i <= qaRetailNext && i < qaRetailRoute.length(); ++i)
        {
            const float d = vec(qaRetailRoute[i]).sub(feet).magnitude();
            if(d < distance) { distance = d; best = i; }
        }
        ++qaRetailRecoveries;
        conoutf("RETAIL_QA recovered to %.2f,%.2f,%.2f: waypoint %d -> %d", feet.x, feet.y, feet.z, qaRetailNext, best);
        qaRetailNext = best;
        qaBridgeCleared = qaBridgeStep = -1;
        qaRide = RIDE_NONE;
        // An action cut short (a fall or revert while shooting) stops firing and starts afresh,
        // with its own timers, when the follower is back on its row.
        attack(false);
        qaAct = -1;
        qaRetailClosest = 1e9f;
        qaRetailProgress = lastmillis;
    }

    // Starts the next chained section: the player arrived there through the transition, with
    // the mission script carried over, as in a real playthrough.
    bool qaRetailNextSection()
    {
        if(qaRetailChain.empty()) return false;
        const std::string file = qaRetailChain.front();
        qaRetailChain.erase(qaRetailChain.begin());
        if(!qaRetailLoad(file)) { conoutf("RETAIL_QA FAIL invalid-route-file"); fflush(stdout); execute("quit"); return true; }
        ++qaRetailSection;
        qaRetailNext = 0;
        qaRetailWalking = qaRetailAwaitNext = false;
        qaRetailStable = qaRetailWaited = qaRetailCrouched = qaRetailJumps = 0;
        qaRetailRetreat = qaRetailCrouchUntil = qaRetailWaitAt = -1;
        qaRetailDetour.setsize(0);
        qaRetailClosest = 1e9f;
        qaRetailStarted = lastmillis;
        qaRetailSeatedSince = 0;
        qaRide = RIDE_NONE;
        qaRideStep = qaBridgeStep = qaBridgeCleared = -1;
        qaRetailRecoveries = 0;
        qaRetailHaveFeet = false;
        qaAct = qaActDone = -1;
        qaVehRow = -1;
        qaVehBoards = qaVehJumps = qaVehUnflips = 0;
        qaTrialPhase = TRIAL_NONE;
        qaWalkStretchFrom = -1;
        qaWalkSpeedMin = 1e9f;
        qaWalkStretches = 0;
        player1->trycrouch = false;
        return true;
    }

    void qaRetailStep()
    {
        // Eight minutes a section; a long route (d40's engine room walks every manifold's bay, 5,000
        // rows) gets 90 ms more for each row past 3,000.
        // (The time held in a scripted seat does not count.)
        const int held = qaRetailSeatedSince ? lastmillis - qaRetailSeatedSince : 0;
        if(lastmillis - qaRetailStarted - held > 480000 + max(0, int(qaRetailRoute.length()) - 3000) * 90) { qaRetailStop("FAIL deadline"); return; }
        addsleep(12, "qa_retail_step", true);
        if(qaRetailStuckAt)
        {
            if(lastmillis >= qaRetailStuckAt) { execute("retaildebug 0"); qaRetailStop("FAIL stuck"); }
            return;
        }
        if(!qaRetailWalking)
        {
            if(halocinematic()) { halocinematicskip(); qaRetailStable = 0; return; }
            const bool live = haloactive() && mission == qaRetailMission && section == qaRetailSection &&
                              !launchPending && !startPending && player1->state == CS_ALIVE;
            qaRetailStable = live ? qaRetailStable + 1 : 0;
            if(qaRetailStable < 25) return;
            haloplayerprotect(true);
            // Vehicles are kept from harm as the player is (a flown Banshee is destroyed by enough fire).
            loopv(qaRetailFlags) if(qaRetailFlags[i] & QA_ROUTE_VEHICLE_ANY) { halo_vehicledamage = 0; break; }
            qaRetailWalking = true;
            qaRetailMap = getclientmap();
            qaRetailLive = lastmillis;
            qaRetailProgress = lastmillis;
            // Let the spawn settle before reading it: gravity has run for half a second.
            const vec feet = vec(player1->o).sub(vec(0, 0, player1->eyeheight));
            const float gap = qaRetailFloorGap(feet, 1.0f);
            conoutf("RETAIL_QA spawn map=%s collision=%d feet=%.2f,%.2f,%.2f floorgap=%.3f onfloor=%d alive=%d stage=%d",
                    getclientmap(), halocollisionactive(), feet.x, feet.y, feet.z, gap, player1->onfloor,
                    player1->state == CS_ALIVE, stage);
            qaRetailAudit();
            fflush(stdout);
            return;
        }
        const bool next = mission == qaRetailMission && section == qaRetailSection + 1;
        if(qaRetailAwaitNext)
        {
            // Left through the exit scene: wait for the next section to load.
            if(halocinematic()) halocinematicskip();
            else if(stage == COMPLETE && !next && lastmillis - qaRetailNudged > 1000) { execute("halo_next"); qaRetailNudged = lastmillis; }
            if(next) qaRetailNextSection();
            else if(lastmillis - qaRetailProgress > 30000) qaRetailStop("FAIL no-next-section");
            return;
        }
        if(halocinematic())
        {
            // The exit cue (or a scene on the way) took over: the section was left on foot.
            if(qaRetailNext >= int(qaRetailRoute.length()) - 12)
            {
                if(qaRetailChain.empty()) { qaRetailStop("PASS exit-cue"); return; }
                qaRetailReport("PASS exit-cue");
                qaRetailAwaitNext = true;
                qaRetailProgress = lastmillis;
            }
            halocinematicskip();
            return;
        }
        // A script holds the player in a seat (a10's cryotube until the tutorial's
        // unit_exit_vehicle): wait, turning for the tutorial's look prompts, and take the time held
        // off the section's deadline. The walk resumes from wherever the script puts the player
        // (a script move, not a recovery: the feet are read afresh).
        if(haloscenarioscriptseated())
        {
            if(!qaRetailSeatedSince)
            {
                qaRetailSeatedSince = lastmillis;
                conoutf("RETAIL_QA held in a scripted seat at waypoint %d", qaRetailNext);
            }
            forward(false);
            player1->strafe = 0;
            player1->yaw = fmodf(player1->yaw + 1.5f, 360.0f);
            qaRetailProgress = lastmillis;
            qaRetailClosest = 1e9f;
            qaRetailHaveFeet = false;
            return;
        }
        if(qaRetailSeatedSince)
        {
            conoutf("RETAIL_QA let out of the scripted seat after %.1f s", (lastmillis - qaRetailSeatedSince) / 1000.0f);
            qaRetailStarted += lastmillis - qaRetailSeatedSince;
            qaRetailSeatedSince = 0;
            qaRetailProgress = lastmillis;
            fflush(stdout);
        }
        if(mission != qaRetailMission || section != qaRetailSection)
        {
            if(!next || qaRetailChain.empty()) { qaRetailStop(next ? "PASS exit-transition" : "FAIL left-section"); return; }
            qaRetailReport("PASS exit-transition");
            qaRetailNextSection();
            return;
        }
        if(stage == COMPLETE)
        {
            if(qaRetailChain.empty()) { qaRetailStop("PASS exit-complete"); return; }
            // Press Enter to continue, as the completion screen asks.
            qaRetailReport("PASS exit-complete");
            qaRetailAwaitNext = true;
            qaRetailProgress = lastmillis;
            execute("halo_next");
            return;
        }
        if(player1->state != CS_ALIVE) { qaRetailStop("FAIL died"); return; }
        if(stage == FIRST_CONTACT || stage == SECOND_CONTACT)
        {
            loopv(enemies) if(enemies[i].bot && enemies[i].bot->state == CS_ALIVE)
                dodamage(INT_MAX, enemies[i].bot, player1, GUN_ASSAULT);
        }
        if(qaTrialPhase != TRIAL_NONE && qaTrialPhase != TRIAL_DONE)
        {
            qaTrialStep();
            qaRetailProgress = lastmillis;
            return;
        }
        if(qaTrialPhase == TRIAL_NONE && qaRetailNext < qaRetailRoute.length())
        {
            // The first straight, flat stretch of 22+ steps that starts at the follower.
            for(int start = qaRetailNext; start <= qaRetailNext + 2 && start < qaRetailRoute.length(); ++start)
            {
                const int length = qaRetailStraight(start);
                if(length >= 22 && qaTrialClear(start, length)) { qaTrialBegin(start, length); return; }
            }
        }
        if(qaRetailNext >= qaRetailRoute.length())
        {
            // The route ends inside the exit volume; give the transition a moment to fire,
            // walking on a little the way the route was heading, then standing.
            const int n = qaRetailRoute.length();
            if(n >= 2)
            {
                const vec last = vec(qaRetailRoute[n - 1]).sub(qaRetailRoute[n - 2]);
                player1->yaw = fmodf(atan2f(last.y, last.x) / RAD + 90.0f + 360.0f, 360.0f);
            }
            // b30's return ends with an interaction at its end marker (board the Pelican with E once it
            // hovers at the ledge): stand there pressing use, as a player would, for up to two minutes
            // while the Pelican comes in. Other sections end on their own, as before.
            const char *interaction = retailCartographer() && section == 2 ? sectionInteraction() : nullptr;
            forward(!interaction && lastmillis - qaRetailProgress < 400);
            player1->strafe = 0;
            if(interaction)
            {
                static int pressed = 0;
                if(lastmillis - pressed > 1500)
                {
                    pressed = lastmillis;
                    conoutf("RETAIL_QA interact \"%s\" at the route's end", interaction);
                    execute("halo_use");
                }
                if(lastmillis - qaRetailProgress > 120000) qaRetailStop("FAIL interaction-never-completed");
                return;
            }
            if(lastmillis - qaRetailProgress > 5000) qaRetailStop("FAIL route-ended-without-exit");
            return;
        }
        const vec feet = vec(player1->o).sub(vec(0, 0, player1->eyeheight));
        // A jump of more than 6 units in one tick is the engine's fall or wedge recovery.
        // (Not while seated or just in or out of a seat: the seat and the way out move the body.)
        if(qaRetailHaveFeet && feet.dist(qaRetailLastFeet) > 6.0f && qaRide != RIDE_ON && !halodriving() && lastmillis - qaVehChanged > 1500)
            qaRetailResync(feet);
        if(halodriving()) qaVehChanged = lastmillis;
        qaRetailLastFeet = feet;
        qaRetailHaveFeet = true;
        if(qaRetailVehicle(feet) || qaRetailRide(feet) || qaRetailBridge(feet) || qaRetailAct(feet)) return;
        if(!qaRetailDetour.empty() || qaRetailRetreat >= 0) player1->strafe = 0;
        if(!qaRetailDetour.empty())
        {
            const vec to = vec(qaRetailDetour[0]).sub(feet);
            if(to.magnitudexy() < .35f) qaRetailDetour.remove(0);
            else
            {
                player1->yaw = fmodf(atan2f(to.y, to.x) / RAD + 90.0f + 360.0f, 360.0f);
                forward(true);
            }
            if(qaRetailDetour.empty() || lastmillis - qaRetailDetourStart > 5000)
            {
                qaRetailDetour.setsize(0);
                qaRetailProgress = lastmillis;
                qaRetailClosest = 1e9f;
            }
            return;
        }
        if(qaRetailRetreat >= 0)
        {
            // Back off a few steps along the route and hold there, then try the device again.
            const vec back = vec(qaRetailRoute[qaRetailRetreat]).sub(feet);
            const bool there = back.magnitudexy() < 1.0f;
            if(!there && lastmillis < qaRetailHoldUntil)
            {
                player1->yaw = fmodf(atan2f(back.y, back.x) / RAD + 90.0f + 360.0f, 360.0f);
                forward(true);
            }
            else
            {
                // Look around while holding: tutorial scripts wait for the player to look at
                // the crewman or the door before they open it.
                forward(false);
                player1->yaw = fmodf(player1->yaw + 2.0f, 360.0f);
            }
            if(lastmillis >= qaRetailHoldUntil)
            {
                qaRetailRetreat = -1;
                qaRetailProgress = lastmillis;
                qaRetailClosest = 1e9f;
                qaWalkStretchFrom = -1;
            }
            return;
        }
        qaWalkStretch(feet);
        // A waypoint counts once the player is within reach of it or of any of the next few,
        // so cutting a corner is not mistaken for missing it. A lift deck or a bridge still to
        // extend is reached only through qaRetailRide and qaRetailBridge.
        const int hold = qaRetailHoldRow();
        for(int ahead = 6; ahead >= 0; --ahead)
        {
            if(qaRetailNext + ahead >= qaRetailRoute.length() || qaRetailNext + ahead >= hold) continue;
            const vec offset = vec(qaRetailRoute[qaRetailNext + ahead]).sub(feet);
            if(offset.magnitudexy() <= 1.2f && fabsf(offset.z) < 2.0f)
            {
                if((qaRetailNext + ahead) / 100 != qaRetailNext / 100)
                    conoutf("RETAIL_QA progress %d/%d", qaRetailNext + ahead, int(qaRetailRoute.length()));
                qaRetailNext += ahead + 1;
                qaRetailClosest = 1e9f;
                qaRetailProgress = lastmillis;
                jumpn(false);
                return;
            }
        }
        const vec delta = vec(qaRetailRoute[qaRetailNext]).sub(feet);
        const float distance = delta.magnitudexy();
        if(distance + .25f < qaRetailClosest)
        {
            qaRetailClosest = distance;
            qaRetailProgress = lastmillis;
        }
        if(lastmillis - qaRetailProgress > 7000)
        {
            HaloCollisionHit hit;
            const vec step = vec(delta.x, delta.y, 0).normalize().mul(.6f);
            const bool wall = halocollisionsweep(feet, step, player1->radius, player1->eyeheight + player1->aboveeye, hit);
            const vec saved = player1->o;
            float hi = 127, lo = -128;
            player1->o.add(step);
            const bool device = haloscenariocollide(player1, hi, lo);
            const char *deviceName = device ? haloscenarioblockingdevice(player1) : nullptr;
            player1->o = saved;
            if(device && qaRetailDetour.empty())
            {
                // A door open only part way (a10's jammed cryo bay doors): look sideways along
                // it for the gap and walk through that instead of the route's line.
                const vec dir = vec(delta.x, delta.y, 0).normalize(), side(-dir.y, dir.x, 0);
                for(int k = 1; k <= 24; ++k)
                {
                    const float offset = (k & 1 ? 1 : -1) * .25f * ((k + 1) / 2);
                    const vec before = vec(feet).add(vec(side).mul(offset));
                    bool open = qaRetailFits(before);
                    for(float ahead = .5f; open && ahead <= 4.0f; ahead += .5f) open = qaRetailFits(vec(before).add(vec(dir).mul(ahead)));
                    if(!open) continue;
                    conoutf("RETAIL_QA squeezing past device %s at waypoint %d, %.2f to the side", deviceName ? deviceName : "?", qaRetailNext, offset);
                    qaRetailDetour.setsize(0);
                    qaRetailDetour.add(before);
                    qaRetailDetour.add(vec(before).add(vec(dir).mul(4.0f)));
                    qaRetailDetourStart = lastmillis;
                    qaRetailProgress = lastmillis;
                    qaRetailClosest = 1e9f;
                    return;
                }
            }
            if(device && qaRetailCrouchUntil < qaRetailNext)
            {
                // A jammed door (a10's cryo bay) opens only part way: crouch under it if a
                // crouching body fits, as the tutorial asks the player to.
                const float stand = player1->eyeheight, low = player1->maxeyeheight * .75f;
                player1->eyeheight = low;
                player1->o = vec(feet).add(step).add(vec(0, 0, low));
                float chi = 127, clo = -128;
                HaloCollisionHit body;
                const bool fits = !haloscenariocollide(player1, chi, clo) &&
                                  !halocollisionoverlap(vec(feet).add(step).add(vec(0, 0, .05f)), player1->radius, low + player1->aboveeye, body);
                player1->eyeheight = stand;
                player1->o = saved;
                if(fits)
                {
                    conoutf("RETAIL_QA crouching under device %s at waypoint %d", deviceName ? deviceName : "?", qaRetailNext);
                    qaRetailCrouchUntil = qaRetailNext + 8;
                    qaRetailProgress = lastmillis;
                    qaRetailClosest = 1e9f;
                    return;
                }
            }
            if(device && qaRetailWaitAt >= 0 && qaRetailNext > qaRetailWaitAt + 2) qaRetailWaited = 0; // a new device
            if(device && qaRetailWaited < qaRetailWaitLimit)
            {
                // A scripted door or lift is closed: wait for the script to open it, as a player
                // would. Scripts open doors once the Covenant in the room is beaten, or once an
                // ally has walked up to them, so clear the hostiles and step back out of the way.
                if(!qaRetailWaited) conoutf("RETAIL_QA waiting for device %s at waypoint %d", deviceName ? deviceName : "?", qaRetailNext);
                qaRetailWaitAt = qaRetailNext;
                loopv(enemies) if(enemies[i].bot && enemies[i].bot->state == CS_ALIVE && !haloteamfriendly(enemies[i].team) &&
                                  !haloactorpassive(enemies[i].bot))
                    dodamage(INT_MAX, enemies[i].bot, player1, GUN_ASSAULT);
                // Doors that are not automatic open from a control beside them: press use.
                execute("halo_use");
                qaRetailWaited += lastmillis - qaRetailProgress + 6000;
                qaRetailRetreat = max(qaRetailNext - 8, 0);
                qaRetailHoldUntil = lastmillis + 6000;
                return;
            }
            conoutf("RETAIL_QA stuck target=%.2f,%.2f,%.2f distance=%.3f dz=%.3f velocity=%.3f,%.3f,%.3f onfloor=%d wall=%d device=%d waited=%dms",
                    qaRetailRoute[qaRetailNext].x, qaRetailRoute[qaRetailNext].y, qaRetailRoute[qaRetailNext].z,
                    distance, delta.z, player1->vel.x, player1->vel.y, player1->vel.z, player1->onfloor, wall, device, qaRetailWaited);
            if(device) conoutf("RETAIL_QA blocked by device %s", deviceName ? deviceName : "?");
            if(wall) conoutf("RETAIL_QA blocked by surface %d normal=%.2f,%.2f,%.2f material=%d",
                             hit.surface, hit.normal.x, hit.normal.y, hit.normal.z, hit.material);
            // Log a few physics ticks of the stuck movement before stopping.
            execute("retaildebug 1");
            qaRetailStuckAt = lastmillis + 25;
            return;
            return;
        }
        // Aim one cell ahead on the route, damped by the current velocity.
        const vec &aim = qaRetailRoute[min(qaRetailNext + 1, int(qaRetailRoute.length()) - 1)];
        const vec toward = vec(aim).sub(feet);
        const float damping = distance < 5.0f ? 1.9f : .65f;
        const float heading = atan2f(toward.y - damping * player1->vel.y, toward.x - damping * player1->vel.x) / RAD + 90.0f;
        player1->yaw = fmodf(heading + 360.0f, 360.0f);
        player1->pitch = 0;
        // Hold forward at full speed, as a player would: the route ends inside the exit volume.
        forward(true);
        // Held up for a while with no door in the way (a crewman standing in the corridor):
        // sidestep one way and then the other, as a player steps round someone.
        player1->strafe = lastmillis - qaRetailProgress > 2500 ? ((lastmillis / 900) & 1 ? 1 : -1) : 0;
        // Covenant standing in the way are fought, as the player would: they are removed.
        if(lastmillis - qaRetailProgress > 2500)
            loopv(enemies) if(enemies[i].bot && enemies[i].bot->state == CS_ALIVE && !haloteamfriendly(enemies[i].team) &&
                              !haloactorpassive(enemies[i].bot) && enemies[i].bot->o.dist(player1->o) < 10.0f)
                dodamage(INT_MAX, enemies[i].bot, player1, GUN_ASSAULT);
        // Crouch through the steps the route marks (a jammed bulkhead), from two steps before.
        bool low = qaRetailNext <= qaRetailCrouchUntil;
        for(int k = max(qaRetailNext - 2, 0); k <= qaRetailNext + 2 && k < qaRetailCrouch.length(); ++k) low = low || qaRetailCrouch[k];
        if(low && !player1->trycrouch) ++qaRetailCrouched;
        player1->trycrouch = low;
        // A jump step: take off from the step before it, at full speed.
        if(qaRetailJump[qaRetailNext] && qaRetailNext > 0 && player1->onfloor && lastmillis - qaRetailLastJump > 400 &&
           vec(qaRetailRoute[qaRetailNext - 1]).sub(feet).magnitudexy() < .9f)
        {
            jumpn(true);
            qaRetailLastJump = lastmillis;
            ++qaRetailJumps;
        }
        else if(lastmillis - qaRetailProgress > 1500 && lastmillis - qaRetailLastJump > 1300 &&
           player1->onfloor && distance < 10.0f && delta.z > 0.25f && delta.z < 4.0f)
        {
            jumpn(true);
            forward(true);
            qaRetailLastJump = lastmillis;
            conoutf("RETAIL_QA jumping-lip waypoint=%d", qaRetailNext);
        }
        else if(lastmillis - qaRetailLastJump > 180) jumpn(false);
    }
}

COMMANDF(qa_retail_step, "", () { qaRetailStep(); });
// The sections loaded after `qa_stock_geometry 1` play on stock AssaultCube geometry even where their
// .hcol exists (halocollisionmap logs "retail collision off for <map>"): the stock-geometry checks of
// test_vehicles.py, test_presentation.py and test_damage_model.py, since no campaign section is stock
// any more (the d40 section 3 batch). Not saved; the game has no such switch.
VARF(qa_stock_geometry, 0, 0, 1, halocollisionstockonly = qa_stock_geometry != 0);
COMMANDF(qa_retail_start, "ii", (int *m, int *s)
{
    // HALOQA_RETAIL_ROUTE: the route file; HALOQA_RETAIL_CHAIN: newline-separated route files
    // of the following sections, walked on through each transition in the same run.
    const char *path = getenv("HALOQA_RETAIL_ROUTE");
    if(!qaRetailLoad(path ? path : "")) { conoutf("RETAIL_QA FAIL invalid-route-file"); fflush(stdout); execute("quit"); return; }
    if(const char *chain = getenv("HALOQA_RETAIL_CHAIN"))
    {
        std::string rest = chain;
        for(size_t start = 0; start < rest.size();)
        {
            size_t end = rest.find('\n', start);
            if(end == std::string::npos) end = rest.size();
            if(end > start) qaRetailChain.push_back(rest.substr(start, end - start));
            start = end + 1;
        }
    }
    qaRetailMission = *m - 1;
    qaRetailSection = *s - 1;
    if(const char *wait = getenv("HALOQA_RETAIL_WAIT")) qaRetailWaitLimit = atoi(wait);
    qaRetailStarted = lastmillis;
    qaRetailSeatedSince = 0;
    loadProgress();
    queueSection(qaRetailMission, qaRetailSection);
    addsleep(100, "qa_retail_step", true);
});
// qa_retail_overlap x y z: whether a standing body with its feet there overlaps retail collision.
COMMANDF(qa_retail_overlap, "fff", (float *x, float *y, float *z)
{
    HaloCollisionHit hit;
    const bool overlap = halocollisionoverlap(vec(*x, *y, *z), player1->radius, player1->eyeheight + player1->aboveeye, hit);
    conoutf("RETAIL_QA overlap %.2f,%.2f,%.2f = %d surface=%d depth=%.3f normal=%.2f,%.2f,%.2f", *x, *y, *z, overlap, hit.surface,
            hit.penetration, hit.normal.x, hit.normal.y, hit.normal.z);
    fflush(stdout);
});
// qa_retail_assists: the loaded .hcol's walking assists (its AST1 trailer, ASSISTS in
// tools/import_halo_campaign.py), each checked the way the engine uses it: a ray (a shot, a sight
// line, a probe) down through its centre must not stop on it, a body's capsule swept down onto it
// must stand on it, and a grenade's sweep (bodies only collide with assists) must pass through it.
// For tests/test_retail_assists.py.
COMMANDF(qa_retail_assists, "", ()
{
    vec centre;
    vec normal;
    const int count = halocollisionassist(-1, centre, normal);
    conoutf("RETAIL_QA assists map=%s collision=%d count=%d", getclientmap(), halocollisionactive(), count);
    loopi(count)
    {
        halocollisionassist(i, centre, normal);
        const vec above = vec(centre).add(vec(0, 0, 1.5f));
        const vec down(0, 0, -3.0f);
        HaloCollisionHit ray;
        HaloCollisionHit body;
        HaloCollisionHit bounce;
        // The ray may stop on retail floor below the assist, never on the assist itself.
        const bool rayHit = halocollisiontrace(above, vec(above).add(down), ray);
        const bool rayThrough = !rayHit || (!(ray.flags & haloCollisionAssist) && above.z + down.z * ray.fraction < centre.z - .1f);
        const bool bodyHit = halocollisionsweep(above, down, player1->radius, player1->eyeheight + player1->aboveeye, body);
        const float standZ = above.z + down.z * body.fraction;
        const bool bodyStands = bodyHit && (body.flags & haloCollisionAssist) && fabsf(standZ - centre.z) < .5f && body.normal.z >= .65f;
        const bool bounceHit = halocollisionsweep(above, down, .2f, .4f, bounce, false);
        const bool bounceThrough = !bounceHit || !(bounce.flags & haloCollisionAssist);
        conoutf("RETAIL_QA assist %d centre=%.2f,%.2f,%.2f normal=%.2f,%.2f,%.2f ray_through=%d body_stands=%d stand_z=%.2f bounce_through=%d",
                i, centre.x, centre.y, centre.z, normal.x, normal.y, normal.z, rayThrough, bodyStands, standZ, bounceThrough);
    }
    fflush(stdout);
});
// qa_retail_assist_stand i: puts the player one unit above walking assist i's centre (a QA move:
// haloplayermoved), to fall onto it; qa_retail_where then reports where the feet came to rest.
COMMANDF(qa_retail_assist_stand, "i", (int *index)
{
    vec centre;
    vec normal;
    if(*index < 0 || *index >= halocollisionassist(*index, centre, normal) || player1->state != CS_ALIVE)
    {
        conoutf("RETAIL_QA assist-stand-unavailable %d", *index);
        fflush(stdout);
        return;
    }
    player1->o = vec(centre).add(vec(0, 0, 1 + player1->eyeheight));
    player1->vel = vec();
    player1->onfloor = false;
    player1->resetinterp();
    haloplayermoved("QA assist stand");
    conoutf("RETAIL_QA assist-stand %d at=%.2f,%.2f,%.2f", *index, centre.x, centre.y, centre.z);
    fflush(stdout);
});
// qa_retail_scenery: the loaded .hcol's scenery placements (its SCN1 trailer, SCENERY_COLLISION in
// tools/import_halo_campaign.py), each checked the way Halo collides with scenery: a ray down onto the
// top of its polygons' bounds stops on a scenery surface (shots and sight lines hit it), a grenade's
// sweep (assists off) stops on one too, and so does a body's capsule. A placement thinner than the
// probe at its centre (a tree trunk's side, a tilted plank) may report a miss; the test reads the
// counts. For tests/test_retail_scenery.py.
COMMANDF(qa_retail_scenery, "", ()
{
    int placement = -1;
    vec low;
    vec high;
    const int count = halocollisionscenery(-1, placement, low, high);
    conoutf("RETAIL_QA scenery map=%s collision=%d count=%d", getclientmap(), halocollisionactive(), count);
    loopi(count)
    {
        halocollisionscenery(i, placement, low, high);
        const vec centre((low.x + high.x) / 2, (low.y + high.y) / 2, high.z);
        const vec above = vec(centre).add(vec(0, 0, 1.5f));
        const vec down(0, 0, -(high.z - low.z) - 1.6f);
        HaloCollisionHit ray;
        HaloCollisionHit body;
        HaloCollisionHit bounce;
        const bool rayHits = halocollisiontrace(above, vec(above).add(down), ray) && (ray.flags & haloCollisionScenery);
        const bool bounceHits = halocollisionsweep(above, down, .2f, .4f, bounce, false) && (bounce.flags & haloCollisionScenery);
        const bool bodyHits = halocollisionsweep(above, down, player1->radius, player1->eyeheight + player1->aboveeye, body) &&
                              (body.flags & haloCollisionScenery);
        conoutf("RETAIL_QA scenery %d placement=%d low=%.2f,%.2f,%.2f high=%.2f,%.2f,%.2f ray_hits=%d bounce_hits=%d body_hits=%d",
                i, placement, low.x, low.y, low.z, high.x, high.y, high.z, rayHits, bounceHits, bodyHits);
    }
    fflush(stdout);
});
// qa_raster_waypoints: AssaultCube's flood waypoints against the section's scenery (the cube-raster
// stamp: tools/halo_collision.py scenery_raster puts the baked scenery into the cube raster the flood
// reads). For each flood waypoint, its cube (the flood puts one at a cube's corner, 2 units over the
// cube's floor): `solid` counts waypoints in a solid cube (the flood never places one there), and
// `scenery` those whose cube has scenery in the body's column at its centre: a ray down from 5.2 over
// the cube's floor to 2 over it stops on a scenery top, or a ray up between the same heights on a
// scenery underside (2, not the step, as the cube's floor is the whole unit under the real one). The
// first 12 such waypoints are listed. `marked` counts the map's scenery cubes and `lightpass` says
// whether the engine lets light through them (worldlight.cpp). For tests/test_scenery_raster.py.
COMMANDF(qa_raster_waypoints, "", ()
{
    int total = 0;
    int solid = 0;
    int scenery = 0;
    int listed = 0;
    loopi(MAX_MAP_GRIDS) loopj(MAX_MAP_GRIDS)
    {
        for(TLinkedList<node_s *>::node_s *p = WaypointClass.m_Waypoints[i][j].GetFirst(); p; p = p->next)
        {
            const vec &o = p->Entry->v_origin;
            const int x = int(floorf(o.x));
            const int y = int(floorf(o.y));
            ++total;
            if(OUTBORD(x, y) || SOLID(S(x, y))) { ++solid; continue; }
            const float floorZ = S(x, y)->floor;
            const vec low(x + .5f, y + .5f, floorZ + 2.0f);
            const vec high(x + .5f, y + .5f, floorZ + 5.2f);
            HaloCollisionHit down;
            HaloCollisionHit up;
            const bool top = halocollisiontrace(high, low, down) && (down.flags & haloCollisionScenery) && down.normal.z > 0;
            const bool under = halocollisiontrace(low, high, up) && (up.flags & haloCollisionScenery) && up.normal.z < 0;
            if(!top && !under) continue;
            ++scenery;
            if(listed++ < 12) conoutf("RETAIL_QA raster waypoint at=%.1f,%.1f,%.1f top=%d under=%d", o.x, o.y, o.z, top, under);
        }
    }
    // The scenery cubes the map holds (halo_campaign.h haloSceneryVdelta) and whether light passes them.
    int marked = 0;
    loopi(ssize) loopj(ssize) if(SOLID(S(j, i)) && S(j, i)->vdelta == haloSceneryVdelta) ++marked;
    conoutf("RETAIL_QA raster map=%s collision=%d waypoints=%d solid=%d scenery=%d marked=%d lightpass=%d", getclientmap(),
            halocollisionactive(), total, solid, scenery, marked, haloscenerycubes);
    fflush(stdout);
});
// qa_retail_where: the player's feet and state, for tests/test_retail_recovery.py.
COMMANDF(qa_retail_where, "", ()
{
    const vec feet = vec(player1->o).sub(vec(0, 0, player1->eyeheight));
    conoutf("RETAIL_QA where map=%s collision=%d feet=%.2f,%.2f,%.2f onfloor=%d alive=%d", getclientmap(), halocollisionactive(),
            feet.x, feet.y, feet.z, player1->onfloor, player1->state == CS_ALIVE);
    fflush(stdout);
});
// qa_halo_moved: what every teleport does to the recovery (halo.cpp haloplayermoved), without
// moving: the safe floors go and the wedge check waits a second (tests/test_retail_recovery.py).
COMMANDF(qa_halo_moved, "", () { haloplayermoved("QA teleport"); });
// qa_retail_recover fall|void|wedge puts the player where the engine's retail-collision recovery
// (halo.cpp updateRecovery) has to take over: 20 units below the section's mesh, in the open air
// 16 units beyond the mesh's east edge with nothing below, or with the feet sunk 0.6 units into
// the floor they stand on. It stands in for a fall or a wedge, not a teleport, so unlike the other
// QA moves it does not call haloplayermoved: the safe floors stay and the wedge check starts at once.
COMMANDF(qa_retail_recover, "s", (const char *kind)
{
    vec low;
    vec high;
    if(!halocollisionbounds(low, high) || player1->state != CS_ALIVE)
    {
        conoutf("RETAIL_QA recover-unavailable %s", kind);
        fflush(stdout);
        return;
    }
    const vec feet = vec(player1->o).sub(vec(0, 0, player1->eyeheight));
    vec to = feet;
    if(!strcmp(kind, "fall")) to.z = low.z - 20;
    else if(!strcmp(kind, "void")) to.x = high.x + 16;
    else if(!strcmp(kind, "wedge")) to.z -= .6f;
    else { conoutf("RETAIL_QA recover-unknown %s", kind); fflush(stdout); return; }
    conoutf("RETAIL_QA recover-from %s feet=%.2f,%.2f,%.2f to=%.2f,%.2f,%.2f", kind, feet.x, feet.y, feet.z, to.x, to.y, to.z);
    fflush(stdout);
    player1->o = vec(to).add(vec(0, 0, player1->eyeheight));
    player1->vel = vec();
    player1->onfloor = false;
    player1->resetinterp();
});
