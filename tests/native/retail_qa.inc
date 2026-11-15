// Retail-collision walk harness for any section (tests/test_retail_collision.py).
// Compiled into halo.cpp only when HALO_QA is defined, after b30_qa.inc.
//
// qa_retail_start <mission> <section> loads the section as a real campaign start, audits
// its spawn and markers against the retail collision surfaces, then walks the importer's
// route (HALOQA_RETAIL_ROUTE, x y z feet rows) with ordinary movement input until the
// section completes. Enemies in the contact stages are removed and the player cannot be
// hurt, so only movement and the exit are measured.
#include "halo_collision.h"

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
            // Markers, spawns and items are authored at floor height. Spawns (the player's and
            // the enemy waves') need room for a body; route markers (100), weapon placements
            // (101) and items only need floor under a point.
            const vec feet(e.x, e.y, e.z);
            HaloCollisionHit hit;
            const bool body = e.type == PLAYERSTART && e.attr2 != 100 && e.attr2 != 101;
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
        if(i + 1 >= qaRetailRoute.length()) return 0;
        const vec step = vec(qaRetailRoute[i + 1]).sub(qaRetailRoute[i]);
        if(fabsf(step.z) > .01f || fabsf(step.magnitudexy() - 1) > .01f || qaRetailCrouch[i] || qaRetailCrouch[i + 1]) return 0;
        int j = i + 1;
        while(j + 1 < qaRetailRoute.length())
        {
            const vec next = vec(qaRetailRoute[j + 1]).sub(qaRetailRoute[j]);
            if(next.dist(step) > .01f || fabsf(qaRetailRoute[j + 1].z - qaRetailRoute[i].z) > .01f || qaRetailCrouch[j + 1]) break;
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
        qaRetailNext = resume;
        qaRetailClosest = 1e9f;
        qaRetailProgress = lastmillis;
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
        qaRetailCrouch.setsize(0);
        qaRetailJump.setsize(0);
        std::ifstream input(file);
        std::string line;
        while(std::getline(input, line))
        {
            // x y z [flags]: 1 crouch, 2 jump
            vec point;
            int flags = 0;
            const int fields = sscanf(line.c_str(), "%f %f %f %d", &point.x, &point.y, &point.z, &flags);
            if(fields < 3)
            {
                if(line.find_first_not_of(" \t\r") != std::string::npos) return false;
                continue;
            }
            if(fields < 4) flags = 0;
            qaRetailRoute.add(point);
            qaRetailCrouch.add((flags & 1) != 0);
            qaRetailJump.add((flags & 2) != 0);
        }
        return !qaRetailRoute.empty();
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
        qaTrialPhase = TRIAL_NONE;
        qaWalkStretchFrom = -1;
        qaWalkSpeedMin = 1e9f;
        qaWalkStretches = 0;
        player1->trycrouch = false;
        return true;
    }

    void qaRetailStep()
    {
        if(lastmillis - qaRetailStarted > 480000) { qaRetailStop("FAIL deadline"); return; }
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
            forward(lastmillis - qaRetailProgress < 400);
            player1->strafe = 0;
            if(lastmillis - qaRetailProgress > 5000) qaRetailStop("FAIL route-ended-without-exit");
            return;
        }
        const vec feet = vec(player1->o).sub(vec(0, 0, player1->eyeheight));
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
        // so cutting a corner is not mistaken for missing it.
        for(int ahead = 6; ahead >= 0; --ahead)
        {
            if(qaRetailNext + ahead >= qaRetailRoute.length()) continue;
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
                loopv(enemies) if(enemies[i].bot && enemies[i].bot->state == CS_ALIVE && enemies[i].team != HALO_TEAM_HUMAN)
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
            loopv(enemies) if(enemies[i].bot && enemies[i].bot->state == CS_ALIVE && enemies[i].team != HALO_TEAM_HUMAN &&
                              enemies[i].bot->o.dist(player1->o) < 10.0f)
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
