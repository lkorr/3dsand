#!/usr/bin/env python3
"""Geometry helpers for sandvox agents.

Quaternions, rotations, voxel placement, coordinate conversions — everything
an agent gets wrong when reasoning about 3D in its head.  Run interactively
or import from another script.

Conventions (must match the engine):
  - Y-up right-handed coordinate system
  - Quat layout is (x, y, z, w), identity = (0, 0, 0, 1)
  - Heading 0 faces +Z, heading pi/2 faces +X
  - .vox files are Z-up; engine = (scene.x, scene.z, -scene.y)
  - Euler order is X-then-Y-then-Z (intrinsic)
  - Model +X is the character's LEFT (scene-to-engine flips handedness)

Usage examples:
  python scripts/geometry.py quat_from_axis_angle 0 1 0 90
  python scripts/geometry.py rotate_point  0 1 0 90  -- 1 0 0
  python scripts/geometry.py quat_mul  0 0.707 0 0.707  -- 0.707 0 0 0.707
  python scripts/geometry.py euler_to_quat  45 0 0
  python scripts/geometry.py quat_to_euler  0.383 0 0 0.924
  python scripts/geometry.py vox_to_engine  10 5 20
  python scripts/geometry.py describe_quat  0 0.707 0 0.707
  python scripts/geometry.py placement  --from 10 20 30  --facing 0 0 1  --offset 0 0 5
"""
import math
import sys

# ---------------------------------------------------------------------------
# Core quaternion math
# ---------------------------------------------------------------------------

def quat_identity():
    return (0.0, 0.0, 0.0, 1.0)

def quat_from_axis_angle(ax, ay, az, angle_deg):
    """Axis-angle to quaternion.  Axis need not be normalized."""
    length = math.sqrt(ax*ax + ay*ay + az*az)
    if length < 1e-12:
        return quat_identity()
    ax, ay, az = ax/length, ay/length, az/length
    half = math.radians(angle_deg) * 0.5
    s = math.sin(half)
    return (ax*s, ay*s, az*s, math.cos(half))

def qx(deg):
    """Rotation about the X axis (pitch)."""
    return quat_from_axis_angle(1, 0, 0, deg)

def qy(deg):
    """Rotation about the Y axis (yaw/heading)."""
    return quat_from_axis_angle(0, 1, 0, deg)

def qz(deg):
    """Rotation about the Z axis (roll)."""
    return quat_from_axis_angle(0, 0, 1, deg)

def quat_mul(a, b):
    """Hamilton product: apply rotation b first, then a."""
    ax, ay, az, aw = a
    bx, by, bz, bw = b
    return (
        aw*bx + ax*bw + ay*bz - az*by,
        aw*by - ax*bz + ay*bw + az*bx,
        aw*bz + ax*by - ay*bx + az*bw,
        aw*bw - ax*bx - ay*by - az*bz,
    )

def quat_conj(q):
    """Conjugate (inverse for unit quaternions)."""
    return (-q[0], -q[1], -q[2], q[3])

def quat_normalize(q):
    n = math.sqrt(sum(c*c for c in q))
    return tuple(c/n for c in q) if n > 1e-12 else quat_identity()

def quat_dot(a, b):
    return sum(x*y for x, y in zip(a, b))

def quat_nlerp(a, b, t):
    """Normalized linear interpolation (shortest arc)."""
    if quat_dot(a, b) < 0:
        b = tuple(-c for c in b)
    r = tuple(a[i] + t * (b[i] - a[i]) for i in range(4))
    return quat_normalize(r)

# ---------------------------------------------------------------------------
# Euler conversions (X-then-Y-then-Z intrinsic, degrees)
# ---------------------------------------------------------------------------

def euler_to_quat(rx, ry, rz):
    """Euler angles (degrees, X-then-Y-then-Z intrinsic) to quaternion."""
    return quat_mul(qz(rz), quat_mul(qy(ry), qx(rx)))

def quat_to_euler(x, y, z, w):
    """Quaternion to Euler angles (degrees, X-then-Y-then-Z intrinsic).
    At gimbal lock (Y near ±90°) the X/Z split is arbitrary."""
    sinp = 2 * (w*y - z*x)
    sinp = max(-1, min(1, sinp))
    ry = math.asin(sinp)
    if abs(sinp) > 0.9999:
        rx = 2 * math.atan2(x, w)
        rz = 0
    else:
        rx = math.atan2(2 * (w*x + y*z), 1 - 2 * (x*x + y*y))
        rz = math.atan2(2 * (w*z + x*y), 1 - 2 * (y*y + z*z))
    return (math.degrees(rx), math.degrees(ry), math.degrees(rz))

# ---------------------------------------------------------------------------
# Vector / rotation operations
# ---------------------------------------------------------------------------

def rotate_point(quat, point):
    """Rotate a 3D point by a quaternion."""
    qx, qy, qz, qw = quat
    px, py, pz = point
    # q * p * q^-1 via the efficient cross-product form
    tx = 2 * (qy*pz - qz*py)
    ty = 2 * (qz*px - qx*pz)
    tz = 2 * (qx*py - qy*px)
    return (
        px + qw*tx + qy*tz - qz*ty,
        py + qw*ty + qz*tx - qx*tz,
        pz + qw*tz + qx*ty - qy*tx,
    )

def look_rotation(forward, up=(0, 1, 0)):
    """Quaternion that rotates +Z to face the given forward direction."""
    fx, fy, fz = forward
    n = math.sqrt(fx*fx + fy*fy + fz*fz)
    if n < 1e-12:
        return quat_identity()
    fx, fy, fz = fx/n, fy/n, fz/n
    dot = fz  # dot(+Z, forward)
    if dot > 0.9999:
        return quat_identity()
    if dot < -0.9999:
        return (0, 1, 0, 0)  # 180 about Y
    # cross(+Z, forward)
    cx, cy, cz = -fy, fx, 0  # simplified: (0,0,1) x (fx,fy,fz)
    # Actually: cross = (0*fz - 1*fy, 1*fx - 0*fz, 0*fy - 0*fx) = (-fy, fx, 0)
    w = 1 + dot
    return quat_normalize((cx, cy, cz, w))

def quat_from_to(a, b):
    """Minimal rotation quaternion from direction a to direction b."""
    ax, ay, az = a
    bx, by, bz = b
    na = math.sqrt(ax*ax + ay*ay + az*az)
    nb = math.sqrt(bx*bx + by*by + bz*bz)
    if na < 1e-12 or nb < 1e-12:
        return quat_identity()
    ax, ay, az = ax/na, ay/na, az/na
    bx, by, bz = bx/nb, by/nb, bz/nb
    dot = ax*bx + ay*by + az*bz
    if dot > 0.9999:
        return quat_identity()
    if dot < -0.9999:
        # pick a perpendicular axis
        if abs(ax) < 0.9:
            px, py, pz = 0, -az, ay
        else:
            px, py, pz = az, 0, -ax
        n = math.sqrt(px*px + py*py + pz*pz)
        return (px/n, py/n, pz/n, 0.0)
    # cross product
    cx = ay*bz - az*by
    cy = az*bx - ax*bz
    cz = ax*by - ay*bx
    w = 1 + dot
    return quat_normalize((cx, cy, cz, w))

# ---------------------------------------------------------------------------
# Coordinate conversions
# ---------------------------------------------------------------------------

def vox_to_engine(sx, sy, sz):
    """Convert MagicaVoxel scene coords (Z-up) to engine coords (Y-up)."""
    return (sx, sz, -sy)

def engine_to_vox(ex, ey, ez):
    """Convert engine coords (Y-up) to MagicaVoxel scene coords (Z-up)."""
    return (ex, -ez, ey)

def heading_to_forward(heading_deg):
    """Convert a heading angle to a forward direction vector.
    Heading 0 = +Z, 90 = +X."""
    r = math.radians(heading_deg)
    return (math.sin(r), 0, math.cos(r))

def forward_to_heading(fx, fy, fz):
    """Convert a forward direction to a heading angle in degrees."""
    return math.degrees(math.atan2(fx, fz))

# ---------------------------------------------------------------------------
# Placement helpers
# ---------------------------------------------------------------------------

def offset_position(pos, facing, right_offset=0, up_offset=0, forward_offset=0):
    """Compute a world position offset from pos along facing direction."""
    fx, fy, fz = facing
    n = math.sqrt(fx*fx + fy*fy + fz*fz)
    if n < 1e-12:
        return pos
    fx, fy, fz = fx/n, fy/n, fz/n
    # right = forward x up
    rx, ry, rz = fy*0 - fz*1, fz*0 - fx*0, fx*1 - fy*0
    # simplified: forward x (0,1,0) = (fz, 0, -fx) ... wait
    rx, ry, rz = fz, 0, -fx  # cross(forward, up) for Y-up
    return (
        pos[0] + fx*forward_offset + rx*right_offset,
        pos[1] + up_offset,
        pos[2] + fz*forward_offset + rz*right_offset,
    )

def grid_snap(x, y, z, grid=1):
    """Snap a position to the voxel grid."""
    return (round(x / grid) * grid, round(y / grid) * grid, round(z / grid) * grid)

# ---------------------------------------------------------------------------
# Description / debugging
# ---------------------------------------------------------------------------

def describe_quat(x, y, z, w):
    """Human-readable description of what a quaternion does."""
    q = quat_normalize((x, y, z, w))
    x, y, z, w = q
    angle = 2 * math.acos(max(-1, min(1, w)))
    angle_deg = math.degrees(angle)
    if angle < 0.001:
        return "Identity (no rotation)"
    s = math.sin(angle / 2)
    if abs(s) < 1e-12:
        return f"~{angle_deg:.1f} degrees about an undefined axis"
    ax, ay, az = x/s, y/s, z/s
    # Describe the axis
    axis_names = []
    if abs(ax) > 0.99: axis_names.append(f"{'+' if ax > 0 else '-'}X")
    if abs(ay) > 0.99: axis_names.append(f"{'+' if ay > 0 else '-'}Y")
    if abs(az) > 0.99: axis_names.append(f"{'+' if az > 0 else '-'}Z")
    axis_str = axis_names[0] if len(axis_names) == 1 else f"({ax:.3f}, {ay:.3f}, {az:.3f})"

    # Show what happens to key directions
    fwd = rotate_point(q, (0, 0, 1))
    up = rotate_point(q, (0, 1, 0))
    right = rotate_point(q, (1, 0, 0))
    lines = [
        f"{angle_deg:.1f} degrees about {axis_str}",
        f"  +Z (forward) -> ({fwd[0]:.3f}, {fwd[1]:.3f}, {fwd[2]:.3f})",
        f"  +Y (up)      -> ({up[0]:.3f}, {up[1]:.3f}, {up[2]:.3f})",
        f"  +X (right)   -> ({right[0]:.3f}, {right[1]:.3f}, {right[2]:.3f})",
    ]
    ex, ey, ez = quat_to_euler(x, y, z, w)
    lines.append(f"  euler (XYZ deg): ({ex:.1f}, {ey:.1f}, {ez:.1f})")
    return "\n".join(lines)

def describe_transform(pos, quat):
    """Describe a full transform (position + rotation)."""
    lines = [f"Position: ({pos[0]:.2f}, {pos[1]:.2f}, {pos[2]:.2f})"]
    lines.append(f"Rotation: {describe_quat(*quat)}")
    fwd = rotate_point(quat, (0, 0, 1))
    lines.append(f"Heading: {forward_to_heading(*fwd):.1f} degrees")
    return "\n".join(lines)

def fmt(v, dp=4):
    """Format a tuple for pasting into JSON/code."""
    return "[" + ", ".join(f"{c:.{dp}f}" for c in v) + "]"

# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def _parse_floats(args):
    """Parse floats from args, splitting on '--' for multi-argument commands."""
    groups = [[]]
    for a in args:
        if a == '--':
            groups.append([])
        else:
            groups[-1].append(float(a))
    return groups

def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return

    cmd = sys.argv[1]
    groups = _parse_floats(sys.argv[2:])
    a = groups[0]
    b = groups[1] if len(groups) > 1 else []

    if cmd == "quat_from_axis_angle":
        if len(a) != 4:
            print("usage: quat_from_axis_angle ax ay az angle_deg"); return
        q = quat_from_axis_angle(*a)
        print(f"quat: {fmt(q)}")
        print(describe_quat(*q))

    elif cmd == "qx":
        q = qx(a[0]); print(f"quat: {fmt(q)}"); print(describe_quat(*q))
    elif cmd == "qy":
        q = qy(a[0]); print(f"quat: {fmt(q)}"); print(describe_quat(*q))
    elif cmd == "qz":
        q = qz(a[0]); print(f"quat: {fmt(q)}"); print(describe_quat(*q))

    elif cmd == "euler_to_quat":
        if len(a) != 3:
            print("usage: euler_to_quat rx ry rz (degrees)"); return
        q = euler_to_quat(*a)
        print(f"quat: {fmt(q)}")
        print(describe_quat(*q))

    elif cmd == "quat_to_euler":
        if len(a) != 4:
            print("usage: quat_to_euler x y z w"); return
        e = quat_to_euler(*a)
        print(f"euler (deg): {fmt(e, 2)}")

    elif cmd == "quat_mul":
        if len(a) != 4 or len(b) != 4:
            print("usage: quat_mul x y z w -- x y z w"); return
        q = quat_mul(tuple(a), tuple(b))
        print(f"quat: {fmt(q)}")
        print(describe_quat(*q))

    elif cmd == "rotate_point":
        if len(a) != 4 or len(b) != 3:
            print("usage: rotate_point ax ay az angle_deg -- px py pz"); return
        q = quat_from_axis_angle(*a)
        p = rotate_point(q, tuple(b))
        print(f"rotated: {fmt(p)}")

    elif cmd == "describe_quat":
        if len(a) != 4:
            print("usage: describe_quat x y z w"); return
        print(describe_quat(*a))

    elif cmd == "vox_to_engine":
        if len(a) != 3:
            print("usage: vox_to_engine sx sy sz"); return
        e = vox_to_engine(*a)
        print(f"engine: {fmt(e, 1)}")

    elif cmd == "engine_to_vox":
        if len(a) != 3:
            print("usage: engine_to_vox ex ey ez"); return
        v = engine_to_vox(*a)
        print(f"vox: {fmt(v, 1)}")

    elif cmd == "heading":
        if len(a) == 1:
            f = heading_to_forward(a[0])
            print(f"forward: {fmt(f)}")
        elif len(a) == 3:
            h = forward_to_heading(*a)
            print(f"heading: {h:.1f} degrees")
        else:
            print("usage: heading <deg> OR heading fx fy fz")

    elif cmd == "placement":
        # Parse --from, --facing, --offset from argv
        args = sys.argv[2:]
        pos = facing = offset = None
        i = 0
        while i < len(args):
            if args[i] == "--from" and i + 3 < len(args):
                pos = tuple(float(args[i+j]) for j in range(1, 4)); i += 4
            elif args[i] == "--facing" and i + 3 < len(args):
                facing = tuple(float(args[i+j]) for j in range(1, 4)); i += 4
            elif args[i] == "--offset" and i + 3 < len(args):
                offset = tuple(float(args[i+j]) for j in range(1, 4)); i += 4
            else:
                i += 1
        if not pos or not facing:
            print("usage: placement --from x y z --facing fx fy fz --offset right up fwd"); return
        off = offset or (0, 0, 0)
        result = offset_position(pos, facing, off[0], off[1], off[2])
        print(f"result: {fmt(result, 2)}")

    elif cmd == "look_at":
        if len(a) != 3 or len(b) != 3:
            print("usage: look_at from_x from_y from_z -- target_x target_y target_z"); return
        d = (b[0]-a[0], b[1]-a[1], b[2]-a[2])
        q = look_rotation(d)
        print(f"quat: {fmt(q)}")
        print(describe_quat(*q))

    elif cmd == "quat_from_to":
        if len(a) != 3 or len(b) != 3:
            print("usage: quat_from_to ax ay az -- bx by bz"); return
        q = quat_from_to(tuple(a), tuple(b))
        print(f"quat: {fmt(q)}")
        print(describe_quat(*q))

    elif cmd == "snap":
        grid = 1
        if len(a) == 4:
            grid = a[3]; a = a[:3]
        if len(a) != 3:
            print("usage: snap x y z [grid_size]"); return
        s = grid_snap(*a, grid)
        print(f"snapped: {fmt(s, 1)}")

    else:
        print(f"Unknown command: {cmd}")
        print("Commands: quat_from_axis_angle qx qy qz euler_to_quat quat_to_euler")
        print("          quat_mul rotate_point describe_quat look_at quat_from_to")
        print("          vox_to_engine engine_to_vox heading placement snap")

if __name__ == "__main__":
    main()
