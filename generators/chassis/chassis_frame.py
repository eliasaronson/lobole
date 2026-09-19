"""The frame: members along paths, coped where they meet.

A member is a bent path and a pipe swept along it, which is what a bender and
a tube laser are each given. Nothing here unions anything: every member stays
its own body so it can be cut, labelled and sent out on its own, and the
joints are COPES, which cut the member and leave its neighbour standing.

Which member copes to which is a RANK: a hoop is primary and is coped to
nothing, a rail copes to the hoops, a brace copes to both. That is the order
a frame is actually tacked up in, and it is what stops two members eating
each other.
"""
import math
import chassis_spec as S


class Frame:
    """Collects members, works out which meet which, and builds them."""

    def __init__(self, d):
        self.d = d
        self.members = []  # dicts: name, points, size, rank, ids

    # -- geometry helpers --------------------------------------------------
    def add(self, name, points, size, rank, former=None):
        """One member. `points` are the CORNERS of its centreline."""
        pts = [tuple(float(c) for c in p) for p in points]
        if len(pts) < 2:
            raise ValueError(f"{name}: a member needs two ends")
        self.members.append(dict(name=name, points=pts, size=size, rank=rank,
                                 former=former or S.BRACE_FORMER))
        return self.members[-1]

    def pair(self, name, points, size, rank, former=None):
        """The member and its mirror image, one per side.

        Built as two members rather than mirrored, because each side is its
        own length of tube with its own label in the shop, and because a
        cut list that says 'two of these' has to have two of them.
        """
        out = []
        for side, tag in ((1.0, "L"), (-1.0, "R")):
            mirrored = [(x, side * y, z) for (x, y, z) in points]
            out.append(self.add(f"{name} {tag}", mirrored, size, rank,
                                former))
        return out

    # -- coping ------------------------------------------------------------
    @staticmethod
    def _arc_polyline(points, radius, per_corner=6):
        """The centreline as the METAL follows it, not as it was typed.

        A corner is where two straights would meet; the tube turns short of
        it on a former, and with a 152 mm former on a hoop the metal is 63 mm
        from the corner. Testing joints against the corners says two members
        meet where there is nothing but air — which is how a brace came to be
        attached to a point the hoop does not pass through.
        """
        if len(points) < 3 or radius <= 0.0:
            return list(points)

        def sub(a, b):
            return tuple(a[i] - b[i] for i in range(3))

        def add(a, b):
            return tuple(a[i] + b[i] for i in range(3))

        def scale(a, k):
            return tuple(a[i] * k for i in range(3))

        def unit(a):
            n = math.sqrt(sum(c * c for c in a))
            return scale(a, 1.0 / n) if n > 1e-12 else a

        out = [points[0]]
        for i in range(1, len(points) - 1):
            prev, corner, nxt = points[i - 1], points[i], points[i + 1]
            u, v = unit(sub(prev, corner)), unit(sub(nxt, corner))
            cosang = max(-1.0, min(1.0, sum(u[k] * v[k] for k in range(3))))
            half = math.acos(cosang) / 2.0
            if half <= 1e-9 or abs(half - math.pi / 2.0) < 1e-9:
                out.append(corner)
                continue
            tangent = radius / math.tan(half)   # back off along each straight
            a = add(corner, scale(u, tangent))
            b = add(corner, scale(v, tangent))
            out.append(a)
            # The arc between them, sampled: good enough to answer "does this
            # member pass through that end", which is all this is for.
            for k in range(1, per_corner):
                t = k / float(per_corner)
                # a quadratic Bezier through the corner is within a few
                # tenths of a true arc at these radii
                p0 = add(scale(a, (1 - t) ** 2), scale(corner, 2 * (1 - t) * t))
                out.append(add(p0, scale(b, t * t)))
            out.append(b)
        out.append(points[-1])
        return out

    def _metal(self, member):
        """The member's centreline where the metal actually runs."""
        if "metal" not in member:
            member["metal"] = self._arc_polyline(member["points"],
                                                 member["former"])
        return member["metal"]

    @staticmethod
    def _segments(member):
        pts = member["points"]
        return [(pts[i], pts[i + 1]) for i in range(len(pts) - 1)]

    @staticmethod
    def _segment_distance(a0, a1, b0, b1):
        """Shortest distance between two 3D segments."""
        def sub(p, q):
            return tuple(p[i] - q[i] for i in range(3))

        def dot(p, q):
            return sum(p[i] * q[i] for i in range(3))

        u, v, w = sub(a1, a0), sub(b1, b0), sub(a0, b0)
        a, b, c = dot(u, u), dot(u, v), dot(v, v)
        dd, e = dot(u, w), dot(v, w)
        den = a * c - b * b
        if den < 1e-12:
            sc, tc = 0.0, (e / c if c > 1e-12 else 0.0)
        else:
            sc = max(0.0, min(1.0, (b * e - c * dd) / den))
            tc = max(0.0, min(1.0, (a * e - b * dd) / den))
        # One refinement pass, since clamping one end moves the other.
        if c > 1e-12:
            tc = max(0.0, min(1.0, (b * sc + e) / c))
        if a > 1e-12:
            sc = max(0.0, min(1.0, (b * tc - dd) / a))
        p = tuple(a0[i] + sc * u[i] for i in range(3))
        q = tuple(b0[i] + tc * v[i] for i in range(3))
        return math.dist(p, q)

    @staticmethod
    def _closest_on(points, e):
        """Closest point on a polyline to `e`, as (distance, arc length along
        the polyline, total length)."""
        best, at, total = None, 0.0, 0.0
        run = 0.0
        for i in range(len(points) - 1):
            a, b = points[i], points[i + 1]
            ab = tuple(b[k] - a[k] for k in range(3))
            ll = sum(c * c for c in ab)
            t = 0.0 if ll < 1e-12 else max(0.0, min(1.0, sum(
                (e[k] - a[k]) * ab[k] for k in range(3)) / ll))
            q = tuple(a[k] + t * ab[k] for k in range(3))
            dist = math.dist(e, q)
            seg = math.sqrt(ll)
            if best is None or dist < best:
                best, at = dist, run + t * seg
            run += seg
        return best, at, run

    def fit(self, m, other):
        """How does `m` meet `other` — "against", "mitre", or None?

        A member whose end lands on a neighbour's SIDE wants a fishmouth; one
        that meets a neighbour END TO END wants a mitre, because there is no
        round side to cut into. Which it is is a question about the NODES,
        and it has to be asked here: the tubes are drawn past their nodes, so
        in the geometry a corner and a tee look alike.
        """
        rm = S.OD[m["size"]] / 2.0
        rn = S.OD[other["size"]] / 2.0
        theirs = self._metal(other)
        for e in (m["points"][0], m["points"][-1]):
            dist, at, total = self._closest_on(theirs, e)
            if dist is None or dist > rm + rn:
                continue
            # Clear of the neighbour's own ends means it runs PAST: a side to
            # land on. At one of its ends, the two stop together: a corner.
            return "against" if (rn < at < total - rn) else "mitre"
        return None

    def meets(self, m, other):
        return self.fit(m, other) is not None

    # -- building ----------------------------------------------------------
    def _mitred_ends(self, m):
        """Which of `m`'s ENDS are MITRED, as (first, last).

        Only a mitred end may be drawn long, and the reason is worth keeping:
        a mitre is a PLANE, and it takes everything past itself, overrun and
        all. A cope is a SADDLE cut from the node outward — on two tubes of
        one diameter its horns reach only to the node — so an overrun on a
        coped end is not removed by anything and stands proud of the frame.
        That is exactly what four hoop feet did.
        """
        rm = S.OD[m["size"]] / 2.0
        first = last = False
        for o in self.members:
            if o is m or o["rank"] >= m["rank"]:
                continue
            if self.fit(m, o) != "mitre":
                continue
            theirs = self._metal(o)
            rn = S.OD[o["size"]] / 2.0
            for idx, e in ((0, m["points"][0]), (-1, m["points"][-1])):
                dist, at, total = self._closest_on(theirs, e)
                if dist is not None and dist < rm + rn:
                    if idx == 0:
                        first = True
                    else:
                        last = True
        return first, last

    @staticmethod
    def _drawn_long(points, od, ends):
        """The path the tube is SWEPT along: a diameter past each END THAT
        WILL BE CUT.

        A mitre is a plane through the node, and a plane through the very end
        of a tube shaves a wedge off it instead of cutting it — so the metal
        has to reach past the node and be trimmed back, which is how a frame
        is built. The NODES stay as they are: they are the topology.
        """
        def step(a, b, by):
            import math
            d = [b[i] - a[i] for i in range(3)]
            n = math.sqrt(sum(c * c for c in d))
            if n < 1e-9:
                return list(a)
            return [a[i] - d[i] / n * by for i in range(3)]

        out = list(points)
        if ends[0]:
            out[0] = step(points[0], points[1], od)
        if ends[1]:
            out[-1] = step(points[-1], points[-2], od)
        return out

    def build(self, log=None):
        d = self.d
        for m in self.members:
            path = d.call("add_feature", feature={
                "type": "Curve3D", "name": f"{m['name']} CL",
                "kind": "bent_path", "radius": m["former"],
                "points": [list(p) for p in
                           self._drawn_long(m["points"], S.OD[m["size"]],
                                            self._mitred_ends(m))]})
            self._check(path, m["name"] + " centreline")
            pipe = d.call("add_feature", feature={
                "type": "Pipe", "name": m["name"], "path_id": path["id"],
                "outer_diameter": S.OD[m["size"]],
                "wall_thickness": S.WALL[m["size"]]})
            self._check(pipe, m["name"])
            m["path_id"] = path["id"]
            m["pipe_id"] = pipe["id"]

        copes = 0
        for m in self.members:
            # A member copes to everything of LOWER rank it meets: the order
            # a frame is tacked up in, and what stops two members eating each
            # other.
            against, mitre = [], []
            for o in self.members:
                if o["rank"] >= m["rank"]:
                    continue
                kind = self.fit(m, o)
                if kind == "against":
                    against.append(o["pipe_id"])
                elif kind == "mitre":
                    mitre.append(o["pipe_id"])
            if not against and not mitre:
                continue
            r = d.call("add_feature", feature={
                "type": "Cope", "name": f"Fit {m['name']}",
                "member_id": m["pipe_id"], "against": against,
                "mitre": mitre, "gap": S.WELD_GAP})
            self._check(r, f"fit {m['name']}")
            copes += 1
            if log:
                log(f"    {m['name']}: {len(against)} coped, "
                    f"{len(mitre)} mitred")
        return copes

    @staticmethod
    def _check(result, what):
        failed = result.get("regen", {}).get("failed", [])
        if result.get("error") or failed:
            raise SystemExit(f"{what}: {failed or result}")
