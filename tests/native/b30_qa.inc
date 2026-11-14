// The Silent Cartographer retail-collision route harness (tests/b30_native_qa.py).
// Compiled into halo.cpp only when HALO_QA is defined.
#include <fstream>
#include <string>
#include <vector>

extern void forward(bool isdown);
extern void jumpn(bool isdown);

namespace
{
    struct QaB30Waypoint
    {
        vec feet;
        std::string action;
    };

    enum QaB30Phase { QA_WAITING, QA_WALKING, QA_CHECKPOINT, QA_DYING, QA_REVERTING, QA_CONTROL, QA_CINEMATIC };
    std::vector<QaB30Waypoint> qaB30Route;
    QaB30Phase qaB30Phase = QA_WAITING;
    size_t qaB30Next = 0, qaB30CheckpointIndex = 0;
    int qaB30Started = 0, qaB30Changed = 0, qaB30LastProgress = 0;
    int qaB30SceneEnded = 0;
    int qaB30LastJump = 0;
    float qaB30Closest = 100000;
    vec qaB30SavedPosition;
    bool qaB30ProvedRevert = false;

    void qaB30State(const char *label)
    {
        const vec feet = vec(player1->o).sub(vec(0, 0, player1->eyeheight));
        conoutf("B30_QA %s waypoint=%d xy=%.2f,%.2f feet=%.2f onfloor=%d alive=%d checkpoint=%d phase=%d",
                label, int(qaB30Next + 1), feet.x, feet.y, feet.z, player1->onfloor,
                player1->state == CS_ALIVE, atCheckpoint, qaB30Phase);
        fflush(stdout);
    }

    void qaB30Stop(const char *label)
    {
        forward(false);
        jumpn(false);
        qaB30State(label);
        execute("halo_script_status");
        execute("quit");
    }

    void qaB30SetPhase(QaB30Phase phase)
    {
        qaB30Phase = phase;
        qaB30Changed = lastmillis;
    }

    void qaB30Advance()
    {
        jumpn(false);
        ++qaB30Next;
        qaB30Closest = 100000;
        qaB30LastProgress = lastmillis;
    }

    void qaB30Drive()
    {
        if(lastmillis - qaB30Started > 260000) { qaB30Stop("deadline"); return; }
        if(qaB30Phase == QA_WAITING)
        {
            if(haloactive() && mission == 3 && section == 1 && !startPending && !halocinematic())
            {
                qaB30SetPhase(QA_WALKING);
                qaB30LastProgress = lastmillis;
                qaB30State("spawn");
                execute("qa_b30_traces");
            }
        }
        else if(qaB30Phase == QA_CINEMATIC)
        {
            if(!halocinematic() && !qaB30SceneEnded) qaB30SceneEnded = lastmillis;
            if(qaB30SceneEnded && lastmillis - qaB30SceneEnded > 300)
            {
                vec target;
                bool mapped = false;
                if(!halocartographertarget(target, mapped) || !mapped) { qaB30Stop("objective-state-lost"); return; }
                if(!atCheckpoint || checkpointPosition.dist(player1->o) > 2.0f)
                {
                    qaB30Stop("post-objective-checkpoint-missing");
                    return;
                }
                execute("screenshot b30-return-to-play");
                qaB30Stop("PASS cinematic-finished");
                return;
            }
        }
        else if(qaB30Phase == QA_CONTROL)
        {
            if(halocinematic())
            {
                qaB30SetPhase(QA_CINEMATIC);
                qaB30State("control-started-cinematic");
                execute("screenshot b30-cartographer");
            }
            else if(lastmillis - qaB30Changed > 6000) { qaB30Stop("control-failed"); return; }
        }
        else if(qaB30Phase == QA_DYING)
        {
            if(player1->state == CS_DEAD)
            {
                qaB30SetPhase(QA_REVERTING);
                qaB30State("dead");
            }
            else if(lastmillis - qaB30Changed > 1000)
            {
                // The generic suicide command queues a local-server message.
                // Campaign damage runs on the client, so exercise that normal
                // damage path if the server has no live player to suicide.
                qaB30State("campaign-self-damage");
                dodamage(INT_MAX, player1, player1);
            }
        }
        else if(qaB30Phase == QA_REVERTING)
        {
            if(player1->state == CS_ALIVE)
            {
                const float offset = player1->o.dist(qaB30SavedPosition);
                conoutf("B30_QA checkpoint revert offset=%.3f", offset);
                if(offset > 1.5f) { qaB30Stop("wrong-revert-location"); return; }
                qaB30ProvedRevert = true;
                qaB30Next = qaB30CheckpointIndex;
                qaB30Closest = 100000;
                qaB30LastProgress = lastmillis;
                qaB30SetPhase(QA_WALKING);
                qaB30State("reverted");
            }
            else if(lastmillis - qaB30Changed >= 1250) attack(false);
        }
        else if(qaB30Phase == QA_CHECKPOINT)
        {
            if(lastmillis - qaB30Changed > 650)
            {
                if(!atCheckpoint || checkpointPosition.dist(player1->o) > 30.0f)
                {
                    qaB30Stop("authored-checkpoint-missing");
                    return;
                }
                qaB30SavedPosition = checkpointPosition;
                qaB30CheckpointIndex = qaB30Next;
                qaB30State("authored-checkpoint");
                qaB30Advance();
                qaB30SetPhase(QA_WALKING);
            }
        }
        else if(qaB30Phase == QA_WALKING)
        {
            if(!haloactive() || mission != 3 || section != 1) { qaB30Stop("left-section"); return; }
            if(player1->state != CS_ALIVE) { qaB30Stop("unexpected-death"); return; }
            if(qaB30Next >= qaB30Route.size())
            {
                forward(false);
                qaB30SetPhase(QA_CONTROL);
                qaB30State("use-cartographer");
                execute("halo_use");
                addsleep(12, "qa_b30_step", true);
                return;
            }
            const QaB30Waypoint &waypoint = qaB30Route[qaB30Next];
            const vec feet = vec(player1->o).sub(vec(0, 0, player1->eyeheight));
            const vec delta = vec(waypoint.feet).sub(feet);
            const float distance = delta.magnitudexy();
            const bool tagged = waypoint.action != "-";
            if(distance <= (tagged ? .8f : 1.05f) && fabsf(delta.z) < (tagged ? 1.5f : 2.0f))
            {
                if(tagged) qaB30State(waypoint.action.c_str());
                else if(qaB30Next % 6 == 0) qaB30State("progress");
                if(waypoint.action == "checkpoint" && !qaB30ProvedRevert)
                {
                    forward(false);
                    qaB30SetPhase(QA_CHECKPOINT);
                }
                else if(waypoint.action == "suicide" && !qaB30ProvedRevert)
                {
                    forward(false);
                    qaB30SetPhase(QA_DYING);
                    qaB30State("request-suicide");
                    execute("suicide");
                    c2sinfo(player1);
                }
                else qaB30Advance();
            }
            else
            {
                if(distance + .25f < qaB30Closest)
                {
                    qaB30Closest = distance;
                    qaB30LastProgress = lastmillis;
                }
                if(lastmillis - qaB30LastProgress > 7000)
                {
                    conoutf("B30_QA stuck target %.2f %.2f %.2f distance %.3f dz %.3f velocity %.3f %.3f %.3f",
                            waypoint.feet.x, waypoint.feet.y, waypoint.feet.z, distance, delta.z,
                            player1->vel.x, player1->vel.y, player1->vel.z);
                    qaB30Stop("stuck");
                    return;
                }
                const float damping = distance < 5.0f ? 1.9f : .65f;
                const float heading = atan2f(delta.y - damping * player1->vel.y,
                                             delta.x - damping * player1->vel.x) / RAD + 90.0f;
                player1->yaw = fmodf(heading + 360.0f, 360.0f);
                player1->pitch = 0;
                forward(distance > 2.8f || (lastmillis / 36) % 3 == 0);
                if(lastmillis - qaB30LastProgress > 1500 && lastmillis - qaB30LastJump > 1300 &&
                   player1->onfloor && distance < 10.0f && delta.z > 0.25f && delta.z < 4.0f)
                {
                    jumpn(true);
                    forward(true);
                    qaB30LastJump = lastmillis;
                    qaB30State("jumping-lip");
                }
                else if(lastmillis - qaB30LastJump > 180) jumpn(false);
            }
        }
        addsleep(12, "qa_b30_step", true);
    }
}

COMMANDF(qa_b30_step, "", () { qaB30Drive(); });
COMMANDF(qa_b30_start, "", ()
{
    const char *path = getenv("HALOQA_B30_ROUTE");
    std::ifstream input(path ? path : "");
    QaB30Waypoint row;
    while(input >> row.feet.x >> row.feet.y >> row.feet.z >> row.action) qaB30Route.push_back(row);
    if(qaB30Route.empty() || !input.eof()) { qaB30Stop("invalid-waypoint-file"); return; }
    qaB30Started = lastmillis;
    loadProgress();
    queueSection(3, 1);
    addsleep(100, "qa_b30_step", true);
});
