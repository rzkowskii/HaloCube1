#ifndef HALO_COLLISION_H
#define HALO_COLLISION_H

struct HaloCollisionHit
{
    float fraction = 1.0f;
    vec normal;
    int material = -1;
    unsigned flags = 0;
    float penetration = 0.0f;
    int surface = -1;
};

// Hit flag of an authored walking assist (the .hcol's AST1 trailer, ASSISTS in
// tools/import_halo_campaign.py): not retail data, never stored in the file's flags.
constexpr unsigned haloCollisionAssist = 16;

void halocollisionmap(const char *name);
bool halocollisionactive();
// A scenario machine whose real polygons are in the loaded .hcol (its box collision is skipped).
bool halocollisionbaked(int device);
// The loaded mesh's bounding box (the section's retail structure and baked machines).
bool halocollisionbounds(vec &low, vec &high);
// Rays (shots, sight, floor probes) never hit walking assists.
bool halocollisiontrace(const vec &from, const vec &to, HaloCollisionHit &hit, unsigned ignoreFlags = 0);
bool halocollisionopen(const vec &point);

// The capsule is vertical. Feet are its lowest point and height includes both caps.
// Walking assists collide only when `assists` is set (bodies: the player and actors). Vehicles
// pass false (halo_vehicle.cpp), like their wheel rays: an assist stands in for a biped's jump.
bool halocollisionsweep(const vec &feet, const vec &delta, float radius, float height, HaloCollisionHit &hit, bool assists = true);
bool halocollisionoverlap(const vec &feet, float radius, float height, HaloCollisionHit &hit, bool assists = true);
// The number of walking assists in the loaded mesh; with index in range, also that assist's
// centre and normal (index in file order: the AST1 trailer's, which is ASSISTS' order) (QA: tests/native/retail_qa.inc qa_retail_assists).
int halocollisionassist(int index, vec &centre, vec &normal);

#endif
