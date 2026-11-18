// Halo's unit dialogue for the campaign (halo_dialogue.h): which line a unit speaks for its pain,
// its death, a scream or a combat event, one line per unit at a time by Halo's speech priorities.
// The rules are ../Halo3DS units/unit_dialogue.c and ai/ai_communication.c, ported tick for tick
// (30 a second); what the engine leaves out is listed in docs/macos-build.md (Unit dialogue).
#include "cube.h"
#include "bot/bot.h"
#include "halo.h"
#include "halo_ai.h"
#include "halo_dialogue.h"
#include "halo_script.h"
#include <algorithm>
#include <climits>
#include <cmath>
#include <deque>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

// Halo's unit dialogue in place of AssaultCube's stock pain and death sounds (with dialogue.txt).
VAR(halo_dialogue, 0, 1, 1);
// halo_dialoguetrace 1 logs every line chosen (VOCALIZATION) and every communication event (COMM_EVENT).
VAR(halo_dialoguetrace, 0, 0, 1);

namespace
{
    constexpr int formatVersion = 1;            // tools/halo_dialogue.py FORMAT
    constexpr int vocalizationTypes = 209;      // NUMBER_OF_DIALOGUE_VOCALIZATION_TYPES (dialogue_definitions.h:20)

    // dialogue_vocalization_lookup (../Halo3DS units/dialogue_definitions.c:830-850): the type
    // unit_test_speech tries next when a udlg has no sound (unit_dialogue.c:140-160). The port's table
    // points 93-95, 108, 109 and 191 at themselves, a loop that would never end for a udlg without
    // them; a self-reference is taken as no fallback here (the conservative reading).
    const short lookup[vocalizationTypes] = {
        1, -1, -1, -1, -1, -1, -1, 6, 6, 6, -1, 10, -1, 12, 15, -1, 14, 15, 14, 15, -1, -1, 21, -1,
        -1, -1, -1, -1, -1, -1, -1, 29, -1, -1, -1, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, -1, -1,
        -1, -1, -1, 49, -1, -1, -1, 53, 54, 53, 54, 53, 54, 53, 54, 53, 54, 53, 53, 53, 53, 53, 53, 53,
        53, 53, 53, 53, -1, -1, -1, -1, -1, 80, 80, 80, 80, 80, 80, 80, 80, 80, 80, 80, 80, 93, 94, 95,
        -1, 96, 96, 98, 96, 100, 100, 100, 100, -1, -1, -1, 108, 109, 108, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 53, 112, 154, 191,
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 0};

    // The vocalization types unit_dialogue.c names (:83-101).
    enum { VOC_PAIN_BODY = 6, VOC_PAIN_BODY_MAJOR = 7, VOC_PAIN_SHIELD = 8, VOC_PAIN_FALLING = 9, VOC_SCREAM_FEAR = 10,
           VOC_SCREAM_PAIN = 11, VOC_DEATH_QUIET = 14, VOC_DEATH_VIOLENT = 15, VOC_DEATH_FALLING = 16, VOC_DEATH_AGONIZING = 17,
           VOC_DEATH_INSTANT = 18, VOC_DEATH_FLYING = 19, VOC_HURT_ENEMY_GRENADE = 39 };
    // unit_speech_priority (units/units.h:29-42) and the play types (unit_dialogue.c:73-79).
    enum { SPEECH_NONE, SPEECH_IDLE, SPEECH_PAIN, SPEECH_TALK, SPEECH_COMMUNICATE, SPEECH_SHOUT, SPEECH_SCRIPTED, SPEECH_INVOLUNTARY,
           SPEECH_EXCLAMATION, SPEECH_SCREAM, SPEECH_DEATH, SPEECH_PRIORITIES };
    enum { PLAY_NONE, PLAY_QUEUE, PLAY_IMMEDIATE, PLAY_IMMEDIATE_DEQUEUE };
    const char *const priorityNames[SPEECH_PRIORITIES] = {"none", "idle", "pain", "talk", "communicate", "shout", "script",
                                                          "involuntary", "exclaim", "scream", "death"};
    // global_speech_override_priorities and global_speech_queue_times (unit_dialogue.c:181-208).
    const int overridePriorities[SPEECH_PRIORITIES] = {SPEECH_NONE, SPEECH_NONE, SPEECH_IDLE, SPEECH_IDLE, SPEECH_PAIN, SPEECH_PAIN,
                                                       SPEECH_SHOUT, SPEECH_SHOUT, SPEECH_INVOLUNTARY, SPEECH_INVOLUNTARY, SPEECH_DEATH};
    const float queueForever = 1e30f;
    const float queueTimes[SPEECH_PRIORITIES] = {0, 0, 0, 1.5f, 3, 4, queueForever, 3, 3, 3, 3};
    // The jpt! damage categories unit_make_damage_sound tests (unit_dialogue.c:103-108).
    enum { CATEGORY_FALLING = 1, CATEGORY_FLAME = 7 };

    // --- the table (packages/halo/dialogue.txt) -------------------------------

    struct Sound
    {
        std::string sample;                     // dialogue/<sample>; permutation n is <sample>_<n>.wav
        int permutations = 0, longest = 0;      // longest permutation, ms (sound_definition +132)
        std::vector<float> skip;
        unsigned played = 0;                    // sound_pitch_range played_permutation_mask
        int previous = -1;
    };
    struct Dialogue { std::string path; int vocalization[vocalizationTypes]; };
    struct Unit { std::string path; std::vector<std::pair<int, int>> variants; };
    std::vector<Sound> sounds;
    std::vector<Dialogue> dialogues;
    std::vector<Unit> units;
    std::unordered_map<std::string, int> actorUnits;
    int playerUnit = -1;
    bool tried = false, loaded = false;
    // unit_dialogue_determine_variant's sequential_counter (unit_dialogue.c:431-435).
    int sequentialCounter = 0;

    std::string lower(std::string s) { for(char &c : s) c = char(tolower(c)); return s; }

    std::string sampleOf(const std::string &tag)
    {
        // tools/import_halo_audio.py dialogue_sample: 'sound\dialog\grunt\pain' -> 'dialogue/dialog-grunt-pain'.
        std::string name = lower(tag);
        if(!name.compare(0, 6, "sound\\")) name.erase(0, 6);
        for(char &c : name) c = c == '\\' ? '-' : c == ' ' ? '_' : c;
        return "dialogue/" + name;
    }

    bool parse(const char *text)
    {
        std::istringstream lines(text);
        std::string line;
        Dialogue *dialogue = nullptr;
        Unit *unit = nullptr;
        bool versioned = false;
        int number = 0;
        while(std::getline(lines, line))
        {
            ++number;
            if(line.empty() || line[0] == '#') continue;
            std::istringstream in(line);
            std::string key;
            in >> key;
            bool ok = true;
            int id = -1;
            if(key == "format") { int v = 0; in >> v; ok = versioned = v == formatVersion; }
            else if(key == "sound")
            {
                Sound s;
                std::string tag;
                ok = bool(in >> id >> s.permutations >> s.longest) && id == int(sounds.size()) && s.permutations > 0 && s.permutations <= 32 &&
                     s.longest >= 0;
                std::getline(in >> std::ws, tag);
                ok = ok && !tag.empty();
                if(ok) { s.sample = sampleOf(tag); s.skip.assign(s.permutations, 0.0f); sounds.push_back(s); }
            }
            else if(key == "skip")
            {
                ok = bool(in >> id) && id >= 0 && id < int(sounds.size());
                for(int i = 0; ok && i < sounds[id].permutations; ++i)
                {
                    float f = 0;
                    if(!(in >> f)) break;
                    sounds[id].skip[i] = std::isfinite(f) ? clamp(f, 0.0f, 1.0f) : 0.0f;
                }
            }
            else if(key == "udlg")
            {
                ok = bool(in >> id) && id == int(dialogues.size());
                if(ok)
                {
                    dialogues.emplace_back();
                    dialogue = &dialogues.back();
                    std::getline(in >> std::ws, dialogue->path);
                    loopi(vocalizationTypes) dialogue->vocalization[i] = -1;
                }
            }
            else if(key == "voc")
            {
                int type = -1, sound = -1;
                ok = dialogue && bool(in >> type >> sound) && type >= 0 && type < vocalizationTypes && sound >= 0 && sound < int(sounds.size());
                if(ok) dialogue->vocalization[type] = sound;
            }
            else if(key == "unit")
            {
                ok = bool(in >> id) && id == int(units.size());
                if(ok) { units.emplace_back(); unit = &units.back(); std::getline(in >> std::ws, unit->path); }
            }
            else if(key == "variant")
            {
                int variant = 0, udlg = -1;
                ok = unit && bool(in >> variant >> udlg) && udlg >= 0 && udlg < int(dialogues.size());
                if(ok) unit->variants.emplace_back(variant, udlg);
            }
            else if(key == "end") dialogue = nullptr, unit = nullptr;
            else if(key == "actor")
            {
                std::string tag;
                ok = bool(in >> id) && id >= 0 && id < int(units.size());
                std::getline(in >> std::ws, tag);
                if(ok && !tag.empty()) actorUnits[lower(tag)] = id;
            }
            else if(key == "player") ok = bool(in >> playerUnit) && playerUnit >= 0 && playerUnit < int(units.size());
            // Unknown keys are skipped, so a newer table still loads.
            if(!ok)
            {
                conoutf("\f3Halo dialogue: invalid line %d in packages/halo/dialogue.txt; re-run tools/halo_dialogue.py", number);
                return false;
            }
        }
        return versioned && !dialogues.empty();
    }

    bool load()
    {
        if(tried) return loaded;
        tried = true;
        int size = 0;
        char *text = loadfile(path("packages/halo/dialogue.txt", true), &size);
        if(!text) return false;     // optional: the stock sounds stay
        loaded = parse(text);
        delete[] text;
        if(!loaded)
        {
            sounds.clear(); dialogues.clear(); units.clear(); actorUnits.clear();
            playerUnit = -1;
        }
        return loaded;
    }

    // --- the units' speech (unit_speech, units/units.h:397-418) --------------

    struct SpeechItem { int priority = SPEECH_NONE, vocalization = -1, sound = -1, delay = 0, pause = 0, aiDelay = 0, usage = -1; };
    struct Speaker
    {
        playerent *unit = nullptr;
        int dialogue = -1;
        SpeechItem current, queued;
        int preDelay = 0, soundTimer = 0, postDelay = 0, aiDelay = 0;
        bool played = false, finished = false, notified = true;
        long long lastFinished = -1;            // last_speech_finished_time (ticks; -1 none)
        // object recent_body_damage and body_damage_decay_timer (objects/damage.c:1221-1227, 1975-1990):
        // the body damage of the last hits as a fraction of the maximum, which decays after 2 s.
        float recentBody = 0;
        int decayTimer = -1;
        // unit_speech damage timers (unit_dialogue.c:831-842, 975-1000).
        int minorTimer = 0, majorTimer = 0, minorSounds = 0, minorDecay = 0;
        // recent_shield_damage and shield_damage_decay_timer (damage.c:950-969, 1995-2006), as the body's.
        float recentShield = 0;
        int shieldDecayTimer = -1;
        // unit delayed_damage_timer, _peak, last_damage_category and _attacker (units/units.c:5897-5915,
        // 6812-6832): 45 ticks after the last hit, ai_handle_damage's delayed call on the peak.
        int delayedTimer = 0, delayedCategory = 0;
        float delayedPeak = 0;
        playerent *delayedAttacker = nullptr;
        int lines = 0;                          // for the QA dump
    };
    // A deque: a Speaker reference stays valid while speaker(p, true) adds others (talkWeight and
    // findActorToTalk do, inside an event); only an erase (a unit gone, a reset) moves them, and nothing
    // holds one across an erase.
    std::deque<Speaker> speakers;
    long long ticks = 0;                        // game time since the section (re)started
    int tickRemainder = 0, tickMillis = -1;
    bool triggersEnabled = true;                // ai_dialogue_triggers (ai_communication.c:6311-6319)
    int actOfGod = 0;

    bool alive(const playerent *p) { return p && p->state == CS_ALIVE; }
    float scale() { return max(haloscenarioscale(), 1e-3f); }

    // A unit still in the game (bots may be deleted under the speech: erased actors, kicked bots).
    bool present(const playerent *p)
    {
        if(p == player1) return true;
        loopv(players) if(players[i] == p) return true;
        return false;
    }

    // unit_dialogue_setup with unit_dialogue_determine_variant (unit_dialogue.c:395-440, 1003-1072):
    // the object's variant number is 0 (the engine does not pick model permutations), so the
    // sequential counter picks among the variant numbers below 100 and the unit takes a random
    // dialogue variant of that number (else of number 0, else any).
    int dialogueFor(playerent *p)
    {
        int unit = -1;
        if(p == player1) unit = playerUnit;
        else if(const HaloActorDef *def = haloactorvariant(p))
        {
            auto found = actorUnits.find(lower(def->path));
            if(found != actorUnits.end()) unit = found->second;
        }
        if(unit < 0) return -1;
        const std::vector<std::pair<int, int>> &variants = units[unit].variants;
        std::vector<int> numbers;
        for(const auto &v : variants) if(v.first < 100 && numbers.size() < 16) numbers.push_back(v.first);
        int number = 0;
        if(!numbers.empty()) number = numbers[sequentialCounter++ % int(numbers.size())];
        auto pick = [&](int wanted) -> int
        {
            std::vector<int> matching;
            for(const auto &v : variants) if(wanted < 0 || v.first == wanted) matching.push_back(v.second);
            if(matching.empty()) return -1;
            return matching.size() == 1 ? matching[0] : matching[rnd(int(matching.size()))];
        };
        int dialogue = number > 0 ? pick(number) : -1;
        if(dialogue < 0) dialogue = pick(0);
        if(dialogue < 0) dialogue = pick(-1);
        return dialogue;
    }

    Speaker *speaker(playerent *p, bool create)
    {
        if(!p) return nullptr;
        for(Speaker &s : speakers) if(s.unit == p) return &s;
        if(!create || !alive(p)) return nullptr;
        speakers.emplace_back();
        Speaker &s = speakers.back();
        s.unit = p;
        s.dialogue = dialogueFor(p);
        return &s;
    }

    const char *unitName(const playerent *p) { return !p ? "-" : p == player1 ? "player" : p->name; }
    const char *dialogueName(int d) { return d >= 0 && d < int(dialogues.size()) ? dialogues[d].path.c_str() : "-"; }

    // unit_test_speech (unit_dialogue.c:226-388).
    int testSpeech(const Speaker &s, int priority, bool allowLookup, bool allowQueue, long long *lastSpeech, int &vocalization, int &sound)
    {
        int result = PLAY_NONE;
        if(sound < 0 && s.dialogue >= 0 && vocalization >= 0)
        {
            const Dialogue &d = dialogues[s.dialogue];
            do
            {
                sound = d.vocalization[vocalization];
                if(!allowLookup || sound >= 0) break;
                const int next = lookup[vocalization];
                vocalization = next == vocalization ? -1 : next;
            }
            while(vocalization >= 0);
        }
        if((alive(s.unit) || priority == SPEECH_DEATH) && sound >= 0)
        {
            int current = s.current.priority;
            if(current == SPEECH_NONE) result = PLAY_IMMEDIATE;
            else
            {
                const int queued = s.queued.priority;
                int highest = max(current, queued);
                if((priority == SPEECH_PAIN || priority == SPEECH_INVOLUNTARY || priority == SPEECH_DEATH) && s.played && s.soundTimer == 0 &&
                   priority > highest)
                {
                    current = SPEECH_NONE;
                    highest = queued;
                }
                if(overridePriorities[priority] >= highest) result = PLAY_IMMEDIATE_DEQUEUE;
                else if(priority >= SPEECH_INVOLUNTARY && overridePriorities[priority] >= current) result = PLAY_IMMEDIATE;
                else if(allowQueue && queueTimes[priority] != 0)
                {
                    bool queue = queueTimes[priority] >= queueForever || s.postDelay + s.soundTimer < int(queueTimes[priority] * 30);
                    if(queue && priority <= highest && priority <= queued) queue = false;
                    if(queue) result = PLAY_QUEUE;
                }
            }
        }
        if(lastSpeech) *lastSpeech = s.lastFinished;
        return result;
    }

    // unit_speak (unit_dialogue.c:599-679).
    void speak(Speaker &s, int playType, SpeechItem item)
    {
        if(!alive(s.unit) && item.priority != SPEECH_DEATH) return;
        if(playType >= PLAY_IMMEDIATE)
        {
            s.current = item;
            if(playType == PLAY_IMMEDIATE_DEQUEUE) s.queued.priority = SPEECH_NONE;
            s.preDelay = s.current.delay;
            s.played = s.notified = s.finished = false;
            s.postDelay = s.current.pause;
            s.aiDelay = s.current.aiDelay;
            // The line lasts the sound's longest permutation (unit_dialogue.c:353-356).
            s.soundTimer = s.current.sound >= 0 ? int((long long)sounds[s.current.sound].longest * 30 / 1000) : 45;
        }
        else if(playType == PLAY_QUEUE) s.queued = item;
    }

    // sound_definition_next_permutation (sound/sound_definitions.c:194-262): a random start, the
    // first permutation not played since the mask was last full, kept unless its skip fraction says.
    int nextPermutation(Sound &sound)
    {
        const int count = sound.permutations;
        int selected = rnd(count);
        for(int attempts = 0;;)
        {
            const unsigned all = count >= 32 ? 0xffffffffu : (1u << count) - 1;
            if((~sound.played & all) == 0) sound.played = count > 1 && sound.previous >= 0 ? 1u << sound.previous : 0;
            if(!(sound.played & (1u << selected)))
            {
                sound.played |= 1u << selected;
                if(attempts++ == 16 || rndscale(1.0f) >= sound.skip[selected]) break;
            }
            if(++selected == count) selected = 0;
        }
        sound.previous = selected;
        return selected;
    }

    void play(Speaker &s)
    {
        if(s.current.sound < 0) return;
        Sound &sound = sounds[s.current.sound];
        const int permutation = nextPermutation(sound);
        defformatstring(sample)("%s_%d", sound.sample.c_str(), permutation);
        ++s.lines;
        if(halo_dialoguetrace)
            conoutf("VOCALIZATION unit=%s dialogue=%s type=%d priority=%s sample=%s", unitName(s.unit), dialogueName(s.dialogue),
                    s.current.vocalization, priorityNames[s.current.priority], sample);
        // From the unit (the player's own from the camera), at its sound class's gain (haloplaysound).
        haloplaysound(sample, S_NULL, s.unit == player1 ? NULL : s.unit);
    }

    // --- ai_communication (ai/ai_communication.c) ------------------------------

    // dialogue_usage (ai_communication.c:680-699) and the rows of global_dialogue_table (1215-1484)
    // for the events the engine raises (halo_dialogue.h), in Halo's order.
    struct Usage
    {
        short type, priority, vocalization, animation, protagonist, protagonistLook, recipientLookDirection, recipientLook;
        float weight, repeatDelay;
        short flags, group, hostility, enemyStatus, subjectRace, causeRace, damage;
    };
    const Usage usages[] = {
        /* death */
        {0, 3, 49, -1, 1, 6, 2, 1, 10.0f, 0.0f, 0, -1, 2, -1, -1, -1, -1},
        {0, 6, 51, -1, 1, 6, 3, 6, 20.0f, 0.0f, 66, -1, 2, -1, 1, -1, -1},
        {0, 2, 53, -1, 1, 1, 2, 1, 10.0f, 0.0f, 8, -1, 3, -1, -1, -1, -1},
        {0, 6, 55, -1, 1, 6, 3, 6, 20.0f, 0.0f, 74, -1, 3, -1, 1, -1, -1},
        {0, 2, 57, -1, 1, 1, 2, 1, 10.0f, 0.0f, 8, -1, 3, -1, 4, -1, -1},
        {0, 2, 59, -1, 1, 1, 2, 1, 40.0f, 0.0f, 8, -1, 3, -1, 8, -1, -1},
        {0, 2, 61, -1, 1, 1, 2, 1, 40.0f, 0.0f, 8, -1, 3, -1, 16, -1, -1},
        {0, 2, 63, -1, 1, 1, 2, 1, 40.0f, 0.0f, 8, -1, 3, -1, 64, -1, -1},
        {0, 2, 65, -1, 1, 1, 2, 1, 10.0f, 10.0f, 0, -1, 3, -1, -1, -1, 2},
        {0, 2, 67, -1, 1, 1, 2, 1, 10.0f, 10.0f, 0, -1, 3, -1, -1, -1, 10},
        {0, 2, 66, -1, 1, 1, 2, 1, 10.0f, 0.0f, 0, -1, 3, -1, -1, -1, 11},
        {0, 3, 68, -1, 1, 1, 2, 1, 20.0f, 0.0f, 0, -1, 3, -1, -1, -1, 5},
        {0, 3, 69, -1, 1, 1, 2, 1, 20.0f, 0.0f, 0, -1, 3, -1, -1, -1, 3},
        {0, 3, 70, -1, 1, 1, 2, 1, 20.0f, 0.0f, 0, -1, 3, -1, -1, -1, 4},
        {0, 3, 71, -1, 1, 1, 2, 1, 20.0f, 0.0f, 0, -1, 3, -1, -1, -1, 6},
        {0, 2, 72, -1, 1, 1, 2, 1, 20.0f, 0.0f, 0, -1, 3, -1, -1, -1, 7},
        {0, 2, 73, -1, 1, 1, 2, 1, 20.0f, 0.0f, 0, -1, 3, -1, -1, -1, 12},
        {0, 3, 74, -1, 1, 1, 2, 1, 30.0f, 0.0f, 0, -1, 3, -1, -1, -1, 9},
        {0, 2, 75, -1, 1, 1, 2, 1, 30.0f, 10.0f, 0, -1, 3, -1, -1, -1, 8},
        {0, 2, 96, -1, 2, 1, 3, 1, 10.0f, 0.0f, 0, -1, -1, -1, -1, -1, -1},
        {0, 6, 97, -1, 2, 6, 3, 6, 15.0f, 0.0f, 66, -1, -1, -1, 1, -1, -1},
        {0, 3, 98, -1, 2, 5, 1, 1, 10.0f, 0.0f, 0, -1, 2, -1, -1, -1, -1},
        {0, 4, 99, -1, 2, 6, 3, 5, 20.0f, 0.0f, 66, -1, 2, -1, -1, 1, -1},
        {0, 2, 100, -1, 2, 1, 3, 1, 10.0f, 0.0f, 0, -1, 3, -1, -1, -1, -1},
        {0, 2, 101, -1, 2, 1, 3, 1, 10.0f, 0.0f, 0, -1, 3, -1, -1, 1, -1},
        {0, 2, 102, -1, 2, 1, 3, 1, 10.0f, 0.0f, 0, -1, 3, -1, -1, 4, -1},
        {0, 2, 103, -1, 2, 1, 3, 1, 40.0f, 0.0f, 0, -1, 3, -1, -1, 56, -1},
        {0, 2, 104, -1, 2, 1, 3, 1, 40.0f, 0.0f, 0, -1, 3, -1, -1, 64, -1},
        {0, 5, 105, -1, 2, 6, 3, 6, 30.0f, 0.0f, 2, -1, 4, -1, -1, -1, -1},
        /* damage */
        {3, 3, 21, -1, 1, 1, 2, 1, 10.0f, 0.0f, 0, -1, 2, -1, -1, -1, -1},
        {3, 3, 22, -1, 1, 1, 2, 1, 10.0f, 0.0f, 2, -1, 2, -1, 1, -1, -1},
        {3, 3, 29, -1, 0, 5, 3, 1, 10.0f, 0.0f, 8, -1, 2, -1, -1, -1, -1},
        {3, 3, 31, -1, 0, 6, 3, 1, 10.0f, 5.0f, 0, -1, 2, -1, -1, 1, -1},
        /* hurt */
        {2, 1, 23, -1, 1, 1, 0, 0, 10.0f, 10.0f, 0, -1, 3, -1, -1, -1, -1},
        {2, 2, 32, -1, 0, 5, 0, 0, 10.0f, 10.0f, 0, -1, 3, -1, -1, -1, -1},
        {2, 2, 35, -1, 0, 5, 0, 0, 10.0f, 10.0f, 0, -1, 3, -1, -1, -1, 2},
        {2, 2, 36, -1, 0, 5, 0, 0, 10.0f, 10.0f, 0, -1, 3, -1, -1, -1, 11},
        {2, 2, 37, -1, 0, 5, 0, 0, 10.0f, 10.0f, 0, -1, 3, -1, -1, -1, 10},
        {2, 3, 38, -1, 0, 5, 0, 0, 20.0f, 0.0f, 0, -1, 3, -1, -1, -1, 5},
        {2, 2, 40, -1, 0, 5, 0, 0, 20.0f, 0.0f, 0, -1, 3, -1, -1, -1, 4},
        {2, 3, 41, -1, 0, 5, 0, 0, 20.0f, 0.0f, 0, -1, 3, -1, -1, -1, 6},
        {2, 2, 42, -1, 0, 5, 0, 0, 20.0f, 0.0f, 0, -1, 3, -1, -1, -1, 7},
        {2, 2, 43, -1, 0, 5, 0, 0, 20.0f, 0.0f, 0, -1, 3, -1, -1, -1, 12},
        {2, 3, 44, -1, 0, 5, 0, 0, 30.0f, 0.0f, 0, -1, 3, -1, -1, -1, 9},
        {2, 2, 45, -1, 0, 5, 0, 0, 30.0f, 0.0f, 0, -1, 3, -1, -1, -1, 8},
        /* sighted_enemy */
        {4, 5, 108, 3, 0, 6, 3, 6, 10.0f, 10.0f, 0, -1, -1, 0, -1, -1, -1},
        {4, 5, 109, 3, 0, 6, 3, 6, 10.0f, 25.0f, 0, -1, -1, 2, -1, -1, -1},
        /* shooting */
        {26, 1, 149, -1, 0, 0, 0, 0, 10.0f, 10.0f, 0, -1, -1, -1, -1, -1, -1},
        /* shooting_vehicle */
        {27, 1, 150, -1, 0, 0, 0, 0, 20.0f, 10.0f, 0, -1, -1, -1, -1, -1, -1},
        /* shooting_berserk */
        {28, 2, 151, -1, 0, 0, 0, 0, 20.0f, 0.0f, 0, -1, -1, -1, -1, -1, -1},
        /* shooting_group */
        {29, 2, 152, -1, 0, 0, 0, 0, 10.0f, 0.0f, 0, -1, -1, -1, -1, -1, -1},
        /* shooting_traitor */
        {30, 1, 153, -1, 0, 0, 0, 0, 30.0f, 10.0f, 0, -1, -1, -1, -1, -1, -1},
        /* flee */
        {31, 3, 156, -1, 0, 0, 2, 1, 10.0f, 0.0f, 0, -1, -1, -1, -1, -1, -1},
        /* flee_leader_died */
        {32, 4, 158, -1, 0, 0, 2, 1, 10.0f, 0.0f, 0, -1, -1, -1, -1, -1, -1},
        /* grenade_sighted */
        {11, 3, 117, -1, 0, 6, 4, 6, 10.0f, 4.0f, 0, 0, 3, -1, -1, -1, -1},
        /* grenade_danger */
        {12, 5, 118, -1, 0, 5, 4, 5, 10.0f, 4.0f, 0, 0, 3, -1, -1, -1, -1},
        {12, 5, 120, -1, 0, 5, 4, 5, 10.0f, 0.0f, 0, 0, 2, -1, -1, -1, -1},
        {12, 6, 119, -1, 0, 6, 4, 5, 10.0f, 0.0f, 0, -1, 1, -1, -1, -1, -1},
        /* berserk */
        {42, 7, 178, -1, 0, 4, 2, 1, 10.0f, 0.0f, 0, -1, -1, -1, -1, -1, -1},
        /* melee */
        {43, 7, 179, -1, 0, 4, 2, 1, 10.0f, 0.0f, 0, -1, -1, -1, -1, -1, -1},
        /* grenade_throwing */
        {9, 7, 115, -1, 0, 4, 2, 1, 10.0f, 0.0f, 0, -1, -1, -1, -1, -1, -1},
        /* dive */
        {44, 7, 180, -1, 0, 4, 2, 1, 10.0f, 0.0f, 0, -1, -1, -1, -1, -1, -1},
        /* leap */
        {47, 7, 182, -1, 0, 4, 2, 1, 10.0f, 0.0f, 0, -1, -1, -1, -1, -1, -1},
    };
    constexpr int usageCount = int(sizeof(usages) / sizeof(usages[0]));
    enum { USAGE_LOOKUP = 1, USAGE_FORCE = 2, USAGE_IMMEDIATE_NOTIFY = 4, USAGE_PLAYER = 8, USAGE_SAME_VEHICLE = 16, USAGE_ALLOW_SUBJECT = 32,
           USAGE_OVERRIDE_SCRIPTED = 64 };
    enum { PROTAGONIST_SUBJECT, PROTAGONIST_CAUSE, PROTAGONIST_FRIEND, PROTAGONIST_TARGET, PROTAGONIST_ENEMY };
    enum { FIND_SAME_TEAM, FIND_FRIEND, FIND_ENEMY };
    enum { FIND_LOOKUP = 1, FIND_NEAR_PLAYERS = 2, FIND_SAME_VEHICLE = 4, FIND_ALLOW_SUBJECT = 8, FIND_ALLOW_CAUSE = 16 };
    enum { COMM_EXCLAIM = 7, COMM_YELL = 6, COMM_SHOUT = 5 };
    // communication_speech_priorities, _notification_delays, _timer_tolerances and _play_delays
    // (ai_communication.c:1133-1213).
    const int commSpeechPriorities[8] = {0, 3, 3, 3, 4, 5, 5, 8};
    const float notificationDelays[8] = {0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.3f};
    const float timerTolerances[8][2][5] = {
        {{0, 0, 0, 0, 0}, {0, 0, 0, 0, 0}},
        {{4.0f, 4.5f, 2.5f, 2.0f, 0}, {5.0f, 5.0f, 3.0f, 2.5f, 0}},
        {{3.0f, 3.5f, 2.0f, 1.5f, 0}, {4.5f, 4.5f, 2.5f, 2.0f, 0}},
        {{0.3f, 1.0f, 1.0f, 1.0f, 0.8f}, {1.0f, 2.0f, 1.5f, 1.5f, 0.3f}},
        {{0, 0.5f, 0.5f, 0.5f, 1.3f}, {0.2f, 1.0f, 1.0f, 1.0f, 0.8f}},
        {{0, 0, 1.0f, 1.0f, 1.5f}, {0, 0, 1.5f, 1.5f, 1.0f}},
        {{0, 0, 0.5f, 0, 1.5f}, {0, 0, 0.5f, 0, 1.5f}},
        {{0, 0, 0.5f, 0, 0}, {0, 0, 0.5f, 0, 0}},
    };
    const float playDelays[5] = {0, 0.5f, 0.8f, 0.5f, 0.8f};
    const int playerAdditionalDelay = 30, overlapTime = 45, lowPriorityTimeout = 30;
    const float playerAbsoluteRange = 30, playerIdealMin = 3, playerIdealMax = 15, playerIdealFov = 0.70710677f, lowPriorityRating = 2;
    // Races (ai/actor_types.h:46-55) by actr type (actor_types.c:107-123 with each actor_type_*.c's race).
    enum { RACE_PLAYER = 1, RACE_HUMAN = 2, RACE_COVENANT = 4, RACE_FLOODCOMBAT = 8, RACE_CARRIER = 16, RACE_INFECTION = 32, RACE_SENTINEL = 64 };
    int raceOfType(int type)
    {
        static const int races[16] = {4, 4, 4, 4, 4, 4, 2, 2, 2, 8, 32, 16, 64, 64, 4, 0};
        return type >= 0 && type < 16 ? races[type] : 0;
    }
    int raceOf(const playerent *p)
    {
        if(!p) return 0;
        if(p == player1) return RACE_PLAYER;
        if(const HaloActorDef *def = haloactorvariant(p)) return raceOfType(def->type);
        // ai_get_race_from_team_index (ai/ai.c:2134-2150) for a unit without an actor.
        switch(haloactorteam(p))
        {
            case HALO_TEAM_PLAYER: return RACE_PLAYER;
            case HALO_TEAM_HUMAN: return RACE_HUMAN;
            case HALO_TEAM_COVENANT: return RACE_COVENANT;
            case HALO_TEAM_FLOOD: return RACE_FLOODCOMBAT | RACE_CARRIER | RACE_INFECTION;
            case HALO_TEAM_SENTINEL: return RACE_SENTINEL;
            default: return 0;
        }
    }
    // actor_communication_team (ai_communication.c:2598-2615): 0 human, 1 covenant, -1 none.
    int communicationTeam(const playerent *p)
    {
        const HaloActorDef *def = p && p != player1 ? haloactorvariant(p) : nullptr;
        if(!def) return -1;
        const int race = raceOfType(def->type);
        return race & RACE_HUMAN ? 0 : race & RACE_COVENANT ? 1 : -1;
    }
    struct EventStatus { long long lastSpoken = -1, disableUntil = -1; };
    EventStatus events[usageCount][2];
    long long lastChatter[2] = {0, 0}, lastTalk[2] = {0, 0}, lastShout[2] = {0, 0};

    int teamOf(const playerent *p) { return p == player1 ? HALO_TEAM_PLAYER : haloactorteam(p); }
    vec headOf(const playerent *p) { return p->o; }

    // ai_communication_get_player_rating (ai_communication.c:2958-3122). Distances in Halo world units.
    float playerRating(const playerent *unit, bool lineOfSight)
    {
        if(!alive(player1)) return 1;           // no player unit: every unit rates 1
        const float s = scale();
        const vec head = headOf(unit), playerHead = headOf(player1);
        vec v = vec(head).sub(playerHead);
        const float distance = v.magnitude() / s;
        if(distance >= playerAbsoluteRange) return 0;
        bool clear = false;
        if(lineOfSight) clear = distance < playerIdealMin || IsVisible(playerHead, head, NULL);
        float rating = 1;
        if(distance < playerIdealMax)
        {
            rating = distance < playerIdealMin ? 2 : 1 + (playerIdealMax - distance) / (playerIdealMax - playerIdealMin);
            if(clear) rating += 0.5f;
            if(distance > 1e-4f)
            {
                const float yaw = player1->yaw * RAD, pitch = player1->pitch * RAD;
                const vec aim(sinf(yaw) * cosf(pitch), -cosf(yaw) * cosf(pitch), sinf(pitch));
                const float facing = aim.dot(v) / (distance * s);
                if(facing > playerIdealFov) rating += 0.7f - (1 - facing) / (1 - playerIdealFov) * 0.35f;
            }
        }
        return rating;
    }

    // ai_communication_consider_speech (ai_communication.c:2094-2179).
    int considerSpeech(Speaker &s, int commPriority, int speechPriority, int delayTicks, bool lookupAllowed, bool recentDisabling,
                       int &vocalization, float &weight, int &sound)
    {
        long long last = -1;
        int playType = testSpeech(s, speechPriority, lookupAllowed, true, &last, vocalization, sound);
        if(playType == PLAY_QUEUE) weight *= 0.3f;
        if(recentDisabling && commPriority < COMM_SHOUT && last >= 0)
        {
            const int elapsed = int(max(0LL, ticks - last));
            const int tolerance = int(timerTolerances[commPriority][0][2] * 30.0f + delayTicks);
            if(elapsed <= tolerance) { playType = PLAY_NONE; weight = 0; }
            else if(elapsed < tolerance + 60) weight = (elapsed - tolerance) * weight * 0.016666668f;
        }
        return playType;
    }

    bool sees(const playerent *from, const playerent *to) { return IsVisible(headOf(from), headOf(to), NULL); }

    // ai_communication_actor_talk_weight (ai_communication.c:2181-2442), for actor `p`. The prop tests
    // (what the actor knows of the subject and the cause) stand as: within the stimulus range, and
    // in clear sight or (for a unit that is not an enemy) known without it except for a death.
    float talkWeight(playerent *p, playerent *subject, playerent *cause, float range, bool causeGate, int type, int commPriority,
                     int speechPriority, int vocalization, int flags)
    {
        if(!alive(p) || haloactorstatus(p) <= 1) return 0;     // asleep or braindead
        if(haloactorflags(p) & HALO_AI_BRAINDEAD) return 0;
        const float s = scale();
        float weight = 10;
        const bool hasTarget = subject || cause;
        if(hasTarget)
        {
            bool passes = subject && headOf(subject).dist(headOf(p)) / s < range;
            // ai_communication_find_global_actor_to_talk never sets the cause's point (its BUG at
            // ai_communication.c:2522-2527): only the encounter search tests the cause here.
            if(!passes && cause && causeGate) passes = headOf(cause).dist(headOf(p)) / s < range;
            if(!passes) return 0;
        }
        if(flags & FIND_NEAR_PLAYERS)
        {
            const float rating = playerRating(p, false);
            if(rating == 0) return 0;
            weight = rating * 5 + 10;
        }
        Speaker *sp = speaker(p, true);
        if(!sp) return 0;
        if(vocalization >= 0)
        {
            int line = vocalization, sound = -1;
            if(considerSpeech(*sp, commPriority, speechPriority, 0, (flags & FIND_LOOKUP) != 0, true, line, weight, sound) == PLAY_NONE) return 0;
        }
        if(hasTarget)
        {
            bool subjectMatches = false, causeMatches = false;
            auto known = [&](playerent *u, bool death) -> float
            {
                const float distance = headOf(u).dist(headOf(p)) / s;
                if(distance > range) return -1;
                const bool enemy = haloactorhostile(p, u);
                if(sees(p, u) || (!enemy && !death)) return distance;
                return -1;
            };
            if(subject)
            {
                if(subject == p) { if(flags & FIND_ALLOW_SUBJECT) subjectMatches = true; else return 0; }
                else
                {
                    const float d = known(subject, type == HALO_COMM_DEATH);
                    if(d >= 0) { subjectMatches = true; weight += (1 - d / range) * 10; }
                }
            }
            if(cause)
            {
                if(cause == p) { if(flags & FIND_ALLOW_CAUSE) causeMatches = true; else return 0; }
                else
                {
                    const float d = known(cause, false);
                    if(d >= 0) { causeMatches = true; weight += (1 - d / range) * 10; }
                }
            }
            if(!subjectMatches && !causeMatches) return 0;
        }
        return weight;
    }

    // ai_communication_find_specific_actor_to_talk (the subject's encounter, 2444-2499) and
    // _find_global_actor_to_talk (every actor of a team relation, 2501-2596).
    playerent *findActorToTalk(int team, int mode, int encounter, playerent *subject, playerent *cause, float range, int type,
                               int commPriority, int speechPriority, int vocalization, int flags)
    {
        playerent *best = nullptr;
        float bestWeight = 0;
        loopv(players)
        {
            playerent *p = players[i];
            if(!p || !alive(p) || !haloisactor(p)) continue;
            if(encounter >= 0)
            {
                int e = -1, squad = -1;
                if(!haloscenariosquadof(p, e, squad) || e != encounter) continue;
            }
            else if(team >= 0)
            {
                const int other = teamOf(p);
                const bool enemy = haloteamhostile(team, other);
                if(mode == FIND_SAME_TEAM ? other != team : mode == FIND_FRIEND ? enemy : !enemy) continue;
            }
            const float weight = talkWeight(p, subject, cause, range, encounter >= 0, type, commPriority, speechPriority, vocalization, flags);
            if(weight > bestWeight) { bestWeight = weight; best = p; }
        }
        return best;
    }

    // ai_communication_update_speech_timers (ai_communication.c:3246-3369).
    void updateSpeechTimers(Speaker &s, int priority, int usage)
    {
        const long long notification = ticks + max(0, s.soundTimer - overlapTime);
        s.lastFinished = notification;
        const int team = communicationTeam(s.unit);
        if(team < 0) return;
        if(priority <= SPEECH_SHOUT)
        {
            lastChatter[team] = max(lastChatter[team], notification);
            if(priority >= SPEECH_TALK) lastTalk[team] = max(lastTalk[team], notification);
            if(priority >= SPEECH_SHOUT) lastShout[team] = max(lastShout[team], notification);
        }
        if(usage >= 0)
        {
            EventStatus &e = events[usage][team];
            e.lastSpoken = ticks;
            if(usages[usage].repeatDelay > 0) e.disableUntil = (long long)(usages[usage].repeatDelay * 30) + notification;
        }
    }

    // The area friends of ai_communication_event's subject groups (actor_perception.c:1863-1940):
    // friends within 8 world units (group 0), or within 3 and in sight (group 1, close).
    void friendGroups(const playerent *unit, bool groups[2])
    {
        groups[0] = groups[1] = false;
        if(!unit || unit == player1 || !haloisactor(unit)) return;
        const float s = scale();
        auto consider = [&](playerent *p)
        {
            if(p == unit || !alive(p) || haloactorhostile(unit, p)) return;
            const float d = headOf(p).dist(headOf(unit)) / s;
            if(d < 8) groups[0] = true;
            if(d < 3 && sees(unit, p)) groups[0] = groups[1] = true;
        };
        consider(player1);
        loopv(players) if(players[i] && haloisactor(players[i])) consider(players[i]);
    }

    struct Possibility
    {
        float weight = 0;
        bool interrupts = false;
        int usage = -1, vocalization = -1, sound = -1, priority = 0, playType = PLAY_NONE, delay = 0, aiDelay = 0;
        playerent *speaker = nullptr;
    };

    void event(int type, playerent *subject, playerent *cause, int hostility, int damage, unsigned enemyStatus)
    {
        if(!halodialogueactive() || halocinematic()) return;
        if(hostility < 0 || hostility > HALO_HOSTILITY_TRAITOR) hostility = HALO_HOSTILITY_NONE;
        if(damage < 0) damage = 0;
        const int subjectTeam = subject ? teamOf(subject) : -1, causeTeam = cause ? teamOf(cause) : -1;
        // Allies whose allegiance broke speak of a traitor (ai_communication.c:6018-6141; the
        // allegiance incidents themselves are halo_ai.cpp's betrayal rule).
        if(subject && cause && subjectTeam != causeTeam && haloallegiancebroken(subjectTeam, causeTeam)) hostility = HALO_HOSTILITY_TRAITOR;
        const int subjectRace = raceOf(subject), causeRace = raceOf(cause);
        bool subjectGroups[2], causeGroups[2];
        friendGroups(subject, subjectGroups);
        friendGroups(cause, causeGroups);
        bool hostilityMatches[5] = {};
        hostilityMatches[hostility] = true;
        if(hostility == HALO_HOSTILITY_TRAITOR) hostilityMatches[HALO_HOSTILITY_ENEMY] = true;
        // The team timers (6254-6309): a slot is disabled while the team's chatter, talk or shout
        // timer is within its tolerance, and delayed by what is left. January's minimum-tolerance
        // test reads the slot's delay before it is written (the port's BUG note at 6290-6296), so
        // any slot with a positive minimum tolerance is never disabled, only delayed; kept.
        bool disabled[2][8][2] = {};
        int delays[2][8][2] = {};
        loopk(2) loopj(2) disabled[k][0][j] = true;
        loopk(2) for(int priority = 1; priority < COMM_YELL; ++priority) loopj(2)
        {
            const float *t = timerTolerances[priority][j];
            bool off = false;
            int delay = 0;
            auto gate = [&](float tolerance, long long last)
            {
                if(tolerance <= 0) return;
                const int remaining = int(tolerance * 30 - int(max(0LL, ticks - last)));
                if(remaining > 0) { off = true; delay = max(delay, remaining); }
            };
            gate(t[0], lastChatter[k]);
            gate(t[1], lastTalk[k]);
            gate(t[3], lastShout[k]);
            if(off && t[4] > 0) off = false;
            disabled[k][priority][j] = off;
            delays[k][priority][j] = delay;
        }
        if(!triggersEnabled) { if(halo_dialoguetrace) conoutf("COMM_EVENT type=%d DISABLED", type); return; }
        std::vector<Possibility> possibilities;
        bool forced = false;
        playerent *friendActor = nullptr, *enemyActor = nullptr;
        bool findFriend = true, findEnemy = true;
        int subjectEncounter = -1, squad = -1;
        if(subject && subject != player1 && haloisactor(subject)) haloscenariosquadof(subject, subjectEncounter, squad);
        const bool scripted = haloscenariodialogueplaying();
        for(int u = 0; u < usageCount; ++u)
        {
            const Usage &usage = usages[u];
            if(usage.type != type) continue;
            if(usage.hostility >= 0 && !hostilityMatches[usage.hostility]) continue;
            if(scripted && usage.priority < COMM_YELL && !(usage.flags & USAGE_OVERRIDE_SCRIPTED)) continue;
            if(usage.enemyStatus >= 0 && !(enemyStatus & (1u << usage.enemyStatus))) continue;
            if(usage.subjectRace >= 0 && (!subject || !(usage.subjectRace & subjectRace))) continue;
            if(usage.causeRace >= 0 && (!cause || !(usage.causeRace & causeRace))) continue;
            if(usage.damage >= 0 && usage.damage != damage) continue;
            const int speechPriority = commSpeechPriorities[usage.priority];
            playerent *protagonist = nullptr;
            const bool *groups = nullptr;
            int findFlags = (usage.flags & USAGE_LOOKUP ? FIND_LOOKUP : 0) | FIND_NEAR_PLAYERS | (usage.flags & USAGE_SAME_VEHICLE ? FIND_SAME_VEHICLE : 0) |
                            (usage.flags & USAGE_ALLOW_SUBJECT ? FIND_ALLOW_SUBJECT : 0);
            switch(usage.protagonist)
            {
                case PROTAGONIST_SUBJECT: protagonist = subject; groups = subjectGroups; break;
                case PROTAGONIST_CAUSE: protagonist = cause; groups = causeGroups; break;
                case PROTAGONIST_FRIEND:
                    if(findFriend)
                    {
                        findFriend = false;
                        friendActor = findActorToTalk(subjectTeam, FIND_FRIEND, subjectEncounter, subject, cause, 10, type, usage.priority, speechPriority,
                                                      usage.vocalization, findFlags | FIND_ALLOW_CAUSE);
                    }
                    protagonist = friendActor;
                    break;
                case PROTAGONIST_ENEMY:
                    if(findEnemy)
                    {
                        findEnemy = false;
                        enemyActor = findActorToTalk(subjectTeam, FIND_ENEMY, -1, subject, cause, 12, type, usage.priority, speechPriority,
                                                     usage.vocalization, findFlags);
                    }
                    protagonist = enemyActor;
                    break;
            }
            // A dead or missing protagonist, the player (whose lines are the reply table's, which
            // the engine does not have), a braindead actor.
            if(!protagonist || !alive(protagonist) || protagonist == player1 || (haloactorflags(protagonist) & HALO_AI_BRAINDEAD)) continue;
            if(usage.group >= 0 && groups && !groups[usage.group]) continue;
            const float rating = playerRating(protagonist, true);
            if(rating == 0) continue;
            const int nearPlayer = rating < lowPriorityRating ? 1 : 0;
            const int team = communicationTeam(protagonist);
            int candidateDelay = 0;
            float repeatRating = 1;
            if(team >= 0)
            {
                if(disabled[team][usage.priority][nearPlayer]) continue;
                candidateDelay = delays[team][usage.priority][nearPlayer];
                if(usage.priority < COMM_EXCLAIM)
                {
                    const EventStatus &e = events[u][team];
                    if(e.lastSpoken >= 0) repeatRating = clamp(float(ticks - e.lastSpoken) * 0.0011111111f, 0.0f, 1.0f);
                    if(e.disableUntil >= 0 && e.disableUntil - ticks + (nearPlayer ? lowPriorityTimeout : 0) > 0) continue;
                }
            }
            int playDelay = int(playDelays[usage.protagonist] * 30);
            if(subjectRace == RACE_PLAYER) playDelay += playerAdditionalDelay;
            const int notificationDelay = usage.flags & USAGE_IMMEDIATE_NOTIFY ? 0 : int(notificationDelays[usage.priority] * 30);
            Speaker *sp = speaker(protagonist, true);
            if(!sp) continue;
            int vocalization = usage.vocalization, sound = -1;
            float speechWeight = 1;
            const int playType = considerSpeech(*sp, usage.priority, speechPriority, playDelay + candidateDelay, (usage.flags & USAGE_LOOKUP) != 0,
                                                false, vocalization, speechWeight, sound);
            if(playType == PLAY_NONE) continue;
            // The animation impulse's weight (2 when the unit can play it) is left out: the engine's
            // bodies play no impulse animations.
            const float weight = speechWeight * rating * usage.weight * repeatRating;
            if(!(weight > 0)) continue;
            Possibility p;
            p.weight = weight;
            p.interrupts = (usage.flags & USAGE_FORCE) != 0;
            forced = forced || p.interrupts;
            p.usage = u;
            p.vocalization = vocalization;
            p.sound = sound;
            p.priority = speechPriority;
            p.playType = playType;
            p.delay = playDelay + candidateDelay;
            p.aiDelay = notificationDelay + candidateDelay;
            p.speaker = protagonist;
            possibilities.push_back(p);
            if(possibilities.size() >= 16) break;       // MAXIMUM_COMMUNICATION_POSSIBILITIES
        }
        if(halo_dialoguetrace)
            conoutf("COMM_EVENT type=%d subject=%s cause=%s hostility=%d damage=%d possibilities=%d", type, unitName(subject), unitName(cause),
                    hostility, damage, int(possibilities.size()));
        if(possibilities.empty()) return;
        float total = 0;
        for(Possibility &p : possibilities)
        {
            if(forced && !p.interrupts) p.weight = 0;
            total += p.weight;
        }
        size_t chosen = 0;
        if(possibilities.size() > 1)
        {
            const float roll = rndscale(1.0f) * total;
            float sum = 0;
            for(chosen = 0; chosen + 1 < possibilities.size(); ++chosen)
            {
                sum += possibilities[chosen].weight;
                if(sum >= roll) break;
            }
        }
        const Possibility &p = possibilities[chosen];
        Speaker *sp = speaker(p.speaker, false);
        if(!sp) return;
        SpeechItem item;
        item.priority = p.priority;
        item.vocalization = p.vocalization;
        item.sound = p.sound;
        item.delay = p.delay;
        item.pause = 24;
        item.aiDelay = p.aiDelay;
        item.usage = p.usage;
        speak(*sp, p.playType, item);
        updateSpeechTimers(*sp, p.priority, p.usage);
    }

    // The delayed ai_handle_damage calls a tick raised (run after every speaker has ticked: an event may
    // add speakers).
    struct DelayedHit { playerent *unit, *attacker; int category; float peak; };
    std::vector<DelayedHit> delayedHits;

    // unit_dialogue_update (unit_dialogue.c:968-1112), the recent damage's decay and the delayed damage
    // timer (units.c:5897-5915), one tick.
    void tick(Speaker &s)
    {
        if(s.decayTimer >= 0)
        {
            if(++s.decayTimer >= 60) s.recentBody = max(0.0f, s.recentBody - 0.016666668f);
            if(s.recentBody <= 0) s.decayTimer = -1;
        }
        if(s.shieldDecayTimer >= 0)
        {
            if(++s.shieldDecayTimer >= 60) s.recentShield = max(0.0f, s.recentShield - 0.016666668f);
            if(s.recentShield <= 0) s.shieldDecayTimer = -1;
        }
        if(s.delayedTimer > 0 && --s.delayedTimer == 0)
        {
            delayedHits.push_back({s.unit, s.delayedAttacker, s.delayedCategory, s.delayedPeak});
            s.delayedCategory = 0;
            s.delayedAttacker = nullptr;
            s.delayedPeak = 0;
        }
        if(s.minorDecay > 0 && --s.minorDecay == 0 && s.minorSounds > 0) { --s.minorSounds; s.minorDecay = 22; }
        // January decrements the minor timer twice and never the major one (the port's BUG note at
        // unit_dialogue.c:987-1000): once a unit's major pain has played it makes no other pain
        // sound for good (its deaths and screams still play). Kept.
        if(s.minorTimer > 0) --s.minorTimer;
        if(s.minorTimer > 0) --s.minorTimer;
        if(s.current.priority > SPEECH_NONE)
        {
            if(s.preDelay > 0) --s.preDelay;
            else
            {
                if(!s.played) { play(s); s.played = true; }
                if(s.aiDelay > 0) --s.aiDelay;
                if(s.soundTimer > 0) --s.soundTimer;
                else
                {
                    s.finished = true;
                    if(s.postDelay > 0) --s.postDelay;
                    if(s.postDelay == 0) s.aiDelay = 0;
                }
            }
        }
        if(s.aiDelay == 0) s.notified = true;
        if(s.current.priority > SPEECH_NONE && s.soundTimer == 0 && s.postDelay == 0) s.current.priority = SPEECH_NONE;
        if(s.current.priority == SPEECH_NONE && s.queued.priority > SPEECH_NONE)
        {
            const SpeechItem queued = s.queued;
            speak(s, PLAY_IMMEDIATE_DEQUEUE, queued);
        }
    }

    float maximumHealth(const playerent *p)
    {
        if(p == player1) return 75;
        const int start = haloactorstarthealth(p);
        return start > 0 ? float(start) : 100.0f;
    }

    // ai_handle_damage's communication (ai/ai.c:1316-1380) for a hit that did not kill (`delayed`: the
    // delayed call on the peak of the recent damage). `responsible` is the unit behind the hit (nobody
    // for an ownerless one), `fraction` what Halo passes: the hit's total_damage in damage points
    // (shield plus body before the vitality normalization, units/units.c:6759), or for the delayed call
    // the peak of the normalized recent shield plus body damage (6812-6832; the port's own mix of
    // units, followed as written). A friend's hit raises damage (hurt friend, damaged friend) unless
    // delayed; otherwise 0.3 or more raises hurt (the rows want an enemy). The falling category is excluded only from
    // actor_handle_damage there, not from these events.
    void handleDamage(playerent *unit, playerent *responsible, int category, float fraction, bool delayed)
    {
        // A dead unit's delayed call is dropped (judgement call: its death event spoke for the kill, and
        // a corpse's actor, so its team, is gone once halokilled detaches it).
        if(!unit || !present(unit) || (delayed && !alive(unit))) return;
        if(responsible && !present(responsible)) responsible = nullptr;
        int hostility = HALO_HOSTILITY_NONE;
        if(responsible == unit) hostility = HALO_HOSTILITY_SELF;
        else if(responsible)
            hostility = haloteamhostile(teamOf(unit), halodamageteam(responsible)) ? HALO_HOSTILITY_ENEMY : HALO_HOSTILITY_FRIEND;
        if(halo_dialoguetrace)
            conoutf("COMM_DAMAGE unit=%s cause=%s hostility=%d category=%d fraction=%.3f delayed=%d", unitName(unit), unitName(responsible),
                    hostility, category, fraction, int(delayed));
        if(!delayed && hostility == HALO_HOSTILITY_FRIEND) event(HALO_COMM_DAMAGE, unit, responsible, HALO_HOSTILITY_FRIEND, category, 0x3f);
        else if(fraction >= 0.3f) event(HALO_COMM_HURT, unit, responsible, hostility, category, 0x3f);
    }

    // unit_make_damage_sound (unit_dialogue.c:725-886) for the hit on the unit `s` speaks for.
    void damageSound(Speaker &s, const HaloHitReport &report, float body, bool died)
    {
        playerent *const unit = s.unit;
        int vocalization = -1;
        bool involuntary = false;
        const bool tookBody = s.recentBody > 0, tookMajor = s.recentBody >= 0.6f;
        if(died)
        {
            bool severe = false;
            const bool instantaneous = report.acceleration >= 2;
            if(haloisactor(unit)) severe = haloactorstatus(unit) >= 6;      // combat_status >= definite
            else severe = s.recentBody > body + 0.2f;
            if(report.category == CATEGORY_FALLING) vocalization = VOC_DEATH_FALLING;
            else if(report.category == CATEGORY_FLAME) vocalization = VOC_DEATH_AGONIZING;
            else if(instantaneous) vocalization = VOC_DEATH_FLYING;
            else if(!severe) vocalization = VOC_DEATH_QUIET;
            else vocalization = report.instant ? VOC_DEATH_INSTANT : VOC_DEATH_VIOLENT;
            involuntary = true;
        }
        else if(s.majorTimer == 0)
        {
            if(report.category == CATEGORY_FALLING) { involuntary = true; vocalization = VOC_PAIN_FALLING; }
            else if(tookMajor) { involuntary = true; vocalization = VOC_PAIN_BODY_MAJOR; }
            else if(s.minorTimer == 0 && s.minorSounds < 3 && (s.current.priority == SPEECH_NONE || rndscale(1.0f) < 0.4f))
                vocalization = tookBody ? VOC_PAIN_BODY : VOC_PAIN_SHIELD;
        }
        if(vocalization < 0) return;
        int sound = -1;
        const int priority = died ? SPEECH_DEATH : involuntary ? SPEECH_INVOLUNTARY : SPEECH_PAIN;
        const int playType = testSpeech(s, priority, true, false, nullptr, vocalization, sound);
        if(playType == PLAY_NONE) return;
        SpeechItem item;
        item.priority = priority;
        item.vocalization = vocalization;
        item.sound = sound;
        item.pause = 7;
        speak(s, playType, item);
        if(tookMajor) s.majorTimer = 60;
        else
        {
            ++s.minorSounds;
            s.minorTimer = 30;
            s.minorDecay = 22;
        }
    }
}

bool halodialogueactive() { return halo_dialogue && haloactive() && load(); }

void halodialoguereset()
{
    speakers.clear();
    ticks = 0;
    tickRemainder = 0;
    tickMillis = -1;
    triggersEnabled = true;
    actOfGod = 0;
    loopi(2) lastChatter[i] = lastTalk[i] = lastShout[i] = 0;
    for(auto &row : events) for(EventStatus &e : row) e = EventStatus();
}

void halodialogueforget(const playerent *unit)
{
    if(!unit) return;
    speakers.erase(std::remove_if(speakers.begin(), speakers.end(), [unit](const Speaker &s) { return s.unit == unit; }), speakers.end());
    for(Speaker &s : speakers) if(s.delayedAttacker == unit) s.delayedAttacker = nullptr;
}

void halodialogueupdate()
{
    if(!halodialogueactive()) { tickMillis = -1; return; }
    if(tickMillis < 0 || lastmillis < tickMillis) tickMillis = lastmillis;
    tickRemainder += (lastmillis - tickMillis) * 30;
    tickMillis = lastmillis;
    // A unit gone from the game (an erased or kicked bot) loses its speech.
    speakers.erase(std::remove_if(speakers.begin(), speakers.end(), [](const Speaker &s) { return !present(s.unit); }), speakers.end());
    for(Speaker &s : speakers) if(s.delayedAttacker && !present(s.delayedAttacker)) s.delayedAttacker = nullptr;
    for(int steps = 0; tickRemainder >= 1000 && steps < 30; ++steps)
    {
        tickRemainder -= 1000;
        ++ticks;
        delayedHits.clear();
        for(Speaker &s : speakers) tick(s);
        // The delayed ai_handle_damage calls, after the loop: their events may add speakers.
        for(const DelayedHit &h : delayedHits) handleDamage(h.unit, h.attacker, h.category, h.peak, true);
    }
    delayedHits.clear();
    tickRemainder = min(tickRemainder, 1000);
}

void halodialogueactofgod(bool on) { actOfGod = max(0, actOfGod + (on ? 1 : -1)); }

void halodialoguetriggers(bool on) { triggersEnabled = on; }

void halodialoguedamage(playerent *unit, playerent *attacker, int healthLost, bool died, int gun)
{
    (void)gun;
    if(!halodialogueactive() || !unit) return;
    HaloHitReport report;
    const bool hit = halohitreport(unit, report);
    if(!hit)
    {
        // A kill dealt outside the damage model (the INT_MAX kills): Halo's act of god deals the
        // globals' falling jpt! (objects/damage.c:1830-1872), so ai_kill's dead speak a falling
        // death; the silent ones (ai_kill_silent, ai_erase, a command list's die atom, the recovery's
        // and the engine's own removals) stay silent.
        if(!died || !actOfGod) return;
        report.category = CATEGORY_FALLING;
        report.instant = true;
    }
    Speaker *sp = speaker(unit, true);
    if(!sp && died)
    {
        // The unit died before it ever spoke: it still has its dialogue for the death.
        speakers.emplace_back();
        sp = &speakers.back();
        sp->unit = unit;
        sp->dialogue = dialogueFor(unit);
    }
    if(!sp) return;
    // ai_get_responsible_unit (ai/ai.c:931-965): nobody for an ownerless hit (dealt in the player's name).
    playerent *responsible = hit ? attacker : nullptr;
    if(responsible == player1 && haloactorownerlessdamage()) responsible = nullptr;
    const float body = clamp(healthLost / maximumHealth(unit), 0.0f, 1.0f), shieldDamage = hit ? report.shieldDamage : 0.0f;
    if(body > 0)
    {
        sp->recentBody = min(1.0f, sp->recentBody + body);
        sp->decayTimer = 0;
    }
    if(shieldDamage > 0)
    {
        sp->recentShield = min(1.0f, sp->recentShield + shieldDamage);
        sp->shieldDecayTimer = 0;
    }
    // The delayed damage (units.c:6812-6832): restarted at 45 ticks by every hit with recent damage, on
    // the peak of the recent shield and body damage, keeping the last attacker that had an owner.
    const float recent = sp->recentShield + sp->recentBody;
    if(hit && recent > 0)
    {
        sp->delayedCategory = max(report.category, 0);
        sp->delayedTimer = 45;
        sp->delayedPeak = max(sp->delayedPeak, recent);
        if(responsible) sp->delayedAttacker = responsible;
    }
    // unit_damage_aftermath's test (units/units.c:6966-6987): the body depleted, or any body or shield damage.
    if(!died && !(body > 0) && !report.shield) return;
    if(sp->dialogue >= 0) damageSound(*sp, report, body, died);
    // Then ai_handle_damage for a biped the hit did not kill (units.c:6993-7013; a kill raises the death
    // event instead, halodialoguedeath). `sp` is not used past this: the event may add speakers.
    // Its fraction is total_damage (units.c:6759, 7004-7011): the hit's damage points, so nearly every
    // hit passes the 0.3 test; only the delayed call's peak is normalized.
    if(hit && !died) handleDamage(unit, responsible, report.category, report.points, false);
}

void halodialoguedeath(playerent *victim, playerent *killer)
{
    if(!halodialogueactive() || !victim) return;
    HaloHitReport report;
    const bool hit = halohitreport(victim, report);
    if(!hit && !actOfGod) return;           // a silent removal (above)
    // ai_get_responsible_unit and the hostility (ai/ai.c:928-1003): nobody for an ownerless kill.
    if(haloactorownerlessdamage() && killer == player1) killer = nullptr;
    if(!hit) killer = nullptr;
    int hostility = HALO_HOSTILITY_NONE;
    if(killer == victim) hostility = HALO_HOSTILITY_SELF;
    else if(killer) hostility = haloteamhostile(teamOf(victim), halodamageteam(killer)) ? HALO_HOSTILITY_ENEMY : HALO_HOSTILITY_FRIEND;
    event(HALO_COMM_DEATH, victim, killer, hostility, hit ? report.category : CATEGORY_FALLING, 0x3f);
}

void halodialoguescream(playerent *unit, int type)
{
    if(!halodialogueactive() || !alive(unit)) return;
    // unit_scream (unit_dialogue.c:887-966).
    int vocalization = -1;
    switch(type)
    {
        case HALO_SCREAM_FALLING: vocalization = VOC_SCREAM_FEAR; break;
        case HALO_SCREAM_GRENADE_ATTACHED: vocalization = rndscale(1.0f) < 0.5f ? VOC_HURT_ENEMY_GRENADE : VOC_SCREAM_PAIN; break;
        case HALO_SCREAM_BURNING: vocalization = VOC_SCREAM_PAIN; break;
        default: return;
    }
    Speaker *sp = speaker(unit, true);
    if(!sp || sp->dialogue < 0 || dialogues[sp->dialogue].vocalization[vocalization] < 0) return;
    int sound = -1;
    const int playType = testSpeech(*sp, SPEECH_SCREAM, true, false, nullptr, vocalization, sound);
    if(playType == PLAY_NONE) return;
    SpeechItem item;
    item.priority = SPEECH_SCREAM;
    item.vocalization = vocalization;
    item.sound = sound;
    item.pause = 7;
    speak(*sp, playType, item);
}

void halodialogueevent(int type, playerent *subject, playerent *cause, int hostility, int damage, unsigned enemyStatus)
{
    event(type, subject, cause, hostility, damage, enemyStatus);
}

void halodialoguedump(const char *label)
{
    conoutf("DIALOGUE_STATE label=%s loaded=%d active=%d dialogues=%d sounds=%d units=%d speakers=%d ticks=%lld triggers=%d", label ? label : "",
            int(load()), int(halodialogueactive()), int(dialogues.size()), int(sounds.size()), int(units.size()), int(speakers.size()), ticks,
            int(triggersEnabled));
    for(const Speaker &s : speakers)
        conoutf("DIALOGUE_UNIT label=%s unit=%s dialogue=%s priority=%s vocalization=%d queued=%s timer=%d lines=%d recent=%.3f major=%d minor=%d",
                label ? label : "", unitName(s.unit), dialogueName(s.dialogue), priorityNames[s.current.priority], s.current.vocalization,
                priorityNames[s.queued.priority], s.soundTimer, s.lines, s.recentBody, s.majorTimer, s.minorSounds);
}
