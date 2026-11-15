// Soak and asset-robustness hooks (tests/test_soak.py, tests/test_asset_robustness.py).
// Compiled into halo.cpp only for halo-ac-qa. Lines start with SOAK_QA, MEM_QA or ROBUST_QA.
//
// qa_soak PASSES SETTLE loads all 55 campaign sections in order (a10 section 1 to d40 section
// 8, the Crash Site's standalone map as a30's first), PASSES times over, in one process. Each
// section is a section select (queueSection standalone, the mission script started afresh);
// once the section is live (its opening scene skipped) it runs for SETTLE ms with its actors
// and scripts, then a SOAK_QA row reports the map, whether the player is alive and the
// process's memory, and the next section is queued. A section that is not live within 90 s
// (120 s under ASan's slower loads, `qa_soak_timeout`) is reported as `SOAK_QA FAIL timeout`
// and the soak moves on. `SOAK_QA done` and a quit end the run.

bool haloqamemory(unsigned long long &resident, unsigned long long &peak, unsigned long long &footprint);

namespace
{
    struct QaSoakSection { int mission, section; };
    std::vector<QaSoakSection> qaSoakOrder;
    int qaSoakIndex = -1, qaSoakPass = 0, qaSoakPasses = 1, qaSoakSettle = 1500;
    int qaSoakQueued = 0, qaSoakLive = 0, qaSoakStable = 0, qaSoakTimeout = 90000;

    void qaMemoryRow(const char *prefix, const char *label)
    {
        unsigned long long resident = 0, peak = 0, footprint = 0;
        haloqamemory(resident, peak, footprint);
        conoutf("%s %s rss=%llu peak=%llu footprint=%llu millis=%d", prefix, label, resident, peak, footprint, lastmillis);
        fflush(stdout);
    }

    void qaSoakQueue()
    {
        const QaSoakSection &next = qaSoakOrder[qaSoakIndex];
        loadProgress();
        queueSection(next.mission, next.section, true);
        qaSoakQueued = lastmillis;
        qaSoakLive = qaSoakStable = 0;
    }

    // Moves to the next section (or pass); false once every pass is done.
    bool qaSoakAdvance()
    {
        if(++qaSoakIndex >= int(qaSoakOrder.size()))
        {
            defformatstring(label)("pass-end pass=%d", qaSoakPass);
            qaMemoryRow("SOAK_QA", label);
            if(++qaSoakPass > qaSoakPasses) return false;
            qaSoakIndex = 0;
        }
        qaSoakQueue();
        return true;
    }
}

COMMANDF(qa_mem_state, "s", (const char *label) { qaMemoryRow("MEM_QA", label); });

COMMANDF(qa_soak_timeout, "i", (int *millis) { qaSoakTimeout = max(*millis, 10000); });

COMMANDF(qa_soak_poll, "", ()
{
    if(qaSoakIndex < 0) return;
    const QaSoakSection &want = qaSoakOrder[qaSoakIndex];
    string expected;
    halosectionname(want.mission, want.section, expected, sizeof(expected));
    if(halocinematic()) halocinematicskip();
    const bool live = haloactive() && !launchPending && !startPending && !halocinematic() &&
                      mission == want.mission && section == want.section && !strcmp(getclientmap(), expected);
    qaSoakStable = live ? qaSoakStable + 1 : 0;
    if(qaSoakStable == 5) qaSoakLive = lastmillis;
    bool next = false;
    if(qaSoakLive && lastmillis - qaSoakLive >= qaSoakSettle)
    {
        defformatstring(label)("pass=%d index=%d map=%s alive=%d stage=%d load=%d", qaSoakPass, qaSoakIndex, expected,
                               player1->state == CS_ALIVE, int(stage), qaSoakLive - qaSoakQueued);
        qaMemoryRow("SOAK_QA", label);
        next = true;
    }
    // The timeout is for reaching live play; a section already live is settling, not hung.
    else if(!qaSoakLive && lastmillis - qaSoakQueued > qaSoakTimeout)
    {
        conoutf("SOAK_QA FAIL timeout pass=%d index=%d map=%s current=%s", qaSoakPass, qaSoakIndex, expected, getclientmap());
        fflush(stdout);
        next = true;
    }
    if(next && !qaSoakAdvance())
    {
        conoutf("SOAK_QA done");
        fflush(stdout);
        qaSoakIndex = -1;
        execute("quit");
        return;
    }
    addsleep(20, "qa_soak_poll", true);
});

COMMANDF(qa_soak, "ii", (int *passes, int *settle)
{
    qaSoakOrder.clear();
    loopi(haloMissionCount) loopj(haloMissions[i].sections) qaSoakOrder.push_back({i, j});
    qaSoakPasses = max(*passes, 1);
    qaSoakSettle = max(*settle, 0);
    qaSoakPass = 1;
    qaSoakIndex = 0;
    conoutf("SOAK_QA start sections=%d passes=%d settle=%d", int(qaSoakOrder.size()), qaSoakPasses, qaSoakSettle);
    qaMemoryRow("SOAK_QA", "baseline");
    qaSoakQueue();
    addsleep(20, "qa_soak_poll", true);
});

// --- asset robustness ----------------------------------------------------------------------
// qa_robust_state LABEL: what the loaders made of the tables and files, one ROBUST_QA row.
// qa_robust_exercise: drives the paths that read them (fire, zoom and swap the carried
// weapons, throw both grenade types, spawn and enter a Warthog, drive, get out, die and revert
// to the checkpoint), so a table that loaded wrongly is used, not only parsed. Each step is
// 700 ms after the last (the revert 1.3 s after the death); `ROBUST_QA exercised` ends it (about
// 6.7 s). The HUD is drawn every
// frame meanwhile. Robustness means the game logs and falls back; the test checks that the
// process exits cleanly afterwards.

COMMANDF(qa_robust_state, "s", (const char *label)
{
    conoutf("ROBUST_QA %s map=%s active=%d alive=%d hud=%d weapons=%d equipment=%d grenades=%d%d vehicledefs=%d vehicles=%d "
            "actors=%d collision=%d script=%d enemies=%d",
            label, getclientmap(), haloactive(), player1->state == CS_ALIVE, halohudready(), haloweaponcount(),
            haloequipmentcount(), halogrenadedef(0) != nullptr, halogrenadedef(1) != nullptr, halovehicledefcount(),
            halovehiclecount(), haloactordefcount(), halocollisionactive(), haloscenarioprogramhash() != 0, enemies.length());
    fflush(stdout);
});

COMMANDF(qa_robust_exercise, "", ()
{
    if(!haloactive() || player1->state != CS_ALIVE) { conoutf("ROBUST_QA exercise skipped (not in a live section)"); fflush(stdout); return; }
    playerProtected = true;
    attack(true);
    addsleep(500, "qa_robust_step 1", true);
});

COMMANDF(qa_robust_step, "i", (int *step)
{
    // Steps 8 (the revert, 1.3 s after the death: halorespawn waits a second) and 9 (the report)
    // run with the player dead or just respawned.
    if(!haloactive() || (player1->state != CS_ALIVE && *step < 8)) { conoutf("ROBUST_QA exercise stopped at step %d", *step); fflush(stdout); return; }
    switch(*step)
    {
        case 1: attack(false); halozoomstep(); halozoomstep(); haloswitchweapon(); attack(true); break;
        case 2: attack(false); halozoomstep(); haloswitchweapon(); grenadeCount(HALO_GRENADE_FRAG) = 1; grenadeType = HALO_GRENADE_FRAG; halofrag(); break;
        case 3:
            haloclearbolts();
            if(grenadeKnown(HALO_GRENADE_PLASMA)) { grenadeCount(HALO_GRENADE_PLASMA) = 1; grenadeType = HALO_GRENADE_PLASMA; halofrag(); }
            break;
        case 4:
        {
            const int vehicle = halovehicleqaspawn(0, 8);
            conoutf("ROBUST_QA vehicle spawned=%d", vehicle);
            if(vehicle >= 0) halovehicleqaenter(vehicle, 0);
            break;
        }
        case 5: halovehicleqainput(1, 0.3f, 800); break;
        case 6: if(halodriving()) halovehicleuse(0); break;
        case 7: playerProtected = false; dodamage(1000, player1, player1, GUN_ASSAULT); break;
        case 8: tryrespawn(); break;
        default: conoutf("ROBUST_QA exercised"); fflush(stdout); return;
    }
    defformatstring(next)("qa_robust_step %d", *step + 1);
    addsleep(*step == 7 ? 1300 : 700, next, true);
});
