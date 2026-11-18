#ifndef HALO_DIALOGUE_H
#define HALO_DIALOGUE_H

// Halo's unit dialogue for the campaign's actors and the player (halo_dialogue.cpp), from
// packages/halo/dialogue.txt (tools/halo_dialogue.py: each unit's udlg and the vocalizations' sounds)
// and the permutations tools/import_halo_audio.py decodes to packages/audio/halo/dialogue/. It follows
// ../Halo3DS units/unit_dialogue.c (speech priority, one line per unit, the pain and death sounds,
// the screams) and ai/ai_communication.c's event dispatch for the events the engine raises.
// Without the table, or with halo_dialogue 0, every unit keeps AssaultCube's stock pain and death
// sounds (clientgame.cpp).

// The table is loaded, halo_dialogue is on and the campaign runs: the stock pain and death sounds
// are replaced (a unit without dialogue, a Sentinel's, stays silent as in Halo).
bool halodialogueactive();
// Every unit's speech state goes (a section start, a revert: haloactorsclear).
void halodialoguereset();
// A unit is gone (haloremoveactor: an erased or freed bot): its speech goes before its playerent is
// freed, so a bot allocated at the same address in the same frame starts silent with its own dialogue.
void halodialogueforget(const playerent *unit);
// Once a frame from haloupdate: advances every unit's speech at Halo's 30 ticks a second.
void halodialogueupdate();

// unit_make_damage_sound (unit_dialogue.c:725-886) for a hit dodamage dealt: `healthLost` health
// points off the body (0 for a hit the shield took whole), `died` when it kills. The shield damage,
// the jpt! category, its instantaneous acceleration and a headshot come from halodamage's report of
// the same hit (halohitreport); a kill without one (the scripts' INT_MAX kills) is silent unless an
// act-of-god scope is open (ai_kill: the globals' falling jpt!, objects/damage.c:1830-1872). A hit
// that does not kill then raises ai_handle_damage's events (ai/ai.c:1316-1380): damage for a
// friend's hit, else hurt when the hit dealt 0.3 damage points or more (total_damage, not normalized,
// units/units.c:6759: nearly every hit); and 45 ticks after the last hit the delayed hurt on the peak
// of the normalized recent shield and body damage (units/units.c:5897-5915, 6812-6832).
void halodialoguedamage(playerent *unit, playerent *attacker, int healthLost, bool died, int gun);
// ai_handle_death's communication event (ai/ai.c:968-1003) for a unit dokill kills.
void halodialoguedeath(playerent *victim, playerent *killer);
// unit_scream (unit_dialogue.c:887-966).
enum { HALO_SCREAM_FALLING = 0, HALO_SCREAM_GRENADE_ATTACHED = 1, HALO_SCREAM_BURNING = 2 };
void halodialoguescream(playerent *unit, int type);
// ai_communication_event (ai/ai_communication.c:5878-7333) for the engine's events. Types are
// Halo's (ai_communication.h:19-75), hostility its _comm_hostility_* (0 none, 1 self, 2 friend,
// 3 enemy, 4 traitor), damage the jpt! category (-1 none). `enemyStatus` holds Halo's six
// enemy-status tests (bit 0 never had a target, 1 dead, 2 lost, 3 not visible, 4 no danger,
// 5 visible) for the subject; all set when the subject is not an actor.
enum { HALO_COMM_DEATH = 0, HALO_COMM_HURT = 2, HALO_COMM_DAMAGE = 3, HALO_COMM_SIGHTED_ENEMY = 4, HALO_COMM_GRENADE_THROWING = 9,
       HALO_COMM_GRENADE_SIGHTED = 11, HALO_COMM_GRENADE_DANGER = 12, HALO_COMM_SHOOTING = 26, HALO_COMM_SHOOTING_VEHICLE = 27,
       HALO_COMM_SHOOTING_BERSERK = 28, HALO_COMM_SHOOTING_GROUP = 29, HALO_COMM_SHOOTING_TRAITOR = 30, HALO_COMM_FLEE = 31,
       HALO_COMM_FLEE_LEADER_DIED = 32, HALO_COMM_BERSERK = 42, HALO_COMM_MELEE = 43, HALO_COMM_DIVE = 44, HALO_COMM_LEAP = 47 };
enum { HALO_HOSTILITY_NONE = 0, HALO_HOSTILITY_SELF = 1, HALO_HOSTILITY_FRIEND = 2, HALO_HOSTILITY_ENEMY = 3, HALO_HOSTILITY_TRAITOR = 4 };
void halodialogueevent(int type, playerent *subject, playerent *cause, int hostility = HALO_HOSTILITY_NONE, int damage = -1,
                       unsigned enemyStatus = 0x3f);
// ai_kill (not _silent): the kills inside the scope speak as Halo's act-of-god death does.
void halodialogueactofgod(bool on);
// ai_dialogue_triggers (ai_communication.c:6311-6319): off, no communication event speaks; on again
// at every section start and revert.
void halodialoguetriggers(bool on);

// QA (tests/test_dialogue.py, qa_dialogue_state): DIALOGUE_STATE and one DIALOGUE_UNIT line per unit
// with speech state.
void halodialoguedump(const char *label);

#endif
