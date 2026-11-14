// Renders the retail HUD from the layouts tools/halo_hud.py exports. Elements
// are sprites on the original HUD sheets, placed in 640x480 HUD units from an
// anchor corner and scaled to AssaultCube's virtual screen (VIRTH = 1800).

#include "cube.h"
#include "halo_hud.h"
#include <sstream>
#include <string>
#include <vector>

namespace
{
    struct vec2 { float x, y; vec2(float x = 0, float y = 0) : x(x), y(y) {} };
    constexpr int formatVersion = 1;
    enum Anchor { TOP_LEFT = 0, TOP_RIGHT, BOTTOM_LEFT, BOTTOM_RIGHT, CENTER };
    enum State { TOTAL_AMMO = 0, LOADED_AMMO, HEAT, AGE };
    enum Role { UNIT_BACKGROUND = 100, SHIELD_BACK, SHIELD_METER, HEALTH_BACK, HEALTH_METER, MOTION_BACK, MOTION_FRONT,
                FLASHLIGHT_BACK, FLASHLIGHT_METER, GRENADE_BACK = 110, GRENADE_COUNT_BACK };
    enum Crosshair { AIM = 0, ZOOM_OVERLAY, CHARGE, SHOULD_RELOAD, FLASH_HEAT, FLASH_TOTAL, FLASH_BATTERY, RELOAD_OVERHEAT,
                     NO_AMMO_FIRED, NO_GRENADE, LOW_AMMO_NONE, BATTERY_DEPLETED_FIRED = 18 };
    enum { ITEM_ONLY_ZOOMED = 4, ITEM_DEFAULT_ZOOM_ONLY = 64 };
    enum { SHOW_FLASHING = 1, SHOW_EMPTY = 2, SHOW_RELOAD = 4, SHOW_DEFAULT = 8, SHOW_ALWAYS = 16 };

    struct Sprite { int page; float left, right, top, bottom, rx, ry; };
    struct Page { int width, height; std::string file; Texture *texture = nullptr; };
    struct Sheet { std::vector<Page> pages; std::vector<std::vector<Sprite>> sequences; };

    struct Placement { int x = 0, y = 0; float sx = 1, sy = 1; int flags = 0; };
    struct Element
    {
        char kind = 's';                    // s static, m meter, n number, c crosshair, o overlay
        int state = 0, sheet = -1, sequence = 0;
        Placement at;
        unsigned colour = 0, flash = 0, low = 0, high = 0, empty = 0;
        float period = 0, translucency = 0;
        int flags = 0, digits = 0, numberFlags = 0, fraction = 0, show = 0, minimum = 0;
    };
    struct Group { int anchor = 0, cutoffs[4] = {}, scope = -1, index = -1; std::vector<Element> elements; };

    std::vector<Sheet> sheets;
    std::vector<Group> weapons, grenades;
    Group unit;
    unsigned healthMid = 0;
    float healthHigh = 0.67f, healthLow = 0.33f;
    int motionX = 41, motionY = 41, sweepSheet = -1, blipSheet = -1;
    float motionRange = 20, motionScale = 32;
    int counterSheet = -1, counterAdvance = 9;
    bool attempted = false, loaded = false;

    float unitScale() { return VIRTH / 480.0f; }

    bool parsePlacement(std::istringstream &in, Placement &p) { return bool(in >> p.x >> p.y >> p.sx >> p.sy >> p.flags); }
    unsigned hex(std::istringstream &in) { std::string s; in >> s; return unsigned(strtoul(s.c_str(), nullptr, 16)); }

    bool parse(const char *text)
    {
        std::istringstream lines(text);
        std::string line;
        Sheet *sheet = nullptr;
        Group *group = nullptr;
        bool versioned = false;
        while(std::getline(lines, line))
        {
            if(line.empty() || line[0] == '#') continue;
            std::istringstream in(line);
            std::string key;
            in >> key;
            if(key == "format") { int v = 0; in >> v; versioned = v == formatVersion; continue; }
            if(key == "bitmap") { int id; in >> id; sheets.resize(max(int(sheets.size()), id + 1)); sheet = &sheets[id]; continue; }
            if(key == "page" && sheet) { Page p; in >> p.width >> p.height >> p.file; sheet->pages.push_back(p); continue; }
            if(key == "sequence" && sheet) { sheet->sequences.emplace_back(); continue; }
            if(key == "sprite" && sheet && !sheet->sequences.empty())
            {
                Sprite s;
                in >> s.page >> s.left >> s.right >> s.top >> s.bottom >> s.rx >> s.ry;
                sheet->sequences.back().push_back(s);
                continue;
            }
            if(key == "weapon") { weapons.emplace_back(); group = &weapons.back(); in >> group->index >> group->anchor; loopi(4) in >> group->cutoffs[i]; in >> group->scope; continue; }
            if(key == "unit") { group = &unit; in >> unit.anchor; continue; }
            if(key == "grenade") { grenades.emplace_back(); group = &grenades.back(); in >> group->index >> group->anchor >> group->cutoffs[0]; continue; }
            if(key == "end") { sheet = nullptr; group = nullptr; continue; }
            if(key == "health") { healthMid = hex(in); in >> healthHigh >> healthLow; continue; }
            if(key == "motion") { in >> motionX >> motionY >> motionRange >> motionScale >> sweepSheet >> blipSheet; continue; }
            if(key == "counter") { int width; in >> counterSheet >> width >> counterAdvance; continue; }
            if(!group) continue;
            Element e;
            e.kind = key[0];
            bool ok = true;
            if(key == "static")
            {
                ok = bool(in >> e.state >> e.sheet >> e.sequence) && parsePlacement(in, e.at);
                e.colour = hex(in); e.flash = hex(in); in >> e.period;
            }
            else if(key == "meter")
            {
                ok = bool(in >> e.state >> e.sheet >> e.sequence) && parsePlacement(in, e.at);
                e.low = hex(in); e.high = hex(in); e.flash = hex(in); e.empty = hex(in);
                int multiplier, bias, scale; float opacity;
                in >> e.flags >> e.minimum >> multiplier >> bias >> scale >> opacity >> e.translucency;
            }
            else if(key == "number")
            {
                ok = bool(in >> e.state) && parsePlacement(in, e.at);
                e.colour = hex(in); e.flash = hex(in);
                in >> e.digits >> e.numberFlags >> e.fraction >> e.flags;
            }
            else if(key == "crosshair")
            {
                ok = bool(in >> e.state >> e.sheet >> e.sequence) && parsePlacement(in, e.at);
                e.colour = hex(in); e.flash = hex(in); int rate; in >> e.period >> rate >> e.flags;
            }
            else if(key == "overlay")
            {
                ok = bool(in >> e.state >> e.sheet >> e.sequence) && parsePlacement(in, e.at);
                e.colour = hex(in); e.flash = hex(in); int rate; in >> rate >> e.show >> e.flags;
            }
            else continue;
            if(!ok) { conoutf("\f3Halo HUD: invalid line in packages/halo/hud.txt: %s", line.c_str()); return false; }
            group->elements.push_back(e);
        }
        return versioned && !sheets.empty();
    }

    const Sprite *sprite(int sheet, int sequence, int frame = 0)
    {
        if(sheet < 0 || sheet >= int(sheets.size())) return nullptr;
        Sheet &s = sheets[sheet];
        if(sequence < 0 || sequence >= int(s.sequences.size()) || s.sequences[sequence].empty()) return nullptr;
        const std::vector<Sprite> &frames = s.sequences[sequence];
        return &frames[clamp(frame, 0, int(frames.size()) - 1)];
    }

    Texture *pageTexture(int sheet, int page)
    {
        Sheet &s = sheets[sheet];
        if(page < 0 || page >= int(s.pages.size())) return nullptr;
        Page &p = s.pages[page];
        if(!p.texture)
        {
            defformatstring(path)("packages/textures/%s", p.file.c_str());
            p.texture = textureload(path, 3, false);
        }
        return p.texture && p.texture != notexture ? p.texture : nullptr;
    }

    // HUD colours are ARGB; an alpha of zero means fully opaque in the HUD tags.
    void setColour(unsigned argb, float alpha = 1)
    {
        const int a = argb >> 24 ? argb >> 24 : 255;
        glColor4ub((argb >> 16) & 255, (argb >> 8) & 255, argb & 255, uchar(clamp(a * alpha, 0.0f, 255.0f)));
    }

    unsigned blend(unsigned from, unsigned to, float t)
    {
        t = clamp(t, 0.0f, 1.0f);
        unsigned out = 0;
        loopi(4)
        {
            const int shift = i * 8;
            const float a = (from >> shift) & 255, b = (to >> shift) & 255;
            out |= unsigned(a + (b - a) * t + 0.5f) << shift;
        }
        return out;
    }

    // Screen position of a placement's top-left corner for an element of the given size.
    vec2 origin(int anchor, const Placement &p, float w, float h)
    {
        const float k = unitScale();
        const float ox = p.x * k, oy = p.y * k, margin = 8 * k;
        switch(anchor)
        {
            case TOP_RIGHT: return vec2(VIRTW - margin - ox - w, margin + oy);
            case BOTTOM_LEFT: return vec2(margin + ox, VIRTH - margin - oy - h);
            case BOTTOM_RIGHT: return vec2(VIRTW - margin - ox - w, VIRTH - margin - oy - h);
            case CENTER: return vec2(VIRTW / 2.0f + ox, VIRTH / 2.0f + oy);
            default: return vec2(margin + ox, margin + oy);
        }
    }

    vec2 spriteSize(int sheet, const Sprite &s, const Placement &p)
    {
        const Page &page = sheets[sheet].pages[clamp(s.page, 0, int(sheets[sheet].pages.size()) - 1)];
        const float k = unitScale();
        return vec2((s.right - s.left) * page.width * p.sx * k, (s.bottom - s.top) * page.height * p.sy * k);
    }

    void quad(Texture *t, const Sprite &s, float x, float y, float w, float h)
    {
        glBindTexture(GL_TEXTURE_2D, t->id);
        glBegin(GL_TRIANGLE_STRIP);
        glTexCoord2f(s.left, s.top); glVertex2f(x, y);
        glTexCoord2f(s.right, s.top); glVertex2f(x + w, y);
        glTexCoord2f(s.left, s.bottom); glVertex2f(x, y + h);
        glTexCoord2f(s.right, s.bottom); glVertex2f(x + w, y + h);
        glEnd();
    }

    bool drawSprite(int anchor, const Element &e, unsigned colour, float alpha = 1)
    {
        const Sprite *s = sprite(e.sheet, e.sequence);
        if(!s) return false;
        Texture *t = pageTexture(e.sheet, s->page);
        if(!t) return false;
        const vec2 size = spriteSize(e.sheet, *s, e.at);
        vec2 at = origin(anchor, e.at, size.x, size.y);
        if(anchor == CENTER)
        {
            // Reticles hang from their registration point at the screen centre.
            const Page &page = sheets[e.sheet].pages[clamp(s->page, 0, int(sheets[e.sheet].pages.size()) - 1)];
            const float k = unitScale();
            at.x -= (s->rx - s->left) * page.width * e.at.sx * k;
            at.y -= (s->ry - s->top) * page.height * e.at.sy * k;
        }
        setColour(colour, alpha);
        quad(t, *s, at.x, at.y, size.x, size.y);
        return true;
    }

    // Meters reveal their sheet's pixels in fill order, which the alpha channel encodes.
    void drawMeter(int anchor, const Element &e, float fraction)
    {
        fraction = clamp(fraction, 0.0f, 1.0f);
        if(fraction <= 0.001f) return;
        unsigned colour = e.flags & 2 ? blend(e.low, e.high, fraction) : e.high;
        glEnable(GL_ALPHA_TEST);
        glAlphaFunc(GL_GEQUAL, max(1.0f - fraction, 1.0f / 255));
        drawSprite(anchor, e, colour, 1 - e.translucency * 0.5f);
        glDisable(GL_ALPHA_TEST);
    }

    // Digits from the counter sheet: 0-9, then the decimal point, colon, minus sign.
    void drawNumber(int anchor, const Element &e, int value, float alpha = 1)
    {
        if(counterSheet < 0) return;
        defformatstring(text)(e.numberFlags & 1 ? "%0*d" : "%*d", max(1, e.digits), max(0, value));
        const int length = int(strlen(text));
        loopi(length)
        {
            if(text[i] == ' ') continue;
            const Sprite *s = sprite(counterSheet, 0, text[i] - '0');
            if(!s) continue;
            Texture *t = pageTexture(counterSheet, s->page);
            if(!t) continue;
            Placement at = e.at;
            at.x += i * counterAdvance;
            const vec2 size = spriteSize(counterSheet, *s, at);
            const vec2 pos = origin(anchor, at, size.x, size.y);
            setColour(e.colour, alpha);
            quad(t, *s, pos.x, pos.y, size.x, size.y);
        }
    }

    float flashPhase(float period)
    {
        if(period <= 0) return 0;
        return 0.5f + 0.5f * sinf(lastmillis / 1000.0f / period * PI2);
    }

    const Group *weaponGroup(int index)
    {
        for(const Group &g : weapons) if(g.index == index) return &g;
        return nullptr;
    }

    bool crosshairActive(const Element &e, const HaloHudWeapon &w, const Group &g)
    {
        const bool zoomed = w.zoom > 0;
        if((e.flags & ITEM_ONLY_ZOOMED) && !zoomed) return false;
        if((e.flags & ITEM_DEFAULT_ZOOM_ONLY) && zoomed) return false;
        switch(e.state)
        {
            case AIM: return !zoomed || !(e.flags & ITEM_DEFAULT_ZOOM_ONLY);
            case ZOOM_OVERLAY: return zoomed;
            case CHARGE: return w.charging;
            case SHOULD_RELOAD: return !w.battery_weapon && w.loaded <= max(1, w.magazine * g.cutoffs[1] / 100) && w.reserve > 0 && !w.reloading;
            case LOW_AMMO_NONE: return !w.battery_weapon && w.loaded <= max(1, w.magazine * g.cutoffs[1] / 100) && w.reserve <= 0;
            case FLASH_HEAT: return w.battery_weapon && w.heat * 100 >= g.cutoffs[2] && g.cutoffs[2] > 0;
            case FLASH_BATTERY: return w.battery_weapon && w.battery * 100 <= g.cutoffs[3] && w.battery > 0;
            case BATTERY_DEPLETED_FIRED: return w.battery_weapon && w.battery <= 0;
            case RELOAD_OVERHEAT: return w.reloading || w.overheated;
            default: return false;
        }
    }

    void drawWeapon(const HaloHudWeapon &w)
    {
        const Group *g = weaponGroup(w.weapon);
        if(!g) return;
        const bool zoomed = w.zoom > 0;
        // Scoped weapons draw their screen mask over the view first.
        if(zoomed && g->scope >= 0)
        {
            Element mask;
            mask.sheet = g->scope;
            if(const Sprite *s = sprite(mask.sheet, 0))
                if(Texture *t = pageTexture(mask.sheet, s->page)) { setColour(0xff000000); quad(t, *s, 0, 0, VIRTW, VIRTH); }
        }
        for(const Element &e : g->elements)
        {
            const float value = e.state == TOTAL_AMMO ? (w.maximum > 0 ? float(w.reserve + w.loaded) / w.maximum : 0)
                              : e.state == LOADED_AMMO ? (w.magazine > 0 ? float(w.loaded) / w.magazine : 0)
                              : e.state == HEAT ? w.heat : e.state == AGE ? w.battery : 0;
            switch(e.kind)
            {
                case 's': if(e.colour) drawSprite(g->anchor, e, e.colour); break;
                case 'm': drawMeter(g->anchor, e, value); break;
                case 'n':
                {
                    if((e.numberFlags & 2) && !zoomed) break;
                    int number = e.state == TOTAL_AMMO ? w.reserve : e.state == LOADED_AMMO ? w.loaded
                               : e.state == AGE ? int(w.battery * 100 + .5f) : e.state == HEAT ? int(w.heat * 100) : 0;
                    if(e.flags & 1) number = w.magazine > 0 ? (w.reserve + w.magazine - 1) / w.magazine : 0;  // magazines left
                    drawNumber(g->anchor, e, number);
                    break;
                }
                case 'o':
                {
                    const bool empty = w.battery_weapon ? w.battery <= 0 : w.loaded + w.reserve <= 0;
                    const bool show = (e.show & SHOW_ALWAYS) || ((e.show & SHOW_DEFAULT) && !empty && !w.reloading) ||
                                      ((e.show & SHOW_EMPTY) && empty) || ((e.show & SHOW_RELOAD) && w.reloading);
                    if(show) drawSprite(g->anchor, e, e.colour);
                    break;
                }
                case 'c':
                {
                    if(!crosshairActive(e, w, *g)) break;
                    // Warnings flash; the aim reticle turns red on an aim-assist target.
                    unsigned colour = e.state == AIM && w.locked ? 0x00ff2020 : e.colour ? e.colour : e.flash;
                    float alpha = 1;
                    if(e.state != AIM && e.state != ZOOM_OVERLAY) alpha = 0.35f + 0.65f * flashPhase(max(e.period, 0.5f));
                    drawSprite(CENTER, e, colour, alpha);
                    break;
                }
            }
        }
    }

    void drawUnit(const HaloHudUnit &u, const HaloHudBlip *blips, int blipCount)
    {
        const float shieldFlash = u.shield < 0.25f ? flashPhase(0.5f) : 0;
        for(const Element &e : unit.elements)
        {
            const int anchor = e.state == MOTION_BACK || e.state == MOTION_FRONT ? BOTTOM_LEFT : unit.anchor;
            switch(e.state)
            {
                case SHIELD_BACK: case HEALTH_BACK:
                    drawSprite(anchor, e, shieldFlash > 0 && e.state == SHIELD_BACK ? blend(e.colour, e.flash, shieldFlash) : e.colour);
                    break;
                case SHIELD_METER:
                    drawMeter(anchor, e, u.shield);
                    break;
                case HEALTH_METER:
                {
                    // Health shades from the low colour through the mid colour to full.
                    Element shaded = e;
                    shaded.flags &= ~2;
                    shaded.high = u.health >= healthHigh ? e.high : u.health >= healthLow ? healthMid : e.low;
                    drawMeter(anchor, shaded, u.health);
                    break;
                }
                case MOTION_BACK:
                {
                    drawSprite(anchor, e, e.colour ? e.colour : 0xff2896ff, 0.85f);
                    // Contacts inside the sensor's range, as blips around the centre.
                    const Sprite *dot = sprite(blipSheet, 0);
                    Texture *t = dot ? pageTexture(blipSheet, dot->page) : nullptr;
                    const float k = unitScale();
                    const vec2 centre = origin(BOTTOM_LEFT, Placement{motionX, motionY}, 0, 0);
                    const float radius = motionScale * k;
                    loopi(blipCount)
                    {
                        const HaloHudBlip &b = blips[i];
                        const float d = sqrtf(b.x * b.x + b.y * b.y);
                        if(d > motionRange || !t) continue;
                        const float x = centre.x + b.x / motionRange * radius, y = centre.y + b.y / motionRange * radius;
                        const float size = 9 * k;
                        setColour(b.ally ? 0xffffdc28 : 0xffff3030);
                        quad(t, *dot, x - size / 2, y - size / 2, size, size);
                    }
                    break;
                }
                case MOTION_FRONT: drawSprite(anchor, e, e.colour); break;
                case FLASHLIGHT_BACK: if(u.flashlight) drawSprite(anchor, e, e.colour); break;
                case FLASHLIGHT_METER: if(u.flashlight) drawMeter(anchor, e, u.flashlightCharge); break;
                default: break;
            }
        }
        for(const Group &g : grenades)
        {
            if(g.index != u.grenadeType) continue;
            for(const Element &e : g.elements)
            {
                if(e.kind == 's' && e.colour) drawSprite(g.anchor, e, e.colour);
                else if(e.kind == 'n') drawNumber(g.anchor, e, u.grenades);
                else if(e.kind == 'o' && ((e.show & 8) || (e.show & 4) || (u.grenades <= 0 && (e.show & 2))))
                    drawSprite(g.anchor, e, u.grenades <= g.cutoffs[0] ? e.flash : e.colour);
            }
        }
    }
}

bool halohudload()
{
    if(attempted) return loaded;
    attempted = true;
    int size = 0;
    char *text = loadfile(path("packages/halo/hud.txt", true), &size);
    if(!text) { conoutf("\f3Halo HUD: packages/halo/hud.txt is missing; run tools/halo_hud.py"); return false; }
    loaded = parse(text);
    delete[] text;
    if(loaded) conoutf("Halo HUD: %d sprite sheets, %d weapon HUDs", int(sheets.size()), int(weapons.size()));
    return loaded;
}

bool halohudready() { return halohudload(); }

void halohuddraw(const HaloHudWeapon &weapon, const HaloHudUnit &u, const HaloHudBlip *blips, int blipCount)
{
    if(!halohudload()) return;
    glEnable(GL_TEXTURE_2D);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    drawUnit(u, blips, blipCount);
    drawWeapon(weapon);
    glColor4f(1, 1, 1, 1);
}
