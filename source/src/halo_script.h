#ifndef HALO_SCRIPT_H
#define HALO_SCRIPT_H

void haloscenariostart(int mission, int section, bool preview, bool continuing);
void haloscenariostop();
void haloscenariotick(int elapsed);
void haloscenariorevert();
bool haloscenarioexit();
bool halopelicanready();
bool haloscenariocomplete();
bool haloscenariotransition();
bool haloscenariouse();
bool halocartographertarget(vec &point, bool &mapped);
// Center of the authored trigger volume that switches to the next section's BSP.
bool haloscenarioexitpoint(vec &point);
// AssaultCube units per Halo world unit for the current section.
float haloscenarioscale();
bool haloscenariocollide(physent *entity, float &hi, float &lo);
struct HaloCollisionHit;
bool haloscenariotrace(const vec &from, const vec &to, HaloCollisionHit &hit);
bool haloscenarioescapetimeout();
bool haloscenarioescapefailed();
bool halocinematicskip();
bool halocinematic();
bool halocinematiccamera(physent &camera);
bool halocinematicground(const vec &point, float &ground);
const char *halocinematicworld();
float halocinematicfov();
float halocinematicnear();
void haloscenariodraw();
const char *haloscenarioobjective();
void haloscenariorender();
void haloscenariopausestate(bool paused);
void haloscenariocheckpoint();
void haloscenarioplayersave();
playerent *haloscenariospawnenemy(int kind, const vec &point, float yaw);
bool haloscenariobotsuspended(const playerent *bot);
void haloscenarioremovebot(const playerent *bot);

#endif
