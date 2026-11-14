#include "cube.h"
#include "halo.h"
#include "halo_campaign.h"
#include "halo_collision.h"
#include "halo_script.h"
#include "halo_ai.h"
#include <algorithm>
#include <cmath>
#include <memory>
#include <random>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

VARP(halo_scripttrace, 0, 0, 1);

namespace
{
    constexpr uint32_t nil = UINT32_MAX;
    constexpr int ticksPerSecond = 30;
    constexpr int noObject = -1, playerObject = -2, playerList = -3;
    // Actors and lists of actors are object values below these bases: an actor
    // is actorObject - its squadBots index, a list is actorList - its actorLists index.
    constexpr int actorObject = -1000, actorList = -100000;

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
    };
    struct Encounter
    {
        std::string name;
        int team, bsp;
        std::vector<Squad> squads;
        std::vector<FiringPosition> positions;
        std::vector<Platoon> platoons;
    };
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
            if(version != 5 && version != 6) return false;
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
            if(!r.valid || r.file->tell() != r.file->size() || nodes.empty() || scripts.empty()) return false;
            if(groupFlags.size() != deviceGroups.size()) return false;
            for(const Script &script : scripts) if(!node(script.root)) return false;
            for(const Global &global : globals) if(!node(global.root)) return false;
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
    };

    struct Playback
    {
        string name;
        sbuffer buffer;
        source channel;
    };

    struct Voice { int id, location, until, scriptUntil; };
    struct Music { int id; const Track *track; int location, until; float gain; bool repeat, ending, alternate; };
    struct Talking { int id, line, wake; };

    enum class SceneTrigger { Entry, Exit, Timeout, Cartographer };
    struct SceneCue { int mission, section, bsp; const char *script; SceneTrigger trigger; };

    // These join authored cinematic scripts to the playable route's entry,
    // interaction and exit boundaries. Sections are zero-based.
    const SceneCue cues[] = {
        {0, 0, 0, "x10", SceneTrigger::Entry}, {0, 1, 1, "x20", SceneTrigger::Exit}, {0, 6, 6, "x30", SceneTrigger::Exit},
        {1, 0, 0, "cutscene_intro", SceneTrigger::Entry}, {1, 1, 0, "cutscene_bridge", SceneTrigger::Exit}, {1, 2, 1, "mission_extraction_river_skip", SceneTrigger::Exit},
        {2, 0, 0, "cutscene_insertion", SceneTrigger::Entry}, {2, 0, 0, "cutscene_energy_lift", SceneTrigger::Exit},
        {2, 3, 3, "cutscene_rescue", SceneTrigger::Exit}, {2, 4, 2, "cutscene_extraction", SceneTrigger::Exit},
        {3, 0, 0, "cutscene_insertion", SceneTrigger::Entry}, {3, 1, 1, "cutscene_map", SceneTrigger::Exit},
        {3, 1, 1, "cutscene_map", SceneTrigger::Cartographer},
        {3, 2, 0, "cutscene_extraction_exit", SceneTrigger::Exit},
        {4, 0, 3, "cutscene_insertion_a", SceneTrigger::Entry}, {4, 6, 2, "cutscene_extraction", SceneTrigger::Exit},
        {5, 0, 0, "insertion", SceneTrigger::Entry}, {5, 4, 3, "x50", SceneTrigger::Exit}, {5, 6, 5, "cutscene_extraction", SceneTrigger::Exit},
        {6, 0, 0, "cinematic_intro", SceneTrigger::Entry}, {6, 3, 3, "cutscene_extraction", SceneTrigger::Exit},
        {7, 0, 2, "cutscene_insertion", SceneTrigger::Entry}, {7, 4, 5, "cutscene_extraction", SceneTrigger::Exit},
        {8, 0, 0, "cinematic_intro", SceneTrigger::Entry}, {8, 2, 2, "cinematic_lift", SceneTrigger::Entry},
        {8, 4, 4, "cinematic_captain", SceneTrigger::Exit}, {8, 5, 3, "cinematic_outro", SceneTrigger::Exit},
        {9, 0, 0, "cinematic_intro", SceneTrigger::Entry}, {9, 4, 4, "cinematic_bridge", SceneTrigger::Entry},
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
        // Encounter combat positions and command lists in this section's coordinates.
        std::vector<std::vector<HaloFiringPosition>> encounterPositions;
        std::vector<HaloCommandProgram> commandPrograms;
        std::vector<int> actorLists;
        std::set<int> maneuvered, flipped;
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
        std::string worldName;
        std::string subtitle;
        std::string objective, help;
        bool showHelp = false;
        int subtitleEnd = 0;
        int savedChapter = -1, savedChapterStart = 0;
        std::string savedObjective, savedHelp;
        bool savedShowHelp = false;
        std::unordered_map<int, Value> savedExternal;
        const char *cache = "";

        void start(int m, int s);
        void stop();
        bool beginCue(SceneTrigger trigger);
        void endCue(bool skipped);
        void step(Thread &thread, bool scene);
        void update(int elapsed);
        void startGameplay();
        void startCartographer();
        void startPelicanApproach();
        bool pelicanReady() const;
        bool cartographerInterior() const;
        bool cartographerMapped() const;
        void placeSquads(int reference, int limit);
        void eraseSquads(int reference);
        int living(int reference) const;
        void restoreSquads();
        bool matchesSquad(int reference, int encounter, int squad) const;
        void prepareAI();
        void configureActor(playerent *bot, int encounter, int squad, const SquadStart &start);
        void updatePlatoons();
        int actorListFor(int reference);
        template<class F> void eachActor(int reference, F f);
        playerent *actorFromValue(int value) const;
        bool objectPosition(int value, vec &position) const;
        Value literal(const Node &node) const;
        Value command(Thread &thread, const Node &node, const char *name, const std::vector<Value> &args, bool scene);
        void wake(int script, bool scene = false);
        void sleep(Thread &thread, const std::vector<Value> &args);
        void complete(Thread &thread, Value result);
        void push(Thread &thread, uint32_t node);
        CameraView currentCamera() const;
        void updateCamera();
        Pose currentActor(int actor, int depth = 0) const;
        bool testVolume(int index, int object = playerObject) const;
        bool switchBsp(int bsp, const char *trigger);
        void updateDevices(bool gameplay);
        void updateRecordings();
        bool setGroup(int index, float value, bool immediate = false);
        float groupValue(int index, float fallback = 0) const;
        bool deviceActive(size_t index) const;
        void renderDevices() const;
        bool collideDevices(physent *entity, float &hi, float &lo) const;
        bool traceDevices(const vec &from, const vec &to, HaloCollisionHit &hit) const;
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
        void setActor(int id, bool visible, bool reset);
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
        for(int budget = 0; budget < 2048 && !thread.stack.empty() && !thread.disabled && thread.wake <= tick; ++budget)
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
                    bool deferred = false;
                    if(script && !scene)
                    {
                        for(const SceneCue &candidate : cues)
                            if(candidate.mission == mission && script->name == candidate.script) deferred = true;
                    }
                    if(deferred && cartographerInterior() && script->name == "cutscene_map")
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
                        if(!condition || (frame.deadline >= 0 && tick >= frame.deadline))
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

    bool Scenario::testVolume(int index, int object) const
    {
        const Volume *volume = at(program.volumes, index);
        if(!volume || !projection) return false;
        vec point;
        if(object == playerObject)
        {
            if(player1->state != CS_ALIVE) return false;
            const Projection &p = *projection;
            point = vec(p.origin.x + (player1->o.x - p.margin) / p.xy,
                        p.origin.y - (player1->o.y - p.margin) / p.xy,
                        p.origin.z + (player1->o.z - .45f * player1->eyeheight) / p.z);
        }
        else
        {
            const Actor *actor = at(actors, object);
            if(!actor || !actor->visible) return false;
            Pose pose = currentActor(object);
            point = pose.position;
            point.add(poseOffset(vec(program.objects[object].center).mul(actor->scale), pose));
        }
        if(volume->kind == 0)
        {
            return point.x > volume->origin.x && point.x < volume->extent.x &&
                   point.y > volume->origin.y && point.y < volume->extent.y &&
                   point.z > volume->origin.z && point.z < volume->extent.z;
        }
        vec local = point.sub(volume->origin);
        vec side;
        side.cross(volume->up, volume->forward);
        float x = local.dot(volume->forward), y = local.dot(side), z = local.dot(volume->up);
        return x > 0 && x < volume->extent.x && y > 0 && y < volume->extent.y && z > 0 && z < volume->extent.z;
    }

    bool Scenario::switchBsp(int bsp, const char *trigger)
    {
        const Projection *next = at(program.projections, section + 1);
        if(!next || !projection || bsp != next->bsp || bsp == projection->bsp) return false;
        if(cartographerInterior() && !cartographerMapped()) return false;
        if(!haloscenariotransition()) return false;
        conoutf("Halo trigger: %s BSP %d to %d", trigger, projection->bsp, bsp);
        return true;
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
            seekDevice(s.power, s.powerVelocity, groupValue(d.powerGroup, s.desiredPower), d.powerAccel, d.powerSpeed);
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
                DeviceBox old = panelBox(d, panel, previous);
                if(!abovePanel(local, old, 0) || fabsf(local.z - old.high.z) > .22f) continue;
                DeviceBox current = panelBox(d, panel, s);
                vec delta = vec(current.high).sub(old.high);
                delta.rotate_around_z(d.pose.yaw);
                delta = vec(delta.x * projection->xy, -delta.y * projection->xy, delta.z * projection->z);
                player1->o.add(delta);
                player1->newpos.add(delta);
                if(halo_scripttrace && previous.positionVelocity == 0)
                    conoutf("Halo lift: carrying player on device %zu", i);
                break;
            }
        }
    }

    bool Scenario::collideDevices(physent *entity, float &hi, float &lo) const
    {
        if(!loaded || cue || editmode || entity->type == ENT_CAMERA) return false;
        const Projection &p = *projection;
        float height = entity->eyeheight + entity->aboveeye;
        float feet = entity->o.z - entity->eyeheight;
        float radius = entity->radius / p.xy;
        for(size_t i = 0; i < devices.size(); ++i)
        {
            if(!deviceActive(i)) continue;
            const Device &d = program.devices[i];
            if(d.control || d.panels.empty() || d.kind == 2) continue;
            vec local = deviceLocal(p, d, entity->o);
            for(const DevicePanel &panel : d.panels)
            {
                if(panel.frames.empty()) continue;
                DeviceBox box = panelBox(d, panel, devices[i]);
                if(!abovePanel(local, box, radius)) continue;
                float bottom = (box.low.z + d.pose.position.z - p.origin.z) * p.z;
                float top = (box.high.z + d.pose.position.z - p.origin.z) * p.z;
                if(feet >= top - .23f * p.z) lo = max(lo, top);
                else if(feet + height <= bottom + .10f * p.z) hi = min(hi, bottom);
                else if(d.elevator != panel.node && top - bottom > .05f * p.z) return true;
                else lo = max(lo, top);
                if(hi - lo < height) return true;
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
            if(!deviceActive(i)) continue;
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
        channel.gain(clamp(gain, 0.0f, 1.0f));
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
                                 gain * track.gain, repeat, false, false});
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

    void Scenario::setActor(int id, bool visible, bool reset)
    {
        if(!at(actors, id)) return;
        if(reset)
        {
            actors[id] = {};
            actors[id].pose = program.objects[id].pose;
        }
        actors[id].visible = visible;
        int device = objectDevices[id];
        if(device >= 0) devices[device].visible = visible;
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
            if(!scene) placeSquads(index(0), is("ai_spawn_actor") ? 1 : index(1));
            return {};
        }
        if(is("ai_kill") || is("ai_kill_silent") || is("ai_erase"))
        {
            if(!scene) eraseSquads(index(0));
            return {};
        }
        if(is("ai_erase_all"))
        {
            if(!scene) for(SquadBot &bot : squadBots) if(bot.bot && bot.bot->state == CS_ALIVE) dodamage(INT_MAX, bot.bot, player1, GUN_ASSAULT);
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
            // 0 no list, 1 still running, 2 finished, across the given units.
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
            return result(p && p->state == CS_ALIVE ? haloactorhealthfraction(p) : -1.0f);
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
        // Vehicles are not driven by actors yet. These stand-ins let the waits that
        // depend on boarding move on instead of hanging.
        if(is("vehicle_test_seat_list")) return result(true);
        if(is("ai_going_to_vehicle")) return result(0);
        if(is("ai_go_to_vehicle") || is("ai_allegiance") || is("ai_vehicle_encounter") || is("ai_vehicle_enterable_distance"))
            return {};
        if(is("ai_migrate"))
        {
            int oldGroup = index(0), newGroup = index(1);
            if(!scene && newGroup != -1) for(SquadBot &bot : squadBots)
            {
                if(!bot.bot || !matchesSquad(oldGroup, bot.encounter, bot.squad)) continue;
                bot.encounter = newGroup & 0xffff;
                const unsigned kind = uint32_t(newGroup) >> 30;
                if(kind == 2) bot.squad = (uint32_t(newGroup) >> 16) & 0xff;
                // The migrated actors take the target squad's positions and walk to them.
                const Encounter *encounter = at(program.encounters, bot.encounter);
                const Squad *squad = encounter ? at(encounter->squads, bot.squad) : nullptr;
                static const unsigned anyGroup[7] = {};
                if(bot.encounter < int(encounterPositions.size()))
                    haloactorsetpositions(bot.bot, &encounterPositions[bot.encounter], squad ? squad->masks : anyGroup);
            }
            return {};
        }
        if(is("random_range") || is("real_random_range"))
        {
            double lower = value(0), upper = value(1);
            if(upper <= lower) return result(lower);
            double selected = std::uniform_real_distribution<double>(lower, upper)(random);
            return result(is("random_range") ? floor(selected) : selected);
        }
        if(is("game_difficulty_get") || is("game_difficulty_get_real")) return result(1);
        if(is("game_is_cooperative") || is("game_saving") || is("game_reverted")) return result(0);
        if(is("game_safe_to_speak") || is("game_all_quiet")) return result(1);
        if(is("cinematic_skip_stop_internal")) return result(1);
        if(is("volume_test_object")) return result(testVolume(index(0), object(1)));
        if(is("volume_test_objects") || is("volume_test_objects_all") || is("volume_test_objects_any"))
            return result(index(1) == playerList && testVolume(index(0)));
        if(is("structure_bsp_index")) return result(scene ? world : projection ? projection->bsp : -1);
        if(is("switch_bsp"))
        {
            if(scene) setWorld(index(0));
            else if(runningGameplay) switchBsp(index(0), "switch_bsp");
            return {};
        }
        if(is("game_save") || is("game_save_no_timeout") || is("game_save_totally_unsafe"))
        {
            if((!scene || (cue && cue->trigger == SceneTrigger::Cartographer)) && !is("game_save_totally_unsafe"))
                pendingCheckpoint = true;
            return {};
        }
        if(is("game_won") || is("game_lost")) return {};
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
        if(is("sound_looping_stop")) { stopMusic(index(0)); return {}; }
        if(is("sound_looping_set_alternate")) { setAlternate(index(0), truth(1)); return {}; }
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
            for(size_t i = 0; i < actors.size(); ++i) setActor(int(i), false, true);
            return {};
        }
        if(is("object_create_containing") || is("object_create_anew_containing") || is("object_destroy_containing"))
        {
            if(*text(0)) for(size_t i = 0; i < actors.size(); ++i)
                if(program.objects[i].name.find(text(0)) != std::string::npos)
                    setActor(int(i), !is("object_destroy_containing"), true);
            return {};
        }
        if(is("object_teleport") || is("object_set_facing"))
        {
            const Flag *flag = at(program.flags, index(1));
            int id = object(0);
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
        if(is("print") || is("cls") || is("object_pvs_set_camera") || is("object_pvs_clear") ||
           is("object_beautify") || is("objects_predict") || is("object_type_predict")) return {};

        if(*name) unsupported.insert(name);
        return result(0);
    }

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

    // Encounters on the default team take the side of their actors: marines and
    // crew fight for the player. Teams 1 and 2 are the player's side explicitly.
    int teamFor(const Encounter &encounter, const SquadStart &start)
    {
        if(encounter.team == 1 || encounter.team == 2) return HALO_TEAM_HUMAN;
        const HaloActorDef *def = haloactordef(start.variant);
        return def && def->kind == 9 ? HALO_TEAM_HUMAN : HALO_TEAM_COVENANT;
    }

    void Scenario::placeSquads(int reference, int limit)
    {
        int id = reference & 0xffff;
        const Encounter *encounter = at(program.encounters, id);
        if(!encounter || !projection || (encounter->bsp >= 0 && encounter->bsp != projection->bsp)) return;
        const Projection &p = *projection;
        int count = 0;
        for(size_t i = 0; i < encounter->squads.size(); ++i)
        {
            if(!matchesSquad(reference, id, int(i))) continue;
            for(const SquadStart &start : encounter->squads[i].starts)
            {
                if(squadBots.size() >= 128 || (limit > 0 && count >= limit)) break;
                if(start.actor < 0 && !haloactordef(start.variant)) continue;
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
        if(count) conoutf("HaloScript: placed %d actors from %s", count, encounter->name.c_str());
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

    playerent *Scenario::actorFromValue(int value) const
    {
        if(value == playerObject) return player1;
        if(value > actorObject || value <= actorList) return nullptr;
        const SquadBot *bot = at(squadBots, actorObject - value);
        return bot ? bot->bot : nullptr;
    }

    int Scenario::living(int reference) const
    {
        int count = 0;
        for(const SquadBot &bot : squadBots)
            if(bot.bot && bot.bot->state == CS_ALIVE && matchesSquad(reference, bot.encounter, bot.squad)) ++count;
        return count;
    }

    void Scenario::eraseSquads(int reference)
    {
        for(SquadBot &bot : squadBots)
            if(bot.bot && bot.bot->state == CS_ALIVE && matchesSquad(reference, bot.encounter, bot.squad))
                dodamage(INT_MAX, bot.bot, player1, GUN_ASSAULT);
    }

    void Scenario::restoreSquads()
    {
        squadBots.clear();
        if(!projection) return;
        const Projection &p = *projection;
        for(SquadBot saved : savedSquadBots)
        {
            const SquadStart &start = *saved.start;
            vec point((start.point.x - p.origin.x) * p.xy + p.margin,
                      (p.origin.y - start.point.y) * p.xy + p.margin,
                      (start.point.z - p.origin.z) * p.z);
            const Encounter *encounter = at(program.encounters, saved.encounter);
            const int team = encounter ? teamFor(*encounter, start) : HALO_TEAM_COVENANT;
            saved.bot = haloscenariospawnenemy(start.actor, point, 90 - start.yaw / RAD, start.variant, team);
            if(saved.bot)
            {
                squadBots.push_back(saved);
                configureActor(saved.bot, saved.encounter, saved.squad, start);
            }
        }
    }

    void Scenario::stop()
    {
        stopSounds();
        loaded = false;
        cue = nullptr;
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

    void Scenario::startPelicanApproach()
    {
        if(mission != 3 || section != 2 || !halocollisionactive()) return;
        const int id = named(program.objects, "extraction_pelican");
        const Flag *entry = at(program.flags, named(program.flags, "extraction_pelican_flag_1"));
        const Recording *approach = at(program.recordings, named(program.recordings, "extraction_pelican_1"));
        if(!at(actors, id) || !entry || !approach || approach->inputs.empty()) return;
        setActor(id, true, true);
        Actor &pelican = actors[id];
        pelican.pose = entry->pose;
        pelican.recording = approach;
        pelican.recordingStart = tick;
        pelican.recordingEnd = tick + approach->ticks;
        pelican.recordingHover = true;
        // The outdoor route boards with the normal interact action.
        const int goal = named(program.scripts, "obj_shafta_goal");
        if(goal >= 0) threads[goal].disabled = true;
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
        stop();
        cache = haloMissions[m].cache;
        if(mission != m || program.scripts.empty())
        {
            program = {};
            if(!program.load(cache))
            {
                program = {};
                conoutf("Halo: no retail scenario program for %s", cache);
                return;
            }
        }
        mission = m;
        section = s;
        projection = at(program.projections, s);
        if(!projection) return;
        prepareAI();
        if(continuing)
        {
            world = projection->bsp;
            exitPlayed = completeExit = failedEscape = pendingCheckpoint = false;
            savedThreads.clear();
            savedActors.clear();
            savedSquadBots.clear();
            squadBots.clear();
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
        exitPlayed = completeExit = failedEscape = runningGameplay = pendingCheckpoint = false;
        director = {};
        threads.assign(program.scripts.size(), {});
        globals.assign(program.globals.size(), {});
        actors.assign(program.objects.size(), {});
        conversationStatus.assign(program.conversations.size(), 0);
        savedGlobals.clear();
        savedThreads.clear();
        savedActors.clear();
        savedSquadBots.clear();
        squadBots.clear();
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
        objective.clear();
        help.clear();
        showHelp = false;
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

    bool Scenario::beginCue(SceneTrigger trigger)
    {
        if(!loaded || cue || (trigger == SceneTrigger::Exit && exitPlayed)) return false;
        if(trigger == SceneTrigger::Exit && cartographerInterior()) return false;
        for(const SceneCue &candidate : cues)
        {
            if(candidate.mission != mission || (candidate.section >= 0 && candidate.section != section) || candidate.trigger != trigger) continue;
            int index = named(program.scripts, candidate.script);
            if(index < 0) continue;
            const int boardedPelican = pelicanReady() ? named(program.objects, "extraction_pelican") : -1;
            stopSounds();
            cue = &candidate;
            director = {};
            sceneThreads.clear();
            director.stack.emplace_back(program.scripts[index].root);
            chapter = -1;
            nearClip = .0625f;
            fade = {vec(), 1, 1, tick, tick};
            letterbox = cameraControl = true;
            camera = {};
            for(size_t i = 0; i < actors.size(); ++i)
            {
                if(int(i) == boardedPelican) continue;
                actors[i] = {};
                actors[i].pose = program.objects[i].pose;
            }
            setWorld(candidate.bsp);
            player1->attacking = false;
            player1->stopmoving();
            conoutf("Halo scene: %s start", candidate.script);
            return true;
        }
        return false;
    }

    void Scenario::endCue(bool skipped)
    {
        if(!cue) return;
        conoutf("Halo scene: %s %s at tick %d", cue->script, skipped ? "skipped" : "finished", tick);
        const SceneTrigger trigger = cue->trigger;
        cue = nullptr;
        worldName.clear();
        world = projection->bsp;
        director = {};
        sceneThreads.clear();
        camera = {};
        fade = {};
        letterbox = cameraControl = false;
        stopSounds();
        player1->attacking = false;
        player1->stopmoving();
        resetcamera();
        if(trigger == SceneTrigger::Cartographer)
        {
            // The scene's original handoff places the player next to the terminal.
            const Flag *done = at(program.flags, named(program.flags, "player0_map_done"));
            if(done)
            {
                const vec old = player1->o;
                player1->o = toWorld(*projection, done->pose.position).add(vec(0, 0, player1->eyeheight));
                if(collide(player1, true)) player1->o = old;
                else player1->yaw = fmodf(450.0f - done->pose.yaw / RAD, 360.0f);
                player1->resetinterp();
            }
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
    }

    void Scenario::update(int elapsed)
    {
        if(!loaded || elapsed <= 0 || paused) return;
        accumulator += clamp(elapsed, 0, 250) * ticksPerSecond;
        while(accumulator >= 1000)
        {
            accumulator -= 1000;
            ++tick;
            updateRecordings();
            updateDevices(!cue && player1->state == CS_ALIVE && !intermission);
            if(!cue && tick % 15 == 0) updatePlatoons();
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
                for(const BspSwitch &entry : program.switches)
                    if(entry.from == world && testVolume(entry.volume) &&
                       switchBsp(entry.to, program.volumes[entry.volume].name.c_str())) break;
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

    void Scenario::render()
    {
        renderDevices();
        const int landingPelican = !cue && mission == 3 && section == 2 && halocollisionactive()
            ? named(program.objects, "extraction_pelican") : -1;
        if(!cue && landingPelican < 0) return;
        for(size_t i = 0; i < actors.size(); ++i)
        {
            if(!cue && int(i) != landingPelican) continue;
            const Actor &actor = actors[i];
            const Object &object = program.objects[i];
            if(!actor.visible || object.model.empty() || objectDevices[i] >= 0) continue;
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
    if(!preview || halocollisionactive()) scenario.start(mission, section);
}

void haloscenariostop() { scenario.stop(); }

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

void haloscenarioremovebot(const playerent *bot)
{
    for(SquadBot &member : scenario.squadBots) if(member.bot == bot) member.bot = nullptr;
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
        if(bot.bot && bot.bot->state == CS_ALIVE) scenario.savedSquadBots.push_back(bot);
    scenario.savedGroups = scenario.groups;
    scenario.savedDevices = scenario.devices;
    scenario.savedExternal = scenario.external;
    scenario.savedObjective = scenario.objective;
    scenario.savedHelp = scenario.help;
    scenario.savedShowHelp = scenario.showHelp;
    scenario.savedChapter = scenario.chapter;
    scenario.savedChapterStart = scenario.chapterStart;
    scenario.savedTick = scenario.tick;
}

void haloscenariorevert()
{
    if(!scenario.loaded) return;
    scenario.stopSounds();
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
        scenario.chapter = scenario.savedChapter;
        scenario.chapterStart = scenario.savedChapterStart;
        scenario.restoreSquads();
        scenario.tick = scenario.savedTick;
        scenario.accumulator = 0;
        std::fill(scenario.conversationStatus.begin(), scenario.conversationStatus.end(), 5);
    }
    else
    {
        const int m = scenario.mission, s = scenario.section;
        scenario.start(m, s);
        if(scenario.cue) scenario.endCue(true);
    }
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
float haloscenarioscale()
{
    // The standalone a30 map has no projection; tools/import_halo_a30.py uses HORIZONTAL_SCALE 2.75.
    return scenario.loaded && scenario.projection ? scenario.projection->xy : 2.75f;
}

bool haloscenarioexitpoint(vec &point)
{
    if(!scenario.loaded || !scenario.projection) return false;
    const Projection *next = at(scenario.program.projections, scenario.section + 1);
    if(!next) return false;
    for(const BspSwitch &entry : scenario.program.switches)
    {
        const Volume *volume = at(scenario.program.volumes, entry.volume);
        if(entry.from != scenario.world || entry.to != next->bsp || !volume) continue;
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

COMMANDF(halo_script_status, "", ()
{
    if(!scenario.loaded) return;
    conoutf("HaloScript: %s section %d tick %d, %zu unbound commands",
            scenario.cache, scenario.section + 1, scenario.tick, scenario.unsupported.size());
    for(const std::string &command : scenario.unsupported) conoutf("HaloScript: unbound %s", command.c_str());
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
