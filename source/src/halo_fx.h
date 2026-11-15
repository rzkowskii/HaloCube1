#ifndef HALO_FX_H
#define HALO_FX_H

#include "halo_weapons.h"

// Presentation (halo_fx.cpp): the effects the mission scripts create, camera shake, the cinematic
// screen effects, the flashlight, the damage-direction indicator and the retail sound classes'
// gains. Nothing here changes the game's state: no damage, no movement, no script results (an
// effect's jpt! damage is only read here, halofxeffectdamage; weapon.cpp deals it).
// The tables come from tools/halo_effects.py (packages/halo/effects.txt: effects, the flashlight,
// each cache's sound classes) and tools/import_halo_audio.py (packages/audio/halo/tags/classes.txt:
// the class of each decoded weapon, vehicle and effect sample). Without them the effects fall back
// to a plain explosion and every class plays at full gain.

void halofxreset();                         // a section (map) starts: shakes, screen effects, gains, the flashlight off
void halofxrevert();                        // a checkpoint revert (haloscenariorevert): shakes, screen effects, arcs
void halofxupdate(int elapsed);             // every frame (haloupdate): the flashlight's charge and light

// effect_new and effect_new_on_object_marker: the retail effect `tag` at `point` (world units of
// the section, AssaultCube coordinates). AssaultCube particles stand in for the tags' particle
// systems: fireballs, sparks and smoke sized by the tags' particle systems, a flash, the effect's
// sound and a camera shake near the player. `visual` false (a cinematic, whose camera is not the
// section's) plays only the sound, unpositioned. False for a tag the table does not hold (it still
// draws: an explosion when the name says so, else a small burst).
bool halofxeffect(const char *tag, const vec &point, bool visual);
// A blast of `radius` AssaultCube units at `point` (grenades, rockets): the camera shakes near it.
void halofxblast(const vec &point, float radius);
// The jpt! area damage of effect `tag` (effects.txt's optional `damage` and `materials` lines: the
// effect's strongest damaging jpt! part; radii in world units), false when the table gives it none.
// Drawing an effect deals nothing: halo.h haloeffectblast deals it, for a vehicle's death
// explosion and (halo_effectdamage 1) the scripts' effect_new.
struct HaloEffectDamage
{
    HaloDamage damage;
    float inner = 0, outer = 0;
    int sideEffect = 0, category = 0;
};
bool halofxeffectdamage(const char *tag, HaloEffectDamage &out);

// player_effect_*: the scripts' camera shake (translation in world units, rotation in degrees,
// rumble 0..1 standing in for the controller's motors: a fine shake). `what`: 0 translation,
// 1 rotation, 2 rumble.
void halofxshakelimit(int what, float a, float b, float c);
void halofxshakestart(float intensity, float attack);  // seconds to reach `intensity` (0..1)
void halofxshakestop(float decay);                     // seconds to die away
void halofxshakeimpulse(float intensity, int millis);  // a short knock (vehicle crashes, blasts, player_effect_impact)
// The camera's offset now (transplayer, the sky): degrees and AssaultCube units. Time-based noise,
// not rnd(): it must not draw on the game's random sequence.
void halofxcamera(float &yaw, float &pitch, float &roll, vec &offset);

// cinematic_screen_effect_*: blur (convolution: radius from, to, over seconds), desaturation and
// tint (filter), a video look (scan lines). Drawn over the 3D view before the HUD (halofxdrawscreen).
void halofxscreenstart(bool on);
void halofxscreenstop();
void halofxconvolution(int iterations, float from, float to, float seconds);
void halofxfilter(float from, float to, float tintFrom, float tintTo, float seconds);
void halofxfiltertint(float r, float g, float b);
void halofxvideo(int mode, float intensity);
// Start of the 2D pass (renderhud.cpp gl_drawhud, after its ortho setup): `w`x`h` is the viewport.
// Leaves GL_TEXTURE_2D disabled and the usual alpha blending.
void halofxdrawscreen(int w, int h);
// The GL context is about to be rebuilt (rendergl.cpp cleanupgl, from resetgl: a resolution or
// display change that recreates the window): deletes the blur's copy and the flashlight's spot,
// which are made again on their next use. A name that went stale some other way is caught by
// glIsTexture before each use; a viewport of a new size reallocates the copy.
void halofxcleanupgl();

// The flashlight (X, halo_flashlight): a spot of the retail light's radius and cone from the eye,
// added over the retail mesh (halofxflashlightworld, after the mesh's solid pass), a dynamic light
// on stock geometry. Its charge drains while it is on and recovers while off (an engine choice;
// the tags hold no flashlight energy).
void halofxflashlighttoggle();
bool halofxflashlight(float *charge = nullptr);
void halofxflashlightworld();

// The damage-direction indicator: a red arc round the reticle towards where a hit came from (a
// shield hit too), fading over a second. Drawn in the HUD pass (halofxdrawdamage).
void halofxdamage(const vec &from, float amount, bool shieldOnly);
void halofxdrawdamage();

// Sound classes (retail snd! classes 0..50: 4 weapon fire, 19 unit dialog, 23 vehicle engine,
// 32 music, 33-35 ambient, ...). sound_class_set_gain fades every class whose name starts with
// `prefix` ("vehicle" is vehicle_collision and vehicle_engine) to `gain` over `seconds`.
enum { HALO_SOUND_CLASSES = 51, HALO_SOUND_CLASS_VEHICLE_ENGINE = 23 };
void halofxsetclassgain(const char *prefix, float gain, float seconds);
float halofxclassgain(int soundClass);                 // 1 for -1 (no class known)
// Saved games (progress v3's optional `s gain` record, halo_script.cpp): each class's gain, a fade
// in progress as where it is heading; false (nothing to save) when every class is at full gain.
// halofxsetgains puts them back at once.
bool halofxgains(float gains[HALO_SOUND_CLASSES]);
void halofxsetgains(const float gains[HALO_SOUND_CLASSES]);
int halofxsoundclass(const char *cache, int tagIndex); // a scenario sound's class, or -1
int halofxsampleclass(const char *sample);             // a decoded tag sample's class, or -1

// QA (tests/native/presentation_qa.inc): one FX_QA line with the state above.
void halofxdump(const char *label);

#endif
