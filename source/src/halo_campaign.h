#ifndef HALO_CAMPAIGN_H
#define HALO_CAMPAIGN_H

// Keep the counts in sync with tools/import_halo_campaign.py.
struct HaloMission
{
    const char *cache;
    const char *title;
    int sections;
    const char *finalObjective;
};

static constexpr HaloMission haloMissions[] = {
    {"a10", "The Pillar of Autumn", 7, "Reach the lifeboats"},
    {"a30", "Halo", 3, "Reach the evacuation site"},
    {"a50", "The Truth and Reconciliation", 5, "Open the escape shuttle"},
    {"b30", "The Silent Cartographer", 3, "Reach the Pelican landing zone"},
    {"b40", "Assault on the Control Room", 7, "Access the control room"},
    {"c10", "343 Guilty Spark", 7, "Reach the swamp extraction point"},
    {"c20", "The Library", 4, "Retrieve the Index"},
    {"c40", "Two Betrayals", 5, "Disable the final pulse generator"},
    {"d20", "Keyes", 6, "Reach the hangar escape point"},
    {"d40", "The Maw", 8, "Reach the escape craft"}
};
static constexpr int haloMissionCount = sizeof(haloMissions) / sizeof(haloMissions[0]);

inline void halosectionname(int mission, int section, char *name, size_t length)
{
    if(mission == 1 && section == 0) copystring(name, "halo_a30", length);
    else snprintf(name, length, "halo_%s_%02d", haloMissions[mission].cache, section + (mission == 1 ? 0 : 1));
}

inline bool haloparsemap(const char *name, int &mission, int &section)
{
    if(strncmp(name, "halo_", 5)) return false;
    for(int i = 0; i < haloMissionCount; ++i) for(int j = 0; j < haloMissions[i].sections; ++j)
    {
        string expected;
        halosectionname(i, j, expected, sizeof(expected));
        if(!strcmp(name, expected)) { mission = i; section = j; return true; }
    }
    return false;
}

// The section maps stamp their baked scenery into the cube raster (scenery_raster in
// tools/halo_collision.py, the cube-raster stamp) as solid cubes with this vdelta, which no solid
// cube of theirs had before (the importer wrote 0), for the actors' flood waypoints and the stock
// fallback only: light passes them (worldlight.cpp), so models are lit as before the stamp. Keep in
// sync with RASTER_SCENERY_VDELTA in tools/import_halo_campaign.py.
constexpr int haloSceneryVdelta = 83;
// Set by load_world for a campaign section map (haloparsemap), before its light is calculated.
extern bool haloscenerycubes;
// One of a section map's scenery cubes: solid for the flood waypoints and the stock fallback, but not
// a full-height wall, so neither light (worldlight.cpp) nor the 2D occlusion map that culls models
// (worldocull.cpp) stops at it.
inline bool haloscenerycube(const sqr *s) { return haloscenerycubes && SOLID(s) && s->vdelta == haloSceneryVdelta; }

#endif
