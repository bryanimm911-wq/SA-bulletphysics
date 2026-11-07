// =============================================================================
//  GTA SA Bullet Physics  (AML, arm64-v8a / 64-bit)
//  - Bullet drop  : peluru melengkung turun setelah jarak tertentu
//  - Ricochet     : peluru kena besi / kendaraan / jalan -> kecil kemungkinan mantul
//  - Tracer       : lintasan melengkung + pantulan digambar lewat libBulletTracers64.so
// =============================================================================
#include <mod/amlmod.h>
#include <android/log.h>
#include <dlfcn.h>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

MYMODCFG(net.local.sa.bulletphysics, GTASA BulletPhysics, 1.0.0, local)

#define TAG "SABulletPhysics"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

// ------------------------------------------------------------------ game types
struct CVector { float x, y, z; };
struct CEntity;
struct CColPoint {
    CVector point; float pad1;
    CVector normal; float pad2;
    uint8_t surfA, pieceA, lightA, surfB, pieceB, lightB, pad3[2];
};

// ------------------------------------------------------------------ AML glue
// (kalau nama fungsi di SDK AML-mu beda, cukup ubah 5 fungsi kecil ini)
static uintptr_t gtasaBase()                     { return aml->GetLib("libGTASA.so"); }
static void*     gtasaHandle()                   { return aml->GetLibHandle("libGTASA.so"); }
static void*     gtasaSym(const char* s)         { return aml->GetSym(gtasaHandle(), s); }
static void      hookFn(void* a, void* f, void** o) { aml->Hook(a, f, o); }
static const char* configDir()                   { return aml->GetConfigPath(); }

// ------------------------------------------------------------------ config
struct WeaponCfg { float velocity; float dropStart; float ricochetMul; };
static WeaponCfg W[64];

static struct {
    bool  enabled = true, affectNPC = true, bendLOS = true;
    float gravity = 9.81f, dropScale = 12.0f, maxDrop = 4.0f;
    float segLen = 6.0f; int maxSeg = 40; float minLOS = 6.0f;

    bool  ricochet = true;
    float ricBase = 0.06f, ricMetal = 1.0f, ricVehicle = 0.8f, ricGround = 0.5f;
    float ricSpread = 0.12f, ricRange = 35.0f, ricLoss = 0.6f;

    bool  suppressGameTracer = true;
    int   tracerMode = 0;                // 0 = AddTrace2 (per-weapon style), 1 = AddTrace (radius/time)
    float tracerRadius = 0.04f; uint32_t tracerTime = 300; int tracerAlpha = 255;
    float tracerSegLen = 20.0f; int tracerMaxSeg = 16;

    bool  logShots = false, logSurface = false;
    uintptr_t offAddTrace5 = 0x8438e8, offAddTrace2 = 0x846db0;   // dari libBulletTracers64.so
    uintptr_t offFIH = 0, offPLOS = 0, offFindPlayerPed = 0;       // 0 = pakai symbol
} C;

static uint8_t g_surfKind[256];   // 0 none, 1 metal, 2 vehicle, 3 ground

static void defaultWeapons() {
    //            id   velocity dropStart ricochetMul
    struct { int id; float v, s, r; } t[] = {
        {22,350,10,1},{23,320,10,1},{24,460,12,1},{25,400,8,0.5f},{26,380,8,0.5f},
        {27,400,8,0.5f},{28,380,12,1},{29,400,14,1},{30,715,25,1},{31,900,30,1},
        {32,360,12,1},{33,850,60,1},{34,920,120,1}};
    for (auto& e : t) W[e.id] = { e.v, e.s, e.r };
}
static void fillSurfaces(const char* list, uint8_t kind) {
    std::string s(list ? list : ""); size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && (s[i]==','||s[i]==' ')) i++;
        int a = atoi(s.c_str()+i), b = a;
        while (i < s.size() && isdigit((unsigned char)s[i])) i++;
        if (i < s.size() && s[i]=='-') { i++; b = atoi(s.c_str()+i); while (i < s.size() && isdigit((unsigned char)s[i])) i++; }
        for (int k = a; k <= b && k < 256; k++) if (k >= 0) g_surfKind[k] = kind;
    }
}
static std::string trim(std::string s){ size_t a=s.find_first_not_of(" \t\r\n"), b=s.find_last_not_of(" \t\r\n"); return a==std::string::npos?"":s.substr(a,b-a+1); }

static void loadConfig() {
    defaultWeapons();
    fillSurfaces("50-58", 1); fillSurfaces("63-65", 2); fillSurfaces("1-8,26,27", 3);
    char path[512]; snprintf(path, sizeof path, "%s/BulletPhysics.ini", configDir());
    FILE* f = fopen(path, "r");
    if (!f) { LOGI("config tidak ada (%s), pakai default", path); return; }
    LOGI("config: %s", path);
    char line[512]; std::string sec;
    while (fgets(line, sizeof line, f)) {
        std::string l = trim(line);
        if (l.empty() || l[0]==';' || l[0]=='#') continue;
        if (l[0]=='[') { sec = l.substr(1, l.find(']')-1); continue; }
        size_t eq = l.find('='); if (eq == std::string::npos) continue;
        std::string k = trim(l.substr(0,eq)), v = trim(l.substr(eq+1));
        size_t sc = v.find(';'); if (sc != std::string::npos) v = trim(v.substr(0,sc));
        float fv = (float)atof(v.c_str()); int iv = atoi(v.c_str()); bool bv = iv != 0;
        uintptr_t uv = (uintptr_t)strtoull(v.c_str(), nullptr, 0);

        if (sec == "General") {
            if (k=="Enabled") C.enabled=bv; else if (k=="AffectNPC") C.affectNPC=bv;
            else if (k=="LogShots") C.logShots=bv; else if (k=="LogSurface") C.logSurface=bv;
        } else if (sec == "Drop") {
            if (k=="Enabled") C.bendLOS=bv; else if (k=="Gravity") C.gravity=fv;
            else if (k=="DropScale") C.dropScale=fv; else if (k=="MaxDrop") C.maxDrop=fv;
            else if (k=="SegmentLength") C.segLen=fv; else if (k=="MaxSegments") C.maxSeg=iv;
            else if (k=="MinShotDistance") C.minLOS=fv;
        } else if (sec == "Ricochet") {
            if (k=="Enabled") C.ricochet=bv; else if (k=="Chance") C.ricBase=fv;
            else if (k=="MetalMul") C.ricMetal=fv; else if (k=="VehicleMul") C.ricVehicle=fv;
            else if (k=="GroundMul") C.ricGround=fv; else if (k=="Spread") C.ricSpread=fv;
            else if (k=="Range") C.ricRange=fv; else if (k=="SpeedLoss") C.ricLoss=fv;
            else if (k=="MetalSurfaces") { for(int i=0;i<256;i++) if(g_surfKind[i]==1) g_surfKind[i]=0; fillSurfaces(v.c_str(),1); }
            else if (k=="VehicleSurfaces") { for(int i=0;i<256;i++) if(g_surfKind[i]==2) g_surfKind[i]=0; fillSurfaces(v.c_str(),2); }
            else if (k=="GroundSurfaces") { for(int i=0;i<256;i++) if(g_surfKind[i]==3) g_surfKind[i]=0; fillSurfaces(v.c_str(),3); }
        } else if (sec == "Tracer") {
            if (k=="SuppressGameTracer") C.suppressGameTracer=bv; else if (k=="Mode") C.tracerMode=iv;
            else if (k=="Radius") C.tracerRadius=fv; else if (k=="TimeMs") C.tracerTime=(uint32_t)iv;
            else if (k=="Alpha") C.tracerAlpha=iv; else if (k=="SegmentLength") C.tracerSegLen=fv;
            else if (k=="MaxSegments") C.tracerMaxSeg=iv;
        } else if (sec == "Addresses") {
            if (k=="AddTrace5") C.offAddTrace5=uv; else if (k=="AddTrace2") C.offAddTrace2=uv;
            else if (k=="FireInstantHit") C.offFIH=uv; else if (k=="ProcessLineOfSight") C.offPLOS=uv;
            else if (k=="FindPlayerPed") C.offFindPlayerPed=uv;
        } else if (sec.rfind("Weapon_", 0) == 0) {
            int id = atoi(sec.c_str()+7); if (id < 0 || id >= 64) continue;
            if (k=="Velocity") W[id].velocity=fv; else if (k=="DropStart") W[id].dropStart=fv;
            else if (k=="RicochetMul") W[id].ricochetMul=fv;
        }
    }
    fclose(f);
}

// ------------------------------------------------------------------ vec helpers
static inline CVector operator+(CVector a, CVector b){ return {a.x+b.x,a.y+b.y,a.z+b.z}; }
static inline CVector operator-(CVector a, CVector b){ return {a.x-b.x,a.y-b.y,a.z-b.z}; }
static inline CVector operator*(CVector a, float s){ return {a.x*s,a.y*s,a.z*s}; }
static inline float dot(CVector a, CVector b){ return a.x*b.x+a.y*b.y+a.z*b.z; }
static inline float len(CVector a){ return sqrtf(dot(a,a)); }
static inline CVector norm(CVector a){ float l=len(a); return l>1e-6f? a*(1.0f/l) : CVector{0,0,1}; }
static inline float frand(){ return (float)rand()/(float)RAND_MAX; }

// ------------------------------------------------------------------ function pointers
typedef bool (*PLOS_t)(const CVector&, const CVector&, CColPoint&, CEntity*&, bool,bool,bool,bool,bool,bool,bool,bool);
typedef uintptr_t (*FIH_t)(uintptr_t,uintptr_t,uintptr_t,uintptr_t,uintptr_t,uintptr_t,uintptr_t,uintptr_t,uintptr_t);
typedef void* (*FindPlayerPed_t)(int);
typedef void (*AT5_t)(CVector*, CVector*, float, uint32_t, uint8_t);
typedef void (*AT2_t)(CVector*, CVector*, int, void*);

static PLOS_t PLOS_orig; static FIH_t FIH_orig; static FindPlayerPed_t FindPlayerPed_fn;
static AT5_t AT5_orig; static AT2_t AT2_orig;           // game (chain)
static AT5_t TRC_AT5;  static AT2_t TRC_AT2;            // libBulletTracers64.so (exported)

// ------------------------------------------------------------------ shot state
static bool g_inFire, g_arcValid, g_losDone, g_hasRic; static int g_wType; static bool g_shooterIsPlayer;
static std::vector<CVector> g_arc, g_ric;
static int g_sup5, g_sup2, g_shotNo;

static const WeaponCfg& wcfg(int t){ static WeaponCfg def{400,15,1}; return (t>=0&&t<64&&W[t].velocity>0)? W[t] : def; }

static float dropAt(const WeaponCfg& w, float s) {
    if (s <= w.dropStart) return 0.0f;
    float t = (s - w.dropStart) / w.velocity;
    float d = 0.5f * C.gravity * C.dropScale * t * t;
    return d > C.maxDrop ? C.maxDrop : d;
}

// ------------------------------------------------------------------ ricochet
static void tryRicochet(const CVector& dir, const CColPoint& cp, bool b0,bool b1,bool b2,bool b3,bool b4,bool b5,bool b6,bool b7)
{
    uint8_t surf = cp.surfB;
    if (C.logSurface) LOGI("hit surface id=%d weapon=%d", (int)surf, g_wType);
    if (!C.ricochet) return;
    int kind = g_surfKind[surf]; if (!kind) return;
    CVector n = cp.normal; if (dot(dir, n) > 0) n = n * -1.0f;
    float graze = 1.0f - fabsf(dot(dir, n));                    // 0 = tegak lurus, 1 = menyerempet
    float mul = kind==1 ? C.ricMetal : kind==2 ? C.ricVehicle : C.ricGround;
    float chance = C.ricBase * mul * wcfg(g_wType).ricochetMul * (0.4f + 1.6f * graze);
    if (frand() > chance) return;

    CVector r = dir - n * (2.0f * dot(dir, n));                 // pantulan
    r = norm(r + CVector{frand()-.5f, frand()-.5f, frand()-.5f} * (2.0f * C.ricSpread));
    if (dot(r, n) < 0.1f) r = norm(r + n * 0.4f);

    const WeaponCfg& w = wcfg(g_wType);
    WeaponCfg rw = w; rw.velocity = w.velocity * C.ricLoss; rw.dropStart = 0;
    CVector P = cp.point + n * 0.08f; g_ric.clear(); g_ric.push_back(P);
    CVector prev = P; const int N = 10; const float seg = C.ricRange / N;
    for (int i = 1; i <= N; i++) {
        float s = seg * i; CVector p = P + r * s; p.z -= dropAt(rw, s);
        CColPoint c2; CEntity* e2 = nullptr;
        if (PLOS_orig(prev, p, c2, e2, b0,b1,b2,b3,b4,b5,b6,b7)) { g_ric.push_back(c2.point); break; }
        g_ric.push_back(p); prev = p;
    }
    g_hasRic = g_ric.size() >= 2;
}

// ------------------------------------------------------------------ hook: ProcessLineOfSight
static bool PLOS_hook(const CVector& from, const CVector& to, CColPoint& cp, CEntity*& ent,
                      bool b0,bool b1,bool b2,bool b3,bool b4,bool b5,bool b6,bool b7)
{
    if (!g_inFire || !C.bendLOS) return PLOS_orig(from,to,cp,ent,b0,b1,b2,b3,b4,b5,b6,b7);
    CVector d = to - from; float R = len(d);
    if (R < C.minLOS) return PLOS_orig(from,to,cp,ent,b0,b1,b2,b3,b4,b5,b6,b7);

    const WeaponCfg& w = wcfg(g_wType); CVector dir = d * (1.0f / R);
    float seg = C.segLen; if (R / seg > C.maxSeg) seg = R / C.maxSeg;
    int n = (int)ceilf(R / seg);
    bool first = !g_losDone; if (first) { g_losDone = true; g_arc.clear(); g_arc.push_back(from); }

    CVector prev = from;
    for (int i = 1; i <= n; i++) {
        float s = (i == n) ? R : seg * i;
        CVector p = from + dir * s; p.z -= dropAt(w, s);
        if (PLOS_orig(prev, p, cp, ent, b0,b1,b2,b3,b4,b5,b6,b7)) {
            if (first) { g_arc.push_back(cp.point); g_arcValid = true; tryRicochet(norm(p - prev), cp, b0,b1,b2,b3,b4,b5,b6,b7); }
            return true;
        }
        if (first) g_arc.push_back(p);
        prev = p;
    }
    if (first) g_arcValid = true;
    return false;
}

// ------------------------------------------------------------------ tracer output
static void emitSeg(CVector a, CVector b) {
    if (TRC_AT2 && C.tracerMode == 0) TRC_AT2(&a, &b, g_wType, nullptr);
    else if (TRC_AT5)                 TRC_AT5(&a, &b, C.tracerRadius, C.tracerTime, (uint8_t)C.tracerAlpha);
    else if (AT2_orig)                AT2_orig(&a, &b, g_wType, nullptr);   // fallback: tracer bawaan game
}
static void emitPath(const std::vector<CVector>& pts) {
    if (pts.size() < 2) return;
    int n = (int)pts.size() - 1;
    float total = 0; for (int i = 0; i < n; i++) total += len(pts[i+1] - pts[i]);
    int want = (int)ceilf(total / C.tracerSegLen); if (want < 1) want = 1; if (want > C.tracerMaxSeg) want = C.tracerMaxSeg;
    int step = (n + want - 1) / want; if (step < 1) step = 1;
    for (int i0 = 0; i0 < n; ) { int i1 = i0 + step; if (i1 > n) i1 = n; emitSeg(pts[i0], pts[i1]); i0 = i1; }
}

// ------------------------------------------------------------------ hook: FireInstantHit
static uintptr_t FIH_hook(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3, uintptr_t a4,
                          uintptr_t a5, uintptr_t a6, uintptr_t a7, uintptr_t a8)
{
    if (!C.enabled || g_inFire || !a0) return FIH_orig(a0,a1,a2,a3,a4,a5,a6,a7,a8);

    bool isPlayer = FindPlayerPed_fn ? ((uintptr_t)FindPlayerPed_fn(-1) == a1) : true;
    if (!isPlayer && !C.affectNPC) return FIH_orig(a0,a1,a2,a3,a4,a5,a6,a7,a8);

    g_wType = *(int*)a0; g_shooterIsPlayer = isPlayer;
    g_inFire = true; g_arcValid = g_losDone = g_hasRic = false; g_arc.clear(); g_ric.clear(); g_sup5 = g_sup2 = 0;
    uintptr_t ret = FIH_orig(a0,a1,a2,a3,a4,a5,a6,a7,a8);
    g_inFire = false;

    if (g_arcValid) { emitPath(g_arc); if (g_hasRic) emitPath(g_ric); }
    if (C.logShots && g_shotNo < 60) { g_shotNo++;
        LOGI("shot#%d weapon=%d arcPts=%zu ricochet=%d suppressed(AT5=%d AT2=%d) tracerLib=%d",
             g_shotNo, g_wType, g_arc.size(), (int)g_hasRic, g_sup5, g_sup2, (int)(TRC_AT2||TRC_AT5)); }
    return ret;
}

// ------------------------------------------------------------------ hooks: tracer bawaan game (supaya garis lurus tidak dobel)
static void AT5_hook(CVector* a, CVector* b, float r, uint32_t t, uint8_t o) {
    if (C.suppressGameTracer && g_inFire && g_arcValid) { g_sup5++; return; }
    AT5_orig(a, b, r, t, o);
}
static void AT2_hook(CVector* a, CVector* b, int wt, void* e) {
    if (C.suppressGameTracer && g_inFire && g_arcValid) { g_sup2++; return; }
    AT2_orig(a, b, wt, e);
}

// ------------------------------------------------------------------ entry points
extern "C" void OnModLoad()
{
    srand((unsigned)time(nullptr));
    loadConfig();
    uintptr_t base = gtasaBase();

    FindPlayerPed_fn = (FindPlayerPed_t)(C.offFindPlayerPed ? base + C.offFindPlayerPed : (uintptr_t)gtasaSym("_Z13FindPlayerPedi"));
    void* fih  = C.offFIH  ? (void*)(base + C.offFIH)  : gtasaSym("_ZN7CWeapon14FireInstantHitEP7CEntityP7CVectorS3_S1_S3_S3_bb");
    void* plos = C.offPLOS ? (void*)(base + C.offPLOS) : gtasaSym("_ZN6CWorld18ProcessLineOfSightERK7CVectorS2_R9CColPointRP7CEntitybbbbbbbb");
    if (!fih || !plos) {
        LOGE("symbol tidak ketemu: FireInstantHit=%p ProcessLineOfSight=%p -> isi [Addresses] di BulletPhysics.ini", fih, plos);
        return;
    }
    hookFn(plos, (void*)PLOS_hook, (void**)&PLOS_orig);
    hookFn(fih,  (void*)FIH_hook,  (void**)&FIH_orig);
    LOGI("physics hooks OK (FireInstantHit=%p ProcessLineOfSight=%p)", fih, plos);
}

// dipanggil AML setelah SEMUA mod ter-load -> hook kita jadi paling luar (di atas hook tracer)
extern "C" void OnAllModsLoaded()
{
    void* h = dlopen("libBulletTracers64.so", RTLD_NOW | RTLD_NOLOAD);
    if (!h) h = aml->GetLibHandle("libBulletTracers64.so");
    if (h) {
        TRC_AT2 = (AT2_t)dlsym(h, "_ZN13CBulletTraces9AddTrace2EP7CVectorS1_11eWeaponTypeP7CEntity");
        TRC_AT5 = (AT5_t)dlsym(h, "_ZN13CBulletTraces8AddTraceEP7CVectorS1_fjh");
    }
    LOGI("tracer lib=%p AddTrace2=%p AddTrace=%p", h, (void*)TRC_AT2, (void*)TRC_AT5);

    uintptr_t base = gtasaBase();
    if (C.offAddTrace5) hookFn((void*)(base + C.offAddTrace5), (void*)AT5_hook, (void**)&AT5_orig);
    if (C.offAddTrace2) hookFn((void*)(base + C.offAddTrace2), (void*)AT2_hook, (void**)&AT2_orig);
}
