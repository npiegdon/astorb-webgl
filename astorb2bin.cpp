// astorb2bin.cpp
//
// Convert Lowell astorb.dat into a compact, GPU-friendly binary:
//
//   [64-byte header][16-byte record * N]
//
// Each record is directly uploadable as an interleaved WebGL2 vertex buffer:
//
//   0  float32  a           semi-major axis, AU
//   4  uint16   e           eccentricity, 0..65535 -> 0..1
//   6  uint16   inc         inclination, 0..65535 -> 0..2pi
//   8  uint16   node        longitude of ascending node, 0..2pi
//  10  uint16   peri        argument of perihelion, 0..2pi
//  12  uint16   M0          mean anomaly at epoch, 0..2pi
//  14  uint16   packed      bits 0..13 = (H+2)*100, bits 14..15 = colour class
//
// Colour classes: 0 = gray (other), 1 = green (main belt),
//                 2 = yellow (Mars-crossing), 3 = red (Earth-crossing)
//
// Build:  g++ -O2 -std=c++17 -Wall -Wextra astorb2bin.cpp -o astorb2bin
// Usage:  ./astorb2bin astorb.dat astorb.bin

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

namespace {

constexpr double PI = 3.14159265358979323846;
constexpr double TAU = 2.0 * PI;
constexpr double DEG2RAD = PI / 180.0;
constexpr double GAUSS_K = 0.01720209895;  // rad / day, heliocentric 2-body

#pragma pack(push, 1)
struct Header {
  char magic[8];
  uint32_t version;
  uint32_t count;
  uint32_t stride;
  uint32_t flags;
  double epochJD;
  uint32_t reserved[8];
};
#pragma pack(pop)

#pragma pack(push, 1)
struct Record {
  float a;
  uint16_t e;
  uint16_t inc;
  uint16_t node;
  uint16_t peri;
  uint16_t M0;
  uint16_t packed;
};
#pragma pack(pop)

static_assert(sizeof(Header) == 64, "header must be 64 bytes");
static_assert(sizeof(Record) == 16, "record must be 16 bytes");

std::string field(const std::string& line, size_t pos, size_t len) {
  if (pos >= line.size()) return {};
  return line.substr(pos, std::min(len, line.size() - pos));
}

bool parseDouble(const std::string& s, double& out) {
  const char* begin = s.c_str();
  char* end = nullptr;
  out = std::strtod(begin, &end);
  if (end == begin) return false;
  while (*end == ' ' || *end == '\t' || *end == '\r') ++end;
  return *end == '\0' && std::isfinite(out);
}

bool parseEpochJD(const std::string& s, double& jd) {
  if (s.size() != 8) return false;
  int y = 0, m = 0, d = 0;
  if (std::sscanf(s.c_str(), "%4d%2d%2d", &y, &m, &d) != 3) return false;
  if (m < 1 || m > 12 || d < 1 || d > 31) return false;

  // Gregorian calendar date at 0h -> Julian Date. (x.5)
  const long a = (14 - m) / 12;
  const long yy = y + 4800 - a;
  const long mm = m + 12 * a - 3;
  const long jdn =
      d + (153 * mm + 2) / 5 + 365 * yy + yy / 4 - yy / 100 + yy / 400 - 32045;
  jd = static_cast<double>(jdn) - 0.5;
  return true;
}

double normalizeRadians(double rad) {
  double r = std::fmod(rad, TAU);
  if (r < 0.0) r += TAU;
  return r;
}

uint16_t quantizeAngle(double rad) {
  const double r = normalizeRadians(rad);
  long q = std::lround(r / TAU * 65535.0);
  if (q < 0) q = 0;
  if (q > 65535) q = 65535;
  return static_cast<uint16_t>(q);
}

uint16_t quantizeUnit(double x) {
  long q = std::lround(x * 65535.0);
  if (q < 0) q = 0;
  if (q > 65535) q = 65535;
  return static_cast<uint16_t>(q);
}

// Scott Manley-style taxonomy, derived geometrically from q and Q.
//   3 = Earth-crossing (red)
//   2 = Mars-crossing  (yellow)
//   1 = main belt      (green)
//   0 = everything else (gray)
int classify(double a, double e) {
  const double q = a * (1.0 - e);
  const double Q = a * (1.0 + e);

  if (q < 1.0167 && Q > 0.9833) return 3;  // overlaps Earth's orbit
  if (q < 1.6660 && Q > 1.3810) return 2;  // overlaps Mars's orbit
  if (q > 1.6660 && Q < 4.95) return 1;    // interior to Jupiter: main belt
  return 0;
}

std::string formatCount(long long n) {
  std::string s = std::to_string(n);
  for (int i = static_cast<int>(s.size()) - 3; i > 0; i -= 3) {
    s.insert(static_cast<size_t>(i), ",");
  }
  return s;
}

}  // namespace

int main(int argc, char** argv) {
  const std::string inPath = argc > 1 ? argv[1] : "astorb.dat";
  const std::string outPath = argc > 2 ? argv[2] : "astorb.bin";

  std::ifstream in(inPath);
  if (!in) {
    std::cerr << "error: cannot open " << inPath << "\n";
    return 1;
  }

  std::vector<Record> records;
  records.reserve(1600000);

  long long total = 0;
  long long malformed = 0;
  long long droppedPoor = 0;
  long long droppedBig = 0;
  long long droppedInvalid = 0;
  long long classCounts[4] = {0, 0, 0, 0};

  bool haveTargetEpoch = false;
  double targetJD = 0.0;

  std::string line;
  while (std::getline(in, line)) {
    ++total;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.size() < 181) {
      ++malformed;
      continue;
    }

    // Epoch of osculation, yyyymmdd.
    double epochJD = 0.0;
    if (!parseEpochJD(field(line, 106, 8), epochJD)) {
      ++malformed;
      continue;
    }
    if (!haveTargetEpoch) {
      targetJD = epochJD;
      haveTargetEpoch = true;
    }

    // Orbit computation code 2. Bits 2 and 4 mean eccentricity (and perhaps
    // semimajor axis) were assumed -- exactly the poor orbits we drop.
    double code2d = 0.0;
    if (!parseDouble(field(line, 74, 4), code2d)) {
      ++malformed;
      continue;
    }
    const int code2 = static_cast<int>(code2d);
    if ((code2 & 6) != 0) {
      ++droppedPoor;
      continue;
    }

    double H = 20.0, a = 0.0, e = 0.0;
    double incDeg = 0.0, nodeDeg = 0.0, periDeg = 0.0, MDeg = 0.0;

    // H is optional in principle (blank -> fallback). Keep the parse failure
    // non-fatal: it only affects display size.
    if (!parseDouble(field(line, 42, 5), H)) H = 20.0;

    if (!parseDouble(field(line, 168, 13), a) ||
        !parseDouble(field(line, 158, 10), e) ||
        !parseDouble(field(line, 147, 10), incDeg) ||
        !parseDouble(field(line, 137, 10), nodeDeg) ||
        !parseDouble(field(line, 126, 10), periDeg) ||
        !parseDouble(field(line, 115, 10), MDeg)) {
      ++droppedInvalid;
      continue;
    }

    if (!(a > 0.0) || !(e >= 0.0 && e < 1.0) || !std::isfinite(a) ||
        !std::isfinite(e)) {
      ++droppedInvalid;
      continue;
    }

    if (a >= 1000.0) {
      ++droppedBig;
      continue;
    }

    // H = 99.99 is the astorb "unknown" sentinel in this file. Clamp the
    // extreme tails so the point-size mapping stays sane.
    if (H > 30.0 || H < -5.0 || !std::isfinite(H)) H = 20.0;
    H = std::max(-2.0, std::min(30.0, H));

    // All records in this file share one epoch, but handle mixed epochs
    // robustly by advancing the mean anomaly to the common target epoch.
    double MRad = MDeg * DEG2RAD;
    if (epochJD != targetJD) {
      const double n = GAUSS_K / std::pow(a, 1.5);
      MRad += n * (targetJD - epochJD);
    }

    const double incRad = incDeg * DEG2RAD;
    const double nodeRad = nodeDeg * DEG2RAD;
    const double periRad = periDeg * DEG2RAD;

    const int cls = classify(a, e);
    ++classCounts[cls];

    int hcode = static_cast<int>(std::lround((H + 2.0) * 100.0));
    if (hcode < 0) hcode = 0;
    if (hcode > 16383) hcode = 16383;

    Record rec{};
    rec.a = static_cast<float>(a);
    rec.e = quantizeUnit(e);
    rec.inc = quantizeAngle(incRad);
    rec.node = quantizeAngle(nodeRad);
    rec.peri = quantizeAngle(periRad);
    rec.M0 = quantizeAngle(MRad);
    rec.packed = static_cast<uint16_t>((cls << 14) | hcode);
    records.push_back(rec);
  }

  if (!haveTargetEpoch) {
    std::cerr << "error: no valid records found\n";
    return 1;
  }

  Header header{};
  std::memcpy(header.magic, "ASTORB2", 8);  // includes trailing NUL
  header.version = 1;
  header.count = static_cast<uint32_t>(records.size());
  header.stride = sizeof(Record);
  header.flags = 1;  // H/class packed into the final uint16
  header.epochJD = targetJD;
  std::memset(header.reserved, 0, sizeof(header.reserved));

  std::ofstream out(outPath, std::ios::binary);
  if (!out) {
    std::cerr << "error: cannot open " << outPath << " for writing\n";
    return 1;
  }
  out.write(reinterpret_cast<const char*>(&header), sizeof(header));
  if (!records.empty()) {
    out.write(reinterpret_cast<const char*>(records.data()),
              static_cast<std::streamsize>(records.size() * sizeof(Record)));
  }
  out.close();
  if (!out) {
    std::cerr << "error: failed while writing " << outPath << "\n";
    return 1;
  }

  const long long rawBytes =
      static_cast<long long>(sizeof(Header) + records.size() * sizeof(Record));

  std::cout << "input records:   " << formatCount(total) << "\n";
  std::cout << "malformed:       " << formatCount(malformed) << "\n";
  std::cout << "dropped poor:    " << formatCount(droppedPoor)
            << "  (assumed eccentricity/semimajor axis)\n";
  std::cout << "dropped a>=1000: " << formatCount(droppedBig) << "\n";
  std::cout << "dropped invalid: " << formatCount(droppedInvalid) << "\n";
  std::cout << "written:         " << formatCount(static_cast<long long>(records.size()))
            << "\n";
  std::cout << "  green (main belt):     " << formatCount(classCounts[1]) << "\n";
  std::cout << "  yellow (Mars-crossing):" << formatCount(classCounts[2]) << "\n";
  std::cout << "  red (Earth-crossing):  " << formatCount(classCounts[3]) << "\n";
  std::cout << "  gray (other):          " << formatCount(classCounts[0]) << "\n";
  std::cout << "epoch JD:        " << std::fixed << targetJD << "\n";
  std::cout << "output:          " << outPath << "  (" << rawBytes
            << " bytes, " << static_cast<double>(rawBytes) / (1024.0 * 1024.0)
            << " MiB)\n";
  return 0;
}
