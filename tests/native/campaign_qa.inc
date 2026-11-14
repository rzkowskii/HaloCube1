// Verification hooks for tests/. Compiled into halo.cpp only when HALO_QA is
// defined (the halo-ac-qa target), because they read that file's internal state.

// ---- Combat, shield and checkpoint replay (tests/qa_replay.py)

// Local verification hooks compiled only into the temporary QA executable.
COMMANDF(qa_halo_status, "s", (const char *name)
{
    fflush(stdout);
    conoutf("QA %s stage=%d live=%d pos=%.2f,%.2f,%.2f yaw=%.2f hp=%d shield=%.2f mag=%d reserve=%d frags=%d checkpoint=%d",
        name, stage, remaining(), player1->o.x, player1->o.y, player1->o.z, player1->yaw,
        player1->health, shield, player1->weaponsel->mag, player1->weaponsel->ammo, player1->mag[GUN_GRENADE], atCheckpoint);
    fflush(stdout);
});
COMMANDF(qa_halo_position, "fff", (float *x, float *y, float *yaw)
{
    const sqr *cell = S(int(*x), int(*y));
    player1->o = vec(*x, *y, cell->floor + player1->eyeheight - (cell->type == FHF ? cell->vdelta / 4.0f : 0));
    player1->yaw = *yaw;
    player1->pitch = 0;
    player1->vel = vec();
    entinmap(player1);
    player1->resetinterp();
});
COMMANDF(qa_halo_damage, "i", (int *amount) { dodamage(*amount, player1, player1, GUN_ASSAULT); });
COMMANDF(qa_halo_revert, "", () { tryrespawn(); });
COMMANDF(qa_halo_clear, "", ()
{
    loopv(enemies) if(enemies[i].bot && enemies[i].bot->state == CS_ALIVE)
        dodamage(INT_MAX, enemies[i].bot, player1, GUN_ASSAULT);
});
COMMANDF(qa_halo_fire, "i", (int *on) { attack(*on != 0); });
COMMANDF(qa_halo_move, "i", (int *on) { player1->move = *on; });
COMMANDF(qa_halo_aim, "", ()
{
    playerent *target = NULL;
    float best = 1e16f;
    loopv(enemies)
    {
        auto &enemy = enemies[i];
        if(!enemy.bot || enemy.bot->state != CS_ALIVE || enemy.kind != GRUNT) continue;
        float distance = player1->o.squareddist(enemy.bot->o);
        if(distance < best && raycubelos(player1->o, enemy.bot->o)) { best = distance; target = enemy.bot; }
    }
    if(!target) return;
    vec direction = vec(target->o).sub(player1->o);
    direction.z -= target->eyeheight * .40f;
    player1->yaw = atan2f(direction.x, -direction.y) / RAD;
    if(player1->yaw < 0) player1->yaw += 360.0f;
    player1->pitch = atan2f(direction.z, direction.magnitudexy()) / RAD;
});

// ---- Campaign progression, devices and the 55-section replay
static vec qaLibraryExit(int part)
{
    const vec exits[] = {vec(121.79f, 462.20f, 97.f), vec(825.98f, 555.53f, 126.f), vec(157.95f, 83.31f, 116.4f)};
    return exits[part];
}

COMMANDF(qa_campaign_load, "ii", (int *m, int *s)
{
    loadProgress();
    queueSection(*m - 1, *s - 1, true);
});
COMMANDF(qa_campaign_state, "", ()
{
    conoutf("CAMPAIGN_QA map=%s stage=%d alive=%d spawn=%d,%d,%d",
        getclientmap(), stage, player1->state == CS_ALIVE, int(player1->o.x), int(player1->o.y), int(player1->o.z));
    fflush(stdout);
});
COMMANDF(qa_campaign_advance, "", ()
{
    if(!haloactive()) return;
    if(stage == FIRST_CONTACT || stage == SECOND_CONTACT)
    {
        loopv(enemies) if(enemies[i].bot && enemies[i].bot->state == CS_ALIVE)
            dodamage(INT_MAX, enemies[i].bot, player1, GUN_ASSAULT);
        return;
    }
    if(stage == COMPLETE) return;
    const vec target = objective();
    player1->o = target;
    entinmap(player1);
    player1->resetinterp();
});

COMMANDF(qa_campaign_full, "", ()
{
    if(!haloactive())
    {
        addsleep(20, "qa_campaign_full", true);
        return;
    }
    static int ticks = 0; static int lastMission = -1; static int lastSection = -1; static int sectionTicks = 0; static bool sectionReported = false;
    if(mission != lastMission || section != lastSection)
    {
        if(lastMission >= 0 && lastMission == mission && section == lastSection + 1 && !sectionReported)
            conoutf("CAMPAIGN_QA completed via authored transition %d %d", lastMission + 1, lastSection + 1);
        sectionReported = false;
        lastMission = mission;
        lastSection = section;
        sectionTicks = 0;
    }
    if(++ticks > 8000 || ++sectionTicks > 650)
    {
        conoutf("CAMPAIGN_QA stuck %s stage %d marker %d", getclientmap(), stage, nextRouteMarker);
        fflush(stdout);
        execute("quit");
        return;
    }
    if(halocinematic())
    {
        if(sectionTicks >= 10) halocinematicskip();
        addsleep(12, "qa_campaign_full", true);
        return;
    }
    if(stage == COMPLETE)
    {
        conoutf("CAMPAIGN_QA completed %s", getclientmap());
        sectionReported = true;
        fflush(stdout);
        if(campaignFinished) { execute("quit"); return; }
        nextSection();
    }
    else
    {
        if(player1->state == CS_DEAD) resetEncounter();
        const vec target = objective();
        if(mission == 6 && section < 3 && stage == EXTRACTION && nextRouteMarker + 10 >= route[2].length())
        {
            // Put the test player inside the original BSP exit above the lift.
            const vec transition = qaLibraryExit(section);
            player1->o = transition;
            player1->resetinterp();
            addsleep(50, "qa_campaign_full", true);
            return;
        }
        if(stage == FIRST_CONTACT || stage == SECOND_CONTACT)
        {
            loopv(enemies) if(enemies[i].bot && enemies[i].bot->state == CS_ALIVE)
                dodamage(INT_MAX, enemies[i].bot, player1, GUN_ASSAULT);
        }
        else
        {
            player1->o = target;
            entinmap(player1);
            player1->resetinterp();
            if(stage == EXTRACTION && sectionInteraction()) execute("halo_use");
        }
    }
    addsleep(12, "qa_campaign_full", true);
});

COMMANDF(qa_timeout, "", ()
{
    if(haloactive() && driving) escapeDeadline = lastmillis;
});
COMMANDF(qa_maw_route, "", ()
{
    loadProgress();
    queueSection(9, 5);
});
COMMANDF(qa_scenario_state, "", ()
{
    conoutf("RETAIL_QA mission=%d section=%d alive=%d cutscene=%d stage=%d",
        mission, section, player1->state == CS_ALIVE, halocinematic(), stage);
    fflush(stdout);
});

COMMANDF(qa_halo_warp, "fff", (float *x, float *y, float *z)
{
    player1->o = vec(*x, *y, *z);
    player1->vel = vec();
    player1->resetinterp();
});
COMMANDF(qa_halo_feet, "fff", (float *x, float *y, float *z)
{
    player1->o = vec(*x, *y, *z + player1->eyeheight);
    player1->vel = vec();
    player1->resetinterp();
});
COMMANDF(qa_halo_block, "fff", (float *x, float *y, float *z)
{
    const vec old = player1->o;
    player1->o = vec(*x, *y, *z + player1->eyeheight);
    float hi = 127; float lo = -128;
    bool blocked = haloscenariocollide(player1, hi, lo);
    player1->o = old;
    conoutf("DEVICE_QA blocked=%d floor=%.2f", blocked, lo);
    fflush(stdout);
});

COMMANDF(qa_campaign_resume, "ii", (int *m, int *s)
{
    loadProgress();
    queueSection(*m - 1, *s - 1);
});
