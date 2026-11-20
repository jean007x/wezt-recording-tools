// GLSL for IntroRenderer. Each raw string stays under 16 KB (MSVC's limit for one string literal) and the pieces are joined at run time.

// ---- shared by every pass: the full-screen triangle ----
static const char* kVertex = R"GLSL(
out vec2 vUv;
void main() {
  vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
  vUv = p;
  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)GLSL";

// ---- the scene: the logo as a solid, the floor, the light ----
static const char* kSceneA = R"GLSL(
in vec2 vUv;
out vec4 oCol;
uniform sampler2D uSdf;
uniform vec2 uRes;
uniform vec2 uHalf;
uniform float uLogoHalfY;
uniform float uTime;
uniform float uDist;
uniform mat3 uRotOW;     // object -> world
uniform mat3 uRotWO;     // world -> object
uniform float uBlack;
uniform float uSweep;    // x of the travelling band of light across the logo (object units); < -50 = none
uniform float uImpact;   // 0..1 flash when the logo lands
uniform float uFade;
uniform float uBeam;
uniform vec3 uDrift;

const float PI = 3.14159265;
const float HD = 0.056;     // half the thickness of the slab
const float RB = 0.0105;    // radius of the rounded edge: the gold rim
const float FH = 0.44;      // how far the floor is below the logo's centre
const float FOCAL = 3.2;

float hash12(vec2 p) { vec3 p3 = fract(vec3(p.xyx) * 0.1031); p3 += dot(p3, p3.yzx + 33.33); return fract((p3.x + p3.y) * p3.z); }
float hash13(vec3 p3) { p3 = fract(p3 * 0.1031); p3 += dot(p3, p3.zyx + 31.32); return fract((p3.x + p3.y) * p3.z); }
float vnoise(vec3 p) {
  vec3 i = floor(p), f = fract(p); f = f * f * (3.0 - 2.0 * f);
  return mix(mix(mix(hash13(i), hash13(i + vec3(1, 0, 0)), f.x), mix(hash13(i + vec3(0, 1, 0)), hash13(i + vec3(1, 1, 0)), f.x), f.y),
             mix(mix(hash13(i + vec3(0, 0, 1)), hash13(i + vec3(1, 0, 1)), f.x), mix(hash13(i + vec3(0, 1, 1)), hash13(i + vec3(1, 1, 1)), f.x), f.y), f.z);
}

// ---- the photo studio: what the polished metal reflects (world space; the logo turns inside it) ----
float rectLight(vec3 r, vec3 c, vec2 hs, float soft) {
  float d = dot(r, c);
  if (d <= 0.0) return 0.0;
  vec3 up = abs(c.y) > 0.95 ? vec3(0.0, 0.0, 1.0) : vec3(0.0, 1.0, 0.0);
  vec3 u = normalize(cross(up, c)), v = cross(c, u);
  vec2 q = vec2(dot(r, u), dot(r, v)) / d;
  vec2 e = abs(q) - hs;
  return (1.0 - smoothstep(-soft, soft, e.x)) * (1.0 - smoothstep(-soft, soft, e.y));
}
vec3 env(vec3 r) {
  vec3 c = vec3(0.0016, 0.0018, 0.0026) + vec3(0.004, 0.0028, 0.0016) * max(0.0, -r.y);
  c += 10.0 * vec3(1.00, 0.96, 0.90) * rectLight(r, normalize(vec3(-0.50, 0.72, 0.60)), vec2(0.55, 0.34), 0.20);    // key soft box, top left
  c += 15.0 * vec3(0.80, 0.90, 1.00) * rectLight(r, normalize(vec3(-0.95, 0.12, 0.34)), vec2(0.09, 0.85), 0.10);    // tall strip, left (cool)
  c += 13.0 * vec3(1.00, 0.76, 0.46) * rectLight(r, normalize(vec3(0.95, 0.08, 0.30)), vec2(0.08, 0.80), 0.10);     // tall strip, right (warm)
  c += 7.0 * vec3(1.00, 0.95, 0.88) * rectLight(r, normalize(vec3(0.05, 1.00, 0.12)), vec2(0.95, 0.22), 0.28);      // long overhead panel
  c += 20.0 * vec3(1.00, 0.60, 0.26) * rectLight(r, normalize(vec3(0.28, 0.18, -1.0)), vec2(0.55, 0.30), 0.18);     // warm back light
  c += 5.0 * vec3(0.55, 0.65, 0.90) * rectLight(r, normalize(vec3(-0.40, -0.35, 0.80)), vec2(0.50, 0.12), 0.16);    // cool kicker from below
  c += 1.6 * vec3(1.00, 0.92, 0.80) * rectLight(r, normalize(vec3(0.10, 0.22, 1.00)), vec2(0.70, 0.22), 0.30);       // wide soft fill behind the camera
  c += 0.10 * vec3(0.9, 0.8, 0.7) * smoothstep(0.2, 1.0, r.z);
  return c;
}

// ---- the logo: its outline pushed out into a slab with rounded edges ----
float sdf2(vec2 q) {
  vec2 c = clamp(q, -uHalf * 0.985, uHalf * 0.985);
  vec2 uv = vec2(c.x / (2.0 * uHalf.x) + 0.5, 0.5 - c.y / (2.0 * uHalf.y));
  return texture(uSdf, uv).r + length(q - c);
}
float map(vec3 p) {
  float d2 = sdf2(p.xy);
  vec2 w = vec2(d2 + RB, abs(p.z) - HD + RB);
  return min(max(w.x, w.y), 0.0) + length(max(w, 0.0)) - RB;
}
vec3 calcNormal(vec3 p) {
  const vec2 k = vec2(1.0, -1.0);
  const float e = 0.0008;
  return normalize(k.xyy * map(p + k.xyy * e) + k.yyx * map(p + k.yyx * e) + k.yxy * map(p + k.yxy * e) + k.xxx * map(p + k.xxx * e));
}
float calcAO(vec3 p, vec3 n) {
  float occ = 0.0, sca = 1.0;
  for (int i = 1; i <= 5; ++i) {
    float h = 0.012 * float(i);
    occ += (h - map(p + n * h)) * sca;
    sca *= 0.8;
  }
  return clamp(1.0 - 9.0 * occ, 0.0, 1.0);
}
bool boxHit(vec3 ro, vec3 rd, vec3 b, out float t0, out float t1) {
  vec3 inv = 1.0 / rd;
  vec3 a = (-b - ro) * inv, c = (b - ro) * inv;
  vec3 mn = min(a, c), mx = max(a, c);
  t0 = max(max(mn.x, mn.y), mn.z);
  t1 = min(min(mx.x, mx.y), mx.z);
  return t1 > max(t0, 0.0);
}
// Ray-marches the object. Returns the distance of a hit, or -1. cover = how much of the pixel the object covers (for smooth silhouettes);
// tBest = where the ray came closest (used to shade a pixel that is only partly covered).
float march(vec3 roO, vec3 rdO, float pxA, float dist, out float cover, out float tBest) {
  cover = 0.0; tBest = -1.0;
  float margin = max(0.02, dist * pxA * 2.5);
  float t0, t1;
  if (!boxHit(roO, rdO, vec3(1.0 + margin, uLogoHalfY + margin, HD + margin), t0, t1)) return -1.0;
  float t = max(t0, 0.0), minR = 1e9;
  for (int i = 0; i < 120; ++i) {
    float d = map(roO + rdO * t);
    float eps = max(0.00012, t * pxA * 0.35);
    if (d < eps) { cover = 1.0; return t; }
    float ratio = d / (t * pxA);
    if (ratio < minR) { minR = ratio; tBest = t; }
    t += d * 0.9;
    if (t > t1) break;
  }
  cover = 1.0 - smoothstep(0.4, 1.6, minR);
  return -1.0;
}
)GLSL";

static const char* kSceneB = R"GLSL(
// The metal at a point on the logo. po = object-space position, rdW = the ray's direction in the world.
vec3 shadeObject(vec3 po, vec3 rdW) {
  vec3 no = calcNormal(po);
  float face = smoothstep(0.88, 0.99, abs(no.z));                       // 1 on the flat faces, 0 on the rounded edge and the side walls
  // a faint, large-scale curvature on the flat faces: the reflections then change gradually across them, like real polished metal
  vec3 nb = normalize(no + vec3(po.xy * vec2(0.62, 1.05), 0.0) * face);
  vec3 nw = uRotOW * nb;
  vec3 v = -rdW;
  float ndv = max(dot(nw, v), 0.0);
  vec3 r = reflect(rdW, nw);
  float ao = calcAO(po, no);
  vec3 e = env(r);
  vec3 F0g = vec3(1.00, 0.70, 0.27);
  vec3 gold = (F0g + (1.0 - F0g) * pow(1.0 - ndv, 5.0)) * e;
  vec3 lacquer = (vec3(0.030) + 0.945 * pow(1.0 - ndv, 5.0)) * e + vec3(0.0018, 0.0016, 0.0015);
  float isGold = uBlack > 0.5 ? 1.0 - face : 1.0;
  vec3 c = mix(lacquer, gold, isGold) * mix(0.45, 1.0, ao);
  // the band of light that travels across the logo
  float band = exp(-pow((po.x - uSweep) / 0.26, 2.0)) * (uSweep > -50.0 ? 1.0 : 0.0);
  c += band * mix(vec3(0.30, 0.27, 0.22), vec3(1.25, 0.92, 0.55), isGold) * (0.35 + 0.65 * pow(ndv, 1.5));
  c *= 1.0 + uImpact * 0.7;
  return c;
}

// What a ray from (ro, rd) sees of the logo alone (used for the floor's reflection). rgb = colour, a = coverage.
vec4 seeObject(vec3 ro, vec3 rd, float pxA) {
  vec3 roO = uRotWO * ro, rdO = uRotWO * rd;
  float cover, tB;
  float t = march(roO, rdO, pxA, length(ro), cover, tB);
  float use = t > 0.0 ? t : tB;
  if (cover <= 0.0 || use < 0.0) return vec4(0.0);
  return vec4(shadeObject(roO + rdO * use, rd), cover);
}

vec3 floorColor(vec3 ro, vec3 rd, float pxA) {
  float tf = (ro.y + FH) / -rd.y;
  vec3 pf = ro + rd * tf;
  float cosT = clamp(-rd.y, 0.0, 1.0);
  float F = 0.10 + 0.90 * pow(1.0 - cosT, 5.0);                        // polished black stone
  vec3 rr = vec3(rd.x, -rd.y, rd.z);
  vec3 refl = vec3(0.0);
  for (int k = 0; k < 4; ++k) {                                        // a few jittered reflection rays = a soft, slightly rough mirror
    vec3 j = vec3(hash12(gl_FragCoord.xy + float(k) * 17.3), hash12(gl_FragCoord.xy * 1.7 + float(k) * 5.1), hash12(gl_FragCoord.xy * 0.7 + float(k) * 9.3)) - 0.5;
    vec4 o = seeObject(pf, normalize(rr + j * vec3(0.012, 0.006, 0.012)), pxA * 4.0);
    refl += o.rgb * o.a;
  }
  refl /= 4.0;
  float r = length(pf.xz - vec2(0.0, 0.15));
  float pool = exp(-r * r * 0.30);
  vec3 base = vec3(0.0012, 0.0012, 0.0016) + vec3(1.0, 0.80, 0.52) * 0.020 * uBeam * pool;
  return base + F * refl * exp(-r * 0.55);
}

// Light beams through haze: two spotlights, sampled along the view ray, broken up by slowly drifting smoke.
vec3 beams(vec3 ro, vec3 rd, float tEnd) {
  float tc = dot(-ro, rd);
  float a = max(0.0, tc - 4.5), b = min(tEnd, tc + 4.5);
  if (b <= a) return vec3(0.0);
  const int N = 40;
  float dt = (b - a) / float(N);
  float j = hash12(gl_FragCoord.xy + uTime);
  vec3 acc = vec3(0.0);
  vec3 L1 = vec3(-1.7, 3.6, 2.4), D1 = normalize(vec3(0.0, -0.1, 0.0) - L1);
  vec3 L2 = vec3(2.8, 2.2, -2.8), D2 = normalize(vec3(0.0, -0.05, 0.0) - L2);
  for (int i = 0; i < N; ++i) {
    vec3 P = ro + rd * (a + (float(i) + j) * dt);
    vec3 l1 = L1 - P; float d1 = length(l1); l1 /= d1;
    vec3 l2 = L2 - P; float d2 = length(l2); l2 /= d2;
    float c1 = smoothstep(0.80, 0.985, dot(-l1, D1));
    float c2 = smoothstep(0.76, 0.980, dot(-l2, D2));
    float smoke = 0.35 + 0.9 * vnoise(P * 1.3 + vec3(0.0, -uTime * 0.07, uTime * 0.04)) * vnoise(P * 2.9 + vec3(uTime * 0.05, 0.0, 0.0));
    float ph1 = 0.5 + 0.5 * pow(max(dot(rd, -l1), 0.0), 3.0);
    float ph2 = 0.4 + 1.6 * pow(max(dot(rd, -l2), 0.0), 6.0);          // looking toward the back light: it flares
    acc += (vec3(1.0, 0.93, 0.82) * c1 * ph1 / (1.0 + 0.12 * d1 * d1) + vec3(1.0, 0.62, 0.30) * c2 * ph2 / (1.0 + 0.14 * d2 * d2)) * smoke;
  }
  return acc * dt * 0.030 * uBeam;
}

void main() {
  vec2 p = (vUv * 2.0 - 1.0) * vec2(1.0, uRes.y / uRes.x);
  float s = sin(uTime * 0.37), cdr = cos(uTime * 0.29);
  vec3 ro = vec3(uDrift.x + 0.05 * s, 0.30 + uDrift.y + 0.03 * cdr, uDist);
  vec3 ta = vec3(0.0, -0.04, 0.0);
  vec3 ww = normalize(ta - ro), uu = normalize(cross(ww, vec3(0.0, 1.0, 0.0))), vv = cross(uu, ww);
  vec3 rd = normalize(p.x * uu + p.y * vv + FOCAL * ww);
  float pxA = 2.0 / (uRes.x * FOCAL);

  vec3 roO = uRotWO * ro, rdO = uRotWO * rd;
  float cover, tB;
  float t = march(roO, rdO, pxA, uDist, cover, tB);
  float tEnd = 60.0 + uDist;
  vec3 col = vec3(0.0);
  if (cover < 1.0) {
    if (rd.y < -0.0015) { float tf = (ro.y + FH) / -rd.y; col = floorColor(ro, rd, pxA); tEnd = tf; }
    else col = vec3(0.0013, 0.0014, 0.0019);
  }
  float use = t > 0.0 ? t : tB;
  if (cover > 0.0 && use > 0.0) {
    col = mix(col, shadeObject(roO + rdO * use, rd), cover);
    if (cover >= 1.0) tEnd = use;
  }
  col += beams(ro, rd, tEnd);
  oCol = vec4(col * uFade, 1.0);
}
)GLSL";

// ---- bloom: a chain of blurred, shrinking copies of the bright parts ----
static const char* kDown = R"GLSL(
in vec2 vUv;
out vec4 oCol;
uniform sampler2D uSrc;
uniform vec2 uTexel;       // 1 / source size
uniform float uFirst;      // 1 on the first level: keep only what is brighter than the screen can show
vec3 T(vec2 o) { return texture(uSrc, vUv + o * uTexel).rgb; }
void main() {
  vec3 a = T(vec2(-2, 2)), b = T(vec2(0, 2)), c = T(vec2(2, 2)), d = T(vec2(-2, 0)), e = T(vec2(0, 0)), f = T(vec2(2, 0));
  vec3 g = T(vec2(-2, -2)), h = T(vec2(0, -2)), i = T(vec2(2, -2)), j = T(vec2(-1, 1)), k = T(vec2(1, 1)), l = T(vec2(-1, -1)), m = T(vec2(1, -1));
  vec3 r = e * 0.125 + (a + c + g + i) * 0.03125 + (b + d + f + h) * 0.0625 + (j + k + l + m) * 0.125;
  if (uFirst > 0.5) {
    float br = max(r.r, max(r.g, r.b));
    float soft = clamp(br - 2.0, 0.0, 0.6); soft = soft * soft / 2.4;
    r *= max(soft, br - 2.6) / max(br, 1e-4);
  }
  oCol = vec4(r, 1.0);
}
)GLSL";

static const char* kUp = R"GLSL(
in vec2 vUv;
out vec4 oCol;
uniform sampler2D uSrc;
uniform vec2 uTexel;
uniform float uGain;
void main() {
  vec3 s = texture(uSrc, vUv + uTexel * vec2(-1, 1)).rgb + texture(uSrc, vUv + uTexel * vec2(1, 1)).rgb
         + texture(uSrc, vUv + uTexel * vec2(-1, -1)).rgb + texture(uSrc, vUv + uTexel * vec2(1, -1)).rgb;
  s += 2.0 * (texture(uSrc, vUv + uTexel * vec2(0, 1)).rgb + texture(uSrc, vUv + uTexel * vec2(0, -1)).rgb
            + texture(uSrc, vUv + uTexel * vec2(-1, 0)).rgb + texture(uSrc, vUv + uTexel * vec2(1, 0)).rgb);
  s += 4.0 * texture(uSrc, vUv).rgb;
  oCol = vec4(s * (1.0 / 16.0) * uGain, 1.0);
}
)GLSL";

// A wide horizontal smear of the bright parts: the long streak a wide-screen lens leaves.
static const char* kStreak = R"GLSL(
in vec2 vUv;
out vec4 oCol;
uniform sampler2D uSrc;
uniform vec2 uTexel;
void main() {
  vec3 s = vec3(0.0); float ws = 0.0;
  for (int i = -40; i <= 40; ++i) {
    float w = exp(-float(i * i) / (2.0 * 15.0 * 15.0));
    s += texture(uSrc, vUv + vec2(float(i) * 2.0 * uTexel.x, 0.0)).rgb * w;
    ws += w;
  }
  oCol = vec4(s / ws, 1.0);
}
)GLSL";

// ---- the final picture: bloom, streaks, god rays, dust, lens, tone mapping, grain ----
static const char* kComposite = R"GLSL(
in vec2 vUv;
out vec4 oCol;
uniform sampler2D uScene;
uniform sampler2D uBloom;
uniform sampler2D uStreak;
uniform vec2 uOut;
uniform float uTime;
uniform float uImpact;
uniform float uMotes;
uniform float uFlare;
uniform float uExposure;

float hash12(vec2 p) { vec3 p3 = fract(vec3(p.xyx) * 0.1031); p3 += dot(p3, p3.yzx + 33.33); return fract((p3.x + p3.y) * p3.z); }
vec3 aces(vec3 x) { return clamp((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14), 0.0, 1.0); }

float motes(vec2 uv, float asp) {
  float acc = 0.0;
  for (int L = 0; L < 4; ++L) {
    float fl = float(L);
    float sc = 5.0 + fl * 6.5;
    vec2 q = uv * vec2(asp, 1.0) * sc + vec2(uTime * (0.035 + 0.02 * fl), -uTime * (0.022 + 0.012 * fl));
    vec2 id = floor(q), f = fract(q) - 0.5;
    float h = hash12(id + fl * 31.7);
    vec2 o = (vec2(hash12(id * 1.3 + 7.0), hash12(id * 2.1 + 3.0)) - 0.5) * 0.6;
    float r = (0.018 + 0.05 * h) * (L == 0 ? 2.2 : (L == 1 ? 1.4 : 1.0));
    float d = length(f - o);
    float tw = 0.5 + 0.5 * sin(uTime * (0.8 + h * 2.5) + h * 40.0);
    acc += smoothstep(r, r * 0.35, d) * step(0.62, h) * tw * (L == 0 ? 0.7 : 0.45);
  }
  return acc;
}

void main() {
  vec2 uv = vUv, cc = uv - 0.5;
  float asp = uOut.x / uOut.y;
  float ca = 0.0035 * dot(cc, cc);
  vec3 col = vec3(texture(uScene, uv + cc * ca).r, texture(uScene, uv).g, texture(uScene, uv - cc * ca).b);
  vec3 bl = texture(uBloom, uv).rgb;
  col += bl * 0.030;
  col += texture(uStreak, uv).rgb * vec3(1.00, 0.80, 0.52) * (0.04 + 0.22 * uImpact);
  // god rays: shafts of the bright parts, drawn toward the logo
  vec2 dir = (vec2(0.5, 0.52) - uv) / 28.0;
  vec2 sp = uv; float w = 1.0; vec3 gr = vec3(0.0);
  for (int i = 0; i < 28; ++i) { sp += dir; gr += texture(uBloom, sp).rgb * w; w *= 0.955; }
  col += gr * 0.0010 * vec3(1.0, 0.86, 0.66) * (0.7 + uImpact * 1.5);
  // dust floating in the light
  float pool = exp(-dot(cc, cc * vec2(1.0, 2.4)) * 5.5);
  col += motes(uv, asp) * vec3(1.0, 0.86, 0.62) * (0.30 + 0.55 * pool) * uMotes * (0.4 + 0.6 * pool);
  // the last moment: the logo has shrunk to a point of light that flares
  vec2 fc = cc * vec2(asp, 1.0);
  float rr = length(fc);
  col += uFlare * vec3(1.0, 0.88, 0.66) * (0.012 / (rr * rr + 0.0009) + 2.2 * exp(-abs(fc.y) * 90.0) * exp(-abs(fc.x) * 2.2) * 0.5);
  col *= 1.0 - 0.62 * smoothstep(0.15, 0.95, length(cc * vec2(1.25, 1.55)));    // vignette
  col = aces(col * uExposure);
  col = pow(col, vec3(1.0 / 2.2));
  float g = hash12(gl_FragCoord.xy + fract(uTime) * 91.7) + hash12(gl_FragCoord.yx * 1.31 + fract(uTime * 1.7) * 37.1) - 1.0;
  col += g * 0.030 * (1.0 - col) ;                                              // film grain, stronger in the shadows
  oCol = vec4(col, 1.0);
}
)GLSL";
