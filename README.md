# The astorb database in a vertex shader

Real-time visualization of the Lowell `astorb.dat` asteroid catalog, inspired by Scott Manley's [Asteroid Discovery From 1980 - 2010](https://www.youtube.com/watch?v=S_d-gs0WoUw) video.
```
astorb.dat  --(astorb2bin)-->  astorb.bin  --(index.html / WebGL2)-->  points
```

The browser does no per-frame CPU work. Each asteroid is one 16-byte vertex; the vertex shader advances its mean anomaly, solves Kepler's equation, and produces its heliocentric position.

## Build the data file

Download the astorb database from [Lowell Minor Planet Service](https://asteroid.lowell.edu/astorb/) and extract into this folder, then:
```sh
make
./astorb2bin astorb.dat astorb.bin
```

The preprocessor drops:
- orbits with assumed eccentricity / semimajor axis (~27k objects)
- objects with `a >= 1000 AU` (~8 objects)
- any malformed records.

It keeps the >1.5M objects and classifies them geometrically:

| class | colour | rule |
|---|---|---|
| red | Earth-crossing | `q < 1.0167 && Q > 0.9833` |
| yellow | Mars-crossing | `q < 1.6660 && Q > 1.3810` |
| green | main belt | `q > 1.6660 && Q < 4.95` |
| gray | other | everything else |

## Run the viewer

Browsers block `fetch()` on `file://`, so serve the folder:
```sh
npx serve -l 8000
# or
python3 -m http.server 8000
# open http://localhost:8000/
```

Controls:
- left-drag to rotate
- right-drag to pan in the ecliptic plane
- wheel to zoom
- double-click to reset the camera and re-center
- space toggles playback

Asteroids are rendered as exact-center Gaussian splats. The Gaussian center is computed in the vertex shader in device pixels and evaluated against `gl_FragCoord` in the fragment shader, so subpixel motion stays smooth. Brightness is normalised by the spot area.

## Binary format

64-byte little-endian header:

| offset | type | field |
|---:|---|---|
| 0 | `char[8]` | `ASTORB2\0` |
| 8 | `uint32` | version (1) |
| 12 | `uint32` | object count |
| 16 | `uint32` | record stride (16) |
| 20 | `uint32` | flags |
| 24 | `float64` | epoch Julian date |
| 32 | `uint32[8]` | reserved |

Then `count` 16-byte records:

| offset | type | field |
|---:|---|---|
| 0 | `float32` | semimajor axis `a` (AU) |
| 4 | `uint16` | eccentricity / 65535 |
| 6 | `uint16` | inclination / 65535 × 2π |
| 8 | `uint16` | ascending node / 65535 × 2π |
| 10 | `uint16` | argument of perihelion / 65535 × 2π |
| 12 | `uint16` | mean anomaly at epoch / 65535 × 2π |
| 14 | `uint16` | bits 0–13: `(H + 2) × 100`; bits 14–15: colour class |

The 16-byte layout is directly uploadable as an interleaved WebGL2 vertex buffer: one `FLOAT`, five normalized `UNSIGNED_SHORT` attributes, and one integer `UNSIGNED_SHORT` attribute.

## Planets

`astorb.dat` contains no planets. The eight planets are hard-coded using the JPL/Standish approximate Keplerian elements and rates (valid 1800–2050).

## Accuracy

Asteroid positions use osculating elements with two-body Kepler propagation. Planetary perturbations are not modeled, so positions drift away from the epoch, especially for near-Earth objects. This is for illustrative visualization and is not an ephemeris-grade tool.
