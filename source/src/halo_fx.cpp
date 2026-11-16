// Presentation for the campaign: script effects, camera shake, cinematic screen effects, the
// flashlight, the damage-direction indicator and the retail sound classes' gains. See halo_fx.h
// and docs/macos-build.md (Presentation). None of it changes the game's state.
#include "cube.h"
#include "halo.h"
#include "halo_fx.h"
#include "halo_script.h"
#include "halo_weapons.h"
#include "halo_world.h"
#include <cmath>
#include <map>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

// Camera shake (the scripts' player_effect_*, vehicle crashes, nearby blasts).
VARP(halo_shake, 0, 1, 1);
// The cinematic screen effects (blur, desaturation and tint, the video look).
VARP(halo_screeneffects, 0, 1, 1);
// The damage-direction indicator.
VARP(halo_damageindicator, 0, 1, 1);
// Logs every script effect and sound class fade.
VAR(halo_fxtrace, 0, 0, 1);

namespace
{
    constexpr int formatVersion = 1;            // tools/halo_effects.py FORMAT
    enum { KIND_EXPLOSION, KIND_BURST, KIND_SMOKE, KIND_GLOW };

    struct EffectDef
    {
        std::string path, sound;
        int kind = KIND_GLOW;
        float scale = 1, lightRadius = 0, lightSeconds = 0, blast = 0;
        vec light = vec(1, 1, 1);
        int systems = 0, particles = 0;
        // The strongest damaging jpt! part (`damage` and `materials` lines), when it has one.
        bool hasDamage = false;
        HaloEffectDamage damage;
    };

    // A value easing linearly from `from` to `to` between two times (lastmillis).
    struct Ramp
    {
        float from = 0, to = 0;
        int start = 0, end = 0;
        float value() const
        {
            if(lastmillis >= end || end <= start) return to;
            if(lastmillis <= start) return from;
            return from + (to - from) * float(lastmillis - start) / float(end - start);
        }
        void set(float target, float seconds) { from = value(); go(from, target, seconds); }
        void go(float a, float b, float seconds)
        {
            from = a;
            to = b;
            start = lastmillis;
            end = lastmillis + max(0, int(seconds * 1000));
        }
        void hold(float v) { from = to = v; start = end = lastmillis; }
    };

    std::vector<EffectDef> effects;
    // The jpt!s the mission scripts name (`jpt` records: damage_object, damage_new).
    struct JptDef
    {
        std::string path;
        bool valid = false;
        HaloEffectDamage damage;
    };
    std::vector<JptDef> jpts;
    bool effectsTried = false, effectsLoaded = false;
    // The cyborg's flashlight light (tools/halo_effects.py): world units and radians.
    float flashRadius = 6, flashFalloff = .349f, flashCutoff = .785f;
    vec flashColour(1, 1, 1);
    // Sound classes: each cache's `classes` line, parsed when the cache is first asked about.
    std::map<std::string, std::string> classLines;
    std::map<std::string, std::vector<signed char>> cacheClasses;
    std::unordered_map<std::string, int> sampleClasses;
    bool samplesTried = false;
    Ramp classGain[HALO_SOUND_CLASSES];
    bool gainsReady = false;
    const char *const classNames[HALO_SOUND_CLASSES] = {
        "projectile_impact", "projectile_detonation", "", "", "weapon_fire", "weapon_ready", "weapon_reload",
        "weapon_empty", "weapon_charge", "weapon_overheat", "weapon_idle", "", "", "object_impacts", "particle_impacts",
        "slow_particle_impacts", "", "", "unit_footsteps", "unit_dialog", "", "", "vehicle_collision", "vehicle_engine",
        "", "", "device_door", "device_force_field", "device_machinery", "device_nature", "device_computers", "",
        "music", "ambient_nature", "ambient_machinery", "ambient_computers", "", "", "", "first_person_damage", "", "",
        "", "", "scripted_dialog_player", "scripted_effect", "scripted_dialog_other",
        "scripted_dialog_force_unspatialized", "", "", "game_event",
    };

    // Camera shake.
    vec shakeTranslation(0, 0, 0), shakeRotation(0, 0, 0);
    float rumbleLeft = 0, rumbleRight = 0;
    Ramp shake;
    float impulse = 0;
    int impulseStart = 0, impulseEnd = 0;

    // Cinematic screen effects.
    bool screenOn = false;
    Ramp blur, desaturation, tintAmount;
    int blurIterations = 1, videoMode = 0;
    float videoIntensity = 0;
    vec tint(1, 1, 1);
    // The blur's copy of the frame: `screenW`x`screenH` copied into a texture of `texW`x`texH` (the
    // next powers of two where the driver lacks non-power-of-two textures). Recreated when the
    // viewport changes size and after a GL context rebuild (halofxcleanupgl, glIsTexture).
    GLuint screenTexture = 0;
    int screenW = 0, screenH = 0, texW = 0, texH = 0;
    int npotTextures = -1;                  // -1 not asked yet in this context
    int screenRebuilds = 0, spotRebuilds = 0;   // QA (halofxdump)

    // The flashlight.
    bool flashOn = false;
    float flashCharge = 1;
    int flashLastLight = 0;
    GLuint spotTexture = 0;
    constexpr float flashDrainSeconds = 60, flashRechargeSeconds = 30;

    // The damage-direction indicator.
    struct Hit { vec from; int at = 0; float strength = 0; bool shield = false; };
    Hit hits[8];
    constexpr int hitMillis = 1000;

    // QA counters.
    int effectCount = 0, effectUnknown = 0, effectVisual = 0;
    std::string lastEffect = "-";

    void resetGains()
    {
        loopi(HALO_SOUND_CLASSES) classGain[i].hold(1);
        gainsReady = true;
    }

    bool loadEffects()
    {
        if(effectsTried) return effectsLoaded;
        effectsTried = true;
        int size = 0;
        char *text = loadfile(path("packages/halo/effects.txt", true), &size);
        if(!text)
        {
            conoutf("\f3Halo effects: packages/halo/effects.txt is missing; run tools/halo_effects.py");
            return false;
        }
        std::istringstream lines(text);
        delete[] text;
        std::string line;
        EffectDef *def = nullptr;
        JptDef *jpt = nullptr;
        bool versioned = false;
        while(std::getline(lines, line))
        {
            if(line.empty() || line[0] == '#') continue;
            std::istringstream in(line);
            std::string key;
            in >> key;
            if(key == "format") { int v = 0; in >> v; versioned = v == formatVersion; continue; }
            if(key == "flashlight")
            {
                float radius, falloff, cutoff, r, g, b;
                if(in >> radius >> falloff >> cutoff >> r >> g >> b && radius > 0 && cutoff > 0 && cutoff < 1.5f)
                {
                    flashRadius = radius;
                    flashFalloff = clamp(falloff, 0.0f, cutoff);
                    flashCutoff = cutoff;
                    flashColour = vec(r, g, b);
                }
                continue;
            }
            if(key == "classes")
            {
                std::string cache;
                in >> cache;
                std::string rest;
                std::getline(in, rest);
                classLines[cache] = rest;
                continue;
            }
            if(key == "effect") { effects.emplace_back(); def = &effects.back(); jpt = nullptr; continue; }
            // A `jpt` record (after every effect: older games skip its lines, having no effect open):
            // path, then `jptdamage <inner> <outer> <lower> <upper min> <upper max> <side effect>
            // <category> <core> <damage flags> <effect flags>` (world units, points; zero damage
            // kept), then `materials`.
            if(key == "jpt") { jpts.emplace_back(); jpt = &jpts.back(); def = nullptr; continue; }
            if(jpt)
            {
                if(key == "path") std::getline(in >> std::ws, jpt->path);
                else if(key == "jptdamage")
                {
                    HaloEffectDamage &d = jpt->damage;
                    float lower, low, high, core;
                    unsigned long long flags = 0, effectFlags = 0;
                    if(in >> d.inner >> d.outer >> lower >> low >> high >> d.sideEffect >> d.category >> core >> flags >> effectFlags &&
                       std::isfinite(d.inner) && std::isfinite(d.outer) && std::isfinite(lower) && std::isfinite(low) &&
                       std::isfinite(high) && std::isfinite(core) && d.outer >= 0 && d.outer < 1000 && high >= 0 && high < 100000 &&
                       flags <= 0xffffffffull && effectFlags <= 0xffffffffull)
                    {
                        d.inner = clamp(d.inner, 0.0f, d.outer);
                        d.damage.upperMax = high;
                        d.damage.upperMin = clamp(low, 0.0f, high);
                        d.damage.lower = clamp(lower, 0.0f, high);
                        d.damage.core = clamp(core, 0.0f, d.outer);
                        d.damage.category = d.category;
                        d.damage.flags = unsigned(flags);
                        d.effectFlags = unsigned(effectFlags);
                        jpt->valid = true;
                    }
                }
                else if(key == "materials")
                {
                    HaloDamage &d = jpt->damage.damage;
                    int read = 0;
                    while(read < HALO_DAMAGE_MATERIALS && in >> d.materials[read])
                    {
                        if(!std::isfinite(d.materials[read])) break;
                        d.materials[read] = clamp(d.materials[read], 0.0f, 100.0f);
                        ++read;
                    }
                    d.hasMaterials = read == HALO_DAMAGE_MATERIALS;
                }
                else if(key == "end") jpt = nullptr;
                continue;
            }
            if(!def) continue;
            if(key == "path") std::getline(in >> std::ws, def->path);
            else if(key == "kind")
            {
                std::string kind;
                in >> kind >> def->scale;
                def->scale = clamp(def->scale, .2f, 3.0f);
                def->kind = kind == "explosion" ? KIND_EXPLOSION : kind == "burst" ? KIND_BURST : kind == "smoke" ? KIND_SMOKE : KIND_GLOW;
            }
            else if(key == "light") in >> def->lightRadius >> def->light.x >> def->light.y >> def->light.z >> def->lightSeconds;
            else if(key == "blast") in >> def->blast;
            else if(key == "damage")
            {
                // inner outer lower upper-min upper-max side-effect category core (world units)
                HaloEffectDamage &d = def->damage;
                float lower, low, high, core;
                if(in >> d.inner >> d.outer >> lower >> low >> high >> d.sideEffect >> d.category >> core &&
                   std::isfinite(d.inner) && std::isfinite(d.outer) && std::isfinite(lower) && std::isfinite(low) &&
                   std::isfinite(high) && std::isfinite(core) && d.outer > 0 && d.outer < 1000 && high > 0 && high < 100000)
                {
                    d.inner = clamp(d.inner, 0.0f, d.outer);
                    d.damage.upperMax = high;
                    d.damage.upperMin = clamp(low, 0.0f, high);
                    d.damage.lower = clamp(lower, 0.0f, high);
                    d.damage.core = clamp(core, 0.0f, d.outer);
                    def->hasDamage = true;
                }
            }
            else if(key == "materials")
            {
                HaloDamage &d = def->damage.damage;
                int read = 0;
                while(read < HALO_DAMAGE_MATERIALS && in >> d.materials[read])
                {
                    if(!std::isfinite(d.materials[read])) break;
                    d.materials[read] = clamp(d.materials[read], 0.0f, 100.0f);
                    ++read;
                }
                d.hasMaterials = read == HALO_DAMAGE_MATERIALS;
            }
            else if(key == "particles") in >> def->systems >> def->particles;
            else if(key == "sound")
            {
                std::string role, tag;
                in >> role;
                std::getline(in >> std::ws, tag);
                if(role == "play" && !tag.empty()) def->sound = halosoundsample(tag);
            }
            else if(key == "end")
            {
                // Out-of-range numbers (a damaged table) would overflow the int conversions below.
                def->lightRadius = std::isfinite(def->lightRadius) ? clamp(def->lightRadius, 0.0f, 1000.0f) : 0;
                def->lightSeconds = std::isfinite(def->lightSeconds) ? clamp(def->lightSeconds, 0.0f, 60.0f) : 0;
                def->blast = std::isfinite(def->blast) ? clamp(def->blast, 0.0f, 1000.0f) : 0;
                def = nullptr;
            }
        }
        if(!versioned)
        {
            conoutf("\f3Halo effects: packages/halo/effects.txt is not format %d; re-run tools/halo_effects.py", formatVersion);
            effects.clear();
            jpts.clear();
            classLines.clear();
            return false;
        }
        effectsLoaded = true;
        conoutf("Halo effects: %d script effects, %d script jpt!s, sound classes for %d caches", int(effects.size()), int(jpts.size()),
                int(classLines.size()));
        return true;
    }

    const EffectDef *findEffect(const char *tag)
    {
        if(!tag || !*tag || !loadEffects()) return nullptr;
        for(const EffectDef &e : effects) if(!strcasecmp(e.path.c_str(), tag)) return &e;
        return nullptr;
    }

    // Smooth noise in -1..1 from the time (no rnd(): the renderer must not draw on the game's
    // random sequence).
    float noise(float t, float a, float b, float phase)
    {
        return (sinf(t * a + phase) + .6f * sinf(t * b + phase * 1.7f)) / 1.6f;
    }

    float hudAngle(const vec &from)
    {
        // As halodrawhud bears on its objective: 0 ahead, positive to the right.
        vec bearing = vec(from).sub(player1->o);
        bearing.rotate_around_z(-player1->yaw * RAD);
        return atan2f(bearing.x, -bearing.y);
    }

    void drawQuad(float x, float y, float w, float h)
    {
        glBegin(GL_TRIANGLE_STRIP);
        glVertex2f(x, y); glVertex2f(x + w, y); glVertex2f(x, y + h); glVertex2f(x + w, y + h);
        glEnd();
    }

    // Whether this GL context takes non-power-of-two textures (OpenGL 2.0, or the ARB extension).
    bool npot()
    {
        if(npotTextures < 0)
        {
            const char *version = (const char *)glGetString(GL_VERSION), *extensions = (const char *)glGetString(GL_EXTENSIONS);
            npotTextures = (version && atoi(version) >= 2) || (extensions && strstr(extensions, "GL_ARB_texture_non_power_of_two")) ? 1 : 0;
        }
        return npotTextures > 0;
    }

    int powerOfTwo(int n)
    {
        int p = 1;
        while(p < n && p < (1 << 14)) p <<= 1;
        return p;
    }

    // A texture name that is no longer one in the current context (the context was rebuilt without
    // halofxcleanupgl, or the driver dropped it) is forgotten, so the caller makes it anew.
    bool stale(GLuint &id)
    {
        if(!id || glIsTexture(id)) return false;
        id = 0;
        return true;
    }

    GLuint makeSpot()
    {
        constexpr int size = 64;
        unsigned char pixels[size * size];
        const float inner = tanf(flashFalloff) / max(tanf(flashCutoff), 1e-3f);
        loop(y, size) loop(x, size)
        {
            const float dx = (x + .5f) / size * 2 - 1, dy = (y + .5f) / size * 2 - 1;
            const float r = sqrtf(dx * dx + dy * dy);
            float v = r <= inner ? 1 : r >= 1 ? 0 : 1 - (r - inner) / max(1 - inner, 1e-3f);
            v = v * v * (3 - 2 * v);
            if(x == 0 || y == 0 || x == size - 1 || y == size - 1) v = 0;
            pixels[y * size + x] = (unsigned char)(clamp(v, 0.0f, 1.0f) * 255);
        }
        GLuint id = 0;
        glGenTextures(1, &id);
        glBindTexture(GL_TEXTURE_2D, id);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, size, size, 0, GL_LUMINANCE, GL_UNSIGNED_BYTE, pixels);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        return id;
    }
}

void halofxreset()
{
    resetGains();
    shakeTranslation = shakeRotation = vec(0, 0, 0);
    rumbleLeft = rumbleRight = 0;
    shake.hold(0);
    impulse = 0;
    impulseStart = impulseEnd = 0;
    halofxscreenstop();
    flashOn = false;
    flashCharge = 1;
    flashLastLight = 0;
    loopi(8) hits[i] = Hit();
}

// The reverted script runs again from the checkpoint and starts its shakes and screen effects
// anew; the ones running at the death must not outlive it (the sound class gains stay: a fade the
// checkpoint came after is not run again).
void halofxrevert()
{
    shake.hold(0);
    impulse = 0;
    impulseStart = impulseEnd = 0;
    halofxscreenstop();
    loopi(8) hits[i] = Hit();
}

void halofxupdate(int elapsed)
{
    if(elapsed <= 0) return;
    if(flashOn && halocinematic()) return;      // held through a scene (the light is not drawn there)
    if(flashOn)
    {
        flashCharge -= elapsed / (flashDrainSeconds * 1000);
        if(flashCharge <= 0 || player1->state != CS_ALIVE) { flashCharge = max(flashCharge, 0.0f); flashOn = false; }
    }
    else flashCharge = min(1.0f, flashCharge + elapsed / (flashRechargeSeconds * 1000));
    // Stock geometry (no retail mesh) takes AssaultCube's dynamic lights: one a little ahead of the
    // eye, renewed while the flashlight is on.
    if(flashOn && !haloworldready() && lastmillis - flashLastLight >= 80)
    {
        flashLastLight = lastmillis;
        const float yaw = player1->yaw * RAD, pitch = player1->pitch * RAD;
        const vec ahead = vec(sinf(yaw) * cosf(pitch), -cosf(yaw) * cosf(pitch), sinf(pitch)).mul(5).add(player1->o);
        const int reach = clamp(int(flashRadius * max(haloscenarioscale(), 1.0f) * .6f), 8, 32);
        adddynlight(player1, ahead, reach, 120, 40, uchar(200 * flashColour.x), uchar(200 * flashColour.y), uchar(190 * flashColour.z));
    }
}

// ---- Effects

bool halofxeffect(const char *tag, const vec &point, bool visual)
{
    const EffectDef *def = findEffect(tag);
    EffectDef fallback;
    if(!def)
    {
        fallback.path = tag ? tag : "";
        fallback.kind = tag && strstr(tag, "explosion") ? KIND_EXPLOSION : KIND_BURST;
        fallback.sound = fallback.kind == KIND_EXPLOSION ? "frag-explode" : "";
        fallback.lightRadius = 10;
        fallback.light = vec(1, .8f, .2f);
        fallback.lightSeconds = 1;
        def = &fallback;
        ++effectUnknown;
    }
    ++effectCount;
    lastEffect = def->path.empty() ? "-" : def->path;
    if(halo_fxtrace) conoutf("Halo effect: %s at %.1f %.1f %.1f%s", lastEffect.c_str(), point.x, point.y, point.z, visual ? "" : " (sound only)");
    if(!def->sound.empty()) haloplaysound(def->sound.c_str(), S_NULL, NULL, visual ? &point : NULL);
    if(!visual) return def != &fallback;
    ++effectVisual;
    const float scale = max(haloscenarioscale(), .5f), s = def->scale;
    const int angle = (effectCount * 47) % 360;
    switch(def->kind)
    {
        case KIND_EXPLOSION:
        {
            particle_fireball(PART_FIREBALL, point);
            // The large explosions' particle systems: two more fireballs about the centre.
            if(s >= 1.5f) loopi(2)
            {
                const float a = (angle + i * 160) * RAD;
                particle_fireball(PART_FIREBALL, vec(point).add(vec(cosf(a) * 2.4f, sinf(a) * 2.4f, 1.2f + i)));
            }
            particle_splash(PART_SPARK, int(40 * s), 280, point);
            particle_splash(PART_SMOKE, int(14 * s), 900, point);
            particle_flash(PART_MUZZLEFLASH, 3.5f * s, float(angle), point);
            halofxblast(point, max(def->blast, 2.5f) * scale);
            break;
        }
        case KIND_BURST:
            particle_splash(PART_SPARK, int(24 * s), 220, point);
            particle_flash(PART_MUZZLEFLASH, 1.5f * s, float(angle), point);
            break;
        case KIND_SMOKE:
            particle_splash(PART_SMOKE, int(16 * s), 2000, point);
            break;
        default:
            particle_flash(PART_MUZZLEFLASH, 1.2f * s, float(angle), point);
            particle_splash(PART_PLASMA, 8, 300, point);
            break;
    }
    // The effect's light, for stock geometry (the retail mesh keeps its lightmaps).
    if(def->lightRadius > 0)
        adddynlight(NULL, point, clamp(int(def->lightRadius * scale * .5f), 8, 48), clamp(int(def->lightSeconds * 1000), 150, 1500), 300,
                    uchar(255 * def->light.x), uchar(255 * def->light.y), uchar(255 * def->light.z));
    return def != &fallback;
}

bool halofxeffectdamage(const char *tag, HaloEffectDamage &out)
{
    const EffectDef *def = findEffect(tag);
    if(!def || !def->hasDamage) return false;
    out = def->damage;
    return true;
}

bool halofxjpt(const char *tag, HaloEffectDamage &out)
{
    if(!tag || !*tag || !loadEffects()) return false;
    std::string wanted(tag);
    for(char &c : wanted) if(c == '/') c = '\\';
    for(const JptDef &j : jpts)
        if(j.valid && !strcasecmp(j.path.c_str(), wanted.c_str())) { out = j.damage; return true; }
    return false;
}

void halofxblast(const vec &point, float radius)
{
    if(!camera1) return;
    const float reach = max(radius * 3, 24.0f), d = camera1->o.dist(point);
    if(d < reach) halofxshakeimpulse(.9f * (1 - d / reach), 450);
}

// ---- Camera shake

void halofxshakelimit(int what, float a, float b, float c)
{
    if(what == 0) shakeTranslation = vec(a, b, c);
    else if(what == 1) shakeRotation = vec(a, b, c);
    else { rumbleLeft = clamp(a, 0.0f, 1.0f); rumbleRight = clamp(b, 0.0f, 1.0f); }
}

void halofxshakestart(float intensity, float attack) { shake.set(clamp(intensity, 0.0f, 1.0f), max(attack, 0.0f)); }
void halofxshakestop(float decay) { shake.set(0, max(decay, 0.0f)); }

void halofxshakeimpulse(float intensity, int millis)
{
    intensity = clamp(intensity, 0.0f, 1.0f);
    // A stronger knock replaces a weaker one still dying away.
    float left = 0;
    if(lastmillis < impulseEnd && impulseEnd > impulseStart) left = impulse * float(impulseEnd - lastmillis) / float(impulseEnd - impulseStart);
    if(intensity <= left) return;
    impulse = intensity;
    impulseStart = lastmillis;
    impulseEnd = lastmillis + max(millis, 50);
}

void halofxcamera(float &yaw, float &pitch, float &roll, vec &offset)
{
    yaw = pitch = roll = 0;
    offset = vec(0, 0, 0);
    if(!halo_shake || !haloactive() || editmode) return;
    const float t = lastmillis / 1000.0f;
    const float level = shake.value();
    if(level > 0)
    {
        yaw += shakeRotation.x * level * noise(t, 17.3f, 29.1f, 0.3f);
        pitch += shakeRotation.y * level * noise(t, 19.7f, 33.4f, 1.1f);
        roll += shakeRotation.z * level * noise(t, 13.1f, 27.9f, 2.3f);
        const float scale = haloscenarioscale();
        offset.x += shakeTranslation.x * scale * level * noise(t, 21.0f, 37.0f, 0.7f);
        offset.y += shakeTranslation.y * scale * level * noise(t, 23.0f, 31.0f, 1.9f);
        offset.z += shakeTranslation.z * scale * level * noise(t, 25.0f, 35.0f, 2.9f);
        // The controller's rumble: a fine, fast tremble.
        const float rumble = max(rumbleLeft, rumbleRight) * level;
        yaw += .35f * rumble * noise(t, 61.0f, 83.0f, 0.5f);
        pitch += .35f * rumble * noise(t, 67.0f, 89.0f, 1.5f);
    }
    if(lastmillis < impulseEnd && impulseEnd > impulseStart)
    {
        const float k = impulse * float(impulseEnd - lastmillis) / float(impulseEnd - impulseStart);
        pitch += 1.6f * k * noise(t, 47.0f, 71.0f, 0.9f);
        roll += 1.2f * k * noise(t, 43.0f, 67.0f, 2.1f);
        yaw += .8f * k * noise(t, 53.0f, 79.0f, 3.3f);
    }
    yaw = clamp(yaw, -8.0f, 8.0f);
    pitch = clamp(pitch, -8.0f, 8.0f);
    roll = clamp(roll, -8.0f, 8.0f);
}

// ---- Cinematic screen effects

void halofxscreenstart(bool on) { screenOn = on; }

void halofxscreenstop()
{
    screenOn = false;
    blur.hold(0);
    desaturation.hold(0);
    tintAmount.hold(0);
    tint = vec(1, 1, 1);
    videoMode = 0;
    videoIntensity = 0;
}

void halofxconvolution(int iterations, float from, float to, float seconds)
{
    blurIterations = clamp(iterations, 1, 3);
    blur.go(max(from, 0.0f), max(to, 0.0f), seconds);
}

void halofxfilter(float from, float to, float tintFrom, float tintTo, float seconds)
{
    desaturation.go(clamp(from, 0.0f, 1.0f), clamp(to, 0.0f, 1.0f), seconds);
    tintAmount.go(clamp(tintFrom, 0.0f, 1.0f), clamp(tintTo, 0.0f, 1.0f), seconds);
}

void halofxfiltertint(float r, float g, float b) { tint = vec(clamp(r, 0.0f, 1.0f), clamp(g, 0.0f, 1.0f), clamp(b, 0.0f, 1.0f)); }
void halofxvideo(int mode, float intensity) { videoMode = mode; videoIntensity = clamp(intensity, 0.0f, 1.0f); }

void halofxcleanupgl()
{
    // Called with the old context still current (cleanupgl in resetgl), before it is destroyed.
    if(screenTexture) glDeleteTextures(1, &screenTexture);
    if(spotTexture) glDeleteTextures(1, &spotTexture);
    screenTexture = spotTexture = 0;
    screenW = screenH = texW = texH = 0;
    npotTextures = -1;
}

void halofxdrawscreen(int w, int h)
{
    if(!halo_screeneffects || !screenOn || !haloactive() || w <= 0 || h <= 0) return;
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    // Blur: the frame copied once and laid back over itself in rings of offset copies, each at the
    // weight that keeps the result an even average (the retail radius is in 480-line pixels).
    const float radius = blur.value() * h / 480.0f * .5f;
    const int wantW = npot() ? w : powerOfTwo(w), wantH = npot() ? h : powerOfTwo(h);
    if(radius >= .5f && (hwtexsize <= 0 || (wantW <= hwtexsize && wantH <= hwtexsize)))
    {
        if(stale(screenTexture)) { screenW = screenH = texW = texH = 0; ++screenRebuilds; }
        if(!screenTexture) { glGenTextures(1, &screenTexture); screenW = screenH = texW = texH = 0; }
        glBindTexture(GL_TEXTURE_2D, screenTexture);
        if(screenW != w || screenH != h)
        {
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            if(texW != wantW || texH != wantH)
            {
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, wantW, wantH, 0, GL_RGB, GL_UNSIGNED_BYTE, NULL);
                texW = wantW;
                texH = wantH;
            }
            screenW = w;
            screenH = h;
        }
        glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, w, h);
        // The frame fills the copy's lower left w x h; a power-of-two copy is larger.
        const float u = float(w) / max(texW, 1), v = float(h) / max(texH, 1);
        glEnable(GL_TEXTURE_2D);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        int layer = 1;
        for(int ring = 1; ring <= blurIterations; ++ring) loopk(8)
        {
            const float a = (k * 45 + ring * 22.5f) * RAD, r = radius * ring / blurIterations;
            const float dx = cosf(a) * r * VIRTW / w, dy = sinf(a) * r * VIRTH / h;
            glColor4f(1, 1, 1, 1.0f / (++layer));
            glBegin(GL_TRIANGLE_STRIP);
            glTexCoord2f(0, v); glVertex2f(dx, dy);
            glTexCoord2f(u, v); glVertex2f(VIRTW + dx, dy);
            glTexCoord2f(0, 0); glVertex2f(dx, VIRTH + dy);
            glTexCoord2f(u, 0); glVertex2f(VIRTW + dx, VIRTH + dy);
            glEnd();
        }
        glDisable(GL_TEXTURE_2D);
    }
    else glDisable(GL_TEXTURE_2D);
    // Desaturation and tint: without shaders, a multiply towards the tint and a wash of its grey,
    // which take the colour out of the frame about as far as the filter asks.
    const float d = desaturation.value(), amount = tintAmount.value();
    if(d > .01f || amount > .01f)
    {
        const vec colour(1 + (tint.x - 1) * amount, 1 + (tint.y - 1) * amount, 1 + (tint.z - 1) * amount);
        glBlendFunc(GL_DST_COLOR, GL_ZERO);
        const float m = .45f * max(d, amount);
        glColor4f(1 - m + m * colour.x, 1 - m + m * colour.y, 1 - m + m * colour.z, 1);
        drawQuad(0, 0, VIRTW, VIRTH);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glColor4f(.42f * colour.x, .42f * colour.y, .42f * colour.z, .32f * d);
        drawQuad(0, 0, VIRTW, VIRTH);
    }
    // The video look (c10's recording): scan lines and a cold cast.
    if(videoMode > 0 && videoIntensity > .01f)
    {
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glColor4f(0, 0, 0, .28f * videoIntensity);
        const int offset = (lastmillis / 40) % 6;
        for(int y = offset; y < VIRTH; y += 6) drawQuad(0, float(y), VIRTW, 2);
        glColor4f(.55f, .75f, .85f, .10f * videoIntensity);
        drawQuad(0, 0, VIRTW, VIRTH);
    }
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glColor4f(1, 1, 1, 1);
}

// ---- The flashlight

void halofxflashlighttoggle()
{
    if(!haloactive() || halocinematic() || player1->state != CS_ALIVE) return;
    if(!flashOn && flashCharge < .05f) return;
    flashOn = !flashOn;
    loadEffects();
    audiomgr.playsound(S_NOAMMO, SP_LOW);
}

bool halofxflashlight(float *charge)
{
    if(charge) *charge = flashCharge;
    return flashOn;
}

void halofxflashlightworld()
{
    if(!flashOn || !haloworldready() || halocinematic() || !hasMT || maxtmus < 2) return;
    if(stale(spotTexture)) ++spotRebuilds;
    if(!spotTexture) spotTexture = makeSpot();
    const float range = flashRadius * max(haloscenarioscale(), .5f);
    // Unit 1: the spot, projected from the eye (eye-linear texture coordinates divided by depth).
    glActiveTexture_(GL_TEXTURE1_ARB);
    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, spotTexture);
    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glLoadIdentity();
    const GLfloat planeS[4] = {1, 0, 0, 0}, planeT[4] = {0, 1, 0, 0}, planeQ[4] = {0, 0, -1, 0};
    glTexGeni(GL_S, GL_TEXTURE_GEN_MODE, GL_EYE_LINEAR);
    glTexGeni(GL_T, GL_TEXTURE_GEN_MODE, GL_EYE_LINEAR);
    glTexGeni(GL_Q, GL_TEXTURE_GEN_MODE, GL_EYE_LINEAR);
    glTexGenfv(GL_S, GL_EYE_PLANE, planeS);
    glTexGenfv(GL_T, GL_EYE_PLANE, planeT);
    glTexGenfv(GL_Q, GL_EYE_PLANE, planeQ);
    glEnable(GL_TEXTURE_GEN_S);
    glEnable(GL_TEXTURE_GEN_T);
    glEnable(GL_TEXTURE_GEN_Q);
    glPopMatrix();
    glMatrixMode(GL_TEXTURE);
    glPushMatrix();
    glLoadIdentity();
    const float k = .5f / max(tanf(flashCutoff), .05f);
    glTranslatef(.5f, .5f, 0);
    glScalef(k, k, 1);
    glMatrixMode(GL_MODELVIEW);
    glActiveTexture_(GL_TEXTURE0_ARB);
    // The light adds the surface's own texture lit by the spot, fading to nothing at the retail
    // radius (fog towards black does the distance falloff).
    glPushAttrib(GL_FOG_BIT | GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_CURRENT_BIT);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE);
    glDepthMask(GL_FALSE);
    glDepthFunc(GL_LEQUAL);
    glEnable(GL_FOG);
    glFogi(GL_FOG_MODE, GL_LINEAR);
    const GLfloat black[4] = {0, 0, 0, 1};
    glFogfv(GL_FOG_COLOR, black);
    glFogf(GL_FOG_START, range * .3f);
    glFogf(GL_FOG_END, range);
    const float strength = .75f * clamp(flashCharge * 4, .3f, 1.0f);
    glColor4f(flashColour.x * strength, flashColour.y * strength, flashColour.z * strength, 1);
    haloworlddrawlight();
    glPopAttrib();
    glActiveTexture_(GL_TEXTURE1_ARB);
    glDisable(GL_TEXTURE_GEN_S);
    glDisable(GL_TEXTURE_GEN_T);
    glDisable(GL_TEXTURE_GEN_Q);
    glMatrixMode(GL_TEXTURE);
    glPopMatrix();
    glMatrixMode(GL_MODELVIEW);
    glDisable(GL_TEXTURE_2D);
    glActiveTexture_(GL_TEXTURE0_ARB);
    glColor4f(1, 1, 1, 1);
}

// ---- The damage-direction indicator

void halofxdamage(const vec &from, float amount, bool shieldOnly)
{
    if(!haloactive()) return;
    const float angle = hudAngle(from);
    // A hit from about the same way renews its arc; otherwise the oldest arc goes.
    int slot = 0;
    loopi(8)
    {
        if(lastmillis - hits[i].at < hitMillis && fabsf(remainderf(hudAngle(hits[i].from) - angle, 2 * PI)) < .35f) { slot = i; break; }
        if(hits[i].at < hits[slot].at) slot = i;
    }
    Hit &h = hits[slot];
    h.from = from;
    h.at = lastmillis;
    h.strength = clamp(.35f + amount / 40.0f, .35f, 1.0f);
    h.shield = shieldOnly;
}

void halofxdrawdamage()
{
    if(!halo_damageindicator || !haloactive() || player1->state != CS_ALIVE) return;
    glDisable(GL_TEXTURE_2D);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    const float cx = VIRTW * .5f, cy = VIRTH * .5f, inner = VIRTH * .15f, outer = inner + VIRTH * .028f;
    loopi(8)
    {
        const Hit &h = hits[i];
        const int age = lastmillis - h.at;
        if(!h.at || age < 0 || age >= hitMillis) continue;
        const float fade = 1 - float(age) / hitMillis, centre = hudAngle(h.from), half = .3f;
        const float alpha = clamp(fade * h.strength, 0.0f, 1.0f);
        glBegin(GL_TRIANGLE_STRIP);
        for(int s = 0; s <= 12; ++s)
        {
            const float a = centre - half + 2 * half * s / 12.0f;
            // The arc is strongest in its middle.
            const float edge = 1 - fabsf(float(s - 6)) / 6.0f;
            glColor4f(1, h.shield ? .3f : .12f, h.shield ? .2f : .08f, alpha * (.35f + .65f * edge) * .85f);
            glVertex2f(cx + sinf(a) * inner, cy - cosf(a) * inner);
            glVertex2f(cx + sinf(a) * outer, cy - cosf(a) * outer);
        }
        glEnd();
    }
    glColor4f(1, 1, 1, 1);
    glEnable(GL_TEXTURE_2D);
}

// ---- Sound classes

void halofxsetclassgain(const char *prefix, float gain, float seconds)
{
    if(!gainsReady) resetGains();
    if(!prefix || !*prefix) return;
    const size_t length = strlen(prefix);
    int changed = 0;
    loopi(HALO_SOUND_CLASSES)
    {
        if(!*classNames[i] || strncmp(classNames[i], prefix, length)) continue;
        classGain[i].set(clamp(gain, 0.0f, 1.0f), seconds);
        ++changed;
    }
    if(halo_fxtrace) conoutf("Halo sound class: %s to %.2f over %.2fs (%d classes)", prefix, gain, seconds, changed);
}

bool halofxgains(float gains[HALO_SOUND_CLASSES])
{
    bool changed = false;
    loopi(HALO_SOUND_CLASSES)
    {
        gains[i] = gainsReady ? clamp(classGain[i].to, 0.0f, 1.0f) : 1.0f;
        changed |= gains[i] < .999f;
    }
    return changed;
}

void halofxsetgains(const float gains[HALO_SOUND_CLASSES])
{
    if(!gainsReady) resetGains();
    loopi(HALO_SOUND_CLASSES) classGain[i].hold(std::isfinite(gains[i]) ? clamp(gains[i], 0.0f, 1.0f) : 1.0f);
}

float halofxclassgain(int soundClass)
{
    if(!gainsReady || soundClass < 0 || soundClass >= HALO_SOUND_CLASSES) return 1;
    return clamp(classGain[soundClass].value(), 0.0f, 1.0f);
}

int halofxsoundclass(const char *cache, int tagIndex)
{
    if(!cache || !*cache || tagIndex < 0 || !loadEffects()) return -1;
    auto found = cacheClasses.find(cache);
    if(found == cacheClasses.end())
    {
        std::vector<signed char> &table = cacheClasses[cache];
        auto line = classLines.find(cache);
        if(line != classLines.end())
        {
            std::istringstream in(line->second);
            std::string pair;
            while(in >> pair)
            {
                const size_t colon = pair.find(':');
                if(colon == std::string::npos) continue;
                const int index = atoi(pair.c_str()), value = atoi(pair.c_str() + colon + 1);
                if(index < 0 || index > 65535 || value < 0 || value >= HALO_SOUND_CLASSES) continue;
                if(int(table.size()) <= index) table.resize(index + 1, -1);
                table[index] = (signed char)value;
            }
        }
        found = cacheClasses.find(cache);
    }
    return tagIndex < int(found->second.size()) ? found->second[tagIndex] : -1;
}

int halofxsampleclass(const char *sample)
{
    if(!sample) return -1;
    if(!samplesTried)
    {
        samplesTried = true;
        int size = 0;
        char *text = loadfile(path("packages/audio/halo/tags/classes.txt", true), &size);
        if(text)
        {
            std::istringstream lines(text);
            delete[] text;
            std::string line;
            while(std::getline(lines, line))
            {
                if(line.empty() || line[0] == '#') continue;
                std::istringstream in(line);
                std::string name;
                int value = -1;
                if(in >> name >> value && value >= 0 && value < HALO_SOUND_CLASSES) sampleClasses[name] = value;
            }
        }
    }
    auto found = sampleClasses.find(sample);
    return found != sampleClasses.end() ? found->second : -1;
}

void halofxdump(const char *label)
{
    float y, p, r;
    vec offset;
    halofxcamera(y, p, r, offset);
    int active = 0;
    loopi(8) if(hits[i].at && lastmillis - hits[i].at < hitMillis) ++active;
    std::string last = lastEffect;
    for(char &c : last) if(c == ' ') c = '_';
    conoutf("FX_QA %s loaded=%d table=%d effects=%d visual=%d unknown=%d last=%s shake=%.3f impulse=%d camera=%.3f,%.3f,%.3f "
            "screen=%d blur=%.3f desat=%.3f tint=%.3f video=%d flashlight=%d charge=%.3f hits=%d "
            "music=%.3f ambient_nature=%.3f vehicle_engine=%.3f weapon_fire=%.3f unit_dialog=%.3f "
            "copy=%dx%d copytex=%dx%d npot=%d screen_tex=%d spot_tex=%d screen_rebuilds=%d spot_rebuilds=%d",
            label, int(loadEffects()), int(effects.size()), effectCount, effectVisual, effectUnknown, last.c_str(),
            shake.value(), int(lastmillis < impulseEnd), y, p, r, int(screenOn), blur.value(), desaturation.value(),
            tintAmount.value(), videoMode, int(flashOn), flashCharge, active, halofxclassgain(32), halofxclassgain(33),
            halofxclassgain(23), halofxclassgain(4), halofxclassgain(19), screenW, screenH, texW, texH, npotTextures,
            int(screenTexture != 0 && glIsTexture(screenTexture)), int(spotTexture != 0 && glIsTexture(spotTexture)),
            screenRebuilds, spotRebuilds);
    fflush(stdout);
}
