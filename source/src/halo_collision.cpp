#include "cube.h"
#include "halo.h"
#include "halo_collision.h"

#include <algorithm>
#include <memory>
#include <vector>

namespace
{
    constexpr unsigned twoSided = 1;
    constexpr float contactSlop = 0.0025f;
    constexpr float small = 1e-6f;

    struct Bounds
    {
        vec low, high;

        void include(const vec &point)
        {
            loopi(3)
            {
                low[i] = min(low[i], point[i]);
                high[i] = max(high[i], point[i]);
            }
        }

        void include(const Bounds &other)
        {
            include(other.low);
            include(other.high);
        }
    };

    struct Vertex
    {
        vec point, inward;
    };

    struct Surface
    {
        Bounds bounds;
        vec normal;
        unsigned first, count, flags;
        int index, material;
    };

    struct Node
    {
        Bounds bounds;
        unsigned first, count, left, right;
    };

    struct CollisionMesh
    {
        std::vector<Vertex> vertices;
        std::vector<Surface> surfaces;
        std::vector<Node> nodes;

        unsigned build(unsigned first, unsigned count)
        {
            Bounds bounds = surfaces[first].bounds;
            for(unsigned i = first + 1; i < first + count; ++i) bounds.include(surfaces[i].bounds);
            unsigned index = unsigned(nodes.size());
            nodes.push_back({bounds, first, count, 0, 0});
            if(count <= 8) return index;

            vec extent = vec(bounds.high).sub(bounds.low);
            int axis = extent.x > extent.y ? 0 : 1;
            if(extent.z > extent[axis]) axis = 2;
            unsigned middle = first + count/2;
            std::nth_element(surfaces.begin() + first, surfaces.begin() + middle, surfaces.begin() + first + count,
                [axis](const Surface &a, const Surface &b)
                {
                    return a.bounds.low[axis] + a.bounds.high[axis] < b.bounds.low[axis] + b.bounds.high[axis];
                });
            unsigned left = build(first, middle - first), right = build(middle, first + count - middle);
            nodes[index] = {bounds, 0, 0, left, right};
            return index;
        }
    };

    CollisionMesh mesh;

    bool finite(const vec &value)
    {
        return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
    }

    bool readvec(stream &file, vec &value)
    {
        const int bytes = 3 * sizeof(float);
        if(file.read(value.v, bytes) != bytes) return false;
        lilswap(value.v, 3);
        return finite(value);
    }

    bool loadmesh(stream &file, CollisionMesh &data)
    {
        const long bytes = file.size();
        if(bytes < 8 || bytes > 32*1024*1024) return false;
        char signature[4];
        if(file.read(signature, sizeof(signature)) != sizeof(signature) || memcmp(signature, "HCL1", sizeof(signature))) return false;
        const unsigned count = file.getlil<uint>();
        if(!count || count > 131072 || count > unsigned((bytes - 8)/56)) return false;
        data.vertices.reserve(count * 4);
        data.surfaces.reserve(count);
        for(unsigned i = 0; i < count; ++i)
        {
            Surface surface = {};
            surface.index = int(i);
            if(!readvec(file, surface.normal)) return false;
            float norm = surface.normal.squaredlen();
            if(norm < 0.98f || norm > 1.02f) return false;
            surface.normal.normalize();
            surface.flags = file.getlil<uchar>();
            file.getlil<uchar>(); // breakable surface index
            surface.material = file.getlil<short>();
            surface.count = file.getlil<uint>();
            surface.first = unsigned(data.vertices.size());
            if(surface.count < 3 || surface.count > 8 || surface.flags & ~15u) return false;
            for(unsigned j = 0; j < surface.count; ++j)
            {
                vec vertex;
                if(!readvec(file, vertex) || vertex.squaredlen() > 1e12f) return false;
                if(j == 0) surface.bounds = {vertex, vertex};
                else surface.bounds.include(vertex);
                data.vertices.push_back({vertex, vec()});
            }
            float area = 0;
            Vertex *vertices = data.vertices.data() + surface.first;
            for(unsigned j = 0; j < surface.count; ++j)
            {
                Vertex &start = vertices[j];
                const Vertex &end = vertices[(j+1)%surface.count];
                vec edge = vec(end.point).sub(start.point);
                start.inward.cross(surface.normal, edge);
                if(start.inward.squaredlen() > small*small) start.inward.normalize();
                area += vec().cross(vec(start.point).sub(vertices[0].point), vec(end.point).sub(vertices[0].point)).dot(surface.normal);
            }
            if(area <= small) continue;
            data.surfaces.push_back(surface);
        }
        if(file.tell() != bytes || data.surfaces.empty()) return false;
        data.build(0, unsigned(data.surfaces.size()));
        return true;
    }

    struct Path
    {
        vec start, delta;
        float radius, height;

        bool intersects(const Bounds &bounds, float limit) const
        {
            float enter = 0;
            loopi(3)
            {
                float lower = bounds.low[i] - contactSlop - (i == 2 ? height : radius);
                float upper = bounds.high[i] + contactSlop + (i == 2 ? 0.0f : radius);
                if(delta[i] == 0)
                {
                    if(start[i] < lower || start[i] > upper) return false;
                    continue;
                }
                float t1 = (lower - start[i])/delta[i], t2 = (upper - start[i])/delta[i];
                enter = max(enter, min(t1, t2));
                limit = min(limit, max(t1, t2));
                if(enter > limit) return false;
            }
            return true;
        }
    };

    template<class Query>
    void visit(unsigned index, const Path &path, HaloCollisionHit &hit, const Query &query)
    {
        const Node &node = mesh.nodes[index];
        if(!path.intersects(node.bounds, hit.fraction)) return;
        if(node.count)
        {
            for(unsigned i = node.first; i < node.first + node.count; ++i)
            {
                const Surface &surface = mesh.surfaces[i];
                if(path.intersects(surface.bounds, hit.fraction)) query(surface);
            }
        }
        else
        {
            visit(node.left, path, hit, query);
            visit(node.right, path, hit, query);
        }
    }

    void recordhit(HaloCollisionHit &hit, const Surface &surface, float fraction, const vec &normal, float penetration = 0)
    {
        hit.fraction = fraction;
        hit.normal = normal;
        hit.material = surface.material;
        hit.flags = surface.flags;
        hit.penetration = penetration;
        hit.surface = surface.index;
    }

    vec segmentgap(const vec &origin, const vec &delta, const vec &start, const vec &end)
    {
        vec edge = vec(end).sub(start), separation = vec(origin).sub(start);
        float a = delta.squaredlen(), e = edge.squaredlen();
        float s = 0, t = 0;
        if(a <= small*small)
        {
            if(e > small*small) t = clamp(edge.dot(separation)/e, 0.0f, 1.0f);
        }
        else
        {
            float c = delta.dot(separation);
            if(e <= small*small) s = clamp(-c/a, 0.0f, 1.0f);
            else
            {
                float b = delta.dot(edge), f = edge.dot(separation);
                float determinant = a*e - b*b;
                if(determinant > small*small*a*e) s = clamp((b*f - c*e)/determinant, 0.0f, 1.0f);
                t = clamp((b*s + f)/e, 0.0f, 1.0f);
                s = clamp((b*t - c)/a, 0.0f, 1.0f);
            }
        }
        return separation.add(vec(delta).mul(s)).sub(edge.mul(t));
    }

    vec capsulegap(const Surface &surface, const vec &origin, const vec &axis)
    {
        const Vertex *vertices = mesh.vertices.data() + surface.first;
        const vec &normal = surface.normal;
        // Clip the capsule's central segment to the prism above the polygon.
        // Where it overlaps the prism, the nearest interior point is on the face.
        float enter = 0, leave = 1;
        for(unsigned i = 0; i < surface.count; ++i)
        {
            const Vertex &vertex = vertices[i];
            float distance = vec(origin).sub(vertex.point).dot(vertex.inward);
            float direction = axis.dot(vertex.inward);
            if(direction > small) enter = max(enter, -distance/direction);
            else if(direction < -small) leave = min(leave, -distance/direction);
            else if(distance < 0) leave = -1;
            if(enter > leave) break;
        }

        vec gap;
        float best = 1e30f;
        if(enter <= leave)
        {
            float distance = vec(origin).sub(vertices[0].point).dot(normal), direction = axis.dot(normal);
            float t = fabsf(direction) > small ? clamp(-distance/direction, enter, leave) : enter;
            gap = vec(normal).mul(distance + direction*t);
            best = gap.squaredlen();
        }
        // The remaining nearest points lie on the original boundary edges.
        for(unsigned i = 0; i < surface.count && best > small*small; ++i)
        {
            vec next = segmentgap(origin, axis, vertices[i].point, vertices[(i+1)%surface.count].point);
            float distance = next.squaredlen();
            if(distance < best)
            {
                gap = next;
                best = distance;
            }
        }
        return gap;
    }

    vec contactnormal(const Surface &surface, const vec &origin, const vec &axis, const vec &gap, float distance)
    {
        if(distance > small) return vec(gap).div(distance);
        const vec center = vec(origin).add(vec(axis).mul(0.5f));
        bool back = vec(center).sub(mesh.vertices[surface.first].point).dot(surface.normal) < 0;
        return back && (surface.flags & twoSided) ? -surface.normal : surface.normal;
    }

    bool frontcontact(const Surface &surface, const vec &normal)
    {
        return (surface.flags & twoSided) || normal.dot(surface.normal) >= -small;
    }

    bool trace(const vec &from, const vec &to, HaloCollisionHit &hit, unsigned ignoreFlags, bool allowBackfaces)
    {
        const Path path = {from, vec(to).sub(from), 0, 0};
        if(!finite(path.delta) || path.delta.iszero()) return false;
        visit(0, path, hit, [&](const Surface &surface)
        {
            if(surface.flags & ignoreFlags) return;
            const Vertex *vertices = mesh.vertices.data() + surface.first;
            float direction = path.delta.dot(surface.normal);
            if(fabsf(direction) <= small || (direction > 0 && !(surface.flags & twoSided) && !allowBackfaces)) return;
            float fraction = vec(vertices[0].point).sub(from).dot(surface.normal)/direction;
            if(fraction < 0 || fraction > hit.fraction) return;
            vec point = vec(from).add(vec(path.delta).mul(fraction));
            for(unsigned i = 0; i < surface.count; ++i)
            {
                if(vec(point).sub(vertices[i].point).dot(vertices[i].inward) < -contactSlop) return;
            }
            vec normal = direction > 0 && !allowBackfaces ? -surface.normal : surface.normal;
            recordhit(hit, surface, fraction, normal);
        });
        return hit.surface >= 0;
    }
}

void halocollisionmap(const char *name)
{
    mesh = {};
    if(!name) return;
    name = behindpath(name);
    if(strcmp(name, "halo_b30_01") && strcmp(name, "halo_b30_02") && strcmp(name, "halo_b30_03")) return;
    // The two outdoor sections use the same Xbox BSP and projection.
    const char *asset = !strcmp(name, "halo_b30_03") ? "halo_b30_01" : name;
    defformatstring(path)("packages/maps/%s.hcol", asset);
    std::unique_ptr<stream> file(openfile(path, "rb"));
    CollisionMesh loaded;
    if(!file || !loadmesh(*file, loaded))
    {
        conoutf("Halo: retail collision unavailable for %s", name);
        return;
    }
    mesh = std::move(loaded);
    conoutf("Halo: loaded %u retail collision surfaces for %s", unsigned(mesh.surfaces.size()), name);
}

bool halocollisionactive()
{
    return !mesh.nodes.empty() && haloactive() && !editmode;
}

bool halocollisiontrace(const vec &from, const vec &to, HaloCollisionHit &hit, unsigned ignoreFlags)
{
    hit = {};
    return halocollisionactive() && finite(from) && finite(to) && trace(from, to, hit, ignoreFlags, false);
}

bool halocollisionopen(const vec &point)
{
    if(!halocollisionactive() || !finite(point)) return false;
    const Bounds &bounds = mesh.nodes[0].bounds;
    // Ignore two-sided sheets. A ray from free space reaches the front of the
    // first boundary. Use different directions to avoid shared polygon edges.
    float range = bounds.high.dist(bounds.low) + 1;
    int open = 0;
    for(const vec &direction : {vec(.31f, .17f, -1), vec(-.23f, -.39f, -1), vec(.21f, -.43f, 1)})
    {
        HaloCollisionHit hit;
        vec end = vec(point).add(vec(direction).mul(range));
        if(trace(point, end, hit, twoSided, true) && direction.dot(hit.normal) < 0 && ++open == 2) return true;
    }
    return false;
}

bool halocollisionsweep(const vec &feet, const vec &delta, float radius, float height, HaloCollisionHit &hit)
{
    hit = {};
    if(!halocollisionactive() || !finite(feet) || !finite(delta) || delta.iszero() ||
       !std::isfinite(radius) || !std::isfinite(height) || radius <= 0 || height <= 0) return false;
    radius = min(radius, height/2);
    const Path path = {feet, delta, radius, height};
    const vec origin = vec(feet).add(vec(0, 0, radius)), axis(0, 0, height - 2*radius);
    visit(0, path, hit, [&](const Surface &surface)
    {
        // The closest-points normal defines a separating plane. Advancing to
        // that plane cannot cross the polygon, including at grazing angles.
        float fraction = 0;
        for(int step = 0; step < 24 && fraction <= hit.fraction; ++step)
        {
            vec position = vec(origin).add(vec(delta).mul(fraction));
            vec gap = capsulegap(surface, position, axis);
            float distance = gap.magnitude();
            vec normal = contactnormal(surface, position, axis, gap, distance);
            float closing = -normal.dot(delta);
            if(closing <= small) return;
            float clearance = distance - radius;
            if(clearance <= contactSlop)
            {
                if(frontcontact(surface, normal)) recordhit(hit, surface, fraction, normal);
                return;
            }
            fraction += clearance/closing;
        }
    });
    return hit.surface >= 0;
}

bool halocollisionoverlap(const vec &feet, float radius, float height, HaloCollisionHit &hit)
{
    hit = {};
    if(!halocollisionactive() || !finite(feet) || !std::isfinite(radius) || !std::isfinite(height) || radius <= 0 || height <= 0) return false;
    radius = min(radius, height/2);
    const Path path = {feet, vec(), radius, height};
    const vec origin = vec(feet).add(vec(0, 0, radius)), axis(0, 0, height - 2*radius);
    visit(0, path, hit, [&](const Surface &surface)
    {
        vec gap = capsulegap(surface, origin, axis);
        float distance = gap.magnitude(), depth = radius - distance;
        if(depth <= max(contactSlop, hit.penetration)) return;
        vec normal = contactnormal(surface, origin, axis, gap, distance);
        if(frontcontact(surface, normal)) recordhit(hit, surface, 0, normal, depth);
    });
    return hit.surface >= 0;
}
