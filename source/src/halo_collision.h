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

void halocollisionmap(const char *name);
bool halocollisionactive();
// A scenario machine whose real polygons are in the loaded .hcol (its box collision is skipped).
bool halocollisionbaked(int device);
// The loaded mesh's bounding box (the section's retail structure and baked machines).
bool halocollisionbounds(vec &low, vec &high);
bool halocollisiontrace(const vec &from, const vec &to, HaloCollisionHit &hit, unsigned ignoreFlags = 0);
bool halocollisionopen(const vec &point);

// The capsule is vertical. Feet are its lowest point and height includes both caps.
bool halocollisionsweep(const vec &feet, const vec &delta, float radius, float height, HaloCollisionHit &hit);
bool halocollisionoverlap(const vec &feet, float radius, float height, HaloCollisionHit &hit);

#endif
