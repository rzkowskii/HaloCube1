#ifndef HALO_WORLD_H
#define HALO_WORLD_H

#include <memory>
#include <unordered_map>
#include <vector>

namespace
{
    enum HaloSurface { HALO_SOLID, HALO_CUTOUT, HALO_WATER, HALO_SKY, HALO_ADDITIVE, HALO_SKY_ADDITIVE, HALO_FOLIAGE };

    struct HaloWorldVertex
    {
        float x, y, z, u, v;
        uchar r, g, b, a;
    };

    struct HaloWorldBatch
    {
        Texture *texture;
        uint flags;
        float detailScale;
        std::vector<HaloWorldVertex> vertices;
        Texture *lightmap = nullptr;
        std::vector<float> lightUV;
    };

    struct HaloCorona
    {
        Texture *texture;
        vec position, direction, color;
        float radius, brightness, falloff, cutoff;
    };

    struct HaloNozzle { vec center; float radius; };

    static_assert(sizeof(HaloWorldVertex) == 24, "Halo world mesh vertex layout");
    std::vector<HaloWorldBatch> haloWorld;
    std::vector<HaloCorona> haloCoronas;
    std::vector<HaloNozzle> haloNozzles;
    string haloWorldMap = "";
    Texture *haloGrassDetail = NULL;
    std::unordered_map<uint64_t, std::vector<const HaloWorldVertex *>> haloGround;
    std::vector<const HaloWorldVertex *> haloWideGround;

    uint64_t groundKey(int x, int y)
    {
        return uint64_t(uint32_t(x)) << 32 | uint32_t(y);
    }

    int groundCell(float x) { return int(floorf(x / 24)); }

    void indexHaloGround()
    {
        for(const HaloWorldBatch &batch : haloWorld)
        {
            if(batch.flags != HALO_SOLID) continue;
            for(size_t i = 0; i < batch.vertices.size(); i += 3)
            {
                const HaloWorldVertex *v = &batch.vertices[i];
                float slope = (v[1].x - v[0].x) * (v[2].y - v[0].y) - (v[1].y - v[0].y) * (v[2].x - v[0].x);
                float nx = (v[1].y - v[0].y) * (v[2].z - v[0].z) - (v[1].z - v[0].z) * (v[2].y - v[0].y);
                float ny = (v[1].z - v[0].z) * (v[2].x - v[0].x) - (v[1].x - v[0].x) * (v[2].z - v[0].z);
                if(slope <= .001f || nx*nx + ny*ny > 4.84f*slope*slope) continue;
                int x0 = groundCell(min(v[0].x, min(v[1].x, v[2].x)));
                int x1 = groundCell(max(v[0].x, max(v[1].x, v[2].x)));
                int y0 = groundCell(min(v[0].y, min(v[1].y, v[2].y)));
                int y1 = groundCell(max(v[0].y, max(v[1].y, v[2].y)));
                if(x1 - x0 > 12 || y1 - y0 > 12) haloWideGround.push_back(v);
                else for(int y = y0; y <= y1; ++y) for(int x = x0; x <= x1; ++x)
                    haloGround[groundKey(x, y)].push_back(v);
            }
        }
    }

    bool loadHaloWorld(std::vector<HaloWorldBatch> &batches, std::vector<HaloCorona> &coronas,
                       std::vector<HaloNozzle> &nozzles, const char *map)
    {
        defformatstring(meshPath)("packages/maps/%s.mesh", map);
        defformatstring(texturePrefix)("packages/textures/%s/bsp/material_", map);
        std::unique_ptr<stream> file(openfile(meshPath, "rb"));
        if(!file || file->size() > 48 * 1024 * 1024) return false;
        char signature[4];
        if(file->read(signature, sizeof(signature)) != sizeof(signature)) return false;
        const bool flyby = !memcmp(signature, "HWM4", sizeof(signature));
        const bool hasCoronas = flyby || !memcmp(signature, "HWM3", sizeof(signature));
        if(!hasCoronas && memcmp(signature, "HWM2", sizeof(signature))) return false;
        const uint count = file->getlil<uint>();
        if(!count || count > 128) return false;
        batches.reserve(count);
        for(uint i = 0; i < count; ++i)
        {
            char texture[96];
            if(file->read(texture, sizeof(texture)) != sizeof(texture) || !memchr(texture, '\0', sizeof(texture))) return false;
            const size_t prefixLength = strlen(texturePrefix);
            if(strncmp(texture, texturePrefix, prefixLength) || strlen(texture + prefixLength) != 6) return false;
            if(texture[prefixLength] < '0' || texture[prefixLength] > '9' ||
               texture[prefixLength+1] < '0' || texture[prefixLength+1] > '9' || strcmp(texture + prefixLength + 2, ".png")) return false;
            const uint flags = file->getlil<uint>(), vertices = file->getlil<uint>();
            const float detailScale = file->getlil<float>();
            const int lightIndex = flyby ? file->getlil<int>() : -1;
            if(flags > HALO_FOLIAGE || !vertices || vertices % 3 || vertices > 200000 ||
               !std::isfinite(detailScale) || detailScale < 0 || detailScale > 400 ||
               lightIndex < -1 || lightIndex > 99) return false;
            HaloWorldBatch batch = { textureload(texture), flags, detailScale, {} };
            if(!batch.texture || batch.texture == notexture) return false;
            batch.vertices.resize(vertices);
            const int bytes = int(vertices * sizeof(HaloWorldVertex));
            if(lightIndex >= 0)
            {
                defformatstring(path)("packages/textures/%s/bsp/lightmap_%02d.png", map, lightIndex);
                batch.lightmap = textureload(path, 3, false);
                if(!batch.lightmap || batch.lightmap == notexture) return false;
                batch.lightUV.resize(vertices * 2);
                for(uint j = 0; j < vertices; ++j)
                {
                    HaloWorldVertex &vertex = batch.vertices[j];
                    float *uv = &batch.lightUV[j * 2];
                    if(file->read(&vertex, sizeof(vertex)) != sizeof(vertex) ||
                       file->read(uv, 2 * sizeof(float)) != 2 * sizeof(float)) return false;
                    lilswap(uv, 2);
                    if(!std::isfinite(uv[0]) || !std::isfinite(uv[1])) return false;
                }
            }
            else if(file->read(batch.vertices.data(), bytes) != bytes) return false;
            for(HaloWorldVertex &vertex : batch.vertices)
            {
                lilswap(&vertex.x, 5);
                if(!std::isfinite(vertex.x) || !std::isfinite(vertex.y) || !std::isfinite(vertex.z) ||
                   !std::isfinite(vertex.u) || !std::isfinite(vertex.v)) return false;
            }
            batches.push_back(std::move(batch));
        }
        if(hasCoronas)
        {
            const uint count = file->getlil<uint>();
            if(count > 64) return false;
            for(uint i = 0; i < count; ++i)
            {
                char texture[96];
                defformatstring(path)("packages/textures/%s/bsp/corona_", map);
                if(file->read(texture, sizeof(texture)) != sizeof(texture) ||
                   !memchr(texture, '\0', sizeof(texture)) || strncmp(texture, path, strlen(path))) return false;
                float values[13];
                if(file->read(values, sizeof(values)) != sizeof(values)) return false;
                lilswap(values, 13);
                for(float value : values) if(!std::isfinite(value)) return false;
                Texture *image = textureload(texture, 3, false);
                if(image && image != notexture)
                    coronas.push_back({image, vec(values), vec(values + 3), vec(values + 8),
                                       values[6], values[7], values[11], values[12]});
            }
        }
        if(flyby)
        {
            const uint count = file->getlil<uint>();
            if(count > 64) return false;
            for(uint i = 0; i < count; ++i)
            {
                float values[4];
                if(file->read(values, sizeof(values)) != sizeof(values)) return false;
                lilswap(values, 4);
                for(float value : values) if(!std::isfinite(value)) return false;
                if(values[3] <= 0 || values[3] > 200) return false;
                nozzles.push_back({vec(values), values[3]});
            }
        }
        return file->tell() == file->size();
    }

    bool hasHaloWorld()
    {
        if(!haloactive() || editmode)
        {
            haloWorldMap[0] = 0;
            haloWorld.clear();
            haloCoronas.clear();
            haloNozzles.clear();
            haloGround.clear();
            haloWideGround.clear();
            return false;
        }
        const char *scene = halocinematicworld();
        const char *map = *scene ? scene : behindpath(getclientmap());
        if(strcmp(haloWorldMap, map))
        {
            copystring(haloWorldMap, map);
            haloGround.clear();
            haloWideGround.clear();
            std::vector<HaloWorldBatch> batches;
            haloCoronas.clear();
            haloNozzles.clear();
            std::vector<HaloCorona> coronas;
            std::vector<HaloNozzle> nozzles;
            if(loadHaloWorld(batches, coronas, nozzles, map))
            {
                haloWorld.swap(batches);
                haloCoronas.swap(coronas);
                haloNozzles.swap(nozzles);
                if(*scene) indexHaloGround();
                haloGrassDetail = !strcmp(behindpath(getclientmap()), "halo_a30") ?
                    textureload("packages/textures/halo_a30/bsp/detail_grass.png") : NULL;
                conoutf("Loaded Halo BSP geometry");
            }
            else haloWorld.clear();
        }
        return !haloWorld.empty();
    }

    void drawHaloNozzles()
    {
        if(haloNozzles.empty()) return;
        glDisable(GL_TEXTURE_2D);
        glDisable(GL_CULL_FACE);
        for(const HaloNozzle &nozzle : haloNozzles)
        {
            if(camera1->o.y >= nozzle.center.y) continue;
            glBegin(GL_TRIANGLE_FAN);
            glColor3ub(5, 8, 14);
            glVertex3f(nozzle.center.x, nozzle.center.y + nozzle.radius * .035f, nozzle.center.z);
            glColor3ub(4, 6, 10);
            for(int i = 0; i <= 64; ++i)
            {
                float angle = i * 2 * PI / 64;
                glVertex3f(nozzle.center.x + cosf(angle) * nozzle.radius, nozzle.center.y,
                           nozzle.center.z + sinf(angle) * nozzle.radius);
            }
            glEnd();
        }
        glColor4ub(255, 255, 255, 255);
        glEnable(GL_CULL_FACE);
        glEnable(GL_TEXTURE_2D);
    }

    void drawHaloCoronas(const vec &cameraRight, const vec &cameraUp, const vec &cameraForward)
    {
        if(haloCoronas.empty()) return;
        glDisable(GL_CULL_FACE);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE);
        glDepthMask(GL_FALSE);
        glDisable(GL_DEPTH_TEST);
        for(const HaloCorona &corona : haloCoronas)
        {
            vec toward = vec(camera1->o).sub(corona.position);
            if(toward.squaredlen() < 1 || corona.falloff <= corona.cutoff) continue;
            float facing = toward.normalize().dot(corona.direction);
            float gain = clamp((facing - corona.cutoff) / (corona.falloff - corona.cutoff), 0.0f, 1.0f);
            if(gain <= 0) continue;
            if(vec(corona.position).sub(camera1->o).dot(cameraForward) <= 0) continue;
            const vec &center = corona.position;
            vec right = vec(cameraRight).mul(corona.radius), up = vec(cameraUp).mul(corona.radius);
            vec points[] = {vec(center).sub(right).sub(up), vec(center).add(right).sub(up),
                            vec(center).sub(right).add(up), vec(center).add(right).add(up)};
            glBindTexture(GL_TEXTURE_2D, corona.texture->id);
            glColor4f(corona.color.x, corona.color.y, corona.color.z, sqrtf(gain) * corona.brightness);
            glBegin(GL_TRIANGLE_STRIP);
            for(int i = 0; i < 4; ++i)
            {
                glTexCoord2f(i & 1, 1 - (i >> 1));
                glVertex3fv(points[i].v);
            }
            glEnd();
        }
        glColor4ub(255, 255, 255, 255);
        glEnable(GL_DEPTH_TEST);
        glDepthMask(GL_TRUE);
        glDisable(GL_BLEND);
        glEnable(GL_CULL_FACE);
    }

    void drawHaloWorld(HaloSurface pass)
    {
        const bool additive = pass == HALO_ADDITIVE || pass == HALO_SKY_ADDITIVE;
        const bool translucent = additive || pass == HALO_WATER || pass == HALO_SKY;
        glEnableClientState(GL_VERTEX_ARRAY);
        glEnableClientState(GL_TEXTURE_COORD_ARRAY);
        glEnableClientState(GL_COLOR_ARRAY);
        if(translucent)
        {
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, additive ? GL_ONE : GL_ONE_MINUS_SRC_ALPHA);
            glDepthMask(GL_FALSE);
        }
        for(const HaloWorldBatch &batch : haloWorld)
        {
            bool cutout = batch.flags == HALO_CUTOUT || batch.flags == HALO_FOLIAGE;
            if(batch.flags != pass && !(pass == HALO_SOLID && cutout)) continue;
            // Xbox BSP surfaces use clockwise front faces.
            if(batch.lightmap) glEnable(GL_CULL_FACE);
            else glDisable(GL_CULL_FACE);
            if(cutout)
            {
                glEnable(GL_ALPHA_TEST);
                glAlphaFunc(GL_GREATER, 0.4f);
            }
            else glDisable(GL_ALPHA_TEST);
            glBindTexture(GL_TEXTURE_2D, batch.texture->id);
            const HaloWorldVertex &v = batch.vertices.front();
            glVertexPointer(3, GL_FLOAT, sizeof(v), &v.x);
            glTexCoordPointer(2, GL_FLOAT, sizeof(v), &v.u);
            glColorPointer(4, GL_UNSIGNED_BYTE, sizeof(v), &v.r);
            if(batch.lightmap && hasMT && maxtmus >= 2)
            {
                glActiveTexture_(GL_TEXTURE1_ARB);
                glEnable(GL_TEXTURE_2D);
                glBindTexture(GL_TEXTURE_2D, batch.lightmap->id);
                resettmu(1);
                glClientActiveTexture_(GL_TEXTURE1_ARB);
                glEnableClientState(GL_TEXTURE_COORD_ARRAY);
                glTexCoordPointer(2, GL_FLOAT, 0, batch.lightUV.data());
                glDrawArrays(GL_TRIANGLES, 0, GLsizei(batch.vertices.size()));
                glDisableClientState(GL_TEXTURE_COORD_ARRAY);
                glClientActiveTexture_(GL_TEXTURE0_ARB);
                glDisable(GL_TEXTURE_2D);
                glActiveTexture_(GL_TEXTURE0_ARB);
            }
            else if(batch.detailScale && hasMT && hasTE && haloGrassDetail && haloGrassDetail != notexture)
            {
                glActiveTexture_(GL_TEXTURE1_ARB);
                glEnable(GL_TEXTURE_2D);
                glBindTexture(GL_TEXTURE_2D, haloGrassDetail->id);
                setuptmu(1, "P * T x 2");
                glMatrixMode(GL_TEXTURE);
                glPushMatrix();
                glScalef(batch.detailScale, batch.detailScale, 1.0f);
                glMatrixMode(GL_MODELVIEW);
                glClientActiveTexture_(GL_TEXTURE1_ARB);
                glEnableClientState(GL_TEXTURE_COORD_ARRAY);
                glTexCoordPointer(2, GL_FLOAT, sizeof(v), &v.u);
                glDrawArrays(GL_TRIANGLES, 0, GLsizei(batch.vertices.size()));
                glDisableClientState(GL_TEXTURE_COORD_ARRAY);
                glClientActiveTexture_(GL_TEXTURE0_ARB);
                glMatrixMode(GL_TEXTURE);
                glPopMatrix();
                glMatrixMode(GL_MODELVIEW);
                resettmu(1);
                glDisable(GL_TEXTURE_2D);
                glActiveTexture_(GL_TEXTURE0_ARB);
            }
            else glDrawArrays(GL_TRIANGLES, 0, GLsizei(batch.vertices.size()));
        }
        if(translucent)
        {
            glDepthMask(GL_TRUE);
            glDisable(GL_BLEND);
        }
        glDisable(GL_ALPHA_TEST);
        glEnable(GL_CULL_FACE);
        glDisableClientState(GL_VERTEX_ARRAY);
        glDisableClientState(GL_TEXTURE_COORD_ARRAY);
        glDisableClientState(GL_COLOR_ARRAY);
        glColor4ub(255, 255, 255, 255);
    }
}

#endif
