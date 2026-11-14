#include "cube.h"
#include "halo.h"

namespace
{
    Texture *digits[10];
    int roundsLoaded = 60;
}

void haloupdatecounter(int rounds)
{
    roundsLoaded = clamp(rounds, 0, 60);
    if(digits[0]) return;

    GLint previousTexture;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &previousTexture);
    loopi(10)
    {
        defformatstring(path)("packages/models/halo/ma5b/digit_%d.png", i);
        digits[i] = textureload(path, 3);
    }
    glBindTexture(GL_TEXTURE_2D, previousTexture);
}

Texture *halocountertexture(const char *mesh, Texture *fallback)
{
    int digit;
    if(!strcmp(mesh, "counter_units")) digit = roundsLoaded % 10;
    else if(!strcmp(mesh, "counter_tens")) digit = roundsLoaded / 10;
    else return fallback;
    Texture *image = digits[digit];
    return image && image != notexture ? image : fallback;
}
