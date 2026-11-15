#include "cube.h"
#include "halo_weapons.h"
#include <cstdlib>
#include <memory>
#include <sstream>

namespace
{
    constexpr int formatVersion = 2;        // tools/halo_weapons.py FORMAT
    std::vector<HaloWeaponDef> definitions;
    bool attempted = false, loaded = false;

    HaloWeaponClass weaponClass(const std::string &text)
    {
        static const char *const names[] = {"assault", "pistol", "shotgun", "sniper", "projectile", "vehicle", "actor"};
        loopi(sizeof(names) / sizeof(names[0])) if(text == names[i]) return HaloWeaponClass(i);
        return HALO_WC_ACTOR;
    }

    std::string displayName(const std::string &path)
    {
        const size_t slash = path.rfind('\\');
        std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
        bool start = true;
        for(char &c : name)
        {
            if(start && c >= 'a' && c <= 'z') c -= 'a' - 'A';
            start = c == ' ';
        }
        return name;
    }

    bool parseDamage(std::istringstream &in, HaloDamage &d)
    {
        return bool(in >> d.lower >> d.upperMin >> d.upperMax) && d.upperMax >= d.upperMin;
    }

    bool parseProjectile(std::istringstream &in, HaloProjectileDef &p)
    {
        std::string first;
        in >> first;
        if(first == "-") return true;
        p.speed = float(atof(first.c_str()));
        if(!(in >> p.finalSpeed >> p.range >> p.gravity >> p.guided) || !parseDamage(in, p.damage)
           || !(in >> p.category >> p.radius)) return false;
        std::getline(in >> std::ws, p.path);
        p.valid = true;
        return true;
    }

    bool parse(const char *text)
    {
        std::istringstream lines(text);
        std::string line;
        HaloWeaponDef *weapon = nullptr;
        bool versioned = false;
        int number = 0;
        while(std::getline(lines, line))
        {
            ++number;
            if(line.empty() || line[0] == '#') continue;
            std::istringstream in(line);
            std::string key;
            in >> key;
            bool ok = true;
            if(key == "format")
            {
                int version = 0;
                in >> version;
                ok = versioned = version == formatVersion;
                if(!ok) conoutf("\f3Halo weapons: packages/halo/weapons.txt is format %d, the game reads format %d; re-run tools/halo_weapons.py", version, formatVersion);
            }
            else if(key == "weapon")
            {
                definitions.emplace_back();
                weapon = &definitions.back();
                in >> weapon->index;
                ok = weapon->index == int(definitions.size()) - 1;
            }
            else if(!weapon) ok = false;
            else if(key == "path") { std::getline(in >> std::ws, weapon->path); weapon->name = displayName(weapon->path); }
            else if(key == "label") { std::string kind; in >> weapon->label >> kind; weapon->kind = weaponClass(kind); }
            else if(key == "viewmodel") in >> weapon->viewmodel;
            else if(key == "worldmodel") in >> weapon->worldmodel;
            else if(key == "ready") ok = bool(in >> weapon->ready);
            else if(key == "heat") ok = bool(in >> weapon->heatRecover >> weapon->heatOverheat >> weapon->heatLoss);
            else if(key == "zoom") ok = bool(in >> weapon->zoomLevels >> weapon->zoomMin >> weapon->zoomMax);
            else if(key == "aim") ok = bool(in >> weapon->autoaimAngle >> weapon->autoaimRange >> weapon->magnetismAngle >> weapon->magnetismRange);
            else if(key == "melee") ok = parseDamage(in, weapon->melee);
            else if(key == "magazine")
            {
                HaloMagazineDef m;
                ok = bool(in >> m.loaded >> m.initial >> m.maximum >> m.reload >> m.reloaded >> m.recharge);
                weapon->magazines.push_back(m);
            }
            else if(key == "trigger")
            {
                HaloTriggerDef t;
                ok = bool(in >> t.rate >> t.finalRate >> t.rateAccel >> t.rateDecel >> t.roundsPerShot >> t.projectilesPerShot
                    >> t.distribution >> t.error >> t.finalError >> t.errorAccel >> t.errorDecel >> t.errorMin >> t.errorMax
                    >> t.charge >> t.heat >> t.age >> t.magazine);
                weapon->triggers.push_back(t);
            }
            else if(key == "projectile")
            {
                ok = !weapon->triggers.empty() && parseProjectile(in, weapon->triggers.back().projectile);
            }
            else if(key == "sound")
            {
                // Unknown roles are skipped, so a newer table still loads.
                std::string role, tag;
                in >> role;
                std::getline(in >> std::ws, tag);
                weapon->sounds.parse(role, tag);
            }
            else if(key == "end") weapon = nullptr;
            if(!ok)
            {
                conoutf("\f3Halo weapons: invalid line %d in packages/halo/weapons.txt", number);
                definitions.clear();
                return false;
            }
        }
        // A table without a format line is refused too, and must not leave definitions behind.
        if(!versioned) definitions.clear();
        return versioned && !definitions.empty();
    }
}

float HaloDamage::roll() const
{
    return upperMin + rndscale(upperMax - upperMin);
}

std::string halosoundsample(const std::string &tag)
{
    std::string name = tag;
    for(char &c : name) c = char(tolower(c));
    if(!name.compare(0, 6, "sound\\")) name.erase(0, 6);
    for(char &c : name) c = c == '\\' ? '-' : c == ' ' ? '_' : c;
    return "tags/" + name;
}

bool HaloSounds::parse(const std::string &role, const std::string &tag)
{
    static const char *const roles[HALO_SOUND_ROLES] = {
        "fire", "charged", "empty", "cycle", "reload", "reload-empty", "melee", "ready", "overheat", "pickup",
        "throw", "explode", "armed",
    };
    if(tag.empty()) return false;
    loopi(HALO_SOUND_ROLES) if(role == roles[i]) { samples[i] = halosoundsample(tag); return true; }
    return false;
}

int HaloWeaponDef::acgun() const
{
    switch(kind)
    {
        case HALO_WC_ASSAULT: return GUN_ASSAULT;
        case HALO_WC_PISTOL: return GUN_PISTOL;
        case HALO_WC_SHOTGUN: return GUN_SHOTGUN;
        case HALO_WC_SNIPER: return GUN_SNIPER;
        default: return GUN_CARBINE;
    }
}

float HaloWeaponDef::magnification(int level) const
{
    if(level <= 0 || zoomLevels <= 0) return 1;
    if(zoomLevels == 1) return max(1.0f, zoomMin);
    const float t = float(clamp(level, 1, zoomLevels) - 1) / float(zoomLevels - 1);
    // The retail zoom levels step geometrically between the bounds.
    return max(1.0f, zoomMin * powf(zoomMax / max(zoomMin, 1e-3f), t));
}

void HaloWeaponState::reset(const HaloWeaponDef *d, bool full)
{
    def = d ? d->index : -1;
    battery = 1;
    heat = error = rate = 0;
    overheated = charged = false;
    lastShot = chargeStart = zoom = 0;
    const HaloMagazineDef *m = d ? d->magazine() : nullptr;
    loaded = m && full ? m->loaded : 0;
    reserve = m && full ? max(0, m->initial - m->loaded) : 0;
}

void HaloWeaponState::tick(const HaloWeaponDef &d, int elapsed)
{
    if(elapsed <= 0) return;
    heat = max(0.0f, heat - d.heatLoss * elapsed / 1000.0f);
    if(overheated && heat <= d.heatRecover) overheated = false;
}

bool HaloWeaponState::canfire(const HaloWeaponDef &d) const
{
    if(overheated) return false;
    if(d.usesBattery()) return battery > 0;
    return loaded > 0;
}

float HaloWeaponState::shoot(const HaloWeaponDef &d, const HaloTriggerDef &t, int millis)
{
    // Ramps fall back towards their initial value while the trigger rests.
    const float idle = lastShot ? (millis - lastShot) / 1000.0f : 1e3f;
    const float period = t.rate > 0 ? 1.0f / max(t.rate, 0.01f) : 0.1f;
    if(idle > period * 1.5f)
    {
        error = t.errorDecel > 0 ? max(0.0f, error - (idle - period) / t.errorDecel) : 0.0f;
        rate = t.rateDecel > 0 ? max(0.0f, rate - (idle - period) / t.rateDecel) : 0.0f;
    }
    const float cone = t.errorMin + (t.errorMax - t.errorMin) * clamp(t.error + (t.finalError - t.error) * error, 0.0f, 1.0f);
    error = t.errorAccel > 0 ? min(1.0f, error + period / t.errorAccel) : 1.0f;
    rate = t.rateAccel > 0 ? min(1.0f, rate + period / t.rateAccel) : 1.0f;
    lastShot = millis;
    heat += t.heat;
    if(d.heatOverheat > 0 && heat >= d.heatOverheat) overheated = true;
    if(d.usesBattery()) battery = max(0.0f, battery - t.age);
    return cone;
}

int HaloWeaponState::delay(const HaloTriggerDef &t) const
{
    const float shotsPerSecond = t.rate + (t.finalRate - t.rate) * rate;
    // A rate of zero means the trigger fires as fast as it is pulled.
    return shotsPerSecond > 0.01f ? int(1000.0f / shotsPerSecond) : 100;
}

bool haloweaponsload()
{
    if(attempted) return loaded;
    attempted = true;
    int size = 0;
    char *text = loadfile(path("packages/halo/weapons.txt", true), &size);
    if(!text)
    {
        conoutf("\f3Halo weapons: packages/halo/weapons.txt is missing; run tools/halo_weapons.py");
        return false;
    }
    loaded = parse(text);
    delete[] text;
    if(loaded) conoutf("Halo weapons: loaded %d definitions", int(definitions.size()));
    return loaded;
}

int haloweaponcount() { haloweaponsload(); return int(definitions.size()); }

const HaloWeaponDef *haloweapon(int index)
{
    haloweaponsload();
    return index >= 0 && index < int(definitions.size()) ? &definitions[index] : nullptr;
}

const HaloWeaponDef *haloweaponfind(const char *path)
{
    haloweaponsload();
    for(const HaloWeaponDef &d : definitions) if(d.path == path) return &d;
    return nullptr;
}

const HaloWeaponDef *haloweaponforgun(int gun)
{
    switch(gun)
    {
        case GUN_ASSAULT: return haloweaponfind("weapons\\assault rifle\\assault rifle");
        case GUN_PISTOL: return haloweaponfind("weapons\\pistol\\pistol");
        case GUN_SHOTGUN: return haloweaponfind("weapons\\shotgun\\shotgun");
        case GUN_SNIPER: return haloweaponfind("weapons\\sniper rifle\\sniper rifle");
        case GUN_SUBGUN: return haloweaponfind("weapons\\plasma rifle\\plasma rifle");
        default: return nullptr;
    }
}

namespace
{
    constexpr int equipmentFormat = 2;      // tools/halo_weapons.py EQUIPMENT_FORMAT
    std::vector<HaloGrenadeDef> grenadeDefs;
    std::vector<HaloEquipmentDef> equipmentDefs;
    HaloSounds interfaceSounds;
    bool equipmentAttempted = false, equipmentLoaded = false;

    bool parseEquipment(const char *text)
    {
        std::istringstream lines(text);
        std::string line;
        HaloGrenadeDef *grenade = nullptr;
        HaloEquipmentDef *item = nullptr;
        bool versioned = false;
        int number = 0;
        while(std::getline(lines, line))
        {
            ++number;
            if(line.empty() || line[0] == '#') continue;
            std::istringstream in(line);
            std::string key;
            in >> key;
            bool ok = true;
            if(key == "format")
            {
                int version = 0;
                in >> version;
                ok = versioned = version == equipmentFormat;
                if(!ok) conoutf("\f3Halo equipment: packages/halo/equipment.txt is format %d, the game reads format %d; re-run tools/halo_weapons.py", version, equipmentFormat);
            }
            else if(key == "grenade" && !item)
            {
                grenadeDefs.emplace_back();
                grenade = &grenadeDefs.back();
                int sticks = 0;
                ok = bool(in >> grenade->type >> grenade->maximum >> sticks >> grenade->timerStarts >> grenade->arming
                          >> grenade->fuse >> grenade->flags) && grenade->type == int(grenadeDefs.size()) - 1;
                grenade->sticks = sticks != 0;
            }
            else if(key == "equipment" && !grenade && !item)
            {
                equipmentDefs.emplace_back();
                item = &equipmentDefs.back();
                ok = bool(in >> item->index) && item->index == int(equipmentDefs.size()) - 1;
            }
            else if(key == "end") grenade = nullptr, item = nullptr;
            else if(key == "sound")
            {
                std::string role, tag;
                in >> role;
                std::getline(in >> std::ws, tag);
                if(grenade) grenade->sounds.parse(role, tag);
                else if(item) item->sounds.parse(role, tag);
                else if(role == "shield-low") interfaceSounds.samples[0] = halosoundsample(tag);
                else if(role == "health-low") interfaceSounds.samples[1] = halosoundsample(tag);
            }
            else if(grenade)
            {
                if(key == "blast") ok = bool(in >> grenade->innerRadius >> grenade->outerRadius) && parseDamage(in, grenade->blast)
                                        && bool(in >> grenade->sideEffect >> grenade->category);
                else if(key == "attached") ok = parseDamage(in, grenade->attached);
                else if(key == "path") std::getline(in >> std::ws, grenade->path);
                else if(key == "equipment") std::getline(in >> std::ws, grenade->equipment);
            }
            else if(item)
            {
                if(key == "path") { std::getline(in >> std::ws, item->path); item->name = displayName(item->path); }
                else if(key == "kind") ok = bool(in >> item->powerup >> item->grenade >> item->powerupTime);
                else if(key == "worldmodel") in >> item->worldmodel;
                else if(key == "ammo") { HaloEquipmentAmmo a; ok = bool(in >> a.weapon >> a.rounds); if(ok) item->ammo.push_back(a); }
            }
            else ok = false;
            if(!ok)
            {
                conoutf("\f3Halo equipment: invalid line %d in packages/halo/equipment.txt", number);
                grenadeDefs.clear();
                equipmentDefs.clear();
                return false;
            }
        }
        if(!versioned) { grenadeDefs.clear(); equipmentDefs.clear(); }
        return versioned && !grenadeDefs.empty();
    }
}

bool haloequipmentload()
{
    if(equipmentAttempted) return equipmentLoaded;
    equipmentAttempted = true;
    int size = 0;
    char *text = loadfile(path("packages/halo/equipment.txt", true), &size);
    if(!text)
    {
        // Without the table the frag grenade keeps AssaultCube's grenade and there is no plasma grenade.
        conoutf("\f3Halo equipment: packages/halo/equipment.txt is missing; run tools/halo_weapons.py");
        return false;
    }
    equipmentLoaded = parseEquipment(text);
    delete[] text;
    if(equipmentLoaded) conoutf("Halo equipment: %d grenade types, %d equipment definitions", int(grenadeDefs.size()), int(equipmentDefs.size()));
    return equipmentLoaded;
}

const HaloGrenadeDef *halogrenadedef(int type)
{
    haloequipmentload();
    return type >= 0 && type < int(grenadeDefs.size()) ? &grenadeDefs[type] : nullptr;
}

int haloequipmentcount() { haloequipmentload(); return int(equipmentDefs.size()); }

const HaloEquipmentDef *haloequipment(int index)
{
    haloequipmentload();
    return index >= 0 && index < int(equipmentDefs.size()) ? &equipmentDefs[index] : nullptr;
}

const HaloEquipmentDef *haloequipmentfind(const char *path)
{
    haloequipmentload();
    for(const HaloEquipmentDef &d : equipmentDefs) if(d.path == path) return &d;
    return nullptr;
}

const HaloEquipmentDef *halogrenadeequipment(int type)
{
    const HaloGrenadeDef *g = halogrenadedef(type);
    if(!g) return nullptr;
    if(const HaloEquipmentDef *d = haloequipmentfind(g->equipment.c_str())) return d;
    for(const HaloEquipmentDef &d : equipmentDefs) if(d.powerup == HALO_POWERUP_GRENADE && d.grenade == type) return &d;
    return nullptr;
}

const char *halointerfacesound(const char *role)
{
    haloequipmentload();
    if(!strcmp(role, "shield-low")) return interfaceSounds.get(0);
    if(!strcmp(role, "health-low")) return interfaceSounds.get(1);
    return nullptr;
}
