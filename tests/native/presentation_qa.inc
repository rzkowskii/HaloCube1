// Presentation verification hooks (tests/test_presentation.py): script effects, camera shake,
// cinematic screen effects, the flashlight, the damage-direction indicator, sound class gains, the
// vehicles' engine loops and the melee lunge. Compiled into halo.cpp only for halo-ac-qa.
// Lines start with FX_QA (halofxdump, halo_fx.cpp) or FX_VEHICLE; the melee lunge logs MELEE_QA
// (halo_meleetrace 1, weapon.cpp).

COMMANDF(qa_fx_state, "s", (const char *label)
{
    halofxdump(label);
    conoutf("FX_VEHICLE %s engines=%d", label, halovehicleengineloops());
    fflush(stdout);
});

// A point `distance` world units from the player at `degrees` from the aim (0 ahead, 90 right).
static vec qaFxAround(float degrees, float distance)
{
    const float a = (player1->yaw + degrees) * RAD;
    return vec(sinf(a), -cosf(a), 0).mul(distance * haloscenarioscale()).add(player1->o);
}

// qa_fx_effect "<effe tag>" <distance>: the effect that many world units ahead of the player.
static void qaFxEffect(const char *tag, float distance)
{
    const bool known = halofxeffect(tag, qaFxAround(0, distance), true);
    conoutf("FX_QA effect known=%d", int(known));
    fflush(stdout);
}
COMMANDF(qa_fx_effect, "sf", (const char *tag, float *distance) { qaFxEffect(tag, *distance); });

// qa_fx_script_effect "<effe tag>" <cutscene flag>: effect_new through the script interpreter.
static void qaFxScriptEffect(const char *tag, const char *flag)
{
    const int index = haloscenarioflag(flag);
    const double values[2] = {0, double(index)};
    const char *const texts[2] = {tag, flag};
    haloscenariocallv("effect_new", values, texts, 2);
    conoutf("FX_QA script-effect flag=%d", index);
    fflush(stdout);
}
COMMANDF(qa_fx_script_effect, "ss", (const char *tag, const char *flag) { qaFxScriptEffect(tag, flag); });

// qa_fx_hit <degrees> <distance>: the damage-direction indicator for a hit from that bearing
// (0 ahead, 90 to the right) that many world units away.
COMMANDF(qa_fx_hit, "ff", (float *degrees, float *distance) { halofxdamage(qaFxAround(*degrees, *distance), 20, false); });

// qa_fx_actor_hit <amount>: the nearest living actor hits the player through halodamage (the
// shields first), which raises the indicator for shield hits as well.
COMMANDF(qa_fx_actor_hit, "i", (int *amount)
{
    playerent *from = NULL;
    float best = 1e16f;
    loopv(enemies)
    {
        botent *bot = enemies[i].bot;
        if(!bot || bot->state != CS_ALIVE) continue;
        const float d = player1->o.squareddist(bot->o);
        if(d < best) { best = d; from = bot; }
    }
    conoutf("FX_QA actor-hit from=%s", from ? from->name : "-");
    fflush(stdout);
    if(from) dodamage(*amount, player1, from, GUN_ASSAULT);
});

// qa_fx_face_actor: turns the player to the nearest living hostile actor (for the melee lunge).
COMMANDF(qa_fx_face_actor, "", ()
{
    playerent *target = NULL;
    float best = 1e16f;
    loopv(enemies)
    {
        botent *bot = enemies[i].bot;
        if(!bot || bot->state != CS_ALIVE || !haloactorhostile(player1, bot)) continue;
        const float d = player1->o.squareddist(bot->o);
        if(d < best) { best = d; target = bot; }
    }
    if(!target) { conoutf("FX_QA face no-actor"); fflush(stdout); return; }
    vec direction = vec(target->o).sub(player1->o);
    player1->yaw = atan2f(direction.x, -direction.y) / RAD;
    player1->pitch = 0;
    conoutf("FX_QA face %s distance=%.2f", target->name, sqrtf(best));
    fflush(stdout);
});

// AUDIO_QA (tests/test_mission_exits.py): the script's loops and conversations; QA_CONVERSATION one status.
COMMANDF(qa_audio_state, "s", (const char *label) { haloscenarioaudiodump(label); });
COMMANDF(qa_conversation_state, "s", (const char *name)
{
    conoutf("QA_CONVERSATION %s %d", name, haloscenarioconversationstatus(name));
    fflush(stdout);
});

// qa_fx_yaw DEGREES: turns the player (and so a turret they man) by that much at once; scheduled
// every 100 ms it turns them at ten times that rate (the turret engine loop's 15 degrees a second).
COMMANDF(qa_fx_yaw, "f", (float *degrees)
{
    player1->yaw = fmodf(player1->yaw + *degrees + 360.0f, 360.0f);
});
