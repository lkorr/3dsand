#pragma once
#include <cmath>
#include <cstdint>

struct Vec3 {
  float x = 0, y = 0, z = 0;
  Vec3() = default;
  Vec3(float x_, float y_, float z_) : x(x_), y(y_), z(z_) {}
  Vec3 operator+(const Vec3& o) const { return {x + o.x, y + o.y, z + o.z}; }
  Vec3 operator-(const Vec3& o) const { return {x - o.x, y - o.y, z - o.z}; }
  Vec3 operator*(float s) const { return {x * s, y * s, z * s}; }
  Vec3& operator+=(const Vec3& o) { x += o.x; y += o.y; z += o.z; return *this; }
  float dot(const Vec3& o) const { return x * o.x + y * o.y + z * o.z; }
  float len() const { return std::sqrt(dot(*this)); }
  Vec3 normalized() const {
    float l = len();
    return l > 1e-8f ? Vec3{x / l, y / l, z / l} : Vec3{0, 0, 0};
  }
  Vec3 cross(const Vec3& o) const {
    return {y * o.z - z * o.y, z * o.x - x * o.z, x * o.y - y * o.x};
  }
};

struct IVec3 {
  int x = 0, y = 0, z = 0;
};

inline int ifloor(float v) { return (int)std::floor(v); }
