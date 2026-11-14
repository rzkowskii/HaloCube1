#ifndef HALO_WORLD_H
#define HALO_WORLD_H

// Retail BSP world meshes drawn over the AssaultCube map (halo_world.cpp).
enum HaloSurface { HALO_SOLID, HALO_CUTOUT, HALO_WATER, HALO_SKY, HALO_ADDITIVE, HALO_SKY_ADDITIVE, HALO_FOLIAGE };

// Loads the current map's or cinematic scene's mesh on demand. False when none applies.
bool haloworldready();
bool haloworldhasnozzles();
void haloworlddraw(HaloSurface pass);
void haloworlddrawnozzles();
void haloworlddrawcoronas(const vec &right, const vec &up, const vec &forward);

#endif
