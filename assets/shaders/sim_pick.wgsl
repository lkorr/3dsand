// sim_pick.wgsl — single-thread DDA down the camera center ray; writes the
// first non-air, non-gas cell hit (and the last empty cell before it) for the
// CPU brush. Read back with the tick's staging copy — one tick latent, which
// is fine for a paint cursor.
//
// pick[0]=hitFlag pick[1]=hitMat pick[2..4]=hitCell pick[5..7]=prevCell

@group(0) @binding(0)  var<storage, read_write> voxels    : array<u32>;
@group(0) @binding(3)  var<storage, read>       materials : array<Material>;
@group(0) @binding(9)  var<storage, read_write> pick      : array<u32>;
@group(0) @binding(10) var<uniform> R : RenderParams;

@compute @workgroup_size(1)
fn main() {
  let ro = R.camPos;
  let rd = normalize(R.camFwd);

  pick[0] = 0u;

  var p = ro;
  var cell = vec3<i32>(floor(p));
  let stepv = vec3<i32>(sign(rd));
  let inv = 1.0 / max(abs(rd), vec3f(1e-6));
  var tMax : vec3f;
  var tDelta = inv;
  for (var a = 0; a < 3; a++) {
    let boundary = f32(cell[a]) + select(0.0, 1.0, rd[a] > 0.0);
    tMax[a] = abs(boundary - ro[a]) * inv[a];
  }

  var prev = cell;
  for (var i = 0; i < 640; i++) {
    if (inBounds(cell)) {
      let w = voxels[cellIndex(vec3<u32>(cell))];
      let mat = voxMat(w);
      if (mat != MAT_AIR && materials[mat].klass != CLASS_GAS) {
        pick[0] = 1u;
        pick[1] = mat;
        pick[2] = u32(cell.x); pick[3] = u32(cell.y); pick[4] = u32(cell.z);
        pick[5] = u32(prev.x); pick[6] = u32(prev.y); pick[7] = u32(prev.z);
        return;
      }
      prev = cell;
    }
    // step
    if (tMax.x < tMax.y && tMax.x < tMax.z) {
      cell.x += stepv.x; tMax.x += tDelta.x;
    } else if (tMax.y < tMax.z) {
      cell.y += stepv.y; tMax.y += tDelta.y;
    } else {
      cell.z += stepv.z; tMax.z += tDelta.z;
    }
    let n = i32(WORLD_N);
    if ((cell.x < 0 && rd.x <= 0.0) || (cell.x >= n && rd.x >= 0.0) ||
        (cell.y < 0 && rd.y <= 0.0) || (cell.y >= n && rd.y >= 0.0) ||
        (cell.z < 0 && rd.z <= 0.0) || (cell.z >= n && rd.z >= 0.0)) {
      return;   // left the world for good
    }
  }
}
