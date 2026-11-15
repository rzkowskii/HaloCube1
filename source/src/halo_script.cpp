#include "cube.h"
#include "halo.h"
#include "halo_campaign.h"
#include "halo_collision.h"
#include "halo_script.h"
#include "halo_fx.h"
#include "halo_ai.h"
#include "halo_weapons.h"
#include "halo_vehicle.h"
#include "halo_hud.h"
#include "halo_state.h"
#include <algorithm>
#include <functional>
#include <cmath>
#include <memory>
#include <random>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

VARP(halo_scripttrace, 0, 0, 1);
// 0 Easy, 1 Normal, 2 Hard, 3 Impossible. Read when a section starts or restarts.
VARP(halo_difficulty, 0, 1, 3);
// A seat the script holds the player in (Scenario::seatPlayer): 0 leaves unit_enter_vehicle of the
// player into a vehicle the engine does not drive ignored, as before; halo_scriptseatmax is the
// longest the hold lasts (seconds, 0 for as long as the script keeps it), a guard against a script
// that never reaches its unit_exit_vehicle.
VAR(halo_scriptseat, 0, 1, 1);
VAR(halo_scriptseatmax, 0, 180, 3600);
// effect_new and effect_new_on_object_marker also deal the effect's jpt! area damage
// (effects.txt `damage` lines, haloeffectblast): off by default, an optional fidelity item. The
// explosions the scripts create (effects\explosions\...) carry the medium explosion's jpt!, 15 to
// 25 inside 1.5 world units and 5 at 2.5; their cinematic variants carry none. With it on, a
// scripted explosion hurts what stands in it, which the planned routes were not made for.
VAR(halo_effectdamage, 0, 0, 1);

int halodifficulty() { return clamp(int(halo_difficulty), 0, 3); }
// Enemy damage and vitality relative to Normal, and the player's incoming damage.
float halodifficultydamage() { static const float scale[4] = {0.6f, 1.0f, 1.3f, 1.7f}; return scale[halodifficulty()]; }
float halodifficultyhealth() { static const float scale[4] = {0.75f, 1.0f, 1.25f, 1.5f}; return scale[halodifficulty()]; }

namespace
{
    constexpr uint32_t nil = UINT32_MAX;
    constexpr int ticksPerSecond = 30;
    constexpr int noObject = -1, playerObject = -2, playerList = -3;
    // Actors and lists of actors are object values below these bases: an actor
    // is actorObject - its squadBots index, a list is actorList - its actorLists index.
    constexpr int actorObject = -1000, actorList = -100000;
    // 343 Guilty Spark's body (halo.cpp ActorKind MONITOR, tools/halo_actors.py KINDS 10): the
    // scripts cannot kill him (eraseSquads).
    constexpr int monitorKind = 10;
    bool monitorBody(const playerent *p) { return haloenemykind(p) == monitorKind; }

    struct Reader
    {
        std::unique_ptr<stream> file;
        bool valid;

        explicit Reader(const char *path) : file(openfile(path, "rb")), valid(file && file->size() <= 4 * 1024 * 1024) {}

        bool read(void *data, int size)
        {
            if(valid && (!size || file->read(data, size) == size)) return true;
            valid = false;
            return false;
        }

        template<class T> T number()
        {
            T value = {};
            if(read(&value, sizeof(value))) lilswap(&value, 1);
            return value;
        }

        std::string bytes(unsigned size)
        {
            if(size > 1024 * 1024) { valid = false; return {}; }
            std::string value(size, '\0');
            if(size) read(&value[0], int(size));
            return value;
        }

        std::string text() { return bytes(number<uint16_t>()); }

        template<class T, class F> std::vector<T> rows(unsigned maximum, F row)
        {
            unsigned count = number<uint32_t>();
            if(count > maximum) { valid = false; return {}; }
            std::vector<T> result;
            result.reserve(count);
            for(unsigned i = 0; i < count && valid; ++i) result.push_back(row());
            return result;
        }

        vec point()
        {
            float x = number<float>(), y = number<float>(), z = number<float>();
            return vec(x, y, z);
        }
    };

    struct Node
    {
        uint16_t salt, opcode, type, flags;
        uint32_t next, text, data;
    };

    struct Script { std::string name; int kind, type; uint32_t root; };
    struct Global { std::string name; int type; uint32_t root; };
    struct Pose { vec position; float yaw = 0, pitch = 0, roll = 0, fov = 70; };
    struct Camera { std::string name; Pose pose; };
    struct Flag { std::string name; Pose pose; };
    struct Volume { std::string name; int kind; vec forward, up, origin, extent; };
    struct Title { std::string name, text; vec times; };
    struct HudMessage { std::string name, text; };
    struct RecordedInput { int tick, state, flags; float forward, left, yaw, pitch, aimYaw, aimPitch, lookYaw, lookPitch; };
    struct Recording { std::string name; int ticks; std::vector<RecordedInput> inputs; };
    struct Seat { std::string name, marker; };
    struct Object
    {
        std::string name, model;
        Pose pose;
        int kind;
        float forward, reverse, acceleration, deceleration, turn, slide;
        vec center;
        std::vector<Flag> markers;
        std::vector<Seat> seats;
        std::string locomotion[3];
        // Version 8: a damage target (d40's manifold detectors, tools/halo_cast.py damage_target): a
        // scripted unit with no AI that the player's shots and blasts hurt. `vitality` is its body
        // vitality (0 for every other object), the box its collision in its own frame, and `placed`
        // whether it exists from the start (the other scripted objects appear when a script creates them).
        float vitality = 0;
        vec targetLow, targetHigh;
        bool targetPlaced = false;
    };
    struct Animation { int object, graph; std::string clip, model; int ticks; };
    struct Sound { int id, duration, scriptDuration; std::string subtitle; };
    struct Line { int delay; std::vector<int> variants; };
    struct Conversation { std::string name; std::vector<Line> lines; };
    struct Track { float gain; int start, loop, end, alt, altEnd; };
    struct Loop { int id; bool repeat; std::vector<Track> tracks; };
    // Version 6 adds per-start AI data: actors.txt index, states and command list.
    struct SquadStart { int actor; vec point; float yaw; int variant = -1, initial = 0, defaultState = 0, commandList = -1; };
    struct FiringPosition { vec point; int group, cluster; };
    struct Platoon { unsigned flags; int attackRule, attackPlatoon, maneuverRule, maneuverPlatoon; };
    struct Squad
    {
        std::string name;
        int platoon;
        std::vector<SquadStart> starts;
        int initial = 0, defaultState = 0, maneuver = -1;
        unsigned flags = 0, masks[7] = {};
        int counts[4] = {-1, -1, -1, -1};  // starts placed on Easy, Normal, Hard, Impossible; -1 = all (version 5 and 6)
        // Version 9: the squad's default variant (an ACTORS kind, -1 when not one, and an
        // actors.txt index, -1 unknown), what ai_attach turns a scripted biped into.
        int defaultKind = -1, defaultVariant = -1;
    };
    struct Encounter
    {
        std::string name;
        int team, bsp;
        std::vector<Squad> squads;
        std::vector<FiringPosition> positions;
        std::vector<Platoon> platoons;
    };
    // Version 9 object flags.
    enum { OBJECT_STORY_FIGURE = 1 };
    struct CommandAtom { int type, modifier; float first, second; int point, otherPoint, animation, script, recording, command, object; };
    struct CommandList { std::string name; unsigned flags; std::vector<CommandAtom> atoms; std::vector<vec> points; };
    struct Projection { int bsp; vec origin; float xy, z, margin; };
    struct BspSwitch { int volume, from, to, flag; };
    struct DeviceBox { vec low, high; };
    struct DevicePanel { int node; std::vector<DeviceBox> frames; };
    struct Device
    {
        int object, powerGroup, positionGroup, elevator;
        unsigned flags, attributes, notPlaced, loopFlags, bspMask;
        bool control;
        int kind, typeFlags, hold;
        float call, radius, powerAccel, powerSpeed, offAccel, offSpeed, onAccel, onSpeed;
        std::string model;
        int frames;
        Pose pose;
        std::vector<DevicePanel> panels;
    };

    struct Program
    {
        std::vector<Node> nodes;
        std::string strings;
        std::vector<Script> scripts;
        std::vector<Global> globals;
        std::vector<Camera> cameras;
        std::vector<Flag> flags;
        std::vector<Volume> volumes;
        std::vector<Title> titles;
        std::vector<Recording> recordings;
        std::vector<Object> objects;
        std::vector<Animation> animations;
        std::vector<Sound> sounds;
        std::vector<Conversation> conversations;
        std::vector<Loop> loops;
        std::vector<Projection> projections;
        std::vector<int> worlds;
        std::vector<HudMessage> messages;
        std::vector<float> deviceGroups;
        std::vector<unsigned> groupFlags;
        std::vector<Device> devices;
        std::vector<BspSwitch> switches;
        std::vector<Encounter> encounters;
        std::vector<CommandList> commandLists;
        std::vector<HaloStartingProfile> profiles;  // version 9: player starting profiles
        // Per object (version 9): OBJECT_* flags; empty with an older file (storyFigure then
        // falls back to the old name prefixes).
        std::vector<unsigned char> objectFlags;

        const Node *node(uint32_t datum) const
        {
            unsigned index = datum & 0xffff;
            if(datum == nil || index >= nodes.size()) return nullptr;
            const Node &result = nodes[index];
            return result.salt && result.salt == datum >> 16 ? &result : nullptr;
        }

        const char *text(uint32_t offset) const
        {
            return offset < strings.size() ? strings.c_str() + offset : "";
        }

        bool load(const char *cache)
        {
            defformatstring(path)("packages/halo/%s.hscb", cache);
            Reader r(path);
            if(r.bytes(4) != "HSCB") return false;
            const uint32_t version = r.number<uint32_t>();
            if(version < 5 || version > 9) return false;
            auto u8 = [&] { return r.number<uint8_t>(); };
            auto u16 = [&] { return r.number<uint16_t>(); };
            auto i16 = [&] { return r.number<int16_t>(); };
            auto u32 = [&] { return r.number<uint32_t>(); };
            auto i32 = [&] { return r.number<int32_t>(); };
            auto f32 = [&] { return r.number<float>(); };
            auto pose = [&]
            {
                Pose p;
                p.position = r.point();
                p.yaw = f32();
                p.pitch = f32();
                return p;
            };
            nodes = r.rows<Node>(20000, [&] { return Node{u16(), u16(), u16(), u16(), u32(), u32(), u32()}; });
            strings = r.bytes(u32());
            scripts = r.rows<Script>(1024, [&] { return Script{r.text(), u16(), u16(), u32()}; });
            globals = r.rows<Global>(512, [&] { return Global{r.text(), u16(), u32()}; });
            cameras = r.rows<Camera>(1024, [&]
            {
                Camera camera{r.text(), pose()};
                camera.pose.roll = f32();
                float authoredFov = f32();
                camera.pose.fov = authoredFov > 0 ? authoredFov / RAD : 70.0f;
                return camera;
            });
            flags = r.rows<Flag>(1024, [&] { return Flag{r.text(), pose()}; });
            volumes = r.rows<Volume>(1024, [&] { return Volume{r.text(), u16(), r.point(), r.point(), r.point(), r.point()}; });
            titles = r.rows<Title>(128, [&] { return Title{r.text(), r.text(), r.point()}; });
            recordings = r.rows<Recording>(2048, [&]
            {
                return Recording{r.text(), u16(), r.rows<RecordedInput>(65536, [&]
                {
                    return RecordedInput{u16(), u8(), u16(), f32(), f32(), f32(), f32(), f32(), f32(), f32(), f32()};
                })};
            });
            objects = r.rows<Object>(4096, [&]
            {
                Object object{};
                object.name = r.text();
                object.model = r.text();
                object.pose = pose();
                object.pose.roll = f32();
                object.kind = i16();
                object.forward = f32();
                object.reverse = f32();
                object.acceleration = f32();
                object.deceleration = f32();
                object.turn = f32();
                object.slide = f32();
                object.center = r.point();
                object.markers = r.rows<Flag>(512, [&] { return Flag{r.text(), pose()}; });
                object.seats = r.rows<Seat>(64, [&] { return Seat{r.text(), r.text()}; });
                for(std::string &path : object.locomotion) path = r.text();
                if(version >= 8)
                {
                    object.vitality = f32();
                    object.targetLow = r.point();
                    object.targetHigh = r.point();
                    object.targetPlaced = u8() != 0;
                    if(!std::isfinite(object.vitality) || object.vitality < 0) object.vitality = 0;
                }
                return object;
            });
            animations = r.rows<Animation>(2048, [&] { return Animation{u16(), u16(), r.text(), r.text(), u16()}; });
            sounds = r.rows<Sound>(4096, [&] { return Sound{u16(), int(u32()), int(u32()), r.text()}; });
            conversations = r.rows<Conversation>(512, [&]
            {
                return Conversation{r.text(), r.rows<Line>(32, [&]
                {
                    return Line{u16(), r.rows<int>(6, [&] { return int(u16()); })};
                })};
            });
            loops = r.rows<Loop>(1024, [&]
            {
                return Loop{u16(), r.number<uint8_t>() != 0, r.rows<Track>(4, [&]
                {
                    return Track{f32(), i32(), i32(), i32(), i32(), i32()};
                })};
            });
            projections = r.rows<Projection>(64, [&] { return Projection{u16(), r.point(), f32(), f32(), f32()}; });
            worlds = r.rows<int>(64, [&] { return int(u16()); });
            messages = r.rows<HudMessage>(1024, [&] { return HudMessage{r.text(), r.text()}; });
            deviceGroups = r.rows<float>(512, f32);
            encounters = r.rows<Encounter>(512, [&]
            {
                return Encounter{r.text(), i16(), i16(), r.rows<Squad>(64, [&]
                {
                    return Squad{r.text(), i16(), r.rows<SquadStart>(256, [&]
                    {
                        return SquadStart{i16(), r.point(), f32()};
                    })};
                })};
            });
            switches = r.rows<BspSwitch>(1024, [&] { return BspSwitch{i16(), i16(), i16(), i16()}; });
            groupFlags = r.rows<unsigned>(512, u32);
            devices = r.rows<Device>(4096, [&]
            {
                Device d{i16(), i16(), i16(), i16(), u32(), u32(), u32(), u32(), u32(), u8() != 0,
                         u16(), u16(), i32(), f32(), f32(), f32(), f32(), f32(), f32(), f32(), f32()};
                d.model = r.text();
                d.frames = u16();
                d.pose = pose();
                d.pose.roll = f32();
                d.panels = r.rows<DevicePanel>(128, [&]
                {
                    return DevicePanel{u16(), r.rows<DeviceBox>(1024, [&] { return DeviceBox{r.point(), r.point()}; })};
                });
                return d;
            });
            if(version >= 6)
            {
                for(Encounter &e : encounters)
                {
                    e.positions = r.rows<FiringPosition>(4096, [&] { vec p = r.point(); int g = i16(); return FiringPosition{p, g, i16()}; });
                    e.platoons = r.rows<Platoon>(256, [&] { unsigned f = u32(); int a = i16(), b = i16(), c = i16(); return Platoon{f, a, b, c, i16()}; });
                    const uint32_t count = r.number<uint32_t>();
                    if(count != e.squads.size()) return false;
                    for(Squad &squad : e.squads)
                    {
                        squad.initial = i16();
                        squad.defaultState = i16();
                        squad.flags = u32();
                        squad.maneuver = i16();
                        loopi(7) squad.masks[i] = u32();
                        if(version >= 7) loopi(4) squad.counts[i] = i16();
                        const uint32_t starts = r.number<uint32_t>();
                        if(starts != squad.starts.size()) return false;
                        for(SquadStart &start : squad.starts)
                        {
                            start.variant = i16();
                            start.initial = i16();
                            start.defaultState = i16();
                            start.commandList = i16();
                        }
                    }
                }
                commandLists = r.rows<CommandList>(1024, [&]
                {
                    CommandList list;
                    list.name = r.text();
                    list.flags = u32();
                    list.atoms = r.rows<CommandAtom>(256, [&]
                    {
                        CommandAtom a;
                        a.type = i16(); a.modifier = i16(); a.first = f32(); a.second = f32();
                        a.point = i16(); a.otherPoint = i16(); a.animation = i16(); a.script = i16();
                        a.recording = i16(); a.command = i16(); a.object = i16();
                        return a;
                    });
                    list.points = r.rows<vec>(256, [&] { return r.point(); });
                    return list;
                });
            }
            if(version >= 9)
            {
                profiles = r.rows<HaloStartingProfile>(64, [&]
                {
                    HaloStartingProfile profile;
                    profile.name = r.text();
                    profile.health = f32();
                    profile.shield = f32();
                    loopi(2)
                    {
                        profile.weapon[i] = r.text();
                        profile.loaded[i] = max(0, int(i16()));
                        profile.total[i] = max(0, int(i16()));
                    }
                    profile.frags = u8();
                    profile.plasmas = u8();
                    if(!std::isfinite(profile.health) || profile.health < 0) profile.health = 0;
                    if(!std::isfinite(profile.shield) || profile.shield < 0) profile.shield = 0;
                    return profile;
                });
                if(r.number<uint32_t>() != encounters.size()) return false;
                for(Encounter &e : encounters)
                {
                    if(r.number<uint32_t>() != e.squads.size()) return false;
                    for(Squad &squad : e.squads)
                    {
                        squad.defaultKind = i16();
                        squad.defaultVariant = i16();
                    }
                }
                objectFlags = r.rows<unsigned char>(4096, u8);
                if(objectFlags.size() != objects.size()) return false;
            }
            if(!r.valid || r.file->tell() != r.file->size() || nodes.empty() || scripts.empty()) return false;
            if(groupFlags.size() != deviceGroups.size()) return false;
            for(const Script &script : scripts) if(!node(script.root)) return false;
            for(const Global &global : globals) if(!node(global.root)) return false;
            // A machine panel with no frames has no box to collide with (panelBox reads frame 0):
            // drop it rather than the whole program.
            for(Device &device : devices)
                device.panels.erase(std::remove_if(device.panels.begin(), device.panels.end(),
                                                   [](const DevicePanel &panel) { return panel.frames.empty(); }),
                                    device.panels.end());
            for(const Projection &projection : projections)
                if(!std::isfinite(projection.xy) || !std::isfinite(projection.z) || projection.xy <= 0 || projection.z <= 0) return false;
            return true;
        }
    };

    struct Value
    {
        double number = 0;
        const char *text = "";
        int type = 4;

        int integer() const
        {
            return std::isfinite(number) && number >= INT_MIN && number <= INT_MAX ? int(number) : 0;
        }

        bool truth() const { return type == 9 ? *text != '\0' : number != 0; }
    };

    Value number(double value, int type = 8) { return Value{value, "", type}; }

    template<class T> const T *at(const std::vector<T> &values, int index)
    {
        return index >= 0 && size_t(index) < values.size() ? &values[index] : nullptr;
    }

    template<class T> int named(const std::vector<T> &values, const char *name)
    {
        for(size_t i = 0; i < values.size(); ++i) if(values[i].name == name) return int(i);
        return -1;
    }

    struct Frame
    {
        uint32_t expression, next = nil;
        int phase = 0, deadline = 0, interval = 30;
        Value result;
        std::vector<Value> arguments;
        std::vector<uint32_t> shuffled;
        explicit Frame(uint32_t expression) : expression(expression) {}
    };

    // The one HUD countdown, and the tutorial prompts that wait for the player to act.
    struct HudTimer { bool shown = false, running = false; int remaining = 0, end = 0; };
    struct TutorialPoll { int first = 0; float yaw = 0, pitch = 0; };
    // A nav point the script shows the player: on a cutscene flag or on an object, raised by
    // `offset` Halo world units, with the retail waypoint arrow type ("default_red").
    struct NavPoint { int flag = -1, object = -1; std::string type; float offset = 0; };

    struct Thread
    {
        std::vector<Frame> stack;
        int wake = 0;
        bool started = false, done = false, disabled = false;
        Value result;
    };

    struct Actor
    {
        Pose pose;
        bool visible = false, animationLoop = false, recordingDeletes = false, recordingHover = false;
        bool grounded = false;
        float scale = 1, speed = 0;
        vec velocity = vec(0, 0, 0);
        int animationStart = 0, animationEnd = 0, recordingStart = 0, recordingEnd = 0;
        int parent = noObject;
        int gait = -1, gaitStart = 0;
        int baseSeat = -1;
        // Script-set object state: unit_open, vehicle_hover, object_cannot_take_damage and friends.
        bool open = false, hovering = true, collideable = true, enterable = true, suspended = false;
        bool cannotTakeDamage = false, meleeInhibited = false, rangedInhibited = false;
        int squadReference = -1;   // ai_attach: the squad this unit belongs to, -1 when free
        float damage = 0;          // body damage a damage target has taken (Object::vitality)
        // unit_set_enterable_by_player true on a vehicle the engine does not drive (a30's
        // extraction Pelicans): the player may board it with E, and the scripts' seat tests for
        // the player answer whether they did (halo_script_ai.inc).
        bool boardable = false;
        std::string permutation;
        size_t input = 0;
        std::string parentMarker, childMarker;
        const Recording *recording = nullptr;
        const Animation *animation = nullptr;
    };

    struct DeviceState
    {
        float power = 1, position = 0, powerVelocity = 0, positionVelocity = 0;
        float desiredPower = 1, desiredPosition = 0;
        int hold = 0;
        bool visible = true, automatic = true, oneSided = false, changed = false;
    };

    struct DeviceGroup { float value; bool once, changed; };

    struct CameraView
    {
        vec position, forward = vec(1, 0, 0), up = vec(0, 0, 1);
        float fov = 70;
    };

    struct CameraMotion
    {
        CameraView view, target;
        vec positionVelocity, positionAcceleration, angularVelocity, angularAcceleration;
        float fovVelocity = 0, fovAcceleration = 0;
        int end = 0, relative = noObject, firstPerson = noObject;
        bool set = false;
    };

    struct Fade { vec color; float from = 0, to = 0; int start = 0, end = 0; };
    struct SquadBot
    {
        const SquadStart *start;
        playerent *bot;
        int encounter, squad;
        bool suspended = false;
        int scriptFlags = 0;   // HALO_AI_* bits scripts set, reapplied when a revert respawns the actor
        // ai_attach: the scripted object this actor stands for (-1 an encounter's own actor). Its
        // start is Scenario::objectStarts[object]: where it took over (its spawn, which
        // ai_teleport_to_starting_location goes back to).
        int object = -1;
        // An attached actor's checkpoint copy (savedSquadBots): where it stood when the checkpoint
        // was taken (scenario units, as SquadStart), which a revert or Continue brings it back to;
        // its start stays its spawn.
        bool stood = false;
        vec stoodPoint = vec(0, 0, 0);
        float stoodYaw = 0;
        // An attached object whose actor died before the checkpoint: kept, with no actor, so that
        // a revert does not make the object an actor again (unit_get_health stays -1).
        bool gone = false;
        // An attached actor's health and shield (fractions) when the checkpoint was taken: a revert
        // or a Continue brings it back with them (a50's game_lost_script reads Keyes's health).
        float health = 1, shield = 1;
    };

    // An actor ai_attach made of a scripted object, alive when the player walked on into the next
    // section of the mission (a50's Keyes and the freed marines, from the cell block to the hangar):
    // spawned again behind the player on the next section's first live ticks, with its health,
    // as a member of its squad standing for its object (Scenario::spawnCarried, gap 4).
    struct CarriedAttach
    {
        int object = -1, encounter = -1, squad = -1;
        SquadStart start{};
        float health = 1, shield = 1;
        bool suspended = false;
        int scriptFlags = 0, retry = 0;
    };

    struct Playback
    {
        string name;
        sbuffer buffer;
        source channel;
        float gain = 1;                     // as played, before its sound class's gain (halo_fx.cpp)
        int soundClass = -1;
        float applied = -1;                 // the class gain last set on the channel
    };

    struct Voice { int id, location, until, scriptUntil; };
    struct Music { int id; const Track *track; int location, until; float gain; bool repeat, ending, alternate; float loopGain; };
    struct Talking { int id, line, wake; };

    // Entry, Exit and Timeout scenes play at the route's boundaries, the Cartographer's at its
    // terminal, and Call scenes when a mission script calls them (the calling thread waits for
    // the scene to end, as in Halo).
    enum class SceneTrigger { Entry, Exit, Timeout, Cartographer, Call };
    // Several Exit rows for one section are variants of one exit (a30's three extractions):
    // the scene whose `vehicle` the player boarded wins, then the one whose `volume` holds the
    // player, then the one whose `site` flag is nearest the player.
    struct SceneCue
    {
        int mission, section, bsp;
        const char *script;
        SceneTrigger trigger;
        const char *vehicle = nullptr, *volume = nullptr, *site = nullptr;
    };

    // These join authored cinematic scripts to the playable route's entry,
    // interaction and exit boundaries, and to the script calls that play scenes in the middle of
    // a section. Sections are zero-based; -1 is any section of the mission. A Call scene plays only
    // when the script calls it, as in Halo (it used to be the section's Exit or Entry scene).
    // tests/test_mission_exits.py checks that every retail scene script is reached from this
    // table or is listed there as not a scene.
    const SceneCue cues[] = {
        {0, 0, 0, "x10", SceneTrigger::Entry}, {0, 1, 1, "x20", SceneTrigger::Exit}, {0, 6, 6, "x30", SceneTrigger::Exit},
        {1, 0, 0, "cutscene_intro", SceneTrigger::Entry}, {1, 1, 0, "cutscene_bridge", SceneTrigger::Exit},
        // The Pelican comes for the player at the last of the three marine sites they clear.
        {1, 2, 1, "mission_extraction_river_skip", SceneTrigger::Exit, "foehammer_river", "river_all", "river_hide_flag"},
        {1, 2, 1, "mission_extraction_cliff_skip", SceneTrigger::Exit, "foehammer_cliff", "cliff_all", "cliff_hide_flag"},
        {1, 2, 1, "mission_extraction_rubble_skip", SceneTrigger::Exit, "foehammer_rubble", "rubble_all", "rubble_hide_flag"},
        {2, 0, 0, "cutscene_insertion", SceneTrigger::Entry}, {2, 0, 0, "cutscene_energy_lift", SceneTrigger::Exit},
        // mission_prison calls the rescue once cell block D's switch is thrown; ini_post_rescue
        // then puts the player by the switch (prison_player0_teleport).
        {2, 3, 3, "cutscene_rescue", SceneTrigger::Call},
        {2, 4, 2, "cutscene_extraction", SceneTrigger::Exit},
        {3, 0, 0, "cutscene_insertion", SceneTrigger::Entry}, {3, 1, 1, "cutscene_map", SceneTrigger::Exit},
        {3, 1, 1, "cutscene_map", SceneTrigger::Cartographer},
        {3, -1, 1, "cutscene_ledge", SceneTrigger::Call}, {3, -1, 0, "cutscene_shaftb_goal", SceneTrigger::Call},
        {3, 2, 0, "cutscene_extraction_exit", SceneTrigger::Exit},
        {4, 0, 3, "cutscene_insertion_a", SceneTrigger::Entry}, {4, 6, 2, "cutscene_extraction", SceneTrigger::Exit},
        {5, 0, 0, "insertion", SceneTrigger::Entry}, {5, 4, 3, "x50", SceneTrigger::Exit}, {5, 6, 5, "cutscene_extraction", SceneTrigger::Exit},
        {6, 0, 0, "cinematic_intro", SceneTrigger::Entry}, {6, 3, 3, "cutscene_extraction", SceneTrigger::Exit},
        {7, 0, 2, "cutscene_insertion", SceneTrigger::Entry}, {7, 4, 5, "cutscene_extraction", SceneTrigger::Exit},
        {8, 0, 0, "cinematic_intro", SceneTrigger::Entry}, {8, -1, 1, "cinematic_drop", SceneTrigger::Call},
        {8, 2, 2, "cinematic_lift", SceneTrigger::Entry},
        // enc6_3 calls the captain's scene in the control room; the walk back starts at player0_capt_done.
        {8, 4, 4, "cinematic_captain", SceneTrigger::Call},
        {8, 5, 3, "cinematic_outro", SceneTrigger::Exit},
        // enc2_5 calls the x70 bridge scene on the Autumn's bridge (BSP 1); play resumes at player0_playon_base.
        {9, 0, 0, "cinematic_intro", SceneTrigger::Entry}, {9, 1, 1, "cinematic_bridge", SceneTrigger::Call},
        {9, 7, 7, "cinematic_finale", SceneTrigger::Exit},
        {9, -1, 8, "cinematic_time_up", SceneTrigger::Timeout}
    };

    class Scenario
    {
    public:
        Program program;
        std::vector<Thread> threads;
        std::vector<Value> globals;
        std::vector<Value> savedGlobals;
        std::vector<Thread> savedThreads;
        std::vector<Actor> savedActors;
        std::vector<SquadBot> squadBots, savedSquadBots;
        // ai_attach (halo_script_ai.inc attachActors): per scripted object, the start its actor
        // spawns from (the squad's default variant at the object's pose), the tick it was attached
        // and the next tick a failed spawn is tried again. savedAttachedAt is attachedAt at the
        // checkpoint: a revert puts it back (an attach after the checkpoint is undone with the
        // script; one before it that is still waiting for its actor keeps waiting), and the state
        // block's `s attaching` record carries the waiting ones through a Continue.
        std::vector<SquadStart> objectStarts;
        std::vector<int> attachedAt, attachRetry, savedAttachedAt;
        // Attached actors carried in from the section before that have no actor here yet
        // (spawnCarried); savedCarried is the list at the checkpoint (the carried-in section's
        // checkpoint is usually taken before they spawn), restored by a revert.
        std::vector<CarriedAttach> carriedAttached, savedCarried;
        // The living attached actors as the last live tick saw them (snapshotAttached): what a
        // walk into the next section carries. Kept as plain data, since the actors themselves are
        // gone (kickallbots) by the time the next section's script starts.
        std::vector<CarriedAttach> carryCandidates;
        // Unit commands a script gave an attached object before its actor existed
        // (unit_set_current_vitality: c10's marine_suicidal right after its ai_attach): the health
        // and shield fractions, applied when the actor spawns (attachActors). Kept with the checkpoint.
        struct QueuedVitality { float health, shield; };
        std::unordered_map<int, QueuedVitality> attachVitality, savedAttachVitality;
        // A Call scene's gameplay music (gap 12): the looping sounds playing when it started,
        // restarted when it ends unless the scene stopped them.
        struct CallMusic { int id; float gain; bool alternate; };
        std::vector<CallMusic> callMusic;
        // The player aboard a script vehicle at the checkpoint (a30's Pelicans): put back aboard by
        // a revert or a Continue. vehicle noObject when not.
        struct Boarded { int vehicle = -1, seat = -1; vec from; bool scripted = false; };
        Boarded savedBoarded;
        // Encounter combat positions and command lists in this section's coordinates.
        std::vector<std::vector<HaloFiringPosition>> encounterPositions;
        std::vector<HaloCommandProgram> commandPrograms;
        std::vector<int> actorLists;
        std::set<int> maneuvered, flipped;
        std::vector<std::pair<int, int>> links;       // ai_link_activation
        std::set<int> migrationTargets;               // ai_automatic_migration_target
        std::vector<DeviceGroup> groups, savedGroups;
        std::vector<DeviceState> devices, savedDevices;
        std::vector<int> objectDevices;
        int savedTick = 0;
        std::vector<Actor> actors;
        std::vector<Voice> voices;
        std::unordered_map<int, std::unique_ptr<Playback>> channels;
        int nextChannel = 0;
        std::vector<Music> music;
        std::vector<Talking> talking;
        std::vector<int> conversationStatus;
        std::unordered_map<int, Value> external;
        std::set<std::string> unsupported;
        std::mt19937 random{0x48414c4f};
        Thread director;
        std::unordered_map<int, Thread> sceneThreads;
        // Gameplay scripts the running scene's threads woke (not scenes of the cue table): in Halo
        // they are ordinary threads that go on after the cutscene, so endCue moves the ones still
        // running onto the gameplay threads (b30's music_b30 waiting for its cues, a50's and d40's
        // insertion dialogue). Gap 3.
        std::vector<int> sceneWoken;
        // The running scene is one a script calls inside (if (cinematic_skip_start) ...) (x10, d40's
        // bridge scene): Halo's skip reverts to before it, so a skip leaves nothing it woke or seated.
        bool cueSkippable = false;
        // A seat a scene's thread gave the player (unit_enter_vehicle (player0) on the scene stage,
        // where the player is off stage) and no later unit_exit_vehicle took back: b40's pelican_up,
        // woken by cutscene_insertion_a. When the scene ends the player takes it in play (endCue).
        int sceneSeat = noObject;
        std::string sceneSeatName;
        CameraMotion camera;
        Fade fade;
        const SceneCue *cue = nullptr;
        const Projection *projection = nullptr;
        int mission = -1, section = 0, tick = 0, accumulator = 0, world = -1;
        int chapter = -1, chapterStart = 0;
        float nearClip = .0625f;
        bool loaded = false, letterbox = false, cameraControl = false, paused = false;
        bool pendingCheckpoint = false;
        bool exitPlayed = false, completeExit = false, failedEscape = false, runningGameplay = false;
        bool gameWon = false, gameLost = false;
        std::string worldName;
        std::string subtitle;
        std::string objective, help;
        bool showHelp = false;
        int subtitleEnd = 0;
        int savedChapter = -1, savedChapterStart = 0;
        std::string savedObjective, savedHelp;
        bool savedShowHelp = false;
        // Nav points and the HUD parts the script hides or blinks (halo_hud.cpp draws them).
        std::vector<NavPoint> navPoints, savedNavPoints;
        int hudHidden = 0, hudBlinking = 0, savedHudHidden = 0, savedHudBlinking = 0;
        std::unordered_map<int, Value> savedExternal;
        const char *cache = "";
        unsigned programHash = 0;     // FNV-1a of the loaded .hscb file (saved games, halo.cpp)
        HudTimer hudTimer;
        std::unordered_map<std::string, TutorialPoll> tutorialPolls;
        // Per program.switches entry, this section: 1 armed, 0 waiting for the player to leave
        // the volume they started in, 2 the entry doorway (never fires). Empty until the first
        // live tick of the section.
        std::vector<signed char> switchArm;
        // A BSP switch half way along a lift shaft (c20's platforms, c10's lifts) ends the
        // section there, and the next one puts the player at its start on the landing: the rest
        // of the ride never happens. The volumes on that stretch of the shaft (c20's
        // section7_trigger, above bsp 2,3, which section6 waits for) count as entered by the
        // player for the first second of the next section (noteRide).
        vec switchPoint;
        bool rideCarried = false, ridePending = false;
        std::vector<int> ridden;
        int riddenUntil = -1;

        // A scene a script called in the middle of a section (SceneTrigger::Call) plays on the
        // scene stage like any other, so the scripted objects are kept aside for it and put back
        // after, except those the scene itself created or destroyed (`sceneObjects`, the last
        // visibility it gave each).
        std::vector<Actor> callActors;
        std::unordered_map<int, bool> sceneObjects;
        // Where the running scene leaves the player (its last object_teleport and object_set_facing
        // of player0): read from the scene's script when it starts, so a skipped scene leaves the
        // player there too, and updated as the scene runs.
        int handoffTeleport = -1, handoffFacing = -1;
        // A skipped Call scene runs the rest of its script at once (endCue): its waits pass and
        // game_reverted is true, so cinematic_skip_start passes over the skippable part as Halo's
        // skip (a revert to the scene's start) does.
        bool skipping = false;

        void start(int m, int s);
        void stop();
        bool beginCue(SceneTrigger trigger, const char *script = nullptr);
        void endCue(bool skipped);
        const SceneCue *callCue(const std::string &script) const;
        bool cueScript(const std::string &script) const;
        bool skippableScene(const char *scene) const;
        bool callsScript(uint32_t datum, int target, int depth) const;
        void findHandoff(uint32_t datum, int depth);
        bool placePlayer(int teleport, int facing, const char *why);
        bool unitInVolume(int volume, const playerent *unit) const;
        void step(Thread &thread, bool scene);
        void update(int elapsed);
        void startGameplay();
        void startCartographer();
        void startPelicanApproach();
        bool pelicanReady() const;
        bool cartographerInterior() const;
        bool cartographerMapped() const;
        void placeSquads(int reference, int limit, bool scaled = true);
        size_t livingBots() const;
        void eraseSquads(int reference, bool erase);
        int living(int reference) const;
        void restoreSquads();
        bool matchesSquad(int reference, int encounter, int squad) const;
        void prepareAI();
        void updateLinks();
        void migrateBot(SquadBot &bot, int newGroup);
        vec startPoint(const SquadStart &start) const;
        void setUnitFlag(playerent *unit, int flag, bool on);
        SquadBot *squadBotOf(const playerent *unit);
        Actor *scripted(int id);
        template<class F> void eachUnit(int value, F f);
        bool aiCommand(const char *name, const std::vector<Value> &args, bool scene, Value &out);
        bool vehicleCommand(const char *name, const std::vector<Value> &args, bool scene, Value &out);
        // Dropships and Pelicans, the scenario's vehicles the engine does not drive
        // (halo_script_ai.inc): the actors riding at their seats, the actors walking to them
        // (ai_go_to_vehicle), and the player boarded in one.
        struct Cargo { int vehicle, seat; playerent *unit; bool walking; int until; };
        std::vector<Cargo> cargo;
        int boardedVehicle = noObject, boardedSeat = -1;
        vec boardedAt, boardedFrom;
        // Seated by the script (seatPlayer) rather than boarded with E: held until
        // unit_exit_vehicle; boardedStart is the seat where it began, boardedSince when.
        bool boardedScripted = false;
        int boardedSince = 0;
        vec boardedStart;
        // The seat's camera marker (the Pelican's "camera p-riderlf", the cryotube's "camera"):
        // the eye of a scripted seat, or -1 for the seat marker and the usual seated eye height.
        int boardedCamera = -1;
        // Where updateCargo last put the player in a scripted seat (a move by anything else lets
        // them out).
        vec boardedPut;
        bool boardedPutValid = false;
        // ai_vehicle_encounter: the squad a vehicle's driver belongs to, applied when one is loaded.
        std::unordered_map<int, int> vehicleSquads;
        bool scriptVehicle(int id) const;
        bool seatWorld(int vehicle, int seat, vec &point) const;
        bool markerWorld(int vehicle, int marker, vec &point) const;
        int cameraMarker(int vehicle, int seat) const;
        int freeSeat(int vehicle, const char *seat) const;
        bool loadCargo(int vehicle, const char *seat, playerent *unit, bool walking);
        int unloadCargo(int vehicle, const char *seat);
        void releaseCargo(Cargo &entry);
        void updateCargo();
        int boardCandidate(int &seat) const;
        bool board();
        void unboard(bool keepPlace);
        bool seatPlayer(int vehicle, const char *seat);
        void unseatScripted(const char *why, bool toFloor = false);
        void clearCargo();
        void configureActor(playerent *bot, int encounter, int squad, const SquadStart &start);
        int attachedEntry(int object) const;
        void attachActors();
        void spawnCarried();
        void snapshotAttached();
        void detachActor(int object);
        void fastForwardFresh();
        void finishTalking();
        void updatePlatoons();
        int actorListFor(int reference);
        template<class F> void eachActor(int reference, F f);
        playerent *actorFromValue(int value) const;
        bool objectPosition(int value, vec &position) const;
        bool objectMarkerPosition(int value, const char *marker, vec &position) const;
        Value literal(const Node &node) const;
        Value command(Thread &thread, const Node &node, const char *name, const std::vector<Value> &args, bool scene);
        bool pacingCommand(const char *name, const std::vector<Value> &args, bool scene, Value &out);
        void wake(int script, bool scene = false);
        void sleep(Thread &thread, const std::vector<Value> &args);
        void complete(Thread &thread, Value result);
        void push(Thread &thread, uint32_t node);
        CameraView currentCamera() const;
        void updateCamera();
        Pose currentActor(int actor, int depth = 0) const;
        bool testVolume(int index, int object = playerObject) const;
        bool playerPoint(vec &point) const;
        void noteRide();
        bool switchBsp(int bsp, const char *trigger);
        void armSwitches();
        void updateDevices(bool gameplay);
        bool carryBodies(const Device &d, const DeviceBox &old, const vec &delta);
        float liftBoard(physent *entity, float lo) const;
        void updateRecordings();
        bool setGroup(int index, float value, bool immediate = false);
        float groupValue(int index, float fallback = 0) const;
        bool deviceActive(size_t index) const;
        void renderDevices() const;
        bool collideDevices(physent *entity, float &hi, float &lo, int *blocking = nullptr) const;
        bool traceDevices(const vec &from, const vec &to, HaloCollisionHit &hit) const;
        // Damage targets (Object::vitality): the first one a segment passes through, the ones a blast
        // reaches, and the damage the player deals them.
        int traceTargets(const vec &from, const vec &to, float &fraction) const;
        int targetsNear(const vec &centre, float radius, int *targets, float *distances, int capacity) const;
        bool damageTarget(int target, float amount);
        vec targetCentre(int target) const;
        bool useDevice();
        void setWorld(int bsp);
        void setCamera(int index, int duration, int object);
        const Sound *sound(int id) const;
        int play(int id, float gain, bool loop, const vec *position = nullptr);
        void stopLocation(int token);
        void stopSounds();
        void updateSound();
        void startConversation(int id);
        void stopConversation(int id);
        void startMusic(int id, float gain);
        void stopMusic(int id);
        void setAlternate(int id, bool enabled);
        void setActor(int id, bool visible, bool reset, bool bulk = false);
        void draw();
        void render();
    };

    Scenario scenario;

    const char *scriptName(const Program &program, int i)
    {
        const Script *script = at(program.scripts, i);
        return script ? script->name.c_str() : "";
    }

    void Scenario::complete(Thread &thread, Value result)
    {
        thread.stack.pop_back();
        if(thread.stack.empty())
        {
            thread.result = result;
            thread.done = true;
        }
        else thread.stack.back().result = result;
    }

    void Scenario::push(Thread &thread, uint32_t datum)
    {
        if(program.node(datum) && thread.stack.size() < 96) thread.stack.emplace_back(datum);
        else if(!thread.stack.empty()) thread.stack.back().result = {};
    }

    Value Scenario::literal(const Node &node) const
    {
        if(node.flags & 4)
        {
            unsigned id = node.data & 0xffff;
            if(id < globals.size()) return globals[id];
            auto found = external.find(id);
            return found != external.end() ? found->second : Value{};
        }

        const char *text = program.text(node.text);
        double value = 0;
        switch(node.opcode)
        {
            case 5: value = (node.data & 0xff) != 0; break;
            case 6:
            {
                float real;
                memcpy(&real, &node.data, sizeof(real));
                value = std::isfinite(real) ? real : 0;
                break;
            }
            case 8: case 17: value = int32_t(node.data); break;
            case 9: break;
            default: value = int16_t(node.data); break;
        }
        // String literals use the compiled string offset. Tag/object literals
        // retain their string for command names and their resolved datum index.
        return Value{value, text, node.type};
    }

    void Scenario::wake(int index, bool scene)
    {
        if(index < 0 || size_t(index) >= threads.size()) return;
        const Script &script = program.scripts[index];
        if(script.kind > 2) return;
        Thread &thread = scene ? sceneThreads[index] : threads[index];
        // A gameplay script a scene wakes (gap 3): endCue carries it on in play if it is still running.
        if(scene && cue && !cueScript(script.name) && std::find(sceneWoken.begin(), sceneWoken.end(), index) == sceneWoken.end())
            sceneWoken.push_back(index);
        if(thread.done && script.kind != 2) return;
        thread.disabled = false;
        thread.wake = tick;
        if(!thread.started)
        {
            thread.started = true;
            thread.stack.emplace_back(script.root);
        }
    }

    void Scenario::sleep(Thread &thread, const std::vector<Value> &args)
    {
        int duration = args.empty() ? 1 : args[0].integer();
        Thread *target = &thread;
        if(args.size() > 1 && args[1].integer() >= 0)
        {
            int id = args[1].integer();
            if(size_t(id) >= threads.size()) return;
            target = &threads[id];
        }
        if(duration < 0) target->disabled = true;
        else if(duration > 0) target->wake = tick + clamp(duration, 1, 180000);
    }

    void Scenario::step(Thread &thread, bool scene)
    {
        // A gameplay thread that started a scene waits for it to end, then carries on after the call.
        for(int budget = 0; budget < 2048 && !thread.stack.empty() && !thread.disabled && thread.wake <= tick && (scene || !cue); ++budget)
        {
            Frame &frame = thread.stack.back();
            const Node &node = *program.node(frame.expression);
            if(node.flags & 1) { complete(thread, literal(node)); continue; }
            const Node *head = program.node(node.data);
            if(!head) { complete(thread, {}); continue; }
            const char *name = program.text(head->text);
            if(node.flags & 2)
            {
                if(!frame.phase++)
                {
                    const Script *script = at(program.scripts, node.opcode);
                    // Scenes in the cue table play on the scene stage: a Call scene now, the
                    // others at the route's boundary (the call itself is passed over).
                    const bool deferred = script && !scene && cueScript(script->name);
                    if(deferred && callCue(script->name) && player1->state == CS_ALIVE && !intermission)
                        beginCue(SceneTrigger::Call, script->name.c_str());
                    else if(deferred && cartographerInterior() && script->name == "cutscene_map")
                        beginCue(SceneTrigger::Cartographer);
                    else if(script && script->kind != 4 && !deferred) push(thread, script->root);
                    continue;
                }
                complete(thread, frame.result);
                continue;
            }

            if(!frame.phase) frame.next = head->next;
            const Node *arg = program.node(frame.next);
            switch(node.opcode)
            {
                case 0: // begin
                case 1: // begin_random
                    if(node.opcode == 1 && !frame.phase)
                    {
                        for(uint32_t it = frame.next; const Node *part = program.node(it); it = part->next)
                        {
                            if(frame.shuffled.size() >= 256) break;
                            frame.shuffled.push_back(it);
                        }
                        std::shuffle(frame.shuffled.begin(), frame.shuffled.end(), random);
                    }
                    ++frame.phase;
                    if(node.opcode == 1)
                    {
                        if(frame.shuffled.empty()) { complete(thread, frame.result); continue; }
                        uint32_t part = frame.shuffled.back();
                        frame.shuffled.pop_back();
                        push(thread, part);
                    }
                    else if(arg)
                    {
                        uint32_t part = frame.next;
                        frame.next = arg->next;
                        push(thread, part);
                    }
                    else complete(thread, frame.result);
                    continue;
                case 2: // if
                    if(frame.phase == 0 && arg)
                    {
                        uint32_t condition = frame.next;
                        frame.phase = 1;
                        frame.next = arg->next;
                        push(thread, condition);
                    }
                    else if(frame.phase == 1 && arg)
                    {
                        uint32_t branch = frame.result.truth() ? frame.next : arg->next;
                        frame.phase = 2;
                        frame.result = {};
                        push(thread, branch);
                    }
                    else complete(thread, frame.result);
                    continue;
                case 4: // set
                    if(frame.phase == 0 && arg)
                    {
                        frame.phase = 1;
                        push(thread, arg->next);
                    }
                    else
                    {
                        if(arg && arg->flags & 4)
                        {
                            int id = arg->data & 0xffff;
                            if(size_t(id) < globals.size()) globals[id] = frame.result;
                            else external[id] = frame.result;
                        }
                        complete(thread, frame.result);
                    }
                    continue;
                case 5: // and
                case 6: // or
                    if(frame.phase && (frame.result.truth() == (node.opcode == 6)))
                    {
                        complete(thread, number(node.opcode == 6, 5));
                        continue;
                    }
                    if(!arg) { complete(thread, number(node.opcode == 5, 5)); continue; }
                    {
                        uint32_t part = frame.next;
                        frame.next = arg->next;
                        ++frame.phase;
                        push(thread, part);
                    }
                    continue;
                case 20: // sleep_until: evaluate timing arguments once and poll the condition.
                {
                    const Node *condition = program.node(head->next);
                    const Node *interval = condition ? program.node(condition->next) : nullptr;
                    if(frame.phase == 0)
                    {
                        frame.phase = 1;
                        frame.deadline = -1;
                        if(interval) push(thread, condition->next);
                    }
                    else if(frame.phase == 1)
                    {
                        if(interval) frame.interval = clamp(frame.result.integer(), 1, 180000);
                        frame.phase = 2;
                        if(interval && interval->next != nil) push(thread, interval->next);
                    }
                    else if(frame.phase == 2)
                    {
                        if(interval && interval->next != nil && frame.result.integer() >= 0)
                            frame.deadline = tick + min(frame.result.integer(), 180000);
                        frame.phase = 3;
                    }
                    else if(frame.phase == 3)
                    {
                        if(!condition || skipping || (frame.deadline >= 0 && tick >= frame.deadline))
                            complete(thread, {});
                        else
                        {
                            frame.phase = 4;
                            push(thread, head->next);
                        }
                    }
                    else if(frame.result.truth()) complete(thread, {});
                    else
                    {
                        frame.phase = 3;
                        thread.wake = tick + frame.interval;
                        if(frame.deadline >= 0) thread.wake = min(thread.wake, frame.deadline);
                    }
                    continue;
                }
            }

            if(frame.phase) frame.arguments.push_back(frame.result);
            if(arg && frame.arguments.size() < 32)
            {
                uint32_t part = frame.next;
                frame.next = arg->next;
                ++frame.phase;
                push(thread, part);
            }
            else
            {
                if(node.opcode == 19) sleep(thread, frame.arguments);
                Value value = node.opcode == 19 ? Value{} : command(thread, node, name, frame.arguments, scene);
                complete(thread, value);
            }
        }
    }

    vec poseOffset(vec offset, const Pose &pose)
    {
        offset.rotate_around_z(pose.yaw);
        offset.rotate_around_y(pose.pitch);
        offset.rotate_around_x(pose.roll);
        return offset;
    }

    CameraView cameraView(const Pose &pose)
    {
        return {pose.position, poseOffset(vec(1, 0, 0), pose), poseOffset(vec(0, 0, 1), pose), pose.fov};
    }

    vec renderAngles(const vec &forward, const vec &up)
    {
        float yaw = atan2f(forward.y, forward.x);
        float pitch = atan2f(forward.z, forward.magnitudexy());
        vec right(sinf(yaw), -cosf(yaw), 0);
        vec levelUp(-cosf(yaw)*sinf(pitch), -sinf(yaw)*sinf(pitch), cosf(pitch));
        return vec(yaw, pitch, atan2f(up.dot(right), up.dot(levelUp))).div(RAD);
    }

    vec renderAngles(const Pose &pose)
    {
        return renderAngles(poseOffset(vec(1, 0, 0), pose), poseOffset(vec(0, 0, 1), pose));
    }

    vec cameraRotation(const CameraView &from, const CameraView &to)
    {
        const vec left = vec().cross(from.up, from.forward), targetLeft = vec().cross(to.up, to.forward);
        const float cosine = clamp((from.forward.dot(to.forward) + left.dot(targetLeft) + from.up.dot(to.up) - 1) * .5f, -1.0f, 1.0f);
        vec axis = vec().cross(from.forward, to.forward).add(vec().cross(left, targetLeft)).add(vec().cross(from.up, to.up));
        const float sine = axis.magnitude() * .5f;
        if(sine > 1e-4f) return axis.mul(atan2f(sine, cosine) / (2 * sine));
        if(cosine > 0) return axis.mul(.5f);

        // At a half turn, recover the axis from a column of R + I.
        const vec diagonal = vec(1, 1, 1).add(vec(from.forward.x*to.forward.x + left.x*targetLeft.x + from.up.x*to.up.x,
                                                    from.forward.y*to.forward.y + left.y*targetLeft.y + from.up.y*to.up.y,
                                                    from.forward.z*to.forward.z + left.z*targetLeft.z + from.up.z*to.up.z));
        const int i = diagonal.x >= diagonal.y && diagonal.x >= diagonal.z ? 0 : (diagonal.y >= diagonal.z ? 1 : 2);
        vec halfTurn = vec(to.forward).mul(from.forward[i]).add(vec(targetLeft).mul(left[i])).add(vec(to.up).mul(from.up[i]));
        halfTurn[i] += 1;
        return halfTurn.normalize().mul(copysignf(atan2f(sine, cosine), halfTurn.dot(axis)));
    }

    void Scenario::updateRecordings()
    {
        for(size_t i = 0; i < actors.size(); ++i)
        {
            Actor &actor = actors[i];
            if(!actor.recording) continue;
            const Object &object = program.objects[i];
            const Recording &record = *actor.recording;
            int elapsed = tick - actor.recordingStart;
            if(elapsed > record.ticks)
            {
                if(halo_scripttrace && cue)
                    conoutf("Halo recording: %s finished on %s at %.2f %.2f %.2f",
                            record.name.c_str(), object.name.c_str(), actor.pose.position.x,
                            actor.pose.position.y, actor.pose.position.z);
                actor.visible &= !actor.recordingDeletes;
                actor.recording = nullptr;
                actor.gait = -1;
                if(actor.recordingHover) { actor.speed = 0; actor.velocity = vec(); }
                continue;
            }
            const auto &inputs = record.inputs;
            while(actor.input + 1 < inputs.size() && inputs[actor.input + 1].tick < elapsed) ++actor.input;
            const RecordedInput &input = inputs[actor.input];
            if(object.kind < -1 || actor.parent >= 0) continue;
            const bool biped = object.kind == -1;
            const bool airborne = object.kind == 3 || object.kind == 5;
            const bool flying = airborne || object.kind == 4;
            if(!(biped && (input.flags & (1 << 8))))
            {
                float heading = input.yaw;
                float turn = object.turn;
                if(object.kind == 1) turn = max(turn, .035f);
                if(!biped && turn > 0)
                    heading = actor.pose.yaw + clamp(remainderf(heading - actor.pose.yaw, 2.0f * float(PI)), -turn, turn);
                actor.pose.yaw = heading;
            }
            if(airborne) actor.pose.pitch = input.pitch;
            float forward = input.forward * (input.forward >= 0 ? object.forward : object.reverse);
            float left = input.left * object.slide;
            if(biped)
            {
                int seat = actor.baseSeat >= 0 ? actor.baseSeat : input.state;
                if(seat == 1 || seat == 2 || (input.flags & (1 << 9)))
                {
                    forward = max(0.0f, input.forward) * (.512f / ticksPerSecond);
                    left = 0;
                }
                if(input.flags & 1) { forward *= .5f; left *= .5f; }
            }
            float rate = fabsf(forward) < fabsf(actor.speed) ? object.deceleration : object.acceleration;
            actor.speed = rate > 0 ? clamp(forward, actor.speed - rate, actor.speed + rate) : forward;
            vec velocity(actor.speed, left, 0);
            if(airborne) velocity.rotate_around_y(actor.pose.pitch);
            velocity.rotate_around_z(actor.pose.yaw);
            actor.velocity = velocity;
            actor.pose.position.add(velocity);
            if(cue && !flying && object.kind != 6)
            {
                vec point(actor.pose.position.x * 4, -actor.pose.position.y * 4, actor.pose.position.z * 4);
                float ground = 0;
                float height = biped ? .08f : .35f;
                if(halocinematicground(point, ground) && (actor.grounded || fabsf(point.z - ground - height) < 1.6f))
                {
                    actor.pose.position.z = (ground + height) / 4;
                    actor.grounded = true;
                }
            }
            if(biped)
            {
                int gait = fabsf(forward) + fabsf(left) < .002f ? -1 :
                           fabsf(left) > fabsf(forward) ? 2 : forward >= 0 ? 0 : 1;
                if(gait != actor.gait) { actor.gait = gait; actor.gaitStart = tick; }
            }
        }
    }

    Pose Scenario::currentActor(int id, int depth) const
    {
        const Actor *actor = at(actors, id);
        if(!actor || depth >= 8) return {};
        if(actor->parent < 0 || !at(program.objects, actor->parent)) return actor->pose;
        Pose pose = currentActor(actor->parent, depth + 1);
        const Object &parent = program.objects[actor->parent], &child = program.objects[id];
        int parentMarker = named(parent.markers, actor->parentMarker.c_str());
        if(const Flag *marker = at(parent.markers, parentMarker))
        {
            pose.position.add(poseOffset(vec(marker->pose.position).mul(actors[actor->parent].scale), pose));
            pose.yaw += marker->pose.yaw;
            pose.pitch += marker->pose.pitch;
        }
        int childMarker = named(child.markers, actor->childMarker.c_str());
        if(const Flag *marker = at(child.markers, childMarker))
        {
            pose.yaw -= marker->pose.yaw;
            pose.pitch -= marker->pose.pitch;
            pose.position.sub(poseOffset(vec(marker->pose.position).mul(actor->scale), pose));
        }
        return pose;
    }

    CameraView Scenario::currentCamera() const
    {
        if(camera.firstPerson >= 0)
        {
            Pose result = currentActor(camera.firstPerson);
            result.position.z += .19f;
            result.fov = 70;
            return cameraView(result);
        }
        CameraView result = camera.view;
        if(camera.relative >= 0)
        {
            Pose target = currentActor(camera.relative);
            result.position.add(target.position);
            if(const Object *object = at(program.objects, camera.relative))
                result.position.add(poseOffset(vec(object->center).mul(actors[camera.relative].scale), target));
        }
        return result;
    }

    void Scenario::updateCamera()
    {
        if(!camera.set || camera.firstPerson >= 0) return;
        const float remaining = camera.end - tick + 1;
        if(remaining <= 1)
        {
            camera.view = camera.target;
            camera.positionVelocity = camera.positionAcceleration = vec();
            camera.angularVelocity = camera.angularAcceleration = vec();
            camera.fovVelocity = camera.fovAcceleration = 0;
            return;
        }

        // The observer solves a new quintic toward the remaining target each
        // tick, including the current velocity and acceleration in the fit.
        const float t = 1 / remaining, u = 1 - t, u2 = u*u, u3 = u2*u;
        const float d = t*t*t*(10 + t*(-15 + 6*t)), v = u3*(1 + 3*t), a = .5f*u3;
        const float dv = 30*t*t*t*u2, vv = u2*(1 + 2*t - 15*t*t), av = .5f*u2*(2 - 5*t);
        const float da = 60*t*t*t*u*(1 - 2*t), va = 12*t*t*u*(5*t - 3), aa = u*(1 - 8*t + 10*t*t);
        auto advance = [&](float delta, float &speed, float &acceleration)
        {
            const float step = delta*d + speed*v + acceleration*a;
            const float nextSpeed = delta*dv + speed*vv + acceleration*av;
            acceleration = delta*da + speed*va + acceleration*aa;
            speed = nextSpeed;
            return step;
        };
        const vec rotation = cameraRotation(camera.view, camera.target);
        vec step;
        loopi(3)
        {
            camera.view.position[i] += advance(camera.target.position[i] - camera.view.position[i],
                                               camera.positionVelocity[i], camera.positionAcceleration[i]);
            step[i] = advance(rotation[i], camera.angularVelocity[i], camera.angularAcceleration[i]);
        }
        const float angle = step.magnitude();
        if(angle > 0)
        {
            step.div(angle);
            camera.view.forward.rotate(angle, step).normalize();
            camera.view.up.rotate(angle, step).normalize();
        }
        camera.view.fov += advance(camera.target.fov - camera.view.fov, camera.fovVelocity, camera.fovAcceleration);
    }

    void Scenario::setCamera(int index, int duration, int object)
    {
        const Camera *point = at(program.cameras, index);
        if(!point) return;
        CameraView target = cameraView(point->pose);
        CameraView previous = camera.set ? currentCamera() : target;
        if(at(program.objects, object))
        {
            Pose relative = currentActor(object);
            previous.position.sub(relative.position);
            previous.position.sub(poseOffset(vec(program.objects[object].center).mul(actors[object].scale), relative));
        }
        camera.view = previous;
        camera.target = target;
        camera.end = tick + clamp(duration, 0, 180000) / ticksPerSecond * ticksPerSecond;
        camera.relative = at(program.objects, object) ? object : noObject;
        camera.firstPerson = noObject;
        camera.set = true;
        if(camera.end == tick) updateCamera();
        if(halo_scripttrace) conoutf("Halo scene: camera %s at tick %d", point->name.c_str(), tick);
    }

    void Scenario::setWorld(int bsp)
    {
        if(std::find(program.worlds.begin(), program.worlds.end(), bsp) == program.worlds.end()) return;
        if(world != bsp) camera = {};
        world = bsp;
        defformatstring(name)("halo_scene_%s_%02d", cache, bsp);
        worldName = name;
        conoutf("Halo scene: %s BSP %d", cache, bsp);
    }

    // The player's body centre in retail coordinates, as the volume tests see it.
    bool Scenario::playerPoint(vec &point) const
    {
        if(!projection || player1->state != CS_ALIVE) return false;
        const Projection &p = *projection;
        point = vec(p.origin.x + (player1->o.x - p.margin) / p.xy,
                    p.origin.y - (player1->o.y - p.margin) / p.xy,
                    p.origin.z + (player1->o.z - .45f * player1->eyeheight) / p.z);
        return true;
    }

    static bool insideVolume(const Volume &volume, vec point)
    {
        if(volume.kind == 0)
        {
            return point.x > volume.origin.x && point.x < volume.extent.x &&
                   point.y > volume.origin.y && point.y < volume.extent.y &&
                   point.z > volume.origin.z && point.z < volume.extent.z;
        }
        vec local = point.sub(volume.origin);
        vec side;
        side.cross(volume.up, volume.forward);
        float x = local.dot(volume.forward), y = local.dot(side), z = local.dot(volume.up);
        return x > 0 && x < volume.extent.x && y > 0 && y < volume.extent.y && z > 0 && z < volume.extent.z;
    }

    // First live tick of a section entered through a switch volume: the player stands at the
    // section's start. When that is more than a unit above or below where the switch fired, the
    // switch was part way along a lift ride; the volumes on the vertical line from the switch
    // point to the start's height are the ones the rest of the ride would have passed.
    void Scenario::noteRide()
    {
        ridden.clear();
        vec start;
        if(!playerPoint(start) || fabs(start.z - switchPoint.z) <= 1) return;
        const float low = min(start.z, switchPoint.z), high = max(start.z, switchPoint.z);
        for(size_t i = 0; i < program.volumes.size(); ++i)
        {
            for(float z = low; z <= high; z += .1f)
            {
                if(!insideVolume(program.volumes[i], vec(switchPoint.x, switchPoint.y, z))) continue;
                ridden.push_back(int(i));
                conoutf("Halo trigger: %s passed on the ride from %.1f to %.1f", program.volumes[i].name.c_str(),
                        switchPoint.z, start.z);
                break;
            }
        }
        riddenUntil = tick + int(ticksPerSecond);
    }

    bool Scenario::testVolume(int index, int object) const
    {
        const Volume *volume = at(program.volumes, index);
        if(!volume || !projection) return false;
        vec point;
        if(object == playerObject)
        {
            if(!playerPoint(point)) return false;
            if(tick <= riddenUntil && std::find(ridden.begin(), ridden.end(), index) != ridden.end()) return true;
        }
        else
        {
            const Actor *actor = at(actors, object);
            if(!actor || !actor->visible) return false;
            Pose pose = currentActor(object);
            point = pose.position;
            point.add(poseOffset(vec(program.objects[object].center).mul(actor->scale), pose));
        }
        return insideVolume(*volume, point);
    }

    // An actor's body in a trigger volume: its middle, as for the player (playerPoint).
    bool Scenario::unitInVolume(int index, const playerent *unit) const
    {
        if(unit == player1) return testVolume(index);
        const Volume *volume = at(program.volumes, index);
        if(!volume || !projection || !unit || unit->state != CS_ALIVE) return false;
        const Projection &p = *projection;
        const vec point(p.origin.x + (unit->o.x - p.margin) / p.xy, p.origin.y - (unit->o.y - p.margin) / p.xy,
                        p.origin.z + (unit->o.z - .45f * unit->eyeheight) / p.z);
        return insideVolume(*volume, point);
    }

    bool Scenario::switchBsp(int bsp, const char *trigger)
    {
        const Projection *next = at(program.projections, section + 1);
        if(!next || !projection || bsp != next->bsp || bsp == projection->bsp) return false;
        if(cartographerInterior() && !cartographerMapped()) return false;
        vec point;
        const bool alive = playerPoint(point);
        if(!haloscenariotransition()) return false;
        conoutf("Halo trigger: %s BSP %d to %d", trigger, projection->bsp, bsp);
        if(alive)
        {
            switchPoint = point;
            rideCarried = true;
        }
        return true;
    }

    // A section can start inside a switch volume to the BSP it goes on to: b40's c1 tower
    // (BSP 11) is entered through its top "bsp 11,2" doorway and left through the bottom one.
    // A volume the player starts in fires only once they have left it, and one with another
    // volume to the same BSP (the way out) never fires in this section.
    void Scenario::armSwitches()
    {
        const size_t count = program.switches.size();
        if(switchArm.size() != count)
        {
            switchArm.assign(count, 1);
            for(size_t i = 0; i < count; ++i)
            {
                const BspSwitch &entry = program.switches[i];
                if(entry.from != world || !testVolume(entry.volume)) continue;
                bool elsewhere = false;
                for(size_t j = 0; j < count && !elsewhere; ++j)
                {
                    const BspSwitch &other = program.switches[j];
                    elsewhere = j != i && other.from == entry.from && other.to == entry.to && !testVolume(other.volume);
                }
                switchArm[i] = elsewhere ? 2 : 0;
                conoutf("Halo trigger: %s %s in this section", program.volumes[entry.volume].name.c_str(),
                        elsewhere ? "is the entry, ignored" : "holds the start, armed once left");
            }
            return;
        }
        for(size_t i = 0; i < count; ++i)
            if(!switchArm[i] && !testVolume(program.switches[i].volume)) switchArm[i] = 1;
    }

    float Scenario::groupValue(int index, float fallback) const
    {
        const DeviceGroup *group = at(groups, index);
        return group ? group->value : fallback;
    }

    bool Scenario::setGroup(int index, float value, bool immediate)
    {
        if(!std::isfinite(value)) return false;
        if(!at(groups, index)) return false;
        DeviceGroup &group = groups[index];
        value = clamp(value, 0.0f, 1.0f);
        if(!immediate && (group.value == value || (group.once && group.changed))) return false;
        group.value = value;
        if(!immediate) group.changed = true;
        else for(size_t i = 0; i < devices.size(); ++i)
        {
            const Device &d = program.devices[i];
            DeviceState &state = devices[i];
            if(d.positionGroup == index) { state.position = value; state.positionVelocity = 0; }
            if(d.powerGroup == index) { state.power = value; state.powerVelocity = 0; }
        }
        return true;
    }

    bool Scenario::deviceActive(size_t index) const
    {
        return loaded && world >= 0 && world < 32 && devices[index].visible &&
               (program.devices[index].bspMask & (1u << world));
    }

    vec toWorld(const Projection &p, const vec &point)
    {
        return vec((point.x - p.origin.x) * p.xy + p.margin,
                   (p.origin.y - point.y) * p.xy + p.margin, (point.z - p.origin.z) * p.z);
    }

    vec deviceLocal(const Projection &p, const Device &d, const vec &point)
    {
        vec local(p.origin.x + (point.x - p.margin) / p.xy,
                  p.origin.y - (point.y - p.margin) / p.xy, p.origin.z + point.z / p.z);
        local.sub(d.pose.position).rotate_around_z(-d.pose.yaw);
        return local;
    }

    float deviceFrame(const Device &d, const DeviceState &s)
    {
        float position = d.flags & 8 ? 1 - s.position : s.position;
        float frame = position * (d.frames - (d.loopFlags & 1 ? 0 : 1));
        return d.loopFlags & 2 ? floorf(frame) : frame;
    }

    DeviceBox panelBox(const Device &d, const DevicePanel &panel, const DeviceState &s)
    {
        float frame = deviceFrame(d, s);
        int count = int(panel.frames.size());
        int a = clamp(int(frame), 0, count - 1);
        int b = a + 1 < count ? a + 1 : (d.loopFlags & 1 ? 0 : a);
        float alpha = frame - int(frame);
        return {vec(panel.frames[a].low).mul(1 - alpha).add(vec(panel.frames[b].low).mul(alpha)),
                vec(panel.frames[a].high).mul(1 - alpha).add(vec(panel.frames[b].high).mul(alpha))};
    }

    // A door leaf lying flat on the floor in the door's current pose (Scenario::collideDevices): its
    // top at most this high above the door's base and the panel at most this thick, in retail units.
    // tools/halo_collision.py floor_leaf uses the same limits for the route planner.
    const float kDoorLeafTop = .25f, kDoorLeafThickness = .3f;

    bool abovePanel(const vec &p, const DeviceBox &box, float radius)
    {
        return p.x > box.low.x - radius && p.x < box.high.x + radius &&
               p.y > box.low.y - radius && p.y < box.high.y + radius;
    }

    bool tracePanel(const DeviceBox &box, const vec &from, const vec &delta, float &fraction, vec &normal)
    {
        float entry = 0, exit = fraction;
        vec entryNormal;
        loopi(3)
        {
            if(fabsf(delta[i]) < 1e-7f)
            {
                if(from[i] <= box.low[i] || from[i] >= box.high[i]) return false;
                continue;
            }
            float near = (box.low[i] - from[i]) / delta[i];
            float far = (box.high[i] - from[i]) / delta[i];
            float facing = -1;
            if(near > far)
            {
                swap(near, far);
                facing = 1;
            }
            if(near >= entry)
            {
                entry = near;
                entryNormal = vec();
                entryNormal[i] = facing;
            }
            exit = min(exit, far);
            if(entry > exit) return false;
        }
        if(exit <= 0) return false;
        fraction = entry;
        normal = entryNormal;
        return true;
    }

    void seekDevice(float &value, float &velocity, float target, float acceleration, float speed)
    {
        float gap = target - value;
        if(speed <= 0 || fabsf(gap) < 1e-6f) { velocity = 0; return; }
        if(acceleration <= 0) { value = target; velocity = 0; return; }
        if(fabsf(gap - velocity) <= min(speed, acceleration))
        {
            value = target;
            velocity = 0;
            return;
        }
        // The two rate fields are in the order consumed by Halo's
        // accelerate_to_position, which differs from their tag labels.
        float requested = copysignf(min(speed, sqrtf(2 * acceleration * fabsf(gap))), gap);
        float step = clamp(requested - velocity, -acceleration, acceleration);
        velocity += step;
        value = clamp(value + velocity + .5f * step, 0.0f, 1.0f);
    }

    void Scenario::updateDevices(bool gameplay)
    {
        vec feet(player1->o);
        feet.z -= player1->eyeheight;
        for(size_t i = 0; i < devices.size(); ++i)
        {
            if(!deviceActive(i)) continue;
            const Device &d = program.devices[i];
            DeviceState &s = devices[i];
            vec local = deviceLocal(*projection, d, feet);
            bool automatic = !d.control && d.kind == 0 && s.automatic;
            if(gameplay && automatic && ((tick + i) & 3) == 0 &&
               fabsf(local.z) < d.radius && local.squaredlen() < d.radius * d.radius &&
               (!s.oneSided || s.position > 0 || local.x <= 0))
            {
                if(d.positionGroup >= 0) setGroup(d.positionGroup, 1);
                else s.desiredPosition = 1;
                s.hold = -3;
            }
            if(automatic && s.position == 1 && ++s.hold > d.hold)
            {
                if(d.positionGroup >= 0) setGroup(d.positionGroup, 0);
                else s.desiredPosition = 0;
            }
            DeviceState previous = s;
            // A machine with no power transition time (a10's jeff-tube tunnel and cryo doors)
            // powers up at once; seekDevice would otherwise treat the zero rate as never moving
            // and the door stays locked after the script powers it.
            const float wantPower = groupValue(d.powerGroup, s.desiredPower);
            if(d.powerSpeed <= 0) { s.power = wantPower; s.powerVelocity = 0; }
            else seekDevice(s.power, s.powerVelocity, wantPower, d.powerAccel, d.powerSpeed);
            float speed = d.offSpeed * (1 - s.power) + d.onSpeed * s.power;
            float accel = d.offAccel * (1 - s.power) + d.onAccel * s.power;
            if(!d.control && d.kind == 2)
            {
                s.position = fmodf(s.position + speed, 1);
                if(at(groups, d.positionGroup)) groups[d.positionGroup].value = s.position;
            }
            else seekDevice(s.position, s.positionVelocity, groupValue(d.positionGroup, s.desiredPosition), accel, speed);
            if(halo_scripttrace && previous.position != s.position && (s.position == 0 || s.position == 1))
            {
                const Object *object = at(program.objects, d.object);
                conoutf("Halo device: %s %zu reached %.0f at tick %d", object ? object->name.c_str() : "unnamed", i, s.position, tick);
            }
            if(!gameplay || d.elevator < 0 || previous.position == s.position) continue;
            for(const DevicePanel &panel : d.panels)
            {
                if(panel.node != d.elevator || panel.frames.empty()) continue;
                const DeviceBox old = panelBox(d, panel, previous), current = panelBox(d, panel, s);
                vec delta = vec(current.high).sub(old.high);
                delta.rotate_around_z(d.pose.yaw);
                delta = vec(delta.x * projection->xy, -delta.y * projection->xy, delta.z * projection->z);
                if(carryBodies(d, old, delta) && halo_scripttrace && previous.positionVelocity == 0)
                    conoutf("Halo lift: carrying player on device %zu", i);
                break;
            }
        }
    }

    // Everyone standing on a lift deck moves with it: the player, allies and enemies alike, on
    // retail collision as on stock geometry. On a rising deck a body counts once any part of it
    // is over the deck (as collideDevices stands it there), so the deck never lifts into someone
    // standing on its edge, which would wedge them in its box. A descending deck takes only the
    // bodies whose centre is over it: one standing on the landing beside a flush deck would
    // otherwise be dragged down through the landing's floor. Returns whether the player was carried.
    bool Scenario::carryBodies(const Device &d, const DeviceBox &old, const vec &delta)
    {
        auto carry = [&](playerent *body)
        {
            if(!body || body->state != CS_ALIVE || (body->type != ENT_PLAYER && body->type != ENT_BOT)) return false;
            const vec local = deviceLocal(*projection, d, vec(body->o).sub(vec(0, 0, body->eyeheight)));
            const float edge = delta.z > 0 ? body->radius / projection->xy : 0;
            if(!abovePanel(local, old, edge) || fabsf(local.z - old.high.z) > .22f) return false;
            body->o.add(delta);
            body->newpos.add(delta);
            return true;
        };
        const bool player = carry(player1);
        loopv(players) if(players[i] && players[i] != player1) carry(players[i]);
        return player;
    }

    // Boarding a lift: a lift deck at rest may stand a little proud of its landing (the Library's
    // platforms are 0.38 units up at the bottom of their shafts, 2.6 units at floor 1's scale, more
    // than a step). A grounded body may step onto the deck top `lo` from up to 0.45 retail units
    // below it. Returns that allowance in world units, or 0 when `lo` is not a lift deck under it.
    float Scenario::liftBoard(physent *entity, float lo) const
    {
        if(!loaded || cue || editmode || !projection) return 0;
        const Projection &p = *projection;
        const float radius = entity->radius / p.xy;
        for(size_t i = 0; i < devices.size(); ++i)
        {
            if(!deviceActive(i) || halocollisionbaked(int(i))) continue;
            const Device &d = program.devices[i];
            if(d.control || d.elevator < 0 || d.kind == 2) continue;
            const vec local = deviceLocal(p, d, entity->o);
            for(const DevicePanel &panel : d.panels)
            {
                if(panel.node != d.elevator || panel.frames.empty()) continue;
                const DeviceBox box = panelBox(d, panel, devices[i]);
                if(!abovePanel(local, box, radius)) continue;
                const float top = (box.high.z + d.pose.position.z - p.origin.z) * p.z;
                if(fabsf(top - lo) < .01f) return .45f * p.z;
            }
        }
        return 0;
    }

    bool Scenario::collideDevices(physent *entity, float &hi, float &lo, int *blocking) const
    {
        if(!loaded || cue || editmode || entity->type == ENT_CAMERA) return false;
        const Projection &p = *projection;
        float height = entity->eyeheight + entity->aboveeye;
        float feet = entity->o.z - entity->eyeheight;
        float radius = entity->radius / p.xy;
        for(size_t i = 0; i < devices.size(); ++i)
        {
            if(!deviceActive(i) || halocollisionbaked(int(i))) continue;
            const Device &d = program.devices[i];
            if(d.control || d.panels.empty() || d.kind == 2) continue;
            vec local = deviceLocal(p, d, entity->o);
            for(const DevicePanel &panel : d.panels)
            {
                if(panel.frames.empty()) continue;
                DeviceBox box = panelBox(d, panel, devices[i]);
                if(!abovePanel(local, box, radius)) continue;
                // A door leaf folded flat on the floor (c10's interior doors open their lower leaves
                // into the doorway, 0.05 to 0.23 units proud of the floor): bodies walk over it, as
                // Halo's biped does (tools/halo_collision.py floor_leaf; the route planner does the same).
                if(d.kind == 0 && d.elevator < 0 && box.high.z <= kDoorLeafTop && box.high.z - box.low.z <= kDoorLeafThickness)
                    continue;
                float bottom = (box.low.z + d.pose.position.z - p.origin.z) * p.z;
                float top = (box.high.z + d.pose.position.z - p.origin.z) * p.z;
                if(feet >= top - .23f * p.z) lo = max(lo, top);
                else if(feet + height <= bottom + .10f * p.z) hi = min(hi, bottom);
                else if(d.elevator != panel.node && top - bottom > .05f * p.z) { if(blocking) *blocking = int(i); return true; }
                else lo = max(lo, top);
                if(hi - lo < height) { if(blocking) *blocking = int(i); return true; }
            }
        }
        return false;
    }

    bool Scenario::traceDevices(const vec &from, const vec &to, HaloCollisionHit &hit) const
    {
        if(!loaded || !projection || cue || editmode) return false;
        const Projection &p = *projection;
        bool found = false;
        for(size_t i = 0; i < devices.size(); ++i)
        {
            // Baked machines are part of the structure trace.
            if(!deviceActive(i) || halocollisionbaked(int(i))) continue;
            const Device &d = program.devices[i];
            if(d.control || d.panels.empty() || d.kind == 2) continue;
            const vec local = deviceLocal(p, d, from);
            const vec delta = deviceLocal(p, d, to).sub(local);
            for(const DevicePanel &panel : d.panels)
            {
                if(panel.frames.empty()) continue;
                vec normal;
                if(!tracePanel(panelBox(d, panel, devices[i]), local, delta, hit.fraction, normal)) continue;
                normal.rotate_around_z(d.pose.yaw);
                normal = vec(normal.x / p.xy, -normal.y / p.xy, normal.z / p.z);
                if(!normal.iszero()) normal.normalize();
                hit.normal = normal;
                hit.material = -1;
                hit.flags = 0;
                hit.surface = -1;
                found = true;
            }
        }
        return found;
    }

    // A damage target's frame: a world point in retail units relative to the object, unrotated.
    vec targetLocal(const Projection &p, const Pose &pose, const vec &point)
    {
        vec local(p.origin.x + (point.x - p.margin) / p.xy, p.origin.y - (point.y - p.margin) / p.xy, p.origin.z + point.z / p.z);
        local.sub(pose.position).rotate_around_z(-pose.yaw);
        return local;
    }

    int Scenario::traceTargets(const vec &from, const vec &to, float &fraction) const
    {
        if(!loaded || !projection || cue || editmode) return -1;
        int found = -1;
        for(size_t i = 0; i < actors.size(); ++i)
        {
            const Object &object = program.objects[i];
            if(object.vitality <= 0 || !actors[i].visible) continue;
            const Pose pose = currentActor(int(i));
            const vec local = targetLocal(*projection, pose, from), delta = targetLocal(*projection, pose, to).sub(local);
            vec normal;
            if(tracePanel({object.targetLow, object.targetHigh}, local, delta, fraction, normal)) found = int(i);
        }
        return found;
    }

    vec Scenario::targetCentre(int target) const
    {
        const Object &object = program.objects[target];
        const Pose pose = currentActor(target);
        vec centre = vec(object.targetLow).add(object.targetHigh).mul(.5f);
        centre.rotate_around_z(pose.yaw);
        return toWorld(*projection, centre.add(pose.position));
    }

    int Scenario::targetsNear(const vec &centre, float radius, int *targets, float *distances, int capacity) const
    {
        if(!loaded || !projection || cue || editmode) return 0;
        int count = 0;
        for(size_t i = 0; i < actors.size() && count < capacity; ++i)
        {
            const Object &object = program.objects[i];
            if(object.vitality <= 0 || !actors[i].visible) continue;
            // The distance from the blast to the nearest point of the target's box, in world units.
            const vec local = targetLocal(*projection, currentActor(int(i)), centre);
            vec gap;
            loopk(3) gap[k] = max(max(object.targetLow[k] - local[k], local[k] - object.targetHigh[k]), 0.0f);
            const float distance = vec(gap.x * projection->xy, gap.y * projection->xy, gap.z * projection->z).magnitude();
            if(distance >= radius) continue;
            targets[count] = int(i);
            distances[count++] = distance;
        }
        return count;
    }

    bool Scenario::damageTarget(int target, float amount)
    {
        if(!at(actors, target) || !(amount > 0)) return false;
        const Object &object = program.objects[target];
        Actor &actor = actors[target];
        if(object.vitality <= 0 || !actor.visible || actor.cannotTakeDamage) return false;
        actor.damage = min(object.vitality, actor.damage + amount);
        if(halo_scripttrace) conoutf("Halo target: %s took %.1f, health %.2f", object.name.c_str(), amount,
                                     max(0.0f, 1 - actor.damage / object.vitality));
        return true;
    }

    bool Scenario::useDevice()
    {
        if(!loaded || cue || player1->state != CS_ALIVE || editmode) return false;
        size_t nearest = devices.size();
        float best = 1.65f * 1.65f;
        vec center(player1->o);
        center.z -= player1->eyeheight * .4f;
        for(size_t i = 0; i < devices.size(); ++i)
        {
            const Device &d = program.devices[i];
            const DeviceState &s = devices[i];
            if(!deviceActive(i) || !d.control || d.typeFlags != 0 || (d.flags & 16) ||
               s.power < .99f || ((d.flags & 4) && s.changed)) continue;
            float distance = deviceLocal(*projection, d, center).squaredlen();
            if(distance >= best) continue;
            best = distance;
            nearest = i;
        }
        if(nearest == devices.size()) return false;
        const Device &d = program.devices[nearest];
        DeviceState &s = devices[nearest];
        float value = d.kind == 0 ? (groupValue(d.positionGroup, s.desiredPosition) > .5f ? 0 : 1)
                    : d.kind == 1 ? 1 : d.kind == 2 ? 0 : clamp(d.call, 0.0f, 1.0f);
        if(d.positionGroup >= 0 && !setGroup(d.positionGroup, value)) return false;
        s.desiredPosition = value;
        s.changed = true;
        if(halo_scripttrace) conoutf("Halo control: device %zu group %d -> %.2f", nearest, d.positionGroup, value);
        return true;
    }

    void Scenario::renderDevices() const
    {
        if(!loaded) return;
        for(size_t i = 0; i < devices.size(); ++i)
        {
            if(!deviceActive(i)) continue;
            const Device &d = program.devices[i];
            if(d.model.empty()) continue;
            const Pose &pose = d.pose;
            vec point = cue ? vec(pose.position.x * 4, -pose.position.y * 4, pose.position.z * 4)
                            : toWorld(*projection, pose.position);
            if(camera1->o.dist(point) > (cue ? 3200 : fog) || (!cue &&
               (point.x < 0 || point.y < 0 || point.x >= ssize || point.y >= ssize))) continue;
            int anim = ANIM_MAPMODEL | (d.loopFlags & 1 ? ANIM_LOOP : 0);
            int time = lastmillis - int(ceilf(deviceFrame(d, devices[i]) * 1000 / ticksPerSecond));
            vec angles = renderAngles(pose);
            rendermodel(d.model.c_str(), anim, 0, -1, point, angles.z, 180 - angles.x,
                        angles.y, 0, time, nullptr, nullptr, cue ? 1.0f : projection->xy / 4);
        }
    }

    const Sound *Scenario::sound(int id) const
    {
        auto found = std::lower_bound(program.sounds.begin(), program.sounds.end(), id,
                                     [](const Sound &sample, int key) { return sample.id < key; });
        return found != program.sounds.end() && found->id == id ? &*found : nullptr;
    }

    int Scenario::play(int id, float gain, bool loop, const vec *position)
    {
        if(!sound(id) || !alcGetCurrentContext() || !soundvol || channels.size() >= 24) return 0;
        defformatstring(sample)("halo/scene/%s/%d", cache, id);
        std::unique_ptr<Playback> playback(new Playback);
        copystring(playback->name, sample);
        playback->buffer.name = playback->name;
        if(!playback->channel.valid || !playback->buffer.load()) return 0;
        source &channel = playback->channel;
        if(!channel.buffer(playback->buffer.id)) return 0;
        channel.sourcerelative(!position);
        channel.position(position ? *position : vec());
        // sound_class_set_gain fades the sound's retail class (music, ambience, dialogue).
        playback->gain = clamp(gain, 0.0f, 1.0f);
        playback->soundClass = halofxsoundclass(cache, id);
        playback->applied = halofxclassgain(playback->soundClass);
        channel.gain(playback->gain * playback->applied);
        channel.looping(loop);
        if(!channel.play()) return 0;
        int token = ++nextChannel;
        channels.emplace(token, std::move(playback));
        return token;
    }

    void Scenario::stopLocation(int token)
    {
        auto found = channels.find(token);
        if(found == channels.end()) return;
        found->second->channel.stop();
        found->second->channel.buffer(0);
        channels.erase(found);
    }

    void Scenario::stopSounds()
    {
        while(!channels.empty()) stopLocation(channels.begin()->first);
        voices.clear();
        music.clear();
        talking.clear();
        subtitle.clear();
        subtitleEnd = 0;
    }

    void Scenario::startConversation(int id)
    {
        const Conversation *conversation = at(program.conversations, id);
        if(!conversation || conversation->lines.empty() || conversationStatus[id] == 2) return;
        conversationStatus[id] = 2;
        talking.push_back({id, 0, tick + conversation->lines[0].delay});
    }

    void Scenario::stopConversation(int id)
    {
        if(!at(conversationStatus, id)) return;
        conversationStatus[id] = 5;
        talking.erase(std::remove_if(talking.begin(), talking.end(), [id](const Talking &t) { return t.id == id; }), talking.end());
        for(Voice &voice : voices) if(voice.id == -id - 2)
        {
            stopLocation(voice.location);
            voice.until = tick;
        }
    }

    void Scenario::startMusic(int id, float gain)
    {
        for(const Music &item : music) if(item.id == id && !item.ending) return;
        for(const Loop &loop : program.loops) if(loop.id == id)
        {
            for(const Track &track : loop.tracks)
            {
                int sample = track.start >= 0 ? track.start : track.loop;
                const Sound *clip = sound(sample);
                if(!clip) continue;
                bool repeat = loop.repeat && track.loop >= 0;
                bool directLoop = repeat && track.start < 0;
                int channel = play(sample, gain * track.gain, directLoop);
                if(halo_scripttrace) conoutf("Halo audio: loop %d / %d (audio %s)", id, sample, channel ? "playing" : "unavailable");
                music.push_back({id, &track, channel, directLoop ? INT_MAX : tick + clip->duration,
                                 gain * track.gain, repeat, false, false, gain});
            }
            return;
        }
    }

    void Scenario::stopMusic(int id)
    {
        for(Music &item : music) if(item.id == id && !item.ending)
        {
            stopLocation(item.location);
            int sample = item.alternate && item.track->altEnd >= 0 ? item.track->altEnd : item.track->end;
            const Sound *clip = sound(sample);
            item.location = clip ? play(sample, item.gain, false) : 0;
            item.until = clip ? tick + clip->duration : tick;
            item.ending = true;
        }
    }

    void Scenario::setAlternate(int id, bool enabled)
    {
        for(Music &item : music) if(item.id == id && !item.ending && item.alternate != enabled)
        {
            int sample = enabled ? item.track->alt : item.track->loop;
            const Sound *clip = sound(sample);
            if(!clip) continue;
            stopLocation(item.location);
            item.location = play(sample, item.gain, item.repeat);
            item.until = item.repeat ? INT_MAX : tick + clip->duration;
            item.alternate = enabled;
            item.ending = !item.repeat;
        }
    }

    void Scenario::updateSound()
    {
        // Class gains fading (sound_class_set_gain) reach the channels already playing.
        for(auto &entry : channels)
        {
            Playback &playback = *entry.second;
            const float classGain = halofxclassgain(playback.soundClass);
            if(fabsf(classGain - playback.applied) < .002f) continue;
            playback.applied = classGain;
            playback.channel.gain(playback.gain * classGain);
        }
        voices.erase(std::remove_if(voices.begin(), voices.end(), [&](const Voice &voice)
        {
            if(voice.until <= tick) stopLocation(voice.location);
            return std::max(voice.until, voice.scriptUntil) <= tick;
        }), voices.end());
        music.erase(std::remove_if(music.begin(), music.end(), [&](Music &item)
        {
            if(item.until > tick) return false;
            stopLocation(item.location);
            if(item.ending || item.track->start < 0 || !sound(item.track->loop)) return true;
            item.location = play(item.track->loop, item.gain, item.repeat);
            item.until = item.repeat ? INT_MAX : tick + sound(item.track->loop)->duration;
            item.ending = !item.repeat;
            return false;
        }), music.end());
        talking.erase(std::remove_if(talking.begin(), talking.end(), [&](Talking &entry)
        {
            if(tick < entry.wake) return false;
            const Conversation &conversation = program.conversations[entry.id];
            if(size_t(entry.line) >= conversation.lines.size())
            {
                conversationStatus[entry.id] = 5;
                return true;
            }
            const Line &line = conversation.lines[entry.line++];
            const Sound *clip = line.variants.empty() ? nullptr : sound(line.variants.front());
            entry.wake = tick + (clip ? clip->duration : 0);
            if(size_t(entry.line) < conversation.lines.size()) entry.wake += conversation.lines[entry.line].delay;
            if(clip)
            {
                int channel = play(clip->id, 1, false);
                voices.push_back({-entry.id - 2, channel, tick + clip->duration, tick + clip->duration});
                if(!clip->subtitle.empty()) { subtitle = clip->subtitle; subtitleEnd = tick + clip->duration; }
                if(halo_scripttrace) conoutf("Halo dialogue: %s / %d at tick %d (audio %s)", conversation.name.c_str(), clip->id, tick, channel ? "playing" : "unavailable");
            }
            return false;
        }), talking.end());
    }

    void Scenario::setActor(int id, bool visible, bool reset, bool bulk)
    {
        if(!at(actors, id)) return;
        if(reset)
        {
            actors[id] = {};
            actors[id].pose = program.objects[id].pose;
        }
        // object_create of an object that does not exist yet makes it at its scenario placement, as
        // in Halo; a section started without an opening scene used to leave it at the origin (a50's
        // captain_keyes, which ai_attach now turns into an actor where the object stands).
        else if(visible && !actors[id].visible && actors[id].parent < 0 && !actors[id].recording)
            actors[id].pose = program.objects[id].pose;
        actors[id].visible = visible;
        if(cue && cue->trigger == SceneTrigger::Call) sceneObjects[id] = visible;
        // A dropship destroyed with actors aboard sets them down where they are; a destroyed
        // Pelican the player boarded lets them out.
        if(!visible)
        {
            for(Cargo &entry : cargo) if(entry.vehicle == id) releaseCargo(entry);
            cargo.erase(std::remove_if(cargo.begin(), cargo.end(), [id](const Cargo &entry) { return entry.vehicle == id; }), cargo.end());
            if(boardedVehicle == id) unboard(true);
        }
        int device = objectDevices[id];
        if(device >= 0) devices[device].visible = visible;
        // A vehicle the engine drives for this object (halo_vehicle.cpp) is created and destroyed with it.
        halovehicleobject(id, visible, reset, bulk);
        // So is scenario equipment with this name (halo.cpp).
        haloequipmentobject(id, visible, reset);
        // And the actor ai_attach made of it.
        if(!visible || reset) detachActor(id);
    }

    Value Scenario::command(Thread &thread, const Node &node, const char *name, const std::vector<Value> &args, bool scene)
    {
        auto is = [name](const char *candidate) { return !strcmp(name, candidate); };
        auto value = [&](size_t index, double fallback = 0) { return index < args.size() ? args[index].number : fallback; };
        auto index = [&](size_t i) { return i < args.size() ? args[i].integer() : -1; };
        auto text = [&](size_t i) { return i < args.size() ? args[i].text : ""; };
        auto truth = [&](size_t i) { return i < args.size() && args[i].truth(); };
        auto result = [&](double value) { return number(value, node.type); };
        auto object = [&](size_t i)
        {
            int id = named(program.objects, text(i));
            return id >= 0 ? id : index(i);
        };

        switch(node.opcode)
        {
            case 7: case 8: case 9: case 10: case 11: case 12:
            {
                double out = value(0);
                for(size_t i = 1; i < args.size(); ++i)
                {
                    switch(node.opcode)
                    {
                        case 7: out += value(i); break;
                        case 8: out -= value(i); break;
                        case 9: out *= value(i); break;
                        case 10: out = value(i) ? out / value(i) : 0; break;
                        case 11: out = std::min(out, value(i)); break;
                        case 12: out = std::max(out, value(i)); break;
                    }
                }
                return result(out);
            }
            case 13: return result(value(0) == value(1));
            case 14: return result(value(0) != value(1));
            case 15: return result(value(0) > value(1));
            case 16: return result(value(0) < value(1));
            case 17: return result(value(0) >= value(1));
            case 18: return result(value(0) <= value(1));
            case 21: wake(index(0), scene); return {};
            case 23: return args.empty() ? number(noObject, node.type) : args.front();
        }
        if(is("not")) return result(!truth(0));
        if(is("players")) return result(playerList);
        if(is("list_get"))
        {
            const int list = index(0), n = index(1);
            if(list == playerList) return result(n == 0 ? playerObject : noObject);
            if(list <= actorList)
            {
                // The n-th living actor of the list's encounter, squad or platoon.
                const int reference = at(actorLists, actorList - list) ? actorLists[actorList - list] : -1;
                int found = noObject, seen = 0;
                eachActor(reference, [&](SquadBot &, int i) { if(seen++ == n) found = actorObject - i; });
                return result(found);
            }
            return result(noObject);
        }
        if(is("list_count"))
        {
            const int list = index(0);
            if(list == playerList) return result(1);
            if(list <= actorList) return result(at(actorLists, actorList - list) ? living(actorLists[actorList - list]) : 0);
            return result(max(0, list));
        }
        if(is("ai_place") || is("ai_spawn_actor"))
        {
            if(!scene) placeSquads(index(0), is("ai_spawn_actor") ? 1 : index(1), !is("ai_spawn_actor"));
            return {};
        }
        if(is("ai_kill") || is("ai_kill_silent") || is("ai_erase"))
        {
            if(!scene) eraseSquads(index(0), is("ai_erase"));
            return {};
        }
        if(is("ai_erase_all"))
        {
            if(!scene)
            {
                HaloScriptDamage scripted;
                for(SquadBot &bot : squadBots)
                {
                    if(!bot.bot || bot.bot->state != CS_ALIVE) continue;
                    if(monitorBody(bot.bot)) haloerasebot(bot.bot);
                    else dodamage(INT_MAX, bot.bot, player1, GUN_ASSAULT);
                }
                haloclearflood();
            }
            return {};
        }
        if(is("ai_living_count")) return result(living(index(0)));
        if(is("ai_actors")) return result(actorListFor(index(0)));
        if(is("ai_nonswarm_count"))
        {
            // Infection forms swarm and are left out of this count.
            int count = 0;
            eachActor(index(0), [&](SquadBot &bot, int) { if(bot.start->actor != 6) ++count; });
            return result(count);
        }
        if(is("ai_living_fraction") || is("ai_strength"))
        {
            int total = 0, alive = 0;
            float strength = 0;
            for(const SquadBot &bot : squadBots)
            {
                if(!matchesSquad(index(0), bot.encounter, bot.squad)) continue;
                ++total;
                if(!bot.bot || bot.bot->state != CS_ALIVE) continue;
                ++alive;
                strength += haloactorhealthfraction(bot.bot);
            }
            if(is("ai_living_fraction")) return result(total ? float(alive) / total : 0.0f);
            return result(total ? strength / total : 0.0f);
        }
        if(is("ai_status"))
        {
            int status = 0;
            eachActor(index(0), [&](SquadBot &bot, int) { status = max(status, haloactorstatus(bot.bot)); });
            return result(status);
        }
        // Switches the mission scripts flip on whole encounters, platoons or squads.
        struct { const char *name; int flag; bool fixed; } switches[] = {
            {"ai_braindead", HALO_AI_BRAINDEAD, false}, {"ai_playfight", HALO_AI_PLAYFIGHT, false},
            {"ai_disregard", HALO_AI_DISREGARD, false}, {"ai_set_blind", HALO_AI_BLIND, false},
            {"ai_set_deaf", HALO_AI_DEAF, false}, {"ai_force_active", HALO_AI_FORCE_ACTIVE, false},
            {"ai_magically_see_players", HALO_AI_MAGIC_SIGHT, true}, {"ai_magically_see_encounter", HALO_AI_MAGIC_SIGHT, true},
            {"ai_follow_target_players", HALO_AI_FOLLOW_PLAYER, true}, {"ai_try_to_fight_player", HALO_AI_MAGIC_SIGHT, true},
            {"ai_try_to_fight", HALO_AI_FORCE_ACTIVE, true},
        };
        for(const auto &sw : switches) if(is(sw.name))
        {
            const bool on = sw.fixed ? true : truth(1);
            if(!scene) eachActor(index(0), [&](SquadBot &bot, int) { haloactorsetflag(bot.bot, sw.flag, on); });
            return {};
        }
        if(is("ai_attack") || is("ai_defend") || is("ai_retreat") || is("ai_maneuver"))
        {
            const int posture = is("ai_attack") ? HALO_ATTACKING : is("ai_maneuver") ? HALO_PURSUING : HALO_DEFENDING;
            if(!scene) eachActor(index(0), [&](SquadBot &bot, int) { haloactorsetposture(bot.bot, posture); });
            return {};
        }
        if(is("ai_command_list") || is("ai_command_list_by_unit"))
        {
            const int list = index(1);
            const HaloCommandProgram *commands = list >= 0 && list < int(commandPrograms.size()) ? &commandPrograms[list] : nullptr;
            if(scene) return {};
            if(is("ai_command_list_by_unit")) { if(playerent *bot = actorFromValue(index(0))) haloactorcommand(bot, commands); }
            else eachActor(index(0), [&](SquadBot &bot, int) { haloactorcommand(bot.bot, commands); });
            return {};
        }
        if(is("ai_command_list_advance"))
        {
            if(!scene) eachActor(index(0), [&](SquadBot &bot, int) { haloactorcommandadvance(bot.bot); });
            return {};
        }
        if(is("ai_command_list_status"))
        {
            // 0 no list, 1 still running, 2 finished or waiting at a loop atom, across the given units.
            const int value = index(0);
            int status = 0;
            if(value <= actorList)
            {
                const int reference = at(actorLists, actorList - value) ? actorLists[actorList - value] : -1;
                bool running = false, finished = false;
                eachActor(reference, [&](SquadBot &bot, int)
                {
                    const int s = haloactorcommandstatus(bot.bot);
                    running |= s == 1;
                    finished |= s == 2;
                });
                status = running ? 1 : finished ? 2 : 0;
            }
            else if(playerent *bot = actorFromValue(value)) status = haloactorcommandstatus(bot);
            return result(status);
        }
        if(is("unit_get_health"))
        {
            playerent *p = actorFromValue(index(0));
            if(p) return result(p->state == CS_ALIVE ? haloactorhealthfraction(p) : -1.0f);
            // A scripted unit without an AI body (a10's Keyes on the bridge) cannot be hurt
            // here: unhurt while it exists. Reporting -1 made a10 think the player shot Keyes.
            // Damage targets (d40's manifold detectors) report what the player's shots left.
            const int value = index(0);
            // A vehicle the engine drives (halo_vehicle.cpp): its body health while it exists, -1
            // once it blew up or was destroyed (b40's dialog_ext_a_c waits for crev_ent_wraith_a
            // and _b to reach 0 or less).
            if(value >= 0 && halovehiclebound(value)) return result(halovehiclehealth(value));
            // An attached object whose actor died or is gone (ai_attach, attachActors).
            if(attachedEntry(value) >= 0) return result(-1.0f);
            // One carried in from the section before that has not spawned here yet (spawnCarried).
            for(const CarriedAttach &c : carriedAttached) if(c.object == value) return result(c.health);
            if(value < 0 || !at(actors, value) || !actors[value].visible) return result(-1.0f);
            const Object &unit = program.objects[value];
            return result(unit.vitality > 0 ? max(0.0f, 1 - actors[value].damage / unit.vitality) : 1.0f);
        }
        if(is("objects_can_see_object") || is("objects_can_see_flag"))
        {
            // Whether the player looks towards the object or flag within the given cone.
            vec target;
            bool found = false;
            if(is("objects_can_see_object")) found = objectPosition(index(1), target);
            else if(const Flag *flag = at(program.flags, index(1))) { if(projection) { target = toWorld(*projection, flag->pose.position); found = true; } }
            if(!found || player1->state != CS_ALIVE) return result(false);
            vec delta = vec(target).sub(player1->o);
            const float distance = delta.magnitude();
            if(distance < 0.01f) return result(true);
            const vec view(sinf(player1->yaw * RAD) * cosf(player1->pitch * RAD), -cosf(player1->yaw * RAD) * cosf(player1->pitch * RAD),
                           sinf(player1->pitch * RAD));
            const float cone = max(1.0f, float(value(2, 30))) * RAD;
            if(delta.dot(view) / distance < cosf(cone)) return result(false);
            vec end = target;
            return result(!intersectgeometry(player1->o, end));
        }
        // The engine's vehicles (halo_vehicle.cpp) answer for the objects they stand for here;
        // vehicleCommand (halo_script_ai.inc) falls through for every other object.
        {
            Value vehicle;
            if(vehicleCommand(name, args, scene, vehicle)) return vehicle;
        }
        // Vehicles the engine does not drive (Pelicans, Banshees, Ghosts): these stand-ins let
        // the waits that depend on boarding them move on instead of hanging.
        if(is("vehicle_test_seat_list") || is("vehicle_test_seat")) return result(true);
        if(is("ai_going_to_vehicle")) return result(0);
        if(is("ai_go_to_vehicle") || is("ai_vehicle_encounter") || is("ai_vehicle_enterable_distance"))
            return {};
        if(is("ai_migrate"))
        {
            int oldGroup = index(0), newGroup = index(1);
            if(!scene && newGroup != -1) for(SquadBot &bot : squadBots)
                if(bot.bot && matchesSquad(oldGroup, bot.encounter, bot.squad)) migrateBot(bot, newGroup);
            return {};
        }
        if(is("random_range") || is("real_random_range"))
        {
            double lower = value(0), upper = value(1);
            if(upper <= lower) return result(lower);
            double selected = std::uniform_real_distribution<double>(lower, upper)(random);
            return result(is("random_range") ? floor(selected) : selected);
        }
        if(is("game_difficulty_get") || is("game_difficulty_get_real")) return result(halodifficulty());
        if(is("game_reverted")) return result(skipping);
        if(is("game_is_cooperative") || is("game_saving")) return result(0);
        // Quiet: no hostile actor is fighting (the test game_safe_to_save makes, the player alive or not).
        if(is("game_all_quiet"))
        {
            for(const SquadBot &bot : squadBots)
                if(bot.bot && bot.bot->state == CS_ALIVE && !bot.suspended && !haloteamfriendly(haloactorteam(bot.bot)) &&
                   !haloactorpassive(bot.bot) && haloactorstatus(bot.bot) >= 6) return result(0);
            return result(1);
        }
        // Safe to speak: no scripted line or conversation is still playing, so a new one does not talk over it.
        if(is("game_safe_to_speak"))
        {
            if(!talking.empty()) return result(0);
            for(const Voice &voice : voices) if(voice.until > tick) return result(0);
            return result(1);
        }
        if(is("cinematic_skip_stop_internal")) return result(1);
        if(is("volume_teleport_players_not_inside"))
        {
            // (volume_teleport_players_not_inside volume flag): a player outside the volume (or a
            // volume the scenario does not have, such as "null") goes to the flag.
            if(!testVolume(index(0)) && at(program.flags, index(1)))
            {
                if(!scene) placePlayer(index(1), -1, name);
                else if(cue) { handoffTeleport = index(1); handoffFacing = -1; }
            }
            return {};
        }
        if(is("structure_bsp_index")) return result(scene ? world : projection ? projection->bsp : -1);
        if(is("switch_bsp"))
        {
            if(scene) setWorld(index(0));
            else if(runningGameplay) switchBsp(index(0), "switch_bsp");
            return {};
        }
        if(is("game_save") || is("game_save_no_timeout") || is("game_save_totally_unsafe"))
        {
            if((!scene || (cue && (cue->trigger == SceneTrigger::Cartographer || cue->trigger == SceneTrigger::Call))) &&
               !is("game_save_totally_unsafe"))
                pendingCheckpoint = true;
            return {};
        }
        if(is("player_add_equipment"))
        {
            // (player_add_equipment unit profile reset): a10's bridge0_profile (the pistol at the
            // bridge), c20's easy_start (a shotgun on Easy). Only the player has a body here; a
            // co-op player1 is no one.
            if(index(0) == playerObject || index(0) == playerList)
            {
                HaloStartingProfile profile;
                if(haloscenarioprofile(index(1), text(1), profile)) haloplayeraddequipment(profile, truth(2));
                else if(halo_scripttrace) conoutf("Halo script: no starting profile %s (an .hscb older than version 9?)", text(1));
            }
            return {};
        }
        if(is("game_won") || is("game_lost"))
        {
            // The mission scripts end the level here. The engine decides what that means (ending, or revert).
            (is("game_won") ? gameWon : gameLost) = true;
            if(halo_scripttrace) conoutf("Halo script: %s at tick %d", is("game_won") ? "game_won" : "game_lost", tick);
            return {};
        }
        if(is("cinematic_skip_start_internal")) return result(1);
        if(is("camera_control")) { if(scene) cameraControl = truth(0); return {}; }
        if(is("camera_set") || is("camera_set_relative"))
        {
            if(scene) setCamera(index(0), index(1), is("camera_set") ? noObject : object(2));
            return {};
        }
        if(is("camera_set_first_person"))
        {
            if(scene && at(actors, object(0))) camera.firstPerson = object(0);
            return {};
        }
        if(is("camera_time")) return result(std::max(0, camera.end - tick));
        if(is("cinematic_start") || is("cinematic_stop") || is("cinematic_show_letterbox"))
        {
            if(scene) letterbox = is("cinematic_start") || (is("cinematic_show_letterbox") && truth(0));
            return {};
        }
        if(is("cinematic_set_near_clip_distance"))
        {
            if(scene) nearClip = clamp(float(value(0)), .001f, 1.0f);
            return {};
        }
        if(is("cinematic_set_title") || is("cinematic_set_title_delayed"))
        {
            chapter = index(0);
            chapterStart = tick + clamp(int(value(1) * ticksPerSecond), 0, 180000);
            return {};
        }
        if(is("fade_in") || is("fade_out"))
        {
            if(scene)
            {
                bool out = is("fade_out");
                fade = {vec(float(value(0)), float(value(1)), float(value(2))), out ? 0.0f : 1.0f, out ? 1.0f : 0.0f,
                        tick, tick + clamp(index(3), 0, 180000)};
            }
            return {};
        }
        if(is("sound_impulse_time"))
        {
            for(const Voice &voice : voices)
                if(voice.id == index(0)) return result(max(0, voice.scriptUntil - tick));
            return result(0);
        }
        if(is("sound_impulse_start") || is("sound_impulse_trigger"))
        {
            const Sound *clip = sound(index(0));
            if(clip)
            {
                for(Voice &voice : voices) if(voice.id == clip->id)
                {
                    stopLocation(voice.location);
                    voice.id = -1;
                    voice.until = voice.scriptUntil = tick;
                }
                const int id = object(1);
                const vec position = scene && at(actors, id) ? vec(currentActor(id).position).mul(4) : vec();
                const vec localPosition(position.x, -position.y, position.z);
                int channel = play(clip->id, float(value(2, 1)), false, scene && at(actors, id) ? &localPosition : nullptr);
                voices.push_back({clip->id, channel, tick + clip->duration, tick + clip->scriptDuration});
                if(!clip->subtitle.empty()) { subtitle = clip->subtitle; subtitleEnd = tick + clip->duration; }
                if(halo_scripttrace) conoutf("Halo dialogue: %s / %d at tick %d (audio %s)", text(0), clip->id, tick, channel ? "playing" : "unavailable");
            }
            return {};
        }
        if(is("sound_impulse_stop"))
        {
            for(Voice &voice : voices) if(voice.id == index(0)) { stopLocation(voice.location); voice.until = voice.scriptUntil = tick; }
            return {};
        }
        if(is("sound_looping_start")) { startMusic(index(0), float(value(2, 1))); return {}; }
        if(is("sound_looping_stop") || is("sound_looping_set_alternate"))
        {
            // A Call scene that stops or switches a gameplay loop it cut: so it stays after the scene.
            const int id = index(0);
            for(size_t i = 0; cue && cue->trigger == SceneTrigger::Call && i < callMusic.size(); ++i)
            {
                if(callMusic[i].id != id) continue;
                if(is("sound_looping_stop")) callMusic.erase(callMusic.begin() + i);
                else callMusic[i].alternate = truth(1);
                break;
            }
            if(is("sound_looping_stop")) stopMusic(id);
            else setAlternate(id, truth(1));
            return {};
        }
        if(is("ai_conversation")) { startConversation(index(0)); return result(1); }
        if(is("ai_conversation_stop")) { stopConversation(index(0)); return {}; }
        if(is("ai_conversation_status"))
        {
            const int *status = at(conversationStatus, index(0));
            return result(status ? *status : 0);
        }
        if(is("object_create") || is("object_create_anew")) { setActor(object(0), true, is("object_create_anew")); return {}; }
        if(is("object_destroy")) { setActor(object(0), false, true); return {}; }
        if(is("object_destroy_all"))
        {
            for(size_t i = 0; i < actors.size(); ++i) setActor(int(i), false, true, true);
            return {};
        }
        if(is("object_create_containing") || is("object_create_anew_containing") || is("object_destroy_containing"))
        {
            if(*text(0)) for(size_t i = 0; i < actors.size(); ++i)
                if(program.objects[i].name.find(text(0)) != std::string::npos)
                    setActor(int(i), !is("object_destroy_containing"), true, true);
            return {};
        }
        if(is("object_teleport") || is("object_set_facing"))
        {
            const Flag *flag = at(program.flags, index(1));
            int id = object(0);
            const bool teleport = is("object_teleport");
            if(flag && (id == playerObject || id == playerList))
            {
                // The player: a scene keeps the place for its end (the player is off stage
                // meanwhile); a gameplay script moves them now.
                if(scene && cue)
                {
                    if(teleport) { handoffTeleport = index(1); handoffFacing = -1; }
                    else handoffFacing = index(1);
                }
                else if(!scene) placePlayer(teleport ? index(1) : -1, teleport ? -1 : index(1), name);
                return {};
            }
            if(flag && !scene && projection)
                if(playerent *unit = actorFromValue(id))
                {
                    // An actor (list_get of ai_actors): moved to the flag, facing its way.
                    if(teleport) haloenemymove(unit, toWorld(*projection, flag->pose.position));
                    unit->yaw = 90 - flag->pose.yaw / RAD;
                    if(unit->type == ENT_BOT) static_cast<botent *>(unit)->targetyaw = unit->yaw;
                    return {};
                }
            if(flag && at(actors, id))
            {
                if(is("object_teleport"))
                {
                    actors[id].pose.position = flag->pose.position;
                    actors[id].grounded = false;
                    actors[id].speed = 0;
                }
                actors[id].pose.yaw = flag->pose.yaw;
                actors[id].pose.pitch = flag->pose.pitch;
                actors[id].pose.roll = 0;
                if(is("object_teleport") && !scene && projection)
                    halovehicleteleport(id, toWorld(*projection, flag->pose.position), 90 - flag->pose.yaw / RAD);
            }
            return {};
        }
        if(is("objects_attach") || is("objects_detach") || is("unit_enter_vehicle") || is("vehicle_load_magic") ||
           is("unit_exit_vehicle") || is("vehicle_unload"))
        {
            bool enter = is("unit_enter_vehicle"), load = is("vehicle_load_magic");
            bool attach = is("objects_attach");
            int parent = object(enter ? 1 : 0), child = object(attach || load ? 2 : enter || is("unit_exit_vehicle") ? 0 : 1);
            if(is("vehicle_unload"))
            {
                int released = 0;
                for(size_t i = 0; i < actors.size(); ++i)
                {
                    Actor &passenger = actors[i];
                    if(passenger.parent != parent || (*text(1) && passenger.parentMarker != text(1))) continue;
                    passenger.pose = currentActor(int(i));
                    passenger.parent = noObject;
                    ++released;
                }
                return result(released);
            }
            if(!at(actors, child) || parent == child && !is("unit_exit_vehicle")) return result(0);
            Actor &actor = actors[child];
            if(attach || enter || load)
            {
                if(!at(program.objects, parent)) return result(0);
                actor.parent = parent;
                actor.parentMarker = text(enter ? 2 : 1);
                actor.childMarker = attach ? text(3) : "";
                if(enter || load)
                {
                    const Object &vehicle = program.objects[parent];
                    const Seat *seat = at(vehicle.seats, named(vehicle.seats, actor.parentMarker.c_str()));
                    if(!seat && !vehicle.seats.empty() && actor.parentMarker.empty()) seat = &vehicle.seats.front();
                    if(seat) actor.parentMarker = seat->marker;
                }
                actor.visible = true;
            }
            else
            {
                actor.pose = currentActor(child);
                actor.parent = noObject;
            }
            return result(1);
        }
        if(is("object_set_scale"))
        {
            int id = object(0);
            if(at(actors, id)) actors[id].scale = clamp(float(value(1)), .001f, 1000.0f);
            return {};
        }
        if(is("unit_set_seat"))
        {
            int id = object(0);
            if(at(actors, id))
            {
                if(!strcmp(text(1), "alert")) actors[id].baseSeat = 1;
                else if(!strcmp(text(1), "stand") || !strcmp(text(1), "crouch")) actors[id].baseSeat = 3;
                else actors[id].baseSeat = -1;
            }
            return {};
        }
        if(is("custom_animation") || is("unit_custom_animation_at_frame"))
        {
            int id = object(0);
            if(at(actors, id)) for(const Animation &animation : program.animations)
            {
                if(animation.object != id || animation.graph != index(1) || strcasecmp(animation.clip.c_str(), text(2))) continue;
                Actor &actor = actors[id];
                actor.animation = &animation;
                actor.animationStart = tick - (is("unit_custom_animation_at_frame") ? max(0, index(4)) : 0);
                actor.animationEnd = actor.animationStart + animation.ticks;
                actor.animationLoop = false;
                break;
            }
            return result(1);
        }
        if(is("unit_get_custom_animation_time"))
        {
            const Actor *actor = at(actors, object(0));
            return result(actor ? std::max(0, actor->animationEnd - tick) : 0);
        }
        if(is("unit_stop_custom_animation"))
        {
            int id = object(0);
            if(at(actors, id)) { actors[id].animation = nullptr; actors[id].animationEnd = tick; }
            return {};
        }
        if(is("recording_play") || is("recording_play_and_delete") || is("recording_play_and_hover"))
        {
            int id = object(0);
            const Recording *record = at(program.recordings, index(1));
            if(!record || record->inputs.empty() || !at(actors, id)) return result(0);
            Actor &actor = actors[id];
            actor.recording = record;
            actor.recordingStart = tick;
            actor.recordingEnd = tick + record->ticks;
            actor.recordingDeletes = is("recording_play_and_delete");
            actor.recordingHover = is("recording_play_and_hover");
            actor.input = 0;
            if(halo_scripttrace && scene)
                conoutf("Halo recording: %s started on %s at %.2f %.2f %.2f", record->name.c_str(),
                        program.objects[id].name.c_str(), actor.pose.position.x, actor.pose.position.y, actor.pose.position.z);
            return result(1);
        }
        if(is("recording_time"))
        {
            const Actor *actor = at(actors, object(0));
            return result(actor ? std::max(0, actor->recordingEnd - tick) : 0);
        }
        if(is("recording_kill"))
        {
            int id = object(0);
            if(at(actors, id))
            {
                actors[id].recording = nullptr;
                actors[id].recordingEnd = 0;
                actors[id].recordingDeletes = false;
                actors[id].gait = -1;
            }
            return {};
        }
        if(is("device_get_position") || is("device_get_power") || is("device_set_position") ||
           is("device_set_position_immediate") || is("device_set_power") ||
           is("device_operates_automatically_set") || is("device_one_sided_set"))
        {
            const int *device = at(objectDevices, object(0));
            if(!device || *device < 0) return result(0);
            const Device &d = program.devices[*device];
            DeviceState &s = devices[*device];
            if(is("device_get_position")) return result(s.position);
            if(is("device_get_power")) return result(s.power);
            if(is("device_operates_automatically_set")) { s.automatic = truth(1); return {}; }
            if(is("device_one_sided_set")) { s.oneSided = truth(1); return {}; }
            float requested = float(value(1));
            if(!std::isfinite(requested)) return result(0);
            requested = clamp(requested, 0.0f, 1.0f);
            bool power = is("device_set_power"), immediate = is("device_set_position_immediate");
            int group = power ? d.powerGroup : d.positionGroup;
            if(group >= 0) return result(setGroup(group, requested, immediate));
            if(power) s.desiredPower = requested;
            else s.desiredPosition = requested;
            if(immediate) { s.position = requested; s.positionVelocity = 0; }
            return result(1);
        }
        if(is("device_group_set") || is("device_group_set_immediate"))
            return result(setGroup(index(0), float(value(1)), is("device_group_set_immediate")));
        if(is("device_group_get")) return result(groupValue(index(0)));
        if(is("device_group_change_only_once_more_set"))
        {
            int id = index(0);
            if(at(groups, id)) { groups[id].once = truth(1); groups[id].changed = false; }
            return {};
        }
        if(is("show_hud")) return {};
        if(is("show_hud_help_text")) { showHelp = truth(0); return {}; }
        if(is("hud_set_help_text") || is("hud_set_objective_text"))
        {
            const HudMessage *message = at(program.messages, index(0));
            if(!message) message = at(program.messages, named(program.messages, text(0)));
            if(message)
            {
                if(is("hud_set_help_text")) help = message->text;
                else objective = message->text;
            }
            return {};
        }
        // Nav points: (activate_team_nav_point_flag <type> <team> <flag> <offset>), the _object form
        // with an object, the per-unit forms (activate_nav_point_*) with a unit instead of a team;
        // the deactivate forms take the team or unit and the flag or object. One player: the team
        // and unit arguments are not checked.
        if(!strncmp(name, "activate_", 9) || !strncmp(name, "deactivate_", 11))
        {
            const bool on = name[0] == 'a';
            const char *rest = name + (on ? 9 : 11);
            if(!strncmp(rest, "team_", 5)) rest += 5;
            const bool flag = !strcmp(rest, "nav_point_flag"), item = !strcmp(rest, "nav_point_object");
            if(flag || item)
            {
                const size_t target = on ? 2 : 1;
                int id = -1;
                if(flag)
                {
                    // Flag literals carry both their name and their index; the name decides.
                    id = named(program.flags, text(target));
                    if(id < 0 && !*text(target)) id = index(target);
                    if(!at(program.flags, id)) return {};
                }
                else id = object(target);
                auto same = [&](const NavPoint &n) { return flag ? n.flag == id : n.object == id; };
                navPoints.erase(std::remove_if(navPoints.begin(), navPoints.end(), same), navPoints.end());
                if(on)
                {
                    NavPoint n;
                    (flag ? n.flag : n.object) = id;
                    n.type = text(0);
                    n.offset = float(value(3));
                    navPoints.push_back(n);
                }
                return {};
            }
        }
        if(!strncmp(name, "hud_show_", 9) || !strncmp(name, "hud_blink_", 10))
        {
            const bool blink = name[4] == 'b';
            const char *part = name + (blink ? 10 : 9);
            const int bit = !strcmp(part, "health") ? HALO_HUD_HEALTH : !strcmp(part, "shield") ? HALO_HUD_SHIELD :
                            !strcmp(part, "motion_sensor") ? HALO_HUD_MOTION : !strcmp(part, "crosshair") ? HALO_HUD_CROSSHAIR : 0;
            if(bit)
            {
                int &bits = blink ? hudBlinking : hudHidden;
                // hud_show_* true shows the part; hud_blink_* true starts it flashing.
                if(truth(0) != blink) bits &= ~bit;
                else bits |= bit;
                return {};
            }
        }
        if(is("print") || is("cls") || is("object_pvs_set_camera") || is("object_pvs_clear") ||
           is("object_beautify") || is("objects_predict") || is("object_type_predict")) return {};

        Value pacing;
        if(pacingCommand(name, args, scene, pacing)) return pacing;
        if(aiCommand(name, args, scene, pacing)) return pacing;

        if(*name) unsupported.insert(name);
        return result(0);
    }

    // Commands that pace the scripts: saves, the HUD countdown, tutorial prompts
    // and presentation calls with nothing to show. They return true when handled.
    bool Scenario::pacingCommand(const char *name, const std::vector<Value> &args, bool scene, Value &out)
    {
        auto is = [name](const char *candidate) { return !strcmp(name, candidate); };
        auto value = [&](size_t i, double fallback = 0) { return i < args.size() ? args[i].number : fallback; };
        auto truth = [&](size_t i) { return i < args.size() && args[i].truth(); };
        auto answer = [&](double v) { out = number(v, 8); return true; };
        auto handled = [&]() { out = {}; return true; };

        // Saves: safe while no hostile actor is fighting and the player lives.
        if(is("game_safe_to_save"))
        {
            if(player1->state != CS_ALIVE) return answer(0);
            for(const SquadBot &bot : squadBots)
                if(bot.bot && bot.bot->state == CS_ALIVE && !bot.suspended && !haloteamfriendly(haloactorteam(bot.bot)) &&
                   !haloactorpassive(bot.bot) && haloactorstatus(bot.bot) >= 6) return answer(0);
            return answer(1);
        }
        if(is("game_save_cancel")) { pendingCheckpoint = false; return handled(); }
        if(is("game_revert"))
        {
            if(!scene && player1->state == CS_ALIVE) dodamage(INT_MAX, player1, player1, GUN_GRENADE);
            return handled();
        }
        if(is("game_time")) return answer(tick);
        if(is("game_skip_ticks") || is("game_speed")) return handled();
        // Vehicles carry no actors yet, so no list of riders.
        if(is("vehicle_riders")) return answer(0);
        if(is("player_count")) return answer(1);

        // One HUD countdown. On the escape run it follows the engine's deadline, which is
        // what fails the run, so scripts waiting on it cannot fire early or late.
        if(is("hud_set_timer_time"))
        {
            hudTimer.remaining = max(0, int((value(0) * 60 + value(1)) * ticksPerSecond));
            if(hudTimer.running) hudTimer.end = tick + hudTimer.remaining;
            return handled();
        }
        if(is("show_hud_timer"))
        {
            hudTimer.shown = truth(0);
            if(hudTimer.shown && !hudTimer.running) { hudTimer.running = true; hudTimer.end = tick + hudTimer.remaining; }
            return handled();
        }
        if(is("pause_hud_timer"))
        {
            const bool pause = truth(0);
            if(pause && hudTimer.running) { hudTimer.remaining = max(0, hudTimer.end - tick); hudTimer.running = false; }
            else if(!pause && !hudTimer.running) { hudTimer.running = true; hudTimer.end = tick + hudTimer.remaining; }
            return handled();
        }
        if(is("hud_get_timer_ticks"))
        {
            const int escape = haloescapemillis();
            if(escape >= 0 && hudTimer.running) return answer(int(int64_t(escape) * ticksPerSecond / 1000));
            return answer(hudTimer.running ? max(0, hudTimer.end - tick) : hudTimer.remaining);
        }
        if(is("hud_set_timer_position") || is("hud_set_timer_warning_time") || is("time_code_show") || is("time_code_start") ||
           is("time_code_stop") || is("time_code_reset") || is("numeric_countdown_timer_set") || is("numeric_countdown_timer_stop"))
            return handled();

        // Tutorial prompts. Each wait ends on the matching input, or after a few
        // seconds so a player who ignores the prompt is not held forever.
        if(!strncmp(name, "player_action_test_", 19))
        {
            if(is("player_action_test_reset")) { tutorialPolls.clear(); return handled(); }
            auto found = tutorialPolls.find(name);
            if(found == tutorialPolls.end())
            {
                TutorialPoll poll;
                poll.first = tick;
                poll.yaw = player1->yaw;
                poll.pitch = player1->pitch;
                found = tutorialPolls.emplace(name, poll).first;
            }
            const TutorialPoll &poll = found->second;
            const float yaw = fabsf(player1->yaw - poll.yaw), pitch = fabsf(player1->pitch - poll.pitch);
            bool done = tick - poll.first >= 5 * ticksPerSecond;
            if(is("player_action_test_primary_trigger")) done |= player1->attacking;
            else if(is("player_action_test_back")) done |= player1->move < 0;
            else if(!strncmp(name, "player_action_test_look_relative_", 33)) done |= yaw > 10 || pitch > 5;
            return answer(done);
        }
        if(is("player0_joystick_set_is_normal")) return answer(1);
        if(is("player0_look_pitch_is_inverted")) return answer(0);
        if(is("player0_look_invert_pitch")) return handled();
        if(is("unit_get_total_grenade_count")) return answer(haloplayergrenades(-1));
        if(is("unit_get_shield")) return answer(haloplayershieldfraction());
        if(is("unit_solo_player_integrated_night_vision_is_active")) return answer(0);
        if(is("unit_has_weapon_readied"))
        {
            const HaloWeaponDef *ready = haloweaponforgun(player1->gunselect);
            return answer(ready && args.size() > 1 && ready->path == args[1].text);
        }

        // Presentation (halo_fx.cpp): the effects, camera shake, cinematic screen effects and sound
        // class gains. A scene's flags and objects are in the scene's frame, not the section's: there
        // an effect only plays its sound.
        auto index = [&](size_t i) { return i < args.size() ? args[i].integer() : -1; };
        auto text = [&](size_t i) { return i < args.size() ? args[i].text : ""; };
        auto object = [&](size_t i) { const int id = named(program.objects, text(i)); return id >= 0 ? id : index(i); };
        if(is("effect_new") || is("effect_new_on_object_marker"))
        {
            vec point(0, 0, 0);
            bool found = false;
            if(is("effect_new"))
            {
                const Flag *flag = at(program.flags, index(1));
                if(flag && projection && !scene) { point = toWorld(*projection, flag->pose.position); found = true; }
            }
            else if(!scene) found = objectMarkerPosition(object(1), text(2), point);
            if(found || scene) halofxeffect(text(0), point, found);
            // Its jpt! damage in play (halo_effectdamage, off by default), in the scripts' name.
            if(found && !scene && halo_effectdamage)
            {
                const bool dealt = haloeffectblast(text(0), point, nullptr);
                if(halo_scripttrace) conoutf("HaloScript: %s %s", text(0), dealt ? "deals its jpt! damage" : "has no jpt! damage");
            }
            return handled();
        }
        if(is("player_effect_set_max_translation")) { halofxshakelimit(0, float(value(0)), float(value(1)), float(value(2))); return handled(); }
        if(is("player_effect_set_max_rotation")) { halofxshakelimit(1, float(value(0)), float(value(1)), float(value(2))); return handled(); }
        if(is("player_effect_set_max_rumble")) { halofxshakelimit(2, float(value(0)), float(value(1)), 0); return handled(); }
        if(is("player_effect_start")) { halofxshakestart(float(value(0, 1)), float(value(1))); return handled(); }
        if(is("player_effect_stop")) { halofxshakestop(float(value(0))); return handled(); }
        if(is("player_effect_impact") || is("player_effect_rumble"))
        {
            halofxshakeimpulse(float(value(0, 1)), int(value(1, .5) * 1000));
            return handled();
        }
        if(is("cinematic_screen_effect_start")) { halofxscreenstart(args.empty() || truth(0)); return handled(); }
        if(is("cinematic_screen_effect_stop")) { halofxscreenstop(); return handled(); }
        if(is("cinematic_screen_effect_set_convolution"))
        {
            // iterations, mode, radius from, radius to, seconds
            halofxconvolution(index(0), float(value(2)), float(value(3)), float(value(4)));
            return handled();
        }
        if(is("cinematic_screen_effect_set_filter"))
        {
            // desaturation from, to, tint from, to, (additive), seconds
            halofxfilter(float(value(0)), float(value(1)), float(value(2)), float(value(3)), float(value(5)));
            return handled();
        }
        if(is("cinematic_screen_effect_set_filter_desaturation_tint")) { halofxfiltertint(float(value(0, 1)), float(value(1, 1)), float(value(2, 1))); return handled(); }
        if(is("cinematic_screen_effect_set_video")) { halofxvideo(index(0), float(value(1, 1))); return handled(); }
        if(is("sound_class_set_gain"))
        {
            halofxsetclassgain(text(0), float(value(1, 1)), float(value(2)) / ticksPerSecond);
            return handled();
        }

        // Nothing to show, change or free in this engine.
        static const char *const quiet[] = {
            "player_enable_input", "player_camera_control", "players_unzoom_all", "garbage_collect_now", "inspect", "render_lights",
            "display_scenario_help", "enable_hud_help_flash", "preload_textures", "object_pvs_activate", "object_pvs_set_object",
            // Presentation the engine does not have: breakable glass, scenery and list animations,
            // the death camera, scene BSP object suppression, the locked-door shader. `ai` (on or
            // off) is only used by scenes, during which the engine's actors are held anyway.
            "breakable_surfaces_enable", "breakable_surfaces_reset", "scenery_animation_start",
            "scenery_animation_start_at_frame", "custom_animation_list", "camera_set_dead",
            "cinematic_suppress_bsp_object_creation", "device_set_never_appears_locked", "ai"};
        for(const char *quietName : quiet) if(is(quietName)) return handled();
        if(!strncmp(name, "rasterizer_", 11)) return handled();
        return false;
    }

#include "halo_script_ai.inc"

    bool Scenario::matchesSquad(int reference, int encounter, int squad) const
    {
        if(reference == -1 || (reference & 0xffff) != encounter) return false;
        unsigned kind = uint32_t(reference) >> 30;
        int local = (uint32_t(reference) >> 16) & 0xff;
        if(kind == 0) return true;
        if(kind == 2) return local == squad;
        const Encounter *group = at(program.encounters, encounter);
        const Squad *member = group ? at(group->squads, squad) : nullptr;
        return kind == 1 && member && member->platoon == local;
    }

    // Project the encounters' combat positions and the command lists into this section.
    void Scenario::prepareAI()
    {
        encounterPositions.assign(program.encounters.size(), {});
        links.clear();
        migrationTargets.clear();
        commandPrograms.clear();
        actorLists.clear();
        maneuvered.clear();
        flipped.clear();
        if(!projection) return;
        for(size_t e = 0; e < program.encounters.size(); ++e)
            for(const FiringPosition &f : program.encounters[e].positions)
                encounterPositions[e].push_back(HaloFiringPosition{toWorld(*projection, f.point), f.group});
        for(const CommandList &list : program.commandLists)
        {
            HaloCommandProgram commands;
            commands.flags = list.flags;
            for(const CommandAtom &a : list.atoms)
                commands.commands.push_back(HaloCommand{a.type, a.first, a.second, a.point, a.otherPoint, a.script});
            for(const vec &point : list.points) commands.points.push_back(toWorld(*projection, point));
            commandPrograms.push_back(commands);
        }
    }

    void Scenario::configureActor(playerent *bot, int e, int s, const SquadStart &start)
    {
        const Encounter *encounter = at(program.encounters, e);
        const Squad *squad = encounter ? at(encounter->squads, s) : nullptr;
        if(!squad) return;
        if(e < int(encounterPositions.size())) haloactorsetpositions(bot, &encounterPositions[e], squad->masks);
        haloactorsetstate(bot, start.initial > 0 ? start.initial : squad->initial);
        const Platoon *platoon = at(encounter->platoons, squad->platoon);
        // Platoons flagged "initially defending" start their squads on defending positions.
        haloactorsetposture(bot, platoon && (platoon->flags & 4) ? HALO_DEFENDING : HALO_ATTACKING);
        if(start.commandList >= 0 && start.commandList < int(commandPrograms.size()))
            haloactorcommand(bot, &commandPrograms[start.commandList]);
    }

    // The encounter's team (retail numbering: 1 player, 2 human, 3 covenant, 4 flood, 5 sentinel,
    // 6-9 unused); on the default team (0) the actors take their unit's default team from
    // actors.txt (marines and crew human, the Flood flood, Sentinels and the Monitor sentinel).
    int teamFor(const Encounter &encounter, const SquadStart &start)
    {
        const int team = haloteamfromretail(encounter.team, HALO_TEAM_DEFAULT);
        if(team != HALO_TEAM_DEFAULT) return team;
        const HaloActorDef *def = haloactordef(start.variant);
        if(def) return haloactordefaultteam(def, HALO_TEAM_COVENANT);
        // A start without a variant: its body decides (halo.cpp kindTeam).
        return HALO_TEAM_DEFAULT;
    }

    size_t Scenario::livingBots() const
    {
        size_t count = 0;
        for(const SquadBot &bot : squadBots) if(bot.bot && bot.bot->state == CS_ALIVE) ++count;
        return count;
    }

    void Scenario::placeSquads(int reference, int limit, bool scaled)
    {
        int id = reference & 0xffff;
        const Encounter *encounter = at(program.encounters, id);
        if(!encounter || !projection || (encounter->bsp >= 0 && encounter->bsp != projection->bsp)) return;
        const Projection &p = *projection;
        int count = 0, selected = 0;
        for(size_t i = 0; i < encounter->squads.size(); ++i)
        {
            if(!matchesSquad(reference, id, int(i))) continue;
            // The squad's size on the chosen difficulty: its first starts that name an actor.
            // Spawning a single actor ignores it.
            const int allowed = scaled ? encounter->squads[i].counts[halodifficulty()] : -1;
            int rank = 0;
            for(const SquadStart &start : encounter->squads[i].starts)
            {
                // Only living actors count towards the cap; dead entries stay for the living fraction.
                if(livingBots() >= 128 || (limit > 0 && count >= limit)) break;
                if(start.actor < 0 && !haloactordef(start.variant)) continue;
                if(allowed >= 0 && rank++ >= allowed) break;
                ++selected;
                if(std::any_of(squadBots.begin(), squadBots.end(), [&](const SquadBot &bot) { return bot.start == &start; })) continue;
                vec point((start.point.x - p.origin.x) * p.xy + p.margin,
                          (p.origin.y - start.point.y) * p.xy + p.margin,
                          (start.point.z - p.origin.z) * p.z);
                if(!std::isfinite(point.x) || !std::isfinite(point.y) || !std::isfinite(point.z)) continue;
                if(playerent *bot = haloscenariospawnenemy(start.actor, point, 90 - start.yaw / RAD, start.variant, teamFor(*encounter, start)))
                {
                    squadBots.push_back({&start, bot, id, int(i)});
                    configureActor(bot, id, int(i), start);
                    ++count;
                }
            }
        }
        if(count) conoutf("HaloScript: placed %d actors from %s (%d starts on difficulty %d)", count, encounter->name.c_str(), selected, halodifficulty());
    }

    // Platoon rules switch squads between attacking and defending, and send
    // maneuver squads to their partner squad's positions, as casualties mount.
    void Scenario::updatePlatoons()
    {
        for(size_t e = 0; e < program.encounters.size(); ++e)
        {
            const Encounter &encounter = program.encounters[e];
            for(size_t pl = 0; pl < encounter.platoons.size(); ++pl)
            {
                auto strength = [&](int platoon, int &total, int &living)
                {
                    total = living = 0;
                    for(const SquadBot &bot : squadBots)
                    {
                        const Squad *squad = bot.encounter == int(e) ? at(encounter.squads, bot.squad) : nullptr;
                        if(!squad || squad->platoon != platoon) continue;
                        ++total;
                        if(bot.bot && bot.bot->state == CS_ALIVE) ++living;
                    }
                };
                auto satisfied = [&](int rule, int platoon)
                {
                    int total, living;
                    strength(platoon < 0 ? int(pl) : platoon, total, living);
                    if(!total || rule <= 0) return false;
                    const float alive = float(living) / total;
                    switch(rule)
                    {
                        case 1: return alive < 0.75f;
                        case 2: return alive < 0.5f;
                        case 3: return alive < 0.25f;
                        case 4: return living < total;
                        case 5: return alive <= 0.75f;
                        case 6: return alive <= 0.5f;
                        case 7: return alive <= 0.25f;
                        case 8: return living <= 1;
                        case 9: return living == 0;
                        default: return false;
                    }
                };
                const Platoon &platoon = encounter.platoons[pl];
                const int key = int(e) * 256 + int(pl);
                if(!flipped.count(key) && satisfied(platoon.attackRule, platoon.attackPlatoon))
                {
                    flipped.insert(key);
                    const int posture = platoon.flags & 4 ? HALO_ATTACKING : HALO_DEFENDING;
                    for(const SquadBot &bot : squadBots)
                    {
                        const Squad *squad = bot.encounter == int(e) ? at(encounter.squads, bot.squad) : nullptr;
                        if(squad && squad->platoon == int(pl) && bot.bot) haloactorsetposture(bot.bot, posture);
                    }
                }
                if(!maneuvered.count(key) && satisfied(platoon.maneuverRule, platoon.maneuverPlatoon))
                {
                    maneuvered.insert(key);
                    for(const SquadBot &bot : squadBots)
                    {
                        const Squad *squad = bot.encounter == int(e) ? at(encounter.squads, bot.squad) : nullptr;
                        const Squad *partner = squad ? at(encounter.squads, squad->maneuver) : nullptr;
                        if(squad && squad->platoon == int(pl) && partner && bot.bot)
                            haloactorsetpositions(bot.bot, &encounterPositions[e], partner->masks);
                    }
                }
            }
        }
    }

    int Scenario::actorListFor(int reference)
    {
        for(size_t i = 0; i < actorLists.size(); ++i) if(actorLists[i] == reference) return actorList - int(i);
        actorLists.push_back(reference);
        return actorList - int(actorLists.size() - 1);
    }

    template<class F> void Scenario::eachActor(int reference, F f)
    {
        for(size_t i = 0; i < squadBots.size(); ++i)
        {
            SquadBot &bot = squadBots[i];
            if(bot.bot && bot.bot->state == CS_ALIVE && matchesSquad(reference, bot.encounter, bot.squad)) f(bot, int(i));
        }
    }

    bool Scenario::objectPosition(int value, vec &position) const
    {
        if(playerent *p = actorFromValue(value)) { position = p->o; return true; }
        if(value < 0 || !projection || !at(actors, value) || !actors[value].visible) return false;
        position = toWorld(*projection, currentActor(value).position);
        return true;
    }

    // effect_new_on_object_marker's place: the named marker of the object's model (the .hscb's
    // markers, in the model's bind pose and the object's frame, as the seats are placed: an
    // animation does not move it), else the object's origin. The player and actors have no markers
    // here (their origin). Every retail call is in a cinematic (the chip insertion at chief's and
    // cortana's hands), where an effect only plays its sound; this places the gameplay ones.
    bool Scenario::objectMarkerPosition(int value, const char *marker, vec &position) const
    {
        const Object *object = at(program.objects, value);
        if(object && marker && *marker && projection && at(actors, value) && actors[value].visible && !actorFromValue(value))
        {
            int found = named(object->markers, marker);
            for(size_t i = 0; i < object->markers.size() && found < 0; ++i)
                if(!strcasecmp(object->markers[i].name.c_str(), marker)) found = int(i);
            if(const Flag *flag = at(object->markers, found))
            {
                Pose pose = currentActor(value);
                pose.position.add(poseOffset(vec(flag->pose.position).mul(actors[value].scale), pose));
                const vec point = toWorld(*projection, pose.position);
                if(std::isfinite(point.x) && std::isfinite(point.y) && std::isfinite(point.z))
                {
                    position = point;
                    if(halo_scripttrace) conoutf("Halo script: effect at %s's marker %s", object->name.c_str(), flag->name.c_str());
                    return true;
                }
            }
        }
        return objectPosition(value, position);
    }

    playerent *Scenario::actorFromValue(int value) const
    {
        if(value == playerObject) return player1;
        // A scripted object ai_attach made an actor: the unit commands, volume tests,
        // unit_get_health and object_teleport reach the actor.
        if(value >= 0)
        {
            const SquadBot *bot = at(squadBots, attachedEntry(value));
            return bot ? bot->bot : nullptr;
        }
        if(value > actorObject || value <= actorList) return nullptr;
        const SquadBot *bot = at(squadBots, actorObject - value);
        return bot ? bot->bot : nullptr;
    }

    // The squad entry standing for a scripted object (ai_attach), or -1.
    int Scenario::attachedEntry(int object) const
    {
        if(object < 0) return -1;
        for(size_t i = 0; i < squadBots.size(); ++i) if(squadBots[i].object == object) return int(i);
        return -1;
    }

    vec fromWorld(const Projection &p, const vec &point)
    {
        return vec(p.origin.x + (point.x - p.margin) / p.xy, p.origin.y - (point.y - p.margin) / p.xy, p.origin.z + point.z / p.z);
    }

    // ai_attach on a scripted biped in play: once nothing poses or carries it (no custom animation,
    // recording or parent) and two ticks have passed since the attach (a50's cell block poses its
    // marines and Keyes right after attaching them), an actor of the squad's default variant takes
    // over at the object's pose, on the encounter's team, as a member of the squad: a50's freed
    // captain_keyes and marines after ini_post_rescue, c10's marine_suicidal. The scripted model is
    // no longer drawn; the actor's death or erasure is the unit's (unit_get_health -1). Attaching in
    // a scene only records the squad, as before (the scene stage has no actors): a50's insertion
    // marines and final Hunters stay figures of their scenes.
    void Scenario::attachActors()
    {
        if(!projection || cue || player1->state != CS_ALIVE || intermission) return;
        if(attachedAt.size() != actors.size()) attachedAt.assign(actors.size(), -1);
        if(attachRetry.size() != actors.size()) attachRetry.assign(actors.size(), 0);
        if(objectStarts.size() != actors.size()) return;
        spawnCarried();
        for(size_t i = 0; i < actors.size(); ++i)
        {
            const Actor &a = actors[i];
            const Object &o = program.objects[i];
            if(attachedAt[i] < 0 || a.squadReference < 0 || !a.visible || o.kind != -1 || o.vitality > 0) continue;
            if(a.animation || a.recording || a.parent >= 0 || tick < attachedAt[i] + 2 || tick < attachRetry[i]) continue;
            if(attachedEntry(int(i)) >= 0) continue;      // has its actor (alive or dead)
            const int e = a.squadReference & 0xffff;
            const unsigned kind = uint32_t(a.squadReference) >> 30;
            const Encounter *encounter = at(program.encounters, e);
            if(!encounter || encounter->squads.empty()) { attachedAt[i] = -1; continue; }
            // The squad named, the first of the platoon named, or the encounter's first.
            int squad = 0;
            if(kind == 2) squad = (uint32_t(a.squadReference) >> 16) & 0xff;
            else if(kind == 1)
                for(size_t k = 0; k < encounter->squads.size(); ++k)
                    if(encounter->squads[k].platoon == int((uint32_t(a.squadReference) >> 16) & 0xff)) { squad = int(k); break; }
            const Squad *member = at(encounter->squads, squad);
            if(!member) { attachedAt[i] = -1; continue; }
            SquadStart &start = objectStarts[i];
            start = SquadStart{};
            start.actor = member->defaultKind;
            start.variant = member->defaultVariant;
            if(start.actor < 0 && !haloactordef(start.variant) && !member->starts.empty())
            {
                start.actor = member->starts.front().actor;
                start.variant = member->starts.front().variant;
            }
            if(start.actor < 0 && !haloactordef(start.variant))
            {
                conoutf("HaloScript: %s attached to %s has no actor variant to become (an .hscb older than version 9?)",
                        o.name.c_str(), encounter->name.c_str());
                attachedAt[i] = -1;
                continue;
            }
            const Pose pose = currentActor(int(i));
            start.point = pose.position;
            start.yaw = pose.yaw;
            playerent *bot = haloscenariospawnenemy(start.actor, toWorld(*projection, start.point), 90 - start.yaw / RAD, start.variant,
                                                    teamFor(*encounter, start));
            if(!bot)
            {
                attachRetry[i] = tick + ticksPerSecond;   // no floor there yet: try again in a second
                continue;
            }
            squadBots.push_back({&start, bot, e, squad});
            squadBots.back().object = int(i);
            squadBots.back().suspended = a.suspended;
            configureActor(bot, e, squad, start);
            // What the script told the object before its actor existed (gap 4): its immunity
            // (object_cannot_take_damage, kept on the object) and its vitality (queued).
            if(a.cannotTakeDamage) setUnitFlag(bot, HALO_AI_NO_DAMAGE, true);
            const auto queued = attachVitality.find(int(i));
            if(queued != attachVitality.end())
            {
                haloactorsethealthfraction(bot, queued->second.health);
                haloenemysetshield(bot, queued->second.shield);
                if(halo_scripttrace) conoutf("HaloScript: %s takes the vitality queued for it (%.2f, %.2f)", o.name.c_str(),
                                             queued->second.health, queued->second.shield);
                attachVitality.erase(queued);
            }
            conoutf("HaloScript: %s attached to %s/%s is an actor now", o.name.c_str(), encounter->name.c_str(), member->name.c_str());
        }
    }

    // The attached actors carried in from the section before (gap 4): each spawns behind the
    // player, one to three Halo units back and spread to the sides, on retail floor (the spawn
    // helper, haloscenariospawnenemy), with the health and shield it had, follows the player and
    // stands for its object again (unit_get_health, ai_living_count, the migrations a50's
    // mig_into_hangar gives it). Judgement call: Halo's actors walk through the doorway with
    // the player; the engine's next section is another map. A spot with no floor is tried again a
    // second later; the object's own destruction (object_destroy, ai_erase) drops it.
    void Scenario::spawnCarried()
    {
        if(carriedAttached.empty() || !projection) return;
        const vec feet = vec(player1->o).sub(vec(0, 0, player1->eyeheight));
        const vec ahead(sinf(player1->yaw * RAD), -cosf(player1->yaw * RAD), 0), side(-ahead.y, ahead.x, 0);
        int slot = 0;
        for(size_t k = 0; k < carriedAttached.size(); ++slot)
        {
            CarriedAttach &c = carriedAttached[k];
            const Encounter *encounter = at(program.encounters, c.encounter);
            if(!encounter || !at(encounter->squads, c.squad) || !at(objectStarts, c.object) || attachedEntry(c.object) >= 0)
            {
                carriedAttached.erase(carriedAttached.begin() + k);
                continue;
            }
            if(tick < c.retry) { ++k; continue; }
            const float back = (1.0f + .5f * (slot % 5)) * projection->xy, across = ((slot % 3) - 1) * .8f * projection->xy;
            const vec tries[] = {
                vec(feet).sub(vec(ahead).mul(back)).add(vec(side).mul(across)),
                vec(feet).sub(vec(ahead).mul(back)),
                vec(feet).add(vec(side).mul(1.2f * projection->xy)),
                vec(feet).sub(vec(side).mul(1.2f * projection->xy)),
                vec(feet).add(vec(ahead).mul(1.5f * projection->xy)),
            };
            playerent *bot = nullptr;
            vec spot = feet;
            // Only spots the player could walk to in a line: not past a wall, a closed door or a
            // ledge's lip between them (the spawn helper checks the floor alone; final review).
            const vec chest(0, 0, player1->eyeheight * .6f);
            for(const vec &point : tries)
            {
                HaloCollisionHit hit;
                if(halocollisionactive() && haloscenariotrace(vec(feet).add(chest), vec(point).add(chest), hit)) continue;
                if((bot = haloscenariospawnenemy(c.start.actor, point, player1->yaw, c.start.variant, teamFor(*encounter, c.start)))) { spot = point; break; }
            }
            if(!bot) { c.retry = tick + ticksPerSecond; ++k; continue; }
            SquadStart &start = objectStarts[c.object];
            start = c.start;
            start.point = fromWorld(*projection, vec(bot->o).sub(vec(0, 0, bot->eyeheight)));
            start.yaw = (90 - bot->yaw) * RAD;
            squadBots.push_back({&start, bot, c.encounter, c.squad});
            SquadBot &entry = squadBots.back();
            entry.object = c.object;
            entry.suspended = c.suspended;
            entry.scriptFlags = c.scriptFlags;
            configureActor(bot, c.encounter, c.squad, start);
            for(int bit = 1; bit <= HALO_AI_NO_MANEUVER; bit <<= 1) if(c.scriptFlags & bit) haloactorsetflag(bot, bit, true);
            haloactorsethealthfraction(bot, c.health);
            haloenemysetshield(bot, c.shield);
            haloactorfollow(bot, player1);
            if(at(attachedAt, c.object)) attachedAt[c.object] = 0;
            conoutf("HaloScript: %s came along with the player into section %d (%.1f units away, health %.2f)",
                    program.objects[c.object].name.c_str(), section + 1, spot.dist(feet), c.health);
            carriedAttached.erase(carriedAttached.begin() + k);
        }
    }

    // The living attached actors, as plain data, every live tick: what a walk on into the next
    // section brings along (Scenario::start), with those still waiting to spawn here.
    void Scenario::snapshotAttached()
    {
        carryCandidates.clear();
        for(const SquadBot &bot : squadBots)
        {
            if(bot.object < 0 || !bot.bot || bot.bot->state != CS_ALIVE || !bot.start) continue;
            CarriedAttach c;
            c.object = bot.object;
            c.encounter = bot.encounter;
            c.squad = bot.squad;
            c.start = *bot.start;
            c.health = haloactorhealthfraction(bot.bot);
            c.shield = haloenemyshieldfraction(bot.bot);
            c.suspended = bot.suspended;
            c.scriptFlags = bot.scriptFlags;
            carryCandidates.push_back(c);
        }
        for(CarriedAttach c : carriedAttached) { c.retry = 0; carryCandidates.push_back(c); }
    }

    // The scripted object is destroyed or created anew: its actor (ai_attach) goes with it, at
    // once and without a death (object_destroy deletes the unit in Halo).
    void Scenario::detachActor(int object)
    {
        if(at(attachedAt, object)) attachedAt[object] = -1;
        // Nor does it come along into the next section, or wait to (gap 4); nor take queued vitality.
        auto same = [object](const CarriedAttach &c) { return c.object == object; };
        carriedAttached.erase(std::remove_if(carriedAttached.begin(), carriedAttached.end(), same), carriedAttached.end());
        carryCandidates.erase(std::remove_if(carryCandidates.begin(), carryCandidates.end(), same), carryCandidates.end());
        attachVitality.erase(object);
        for(SquadBot &entry : squadBots)
        {
            if(entry.object != object) continue;
            playerent *bot = entry.bot;
            entry.object = -1;
            entry.bot = nullptr;
            if(bot) haloerasebot(bot);
        }
    }

    // A scene cuts the conversations being spoken: they count as finished (ai_conversation_status
    // 5), so the scripts waiting for them move on instead of waiting at status 2 for good.
    void Scenario::finishTalking()
    {
        for(const Talking &entry : talking) if(at(conversationStatus, entry.id)) conversationStatus[entry.id] = 5;
    }

    int Scenario::living(int reference) const
    {
        int count = 0;
        for(const SquadBot &bot : squadBots)
            if(bot.bot && bot.bot->state == CS_ALIVE && matchesSquad(reference, bot.encounter, bot.squad)) ++count;
        return count;
    }

    // ai_kill, ai_kill_silent and ai_erase: the squad's living actors die (the engine has no silent
    // erase of a fighting actor; they fall where they stand, as before). 343 Guilty Spark cannot
    // be killed in Halo: ai_kill of the Monitor does nothing, and ai_erase takes him away at once
    // with no death, body or sound (c20 erases bsp0_monitor once the player moves on).
    void Scenario::eraseSquads(int reference, bool erase)
    {
        HaloScriptDamage scripted;
        // Attached actors carried in that have not spawned yet (spawnCarried) die too: their object
        // keeps an entry with no actor, so unit_get_health answers -1 as for one killed here.
        for(size_t k = 0; k < carriedAttached.size();)
        {
            const CarriedAttach &c = carriedAttached[k];
            if(!matchesSquad(reference, c.encounter, c.squad) || !at(objectStarts, c.object)) { ++k; continue; }
            objectStarts[c.object] = c.start;
            squadBots.push_back({&objectStarts[c.object], nullptr, c.encounter, c.squad});
            squadBots.back().object = c.object;
            squadBots.back().gone = true;
            carriedAttached.erase(carriedAttached.begin() + k);
        }
        for(SquadBot &bot : squadBots)
        {
            if(!bot.bot || bot.bot->state != CS_ALIVE || !matchesSquad(reference, bot.encounter, bot.squad)) continue;
            if(monitorBody(bot.bot))
            {
                if(halo_scripttrace) conoutf("HaloScript: %s %s the Monitor", erase ? "ai_erase removes" : "ai_kill spares", bot.bot->name);
                if(erase) haloerasebot(bot.bot);
                continue;
            }
            dodamage(INT_MAX, bot.bot, player1, GUN_ASSAULT);
        }
    }

    void Scenario::restoreSquads()
    {
        squadBots.clear();
        if(!projection) return;
        const Projection &p = *projection;
        for(SquadBot saved : savedSquadBots)
        {
            // An attached object whose actor was dead at the checkpoint stays without one.
            if(saved.gone) { saved.bot = nullptr; squadBots.push_back(saved); continue; }
            const SquadStart &start = *saved.start;
            // An attached actor comes back where it stood at the checkpoint; the rest at their start.
            const vec stand = saved.stood ? saved.stoodPoint : start.point;
            const float yaw = saved.stood ? saved.stoodYaw : start.yaw;
            vec point((stand.x - p.origin.x) * p.xy + p.margin,
                      (p.origin.y - stand.y) * p.xy + p.margin,
                      (stand.z - p.origin.z) * p.z);
            const Encounter *encounter = at(program.encounters, saved.encounter);
            const int team = encounter ? teamFor(*encounter, start) : HALO_TEAM_DEFAULT;
            saved.bot = haloscenariospawnenemy(start.actor, point, 90 - yaw / RAD, start.variant, team);
            if(saved.bot)
            {
                squadBots.push_back(saved);
                configureActor(saved.bot, saved.encounter, saved.squad, start);
                for(int bit = 1; bit <= HALO_AI_NO_MANEUVER; bit <<= 1)
                    if(saved.scriptFlags & bit) haloactorsetflag(saved.bot, bit, true);
                // An attached actor (a50's Keyes) comes back as hurt as it was at the checkpoint.
                if(saved.object >= 0)
                {
                    if(saved.health < 1) haloactorsethealthfraction(saved.bot, saved.health);
                    if(saved.shield < 1) haloenemysetshield(saved.bot, saved.shield);
                }
            }
        }
    }

    void Scenario::stop()
    {
        stopSounds();
        clearCargo();
        vehicleSquads.clear();
        loaded = false;
        cue = nullptr;
        callActors.clear();
        sceneObjects.clear();
        sceneWoken.clear();
        sceneSeat = noObject;
        sceneSeatName.clear();
        cueSkippable = false;
        handoffTeleport = handoffFacing = -1;
        projection = nullptr;
        worldName.clear();
        paused = false;
    }

    bool Scenario::cartographerInterior() const
    {
        return mission == 3 && section == 1 && halocollisionactive();
    }

    bool Scenario::cartographerMapped() const
    {
        const Value *mapped = at(globals, named(program.globals, "global_shafta_switched"));
        return mapped && mapped->truth();
    }

    void Scenario::startCartographer()
    {
        if(!cartographerInterior()) return;
        // The adapted approach enters the descent after the security detour.
        const int *door = at(objectDevices, named(program.objects, "shafta_lock_door"));
        if(door && *door >= 0) setGroup(program.devices[*door].positionGroup, 1, true);
        const int lock = named(program.scripts, "mission_shafta_lock");
        if(lock >= 0) threads[lock].disabled = true;
        wake(named(program.scripts, "obj_shafta_goal"));
        wake(named(program.scripts, "save_shafta_beam_enter"));
        wake(named(program.scripts, "save_shafta_mind_enter"));
        wake(named(program.scripts, "save_shafta_ante_enter"));
    }

    // Landing craft the engine brings in itself, where the script that would is part of a scene
    // the route skips: b30's extraction Pelican on retail collision, which the player boards with
    // E (halo.cpp). Each starts at its flag on its recording and hovers where that ends; the
    // script that would wait for the player instead is held. Elsewhere the scripts fly their
    // dropships and Pelicans themselves (recording_play_and_hover), and those carry actors and
    // the player through the Cargo code (halo_script_ai.inc).
    struct EngineApproach { int mission, section; const char *object, *flag, *recording, *held; };
    const EngineApproach approaches[] = {
        {3, 2, "extraction_pelican", "extraction_pelican_flag_1", "extraction_pelican_1", "obj_shafta_goal"},
    };

    void Scenario::startPelicanApproach()
    {
        if(!halocollisionactive()) return;
        for(const EngineApproach &a : approaches)
        {
            if(a.mission != mission || a.section != section) continue;
            const int id = named(program.objects, a.object);
            const Flag *entry = at(program.flags, named(program.flags, a.flag));
            const Recording *approach = at(program.recordings, named(program.recordings, a.recording));
            if(!at(actors, id) || !entry || !approach || approach->inputs.empty()) continue;
            setActor(id, true, true);
            Actor &craft = actors[id];
            craft.pose = entry->pose;
            craft.recording = approach;
            craft.recordingStart = tick;
            craft.recordingEnd = tick + approach->ticks;
            craft.recordingHover = true;
            // The outdoor route boards with the normal interact action.
            const int goal = named(program.scripts, a.held);
            if(goal >= 0) threads[goal].disabled = true;
        }
    }

    bool Scenario::pelicanReady() const
    {
        if(!loaded || cue || mission != 3 || section != 2 || !halocollisionactive()) return false;
        const Actor *pelican = at(actors, named(program.objects, "extraction_pelican"));
        return pelican && pelican->visible && !pelican->recording;
    }

    void Scenario::start(int m, int s)
    {
        const bool continuing = mission == m && s == section + 1 && runningGameplay && !threads.empty();
        ridePending = continuing && rideCarried;
        rideCarried = false;
        ridden.clear();
        riddenUntil = -1;
        stop();
        // A mission (or a section entered afresh) starts from the default allegiances, even when
        // its script is missing; a carried-in section keeps what the scripts set.
        if(!continuing) haloallegiancereset();
        cache = haloMissions[m].cache;
        if(mission != m || program.scripts.empty())
        {
            program = {};
            programHash = 0;
            if(!program.load(cache))
            {
                program = {};
                conoutf("Halo: no retail scenario program for %s (packages/halo/%s.hscb is missing or unreadable; "
                        "run tools/import_halo_scripts.py): the section plays without its mission script", cache, cache);
                return;
            }
            defformatstring(file)("packages/halo/%s.hscb", cache);
            programHash = halofilehash(file);
        }
        mission = m;
        section = s;
        projection = at(program.projections, s);
        switchArm.clear();
        if(!projection) return;
        prepareAI();
        if(continuing)
        {
            world = projection->bsp;
            exitPlayed = completeExit = failedEscape = pendingCheckpoint = gameWon = gameLost = false;
            hudTimer = HudTimer();
            tutorialPolls.clear();
            savedThreads.clear();
            savedActors.clear();
            savedSquadBots.clear();
            // Actors are not carried into the next section (its map is another), but for those
            // ai_attach made of scripted objects (gap 4): the living ones come along behind the
            // player (snapshotAttached, spawnCarried); the others' objects stay as the scripts
            // left them, with no actor again. The squad entries' actors are gone already.
            carriedAttached = std::move(carryCandidates);
            carryCandidates.clear();
            for(const CarriedAttach &c : carriedAttached)
                conoutf("HaloScript: %s (%s) comes along into section %d, health %.2f", program.objects[c.object].name.c_str(),
                        program.encounters[c.encounter].name.c_str(), s + 1, c.health);
            squadBots.clear();
            std::fill(attachedAt.begin(), attachedAt.end(), -1);
            std::fill(savedAttachedAt.begin(), savedAttachedAt.end(), -1);
            std::fill(attachRetry.begin(), attachRetry.end(), 0);
            attachVitality.clear();
            savedAttachVitality.clear();
            savedCarried.clear();
            savedBoarded = {};
            std::fill(conversationStatus.begin(), conversationStatus.end(), 5);
            loaded = true;
            startCartographer();
            startPelicanApproach();
            conoutf("HaloScript: %s resumed on section %d (BSP %d)", cache, s + 1, world);
            beginCue(SceneTrigger::Entry);
            return;
        }
        tick = accumulator = 0;
        world = projection->bsp;
        camera = {};
        fade = {};
        chapter = -1;
        exitPlayed = completeExit = failedEscape = runningGameplay = pendingCheckpoint = gameWon = gameLost = false;
        hudTimer = HudTimer();
        tutorialPolls.clear();
        director = {};
        threads.assign(program.scripts.size(), {});
        globals.assign(program.globals.size(), {});
        actors.assign(program.objects.size(), {});
        // Damage targets placed in the scenario exist from the start, as in Halo; without this they
        // read as destroyed (unit_get_health -1), which ended d40's manifold fight before it began.
        for(size_t i = 0; i < actors.size(); ++i)
            if(program.objects[i].vitality > 0 && program.objects[i].targetPlaced)
            {
                actors[i].pose = program.objects[i].pose;
                actors[i].visible = true;
            }
        conversationStatus.assign(program.conversations.size(), 0);
        savedGlobals.clear();
        savedThreads.clear();
        savedActors.clear();
        savedSquadBots.clear();
        squadBots.clear();
        objectStarts.assign(program.objects.size(), SquadStart{});
        attachedAt.assign(program.objects.size(), -1);
        savedAttachedAt.assign(program.objects.size(), -1);
        attachRetry.assign(program.objects.size(), 0);
        carriedAttached.clear();
        savedCarried.clear();
        carryCandidates.clear();
        attachVitality.clear();
        savedAttachVitality.clear();
        callMusic.clear();
        savedBoarded = {};
        savedGroups.clear();
        external.clear();
        groups.clear();
        for(size_t i = 0; i < program.deviceGroups.size(); ++i)
            groups.push_back({program.deviceGroups[i], (program.groupFlags[i] & 1) != 0, false});
        devices.assign(program.devices.size(), {});
        objectDevices.assign(program.objects.size(), -1);
        for(size_t i = 0; i < devices.size(); ++i)
        {
            const Device &d = program.devices[i];
            DeviceState &state = devices[i];
            if(at(objectDevices, d.object)) objectDevices[d.object] = int(i);
            state.visible = !(d.notPlaced & 1);
            state.automatic = !(d.attributes & 1);
            state.oneSided = (d.attributes & 2) != 0;
            state.power = state.desiredPower = groupValue(d.powerGroup, d.flags & 2 ? 0 : 1);
            state.position = state.desiredPosition = groupValue(d.positionGroup, d.flags & 1 ? 1 : 0);
        }
        unsupported.clear();
        haloaisetgrenades(true);
        // A new mission starts from the default allegiances; its startup script sets its own.
        haloallegiancereset();
        objective.clear();
        help.clear();
        showHelp = false;
        navPoints.clear();
        savedNavPoints.clear();
        hudHidden = hudBlinking = savedHudHidden = savedHudBlinking = 0;
        loaded = true;
        for(size_t i = 0; i < program.globals.size(); ++i)
        {
            Thread initializer;
            initializer.stack.emplace_back(program.globals[i].root);
            step(initializer, false);
            globals[i] = initializer.result;
        }
        conoutf("HaloScript: %s loaded %zu scripts, %zu camera points, %zu sound tags",
                cache, program.scripts.size(), program.cameras.size(), program.sounds.size());
        fastForwardFresh();
        if(!beginCue(SceneTrigger::Entry)) startGameplay();
    }

    void Scenario::startGameplay()
    {
        if(runningGameplay) return;
        runningGameplay = true;
        for(size_t i = 0; i < program.scripts.size(); ++i)
        {
            const Script &script = program.scripts[i];
            if(script.kind != 0 && script.kind != 2) continue;
            bool handled = false;
            for(const SceneCue &candidate : cues)
                if(candidate.mission == mission && script.name == candidate.script) handled = true;
            if(!handled) wake(int(i));
        }
        startCartographer();
        startPelicanApproach();
    }

    // A section started afresh (Mission select, a QA start, a revert with no checkpoint: no script
    // carried in) runs its mission's startup script from the top, which waits for what play in
    // the earlier sections sets. Where a section's way on is opened only by a script that waits for
    // that play, the script is brought forward to where a walk from the mission's start would have
    // it: these globals set, these threads passed over (as finished) and these woken. The
    // counterpart of halo.cpp's FreshProfile (the loadout). Judgement call (gap 6), read
    // statically from the decompiled a10 scripts, not checked in Halo; sections from..to, zero-based.
    struct FreshScript { int mission, from, to; const char *globals, *done, *wake; };
    const FreshScript freshScripts[] = {
        // a10 sections 5 and 6. mission_a10 wakes mission_tunnel (which powers tunnel_door_2 in
        // section 5) and cryo_search (which opens cryo_door_1 and _2 in section 6) only once
        // mission_bridge has set mark_bridge_cutscene after cinematic_x20, at the end of section 2;
        // before that it waits for the cryo exit (mark_fast_setup or mark_tutorial_setup). Started
        // here the bridge and its flags never come. The cryo exit is section 1's: fast_setup and
        // tutorial_setup would seat the player in cryotube_1, on section 1's map (on Normal for the
        // minutes the tutorial takes), so both are passed over and mark_fast_setup set. The bridge
        // flags (mark_bridge_cutscene_start and mark_bridge_cutscene, set by mission_bridge;
        // mark_weapon and mark_shoot, by mission_shoot in section 3) let mission_a10 go on to its
        // later wakes (a no-op for the two woken here). mission_bsp, woken as usual, closes the
        // earlier BSPs' doors itself. a10 has no player_location_index.
        {0, 4, 5, "mark_fast_setup mark_bridge_cutscene_start mark_bridge_cutscene mark_weapon mark_shoot",
         "fast_setup tutorial_setup", "mission_tunnel cryo_search"},
    };

    void Scenario::fastForwardFresh()
    {
        for(const FreshScript &f : freshScripts)
        {
            if(f.mission != mission || section < f.from || section > f.to) continue;
            auto each = [](const char *list, auto fn)
            {
                for(const char *word = list; *word;)
                {
                    while(*word == ' ') ++word;
                    const char *end = word;
                    while(*end && *end != ' ') ++end;
                    if(end > word) fn(std::string(word, end));
                    word = end;
                }
            };
            int set = 0, passed = 0, woken = 0;
            each(f.globals, [&](const std::string &name)
            {
                const int g = named(program.globals, name.c_str());
                if(at(globals, g)) { globals[g] = number(1, 5); ++set; }
                else conoutf("HaloScript: the fresh start's global %s is not in %s", name.c_str(), cache);
            });
            each(f.done, [&](const std::string &name)
            {
                const int s = named(program.scripts, name.c_str());
                if(!at(threads, s)) return;
                threads[s] = {};
                threads[s].started = threads[s].done = true;
                ++passed;
            });
            each(f.wake, [&](const std::string &name)
            {
                const int s = named(program.scripts, name.c_str());
                if(s < 0) return;
                wake(s);
                ++woken;
            });
            conoutf("HaloScript: %s section %d started afresh: the script brought forward (%d globals set, %d threads passed over, %d woken)",
                    cache, section + 1, set, passed, woken);
        }
    }

    // Whether the expression at `datum` calls script `target` (itself or inside its arguments).
    bool Scenario::callsScript(uint32_t datum, int target, int depth) const
    {
        const Node *node = program.node(datum);
        if(!node || (node->flags & 1) || depth > 8) return false;
        if((node->flags & 2) && node->opcode == target) return true;
        const Node *head = program.node(node->data);
        for(uint32_t it = head ? head->next : nil; const Node *arg = program.node(it); it = arg->next)
            if(callsScript(it, target, depth + 1)) return true;
        return false;
    }

    // Whether some script plays this scene inside (if (cinematic_skip_start) ...), the retail way of
    // making a cutscene skippable (a10's x10, x20 and x30, d40's bridge scene, the cinematic_intro
    // scenes); b30's and b40's insertions, d20's drop and d40's finale are not.
    bool Scenario::skippableScene(const char *scene) const
    {
        const int skip = named(program.scripts, "cinematic_skip_start"), target = named(program.scripts, scene);
        if(skip < 0 || target < 0) return false;
        for(const Node &node : program.nodes)
        {
            if((node.flags & 3) || node.opcode != 2) continue;   // a call of if
            const Node *head = program.node(node.data);
            const Node *condition = head ? program.node(head->next) : nullptr;
            if(!condition || !(condition->flags & 2) || condition->opcode != skip) continue;
            if(callsScript(condition->next, target, 0)) return true;   // its then-branch
        }
        return false;
    }

    // Whether a script is one the cue table plays (and so is not run inline by a gameplay thread).
    bool Scenario::cueScript(const std::string &script) const
    {
        for(const SceneCue &candidate : cues)
            if(candidate.mission == mission && script == candidate.script) return true;
        return false;
    }

    // The Call row that plays this script in this section, if any.
    const SceneCue *Scenario::callCue(const std::string &script) const
    {
        for(const SceneCue &candidate : cues)
            if(candidate.mission == mission && candidate.trigger == SceneTrigger::Call &&
               (candidate.section < 0 || candidate.section == section) && script == candidate.script) return &candidate;
        return nullptr;
    }

    // The last object_teleport and object_set_facing of player0 in a scene's script, in the
    // order the script runs them, following its calls into other static scripts.
    void Scenario::findHandoff(uint32_t datum, int depth)
    {
        const Node *node = program.node(datum);
        if(!node || (node->flags & 1) || depth > 8) return;
        const Node *head = program.node(node->data);
        if(!head) return;
        if(node->flags & 2)
        {
            const Script *script = at(program.scripts, node->opcode);
            if(script && script->kind == 3 && script->name != "player0" && script->name != "player1") findHandoff(script->root, depth + 1);
            return;
        }
        const char *name = program.text(head->text);
        const bool teleport = !strcmp(name, "object_teleport"), facing = !strcmp(name, "object_set_facing");
        if(teleport || facing)
        {
            const Node *unit = program.node(head->next);
            const Node *flag = unit ? program.node(unit->next) : nullptr;
            const Node *unitHead = unit && !(unit->flags & 1) ? program.node(unit->data) : nullptr;
            if(unitHead && flag && (flag->flags & 1) && !strcmp(program.text(unitHead->text), "player0"))
            {
                int index = named(program.flags, program.text(flag->text));
                if(index < 0) index = literal(*flag).integer();
                if(at(program.flags, index))
                {
                    if(teleport) { handoffTeleport = index; handoffFacing = -1; }
                    else handoffFacing = index;
                }
            }
            return;
        }
        for(uint32_t it = head->next; const Node *arg = program.node(it); it = arg->next) findHandoff(it, depth);
    }

    // Puts the player at a cutscene flag (or turns them to its facing), when the flag is on a
    // floor of this section's map (halo.cpp checks the body fits there). False, and the player
    // left where they are, otherwise: flags of other BSPs fall outside the section's map.
    bool Scenario::placePlayer(int teleport, int facing, const char *why)
    {
        const Flag *to = at(program.flags, teleport), *face = at(program.flags, facing >= 0 ? facing : teleport);
        if(!face) face = to;
        if(!projection || !face) return false;
        const float yaw = fmodf(450.0f - face->pose.yaw / RAD, 360.0f);
        if(!to)
        {
            if(!haloplayerplace(nullptr, yaw, why)) return false;
            if(halo_scripttrace) conoutf("Halo script: player turned to %s (%s)", face->name.c_str(), why);
            return true;
        }
        const vec feet = toWorld(*projection, to->pose.position);
        if(!haloplayerplace(&feet, yaw, why))
        {
            conoutf("Halo script: %s (%s) has no floor in this section's map (or the player is seated or dead); the player stays where they are",
                    to->name.c_str(), why);
            return false;
        }
        conoutf("Halo script: player placed at %s (%s)", to->name.c_str(), why);
        return true;
    }

    bool Scenario::beginCue(SceneTrigger trigger, const char *script)
    {
        if(!loaded || cue || (trigger == SceneTrigger::Exit && exitPlayed)) return false;
        if(trigger == SceneTrigger::Exit && cartographerInterior()) return false;
        // The rows that could play here; several Exit rows are variants of one exit.
        std::vector<const SceneCue *> matches;
        for(const SceneCue &candidate : cues)
        {
            if(candidate.mission != mission || (candidate.section >= 0 && candidate.section != section) || candidate.trigger != trigger) continue;
            if(script && strcmp(script, candidate.script)) continue;
            if(named(program.scripts, candidate.script) < 0) continue;
            matches.push_back(&candidate);
        }
        if(matches.empty()) return false;
        const SceneCue *chosen = matches.front();
        if(matches.size() > 1)
        {
            // The variant the player boarded, else the one whose site they are in, else the nearest site.
            vec point;
            const bool placed = playerPoint(point);
            float best = 1e30f;
            const SceneCue *inside = nullptr, *boarded = nullptr, *nearest = nullptr;
            for(const SceneCue *candidate : matches)
            {
                if(candidate->vehicle && boardedVehicle != noObject && boardedVehicle == named(program.objects, candidate->vehicle)) boarded = candidate;
                if(candidate->volume && !inside && testVolume(named(program.volumes, candidate->volume))) inside = candidate;
                const Flag *site = candidate->site ? at(program.flags, named(program.flags, candidate->site)) : nullptr;
                if(site && placed && site->pose.position.dist(point) < best) { best = site->pose.position.dist(point); nearest = candidate; }
            }
            chosen = boarded ? boarded : inside ? inside : nearest ? nearest : chosen;
            conoutf("Halo scene: %s chosen for the exit (%s)", chosen->script,
                    boarded ? "the Pelican boarded" : inside ? "the player's site" : nearest ? "the site nearest the player" : "the first");
        }
        const int index = named(program.scripts, chosen->script);
        const int boardedPelican = pelicanReady() ? named(program.objects, "extraction_pelican") : -1;
        // A scene a script calls mid-section cuts the gameplay music for its own; the loops come
        // back when it ends (endCue). The conversations it cuts count as finished.
        callMusic.clear();
        if(trigger == SceneTrigger::Call)
            for(const Music &item : music)
            {
                if(item.ending || std::any_of(callMusic.begin(), callMusic.end(), [&](const CallMusic &m) { return m.id == item.id; })) continue;
                callMusic.push_back({item.id, item.loopGain, item.alternate});
            }
        finishTalking();
        stopSounds();
        cue = chosen;
        director = {};
        sceneThreads.clear();
        sceneWoken.clear();
        sceneSeat = noObject;
        sceneSeatName.clear();
        cueSkippable = skippableScene(chosen->script);
        director.stack.emplace_back(program.scripts[index].root);
        chapter = -1;
        nearClip = .0625f;
        fade = {vec(), 1, 1, tick, tick};
        letterbox = cameraControl = true;
        camera = {};
        // A scene in the middle of a section borrows the scripted objects and gives them back.
        sceneObjects.clear();
        if(trigger == SceneTrigger::Call) callActors = actors;
        else callActors.clear();
        std::vector<std::pair<int, int>> standing;   // attached objects with a living actor, and their squad
        for(const SquadBot &bot : squadBots)
            if(bot.object >= 0 && bot.bot && bot.bot->state == CS_ALIVE && at(actors, bot.object))
                standing.emplace_back(bot.object, actors[bot.object].squadReference);
        for(size_t i = 0; i < actors.size(); ++i)
        {
            if(int(i) == boardedPelican) continue;
            actors[i] = {};
            actors[i].pose = program.objects[i].pose;
        }
        // A scene in the middle of a section, or the one that ends it, finds the actors ai_attach
        // made of scripted objects where they stand, not at their placement or their pose when
        // attached (gap 4): Halo's units are the actors. The squad stays on the object.
        if(projection && (trigger == SceneTrigger::Call || trigger == SceneTrigger::Exit))
            for(const auto &entry : standing)
            {
                const SquadBot *bot = at(squadBots, attachedEntry(entry.first));
                if(!bot || !bot->bot || entry.first == boardedPelican) continue;
                Actor &unit = actors[entry.first];
                unit.visible = true;
                unit.squadReference = entry.second;
                unit.pose.position = fromWorld(*projection, vec(bot->bot->o).sub(vec(0, 0, bot->bot->eyeheight)));
                unit.pose.yaw = (90 - bot->bot->yaw) * RAD;
            }
        handoffTeleport = handoffFacing = -1;
        findHandoff(program.scripts[index].root, 0);
        setWorld(chosen->bsp);
        player1->attacking = false;
        player1->stopmoving();
        conoutf("Halo scene: %s start", chosen->script);
        return true;
    }

    void Scenario::endCue(bool skipped)
    {
        if(!cue) return;
        conoutf("Halo scene: %s %s at tick %d", cue->script, skipped ? "skipped" : "finished", tick);
        if(skipped && cue->trigger == SceneTrigger::Call && !director.done && !director.disabled)
        {
            // A scene called mid-section keeps what it does for the play after it (b30's
            // cutscene_shaftb_goal opens shafta_lock_door, which it first closed): the rest of
            // its script runs now, with no waits. A bounded number of passes, so a loop cannot hold it.
            skipping = true;
            for(int pass = 0; pass < 1024 && !director.done && !director.disabled && !director.stack.empty(); ++pass)
            {
                director.wake = tick;
                step(director, true);
            }
            skipping = false;
            if(!director.done && !director.disabled) conoutf("Halo scene: %s did not run to its end when skipped", cue->script);
        }
        // The gameplay scripts the scene woke that are still running go on in play, from where they
        // are (gap 3): in Halo a script woken in a cutscene is an ordinary thread. One the scene put
        // to sleep for good (sleep -1), one finished, and one the gameplay threads already run stay
        // as they are; a skipped Call scene's wakes from the rest of its script start from their top.
        // A skipped scene the retail scripts make skippable (cinematic_skip_start) leaves none of
        // them, nor its seat: Halo's skip reverts the game to before the scene, and the script after
        // the call sets up the play that follows (judgement call: the engine's skip of a scene Halo
        // cannot skip, such as b30's and b40's insertions, keeps what the scene had started).
        const bool dropWoken = skipped && cueSkippable;
        if(dropWoken && !sceneWoken.empty())
            conoutf("Halo scene: %s skipped: the %d scripts it woke stop with it (cinematic_skip_start)", cue->script, int(sceneWoken.size()));
        for(int index : sceneWoken)
        {
            if(dropWoken) break;
            auto found = sceneThreads.find(index);
            if(found == sceneThreads.end() || !at(threads, index)) continue;
            Thread &woken = found->second;
            Thread &gameplay = threads[index];
            if(woken.done || woken.disabled || woken.stack.empty() || gameplay.started || gameplay.done) continue;
            gameplay = std::move(woken);
            gameplay.wake = max(gameplay.wake, tick);
            conoutf("Halo scene: %s, woken by %s, goes on in play", program.scripts[index].name.c_str(), cue->script);
        }
        sceneWoken.clear();
        const int seatVehicle = dropWoken ? noObject : sceneSeat;
        const std::string seatName = sceneSeatName;
        sceneSeat = noObject;
        sceneSeatName.clear();
        const SceneTrigger trigger = cue->trigger;
        const char *const played = cue->script;
        cue = nullptr;
        worldName.clear();
        world = projection->bsp;
        director = {};
        sceneThreads.clear();
        camera = {};
        fade = {};
        letterbox = cameraControl = false;
        finishTalking();
        stopSounds();
        player1->attacking = false;
        player1->stopmoving();
        resetcamera();
        if(trigger == SceneTrigger::Call)
        {
            // The gameplay music the scene cut, unless the scene stopped it (sound_looping_stop).
            for(const CallMusic &item : callMusic)
            {
                startMusic(item.id, item.gain);
                if(item.alternate) setAlternate(item.id, true);
                if(halo_scripttrace) conoutf("Halo audio: loop %d restarted after %s", item.id, played);
            }
            callMusic.clear();
            // The scripted objects as gameplay left them, but for those the scene created or
            // destroyed. The calling thread carries on after the call on its next step.
            if(callActors.size() == actors.size())
            {
                std::vector<Actor> after = std::move(actors);
                actors = std::move(callActors);
                for(const auto &changed : sceneObjects) if(at(after, changed.first)) actors[changed.first] = after[changed.first];
            }
            callActors.clear();
            sceneObjects.clear();
            placePlayer(handoffTeleport, handoffFacing, played);
        }
        else if(trigger == SceneTrigger::Cartographer)
        {
            // The scene's original handoff places the player next to the terminal (player0_map_done).
            if(handoffTeleport < 0) handoffTeleport = named(program.flags, "player0_map_done");
            placePlayer(handoffTeleport, handoffFacing, played);
            pendingCheckpoint = true;
        }
        else if(trigger == SceneTrigger::Exit) exitPlayed = completeExit = true;
        else if(trigger == SceneTrigger::Timeout) failedEscape = true;
        else
        {
            // The retail insertion sets this after the landing. Skipping
            // the scene still has to release the beach encounter scripts.
            if(mission == 3 && section == 0 && skipped)
            {
                const int landed = named(program.globals, "global_mission_start");
                if(landed >= 0) globals[landed] = number(1, 5);
            }
            startGameplay();
        }
        handoffTeleport = handoffFacing = -1;
        // A scene can leave the player somewhere else (the handoffs above): the fall and wedge
        // recovery forgets the floors from before it.
        haloplayermoved("after a cutscene");
        // A seat the scene's threads gave the player and kept (b40's pelican_up seats the player in
        // the insertion Pelican during cutscene_insertion_a; cutscene_insertion_b, gameplay, waits
        // for the Pelican's recording and lets them out): taken now, in play, while the vehicle
        // still exists. Not at an exit or a time-out scene, which end the section.
        if(seatVehicle != noObject && trigger != SceneTrigger::Exit && trigger != SceneTrigger::Timeout)
        {
            const Actor *vehicle = at(actors, seatVehicle);
            if(vehicle && vehicle->visible && scriptVehicle(seatVehicle) && seatPlayer(seatVehicle, seatName.c_str()))
                conoutf("Halo scene: %s leaves the player seated in %s (%s)", played, program.objects[seatVehicle].name.c_str(),
                        seatName.empty() ? "-" : seatName.c_str());
            else if(at(program.objects, seatVehicle))
                conoutf("Halo scene: %s left the player a seat in %s, which is gone or cannot take them", played,
                        program.objects[seatVehicle].name.c_str());
        }
    }

    void Scenario::update(int elapsed)
    {
        if(!loaded || elapsed <= 0 || paused) return;
        if(!cue) updateCargo();
        accumulator += clamp(elapsed, 0, 250) * ticksPerSecond;
        while(accumulator >= 1000)
        {
            accumulator -= 1000;
            ++tick;
            updateRecordings();
            updateDevices(!cue && player1->state == CS_ALIVE && !intermission);
            if(!cue && tick % 15 == 0) { updatePlatoons(); updateLinks(); }
            updateSound();
            if(cue)
            {
                updateCamera();
                step(director, true);
                for(size_t i = 0; i < program.scripts.size(); ++i)
                {
                    auto found = sceneThreads.find(int(i));
                    if(found == sceneThreads.end()) continue;
                    Thread &thread = found->second;
                    if(thread.done && !thread.disabled && program.scripts[i].kind == 2)
                    {
                        thread.done = false;
                        thread.stack.emplace_back(program.scripts[i].root);
                    }
                    if(!thread.done) step(thread, true);
                }
                if(director.done || director.disabled) endCue(false);
                continue;
            }
            if(player1->state != CS_ALIVE || intermission) continue;
            if(ridePending)
            {
                ridePending = false;
                noteRide();
            }
            for(size_t i = 0; i < threads.size() && !cue; ++i)
            {
                Thread &thread = threads[i];
                if(!thread.started || thread.disabled || thread.wake > tick) continue;
                if(thread.done && program.scripts[i].kind == 2)
                {
                    thread.done = false;
                    thread.stack.emplace_back(program.scripts[i].root);
                }
                if(!thread.done)
                {
                    const bool hadSave = pendingCheckpoint;
                    step(thread, false);
                    if(halo_scripttrace && cartographerInterior() && !hadSave && pendingCheckpoint)
                        conoutf("Halo checkpoint request: %s at tick %d", program.scripts[i].name.c_str(), tick);
                }
            }
            if(!cue)
            {
                attachActors();
                snapshotAttached();
            }
            if(!cue)
            {
                armSwitches();
                for(size_t i = 0; i < program.switches.size(); ++i)
                {
                    const BspSwitch &entry = program.switches[i];
                    if(switchArm[i] == 1 && entry.from == world && testVolume(entry.volume) &&
                       switchBsp(entry.to, program.volumes[entry.volume].name.c_str())) break;
                }
            }
            if(pendingCheckpoint && !cue && !intermission && (!cartographerInterior() || player1->onfloor))
            {
                pendingCheckpoint = false;
                haloscenarioplayersave();
            }
        }
    }

    void overlayBox(float x, float y, float w, float h, const vec &color, float alpha)
    {
        glDisable(GL_TEXTURE_2D);
        glColor4f(color.x, color.y, color.z, alpha);
        glBegin(GL_TRIANGLE_STRIP);
        glVertex2f(x, y);
        glVertex2f(x + w, y);
        glVertex2f(x, y + h);
        glVertex2f(x + w, y + h);
        glEnd();
        glEnable(GL_TEXTURE_2D);
    }

    void overlayText(const char *text, float y, float scale, int opacity = 255)
    {
        glPushMatrix();
        glTranslatef(VIRTW * .5f, y, 0);
        glScalef(scale, scale, 1);
        int width = min(text_width(text), int(VIRTW * .83f / scale));
        draw_text(text, -width/2, 0, 242, 247, 255, opacity, -1, width + 2);
        glPopMatrix();
    }

    void Scenario::draw()
    {
        if(!loaded) return;
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        if(cue)
        {
            if(letterbox)
            {
                const int border = VIRTH / 8;
                overlayBox(0, 0, VIRTW, border, vec(), 1);
                overlayBox(0, VIRTH - border, VIRTW, border, vec(), 1);
            }
            float blend = fade.end > fade.start ? clamp(float(tick - fade.start) / (fade.end - fade.start), 0.0f, 1.0f) : 1;
            float alpha = fade.from + (fade.to - fade.from) * blend;
            if(alpha > 0) overlayBox(0, 0, VIRTW, VIRTH, fade.color, alpha);
            overlayText("Enter to skip", VIRTH - 68, .34f, 177);
        }
        const Title *title = at(program.titles, chapter);
        if(title && tick >= chapterStart && !title->text.empty())
        {
            int age = tick - chapterStart;
            float alpha = age < title->times.x ? age / max(1.0f, title->times.x) :
                          1 - (age - title->times.y) / max(1.0f, title->times.z);
            if(alpha > 0) overlayText(title->text.c_str(), VIRTH * .23f, .70f, int(255 * min(1.0f, alpha)));
        }
        if(subtitleEnd > tick && !subtitle.empty())
        {
            overlayBox(VIRTW * .08f, VIRTH * .79f, VIRTW * .84f, VIRTH * .088f, vec(), .70f);
            overlayText(subtitle.c_str(), VIRTH * .801f, .52f);
        }
        if(!cue && showHelp && !help.empty()) overlayText(help.c_str(), VIRTH * .30f, .53f);
        glColor4ub(255, 255, 255, 255);
    }

    // Story figures the scripts show during play, outside any scene (a50's Keyes sitting in his
    // cell, d40's Cortana left on the bridge after cinematic_bridge, d20's Flood-infested Keyes):
    // drawn with their scene model. The .hscb (version 9) flags them (OBJECT_STORY_FIGURE,
    // tools/import_halo_scripts.py story_figures: created by a gameplay script or left alive by a
    // Call scene, not placed dead); an older one falls back to the keyes, cortana and monitor name
    // prefixes. A figure attached to a squad stands for an actor: drawn only while a custom
    // animation poses it and no actor has taken it over (attachActors).
    bool storyFigure(const Program &program, size_t index, const Actor &actor, bool hasActor)
    {
        const Object &object = program.objects[index];
        bool figure = false;
        if(!program.objectFlags.empty()) figure = index < program.objectFlags.size() && (program.objectFlags[index] & OBJECT_STORY_FIGURE);
        else
        {
            static const char *const prefixes[] = {"keyes", "captain_keyes", "cortana", "monitor"};
            const std::string &name = object.name;
            for(const char *prefix : prefixes) if(!name.compare(0, strlen(prefix), prefix)) { figure = true; break; }
            if(name.find("chip") != std::string::npos || name.find("effect") != std::string::npos ||
               name.find("teleport") != std::string::npos) figure = false;
        }
        if(!figure) return false;
        return actor.squadReference < 0 || (actor.animation != nullptr && !hasActor);
    }

    void Scenario::render()
    {
        renderDevices();
        const int landingPelican = !cue && mission == 3 && section == 2 && halocollisionactive()
            ? named(program.objects, "extraction_pelican") : -1;
        if(!cue && !projection) return;
        for(size_t i = 0; i < actors.size(); ++i)
        {
            const Actor &actor = actors[i];
            const Object &object = program.objects[i];
            // Outside scenes: b30's landing Pelican, the story figures, and the vehicles the engine
            // does not drive (dropships, Pelicans, cinematic Banshees) while a recording or a
            // parent moves them, after one has (hovering where it set them down), or while
            // someone rides in them.
            const bool flown = object.kind >= 0 && !halovehiclebound(int(i)) &&
                               (actor.recording || actor.parent >= 0 || actor.recordingEnd > 0 || boardedVehicle == int(i) ||
                                std::any_of(cargo.begin(), cargo.end(), [i](const Cargo &entry) { return entry.vehicle == int(i) && !entry.walking; }));
            // An attached object carried in from the section before stands for its actor too while
            // that waits to spawn behind the player (its pose is the old section's).
            const bool owned = actor.squadReference >= 0 &&
                (attachedEntry(int(i)) >= 0 || std::any_of(carriedAttached.begin(), carriedAttached.end(), [i](const CarriedAttach &c) { return c.object == int(i); }));
            if(!cue && int(i) != landingPelican && !(actor.visible && (storyFigure(program, i, actor, owned) || flown))) continue;
            // Damage targets are invisible in Halo (d40's manifold detectors).
            if(!actor.visible || object.model.empty() || objectDevices[i] >= 0 || object.vitality > 0) continue;
            Pose pose = currentActor(int(i));
            if(!std::isfinite(pose.position.x) || !std::isfinite(pose.position.y) || !std::isfinite(pose.position.z)) continue;
            vec point(pose.position.x * 4, -pose.position.y * 4, pose.position.z * 4);
            if(!cue) point = toWorld(*projection, pose.position);
            if(camera1->o.dist(point) > 3200) continue;
            const bool moving = !actor.animation && actor.gait >= 0 && !object.locomotion[actor.gait].empty();
            const char *model = actor.animation ? actor.animation->model.c_str()
                              : moving ? object.locomotion[actor.gait].c_str() : object.model.c_str();
            int animation = ANIM_MAPMODEL | (actor.animationLoop || moving ? ANIM_LOOP : 0);
            int start = actor.animation ? actor.animationStart : actor.gaitStart;
            int basetime = actor.animation || moving ? lastmillis - (tick - start) * 1000 / ticksPerSecond : 0;
            vec angles = renderAngles(pose);
            rendermodel(model, animation, 0, -1, point, angles.z, 180 - angles.x,
                        angles.y, 0, basetime, nullptr, nullptr, actor.scale * (cue ? 1.0f : projection->xy / 4));
        }
    }
}

void haloscenariostart(int mission, int section, bool preview, bool continuing)
{
    if(!continuing) scenario.runningGameplay = false;
    if(!preview || (mission == 3 && halocollisionactive())) scenario.start(mission, section);
}

void haloscenariostop() { scenario.stop(); }

bool haloscenarioprofile(int index, const char *name, HaloStartingProfile &out)
{
    if(!scenario.loaded) return false;
    const std::vector<HaloStartingProfile> &profiles = scenario.program.profiles;
    const HaloStartingProfile *found = nullptr;
    if(name && *name) for(const HaloStartingProfile &profile : profiles) if(profile.name == name) { found = &profile; break; }
    if(!found) found = at(profiles, index);
    if(!found) return false;
    out = *found;
    return true;
}

// For the QA client: adds `count` entries for actors that are gone, as a long fight leaves behind.
void haloscenarioqapad(int count)
{
    for(int i = 0; i < count; ++i) scenario.squadBots.push_back({nullptr, nullptr, -1, -1});
}

int haloscenarioencountercount() { return scenario.loaded ? int(scenario.program.encounters.size()) : 0; }

const char *haloscenarioencountername(int index)
{
    return scenario.loaded && index >= 0 && index < int(scenario.program.encounters.size()) ? scenario.program.encounters[index].name.c_str() : "";
}

// Start count of an encounter on the loaded BSP, or -1 when it belongs to another one.
int haloscenarioencounterstarts(int index)
{
    if(!scenario.loaded || !scenario.projection || index < 0 || index >= int(scenario.program.encounters.size())) return -1;
    const Encounter &encounter = scenario.program.encounters[index];
    if(encounter.bsp >= 0 && encounter.bsp != scenario.projection->bsp) return -1;
    int starts = 0;
    for(const Squad &squad : encounter.squads) starts += int(squad.starts.size());
    return starts;
}

int haloscenarioplace(const char *encounter)
{
    if(!scenario.loaded) return -1;
    for(size_t i = 0; i < scenario.program.encounters.size(); ++i)
        if(scenario.program.encounters[i].name == encounter)
        {
            const size_t before = scenario.squadBots.size();
            scenario.placeSquads(int(i), 0);
            return int(scenario.squadBots.size() - before);
        }
    return -1;
}

int haloscenarioencounter(const char *name)
{
    if(!scenario.loaded) return -1;
    for(size_t i = 0; i < scenario.program.encounters.size(); ++i)
        if(scenario.program.encounters[i].name == name) return int(i);
    return -1;
}

double haloscenariocall(const char *command, double first, double second)
{
    // Runs a script command outside any thread, as the QA client's probe.
    if(!scenario.loaded) return 0;
    Thread thread;
    Node node{1, 0xffff, 8, 0, nil, 0, 0};
    std::vector<Value> args{number(first), number(second)};
    return scenario.command(thread, node, command, args, false).number;
}

// QA probes: a script command with several arguments, an object's index by name, and the state the object commands set.
double haloscenariocallv(const char *command, const double *values, const char *const *texts, int count)
{
    if(!scenario.loaded) return 0;
    Thread thread;
    Node node{1, 0xffff, 8, 0, nil, 0, 0};
    std::vector<Value> args;
    for(int i = 0; i < count; ++i) { args.push_back(number(values[i])); args.back().text = texts[i]; }
    return scenario.command(thread, node, command, args, false).number;
}

int haloscenarioobject(const char *name) { return scenario.loaded ? named(scenario.program.objects, name) : -1; }
int haloscenarioflag(const char *name) { return scenario.loaded ? named(scenario.program.flags, name) : -1; }

double haloscenarioobjectstate(const char *what, int id)
{
    if(!scenario.loaded) return -1;
    const Actor *item = at(scenario.actors, id);
    if(!item) return -1;
    if(!strcmp(what, "open")) return item->open;
    if(!strcmp(what, "hovering")) return item->hovering;
    if(!strcmp(what, "collideable")) return item->collideable;
    if(!strcmp(what, "enterable")) return item->enterable;
    if(!strcmp(what, "suspended")) return item->suspended;
    if(!strcmp(what, "nodamage")) return item->cannotTakeDamage;
    if(!strcmp(what, "melee")) return item->meleeInhibited;
    if(!strcmp(what, "ranged")) return item->rangedInhibited;
    if(!strcmp(what, "squad")) return item->squadReference;
    if(!strcmp(what, "boardable")) return item->boardable;
    if(!strcmp(what, "permutation")) return double(item->permutation.size());
    if(!strcmp(what, "door")) return at(scenario.objectDevices, id) && scenario.objectDevices[id] >= 0 ? scenario.devices[scenario.objectDevices[id]].visible : -1;
    if(!strcmp(what, "visible")) return item->visible;
    // tests/test_ai_attach.py (gap 4): how far, in Halo world units, the object's pose is from the
    // feet of the actor ai_attach made of it (-1 without a living one); a scene's start copies one onto the other.
    if(!strcmp(what, "attachgap"))
    {
        const SquadBot *bot = at(scenario.squadBots, scenario.attachedEntry(id));
        if(!bot || !bot->bot || bot->bot->state != CS_ALIVE || !scenario.projection) return -1;
        return item->pose.position.dist(fromWorld(*scenario.projection, vec(bot->bot->o).sub(vec(0, 0, bot->bot->eyeheight))));
    }
    // Whether it waits to come along behind the player (carried in from the section before, not spawned yet).
    if(!strcmp(what, "carried"))
        return std::any_of(scenario.carriedAttached.begin(), scenario.carriedAttached.end(), [id](const CarriedAttach &c) { return c.object == id; });
    return -1;
}

// QA probe (the shipping client never calls it): wake a script by name (action 0), or ask whether it has started (1) or finished (2).
double haloscenarioqascript(const char *name, int action)
{
    if(!scenario.loaded) return -1;
    const int index = named(scenario.program.scripts, name);
    if(index < 0 || size_t(index) >= scenario.threads.size()) return -1;
    if(action == 0) { scenario.wake(index, false); return 1; }
    // 3: run a static script once as a thread of its own, as the script that calls it would (wake
    // starts only startup, dormant and continuous scripts; tests/test_mission_exits.py item 3).
    if(action == 3)
    {
        Thread &thread = scenario.threads[index];
        if(scenario.program.scripts[index].kind <= 2 || (thread.started && !thread.done)) return 0;
        thread = Thread();
        thread.started = true;
        thread.wake = scenario.tick;
        thread.stack.emplace_back(scenario.program.scripts[index].root);
        return 1;
    }
    return action == 1 ? scenario.threads[index].started : scenario.threads[index].done;
}

// QA probes (tests/test_frontend_progress.py): a global by name (read, or set with `set`), and
// the script's clock and how many threads have started and objects are visible.
double haloscenarioqaglobal(const char *name, bool set, double value)
{
    if(!scenario.loaded) return -1;
    const int index = named(scenario.program.globals, name);
    if(index < 0 || size_t(index) >= scenario.globals.size()) return -1;
    if(set) scenario.globals[index].number = value;
    return scenario.globals[index].number;
}

bool haloscenarioqacue(const char *script)
{
    if(!scenario.loaded || scenario.cue || !scenario.callCue(script)) return false;
    return scenario.beginCue(SceneTrigger::Call, script);
}

bool haloscenarioflagpoint(const char *flag, vec &feet, float &yaw)
{
    const Flag *f = scenario.loaded && scenario.projection ? at(scenario.program.flags, named(scenario.program.flags, flag)) : nullptr;
    if(!f) return false;
    feet = toWorld(*scenario.projection, f->pose.position);
    yaw = fmodf(450.0f - f->pose.yaw / RAD, 360.0f);
    return true;
}

void haloscenarioqastate(int &tick, int &started, int &visible)
{
    tick = scenario.loaded ? scenario.tick : -1;
    started = visible = 0;
    for(const Thread &t : scenario.threads) started += t.started;
    for(const Actor &a : scenario.actors) visible += a.visible;
}

void haloscenariowakescript(int script)
{
    if(scenario.loaded) scenario.wake(script, false);
}

bool haloscenariobotsuspended(const playerent *bot)
{
    if(!scenario.loaded) return false;
    for(const SquadBot &member : scenario.squadBots) if(member.bot == bot) return member.suspended;
    return false;
}

bool haloscenarioattachedactor(const playerent *bot)
{
    if(!scenario.loaded || !bot) return false;
    for(const SquadBot &member : scenario.squadBots) if(member.bot == bot && member.object >= 0) return true;
    return false;
}

void haloscenarioremovebot(const playerent *bot)
{
    for(SquadBot &member : scenario.squadBots) if(member.bot == bot) member.bot = nullptr;
    // A rider or walker of a dropship that is removed (an attached actor erased) is no longer aboard.
    scenario.cargo.erase(std::remove_if(scenario.cargo.begin(), scenario.cargo.end(),
                                        [bot](const Scenario::Cargo &entry) { return entry.unit == bot; }), scenario.cargo.end());
}

void haloscenariotick(int elapsed)
{
    haloscenariopausestate(menuvisible() || ispaused);
    scenario.update(elapsed);
}

void haloscenariocheckpoint()
{
    if(!scenario.loaded || scenario.cue) return;
    scenario.savedGlobals = scenario.globals;
    scenario.savedThreads = scenario.threads;
    scenario.savedActors = scenario.actors;
    scenario.savedSquadBots.clear();
    for(const SquadBot &bot : scenario.squadBots)
    {
        if(bot.bot && bot.bot->state == CS_ALIVE)
        {
            scenario.savedSquadBots.push_back(bot);
            // An attached actor comes back where it stands now, with the health and shield it has
            // now; its start stays where it took over (ai_teleport_to_starting_location).
            if(bot.object >= 0 && scenario.projection)
            {
                SquadBot &copy = scenario.savedSquadBots.back();
                copy.stood = true;
                copy.stoodPoint = fromWorld(*scenario.projection, vec(bot.bot->o).sub(vec(0, 0, bot.bot->eyeheight)));
                copy.stoodYaw = (90 - bot.bot->yaw) * RAD;
                copy.health = haloactorhealthfraction(bot.bot);
                copy.shield = haloenemyshieldfraction(bot.bot);
            }
        }
        else if(bot.object >= 0)
        {
            scenario.savedSquadBots.push_back(bot);
            scenario.savedSquadBots.back().gone = true;
        }
    }
    scenario.savedAttachedAt = scenario.attachedAt;
    scenario.savedCarried = scenario.carriedAttached;
    scenario.savedAttachVitality = scenario.attachVitality;
    scenario.savedBoarded = {};
    if(scenario.boardedVehicle != noObject)
    {
        scenario.savedBoarded.vehicle = scenario.boardedVehicle;
        scenario.savedBoarded.seat = scenario.boardedSeat;
        scenario.savedBoarded.from = scenario.boardedFrom;
        scenario.savedBoarded.scripted = scenario.boardedScripted;
    }
    scenario.savedGroups = scenario.groups;
    scenario.savedDevices = scenario.devices;
    scenario.savedExternal = scenario.external;
    scenario.savedObjective = scenario.objective;
    scenario.savedHelp = scenario.help;
    scenario.savedShowHelp = scenario.showHelp;
    scenario.savedNavPoints = scenario.navPoints;
    scenario.savedHudHidden = scenario.hudHidden;
    scenario.savedHudBlinking = scenario.hudBlinking;
    scenario.savedChapter = scenario.chapter;
    scenario.savedChapterStart = scenario.chapterStart;
    scenario.savedTick = scenario.tick;
    haloallegiancecheckpoint();
}

void haloscenariorevert()
{
    if(!scenario.loaded) return;
    // A revert during a scene a script called (not expected: the actors are held meanwhile) ends
    // it without handing back the objects or moving the player: the checkpoint's state replaces both.
    if(scenario.cue && scenario.cue->trigger == SceneTrigger::Call)
    {
        scenario.director = {};
        scenario.callActors.clear();
        scenario.sceneObjects.clear();
        // Nor does it carry the scene's woken scripts or its seat into play: the checkpoint's threads replace them.
        scenario.sceneWoken.clear();
        scenario.sceneSeat = noObject;
        scenario.handoffTeleport = scenario.handoffFacing = -1;
        scenario.endCue(false);
    }
    scenario.stopSounds();
    halofxrevert();
    // The actors aboard were respawned with the revert; the player is put back on foot.
    scenario.clearCargo();
    if(!scenario.savedThreads.empty())
    {
        scenario.globals = scenario.savedGlobals;
        scenario.threads = scenario.savedThreads;
        scenario.actors = scenario.savedActors;
        scenario.groups = scenario.savedGroups;
        scenario.devices = scenario.savedDevices;
        scenario.external = scenario.savedExternal;
        scenario.objective = scenario.savedObjective;
        scenario.help = scenario.savedHelp;
        scenario.showHelp = scenario.savedShowHelp;
        scenario.navPoints = scenario.savedNavPoints;
        scenario.hudHidden = scenario.savedHudHidden;
        scenario.hudBlinking = scenario.savedHudBlinking;
        scenario.chapter = scenario.savedChapter;
        scenario.chapterStart = scenario.savedChapterStart;
        haloallegiancerevert();
        scenario.restoreSquads();
        scenario.tick = scenario.savedTick;
        scenario.accumulator = 0;
        // The attaches as they stood at the checkpoint; a failed spawn's retry tick belonged to the
        // time after it (left as it was, it could hold an attach back for as long as that lasted).
        if(scenario.savedAttachedAt.size() == scenario.attachedAt.size()) scenario.attachedAt = scenario.savedAttachedAt;
        std::fill(scenario.attachRetry.begin(), scenario.attachRetry.end(), 0);
        // The actors carried in that had not spawned at the checkpoint spawn behind the player
        // again (the section's start checkpoint is usually taken before they do), and the
        // vitality queued then waits for its actor again.
        scenario.carriedAttached = scenario.savedCarried;
        for(CarriedAttach &c : scenario.carriedAttached) c.retry = 0;
        scenario.attachVitality = scenario.savedAttachVitality;
        std::fill(scenario.conversationStatus.begin(), scenario.conversationStatus.end(), 5);
        // Aboard a script vehicle at the checkpoint (a30's extraction Pelicans): aboard again.
        const Scenario::Boarded &b = scenario.savedBoarded;
        const Actor *vehicle = at(scenario.actors, b.vehicle);
        vec seat;
        if(vehicle && vehicle->visible && (vehicle->boardable || b.scripted) && scenario.scriptVehicle(b.vehicle) &&
           player1->state == CS_ALIVE && scenario.seatWorld(b.vehicle, b.seat, seat))
        {
            scenario.boardedVehicle = b.vehicle;
            scenario.boardedSeat = b.seat;
            scenario.boardedFrom = b.from;
            scenario.boardedAt = seat;
            // A seat the script held the player in at the checkpoint holds them again; the reverted
            // script lets them out (its hold timer starts over).
            scenario.boardedScripted = b.scripted;
            scenario.boardedSince = lastmillis;
            scenario.boardedStart = seat;
            scenario.boardedCamera = b.scripted ? scenario.cameraMarker(b.vehicle, b.seat) : -1;
            scenario.boardedPutValid = false;
            conoutf("Halo: player aboard %s again (the checkpoint's seat)", scenario.program.objects[b.vehicle].name.c_str());
        }
    }
    else
    {
        const int m = scenario.mission, s = scenario.section;
        scenario.start(m, s);
        if(scenario.cue) scenario.endCue(true);
    }
}

// ---- Saved games: the mission script's state in config/halo-progress.txt (halo.cpp, version 3)
//
// The records hold what haloscenariocheckpoint() keeps for a revert (globals, threads with their
// whole call stacks, scripted objects, the living squad members, device groups and devices,
// external globals, objective and help text, nav points, the HUD parts hidden or blinking, the
// chapter and the tick), plus what the scripts set on the AI for the rest of the section and a
// revert leaves as it is (actor lists, activation links, migration targets, platoon rules that
// fired, ai_grenades, the HUD timer). Script, global, object, device, encounter and squad records
// carry their names, checked against the loaded .hscb; a thread's frames are node datums, which
// only mean anything with the same .hscb, so halo.cpp also compares the file's hash and falls back
// to restarting the script when it differs. Pointers (text, recordings, animations, squad starts)
// are written as offsets and indices.
namespace
{
    struct StateView
    {
        const std::vector<Value> *globals;
        const std::vector<Thread> *threads;
        const std::vector<Actor> *actors;
        std::vector<SquadBot> squads;
        const std::vector<DeviceGroup> *groups;
        const std::vector<DeviceState> *devices;
        const std::unordered_map<int, Value> *external;
        const std::string *objective, *help;
        bool showHelp;
        const std::vector<NavPoint> *navs;
        int hudHidden, hudBlinking, chapter, chapterStart, tick;
        Scenario::Boarded boarded;
        // Scripted objects attached in play whose actor does not exist yet (posed, carried or
        // within the two ticks): the optional `s attaching` record.
        std::vector<int> attaching;
        // Attached actors carried in from the section before, not spawned yet (gap 4).
        const std::vector<CarriedAttach> *carried = nullptr;
    };

    // The optional `s attached` record: an actor ai_attach made of a scripted object, its squad,
    // its variant and where it stands, then (gap 4, optional on reading) its health and shield
    // fractions and whether it is one carried in from the section before that spawns behind the
    // player (its point unused), then (batch 5's critic, gap 11; optional on reading) its start,
    // where it took over, which ai_teleport_to_starting_location goes back to.
    void writeAttached(std::string &out, int object, int encounter, int squad, bool suspended, int flags, bool gone,
                       const SquadStart &start, const vec &point, float yaw, float health, float shield, bool near)
    {
        const Program &p = scenario.program;
        halostatef(out, "s attached %d", object);
        halostateword(out, p.objects[object].name);
        halostatef(out, " %d", encounter);
        halostateword(out, p.encounters[encounter].name);
        halostatef(out, " %d", squad);
        halostateword(out, p.encounters[encounter].squads[squad].name);
        halostatef(out, " %d %d %d %d %d %.9g %.9g %.9g %.9g %.6g %.6g %d %.9g %.9g %.9g %.9g\n", suspended, flags, gone, start.actor,
                   start.variant, point.x, point.y, point.z, yaw, clamp(health, 0.0f, 1.0f), clamp(shield, 0.0f, 1.0f), near,
                   start.point.x, start.point.y, start.point.z, start.yaw);
    }

    void writeValue(std::string &out, const Value &v)
    {
        const std::string &strings = scenario.program.strings;
        long long offset = -1;
        if(v.text >= strings.c_str() && v.text < strings.c_str() + strings.size()) offset = v.text - strings.c_str();
        // The state reader refuses "nan" and "inf" (a damaged record): a script value that went
        // non-finite (a division by zero) is saved as the largest finite number, or 0 for NaN, so
        // Continue still restores the script instead of restarting it.
        const double number = std::isfinite(v.number) ? v.number : std::isnan(v.number) ? 0.0 : v.number > 0 ? 1.7976931348623157e308 : -1.7976931348623157e308;
        halostatef(out, " %.17g %d %lld", number, v.type, offset);
    }

    Value readValue(HaloStateReader &r)
    {
        Value v;
        v.number = r.d();
        v.type = r.i();
        const long long offset = r.integer();
        if(offset < -1 || offset >= (long long)scenario.program.strings.size()) r.ok = false;
        v.text = offset >= 0 && r.ok ? scenario.program.text(uint32_t(offset)) : "";
        return v;
    }

    void writeActor(std::string &out, const Actor &a)
    {
        const Program &p = scenario.program;
        const long long recording = a.recording && !p.recordings.empty() ? a.recording - p.recordings.data() : -1;
        const long long animation = a.animation && !p.animations.empty() ? a.animation - p.animations.data() : -1;
        halostatef(out, " %.9g %.9g %.9g %.9g %.9g %.9g %.9g %d %d %d %d %d %.9g %.9g %.9g %.9g %.9g %d %d %d %d %d %d %d %d",
                   a.pose.position.x, a.pose.position.y, a.pose.position.z, a.pose.yaw, a.pose.pitch, a.pose.roll, a.pose.fov,
                   a.visible, a.animationLoop, a.recordingDeletes, a.recordingHover, a.grounded, a.scale, a.speed,
                   a.velocity.x, a.velocity.y, a.velocity.z, a.animationStart, a.animationEnd, a.recordingStart, a.recordingEnd,
                   a.parent, a.gait, a.gaitStart, a.baseSeat);
        halostatef(out, " %d %d %d %d %d %d %d %d %d %zu %lld %lld", a.open, a.hovering, a.collideable, a.enterable, a.suspended,
                   a.cannotTakeDamage, a.meleeInhibited, a.rangedInhibited, a.squadReference, a.input, recording, animation);
        halostateword(out, a.permutation);
        halostateword(out, a.parentMarker);
        halostateword(out, a.childMarker);
    }

    bool readActor(HaloStateReader &r, Actor &a)
    {
        const Program &p = scenario.program;
        a.pose.position.x = r.f(); a.pose.position.y = r.f(); a.pose.position.z = r.f();
        a.pose.yaw = r.f(); a.pose.pitch = r.f(); a.pose.roll = r.f(); a.pose.fov = r.f();
        a.visible = r.b(); a.animationLoop = r.b(); a.recordingDeletes = r.b(); a.recordingHover = r.b(); a.grounded = r.b();
        a.scale = r.f(); a.speed = r.f();
        a.velocity.x = r.f(); a.velocity.y = r.f(); a.velocity.z = r.f();
        a.animationStart = r.i(); a.animationEnd = r.i(); a.recordingStart = r.i(); a.recordingEnd = r.i();
        a.parent = r.i(); a.gait = r.i(); a.gaitStart = r.i(); a.baseSeat = r.i();
        a.open = r.b(); a.hovering = r.b(); a.collideable = r.b(); a.enterable = r.b(); a.suspended = r.b();
        a.cannotTakeDamage = r.b(); a.meleeInhibited = r.b(); a.rangedInhibited = r.b();
        a.squadReference = r.i();
        const long long input = r.integer(), recording = r.integer(), animation = r.integer();
        a.permutation = r.word();
        a.parentMarker = r.word();
        a.childMarker = r.word();
        // gait indexes Object::locomotion[3] unchecked when the actor is drawn.
        if(!r.ok || input < 0 || recording < -1 || recording >= (long long)p.recordings.size() ||
           animation < -1 || animation >= (long long)p.animations.size() || a.gait < -1 || a.gait > 2) return false;
        a.recording = recording >= 0 ? &p.recordings[size_t(recording)] : nullptr;
        a.animation = animation >= 0 ? &p.animations[size_t(animation)] : nullptr;
        a.input = size_t(input);
        // A recording's input index past its end would read out of range.
        if(a.recording && (a.recording->inputs.empty() || a.input >= a.recording->inputs.size())) return false;
        return true;
    }

    template<class T> void writeInts(std::string &out, const char *key, const T &values)
    {
        halostatef(out, "s %s %zu", key, values.size());
        for(int v : values) halostatef(out, " %d", v);
        out += '\n';
    }

    template<class F> bool readInts(HaloStateReader &r, F add)
    {
        const int count = r.i();
        if(!r.ok || count < 0 || count > 100000) return false;
        for(int k = 0; k < count && r.ok; ++k) add(r.i());
        return r.ok;
    }

    std::string objectName(int object)
    {
        const Object *o = at(scenario.program.objects, object);
        return o ? o->name : std::string();
    }

    void writeState(std::string &out, const StateView &v)
    {
        const Program &p = scenario.program;
        halostatef(out, "s program");
        halostateword(out, scenario.cache);
        halostatef(out, " %08x %zu %zu %zu %zu %zu %zu %zu\n", scenario.programHash, p.nodes.size(), p.scripts.size(), p.globals.size(),
                   p.objects.size(), p.devices.size(), p.deviceGroups.size(), p.encounters.size());
        halostatef(out, "s clock %d %d %d %d %d %d\n", v.tick, v.chapter, v.chapterStart, v.hudHidden, v.hudBlinking, haloaigrenades() ? 1 : 0);
        halostatef(out, "s text");
        halostateword(out, *v.objective);
        halostateword(out, *v.help);
        halostatef(out, " %d\n", v.showHelp);
        for(size_t i = 0; i < v.globals->size(); ++i)
        {
            halostatef(out, "s global %zu", i);
            halostateword(out, p.globals[i].name);
            writeValue(out, (*v.globals)[i]);
            out += '\n';
        }
        // Map order is not stable: sort the external globals by id.
        std::vector<int> ids;
        for(const auto &e : *v.external) ids.push_back(e.first);
        std::sort(ids.begin(), ids.end());
        for(int id : ids)
        {
            halostatef(out, "s external %d", id);
            writeValue(out, v.external->at(id));
            out += '\n';
        }
        for(size_t i = 0; i < v.threads->size(); ++i)
        {
            const Thread &t = (*v.threads)[i];
            if(t.stack.empty() && !t.wake && !t.started && !t.done && !t.disabled && t.result.type == 4 && !t.result.number) continue;
            halostatef(out, "s thread %zu", i);
            halostateword(out, p.scripts[i].name);
            halostatef(out, " %d %d %d %d", t.wake, t.started, t.done, t.disabled);
            writeValue(out, t.result);
            halostatef(out, " %zu\n", t.stack.size());
            for(const Frame &f : t.stack)
            {
                halostatef(out, "s frame %08x %08x %d %d %d", f.expression, f.next, f.phase, f.deadline, f.interval);
                writeValue(out, f.result);
                halostatef(out, " %zu", f.arguments.size());
                for(const Value &a : f.arguments) writeValue(out, a);
                halostatef(out, " %zu", f.shuffled.size());
                for(uint32_t s : f.shuffled) halostatef(out, " %08x", s);
                out += '\n';
            }
        }
        std::string blank;
        writeActor(blank, Actor());
        for(size_t i = 0; i < v.actors->size(); ++i)
        {
            std::string fields;
            writeActor(fields, (*v.actors)[i]);
            if(fields == blank) continue;
            halostatef(out, "s object %zu", i);
            halostateword(out, p.objects[i].name);
            out += fields;
            out += '\n';
        }
        for(const SquadBot &bot : v.squads)
        {
            const Encounter *e = at(p.encounters, bot.encounter);
            const Squad *s = e ? at(e->squads, bot.squad) : nullptr;
            // An actor ai_attach made of a scripted object (optional record, newer than the
            // format: a save without it still loads): its object, squad and start.
            if(bot.object >= 0)
            {
                const SquadStart *start = at(scenario.objectStarts, bot.object);
                if(!s || !start || !at(p.objects, bot.object)) continue;
                // Where it stood at the checkpoint (its start when it had not moved: a record read
                // from a save written before the stood point was kept).
                vec point = bot.stood ? bot.stoodPoint : start->point;
                float yaw = bot.stood ? bot.stoodYaw : start->yaw, health = bot.health, shield = bot.shield;
                if(bot.bot && bot.bot->state == CS_ALIVE && scenario.projection && v.threads == &scenario.threads)
                {
                    point = fromWorld(*scenario.projection, vec(bot.bot->o).sub(vec(0, 0, bot.bot->eyeheight)));
                    yaw = (90 - bot.bot->yaw) * RAD;
                    health = haloactorhealthfraction(bot.bot);
                    shield = haloenemyshieldfraction(bot.bot);
                }
                writeAttached(out, bot.object, bot.encounter, bot.squad, bot.suspended, bot.scriptFlags, bot.gone, *start, point, yaw,
                              health, shield, false);
                continue;
            }
            if(!s || s->starts.empty() || bot.start < s->starts.data() || bot.start >= s->starts.data() + s->starts.size()) continue;
            halostatef(out, "s squad %d", bot.encounter);
            halostateword(out, e->name);
            halostatef(out, " %d", bot.squad);
            halostateword(out, s->name);
            halostatef(out, " %d %d %d\n", int(bot.start - s->starts.data()), bot.suspended, bot.scriptFlags);
        }
        // Attached actors carried in from the section before that have not spawned yet (gap 4):
        // the same record, flagged to spawn behind the player again.
        if(v.carried)
            for(const CarriedAttach &c : *v.carried)
            {
                if(!at(p.objects, c.object) || !at(p.encounters, c.encounter) || !at(p.encounters[c.encounter].squads, c.squad)) continue;
                writeAttached(out, c.object, c.encounter, c.squad, c.suspended, c.scriptFlags, false, c.start, vec(0, 0, 0), 0, c.health,
                              c.shield, true);
            }
        for(size_t i = 0; i < v.groups->size(); ++i)
        {
            const DeviceGroup &g = (*v.groups)[i];
            halostatef(out, "s group %zu %.9g %d %d\n", i, g.value, g.once, g.changed);
        }
        for(size_t i = 0; i < v.devices->size(); ++i)
        {
            const DeviceState &d = (*v.devices)[i];
            halostatef(out, "s device %zu", i);
            halostateword(out, objectName(p.devices[i].object));
            halostatef(out, " %.9g %.9g %.9g %.9g %.9g %.9g %d %d %d %d %d\n", d.power, d.position, d.powerVelocity, d.positionVelocity,
                       d.desiredPower, d.desiredPosition, d.hold, d.visible, d.automatic, d.oneSided, d.changed);
        }
        for(const NavPoint &n : *v.navs)
        {
            halostatef(out, "s nav %d %d", n.flag, n.object);
            halostateword(out, n.type);
            halostatef(out, " %.9g\n", n.offset);
        }
        // What the scripts set on the AI and the HUD timer: live state, the same as the
        // checkpoint's when written as one is taken.
        writeInts(out, "lists", scenario.actorLists);
        halostatef(out, "s links %zu", scenario.links.size());
        for(const auto &link : scenario.links) halostatef(out, " %d %d", link.first, link.second);
        out += '\n';
        writeInts(out, "migrations", scenario.migrationTargets);
        writeInts(out, "maneuvered", scenario.maneuvered);
        writeInts(out, "flipped", scenario.flipped);
        const HudTimer &timer = scenario.hudTimer;
        halostatef(out, "s timer %d %d %d %d\n", timer.shown, timer.running, timer.remaining, timer.end);
        // ai_allegiance and betrayals: allied and broken team pairs (halo_ai.cpp's bit masks), as
        // the checkpoint keeps them when the checkpoint's state is written, else as they are now.
        // Optional on reading, so a save written before it still loads (default allegiances).
        unsigned allied = 0, broken = 0;
        haloallegianceget(allied, broken, v.threads == &scenario.savedThreads);
        halostatef(out, "s allegiance %u %u\n", allied, broken);
        // Optional records (a save without them loads with their defaults): the script vehicles
        // the player may board (unit_set_enterable_by_player: a30's extraction Pelicans), the one
        // they are aboard, and the sound class gains (sound_class_set_gain: c10's fades), as they
        // stand when the save is written.
        std::vector<int> boardable;
        for(size_t i = 0; i < v.actors->size(); ++i) if((*v.actors)[i].boardable) boardable.push_back(int(i));
        if(!boardable.empty()) writeInts(out, "board", boardable);
        // The vehicle the player is aboard: a Pelican boarded with E, or (gap 10) a seat the script
        // holds them in, with a trailing scripted flag (a10's cryotube: a Continue puts the player
        // back in the tube, and the restored tutorial's unit_exit_vehicle lets them out). A scripted
        // seat may be -1, the middle of a vehicle with no seats in the table. Older saves have no flag.
        if(at(p.objects, v.boarded.vehicle))
        {
            halostatef(out, "s boarded %d", v.boarded.vehicle);
            halostateword(out, p.objects[v.boarded.vehicle].name);
            halostatef(out, " %d %.9g %.9g %.9g", v.boarded.seat, v.boarded.from.x, v.boarded.from.y, v.boarded.from.z);
            if(v.boarded.scripted) halostatef(out, " 1");
            out += '\n';
        }
        if(!v.attaching.empty()) writeInts(out, "attaching", v.attaching);
        float gains[HALO_SOUND_CLASSES];
        if(halofxgains(gains))
        {
            halostatef(out, "s gain %d", int(HALO_SOUND_CLASSES));
            loopi(HALO_SOUND_CLASSES) halostatef(out, " %.6g", gains[i]);
            out += '\n';
        }
    }

    // Everything a state block restores, read in full before any of it is applied.
    struct ParsedState
    {
        std::vector<Value> globals;
        std::unordered_map<int, Value> external;
        std::vector<Thread> threads;
        std::vector<Actor> actors;
        std::vector<SquadBot> squads;
        std::vector<DeviceGroup> groups;
        std::vector<DeviceState> devices;
        std::vector<NavPoint> navs;
        std::string objective, help;
        bool showHelp = false, grenades = true;
        int tick = 0, chapter = -1, chapterStart = 0, hudHidden = 0, hudBlinking = 0;
        std::vector<int> actorLists;
        std::vector<std::pair<int, int>> links;
        std::set<int> migrations, maneuvered, flipped;
        HudTimer timer;
        bool allegiance = false;
        unsigned allied = 0, broken = 0;
        // Optional records: attached actors (with their starts), boardable script vehicles, the
        // vehicle the player is aboard, the sound class gains.
        std::vector<std::pair<SquadBot, SquadStart>> attached;
        std::vector<CarriedAttach> carried;
        std::vector<int> boardable, attaching;
        Scenario::Boarded boarded;
        bool gains = false;
        float gain[HALO_SOUND_CLASSES] = {};
    };

    bool parseState(const std::vector<std::string> &records, ParsedState &s, std::string &why)
    {
        const Program &p = scenario.program;
        s.globals.assign(p.globals.size(), {});
        s.threads.assign(p.scripts.size(), {});
        s.actors.assign(p.objects.size(), {});
        s.groups.clear();
        s.devices.clear();
        bool program = false, clock = false, text = false, timer = false;
        std::vector<bool> globalSeen(p.globals.size(), false);
        Thread *thread = nullptr;
        size_t frames = 0;
        auto fail = [&](const char *reason, size_t line) { why = std::string(reason) + " (script record " + std::to_string(line + 1) + ")"; return false; };
        for(size_t line = 0; line < records.size(); ++line)
        {
            HaloStateReader r(records[line].c_str());
            if(frames && !r.key("frame")) return fail("a thread is missing frames", line);
            if(frames)
            {
                Frame f(r.x());
                f.next = r.x();
                f.phase = r.i(); f.deadline = r.i(); f.interval = r.i();
                f.result = readValue(r);
                const int args = r.i();
                if(!r.ok || args < 0 || args > 4096) return fail("bad frame", line);
                for(int k = 0; k < args && r.ok; ++k) f.arguments.push_back(readValue(r));
                const int shuffled = r.i();
                if(!r.ok || shuffled < 0 || shuffled > 256) return fail("bad frame", line);
                for(int k = 0; k < shuffled && r.ok; ++k) f.shuffled.push_back(r.x());
                // step() dereferences a frame's expression without a check.
                if(!r.ok || r.more() || !p.node(f.expression)) return fail("bad frame", line);
                thread->stack.push_back(std::move(f));
                --frames;
                continue;
            }
            if(r.key("program"))
            {
                const std::string cache = r.word();
                r.x();
                const long long counts[7] = {r.integer(), r.integer(), r.integer(), r.integer(), r.integer(), r.integer(), r.integer()};
                const size_t sizes[7] = {p.nodes.size(), p.scripts.size(), p.globals.size(), p.objects.size(), p.devices.size(),
                                         p.deviceGroups.size(), p.encounters.size()};
                if(!r.ok || cache != scenario.cache) return fail("saved for another mission's script", line);
                for(int k = 0; k < 7; ++k) if(counts[k] != (long long)sizes[k]) return fail("the mission script's tables changed", line);
                program = true;
            }
            else if(!program) return fail("no program record", line);
            else if(r.key("clock"))
            {
                s.tick = r.i(); s.chapter = r.i(); s.chapterStart = r.i(); s.hudHidden = r.i(); s.hudBlinking = r.i(); s.grenades = r.b();
                if(!r.ok || s.tick < 0) return fail("bad clock", line);
                clock = true;
            }
            else if(r.key("text"))
            {
                s.objective = r.word(); s.help = r.word(); s.showHelp = r.b();
                if(!r.ok) return fail("bad text", line);
                text = true;
            }
            else if(r.key("global"))
            {
                const int index = r.i();
                const std::string name = r.word();
                const Value v = readValue(r);
                if(!r.ok || !at(p.globals, index) || p.globals[index].name != name) return fail("unknown global", line);
                s.globals[index] = v;
                globalSeen[index] = true;
            }
            else if(r.key("external"))
            {
                const int id = r.i();
                const Value v = readValue(r);
                if(!r.ok) return fail("bad external global", line);
                s.external[id] = v;
            }
            else if(r.key("thread"))
            {
                const int index = r.i();
                const std::string name = r.word();
                if(!r.ok || !at(p.scripts, index) || p.scripts[index].name != name) return fail("unknown script", line);
                Thread &t = s.threads[index];
                t.wake = r.i(); t.started = r.b(); t.done = r.b(); t.disabled = r.b();
                t.result = readValue(r);
                const int count = r.i();
                if(!r.ok || count < 0 || count > 96) return fail("bad thread", line);
                thread = &t;
                frames = size_t(count);
            }
            else if(r.key("object"))
            {
                const int index = r.i();
                const std::string name = r.word();
                if(!r.ok || !at(p.objects, index) || p.objects[index].name != name) return fail("unknown object", line);
                if(!readActor(r, s.actors[index])) return fail("bad object", line);
            }
            else if(r.key("squad"))
            {
                SquadBot bot{};
                bot.encounter = r.i();
                const std::string encounter = r.word();
                bot.squad = r.i();
                const std::string squad = r.word();
                const int start = r.i();
                bot.suspended = r.b();
                bot.scriptFlags = r.i();
                const Encounter *e = at(p.encounters, bot.encounter);
                const Squad *sq = e ? at(e->squads, bot.squad) : nullptr;
                if(!r.ok || !sq || e->name != encounter || sq->name != squad || !at(sq->starts, start)) return fail("unknown squad", line);
                bot.start = &sq->starts[start];
                bot.bot = nullptr;
                s.squads.push_back(bot);
            }
            else if(r.key("group"))
            {
                const int index = r.i();
                DeviceGroup g{};
                g.value = r.f(); g.once = r.b(); g.changed = r.b();
                if(!r.ok || index != int(s.groups.size()) || size_t(index) >= p.deviceGroups.size()) return fail("bad device group", line);
                s.groups.push_back(g);
            }
            else if(r.key("device"))
            {
                const int index = r.i();
                const std::string name = r.word();
                DeviceState d;
                d.power = r.f(); d.position = r.f(); d.powerVelocity = r.f(); d.positionVelocity = r.f();
                d.desiredPower = r.f(); d.desiredPosition = r.f(); d.hold = r.i();
                d.visible = r.b(); d.automatic = r.b(); d.oneSided = r.b(); d.changed = r.b();
                if(!r.ok || index != int(s.devices.size()) || size_t(index) >= p.devices.size() || objectName(p.devices[index].object) != name)
                    return fail("unknown device", line);
                s.devices.push_back(d);
            }
            else if(r.key("nav"))
            {
                NavPoint n;
                n.flag = r.i(); n.object = r.i(); n.type = r.word(); n.offset = r.f();
                if(!r.ok || (n.flag >= 0 && !at(p.flags, n.flag))) return fail("bad nav point", line);
                s.navs.push_back(n);
            }
            else if(r.key("lists")) { if(!readInts(r, [&](int v) { s.actorLists.push_back(v); })) return fail("bad actor lists", line); }
            else if(r.key("links"))
            {
                const int count = r.i();
                if(!r.ok || count < 0 || count > 100000) return fail("bad links", line);
                for(int k = 0; k < count && r.ok; ++k) { const int a = r.i(); s.links.emplace_back(a, r.i()); }
                if(!r.ok) return fail("bad links", line);
            }
            else if(r.key("migrations")) { if(!readInts(r, [&](int v) { s.migrations.insert(v); })) return fail("bad migrations", line); }
            else if(r.key("maneuvered")) { if(!readInts(r, [&](int v) { s.maneuvered.insert(v); })) return fail("bad platoon state", line); }
            else if(r.key("flipped")) { if(!readInts(r, [&](int v) { s.flipped.insert(v); })) return fail("bad platoon state", line); }
            else if(r.key("allegiance"))
            {
                const long long allied = r.integer(), broken = r.integer();
                if(!r.ok || allied < 0 || broken < 0 || allied > 0xffffffffll || broken > 0xffffffffll) return fail("bad allegiances", line);
                s.allied = unsigned(allied);
                s.broken = unsigned(broken);
                s.allegiance = true;
            }
            else if(r.key("timer"))
            {
                s.timer.shown = r.b(); s.timer.running = r.b(); s.timer.remaining = r.i(); s.timer.end = r.i();
                if(!r.ok) return fail("bad HUD timer", line);
                timer = true;
            }
            else if(r.key("attached"))
            {
                SquadBot bot{};
                SquadStart start{};
                bot.object = r.i();
                const std::string object = r.word();
                bot.encounter = r.i();
                const std::string encounter = r.word();
                bot.squad = r.i();
                const std::string squad = r.word();
                bot.suspended = r.b();
                bot.scriptFlags = r.i();
                bot.gone = r.b();
                start.actor = r.i();
                start.variant = r.i();
                start.point.x = r.f(); start.point.y = r.f(); start.point.z = r.f();
                start.yaw = r.f();
                // Optional (gap 4): health, shield, and whether it was carried in and spawns behind the player.
                bool near = false;
                if(r.ok && r.more())
                {
                    bot.health = r.f();
                    bot.shield = r.f();
                    near = r.b();
                    if(!std::isfinite(bot.health) || !std::isfinite(bot.shield)) r.ok = false;
                    bot.health = clamp(bot.health, 0.0f, 1.0f);
                    bot.shield = clamp(bot.shield, 0.0f, 1.0f);
                }
                // Optional (gap 11): its start, where it took over; the point above is where it
                // stood, which a Continue brings it back to. Without it the two are the same.
                if(r.ok && r.more())
                {
                    vec origin;
                    origin.x = r.f(); origin.y = r.f(); origin.z = r.f();
                    const float originYaw = r.f();
                    if(r.ok && (!std::isfinite(origin.x) || !std::isfinite(origin.y) || !std::isfinite(origin.z) || !std::isfinite(originYaw)))
                        r.ok = false;
                    if(r.ok && !near)
                    {
                        bot.stood = true;
                        bot.stoodPoint = start.point;
                        bot.stoodYaw = start.yaw;
                        start.point = origin;
                        start.yaw = originYaw;
                    }
                }
                const Encounter *e = at(p.encounters, bot.encounter);
                const Squad *sq = e ? at(e->squads, bot.squad) : nullptr;
                if(!r.ok || !at(p.objects, bot.object) || p.objects[bot.object].name != object || !sq || e->name != encounter ||
                   sq->name != squad || !std::isfinite(start.point.x) || !std::isfinite(start.point.y) || !std::isfinite(start.point.z) ||
                   !std::isfinite(start.yaw))
                    return fail("unknown attached actor", line);
                bot.bot = nullptr;
                if(near && !bot.gone)
                {
                    CarriedAttach c;
                    c.object = bot.object;
                    c.encounter = bot.encounter;
                    c.squad = bot.squad;
                    c.start = start;
                    c.health = bot.health;
                    c.shield = bot.shield;
                    c.suspended = bot.suspended;
                    c.scriptFlags = bot.scriptFlags;
                    s.carried.push_back(c);
                }
                else s.attached.emplace_back(bot, start);
            }
            else if(r.key("board"))
            {
                if(!readInts(r, [&](int v) { s.boardable.push_back(v); })) return fail("bad boardable vehicles", line);
                for(int v : s.boardable) if(!at(p.objects, v)) return fail("bad boardable vehicles", line);
            }
            else if(r.key("attaching"))
            {
                if(!readInts(r, [&](int v) { s.attaching.push_back(v); })) return fail("bad attaching objects", line);
                for(int v : s.attaching) if(!at(p.objects, v)) return fail("bad attaching objects", line);
            }
            else if(r.key("boarded"))
            {
                s.boarded.vehicle = r.i();
                const std::string name = r.word();
                s.boarded.seat = r.i();
                s.boarded.from.x = r.f(); s.boarded.from.y = r.f(); s.boarded.from.z = r.f();
                // Optional: a seat the script holds the player in (gap 10).
                s.boarded.scripted = r.ok && r.more() && r.b();
                const Object *o = at(p.objects, s.boarded.vehicle);
                const bool seat = o && (at(o->seats, s.boarded.seat) || (s.boarded.scripted && s.boarded.seat == -1 && o->seats.empty()));
                if(!r.ok || !o || o->name != name || !seat || !std::isfinite(s.boarded.from.x) ||
                   !std::isfinite(s.boarded.from.y) || !std::isfinite(s.boarded.from.z))
                    return fail("bad boarded vehicle", line);
            }
            else if(r.key("gain"))
            {
                const int count = r.i();
                if(!r.ok || count != HALO_SOUND_CLASSES) return fail("bad sound class gains", line);
                loopi(HALO_SOUND_CLASSES)
                {
                    s.gain[i] = r.f();
                    if(!std::isfinite(s.gain[i])) r.ok = false;
                }
                if(!r.ok) return fail("bad sound class gains", line);
                s.gains = true;
            }
            else return fail("unknown record", line);
            if(!r.ok || r.more()) return fail("malformed record", line);
        }
        if(frames) { why = "a thread is missing frames"; return false; }
        if(!program || !clock || !text || !timer) { why = "the script state is incomplete"; return false; }
        if(s.groups.size() != p.deviceGroups.size() || s.devices.size() != p.devices.size()) { why = "device records are missing"; return false; }
        if(std::find(globalSeen.begin(), globalSeen.end(), false) != globalSeen.end()) { why = "global records are missing"; return false; }
        return true;
    }
}

bool haloscenariostatesave(std::string &out, bool live)
{
    Scenario &s = scenario;
    if(!s.loaded || !s.projection || (live ? s.cue != nullptr : s.savedThreads.empty())) return false;
    StateView v;
    if(live)
    {
        v = StateView{&s.globals, &s.threads, &s.actors, {}, &s.groups, &s.devices, &s.external, &s.objective, &s.help, s.showHelp,
                      &s.navPoints, s.hudHidden, s.hudBlinking, s.chapter, s.chapterStart, s.tick, {}};
        for(const SquadBot &bot : s.squadBots)
        {
            if(bot.bot && bot.bot->state == CS_ALIVE) v.squads.push_back(bot);
            else if(bot.object >= 0) { v.squads.push_back(bot); v.squads.back().gone = true; }
        }
        if(s.boardedVehicle != noObject)
        {
            v.boarded.vehicle = s.boardedVehicle;
            v.boarded.seat = s.boardedSeat;
            v.boarded.from = s.boardedFrom;
            v.boarded.scripted = s.boardedScripted;
        }
    }
    else v = StateView{&s.savedGlobals, &s.savedThreads, &s.savedActors, s.savedSquadBots, &s.savedGroups, &s.savedDevices,
                       &s.savedExternal, &s.savedObjective, &s.savedHelp, s.savedShowHelp, &s.savedNavPoints, s.savedHudHidden,
                       s.savedHudBlinking, s.savedChapter, s.savedChapterStart, s.savedTick, s.savedBoarded, {}};
    v.carried = live ? &s.carriedAttached : &s.savedCarried;
    // The objects attached in play still waiting for their actor (a50's jailed marines, posed in
    // their cells until the rescue): without them a Continue would leave them figures for good.
    const std::vector<int> &attachedAt = live ? s.attachedAt : s.savedAttachedAt;
    for(size_t i = 0; i < attachedAt.size() && i < v.actors->size(); ++i)
    {
        if(attachedAt[i] < 0 || (*v.actors)[i].squadReference < 0) continue;
        if(std::none_of(v.squads.begin(), v.squads.end(), [i](const SquadBot &bot) { return bot.object == int(i); })) v.attaching.push_back(int(i));
    }
    writeState(out, v);
    return true;
}

bool haloscenariostaterestore(const std::vector<std::string> &records, bool commit, std::string &why)
{
    Scenario &s = scenario;
    if(!s.loaded || !s.projection) { why = "no mission script is loaded"; return false; }
    ParsedState parsed;
    if(!parseState(records, parsed, why)) return false;
    if(!commit) return true;
    // Into the checkpoint's slots: the revert that follows (halo.cpp) restores them as it does
    // after a death, and they stay the checkpoint a later death reverts to.
    s.savedGlobals = std::move(parsed.globals);
    s.savedThreads = std::move(parsed.threads);
    s.savedActors = std::move(parsed.actors);
    s.savedSquadBots = std::move(parsed.squads);
    s.savedGroups = std::move(parsed.groups);
    s.savedDevices = std::move(parsed.devices);
    s.savedExternal = std::move(parsed.external);
    s.savedObjective = parsed.objective;
    s.savedHelp = parsed.help;
    s.savedShowHelp = parsed.showHelp;
    s.savedNavPoints = std::move(parsed.navs);
    s.savedHudHidden = parsed.hudHidden;
    s.savedHudBlinking = parsed.hudBlinking;
    s.savedChapter = parsed.chapter;
    s.savedChapterStart = parsed.chapterStart;
    s.savedTick = parsed.tick;
    s.actorLists = std::move(parsed.actorLists);
    s.links = std::move(parsed.links);
    s.migrationTargets = std::move(parsed.migrations);
    s.maneuvered = std::move(parsed.maneuvered);
    s.flipped = std::move(parsed.flipped);
    s.hudTimer = parsed.timer;
    if(s.objectStarts.size() != s.program.objects.size()) s.objectStarts.assign(s.program.objects.size(), SquadStart{});
    for(auto &entry : parsed.attached)
    {
        s.objectStarts[entry.first.object] = entry.second;
        entry.first.start = &s.objectStarts[entry.first.object];
        s.savedSquadBots.push_back(entry.first);
        // The scripted object is attached already: no second actor from attachActors.
        if(at(s.attachedAt, entry.first.object)) s.attachedAt[entry.first.object] = 0;
        if(at(s.savedAttachedAt, entry.first.object)) s.savedAttachedAt[entry.first.object] = 0;
    }
    // Carried in from the section before and not spawned yet: they spawn behind the player after
    // the revert that follows (it copies savedCarried), as they did when the section began.
    s.savedCarried = std::move(parsed.carried);
    // Attached in play, its actor still to come (the revert that follows copies savedAttachedAt).
    for(int object : parsed.attaching)
    {
        if(at(s.attachedAt, object)) s.attachedAt[object] = 0;
        if(at(s.savedAttachedAt, object)) s.savedAttachedAt[object] = 0;
    }
    for(int v : parsed.boardable) if(at(s.savedActors, v)) s.savedActors[v].boardable = true;
    s.savedBoarded = parsed.boarded;
    if(parsed.gains) halofxsetgains(parsed.gain);
    haloaisetgrenades(parsed.grenades);
    if(parsed.allegiance) haloallegianceput(parsed.allied, parsed.broken);
    else haloallegiancereset();
    return true;
}

unsigned haloscenarioprogramhash() { return scenario.loaded ? scenario.programHash : 0; }

unsigned haloscenariofilehash(int mission)
{
    if(mission < 0 || mission >= haloMissionCount) return 0;
    defformatstring(path)("packages/halo/%s.hscb", haloMissions[mission].cache);
    return halofilehash(path);
}

const char *haloscenarioobjectname(int object)
{
    const Object *o = at(scenario.program.objects, object);
    return o ? o->name.c_str() : "";
}

bool haloscenarioexit() { return scenario.beginCue(SceneTrigger::Exit); }
bool halopelicanready() { return scenario.pelicanReady(); }
bool haloscenarioescapetimeout() { return scenario.beginCue(SceneTrigger::Timeout); }
bool haloscenarioescapefailed()
{
    const bool result = scenario.failedEscape;
    scenario.failedEscape = false;
    return result;
}

bool haloscenariocomplete()
{
    const bool result = scenario.completeExit;
    scenario.completeExit = false;
    return result;
}

bool haloscenariogamewon()
{
    const bool result = scenario.gameWon;
    scenario.gameWon = false;
    return result;
}

bool haloscenariogamelost()
{
    const bool result = scenario.gameLost;
    scenario.gameLost = false;
    return result;
}

bool halocinematicskip()
{
    if(!halocinematic()) return false;
    scenario.endCue(true);
    return true;
}

bool halocinematic() { return scenario.cue && haloactive() && !editmode; }

bool halocinematiccamera(physent &camera)
{
    if(!halocinematic()) return false;
    camera = *static_cast<physent *>(player1);
    camera.type = ENT_CAMERA;
    if(scenario.cameraControl && (scenario.camera.set || scenario.camera.firstPerson >= 0))
    {
        CameraView pose = scenario.currentCamera();
        camera.o = vec(pose.position.x * 4, -pose.position.y * 4, pose.position.z * 4);
        vec angles = renderAngles(pose.forward, pose.up);
        camera.yaw = 90 - angles.x;
        camera.pitch = angles.y;
        camera.roll = angles.z;
    }
    else if(scenario.projection)
    {
        const Projection &p = *scenario.projection;
        camera.o = vec(4 * (p.origin.x + (player1->o.x - p.margin) / p.xy),
                       -4 * (p.origin.y - (player1->o.y - p.margin) / p.xy),
                       4 * (p.origin.z + player1->o.z / p.z));
    }
    return true;
}

const char *halocinematicworld() { return halocinematic() ? scenario.worldName.c_str() : ""; }
float halocinematicfov() { return halocinematic() ? clamp(scenario.currentCamera().fov, 25.0f, 140.0f) : 0; }
float halocinematicnear() { return halocinematic() ? scenario.nearClip * 4 : .15f; }
void haloscenariodraw() { scenario.draw(); }
int haloscenarionavpoints(HaloHudNav *out, int capacity)
{
    if(!scenario.loaded || scenario.cue || !scenario.projection) return 0;
    int count = 0;
    for(const NavPoint &n : scenario.navPoints)
    {
        if(count >= capacity) break;
        vec point;
        if(n.flag >= 0)
        {
            const Flag *flag = at(scenario.program.flags, n.flag);
            if(!flag) continue;
            point = toWorld(*scenario.projection, vec(flag->pose.position).add(vec(0, 0, n.offset)));
        }
        else
        {
            if(!scenario.objectPosition(n.object, point)) continue;
            point.z += n.offset * scenario.projection->z;
        }
        out[count++] = HaloHudNav{point.x, point.y, point.z, n.type.c_str()};
    }
    return count;
}

int haloscenariohudhidden(int *blinking)
{
    if(blinking) *blinking = scenario.loaded ? scenario.hudBlinking : 0;
    return scenario.loaded ? scenario.hudHidden : 0;
}

const char *haloscenarioobjective()
{
    return scenario.loaded && !scenario.objective.empty() ? scenario.objective.c_str() : nullptr;
}
void haloscenariorender() { scenario.render(); }
bool haloscenariouse() { return scenario.useDevice(); }
static vec volumeCenter(const Volume &volume)
{
    vec side;
    side.cross(volume.up, volume.forward);
    return vec(volume.origin).add(vec(volume.forward).mul(volume.extent.x * .5f))
        .add(side.mul(volume.extent.y * .5f)).add(vec(volume.up).mul(volume.extent.z * .5f));
}

bool halocartographertarget(vec &point, bool &mapped)
{
    if(!scenario.loaded || !scenario.cartographerInterior()) return false;
    mapped = scenario.cartographerMapped();
    if(mapped)
    {
        const Volume *volume = at(scenario.program.volumes, named(scenario.program.volumes, "bsp1,0"));
        if(!volume) return false;
        point = toWorld(*scenario.projection, volumeCenter(*volume));
        return true;
    }
    const Object *control = at(scenario.program.objects, named(scenario.program.objects, "shafta_map_control"));
    if(!control) return false;
    point = toWorld(*scenario.projection, control->pose.position);
    return true;
}
bool haloscenariocollide(physent *entity, float &hi, float &lo) { return scenario.collideDevices(entity, hi, lo); }
float haloscenarioliftboard(physent *entity, float lo) { return scenario.liftBoard(entity, lo); }
// The script name of the device a body at this spot would collide with, or null (QA diagnostics).
const char *haloscenarioblockingdevice(physent *entity)
{
    float hi = 1e9f, lo = -1e9f;
    int index = -1;
    if(!scenario.collideDevices(entity, hi, lo, &index) || index < 0) return nullptr;
    const auto *object = at(scenario.program.objects, scenario.program.devices[index].object);
    return object && !object->name.empty() ? object->name.c_str() : "unnamed";
}
float haloscenarioscale()
{
    // Without a loaded program: 2.75, the authored Crash Site's horizontal scale (tools/import_halo_a30.py);
    // the retail Crash Site's scale (4.1, CRASH_SITE in tools/import_halo_campaign.py) is in a30.hscb.
    return scenario.loaded && scenario.projection ? scenario.projection->xy : 2.75f;
}

bool haloscenarioexitpoint(vec &point)
{
    if(!scenario.loaded || !scenario.projection) return false;
    const Projection *next = at(scenario.program.projections, scenario.section + 1);
    if(!next) return false;
    const std::vector<BspSwitch> &switches = scenario.program.switches;
    for(size_t i = 0; i < switches.size(); ++i)
    {
        const BspSwitch &entry = switches[i];
        const Volume *volume = at(scenario.program.volumes, entry.volume);
        if(entry.from != scenario.world || entry.to != next->bsp || !volume) continue;
        // Not the doorway the section was entered by (b40's c1 tower).
        if(scenario.switchArm.size() == switches.size() && scenario.switchArm[i] == 2) continue;
        point = toWorld(*scenario.projection, volumeCenter(*volume));
        return true;
    }
    return false;
}

bool haloscenariotrace(const vec &from, const vec &to, HaloCollisionHit &hit)
{
    if(from == to || !std::isfinite(vec(to).sub(from).squaredlen()) || !std::isfinite(from.squaredlen())) return false;
    const bool structure = halocollisiontrace(from, to, hit);
    return scenario.traceDevices(from, to, hit) || structure;
}

int haloscenariotargettrace(const vec &from, const vec &to, float &fraction)
{
    fraction = 1;
    if(from == to || !std::isfinite(vec(to).sub(from).squaredlen()) || !std::isfinite(from.squaredlen())) return -1;
    return scenario.traceTargets(from, to, fraction);
}

bool haloscenariotargetdamage(int target, float damage) { return scenario.damageTarget(target, damage); }

int haloscenariotargetsnear(const vec &centre, float radius, int *targets, float *distances, int capacity)
{
    if(!std::isfinite(centre.squaredlen()) || !(radius > 0) || capacity <= 0) return 0;
    int found = scenario.targetsNear(centre, radius, targets, distances, capacity), kept = 0;
    // A blast reaches a target it can see: not through a wall or a closed piston (d40's manifolds).
    for(int i = 0; i < found; ++i)
    {
        HaloCollisionHit hit;
        const vec to = scenario.targetCentre(targets[i]);
        if(halocollisionactive() && haloscenariotrace(centre, to, hit) && hit.fraction < .98f) continue;
        targets[kept] = targets[i];
        distances[kept++] = distances[i];
    }
    return kept;
}

bool haloscenariotargetstate(const char *name, vec &centre, float &health, bool &damageable)
{
    if(!scenario.loaded || !scenario.projection || !name) return false;
    const int id = named(scenario.program.objects, name);
    if(id < 0 || scenario.program.objects[id].vitality <= 0) return false;
    const Actor &actor = scenario.actors[id];
    centre = scenario.targetCentre(id);
    health = actor.visible ? max(0.0f, 1 - actor.damage / scenario.program.objects[id].vitality) : -1;
    damageable = actor.visible && !actor.cannotTakeDamage;
    return true;
}

COMMANDF(halo_script_status, "", ()
{
    if(!scenario.loaded) return;
    conoutf("HaloScript: %s section %d tick %d, %zu unbound commands",
            scenario.cache, scenario.section + 1, scenario.tick, scenario.unsupported.size());
    for(const std::string &command : scenario.unsupported) conoutf("HaloScript: unbound %s", command.c_str());
    if(!halo_scripttrace) return;
    // Where each live script is waiting: its name and the call chain of its stack.
    for(size_t i = 0; i < scenario.threads.size(); ++i)
    {
        const auto &thread = scenario.threads[i];
        if(thread.done || thread.disabled || thread.stack.empty()) continue;
        std::string chain;
        // A short rendering of an expression: (head arg arg ...), two levels deep.
        std::function<void(uint32_t, int)> show = [&](uint32_t datum, int depth)
        {
            const auto *node = scenario.program.node(datum);
            if(!node) return;
            if(node->flags & 1) { chain += scenario.program.text(node->text); return; }
            const auto *head = scenario.program.node(node->data);
            if(!head) return;
            chain += "(";
            chain += scenario.program.text(head->text);
            if(depth < 2) for(uint32_t it = head->next; const auto *arg = scenario.program.node(it); it = arg->next)
            {
                chain += " ";
                show(it, depth + 1);
            }
            else chain += " ...";
            chain += ")";
        };
        for(const auto &frame : thread.stack)
        {
            if(!chain.empty()) chain += " > ";
            if(&frame == &thread.stack.back()) show(frame.expression, 0);
            else
            {
                const auto *node = scenario.program.node(frame.expression);
                const auto *head = node && !(node->flags & 1) ? scenario.program.node(node->data) : nullptr;
                if(head) chain += scenario.program.text(head->text);
            }
        }
        conoutf("HaloScript: thread %s waits in %s", scriptName(scenario.program, int(i)), chain.c_str());
    }
});

void haloscenariopausestate(bool paused)
{
    if(paused == scenario.paused) return;
    scenario.paused = paused;
    for(const auto &channel : scenario.channels)
    {
        if(paused) alSourcePause(channel.second->channel.id);
        else if(channel.second->channel.state() == AL_PAUSED) channel.second->channel.play();
    }
}

bool haloscenarioobjectvisible(int object)
{
    const Actor *actor = scenario.loaded ? at(scenario.actors, object) : nullptr;
    return actor && actor->visible;
}

void haloscenarioobjectdestroyed(int object)
{
    // As object_destroy: the object is gone for the scripts (unit_get_health -1, volume tests,
    // object lists), its cargo set down; the vehicle itself already is.
    if(scenario.loaded && at(scenario.actors, object) && scenario.actors[object].visible) scenario.setActor(object, false, false);
}

bool haloscenarioobjectcarried(int object, vec &feet, float &yaw, float *pitch)
{
    if(!scenario.loaded || scenario.cue || !scenario.projection) return false;
    const Actor *actor = at(scenario.actors, object);
    if(!actor || !actor->visible || (actor->parent < 0 && !actor->recording)) return false;
    const Pose pose = scenario.currentActor(object);
    feet = toWorld(*scenario.projection, pose.position);
    yaw = 90 - pose.yaw / RAD;
    if(pitch) *pitch = renderAngles(pose).y;
    return true;
}

bool haloscenariocarried(const playerent *unit)
{
    if(!scenario.loaded || !unit) return false;
    for(const auto &entry : scenario.cargo) if(entry.unit == unit && !entry.walking) return true;
    return false;
}

bool haloscenarioboard() { return scenario.loaded && !scenario.cue && scenario.board(); }

void haloscenariocargodump(const char *label)
{
    fflush(stdout);
    if(!scenario.loaded) { conoutf("CARGO_QA %s loaded=0", label); fflush(stdout); return; }
    const Scenario &sc = scenario;
    conoutf("CARGO_QA %s loaded=1 cargo=%d boarded=%s seat=%d player=%.2f,%.2f,%.2f scripted=%d held=%d", label, int(sc.cargo.size()),
            at(sc.program.objects, sc.boardedVehicle) ? sc.program.objects[sc.boardedVehicle].name.c_str() : "-", sc.boardedSeat,
            player1->o.x, player1->o.y, player1->o.z, int(sc.boardedVehicle != noObject && sc.boardedScripted),
            sc.boardedVehicle != noObject && sc.boardedScripted ? lastmillis - sc.boardedSince : 0);
    for(const auto &entry : sc.cargo)
    {
        const Object &o = sc.program.objects[entry.vehicle];
        conoutf("CARGO_QA %s rider=%s vehicle=%s seat=%s walking=%d pos=%.2f,%.2f,%.2f alive=%d", label, entry.unit ? entry.unit->name : "-",
                o.name.c_str(), at(o.seats, entry.seat) ? o.seats[entry.seat].name.c_str() : "-", entry.walking,
                entry.unit ? entry.unit->o.x : 0.0f, entry.unit ? entry.unit->o.y : 0.0f, entry.unit ? entry.unit->o.z : 0.0f,
                livingUnit(entry.unit));
    }
    for(size_t i = 0; i < sc.actors.size(); ++i)
    {
        const Actor &a = sc.actors[i];
        const Object &o = sc.program.objects[i];
        if(!a.visible || o.kind < 0) continue;
        vec at = sc.projection ? toWorld(*sc.projection, sc.currentActor(int(i)).position) : vec(0, 0, 0);
        conoutf("CARGO_QA %s object=%s kind=%d bound=%d recording=%d flown=%d boardable=%d seats=%d model=%d pos=%.2f,%.2f,%.2f", label,
                o.name.c_str(), o.kind, halovehiclebound(int(i)), a.recording != nullptr, a.recordingEnd > 0, a.boardable,
                int(o.seats.size()), !o.model.empty(), at.x, at.y, at.z);
    }
    fflush(stdout);
}

bool haloscenarioqaboardpoint(const char *object, vec &feet)
{
    if(!scenario.loaded) return false;
    const int id = named(scenario.program.objects, object);
    const Object *o = at(scenario.program.objects, id);
    if(!o || !scenario.scriptVehicle(id)) return false;
    for(size_t i = 0; i < o->seats.size(); ++i)
    {
        if(!seatNamed(o->seats[i], "rider") && !seatNamed(o->seats[i], "passenger")) continue;
        if(!scenario.seatWorld(id, int(i), feet)) continue;
        return true;
    }
    return false;
}
bool haloscenarioboarded() { return scenario.loaded && scenario.boardedVehicle != noObject; }
bool haloscenarioscriptseated() { return haloscenarioboarded() && scenario.boardedScripted; }

// QA (tests/test_ai_attach.py): an ATTACH_QA line per actor ai_attach made of a scripted object,
// one per object attached in play that has none yet (posed, carried or waiting), and a count.
void haloscenarioattachdump(const char *label)
{
    fflush(stdout);
    const Scenario &sc = scenario;
    int count = 0;
    for(size_t i = 0; sc.loaded && i < sc.squadBots.size(); ++i)
    {
        const SquadBot &b = sc.squadBots[i];
        if(b.object < 0) continue;
        ++count;
        const Encounter *e = at(sc.program.encounters, b.encounter);
        const Squad *s = e ? at(e->squads, b.squad) : nullptr;
        const bool alive = b.bot && b.bot->state == CS_ALIVE;
        const vec where = b.bot ? b.bot->o : vec(0, 0, 0);
        conoutf("ATTACH_QA %s object=%s squad=%s/%s alive=%d gone=%d health=%.2f pos=%.2f,%.2f,%.2f distance=%.2f variant=%d drawn=%d",
                label, sc.program.objects[b.object].name.c_str(), e ? e->name.c_str() : "-", s ? s->name.c_str() : "-", alive, b.gone,
                alive ? haloactorhealthfraction(b.bot) : -1.0f, where.x, where.y, where.z, b.bot ? where.dist(player1->o) : -1.0f,
                b.start ? b.start->variant : -1,
                storyFigure(sc.program, size_t(b.object), sc.actors[b.object], true) && sc.actors[b.object].visible);
    }
    for(size_t i = 0; sc.loaded && i < sc.actors.size() && i < sc.attachedAt.size(); ++i)
    {
        const Actor &a = sc.actors[i];
        if(sc.attachedAt[i] < 0 || a.squadReference < 0 || sc.attachedEntry(int(i)) >= 0) continue;
        conoutf("ATTACH_QA %s pending=%s visible=%d posed=%d recording=%d riding=%d drawn=%d", label, sc.program.objects[i].name.c_str(),
                a.visible, a.animation != nullptr, a.recording != nullptr, a.parent >= 0,
                a.visible && storyFigure(sc.program, i, a, false));
    }
    conoutf("ATTACH_QA %s count=%d figures=%d", label, count,
            int(std::count_if(sc.program.objectFlags.begin(), sc.program.objectFlags.end(), [](unsigned char f) { return (f & OBJECT_STORY_FIGURE) != 0; })));
    fflush(stdout);
}

// QA (tests/test_ai_attach.py item 10): the living actor attached to scripted object `object` walks
// (is put) `forward` and `side` world units from where it stands, by its facing; false without one.
bool haloscenarioqaattachmove(const char *object, float forward, float side)
{
    if(!scenario.loaded || !object) return false;
    for(const SquadBot &b : scenario.squadBots)
    {
        if(b.object < 0 || !b.bot || b.bot->state != CS_ALIVE || scenario.program.objects[b.object].name != object) continue;
        const float yaw = b.bot->yaw * RAD, s = haloscenarioscale();
        const vec ahead(sinf(yaw), -cosf(yaw), 0), right(cosf(yaw), sinf(yaw), 0);
        const vec feet = vec(b.bot->o).sub(vec(0, 0, b.bot->eyeheight)).add(vec(ahead).mul(forward * s)).add(vec(right).mul(side * s));
        return haloenemymove(b.bot, feet);
    }
    return false;
}

// QA (tests/test_mission_exits.py): the looping sounds playing, the conversations being spoken and
// those finished, and the gameplay loops a running Call scene will restart.
void haloscenarioaudiodump(const char *label)
{
    fflush(stdout);
    const Scenario &sc = scenario;
    std::string loops, talking, restart;
    for(const auto &item : sc.music) if(!item.ending) loops += (loops.empty() ? "" : ",") + std::to_string(item.id);
    for(const auto &entry : sc.talking) talking += (talking.empty() ? "" : ",") + std::string(sc.program.conversations[entry.id].name);
    for(const auto &item : sc.callMusic) restart += (restart.empty() ? "" : ",") + std::to_string(item.id);
    int finished = 0, playing = 0;
    for(int status : sc.conversationStatus) { finished += status == 5; playing += status == 2; }
    conoutf("AUDIO_QA %s loaded=%d scene=%s loops=[%s] talking=[%s] playing=%d finished=%d restart=[%s]", label, sc.loaded,
            sc.cue ? sc.cue->script : "-", loops.c_str(), talking.c_str(), playing, finished, restart.c_str());
    fflush(stdout);
}

// QA: a conversation's status by name (ai_conversation_status), -1 when the mission has none by that name.
int haloscenarioconversationstatus(const char *name)
{
    if(!scenario.loaded) return -1;
    const int id = named(scenario.program.conversations, name);
    const int *status = at(scenario.conversationStatus, id);
    return status ? *status : -1;
}

const char *haloscenarioboardprompt()
{
    if(!scenario.loaded || scenario.cue || scenario.boardedVehicle != noObject || player1->state != CS_ALIVE) return nullptr;
    int seat;
    const int id = scenario.boardCandidate(seat);
    if(id < 0) return nullptr;
    static string prompt;
    const std::string &name = scenario.program.objects[id].name;
    formatstring(prompt)("E  Board the %s", name.find("pelican") != std::string::npos || name.find("foehammer") != std::string::npos ? "Pelican" : "dropship");
    return prompt;
}
