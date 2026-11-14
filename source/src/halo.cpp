#include "cube.h"
#include "bot/bot.h"
#include "halo.h"
#include "halo_collision.h"
#include "halo_script.h"
#include "halo_campaign.h"

extern void startgame(const char *name, int mode, int minutes, bool notify);

namespace
{
    const char *const progressFile = "config/halo-progress.txt";
    const int noMission = -1;
    int mission = noMission, section = 0;
    int pendingMission = 0, pendingSection = 0;
    bool preview = false, pendingPreview = false;
    bool requestedSection = false, pendingContinuation = false;
    int savedMission = 0, savedSection = 0;
    bool progressLoaded = false;
    bool campaignFinished = false, authoredTransition = false;
    bool driving = false;
    int escapeDeadline = 0, incomingEscapeTime = 0;
    const int escapeTime = 180000;
    const int playerShield = 75, playerHealth = 75;
    const int shieldDelay = 6000, shieldRecharge = 4000;
    const int eliteDelay = 12000, eliteRecharge = 10000;
    const float markerRadius = 8.0f, firstContactRadius = 62.0f;

    enum MissionStage { INSERTION, FIRST_CONTACT, REACH_PASS, SECOND_CONTACT, EXTRACTION, COMPLETE };
    enum ActorKind { GRUNT, ELITE, JACKAL, HUNTER, FLOOD_ELITE, FLOOD_HUMAN, INFECTION, CARRIER, SENTINEL, ACTOR_COUNT };

    struct ActorProfile
    {
        const char *name, *model;
        int health, shield, delay, damage;
        float eyeheight, radius;
        bool melee;
    };

    const ActorProfile profiles[ACTOR_COUNT] = {
        {"Grunt", "halo/grunt", 40, 0, 400, 5, 2.6f, 0.9f, false},
        {"Elite", "halo/elite", 100, 100, 180, 8, 4.0f, 1.1f, false},
        {"Jackal", "halo/jackal", 50, 45, 420, 6, 3.3f, 0.9f, false},
        {"Hunter", "halo/hunter", 240, 0, 960, 26, 5.6f, 1.4f, false},
        {"Elite Flood", "halo/flood-elite", 75, 0, 265, 9, 4.0f, 1.1f, false},
        {"Human Flood", "halo/flood-human", 65, 0, 270, 9, 3.7f, 1.1f, false},
        {"Infection Form", "halo/infection", 15, 0, 550, 18, 1.0f, 0.45f, true},
        {"Carrier Form", "halo/carrier", 50, 0, 900, 36, 2.7f, 0.95f, true},
        {"Sentinel", "halo/sentinel", 65, 0, 225, 7, 3.8f, 0.95f, false},
    };

    struct HaloEnemy
    {
        botent *bot;
        ActorKind kind;
        float shield;
        int hitTime;
    };

    bool active = false, startPending = false, launchPending = false, controlsLoaded = false;
    MissionStage stage = INSERTION;
    int insertion = -1, checkpoint = -1, extraction = -1;
    int shieldHit = 0;
    bool shieldRecharging = false;
    float shield = playerShield;
    bool atCheckpoint = false;
    int checkpointHealth = playerHealth, checkpointWeapon = GUN_ASSAULT;
    int checkpointMag[NUMGUNS], checkpointAmmo[NUMGUNS];
    vector<uchar> checkpointItems;
    vec checkpointPosition;
    float checkpointYaw = 0;
    MissionStage checkpointStage = SECOND_CONTACT;
    int checkpointMarker = 0;
    vector<int> waveStarts[2], route[3];
    int nextRouteMarker = 0;
    vector<HaloEnemy> enemies;

    vec position(const entity &e) { return vec(e.x, e.y, e.z); }
    bool near(int i, float radius)
    {
        const entity &e = ents[i];
        return fabsf(player1->o.z - e.z) < 14.0f && player1->o.distxy(position(e)) <= radius;
    }

    HaloEnemy *enemyState(const playerent *p)
    {
        loopv(enemies) if(enemies[i].bot == p) return &enemies[i];
        return NULL;
    }

    void campaignWeapon(playerent *p, int gun, int mag, int delay, int reload, int damage, int spread)
    {
        guninfo &info = p->weapons[gun]->info;
        info.magsize = mag;
        info.attackdelay = delay;
        info.reloadtime = reload;
        info.damage = damage;
        info.spread = spread;
        info.recoil = 3;
        info.mdl_kick_back = info.mdl_kick_rot = 0;
        info.recoilincrease = 32;
        info.recoilbase = 3;
        info.maxrecoil = 8;
    }

    void resetLoadout()
    {
        campaignWeapon(player1, GUN_ASSAULT, 60, 67, 3400, 10, 60);
        campaignWeapon(player1, GUN_PISTOL, 12, 286, 2170, 25, 14);
        player1->setnextprimary(GUN_ASSAULT);
        player1->setprimary(GUN_ASSAULT);
        player1->selectweapon(atCheckpoint ? checkpointWeapon : GUN_ASSAULT);
        player1->nextweaponsel = player1->weaponsel;
        player1->weaponchanging = 0;
        player1->health = atCheckpoint ? checkpointHealth : playerHealth;
        player1->armour = 0;
        player1->mag[GUN_ASSAULT] = 60;
        player1->ammo[GUN_ASSAULT] = 180;
        player1->mag[GUN_PISTOL] = 12;
        player1->ammo[GUN_PISTOL] = 48;
        player1->mag[GUN_GRENADE] = 2;
        if(atCheckpoint)
        {
            memcpy(player1->mag, checkpointMag, sizeof(checkpointMag));
            memcpy(player1->ammo, checkpointAmmo, sizeof(checkpointAmmo));
        }
        shield = playerShield;
        shieldHit = 0;
        shieldRecharging = false;
    }

    void saveCheckpoint(bool scripted = false)
    {
        atCheckpoint = true;
        checkpointStage = scripted ? stage : SECOND_CONTACT;
        checkpointMarker = scripted ? nextRouteMarker : 0;
        checkpointPosition = scripted ? player1->o : position(ents[checkpoint]);
        checkpointYaw = scripted ? player1->yaw : ents[checkpoint].attr1 / 10.0f;
        checkpointHealth = player1->health;
        checkpointWeapon = player1->weaponsel->type;
        memcpy(checkpointMag, player1->mag, sizeof(checkpointMag));
        memcpy(checkpointAmmo, player1->ammo, sizeof(checkpointAmmo));
        checkpointItems.setsize(0);
        loopv(ents) checkpointItems.add(ents[i].spawned);
        haloscenariocheckpoint();
        conoutf("Halo: checkpoint reached");
    }

    botent *spawnEnemy(ActorKind kind, const vec &origin, float yaw)
    {
        const ActorProfile &profile = profiles[kind];
        defformatstring(name)("%.12s %d", profile.name, enemies.length() + 1);
        botent *bot = BotManager.CreateBot("CLA", kind == ELITE ? "worse" : "bad", name);
        if(!bot) return nullptr;
        enemies.add(HaloEnemy{bot, kind, float(profile.shield), lastmillis});
        bot->health = profile.health;
        bot->maxeyeheight = bot->eyeheight = profile.eyeheight;
        bot->radius = profile.radius;
        bot->armour = 0;
        bot->o = origin;
        bot->yaw = yaw;
        bot->pitch = bot->roll = 0;
        entinmap(bot);
        bot->targetyaw = bot->yaw;
        const int gun = profile.melee ? GUN_KNIFE : GUN_SUBGUN;
        bot->setprimary(gun);
        bot->setnextprimary(gun);
        bot->selectweapon(gun);
        bot->nextweaponsel = bot->weaponsel;
        bot->weaponchanging = 0;
        loopj(NUMGUNS) bot->mag[j] = bot->ammo[j] = 0;
        campaignWeapon(bot, gun, 100, profile.delay, 0, profile.damage, kind == ELITE ? 36 : 56);
        bot->mag[gun] = 100;
        return bot;
    }

    void spawnWave(int wave)
    {
        loopv(waveStarts[wave])
        {
            entity &start = ents[waveStarts[wave][i]];
            const ActorKind kind = start.attr4 < ACTOR_COUNT ? ActorKind(start.attr4) : GRUNT;
            spawnEnemy(kind, position(start), start.attr1 / 10.0f);
        }
        if(!waveStarts[wave].empty()) conoutf("Halo: encounter %d", wave + 1);
    }

    int remaining()
    {
        int count = 0;
        loopv(enemies) if(enemies[i].bot && enemies[i].bot->state == CS_ALIVE) ++count;
        return count;
    }

    vec contactPoint()
    {
        if(waveStarts[0].empty()) return position(ents[checkpoint]);
        vec midpoint;
        loopv(waveStarts[0]) midpoint.add(position(ents[waveStarts[0][i]]));
        return midpoint.div(waveStarts[0].length());
    }

    void setStage(MissionStage next)
    {
        stage = next;
        nextRouteMarker = 0;
        if(next == FIRST_CONTACT) spawnWave(0);
        if(next == SECOND_CONTACT) spawnWave(1);
        if(next == COMPLETE)
        {
            player1->attacking = false;
            player1->stopmoving();
            intermission = true;
            conoutf("Halo: mission complete");
        }
    }

    bool escapeRun() { return mission == 9 && section >= 5; }
    bool retailCartographer() { return mission == 3 && halocollisionactive(); }

    const char *sectionInteraction()
    {
        if(preview) return NULL;
        if(mission == 2 && section == 3) return "Free Captain Keyes";
        if(mission == 3 && section == 1) return "Access the Cartographer";
        if(mission == 3 && section == 2 && halocollisionactive()) return "Board the Pelican";
        if(mission == 4 && section == 6) return "Access the control room";
        if(mission == 5 && section == 4) return "Investigate the containment lab";
        if(mission == 6 && section == 3) return "Retrieve the Index";
        if(mission == 7 && section == 4) return "Disable the final pulse generator";
        if(mission == 8 && section == 4) return "Recover the Captain's implants";
        if(mission == 9 && section == 4) return "Sabotage the reactor controls";
        return NULL;
    }

    void finishSection(bool completedScene = false)
    {
        if(!completedScene && haloscenarioexit()) return;
        setStage(COMPLETE);
        campaignFinished = !preview && mission == haloMissionCount - 1 && section == haloMissions[mission].sections - 1;
    }

    void resetEncounter()
    {
        haloclearbolts();
        enemies.setsize(0);
        kickallbots();
        clearbounceents();
        showscores(false);
        player1->team = TEAM_RVSF;
        spawnplayer(player1, false);
        gotoplayerstart(player1, &ents[insertion]);
        if(atCheckpoint)
        {
            player1->o = checkpointPosition;
            player1->yaw = checkpointYaw;
        }
        else if(halocollisionactive()) player1->o.z += player1->eyeheight;
        entinmap(player1);
        player1->resetinterp();
        resetLoadout();
        driving = escapeRun();
        player1->maxspeed = driving ? 36.0f : 16.0f;
        escapeDeadline = driving ? lastmillis + (incomingEscapeTime > 0 ? incomingEscapeTime : escapeTime) : 0;
        incomingEscapeTime = 0;
        loopv(ents) if(isitem(ents[i].type))
        {
            bool available = !atCheckpoint || (checkpointItems.inrange(i) && checkpointItems[i]);
            setpickupspawn(i, available);
            ents[i].lastmillis = 0;
        }
        intermission = false;
        if(driving) setStage(EXTRACTION);
        else if(atCheckpoint)
        {
            setStage(checkpointStage);
            nextRouteMarker = checkpointMarker;
        }
        else setStage(halocollisionactive() ? EXTRACTION : INSERTION);
    }

    void loadProgress()
    {
        if(progressLoaded) return;
        progressLoaded = true;
        FILE *file = fopen(findfile(progressFile, "rb"), "rb");
        if(!file) return;
        int version = 0, m = 0, s = 0;
        if(fscanf(file, "%d %d %d", &version, &m, &s) == 3 && version == 1 &&
           m >= 0 && m < haloMissionCount && s >= 0 && s < haloMissions[m].sections)
        {
            savedMission = m;
            savedSection = s;
        }
        fclose(file);
    }

    void saveProgress()
    {
        string destination, temporary;
        copystring(destination, findfile(progressFile, "wb"));
        formatstring(temporary)("%s.tmp", destination);
        FILE *file = fopen(temporary, "wb");
        if(!file) { conoutf("Halo: unable to open campaign progress"); return; }
        bool written = fprintf(file, "1 %d %d\n", savedMission, savedSection) > 0;
        if(fclose(file) != 0 || !written || rename(temporary, destination) != 0)
        {
            remove(temporary);
            conoutf("Halo: unable to save campaign progress");
        }
    }

    bool installed(int m, int s)
    {
        string name;
        halosectionname(m, s, name, sizeof(name));
        defformatstring(path)("packages/maps/%s.cgz", name);
        return fileexists(findfile(path, "rb"), "rb");
    }

    void queueSection(int m, int s, bool standalone = false)
    {
        if(multiplayer("halo_campaign")) return;
        if(!installed(m, s))
        {
            conoutf("Halo: import %s, section %d to continue", haloMissions[m].title, s + 1);
            return;
        }
        closemenu(NULL);
        incomingEscapeTime = 0;
        pendingMission = m;
        pendingSection = s;
        pendingPreview = standalone;
        pendingContinuation = false;
        launchPending = true;
    }

    void nextSection()
    {
        if(!haloactive() || stage != COMPLETE || preview || campaignFinished) return;
        int m = mission, s = section + 1;
        if(s == haloMissions[m].sections) { ++m; s = 0; }
        if(m >= haloMissionCount) return;
        int remainingTime = driving ? max(1, escapeDeadline - lastmillis) : 0;
        queueSection(m, s);
        if(launchPending && m == mission)
        {
            incomingEscapeTime = remainingTime;
            pendingContinuation = true;
        }
    }

    void shieldTick(float &value, float capacity, int hit, int delay, int recharge, int elapsed)
    {
        int regenTime = min(elapsed, lastmillis - hit - delay);
        if(value < capacity && regenTime > 0) value = min(capacity, value + capacity * regenTime / recharge);
    }

    void box(float x, float y, float w, float h, int r, int g, int b, int a)
    {
        glColor4ub(r, g, b, a);
        glBegin(GL_TRIANGLE_STRIP);
        glVertex2f(x, y); glVertex2f(x+w, y);
        glVertex2f(x, y+h); glVertex2f(x+w, y+h);
        glEnd();
    }

    void ring(float x, float y, float radius, int r, int g, int b, int a)
    {
        glColor4ub(r, g, b, a);
        glBegin(GL_LINE_LOOP);
        loopi(40)
        {
            float angle = i * PI2 / 40.0f;
            glVertex2f(x + cosf(angle)*radius, y + sinf(angle)*radius);
        }
        glEnd();
    }

    vec objective()
    {
        vec cartographer;
        bool mapped;
        if(halocartographertarget(cartographer, mapped)) return cartographer;
        if(retailCartographer()) return position(ents[extraction]);
        if(stage == INSERTION || stage == REACH_PASS)
        {
            const vector<int> &markers = route[stage == REACH_PASS];
            if(markers.inrange(nextRouteMarker)) return position(ents[markers[nextRouteMarker]]);
            if(stage == INSERTION) return contactPoint();
            return position(ents[checkpoint]);
        }
        if(stage == EXTRACTION)
        {
            int marker = nextRouteMarker;
            if(driving) for(int i = 0; i < 2; ++i)
            {
                if(route[i].inrange(marker)) return position(ents[route[i][marker]]);
                marker -= route[i].length();
            }
            if(route[2].inrange(marker)) return position(ents[route[2][marker]]);
            return position(ents[extraction]);
        }
        vec closest = position(ents[stage == FIRST_CONTACT ? checkpoint : extraction]);
        float distance = 1e16f;
        loopv(enemies)
        {
            botent *bot = enemies[i].bot;
            if(!bot || bot->state != CS_ALIVE) continue;
            float d = player1->o.squareddist(bot->o);
            if(d < distance) { closest = bot->o; distance = d; }
        }
        return closest;
    }

    void label(const char *text, int x, int y, float scale = 0.56f, bool alignRight = false)
    {
        glPushMatrix();
        glTranslatef(x, y, 0);
        glScalef(scale, scale, 1);
        draw_text(text, alignRight ? -text_width(text) : 0, 0, 146, 231, 250, 245);
        glPopMatrix();
    }
}

bool haloactive() { return active && gamemode == GMODE_BOTTEAMDEATHMATCH && !multiplayer(NULL); }

void halomapstart(const char *name)
{
    haloscenariostop();
    halocollisionmap(nullptr);
    authoredTransition = false;
    active = startPending = driving = false;
    player1->maxspeed = 16.0f;
    haloclearbolts();
    enemies.setsize(0);
    loopi(NUMGUNS) player1->weapons[i]->info = guns[i];
    if(gamemode != GMODE_BOTTEAMDEATHMATCH || multiplayer(NULL) || !haloparsemap(behindpath(name), mission, section)) return;
    if(!requestedSection) preview = true;
    insertion = checkpoint = extraction = -1;
    loopi(2) waveStarts[i].setsize(0);
    loopi(3) route[i].setsize(0);
    loopv(ents)
    {
        const entity &e = ents[i];
        if(e.type != PLAYERSTART) continue;
        switch(e.attr2)
        {
            case TEAM_RVSF:
                if(insertion < 0) insertion = i;
                else if(checkpoint < 0) checkpoint = i;
                break;
            case TEAM_CLA:
                waveStarts[e.attr3 == 1].add(i);
                break;
            case 100:
                if(e.attr3 >= 1 && e.attr3 <= 3) route[e.attr3 - 1].add(i);
                else if(e.attr3 == 0) extraction = i;
                break;
        }
    }
    if(insertion < 0 || checkpoint < 0 || extraction < 0)
    {
        conoutf("Halo: incomplete campaign section markers");
        return;
    }
    atCheckpoint = false;
    active = startPending = true;
    halocollisionmap(behindpath(name));
    stage = INSERTION;
    checkpointItems.setsize(0);
}

playerent *haloscenariospawnenemy(int kind, const vec &point, float yaw)
{
    if(kind < 0 || kind >= ACTOR_COUNT || !haloactive() || halocinematic() || editmode) return nullptr;
    if(halocollisionactive())
    {
        const ActorProfile &profile = profiles[kind];
        HaloCollisionHit hit;
        if(!halocollisionsweep(vec(point).add(vec(0, 0, 1.26f)), vec(0, 0, -4.26f), profile.radius,
                               profile.eyeheight + .7f, hit) || hit.normal.z < .65f) return nullptr;
        return spawnEnemy(ActorKind(kind), vec(point).add(vec(0, 0, profile.eyeheight)), yaw);
    }
    const int x = int(point.x), y = int(point.y);
    if(OUTBORD(x, y) || SOLID(S(x, y)) || fabsf(S(x, y)->floor - point.z) > 5.0f) return nullptr;
    return spawnEnemy(ActorKind(kind), point, yaw);
}

void haloscenarioplayersave()
{
    if(haloactive() && player1->state == CS_ALIVE && !driving && stage != FIRST_CONTACT &&
       stage != SECOND_CONTACT && stage != COMPLETE) saveCheckpoint(true);
}

bool halorespawn()
{
    if(!haloactive()) return false;
    if(player1->state == CS_DEAD && lastmillis - player1->lastdeath >= 1000)
    {
        resetEncounter();
        haloscenariorevert();
        resetcamera();
    }
    return true;
}

bool haloscenariotransition()
{
    if(!haloactive() || (preview && !halocollisionactive()) || authoredTransition || launchPending || halocinematic() ||
       stage == COMPLETE || section + 1 >= haloMissions[mission].sections) return false;
    if(!installed(mission, section + 1)) return false;
    authoredTransition = true;
    finishSection();
    return true;
}

void haloupdate(int elapsed)
{
    if(launchPending && getclientnum() >= 0)
    {
        launchPending = false;
        preview = pendingPreview;
        campaignFinished = false;
        string name;
        halosectionname(pendingMission, pendingSection, name, sizeof(name));
        requestedSection = true;
        startgame(name, GMODE_BOTTEAMDEATHMATCH, 60, true);
        requestedSection = false;
        if(haloactive() && !controlsLoaded) controlsLoaded = execfile("config/halo.cfg");
    }
    if(!haloactive() || editmode) return;
    if(ispaused || elapsed <= 0)
    {
        haloscenariopausestate(ispaused || menuvisible());
        return;
    }
    if(startPending)
    {
        if(getclientnum() < 0) return;
        startPending = false;
        resetEncounter();
        resetcamera();
        conoutf("Halo: %s, section %d", haloMissions[mission].title, section + 1);
        haloscenariostart(mission, section, preview, pendingContinuation);
        if(!preview && (mission > savedMission || (mission == savedMission && section > savedSection)))
        {
            savedMission = mission;
            savedSection = section;
            saveProgress();
        }
    }
    haloscenariotick(elapsed);
    if(haloscenariocomplete()) finishSection(true);
    if(authoredTransition && stage == COMPLETE)
    {
        nextSection();
        return;
    }
    if(halocinematic() || player1->state != CS_ALIVE || stage == COMPLETE) return;
    if(driving && lastmillis >= escapeDeadline)
    {
        if(haloscenarioescapefailed() || !haloscenarioescapetimeout())
            dodamage(INT_MAX, player1, player1, GUN_GRENADE);
        return;
    }
    if(!retailCartographer() && (stage == INSERTION || stage == REACH_PASS || stage == EXTRACTION))
    {
        const vector<int> &markers = route[stage == INSERTION ? 0 : stage == REACH_PASS ? 1 : 2];
        if(driving)
        {
            const vec target = objective();
            if(player1->o.distxy(target) <= 13.0f && nextRouteMarker < route[0].length() + route[1].length() + route[2].length())
                ++nextRouteMarker;
        }
        else if(markers.inrange(nextRouteMarker) && near(markers[nextRouteMarker], 13.0f)) ++nextRouteMarker;
    }
    if(shield < playerShield && !shieldRecharging && lastmillis - shieldHit >= shieldDelay)
    {
        haloplaysound("shield-charge", S_NULL);
        shieldRecharging = true;
    }
    shieldTick(shield, playerShield, shieldHit, shieldDelay, shieldRecharge, elapsed);
    loopv(enemies)
    {
        HaloEnemy &enemy = enemies[i];
        if(profiles[enemy.kind].shield && enemy.bot && enemy.bot->state == CS_ALIVE)
            shieldTick(enemy.shield, profiles[enemy.kind].shield, enemy.hitTime, eliteDelay, eliteRecharge, elapsed);
    }
    if(retailCartographer()) return;
    switch(stage)
    {
        case INSERTION:
            if(!route[0].inrange(nextRouteMarker) && player1->o.distxy(contactPoint()) <= firstContactRadius) setStage(FIRST_CONTACT);
            break;
        case FIRST_CONTACT:
            if(!remaining()) setStage(REACH_PASS);
            break;
        case REACH_PASS:
            if(!route[1].inrange(nextRouteMarker) && near(checkpoint, markerRadius)) { saveCheckpoint(); setStage(SECOND_CONTACT); }
            break;
        case SECOND_CONTACT:
            if(!remaining()) setStage(EXTRACTION);
            break;
        case EXTRACTION:
            if(nextRouteMarker >= route[2].length() + (driving ? route[0].length() + route[1].length() : 0) &&
               near(extraction, markerRadius) && !sectionInteraction()) finishSection();
            break;
        case COMPLETE: break;
    }
}

void halodamage(int &damage, playerent *target, playerent *attacker, int gun)
{
    if(damage <= 0 || damage == INT_MAX) return;
    if(target == player1)
    {
        shieldHit = lastmillis;
        shieldRecharging = false;
        const bool hadShield = shield > 0;
        int factor = attacker != player1 && gun == GUN_SUBGUN ? 2 : 1;
        float absorbed = min(shield, float(damage * factor));
        shield -= absorbed;
        if(hadShield) haloplaysound(shield > 0 ? "shield-hit" : "shield-depleted", S_NULL);
        damage = max(0, int(ceilf(damage - absorbed / factor - 0.001f)));
        return;
    }
    HaloEnemy *enemy = enemyState(target);
    if(!enemy || profiles[enemy->kind].shield == 0) return;
    enemy->hitTime = lastmillis;
    float modifier = gun == GUN_ASSAULT ? 0.7f : 1.0f;
    float absorbed = min(enemy->shield, damage * modifier);
    if(absorbed > 0)
    {
        enemy->shield -= absorbed;
        damage = max(0, int(ceilf(damage - absorbed / modifier - 0.001f)));
        particle_splash(PART_PLASMA, 6, 220, target->o);
    }
}

bool halopickup(int index, playerent *p)
{
    if(!haloactive()) return false;
    if(p != player1) return true;
    entity &e = ents[index];
    int &ammo = p->ammo[GUN_ASSAULT], &clips = p->ammo[GUN_PISTOL], &grenades = p->mag[GUN_GRENADE];
    switch(e.type)
    {
        case I_HEALTH:
            if(p->health == playerHealth) return true;
            p->health = playerHealth;
            break;
        case I_AMMO:
            if(ammo >= 600) return true;
            ammo = min(ammo + 60, 600);
            break;
        case I_CLIPS:
            if(clips >= 120) return true;
            clips = min(clips + 12, 120);
            break;
        case I_GRENADE:
            if(grenades >= 4) return true;
            ++grenades;
            break;
        default: return true;
    }
    e.spawned = false;
    itemstat *stats = p->itemstats(e.type);
    if(stats) audiomgr.playsound(stats->sound, SP_HIGH);
    return true;
}

const char *haloactormodel(const playerent *p)
{
    if(!haloactive()) return NULL;
    if(p == player1) return "halo/cyborg";
    HaloEnemy *enemy = enemyState(p);
    if(!enemy) return NULL;
    return profiles[enemy->kind].model;
}

const char *halohudmodel(int gun)
{
    if(!haloactive()) return NULL;
    if(gun == GUN_ASSAULT) return "halo/ma5b";
    if(gun == GUN_PISTOL) return "halo/pistol";
    return NULL;
}

void halodrawhud()
{
    const char *const objectives[] = {
        "Follow the distress beacon", "Clear the Covenant patrol", "Reach the pass",
        "Clear the extraction zone", "Reach the extraction beacon", "Mission complete"
    };
    const int right = VIRTW - 74, left = 74;
    const float trackerX = 205, trackerY = VIRTH - 210, trackerRadius = 124;
    const float yaw = player1->yaw * RAD;
    const vec target = objective();
    vec bearing = vec(target).sub(player1->o);
    bearing.rotate_around_z(-yaw);

    glDisable(GL_TEXTURE_2D);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    if(lastmillis - shieldHit < 230 && shieldHit > 0)
    {
        int alpha = (230 - (lastmillis-shieldHit))/3;
        box(0, 0, VIRTW, 25, 70, 168, 255, alpha);
        box(0, VIRTH-25, VIRTW, 25, 70, 168, 255, alpha);
        box(0, 0, 25, VIRTH, 70, 168, 255, alpha);
        box(VIRTW-25, 0, 25, VIRTH, 70, 168, 255, alpha);
    }
    box(right-318, 72, 318, 120, 7, 24, 44, 160);
    box(right-299, 123, 278, 16, 18, 60, 81, 210);
    box(right-299, 123, 278 * shield / playerShield, 16, 91, 216, 255, 235);
    loopi(8) box(right-299 + i*35, 151, 30, 13,
        player1->health * 8 > i * playerHealth ? 114 : 24,
        player1->health * 8 > i * playerHealth ? 217 : 61,
        player1->health * 8 > i * playerHealth ? 232 : 86, 225);

    glColor4ub(8, 30, 48, 164);
    glBegin(GL_TRIANGLE_FAN);
    glVertex2f(trackerX, trackerY);
    loopi(41)
    {
        float angle = i * PI2 / 40.0f;
        glVertex2f(trackerX + cosf(angle)*trackerRadius, trackerY + sinf(angle)*trackerRadius);
    }
    glEnd();
    ring(trackerX, trackerY, trackerRadius, 79, 183, 216, 190);
    ring(trackerX, trackerY, trackerRadius*0.5f, 70, 144, 169, 120);
    box(trackerX-4, trackerY-4, 8, 8, 111, 220, 244, 255);
    loopv(enemies)
    {
        const botent *bot = enemies[i].bot;
        if(!bot || bot->state != CS_ALIVE || (bot->vel.sqrxy() < 0.01f && lastmillis-bot->lastaction > 500)) continue;
        vec delta = vec(bot->o).sub(player1->o);
        if(delta.sqrxy() > 58.0f*58.0f) continue;
        delta.rotate_around_z(-yaw);
        box(trackerX + delta.x*2-7, trackerY + delta.y*2-7, 14, 14, 255, 74, 81, 250);
    }
    if(stage != COMPLETE)
    {
        const float direction = atan2f(bearing.x, -bearing.y);
        float x = VIRTW * 0.5f + clamp(direction / PI, -0.9f, 0.9f) * VIRTW * 0.42f;
        float y = 170.0f;
        if(!preview)
        {
            vec4 clip;
            mvpmatrix.transform(vec(target).add(vec(0, 0, 2.5f)), clip);
            if(clip.w > 0.01f)
            {
                x = VIRTW * clamp(0.5f + 0.5f * clip.x / clip.w, 0.045f, 0.955f);
                y = VIRTH * clamp(0.5f - 0.5f * clip.y / clip.w, 0.14f, 0.83f);
            }
        }
        glColor4ub(105, 218, 255, 240);
        glBegin(GL_LINE_LOOP);
        glVertex2f(x, y-12); glVertex2f(x+12, y); glVertex2f(x, y+12); glVertex2f(x-12, y);
        glEnd();
    }
    glEnable(GL_TEXTURE_2D);
    label("SHIELD", right-299, 79);
    if(driving && stage != COMPLETE)
    {
        int seconds = max(0, (escapeDeadline - lastmillis + 999) / 1000);
        defformatstring(timer)("ESCAPE  %d:%02d", seconds / 60, seconds % 60);
        label(timer, right, 203, 0.61f, true);
    }
    if(!preview || mission != 1 || section != 0)
    {
        defformatstring(title)("%s  %d/%d", haloMissions[mission].title, section + 1, haloMissions[mission].sections);
        label(title, left, 37, 0.56f);
    }
    const char *task = objectives[stage];
    if(mission != 1 || section != 0 || !preview)
    {
        const char *const sectionObjectives[] = {
            "Follow the route", "Clear the hostiles", "Reach the checkpoint",
            "Clear the hostiles", "Reach the next sector", "Section complete"
        };
        task = campaignFinished ? "Campaign complete" : sectionObjectives[stage];
        if(stage == INSERTION || stage == REACH_PASS)
        {
            if(const char *authored = haloscenarioobjective()) task = authored;
        }
        if(stage == EXTRACTION)
        {
            const char *interaction = sectionInteraction();
            if(interaction) task = interaction;
            else if(section + 1 == haloMissions[mission].sections) task = haloMissions[mission].finalObjective;
            else if(retailCartographer() && section == 0) task = "Reach the Cartographer entrance";
        }
    }
    vec control;
    bool mapped = false;
    const bool cartographer = stage != COMPLETE && halocartographertarget(control, mapped);
    if(cartographer) task = mapped ? "Return to the surface" : "Access the Cartographer";
    if(retailCartographer() && section == 2 && stage != COMPLETE && !halopelicanready()) task = "Reach the Pelican landing zone";
    label(task, left, 76, 0.72f);
    if(player1->state == CS_DEAD)
    {
        label("Click to revert to checkpoint", VIRTW/2-240, VIRTH/2+100, 0.65f);
        return;
    }
    if(stage == COMPLETE)
    {
        label(preview || campaignFinished ? "F5  Restart section" : "Enter  Continue   /   F5  Restart section", left, 125);
        return;
    }
    if((cartographer && !mapped && player1->o.dist(control) < 13.0f) ||
       (stage == EXTRACTION && sectionInteraction() &&
        (retailCartographer() ? section == 2 && halopelicanready() : !route[2].inrange(nextRouteMarker)) &&
        near(extraction, markerRadius)))
        label("E  Interact", left, 128);
    else
    {
        defformatstring(distance)("%d m", int(bearing.magnitudexy()/2.75f + 0.5f));
        label(distance, left, 128);
    }
    if(driving)
    {
        label("WARTHOG", right, VIRTH-155, 0.70f, true);
        return;
    }
    const weapon *current = player1->weaponsel;
    const char *name = current->type == GUN_PISTOL ? "M6D MAGNUM" : current->type == GUN_KNIFE ? "MELEE" : "MA5B ASSAULT RIFLE";
    label(name, right, VIRTH-212, 0.60f, true);
    defformatstring(ammo)("%02d / %03d", current->mag, current->ammo);
    label(ammo, right, VIRTH-173, 0.96f, true);
    defformatstring(grenades)("FRAG  %d", player1->mag[GUN_GRENADE]);
    label(grenades, right, VIRTH-100, 0.57f, true);
}

COMMANDF(halo_campaign, "", ()
{
    loadProgress();
    if(installed(savedMission, savedSection)) queueSection(savedMission, savedSection);
    else if(!installed(0, 0) && installed(1, 0)) queueSection(1, 0, true);
    else queueSection(0, 0);
});
COMMANDF(halo_preview, "", () { queueSection(1, 0, true); });
COMMANDF(halo_section, "ii", (int *m, int *s)
{
    if(*m >= 1 && *m <= haloMissionCount && *s >= 1 && *s <= haloMissions[*m - 1].sections)
        queueSection(*m - 1, *s - 1, true);
});
COMMANDF(halo_restart, "", () { if(haloactive()) queueSection(mission, section, preview); });
COMMANDF(halo_next, "", () { if(!halocinematicskip()) nextSection(); });
COMMANDF(halo_use, "", ()
{
    if(haloscenariouse()) return;
    if(retailCartographer() && (section != 2 || !halopelicanready())) return;
    if(haloactive() && !halocinematic() && player1->state == CS_ALIVE && stage == EXTRACTION && sectionInteraction() &&
       (retailCartographer() || !route[2].inrange(nextRouteMarker)) && near(extraction, markerRadius))
    {
        audiomgr.playsound(S_ITEMAMMO, SP_HIGH);
        finishSection();
    }
});
COMMANDF(halo_mission, "i", (int *index)
{
    loadProgress();
    if(*index >= 1 && *index <= haloMissionCount) queueSection(*index - 1, 0);
});
COMMANDF(halo_switch, "d", (bool down)
{
    if(down && haloactive() && !halocinematic()) execute(player1->weaponsel->type == GUN_ASSAULT ? "weapon pistol" : "weapon assault");
});

void haloremoveactor(const playerent *p)
{
    loopv(enemies) if(enemies[i].bot == p) enemies[i].bot = NULL;
    haloscenarioremovebot(p);
}

COMMANDF(halo_frag, "d", (bool down) { if(down) halofrag(); });
COMMANDF(halo_melee, "d", (bool down) { if(down) halomelee(); });

COMMANDF(halo_slot, "i", (int *slot)
{
    if(haloactive() && !halocinematic() && keypressed) execute(*slot == 2 ? "weapon pistol" : "weapon assault");
});

void haloplaysound(const char *sample, int fallback, physent *actor, const vec *position)
{
    int id = fallback;
    if(haloactive() && sample)
    {
        defformatstring(name)("halo/%s", sample);
        defformatstring(path)("packages/audio/%s.wav", name);
        if(fileexists(findfile(path, "rb"), "rb"))
        {
            id = audiomgr.findsound(name, 220, gamesounds);
            if(id < 0) id = audiomgr.addsound(name, 220, -1, false, gamesounds, true, 0);
        }
    }
    if(id < 0 || id == S_NULL) return;
    if(position) audiomgr.playsound(id, position);
    else if(actor && actor != player1) audiomgr.playsound(id, actor);
    else audiomgr.playsound(id, SP_HIGH);
}

COMMANDF(halo_pause, "", ()
{
    if(haloactive())
    {
        player1->attacking = false;
        player1->stopmoving();
        showmenu("Halo Campaign");
    }
    else execute("key_escape");
});


bool halodriving() { return haloactive() && driving && !editmode && player1->state == CS_ALIVE; }

void halorendervehicle()
{
    if(!halodriving()) return;
    vec origin = player1->o;
    origin.z -= player1->eyeheight;
    rendermodel("halo/warthog", ANIM_MAPMODEL, 0, -1, origin, 0, player1->yaw + 90, 0, 0, 0);
}

#ifdef HALO_QA
// Verification hooks live in tests/native. They read this file's internal
// campaign state, so they compile into the same translation unit.
#include "campaign_qa.inc"
#include "b30_qa.inc"
#endif
