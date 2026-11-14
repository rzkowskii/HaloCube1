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

#endif
