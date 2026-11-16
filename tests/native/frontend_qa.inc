// Title menu, saved games and nav points (tests/test_frontend_progress.py, tests/test_nav_points.py).
// Compiled into halo.cpp only for halo-ac-qa (HALO_QA). Written while testing was on hold: not yet run.

// QA_STATE: the version 3 state block's header and how many records of each kind it holds.
static void qaStateSummary(const std::string &state)
{
    int records[4] = {};
    const char *kinds = "svwp";
    for(size_t at = 0; at < state.size();)
    {
        const char *kind = at + 1 < state.size() && state[at + 1] == ' ' && state[at] ? strchr(kinds, state[at]) : NULL;
        if(kind) ++records[kind - kinds];
        const size_t eol = state.find('\n', at);
        at = eol == std::string::npos ? state.size() : eol + 1;
    }
    const size_t eol = state.find('\n');
    conoutf("QA_STATE header=[%s] script=%d vehicles=%d world=%d player=%d bytes=%d",
        state.substr(0, eol == std::string::npos ? 0 : eol).c_str(), records[0], records[1], records[2], records[3], int(state.size()));
    fflush(stdout);
}

// QA_HS_STATE: the mission script's clock, started threads and visible objects, and the recovery's safe floors.
static void qaScriptState(const char *name)
{
    int tick = 0, started = 0, visible = 0;
    haloscenarioqastate(tick, started, visible);
    conoutf("QA_HS_STATE %s tick=%d started=%d visible=%d safe=%d grace=%d atcheckpoint=%d seated=%d", name, tick, started, visible,
        safeSpots.length(), inMoveGrace(), atCheckpoint, halodriving());
    fflush(stdout);
}
COMMANDF(qa_hs_state, "s", (const char *name) { qaScriptState(name); });

// QA_PROGRESS: what the engine holds of config/halo-progress.txt.
COMMANDF(qa_progress_state, "", ()
{
    loadProgress();
    const SavedGame &g = savedGame;
    // (No top-level commas in COMMANDF bodies: the macro would split there.)
    const HaloWeaponDef *w0 = haloweapon(g.loadout.carried[0].def);
    const HaloWeaponDef *w1 = haloweapon(g.loadout.carried[1].def);
    conoutf("QA_PROGRESS furthest=%d,%d beaten=%d game=%d at=%d,%d difficulty=%d checkpoint=%d pos=%.2f,%.2f,%.2f stage=%d marker=%d "
            "health=%d shield=%.1f frags=%d plasmas=%d current=%d w0=%s:%d/%d w1=%s:%d/%d escape=%d unlocked=%d",
        savedMission, savedSection, campaignBeaten, g.valid, g.mission, g.section, g.difficulty, g.checkpoint,
        g.position.x, g.position.y, g.position.z, g.stage, g.marker, g.loadout.health, g.loadout.shield,
        g.loadout.frags, g.loadout.plasmas, g.loadout.current,
        w0 ? w0->label.c_str() : g.weaponPath[0], g.loadout.carried[0].loaded, g.loadout.carried[0].reserve,
        w1 ? w1->label.c_str() : g.weaponPath[1], g.loadout.carried[1].loaded, g.loadout.carried[1].reserve,
        g.escapeMillis, halounlockedmissions());
    // On a line of its own: conoutf cuts its lines at MAXSTRLEN (260), which the line above and
    // a long warning together would pass.
    conoutf("QA_PROGRESS_NOTE [%s]", progressNote);
    qaStateSummary(g.state);
});

// qa_hs_global NAME prints a script global; qa_hs_global NAME VALUE sets it first.
COMMANDF(qa_hs_global, "ss", (const char *name, const char *value)
{
    const bool set = value && *value;
    conoutf("QA_HS_GLOBAL %s %.4f", name, haloscenarioqaglobal(name, set, set ? atof(value) : 0));
    fflush(stdout);
});

// Forgets what was loaded and reads the progress file again (after a test rewrote it).
COMMANDF(qa_progress_reload, "", () { progressLoaded = false; loadProgress(); });

// QA_LOADOUT: what the player carries now.
COMMANDF(qa_loadout_state, "s", (const char *name)
{
    syncCarried();
    const HaloWeaponDef *w0 = haloweapon(carried[0].def);
    const HaloWeaponDef *w1 = haloweapon(carried[1].def);
    conoutf("QA_LOADOUT %s map=%s stage=%d checkpoint=%d pos=%.2f,%.2f,%.2f health=%d shield=%.1f frags=%d plasmas=%d type=%d current=%d "
            "w0=%s:%d/%d w1=%s:%d/%d difficulty=%d unarmed=%d gun=%d",
        name, getclientmap(), stage, atCheckpoint, player1->o.x, player1->o.y, player1->o.z, player1->health, shield,
        grenadeCount(HALO_GRENADE_FRAG), plasmaGrenades, grenadeType, currentCarried,
        w0 ? w0->label.c_str() : "-", carried[0].loaded, carried[0].reserve,
        w1 ? w1->label.c_str() : "-", carried[1].loaded, carried[1].reserve, halodifficulty(), unarmed(),
        player1->weaponsel ? player1->weaponsel->type : -1);
    fflush(stdout);
});

// QA_PROFILE: the loaded mission script's player starting profiles (.hscb version 9), one line
// each (weapons by label, `-` for none), then a count (0 with an older .hscb).
COMMANDF(qa_profile_state, "", ()
{
    int count = 0;
    HaloStartingProfile p;
    for(; haloscenarioprofile(count, NULL, p); ++count)
    {
        const HaloWeaponDef *w0 = p.weapon[0].empty() ? NULL : haloweaponfind(p.weapon[0].c_str());
        const HaloWeaponDef *w1 = p.weapon[1].empty() ? NULL : haloweaponfind(p.weapon[1].c_str());
        conoutf("QA_PROFILE %d name=%s health=%.2f shield=%.2f w0=%s:%d/%d w1=%s:%d/%d frags=%d plasmas=%d", count,
                p.name.empty() ? "-" : p.name.c_str(), p.health, p.shield, w0 ? w0->label.c_str() : "-", p.loaded[0], p.total[0],
                w1 ? w1->label.c_str() : "-", p.loaded[1], p.total[1], p.frags, p.plasmas);
    }
    conoutf("QA_PROFILE count=%d", count);
    fflush(stdout);
});

// Sets the loadout to something recognisable: frag and plasma counts and the carried rounds.
COMMANDF(qa_loadout_mark, "iiii", (int *frags, int *plasmas, int *loaded, int *reserve)
{
    grenadeCount(HALO_GRENADE_FRAG) = *frags;
    plasmaGrenades = *plasmas;
    syncCarried();
    // An unarmed start (a10's starting profile) gets the default assault rifle in the selected slot
    // first, so the rounds mark a weapon.
    if(tagWeapons() && !slotArmed(currentCarried))
    {
        carried[currentCarried].reset(haloweaponfind(defaultWeapons[0]));
        loopi(NUMGUNS) if(i != GUN_GRENADE && i != GUN_KNIFE) player1->mag[i] = player1->ammo[i] = 0;
        equipCarried(true);
    }
    carried[currentCarried].loaded = *loaded;
    carried[currentCarried].reserve = *reserve;
    if(tagWeapons()) applyCarried(currentCarried);
});

// QA_TITLE: the title screen, the open menu and what the menus would offer.
COMMANDF(qa_title_state, "", ()
{
    loadProgress();
    extern gmenu *curmenu;
    conoutf("QA_TITLE title=%d active=%d menu=%s continue=%d unlocked=%d note=[%s]",
        halotitleactive(), haloactive(), curmenu && curmenu->name ? curmenu->name : "-",
        savedGame.valid && installed(savedGame.mission, savedGame.section), halounlockedmissions(), progressNote);
    fflush(stdout);
});

// QA_NAV: the script's nav points (world position and arrow type) and the HUD parts it hides or blinks.
COMMANDF(qa_nav_state, "s", (const char *name)
{
    HaloHudNav navs[16];
    const int count = haloscenarionavpoints(navs, 16);
    int blinking = 0;
    const int hidden = haloscenariohudhidden(&blinking);
    conoutf("QA_NAV %s count=%d hidden=%d blinking=%d", name, count, hidden, blinking);
    loopi(count)
    {
        vec4 clip;
        mvpmatrix.transform(vec(navs[i].x, navs[i].y, navs[i].z), clip);
        conoutf("QA_NAV_POINT %s %d type=%s at=%.2f,%.2f,%.2f dist=%.2f front=%d",
            name, i, navs[i].type, navs[i].x, navs[i].y, navs[i].z, player1->o.dist(vec(navs[i].x, navs[i].y, navs[i].z)), clip.w > 0.01f);
    }
    fflush(stdout);
});

// The stood-members change (tests/test_frontend_progress.py item 16, not yet run): SQUAD_STOOD per
// living squad member (where it stands, health, shield, riding) and SQUAD_STOOD_STATE with the
// last revert's counts; qa_squad_hurt HEALTH SHIELD sets every living non-attached member's fractions.
COMMANDF(qa_squad_stood, "s", (const char *label) { haloscenarioqasquads(label); });
COMMANDF(qa_squad_hurt, "ff", (float *health, float *shield) { haloscenarioqasquadhurt(*health, *shield); });

// Takes a scripted checkpoint where the player stands, as game_save does once it is safe (and
// saves it to the progress file).
COMMANDF(qa_checkpoint_save, "", () { if(haloactive() && player1->state == CS_ALIVE) saveCheckpoint(true); });
