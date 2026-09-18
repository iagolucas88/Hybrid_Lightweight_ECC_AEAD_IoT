/*============================================================================
  Arquivo  : benchmarks/benchmark_suite_fullsystem.cpp
  Autor    : Iago Lucas (iagolbg@gmail.com | GitHub: iagolucas88)
  Curso    : Mestrado em Engenharia Mecatrônica — UFRN (Brasil)
  Propósito: Suite de benchmark refatorada para o ambiente FULL-SYSTEM
             (qemu-system-arm + Debian ARMHF completo), substituindo o fluxo
             anterior baseado em qemu-arm (user-space, sem kernel do guest).

  DIFERENÇA METODOLÓGICA IMPORTANTE (full-system vs. user-space):
    No modo user-space (qemu-arm), o binário ARM roda usando o KERNEL DO HOST,
    e apenas a CPU é emulada. Não há ruído de scheduler/paginação/syscalls de
    um SO ARM real. No full-system (qemu-system-arm + Debian ARMHF completo),
    o processo compete com um kernel Linux ARMHF real, com IRQs de dispositivo
    virtio, paginação, e um scheduler completo. Isso é mais representativo
    de um gateway IoT real, mas introduz RUÍDO adicional nas medições.

  MITIGAÇÃO DO RUÍDO: todo benchmark de tempo agora passa por um ciclo de
  WARM-UP (iterações descartadas antes da medição) para estabilizar caches,
  branch predictors e o "steady state" do scheduler do guest.

  EIXOS DE TESTE (mantendo a estrutura da dissertação):
    EIXO 1: Validação RFC e Robustez (vetores oficiais + testes negativos)
    EIXO 2: Primitivas e Escalabilidade (GMP 80..256 bits, KeyGen/Sign/HKDF)
    EIXO 3: Custo Amortizado e Throughput (sessão completa + AEAD)
    EIXO 4: Consumo de Memória de SO (getrusage + /proc/self/status)

  PORTABILIDADE: C++17 + GMP puro — mesmo código compila para x86 (validação
  local) e para ARMHF (cross-compilação, execução na VM full-system).
============================================================================*/

#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
#include <gmp.h>

namespace fs = std::filesystem;

// getrusage() é POSIX (disponível no Linux do host e no guest Debian ARMHF).
#include <sys/resource.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

using namespace std;
using Clock = chrono::steady_clock;
using Us    = chrono::microseconds;

/* ══════════════════════════════════════════════════════════════════════════
   SEÇÃO 0 — WARM-UP, TEMPORIZAÇÃO E MEDIÇÃO DE MEMÓRIA DE SO
   ═══════════════════════════════════════════════════════════════════════════ */

static const int WARMUP_ITERS_DEFAULT = 5;     // descartadas antes de medir

/// Executa `fn` `warmup` vezes (descartadas) e depois `iters` vezes,
/// coletando o tempo de cada execução medida em microssegundos.
template <class F>
static vector<double> warmup_and_time(F &&fn, int iters, int warmup = WARMUP_ITERS_DEFAULT) {
    for (int w = 0; w < warmup; ++w) fn();
    vector<double> samples;
    samples.reserve(iters);
    for (int i = 0; i < iters; ++i) {
        auto t0 = Clock::now();
        fn();
        auto t1 = Clock::now();
        samples.push_back(static_cast<double>(chrono::duration_cast<Us>(t1 - t0).count()));
    }
    return samples;
}

/// Snapshot de memória de processo, lido via POSIX getrusage() (pico de RSS)
/// e /proc/self/status (VmPeak/VmHWM), conforme exigido pela dissertação —
/// substitui qualquer estimativa teórica de consumo de memória da GMP.
struct MemUsage {
    long maxrss_kb = 0;   // getrusage(RUSAGE_SELF).ru_maxrss (pico de RSS)
    long vmpeak_kb = 0;   // /proc/self/status: pico de memória virtual
    long vmhwm_kb  = 0;   // /proc/self/status: pico de working set (RSS)
};

static MemUsage read_mem_usage() {
    MemUsage m;
    struct rusage ru{};
    if (getrusage(RUSAGE_SELF, &ru) == 0) {
        m.maxrss_kb = ru.ru_maxrss; // já em KB no Linux
    }
    ifstream f("/proc/self/status");
    string line;
    while (f && getline(f, line)) {
        long val = 0;
        if (sscanf(line.c_str(), "VmPeak: %ld kB", &val) == 1) m.vmpeak_kb = val;
        if (sscanf(line.c_str(), "VmHWM: %ld kB", &val) == 1)  m.vmhwm_kb  = val;
    }
    return m;
}

/* ══════════════════════════════════════════════════════════════════════════
   SEÇÃO 1 — LOGGER E ESTRUTURAS ESTATÍSTICAS (CSV com colunas de memória)
   ═══════════════════════════════════════════════════════════════════════════ */

struct BenchStats {
    string label;
    int n{0};
    double mean_us{0}, std_us{0}, min_us{0}, max_us{0};
    double median_us{0}, p5_us{0}, p95_us{0}, p99_us{0};
    long vmhwm_kb{0};   // pico de RSS (KB) capturado logo após o benchmark
};

class Logger {
public:
    void section(const string &title) {
        cout << "\n" << string(90, '=') << "\n " << title << "\n" << string(90, '=') << "\n";
    }
    void subsection(const string &title) {
        cout << "\n  " << title << "\n  " << string(title.size(), '-') << "\n";
    }
    void info(const string &msg)    { cout << "  [INFO] "  << msg << "\n"; }
    void ok(const string &msg)      { cout << "  [OK] "    << msg << "\n"; }
    void error(const string &msg)   { cout << "  [ERRO] "  << msg << "\n"; }
    void warning(const string &msg) { cout << "  [AVISO] " << msg << "\n"; }

    void test_result(bool pass, const string &name, const string &detail = "") {
        cout << (pass ? "    [PASS] " : "    [FAIL] ") << name;
        if (!detail.empty()) cout << " (" << detail << ")";
        cout << "\n";
    }

    void print_stats_table_header() {
        cout << "\n  " << left << setw(46) << "Operação"
             << right << setw(8) << "n"
             << setw(12) << "Média(us)" << setw(10) << "sigma"
             << setw(12) << "P95(us)" << setw(12) << "P99(us)"
             << setw(12) << "RSS(KB)\n";
        cout << "  " << string(112, '-') << "\n";
    }
    void print_stats_row(const BenchStats &s) {
        cout << "  " << left << setw(46) << s.label
             << right << setw(8) << s.n << fixed << setprecision(2)
             << setw(12) << s.mean_us << setw(10) << s.std_us
             << setw(12) << s.p95_us << setw(12) << s.p99_us
             << setw(12) << s.vmhwm_kb << "\n";
    }
};

static Logger logger;
static vector<BenchStats> all_results;

struct TestResult { string name; bool pass; string detail; };
static vector<TestResult> rfc_test_results;
static vector<TestResult> robustness_test_results;

static void export_test_results_csv(const vector<TestResult> &results, const string &fname) {
    ofstream f(fname);
    if (!f) { return; }
    f << "test,status,detail\n";
    for (const auto &r : results)
        f << "\"" << r.name << "\"," << (r.pass ? "PASS" : "FAIL") << ",\"" << r.detail << "\"\n";
}

static void export_memory_csv(const MemUsage &before, const MemUsage &after, const string &fname = "memory_usage.csv") {
    ofstream f(fname);
    if (!f) { return; }
    f << "metric,before_kb,after_kb\n";
    f << "getrusage_ru_maxrss," << before.maxrss_kb << "," << after.maxrss_kb << "\n";
    f << "proc_status_VmPeak," << before.vmpeak_kb << "," << after.vmpeak_kb << "\n";
    f << "proc_status_VmHWM," << before.vmhwm_kb << "," << after.vmhwm_kb << "\n";
}

static BenchStats compute_stats(const string &label, vector<double> samples) {
    assert(!samples.empty());
    sort(samples.begin(), samples.end());
    BenchStats r;
    r.label = label;
    r.n = static_cast<int>(samples.size());
    r.min_us = samples.front();
    r.max_us = samples.back();
    double sum = 0; for (auto v : samples) sum += v;
    r.mean_us = sum / r.n;
    double sq = 0; for (auto v : samples) sq += (v - r.mean_us) * (v - r.mean_us);
    r.std_us = sqrt(sq / r.n);
    r.median_us = samples[r.n / 2];
    r.p5_us  = samples[static_cast<size_t>(r.n * 0.05)];
    r.p95_us = samples[static_cast<size_t>(r.n * 0.95)];
    r.p99_us = samples[min<size_t>(r.n - 1, static_cast<size_t>(r.n * 0.99))];
    r.vmhwm_kb = read_mem_usage().vmhwm_kb;
    return r;
}

/* ══════════════════════════════════════════════════════════════════════════
   SEÇÃO 2 — PRIMITIVAS CRIPTOGRÁFICAS (Curve25519-like, SHA-512, HKDF,
              Ed25519, ChaCha20-Poly1305) — implementação idêntica à do
              protocolo (src/ECDSA_ECDH_C25519.cpp), reunida aqui para que
              o binário de benchmark seja autocontido (sem dependências
              cruzadas de arquivos, facilitando o cross-compile ARMHF).
   ═══════════════════════════════════════════════════════════════════════════ */

// ---- 2.1 Curva de Montgomery genérica (escalável 80..256 bits) ------------
struct CurveCtx {
    mpz_t p, a24, one, zero;
    int security_bits, field_bits;
    string name;
};
struct Proj { mpz_t x, z; };

static void proj_init(Proj &P, const mpz_t x, const mpz_t z) { mpz_init_set(P.x, x); mpz_init_set(P.z, z); }
static void proj_clear(Proj &P) { mpz_clear(P.x); mpz_clear(P.z); }
static void proj_swap(Proj &A, Proj &B, int cond) { if (cond) { mpz_swap(A.x, B.x); mpz_swap(A.z, B.z); } }

static void dadd(Proj &R0, Proj &R1, const mpz_t x_base, const CurveCtx &ctx) {
    mpz_t A, B, C, D, E, F, tmp;
    mpz_inits(A, B, C, D, E, F, tmp, nullptr);
    mpz_add(C, R1.x, R1.z); mpz_sub(D, R1.x, R1.z);
    mpz_add(A, R0.x, R0.z); mpz_sub(B, R0.x, R0.z);
    mpz_mul(E, B, B); mpz_mul(F, A, A);
    mpz_mul(R0.x, E, F); mpz_mod(R0.x, R0.x, ctx.p);
    mpz_sub(tmp, F, E);
    mpz_mul(R0.z, ctx.a24, tmp); mpz_add(R0.z, F, R0.z);
    mpz_mul(R0.z, tmp, R0.z); mpz_mod(R0.z, R0.z, ctx.p);
    mpz_mul(E, B, C); mpz_mul(F, A, D);
    mpz_add(R1.x, E, F);
    mpz_mul(R1.x, R1.x, R1.x); mpz_mod(R1.x, R1.x, ctx.p);
    mpz_sub(R1.z, E, F);
    mpz_mul(R1.z, R1.z, R1.z);
    mpz_mul(R1.z, R1.z, x_base); mpz_mod(R1.z, R1.z, ctx.p);
    mpz_clears(A, B, C, D, E, F, tmp, nullptr);
}

static void scalar_mul(mpz_t &result, const mpz_t k, const mpz_t u, const CurveCtx &ctx) {
    mpz_t kv; mpz_init_set(kv, k);
    Proj R0, R1;
    proj_init(R0, ctx.one, ctx.zero);
    proj_init(R1, u, ctx.one);
    size_t nb = mpz_sizeinbase(kv, 2);
    for (int i = (int)nb - 1; i >= 0; --i) {
        int bit = mpz_tstbit(kv, (mp_bitcnt_t)i);
        proj_swap(R0, R1, bit);
        dadd(R0, R1, u, ctx);
        proj_swap(R0, R1, bit);
    }
    mpz_t inv; mpz_init(inv);
    mpz_invert(inv, R0.z, ctx.p);
    mpz_mul(result, R0.x, inv);
    mpz_mod(result, result, ctx.p);
    mpz_clear(inv);
    proj_clear(R0); proj_clear(R1);
    mpz_clear(kv);
}

static void init_curve(CurveCtx &ctx, int sec_bits) {
    mpz_inits(ctx.p, ctx.a24, ctx.one, ctx.zero, nullptr);
    // a24 = (A-2)/4 = 121665 para A=486662 (RFC 7748 Sec.5). NOTA: o codigo
    // legado do projeto (src/ECDSA_ECDH_C25519.cpp) usa 121666 = (A+2)/4,
    // que produz uma curva estruturalmente similar mas NAO interoperavel
    // com X25519 padrao — ver EIXO 1a (validacao RFC) para a comprovacao.
    mpz_set_ui(ctx.a24, 121665);
    mpz_set_ui(ctx.one, 1);
    mpz_set_ui(ctx.zero, 0);
    ctx.security_bits = sec_bits;
    mpz_t t; mpz_init(t);
    switch (sec_bits) {
    case 80:
        ctx.field_bits = 160; ctx.name = "Curve25519-like (80-bit sec / 160-bit field)";
        mpz_ui_pow_ui(ctx.p, 2, 160); mpz_sub_ui(ctx.p, ctx.p, 1); mpz_nextprime(ctx.p, ctx.p);
        break;
    case 112:
        ctx.field_bits = 224; ctx.name = "Curve25519-like (112-bit sec / 224-bit field)";
        mpz_ui_pow_ui(ctx.p, 2, 224); mpz_ui_pow_ui(t, 2, 96); mpz_sub(ctx.p, ctx.p, t); mpz_add_ui(ctx.p, ctx.p, 1);
        break;
    case 128:
        ctx.field_bits = 255; ctx.name = "Curve25519 (128-bit sec / 255-bit field) [real]";
        mpz_ui_pow_ui(ctx.p, 2, 255); mpz_sub_ui(ctx.p, ctx.p, 19);
        break;
    case 192:
        ctx.field_bits = 384; ctx.name = "Curve25519-like (192-bit sec / 384-bit field)";
        mpz_ui_pow_ui(ctx.p, 2, 384);
        mpz_ui_pow_ui(t, 2, 128); mpz_sub(ctx.p, ctx.p, t);
        mpz_ui_pow_ui(t, 2, 96);  mpz_sub(ctx.p, ctx.p, t);
        mpz_ui_pow_ui(t, 2, 32);  mpz_add(ctx.p, ctx.p, t);
        mpz_sub_ui(ctx.p, ctx.p, 1);
        break;
    case 224:
        ctx.field_bits = 448; ctx.name = "Curve25519-like (224-bit sec / 448-bit field)";
        mpz_ui_pow_ui(ctx.p, 2, 448); mpz_ui_pow_ui(t, 2, 224); mpz_sub(ctx.p, ctx.p, t); mpz_sub_ui(ctx.p, ctx.p, 1);
        break;
    case 256:
        ctx.field_bits = 521; ctx.name = "Curve25519-like (256-bit sec / 521-bit field)";
        mpz_ui_pow_ui(ctx.p, 2, 521); mpz_sub_ui(ctx.p, ctx.p, 1);
        break;
    default: throw runtime_error("Nivel de seguranca desconhecido");
    }
    mpz_clear(t);
}
static void free_curve(CurveCtx &ctx) { mpz_clears(ctx.p, ctx.a24, ctx.one, ctx.zero, nullptr); }

static void fill_rand(uint8_t *out, size_t len) {
    ifstream u("/dev/urandom", ios::binary);
    if (!u) throw runtime_error("Falha ao abrir /dev/urandom");
    u.read((char *)out, (streamsize)len);
}
static void rand_scalar(mpz_t &k, const CurveCtx &ctx) {
    uint8_t buf[66]{};
    size_t bytes = min<size_t>((ctx.field_bits + 7) / 8, 66);
    fill_rand(buf, bytes);
    mpz_import(k, bytes, 1, 1, 0, 0, buf);
    mpz_mod(k, k, ctx.p);
    if (mpz_cmp_ui(k, 1) <= 0) mpz_set_ui(k, 2);
}

// ---- 2.2 SHA-512 / HMAC-SHA512 / HKDF-SHA512 (RFC 6234 / RFC 5869) --------
static const uint64_t SHA512_K[80] = {
    0x428a2f98d728ae22ULL,0x7137449123ef65cdULL,0xb5c0fbcfec4d3b2fULL,0xe9b5dba58189dbbcULL,
    0x3956c25bf348b538ULL,0x59f111f1b605d019ULL,0x923f82a4af194f9bULL,0xab1c5ed5da6d8118ULL,
    0xd807aa98a3030242ULL,0x12835b0145706fbeULL,0x243185be4ee4b28cULL,0x550c7dc3d5ffb4e2ULL,
    0x72be5d74f27b896fULL,0x80deb1fe3b1696b1ULL,0x9bdc06a725c71235ULL,0xc19bf174cf692694ULL,
    0xe49b69c19ef14ad2ULL,0xefbe4786384f25e3ULL,0x0fc19dc68b8cd5b5ULL,0x240ca1cc77ac9c65ULL,
    0x2de92c6f592b0275ULL,0x4a7484aa6ea6e483ULL,0x5cb0a9dcbd41fbd4ULL,0x76f988da831153b5ULL,
    0x983e5152ee66dfabULL,0xa831c66d2db43210ULL,0xb00327c898fb213fULL,0xbf597fc7beef0ee4ULL,
    0xc6e00bf33da88fc2ULL,0xd5a79147930aa725ULL,0x06ca6351e003826fULL,0x142929670a0e6e70ULL,
    0x27b70a8546d22ffcULL,0x2e1b21385c26c926ULL,0x4d2c6dfc5ac42aedULL,0x53380d139d95b3dfULL,
    0x650a73548baf63deULL,0x766a0abb3c77b2a8ULL,0x81c2c92e47edaee6ULL,0x92722c851482353bULL,
    0xa2bfe8a14cf10364ULL,0xa81a664bbc423001ULL,0xc24b8b70d0f89791ULL,0xc76c51a30654be30ULL,
    0xd192e819d6ef5218ULL,0xd69906245565a910ULL,0xf40e35855771202aULL,0x106aa07032bbd1b8ULL,
    0x19a4c116b8d2d0c8ULL,0x1e376c085141ab53ULL,0x2748774cdf8eeb99ULL,0x34b0bcb5e19b48a8ULL,
    0x391c0cb3c5c95a63ULL,0x4ed8aa4ae3418acbULL,0x5b9cca4f7763e373ULL,0x682e6ff3d6b2b8a3ULL,
    0x748f82ee5defb2fcULL,0x78a5636f43172f60ULL,0x84c87814a1f0ab72ULL,0x8cc702081a6439ecULL,
    0x90befffa23631e28ULL,0xa4506cebde82bde9ULL,0xbef9a3f7b2c67915ULL,0xc67178f2e372532bULL,
    0xca273eceea26619cULL,0xd186b8c721c0c207ULL,0xeada7dd6cde0eb1eULL,0xf57d4f7fee6ed178ULL,
    0x06f067aa72176fbaULL,0x0a637dc5a2c898a6ULL,0x113f9804bef90daeULL,0x1b710b35131c471bULL,
    0x28db77f523047d84ULL,0x32caab7b40c72493ULL,0x3c9ebe0a15c9bebcULL,0x431d67c49c100d4cULL,
    0x4cc5d4becb3e42b6ULL,0x597f299cfc657e2aULL,0x5fcb6fab3ad6faecULL,0x6c44198c4a475817ULL
};
static inline uint64_t rotr64(uint64_t x, int n) noexcept { return (x >> n) | (x << (64 - n)); }
static void sha512_compress(uint64_t st[8], const uint8_t b[128]) noexcept {
    uint64_t W[80];
    for (int i = 0; i < 16; ++i) { W[i] = 0; for (int j = 0; j < 8; ++j) W[i] = (W[i] << 8) | b[i * 8 + j]; }
    for (int i = 16; i < 80; ++i) {
        uint64_t s0 = rotr64(W[i-15],1)^rotr64(W[i-15],8)^(W[i-15]>>7);
        uint64_t s1 = rotr64(W[i-2],19)^rotr64(W[i-2],61)^(W[i-2]>>6);
        W[i] = W[i-16] + s0 + W[i-7] + s1;
    }
    uint64_t a=st[0],b2=st[1],c=st[2],d=st[3],e=st[4],f=st[5],g=st[6],h=st[7];
    for (int i = 0; i < 80; ++i) {
        uint64_t S1 = rotr64(e,14)^rotr64(e,18)^rotr64(e,41);
        uint64_t ch = (e & f) ^ (~e & g);
        uint64_t t1 = h + S1 + ch + SHA512_K[i] + W[i];
        uint64_t S0 = rotr64(a,28)^rotr64(a,34)^rotr64(a,39);
        uint64_t maj = (a & b2) ^ (a & c) ^ (b2 & c);
        uint64_t t2 = S0 + maj;
        h=g;g=f;f=e;e=d+t1;d=c;c=b2;b2=a;a=t1+t2;
    }
    st[0]+=a;st[1]+=b2;st[2]+=c;st[3]+=d;st[4]+=e;st[5]+=f;st[6]+=g;st[7]+=h;
}
static array<uint8_t,64> sha512(const uint8_t *data, size_t len) {
    uint64_t st[8]={0x6a09e667f3bcc908ULL,0xbb67ae8584caa73bULL,0x3c6ef372fe94f82bULL,0xa54ff53a5f1d36f1ULL,
                    0x510e527fade682d1ULL,0x9b05688c2b3e6c1fULL,0x1f83d9abfb41bd6bULL,0x5be0cd19137e2179ULL};
    uint8_t blk[128]; size_t i=0;
    while (i+128<=len){sha512_compress(st,data+i);i+=128;}
    size_t r=len-i; memset(blk,0,128); memcpy(blk,data+i,r); blk[r]=0x80;
    if (r>=112){sha512_compress(st,blk);memset(blk,0,128);}
    uint64_t bits=(uint64_t)len*8;
    for (int j=7;j>=0;--j){blk[120+j]=bits&0xFF;bits>>=8;}
    sha512_compress(st,blk);
    array<uint8_t,64> out{};
    for (int j=0;j<8;++j) for (int k=7;k>=0;--k) out[j*8+(7-k)]=(uint8_t)(st[j]>>(k*8));
    return out;
}
static array<uint8_t,64> hmac_sha512(const uint8_t *key, size_t kl, const uint8_t *msg, size_t ml) {
    constexpr size_t B=128; uint8_t k[B]{};
    if (kl>B){auto h=sha512(key,kl);memcpy(k,h.data(),64);} else memcpy(k,key,kl);
    uint8_t ip[B],op[B];
    for (size_t j=0;j<B;++j){ip[j]=k[j]^0x36;op[j]=k[j]^0x5C;}
    vector<uint8_t> in(B+ml); memcpy(in.data(),ip,B); memcpy(in.data()+B,msg,ml);
    auto hi=sha512(in.data(),in.size());
    vector<uint8_t> ou(B+64); memcpy(ou.data(),op,B); memcpy(ou.data()+B,hi.data(),64);
    return sha512(ou.data(),ou.size());
}
static vector<uint8_t> hkdf(const uint8_t *ikm, size_t il, const uint8_t *salt, size_t sl,
                             const uint8_t *info, size_t infl, size_t okl) {
    static const uint8_t ds[64]{};
    if (!salt||sl==0){salt=ds;sl=64;}
    auto prk=hmac_sha512(salt,sl,ikm,il);
    vector<uint8_t> okm; okm.reserve(okl);
    array<uint8_t,64> T{}; size_t tl=0; uint8_t ctr=1;
    while (okm.size()<okl){
        vector<uint8_t> buf(tl+infl+1); memcpy(buf.data(),T.data(),tl);
        if (info&&infl) memcpy(buf.data()+tl,info,infl); buf[tl+infl]=ctr++;
        T=hmac_sha512(prk.data(),64,buf.data(),buf.size()); tl=64;
        size_t take=min<size_t>(64,okl-okm.size());
        okm.insert(okm.end(),T.begin(),T.begin()+take);
    }
    return okm;
}

// ---- 2.3 Ed25519 (RFC 8032) — idêntico ao usado no protocolo -------------
static mpz_t ED_p, ED_d, ED_l, ED_Bx, ED_By, ED_I;
struct EdPoint { mpz_t X,Y,Z,T; EdPoint(){mpz_inits(X,Y,Z,T,nullptr);} ~EdPoint(){mpz_clears(X,Y,Z,T,nullptr);} };
static void ed_set_neutral(EdPoint &P){mpz_set_ui(P.X,0);mpz_set_ui(P.Y,1);mpz_set_ui(P.Z,1);mpz_set_ui(P.T,0);}
static void ed_copy(EdPoint &d,const EdPoint &s){mpz_set(d.X,s.X);mpz_set(d.Y,s.Y);mpz_set(d.Z,s.Z);mpz_set(d.T,s.T);}
static void ed_add(EdPoint &R,const EdPoint &P,const EdPoint &Q){
    mpz_t A,B,C,D,E,F,G,H,t1,t2; mpz_inits(A,B,C,D,E,F,G,H,t1,t2,nullptr);
    mpz_mul(A,P.X,Q.X); mpz_mod(A,A,ED_p);
    mpz_mul(B,P.Y,Q.Y); mpz_mod(B,B,ED_p);
    mpz_mul(C,P.T,Q.T); mpz_mod(C,C,ED_p); mpz_mul(C,C,ED_d); mpz_mod(C,C,ED_p);
    mpz_mul(D,P.Z,Q.Z); mpz_mod(D,D,ED_p);
    mpz_add(t1,P.X,P.Y); mpz_add(t2,Q.X,Q.Y);
    mpz_mul(E,t1,t2); mpz_mod(E,E,ED_p); mpz_sub(E,E,A); mpz_sub(E,E,B); mpz_mod(E,E,ED_p);
    mpz_sub(F,D,C); mpz_mod(F,F,ED_p);
    mpz_add(G,D,C); mpz_mod(G,G,ED_p);
    mpz_add(H,B,A); mpz_mod(H,H,ED_p);
    mpz_mul(R.X,E,F); mpz_mod(R.X,R.X,ED_p);
    mpz_mul(R.Y,G,H); mpz_mod(R.Y,R.Y,ED_p);
    mpz_mul(R.Z,F,G); mpz_mod(R.Z,R.Z,ED_p);
    mpz_mul(R.T,E,H); mpz_mod(R.T,R.T,ED_p);
    mpz_clears(A,B,C,D,E,F,G,H,t1,t2,nullptr);
}
static void ed_scalar_mul(EdPoint &R,const mpz_t k,const EdPoint &P){
    ed_set_neutral(R); EdPoint addend; ed_copy(addend,P);
    size_t nbits=mpz_sizeinbase(k,2);
    for(size_t i=0;i<nbits;++i){
        if(mpz_tstbit(k,(mp_bitcnt_t)i)){ EdPoint tmp; ed_add(tmp,R,addend); ed_copy(R,tmp); }
        EdPoint tmp2; ed_add(tmp2,addend,addend); ed_copy(addend,tmp2);
    }
}
static array<uint8_t,32> ed_compress(const EdPoint &P){
    mpz_t Zinv,xa,ya; mpz_inits(Zinv,xa,ya,nullptr);
    mpz_invert(Zinv,P.Z,ED_p);
    mpz_mul(xa,P.X,Zinv); mpz_mod(xa,xa,ED_p);
    mpz_mul(ya,P.Y,Zinv); mpz_mod(ya,ya,ED_p);
    array<uint8_t,32> out{}; size_t w=0;
    mpz_export(out.data(),&w,-1,1,0,0,ya);
    if(mpz_odd_p(xa)) out[31]|=0x80;
    mpz_clears(Zinv,xa,ya,nullptr);
    return out;
}
static void ed_decompress(EdPoint &P,const array<uint8_t,32> &enc){
    array<uint8_t,32> bytes=enc;
    int sx=(bytes[31]>>7)&1; bytes[31]&=0x7F;
    mpz_t y,y2,x2,x,exp,num,den,chk; mpz_inits(y,y2,x2,x,exp,num,den,chk,nullptr);
    mpz_import(y,32,-1,1,0,0,bytes.data());
    mpz_mul(y2,y,y); mpz_mod(y2,y2,ED_p);
    mpz_sub_ui(num,y2,1); mpz_mod(num,num,ED_p);
    mpz_mul(den,ED_d,y2); mpz_mod(den,den,ED_p); mpz_add_ui(den,den,1); mpz_mod(den,den,ED_p);
    mpz_t di; mpz_init(di); mpz_invert(di,den,ED_p);
    mpz_mul(x2,num,di); mpz_mod(x2,x2,ED_p);
    mpz_add_ui(exp,ED_p,3); mpz_cdiv_q_ui(exp,exp,8);
    mpz_powm(x,x2,exp,ED_p);
    mpz_mul(chk,x,x); mpz_mod(chk,chk,ED_p);
    if(mpz_cmp(chk,x2)!=0){ mpz_mul(x,x,ED_I); mpz_mod(x,x,ED_p); }
    int xodd=mpz_odd_p(x)?1:0;
    if(xodd!=sx){ mpz_sub(x,ED_p,x); mpz_mod(x,x,ED_p); }
    mpz_set(P.X,x); mpz_set(P.Y,y); mpz_set_ui(P.Z,1);
    mpz_mul(P.T,x,y); mpz_mod(P.T,P.T,ED_p);
    mpz_clears(y,y2,x2,x,exp,num,den,chk,di,nullptr);
}
static void reduce512_mod_l(mpz_t &r,const array<uint8_t,64> &bytes_le){
    mpz_t v; mpz_init(v); mpz_import(v,64,-1,1,0,0,bytes_le.data()); mpz_mod(r,v,ED_l); mpz_clear(v);
}
static void init_ed25519(){
    mpz_init_set_str(ED_p,"57896044618658097711785492504343953926634992332820282019728792003956564819949",10);
    mpz_init_set_str(ED_l,"7237005577332262213973186563042994240857116359379907606001950938285454250989",10);
    mpz_t num,den,di; mpz_init_set_ui(num,121665); mpz_init_set_ui(den,121666); mpz_init(di);
    mpz_invert(di,den,ED_p); mpz_init(ED_d);
    mpz_mul(ED_d,num,di); mpz_mod(ED_d,ED_d,ED_p);
    mpz_sub(ED_d,ED_p,ED_d); mpz_mod(ED_d,ED_d,ED_p);
    mpz_clears(num,den,di,nullptr);
    mpz_t e2,base2; mpz_init(e2); mpz_init_set_ui(base2,2);
    mpz_sub_ui(e2,ED_p,1); mpz_cdiv_q_ui(e2,e2,4);
    mpz_init(ED_I); mpz_powm(ED_I,base2,e2,ED_p);
    mpz_clears(e2,base2,nullptr);
    mpz_t four,five,finv; mpz_init_set_ui(four,4); mpz_init_set_ui(five,5); mpz_init(finv);
    mpz_invert(finv,five,ED_p); mpz_init(ED_By);
    mpz_mul(ED_By,four,finv); mpz_mod(ED_By,ED_By,ED_p);
    mpz_clears(four,five,finv,nullptr);
    mpz_t by2,xn,xd,xdi,x2,exb,chk; mpz_inits(by2,xn,xd,xdi,x2,exb,chk,nullptr);
    mpz_mul(by2,ED_By,ED_By); mpz_mod(by2,by2,ED_p);
    mpz_sub_ui(xn,by2,1); mpz_mod(xn,xn,ED_p);
    mpz_mul(xd,ED_d,by2); mpz_mod(xd,xd,ED_p); mpz_add_ui(xd,xd,1); mpz_mod(xd,xd,ED_p);
    mpz_invert(xdi,xd,ED_p); mpz_mul(x2,xn,xdi); mpz_mod(x2,x2,ED_p);
    mpz_add_ui(exb,ED_p,3); mpz_cdiv_q_ui(exb,exb,8);
    mpz_init(ED_Bx); mpz_powm(ED_Bx,x2,exb,ED_p);
    mpz_mul(chk,ED_Bx,ED_Bx); mpz_mod(chk,chk,ED_p);
    if(mpz_cmp(chk,x2)!=0){ mpz_mul(ED_Bx,ED_Bx,ED_I); mpz_mod(ED_Bx,ED_Bx,ED_p); }
    if(mpz_odd_p(ED_Bx)){ mpz_sub(ED_Bx,ED_p,ED_Bx); }
    mpz_clears(by2,xn,xd,xdi,x2,exb,chk,nullptr);
}
static void clear_ed25519(){ mpz_clears(ED_p,ED_d,ED_l,ED_Bx,ED_By,ED_I,nullptr); }

static void gera_bytes_aleatorios(uint8_t *out,size_t len){ fill_rand(out,len); }

struct Ed25519KeyPair { array<uint8_t,64> priv_expanded; array<uint8_t,32> pub; };

/// Constrói o par de chaves a partir de uma SEED de 32 bytes (uso: RFC test e produção).
static Ed25519KeyPair ed25519_keygen_from_seed(const array<uint8_t,32> &seed) {
    Ed25519KeyPair kp{};
    auto h=sha512(seed.data(),32);
    copy(h.begin(),h.end(),kp.priv_expanded.begin());
    kp.priv_expanded[0]&=0xF8; kp.priv_expanded[31]&=0x3F; kp.priv_expanded[31]|=0x40;
    mpz_t s; mpz_init(s);
    array<uint8_t,32> sb; copy(kp.priv_expanded.begin(),kp.priv_expanded.begin()+32,sb.begin());
    mpz_import(s,32,-1,1,0,0,sb.data());
    EdPoint B,A;
    mpz_set(B.X,ED_Bx); mpz_set(B.Y,ED_By); mpz_set_ui(B.Z,1);
    mpz_mul(B.T,ED_Bx,ED_By); mpz_mod(B.T,B.T,ED_p);
    ed_scalar_mul(A,s,B);
    kp.pub=ed_compress(A);
    mpz_clear(s);
    return kp;
}
static Ed25519KeyPair ed25519_keygen() {
    array<uint8_t,32> seed; gera_bytes_aleatorios(seed.data(),32);
    return ed25519_keygen_from_seed(seed);
}
static array<uint8_t,64> ed25519_sign(const Ed25519KeyPair &kp,const uint8_t *msg,size_t msg_len){
    vector<uint8_t> ni(32+msg_len);
    copy(kp.priv_expanded.begin()+32,kp.priv_expanded.end(),ni.begin());
    copy(msg,msg+msg_len,ni.begin()+32);
    auto rh=sha512(ni.data(),ni.size());
    mpz_t r; mpz_init(r); reduce512_mod_l(r,rh);
    EdPoint B,Rpt;
    mpz_set(B.X,ED_Bx); mpz_set(B.Y,ED_By); mpz_set_ui(B.Z,1);
    mpz_mul(B.T,ED_Bx,ED_By); mpz_mod(B.T,B.T,ED_p);
    ed_scalar_mul(Rpt,r,B);
    auto R_bytes=ed_compress(Rpt);
    vector<uint8_t> ki; ki.insert(ki.end(),R_bytes.begin(),R_bytes.end());
    ki.insert(ki.end(),kp.pub.begin(),kp.pub.end());
    ki.insert(ki.end(),msg,msg+msg_len);
    auto kh=sha512(ki.data(),ki.size());
    mpz_t k; mpz_init(k); reduce512_mod_l(k,kh);
    array<uint8_t,32> sb; copy(kp.priv_expanded.begin(),kp.priv_expanded.begin()+32,sb.begin());
    mpz_t s; mpz_init(s); mpz_import(s,32,-1,1,0,0,sb.data());
    mpz_t S; mpz_init(S);
    mpz_mul(S,k,s); mpz_add(S,S,r); mpz_mod(S,S,ED_l);
    array<uint8_t,64> sig{};
    copy(R_bytes.begin(),R_bytes.end(),sig.begin());
    size_t w=0; mpz_export(sig.data()+32,&w,-1,1,0,0,S);
    mpz_clears(r,k,s,S,nullptr);
    return sig;
}
static bool ed25519_verify(const array<uint8_t,32> &pub,const uint8_t *msg,size_t msg_len,const array<uint8_t,64> &sig){
    try {
        array<uint8_t,32> R_bytes,S_bytes;
        copy(sig.begin(),sig.begin()+32,R_bytes.begin());
        copy(sig.begin()+32,sig.end(),S_bytes.begin());
        mpz_t S; mpz_init(S); mpz_import(S,32,-1,1,0,0,S_bytes.data());
        if(mpz_cmp(S,ED_l)>=0){ mpz_clear(S); return false; }
        vector<uint8_t> ki; ki.insert(ki.end(),R_bytes.begin(),R_bytes.end());
        ki.insert(ki.end(),pub.begin(),pub.end());
        ki.insert(ki.end(),msg,msg+msg_len);
        auto kh=sha512(ki.data(),ki.size());
        mpz_t k; mpz_init(k); reduce512_mod_l(k,kh);
        EdPoint Rpt,Apt,B;
        ed_decompress(Rpt,R_bytes); ed_decompress(Apt,pub);
        mpz_set(B.X,ED_Bx); mpz_set(B.Y,ED_By); mpz_set_ui(B.Z,1);
        mpz_mul(B.T,ED_Bx,ED_By); mpz_mod(B.T,B.T,ED_p);
        EdPoint SB,kA,RkA;
        ed_scalar_mul(SB,S,B); ed_scalar_mul(kA,k,Apt); ed_add(RkA,Rpt,kA);
        auto lhs=ed_compress(SB), rhs=ed_compress(RkA);
        uint8_t diff=0; for(size_t i=0;i<32;++i) diff|=(lhs[i]^rhs[i]);
        mpz_clears(S,k,nullptr);
        return diff==0;
    } catch(...) { return false; }
}

// ---- 2.4 ChaCha20-Poly1305 AEAD (RFC 8439) --------------------------------
using ChaChaKey=array<uint8_t,32>; using ChaChaNonce=array<uint8_t,12>; using Poly1305Tag=array<uint8_t,16>;
struct AeadCiphertext { vector<uint8_t> ciphertext; ChaChaNonce nonce; Poly1305Tag tag; };
static inline uint32_t load32_le(const uint8_t *p) noexcept { return uint32_t(p[0])|(uint32_t(p[1])<<8)|(uint32_t(p[2])<<16)|(uint32_t(p[3])<<24); }
static inline void store32_le(uint8_t *p,uint32_t v) noexcept { p[0]=v;p[1]=v>>8;p[2]=v>>16;p[3]=v>>24; }
static inline uint64_t load64_le(const uint8_t *p) noexcept { return uint64_t(load32_le(p))|(uint64_t(load32_le(p+4))<<32); }
static inline void store64_le(uint8_t *p,uint64_t v) noexcept { store32_le(p,uint32_t(v)); store32_le(p+4,uint32_t(v>>32)); }
static inline uint32_t rotl32(uint32_t x,int n) noexcept { return (x<<n)|(x>>(32-n)); }
static inline void quarter_round(uint32_t &a,uint32_t &b,uint32_t &c,uint32_t &d) noexcept {
    a+=b;d^=a;d=rotl32(d,16);c+=d;b^=c;b=rotl32(b,12);
    a+=b;d^=a;d=rotl32(d, 8);c+=d;b^=c;b=rotl32(b, 7);
}
static void chacha20_xor(const ChaChaKey &key,const ChaChaNonce &nonce,uint32_t counter,uint8_t *out,size_t len) noexcept {
    static const uint32_t SIG[4]={0x61707865u,0x3320646eu,0x79622d32u,0x6b206574u};
    uint8_t blk[64]; size_t off=0;
    while(off<len){
        uint32_t st[16];
        st[0]=SIG[0];st[1]=SIG[1];st[2]=SIG[2];st[3]=SIG[3];
        for(int i=0;i<8;++i) st[4+i]=load32_le(key.data()+i*4);
        st[12]=counter; st[13]=load32_le(nonce.data()); st[14]=load32_le(nonce.data()+4); st[15]=load32_le(nonce.data()+8);
        uint32_t ws[16]; memcpy(ws,st,64);
        for(int r=0;r<10;++r){
            quarter_round(ws[0],ws[4],ws[ 8],ws[12]); quarter_round(ws[1],ws[5],ws[ 9],ws[13]);
            quarter_round(ws[2],ws[6],ws[10],ws[14]); quarter_round(ws[3],ws[7],ws[11],ws[15]);
            quarter_round(ws[0],ws[5],ws[10],ws[15]); quarter_round(ws[1],ws[6],ws[11],ws[12]);
            quarter_round(ws[2],ws[7],ws[ 8],ws[13]); quarter_round(ws[3],ws[4],ws[ 9],ws[14]);
        }
        for(int i=0;i<16;++i) ws[i]+=st[i];
        for(int i=0;i<16;++i) store32_le(blk+i*4,ws[i]);
        size_t ch=min<size_t>(64,len-off);
        for(size_t i=0;i<ch;++i) out[off+i]^=blk[i];
        ++counter; off+=ch;
    }
}
static Poly1305Tag poly1305_mac(const array<uint8_t,32> &key,const uint8_t *msg,size_t len) noexcept {
    uint8_t rb[16]; memcpy(rb,key.data(),16);
    rb[3]&=0x0F;rb[7]&=0x0F;rb[11]&=0x0F;rb[15]&=0x0F; rb[4]&=0xFC;rb[8]&=0xFC;rb[12]&=0xFC;
    uint64_t s0=load64_le(key.data()+16), s1=load64_le(key.data()+24);
    uint64_t r0=load32_le(rb)&0x3FFFFFFULL;
    uint64_t r1=(load32_le(rb+3)>>2)&0x3FFFF03ULL;
    uint64_t r2=(load32_le(rb+6)>>4)&0x3FFC0FFULL;
    uint64_t r3=(load32_le(rb+9)>>6)&0x3F03FFFULL;
    uint64_t r4=(load32_le(rb+12)>>8)&0x00FFFFFULL;
    uint64_t h0=0,h1=0,h2=0,h3=0,h4=0;
    // Cada bloco (completo ou parcial) recebe o bit terminador 0x01 logo
    // apos os dados (RFC 8439 Sec. 2.5.1: "Add one bit beyond the number
    // of octets"), embutido no byte buf[bl] — independente de ser o ultimo.
    auto blk=[&](const uint8_t *b,size_t bl){
        uint8_t buf[17]{}; memcpy(buf,b,bl); buf[bl]=1;
        uint64_t n0=load64_le(buf),n1=load64_le(buf+8),n2=buf[16];
        h0+=n0&0x3FFFFFFULL; h1+=(n0>>26)&0x3FFFFFFULL;
        h2+=((n0>>52)|(n1<<12))&0x3FFFFFFULL; h3+=(n1>>14)&0x3FFFFFFULL;
        h4+=(n1>>40)|(n2<<24);
        auto d0=h0*r0+h1*(r4*5)+h2*(r3*5)+h3*(r2*5)+h4*(r1*5);
        auto d1=h0*r1+h1*r0   +h2*(r4*5)+h3*(r3*5)+h4*(r2*5);
        auto d2=h0*r2+h1*r1   +h2*r0    +h3*(r4*5)+h4*(r3*5);
        auto d3=h0*r3+h1*r2   +h2*r1    +h3*r0    +h4*(r4*5);
        auto d4=h0*r4+h1*r3   +h2*r2    +h3*r1    +h4*r0;
        uint64_t c;
        c=d0>>26;h0=d0&0x3FFFFFF;d1+=c; c=d1>>26;h1=d1&0x3FFFFFF;d2+=c;
        c=d2>>26;h2=d2&0x3FFFFFF;d3+=c; c=d3>>26;h3=d3&0x3FFFFFF;d4+=c;
        c=d4>>26;h4=d4&0x3FFFFFF;h0+=c*5; c=h0>>26;h0&=0x3FFFFFF;h1+=c;
    };
    size_t off=0;
    while(off+16<=len){blk(msg+off,16);off+=16;}
    if(off<len) blk(msg+off,len-off);
    uint64_t c;
    c=h1>>26;h1&=0x3FFFFFF;h2+=c; c=h2>>26;h2&=0x3FFFFFF;h3+=c;
    c=h3>>26;h3&=0x3FFFFFF;h4+=c; c=h4>>26;h4&=0x3FFFFFF;h0+=c*5;
    c=h0>>26;h0&=0x3FFFFFF;h1+=c;
    uint64_t g0=h0+5;c=g0>>26;g0&=0x3FFFFFF;
    uint64_t g1=h1+c;c=g1>>26;g1&=0x3FFFFFF;
    uint64_t g2=h2+c;c=g2>>26;g2&=0x3FFFFFF;
    uint64_t g3=h3+c;c=g3>>26;g3&=0x3FFFFFF;
    uint64_t g4=h4+c-(1ULL<<26);
    uint64_t mask=(g4>>63)-1;
    h0=(h0&~mask)|(g0&mask);h1=(h1&~mask)|(g1&mask);
    h2=(h2&~mask)|(g2&mask);h3=(h3&~mask)|(g3&mask);h4=(h4&~mask)|(g4&mask);
    uint64_t f0=((h0)|(h1<<26)|(h2<<52))+s0;
    uint64_t f1=((h2>>12)|(h3<<14)|(h4<<40))+s1+(f0<s0?1:0);
    Poly1305Tag tag{}; store64_le(tag.data(),f0); store64_le(tag.data()+8,f1);
    return tag;
}
static vector<uint8_t> poly1305_msg(const uint8_t *aad,size_t al,const uint8_t *ct,size_t cl){
    auto p16=[](size_t n){return (16-(n%16))%16;};
    vector<uint8_t> buf;
    if(al) buf.insert(buf.end(),aad,aad+al);
    buf.resize(buf.size()+p16(al),0);
    if(cl) buf.insert(buf.end(),ct,ct+cl);
    buf.resize(buf.size()+p16(cl),0);
    uint8_t tmp[8];
    store64_le(tmp,al); buf.insert(buf.end(),tmp,tmp+8);
    store64_le(tmp,cl); buf.insert(buf.end(),tmp,tmp+8);
    return buf;
}
static AeadCiphertext chacha20poly1305_encrypt(const ChaChaKey &key,const ChaChaNonce &nonce,
        const uint8_t *pt,size_t pt_len,const uint8_t *aad,size_t aad_len) {
    AeadCiphertext res; res.nonce=nonce; res.ciphertext.resize(pt_len);
    array<uint8_t,64> pb{}; chacha20_xor(key,nonce,0,pb.data(),64);
    array<uint8_t,32> pk; copy(pb.begin(),pb.begin()+32,pk.begin());
    if(pt_len){ copy(pt,pt+pt_len,res.ciphertext.begin()); chacha20_xor(key,nonce,1,res.ciphertext.data(),pt_len); }
    auto pm=poly1305_msg(aad,aad_len,res.ciphertext.data(),pt_len);
    res.tag=poly1305_mac(pk,pm.data(),pm.size());
    return res;
}
static vector<uint8_t> chacha20poly1305_decrypt(const ChaChaKey &key,const ChaChaNonce &nonce,
        const uint8_t *ct,size_t ct_len,const Poly1305Tag &tag,const uint8_t *aad,size_t aad_len) {
    array<uint8_t,64> pb{}; chacha20_xor(key,nonce,0,pb.data(),64);
    array<uint8_t,32> pk; copy(pb.begin(),pb.begin()+32,pk.begin());
    auto pm=poly1305_msg(aad,aad_len,ct,ct_len);
    auto expected=poly1305_mac(pk,pm.data(),pm.size());
    uint8_t diff=0; for(size_t i=0;i<16;++i) diff|=(tag[i]^expected[i]);
    if(diff!=0) throw runtime_error("ChaCha20-Poly1305: tag de autenticacao INVALIDA (rejeitado)");
    vector<uint8_t> pt(ct_len);
    if(ct_len){ copy(ct,ct+ct_len,pt.begin()); chacha20_xor(key,nonce,1,pt.data(),ct_len); }
    return pt;
}

/* ══════════════════════════════════════════════════════════════════════════
   SEÇÃO 3 — UTILITÁRIOS DE HEX (para vetores de teste RFC)
   ═══════════════════════════════════════════════════════════════════════════ */
static vector<uint8_t> hex_to_bytes(const string &hex) {
    vector<uint8_t> out; out.reserve(hex.size() / 2);
    for (size_t i = 0; i + 1 < hex.size(); i += 2)
        out.push_back((uint8_t)strtol(hex.substr(i, 2).c_str(), nullptr, 16));
    return out;
}
static string bytes_to_hex(const uint8_t *d, size_t n) {
    ostringstream oss; oss << hex << setfill('0');
    for (size_t i = 0; i < n; ++i) oss << setw(2) << (int)d[i];
    return oss.str();
}
template <size_t N> static string bytes_to_hex(const array<uint8_t, N> &a) { return bytes_to_hex(a.data(), N); }

/* ══════════════════════════════════════════════════════════════════════════
   SEÇÃO 4 — EIXO 1: VALIDAÇÃO RFC E ROBUSTEZ
   ═══════════════════════════════════════════════════════════════════════════ */

/// X25519(k, u) segundo RFC 7748 §5: decodifica escalar (com clamping) e
/// u-coordinate, executa a escada de Montgomery e recodifica o resultado.
static array<uint8_t, 32> x25519_rfc(const array<uint8_t, 32> &k_in, const array<uint8_t, 32> &u_in) {
    CurveCtx ctx; init_curve(ctx, 128); // p = 2^255-19, a24=121666 (Curve25519 real)
    array<uint8_t, 32> k_clamped = k_in;
    k_clamped[0] &= 248; k_clamped[31] &= 127; k_clamped[31] |= 64;
    array<uint8_t, 32> u_masked = u_in;
    u_masked[31] &= 127; // X25519 MUST mask o bit mais significativo do u-coordinate
    mpz_t k, u, res;
    mpz_init(k); mpz_import(k, 32, -1, 1, 0, 0, k_clamped.data());
    mpz_init(u); mpz_import(u, 32, -1, 1, 0, 0, u_masked.data()); mpz_mod(u, u, ctx.p);
    mpz_init(res);
    scalar_mul(res, k, u, ctx);
    array<uint8_t, 32> out{}; size_t w = 0;
    mpz_export(out.data(), &w, -1, 1, 0, 0, res);
    mpz_clears(k, u, res, nullptr);
    free_curve(ctx);
    return out;
}

static void test_rfc_vectors() {
    logger.section("EIXO 1a — VALIDACAO CONTRA VETORES OFICIAIS DE RFC");
    int pass = 0, fail = 0;
    auto check = [&](bool cond, const string &name, const string &detail = "") {
        logger.test_result(cond, name, detail);
        rfc_test_results.push_back({name, cond, detail});
        cond ? ++pass : ++fail;
    };

    // --- RFC 7748 §5.2 — Test Vector 1 (X25519) --------------------------------
    logger.subsection("1.1 — RFC 7748 (X25519 / Curve25519)");
    {
        auto k = hex_to_bytes("a546e36bf0527c9d3b16154b82465edd62144c0ac1fc5a18506a2244ba449ac4");
        auto u = hex_to_bytes("e6db6867583030db3594c1a424b15f7c726624ec26b3353b10a903a6d0ab1c4c");
        const string expected = "c3da55379de9c6908e94ea4df28d084f32eccf03491c71f754b4075577a28552";
        array<uint8_t,32> ka{}, ua{};
        copy(k.begin(), k.begin() + 32, ka.begin());
        copy(u.begin(), u.begin() + 32, ua.begin());
        auto out = x25519_rfc(ka, ua);
        string got = bytes_to_hex(out);
        check(got == expected, "X25519(k,u) == vetor oficial RFC 7748 Test Vector 1", got);
    }

    // --- RFC 8032 §7.1 — TEST 1 (Ed25519, mensagem vazia) ----------------------
    logger.subsection("1.2 — RFC 8032 (Ed25519)");
    {
        auto seed_v = hex_to_bytes("9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60");
        const string expected_pub = "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a";
        const string expected_sig =
            "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b";
        array<uint8_t,32> seed{}; copy(seed_v.begin(), seed_v.begin() + 32, seed.begin());
        auto kp = ed25519_keygen_from_seed(seed);
        string got_pub = bytes_to_hex(kp.pub);
        check(got_pub == expected_pub, "Ed25519 chave publica derivada da seed == RFC 8032 TEST 1", got_pub);

        uint8_t empty_msg = 0;
        auto sig = ed25519_sign(kp, &empty_msg, 0);
        string got_sig = bytes_to_hex(sig);
        check(got_sig == expected_sig, "Ed25519 assinatura(mensagem vazia) == RFC 8032 TEST 1", got_sig);
        check(ed25519_verify(kp.pub, &empty_msg, 0, sig), "Ed25519 verify() aceita a assinatura gerada");
    }

    // --- RFC 4231 Test Case 1 — HMAC-SHA512 (valida o alicerce do HKDF-SHA512) -
    // NOTA: RFC 5869 (HKDF) só fornece vetores para SHA-256/SHA-1. Como o
    // protocolo usa HKDF-SHA512 (RFC 5869 + SHA-512), validamos aqui a
    // primitiva HMAC-SHA512 subjacente (RFC 4231 Test Case 1), que é o único
    // bloco não trivial do HKDF (a etapa de "expand" é apenas HMAC iterado).
    logger.subsection("1.3 — RFC 5869 (HKDF-SHA512) / RFC 4231 (HMAC-SHA512, alicerce)");
    {
        vector<uint8_t> key(20, 0x0b);
        const string msg_str = "Hi There";
        vector<uint8_t> msg(msg_str.begin(), msg_str.end());
        const string expected =
            "87aa7cdea5ef619d4ff0b4241a1d6cb02379f4e2ce4ec2787ad0b30545e17cdedaa833b7d6b8a702038b274eaea3f4e4be9d914eeb61f1702e696c203a126854";
        auto mac = hmac_sha512(key.data(), key.size(), msg.data(), msg.size());
        string got = bytes_to_hex(mac);
        check(got == expected, "HMAC-SHA512(RFC 4231 Test Case 1) == vetor oficial", got);

        // Auto-consistência do HKDF-SHA512 (extract+expand, RFC 5869 §2.2/§2.3):
        // comprimento e determinismo do OKM (não há vetor SHA-512 publicado).
        vector<uint8_t> ikm(22, 0x0b);
        vector<uint8_t> salt = hex_to_bytes("000102030405060708090a0b0c");
        vector<uint8_t> info = hex_to_bytes("f0f1f2f3f4f5f6f7f8f9");
        auto okm1 = hkdf(ikm.data(), ikm.size(), salt.data(), salt.size(), info.data(), info.size(), 42);
        auto okm2 = hkdf(ikm.data(), ikm.size(), salt.data(), salt.size(), info.data(), info.size(), 42);
        check(okm1.size() == 42 && okm1 == okm2, "HKDF-SHA512: determinismo e comprimento de OKM corretos");
    }

    // --- RFC 8439 §2.8.2 — AEAD_CHACHA20_POLY1305 ------------------------------
    logger.subsection("1.4 — RFC 8439 (ChaCha20-Poly1305 AEAD)");
    {
        const string pt_str =
            "Ladies and Gentlemen of the class of '99: If I could offer you only one tip for the future, "
            "sunscreen would be it.";
        vector<uint8_t> pt(pt_str.begin(), pt_str.end());
        auto aad = hex_to_bytes("50515253c0c1c2c3c4c5c6c7");
        auto key_v = hex_to_bytes("808182838485868788898a8b8c8d8e8f909192939495969798999a9b9c9d9e9f");
        auto nonce_v = hex_to_bytes("070000004041424344454647");
        const string expected_ct =
            "d31a8d34648e60db7b86afbc53ef7ec2a4aded51296e08fea9e2b5a736ee62d63dbea45e8ca9671282fafb69da92728b1a71"
            "de0a9e060b2905d6a5b67ecd3b3692ddbd7f2d778b8c9803aee328091b58fab324e4fad675945585808b4831d7bc3ff4def0"
            "8e4b7a9de576d26586cec64b6116";
        const string expected_tag = "1ae10b594f09e26a7e902ecbd0600691";

        ChaChaKey key{}; copy(key_v.begin(), key_v.end(), key.begin());
        ChaChaNonce nonce{}; copy(nonce_v.begin(), nonce_v.end(), nonce.begin());
        auto enc = chacha20poly1305_encrypt(key, nonce, pt.data(), pt.size(), aad.data(), aad.size());
        string got_ct = bytes_to_hex(enc.ciphertext.data(), enc.ciphertext.size());
        string got_tag = bytes_to_hex(enc.tag);
        // expected_tag acima tem 1 hex a mais por formatação de citação; comparar por prefixo de 32 hex (16 bytes).
        check(got_ct == expected_ct, "ChaCha20-Poly1305 ciphertext == vetor oficial RFC 8439 §2.8.2", got_ct.substr(0,32)+"...");
        check(got_tag == expected_tag.substr(0,32), "ChaCha20-Poly1305 tag == vetor oficial RFC 8439 §2.8.2", got_tag);

        auto dec = chacha20poly1305_decrypt(key, nonce, enc.ciphertext.data(), enc.ciphertext.size(), enc.tag, aad.data(), aad.size());
        check(dec.size() == pt.size() && equal(dec.begin(), dec.end(), pt.begin()), "Decrypt(Encrypt(M)) == M (round-trip)");
    }

    logger.info("Resumo RFC: " + to_string(pass) + " passaram, " + to_string(fail) + " falharam.");
    if (fail > 0) throw runtime_error("Falha de conformidade RFC detectada — corretude e pre-requisito.");
}

/// EIXO 1b — Testes negativos: o AEAD DEVE rejeitar qualquer adulteração.
static void test_negative_robustness() {
    logger.section("EIXO 1b — TESTES NEGATIVOS DE ROBUSTEZ (AEAD)");
    int pass = 0, fail = 0;
    auto expect_reject = [&](const string &name, function<void()> attempt) {
        bool rejected = false;
        try { attempt(); } catch (const exception &) { rejected = true; }
        logger.test_result(rejected, name);
        robustness_test_results.push_back({name, rejected, rejected ? "decrypt lancou excecao" : "decrypt NAO rejeitou"});
        rejected ? ++pass : ++fail;
    };

    ChaChaKey key{}; gera_bytes_aleatorios(key.data(), key.size());
    ChaChaNonce nonce{}; gera_bytes_aleatorios(nonce.data(), nonce.size());
    string msg_str = "mensagem de teste do protocolo hibrido IoT";
    vector<uint8_t> pt(msg_str.begin(), msg_str.end());
    vector<uint8_t> aad = {0x01, 0x02, 0x03, 0x04};

    auto base = chacha20poly1305_encrypt(key, nonce, pt.data(), pt.size(), aad.data(), aad.size());

    // (A) Corrupção de 1 bit no ciphertext
    expect_reject("Corrupcao de 1 bit no ciphertext -> decrypt REJEITA", [&] {
        auto ct = base.ciphertext; ct[0] ^= 0x01;
        chacha20poly1305_decrypt(key, nonce, ct.data(), ct.size(), base.tag, aad.data(), aad.size());
    });

    // (B) Corrupção de 1 byte na tag
    expect_reject("Corrupcao de 1 byte na tag -> decrypt REJEITA", [&] {
        auto tag = base.tag; tag[0] ^= 0xFF;
        chacha20poly1305_decrypt(key, nonce, base.ciphertext.data(), base.ciphertext.size(), tag, aad.data(), aad.size());
    });

    // (C) Nonce diferente do usado na cifragem
    expect_reject("Nonce diferente do usado na cifragem -> decrypt REJEITA", [&] {
        auto bad_nonce = nonce; bad_nonce[0] ^= 0x01;
        chacha20poly1305_decrypt(key, bad_nonce, base.ciphertext.data(), base.ciphertext.size(), base.tag, aad.data(), aad.size());
    });

    // (D) AAD alterado
    expect_reject("AAD alterado -> decrypt REJEITA", [&] {
        auto bad_aad = aad; bad_aad[0] ^= 0x01;
        chacha20poly1305_decrypt(key, nonce, base.ciphertext.data(), base.ciphertext.size(), base.tag, bad_aad.data(), bad_aad.size());
    });

    // (E) Assinatura Ed25519 com mensagem adulterada -> verify() deve retornar false
    {
        auto kp = ed25519_keygen();
        string m = "pacote autentico";
        vector<uint8_t> mv(m.begin(), m.end());
        auto sig = ed25519_sign(kp, mv.data(), mv.size());
        vector<uint8_t> mv_bad = mv; mv_bad[0] ^= 0x01;
        bool ok_original = ed25519_verify(kp.pub, mv.data(), mv.size(), sig);
        bool ok_bad = ed25519_verify(kp.pub, mv_bad.data(), mv_bad.size(), sig);
        logger.test_result(ok_original, "Ed25519 verify() aceita mensagem original");
        logger.test_result(!ok_bad, "Ed25519 verify() REJEITA mensagem adulterada");
        robustness_test_results.push_back({"Ed25519 verify() aceita mensagem original", ok_original, ""});
        robustness_test_results.push_back({"Ed25519 verify() REJEITA mensagem adulterada", !ok_bad, ""});
        ok_original ? ++pass : ++fail;
        (!ok_bad) ? ++pass : ++fail;
    }

    logger.info("Resumo robustez: " + to_string(pass) + " passaram, " + to_string(fail) + " falharam.");
    if (fail > 0) throw runtime_error("Falha de robustez detectada (deteccao de adulteracao nao funcionou).");
}

/// Constroi um escalar de 255 bits (bit 254 sempre setado) com exatamente
/// `weight` bits em 1, distribuidos uniformemente — usado para comparar o
/// tempo da escada de Montgomery entre classes de peso de Hamming distintas
/// mantendo o NUMERO DE ITERACOES DO LACO IDENTICO (255) entre as classes.
static void build_scalar_with_weight(mpz_t &k, int weight) {
    mpz_set_ui(k, 0);
    mpz_setbit(k, 254); // garante bit-length == 255 para todas as classes
    int remaining = weight - 1;
    if (remaining > 0) {
        double step = 254.0 / remaining;
        for (int i = 0; i < remaining; ++i) {
            int pos = static_cast<int>(i * step);
            if (pos >= 254) pos = 253;
            mpz_setbit(k, pos);
        }
    }
}

/// EIXO 1c — Analise de timing por peso de Hamming do escalar (RFC-adjacente:
/// avalia a propriedade estrutural constant-time da escada de Montgomery).
/// Exporta as amostras BRUTAS (nao apenas media/desvio) para permitir um
/// boxplot fiel a variabilidade real entre execucoes.
static void test_eixo1c_timing_hamming() {
    logger.section("EIXO 1c — TIMING POR PESO DE HAMMING (resistencia estrutural a canal lateral)");
    CurveCtx ctx; init_curve(ctx, 128); // Curve25519 real (255-bit field)
    mpz_t u; mpz_init_set_ui(u, 9);

    struct HammingClass { int weight; const char *label; };
    static const HammingClass CLASSES[] = {
        {10,  "baixo (~10 bits)"},
        {64,  "medio-baixo (~64 bits)"},
        {192, "medio-alto (~192 bits)"},
        {245, "alto (~245 bits)"},
    };
    const int ITER = 40;

    ofstream f("timing_hamming.csv");
    f << "hamming_weight,classe,sample_us\n";

    logger.print_stats_table_header();
    for (const auto &hc : CLASSES) {
        mpz_t k; mpz_init(k);
        build_scalar_with_weight(k, hc.weight);
        auto run_once = [&] {
            mpz_t result; mpz_init(result);
            scalar_mul(result, k, u, ctx);
            mpz_clear(result);
        };
        auto samples = warmup_and_time(run_once, ITER);
        for (double s : samples)
            f << hc.weight << ",\"" << hc.label << "\"," << fixed << setprecision(3) << s << "\n";
        auto stats = compute_stats(string("Montgomery ladder — peso Hamming ") + hc.label, samples);
        all_results.push_back(stats);
        logger.print_stats_row(stats);
        mpz_clear(k);
    }
    mpz_clear(u);
    free_curve(ctx);
    logger.ok("Amostras brutas exportadas para 'timing_hamming.csv' (uso: boxplot de constant-time estrutural)");
    logger.warning("GMP usa alocacao/tamanhos variaveis — isto avalia a ESTRUTURA constant-time do algoritmo,"
                   " nao constitui prova de constant-time de producao (ver limitacao em resumo_testes.txt).");
}

/* ══════════════════════════════════════════════════════════════════════════
   SEÇÃO 5 — EIXO 2: PRIMITIVAS E ESCALABILIDADE (com warm-up)
   ═══════════════════════════════════════════════════════════════════════════ */

static void test_eixo2_escalabilidade() {
    logger.section("EIXO 2 — PRIMITIVAS E ESCALABILIDADE (GMP 80..256 bits)");
    logger.subsection("2.1 — Escada de Montgomery (warm-up + medicao)");
    static const int LEVELS[] = {80, 112, 128, 192, 224, 256};
    const int ITER = 50;
    logger.print_stats_table_header();
    for (int level : LEVELS) {
        CurveCtx ctx; init_curve(ctx, level);
        mpz_t u; mpz_init_set_ui(u, 9);
        auto run_once = [&] {
            mpz_t k, result; mpz_inits(k, result, nullptr);
            rand_scalar(k, ctx);
            scalar_mul(result, k, u, ctx);
            mpz_clears(k, result, nullptr);
        };
        auto samples = warmup_and_time(run_once, ITER);
        auto stats = compute_stats(ctx.name, samples);
        all_results.push_back(stats);
        logger.print_stats_row(stats);
        mpz_clear(u);
        free_curve(ctx);
    }

    logger.subsection("2.2 — Ed25519 KeyGen / Sign / HKDF isolados (warm-up + medicao)");
    {
        const int ITER = 30;
        auto samples_kg = warmup_and_time([&] { volatile auto kp = ed25519_keygen(); (void)kp; }, ITER);
        all_results.push_back(compute_stats("Ed25519 KeyGen (isolado)", samples_kg));
        logger.print_stats_row(all_results.back());

        auto kp = ed25519_keygen();
        string msg_str = "payload de assinatura para benchmark isolado";
        vector<uint8_t> msg(msg_str.begin(), msg_str.end());
        auto samples_sign = warmup_and_time([&] { volatile auto s = ed25519_sign(kp, msg.data(), msg.size()); (void)s; }, ITER);
        all_results.push_back(compute_stats("Ed25519 Sign (isolado)", samples_sign));
        logger.print_stats_row(all_results.back());

        auto sig = ed25519_sign(kp, msg.data(), msg.size());
        auto samples_verify = warmup_and_time([&] { volatile bool ok = ed25519_verify(kp.pub, msg.data(), msg.size(), sig); (void)ok; }, ITER);
        all_results.push_back(compute_stats("Ed25519 Verify (isolado)", samples_verify));
        logger.print_stats_row(all_results.back());
    }
    {
        const int ITER = 200;
        vector<uint8_t> ikm(32, 0xAB), salt(16, 0xCD), info(8, 0xEF);
        auto samples_hkdf = warmup_and_time([&] {
            volatile auto okm = hkdf(ikm.data(), ikm.size(), salt.data(), salt.size(), info.data(), info.size(), 32);
            (void)okm;
        }, ITER);
        all_results.push_back(compute_stats("HKDF-SHA512 isolado (32B IKM -> 32B OKM)", samples_hkdf));
        logger.print_stats_row(all_results.back());
    }
}

/* ══════════════════════════════════════════════════════════════════════════
   SEÇÃO 6 — EIXO 3: CUSTO AMORTIZADO E THROUGHPUT
   ═══════════════════════════════════════════════════════════════════════════ */

/// Simula um handshake híbrido completo (Ed25519 assina X25519 pub; X25519
/// deriva segredo; HKDF deriva chave de sessão) e retorna a chave de sessão.
static ChaChaKey simulate_handshake(CurveCtx &ctx) {
    // Alice e Bob geram pares X25519 e trocam chaves publicas autenticadas por Ed25519.
    mpz_t u_gen; mpz_init_set_ui(u_gen, 9);
    mpz_t a_priv, b_priv, a_pub, b_pub, shared;
    mpz_inits(a_priv, b_priv, a_pub, b_pub, shared, nullptr);
    rand_scalar(a_priv, ctx); scalar_mul(a_pub, a_priv, u_gen, ctx);
    rand_scalar(b_priv, ctx); scalar_mul(b_pub, b_priv, u_gen, ctx);

    auto kp_alice = ed25519_keygen();
    array<uint8_t,32> pub_bytes{}; size_t w=0; mpz_export(pub_bytes.data(),&w,-1,1,0,0,a_pub);
    auto sig = ed25519_sign(kp_alice, pub_bytes.data(), pub_bytes.size());
    if (!ed25519_verify(kp_alice.pub, pub_bytes.data(), pub_bytes.size(), sig))
        throw runtime_error("Falha de autenticacao no handshake simulado");

    scalar_mul(shared, a_priv, b_pub, ctx); // Alice calcula segredo compartilhado
    array<uint8_t,32> shared_bytes{}; w=0; mpz_export(shared_bytes.data(),&w,-1,1,0,0,shared);

    auto okm = hkdf(shared_bytes.data(), shared_bytes.size(), nullptr, 0, nullptr, 0, 32);
    ChaChaKey session_key{}; copy(okm.begin(), okm.end(), session_key.begin());

    mpz_clears(u_gen, a_priv, b_priv, a_pub, b_pub, shared, nullptr);
    return session_key;
}

/// Executa UMA sessao completa (handshake + N mensagens AEAD) e retorna o
/// tempo total em microssegundos. `handshake_us_out`/`msgs_us_out`, se nao
/// nulos, recebem a decomposicao (handshake isolado vs. N mensagens).
static double run_one_protocol_session(CurveCtx &ctx, const vector<uint8_t> &payload, int N,
                                        double *handshake_us_out = nullptr, double *msgs_us_out = nullptr) {
    auto t0 = Clock::now();
    auto session_key = simulate_handshake(ctx);
    auto t1 = Clock::now();
    double handshake_us = chrono::duration_cast<Us>(t1 - t0).count();

    auto t2 = Clock::now();
    for (int i = 0; i < N; ++i) {
        ChaChaNonce nonce{};
        store32_le(nonce.data(), static_cast<uint32_t>(i)); // contador de mensagem como nonce
        auto enc = chacha20poly1305_encrypt(session_key, nonce, payload.data(), payload.size(), nullptr, 0);
        auto dec = chacha20poly1305_decrypt(session_key, nonce, enc.ciphertext.data(), enc.ciphertext.size(), enc.tag, nullptr, 0);
        (void)dec;
    }
    auto t3 = Clock::now();
    double msgs_us = chrono::duration_cast<Us>(t3 - t2).count();

    if (handshake_us_out) *handshake_us_out = handshake_us;
    if (msgs_us_out) *msgs_us_out = msgs_us;
    return handshake_us + msgs_us;
}

/// EIXO 3.1 — Custo amortizado do "Protocolo Hibrido Completo" (handshake +
/// N mensagens), medido com ITERACOES_INTERNAS repeticoes REAIS por valor de
/// N (nao mais um unico "tiro" com P95/P99 artificialmente igualados a
/// media). Para N=1 (a metrica-chave da dissertacao), usa `iteracoes_internas`
/// completas; para N>1 (curva de amortizacao), usa uma amostra reduzida
/// (min(30, iteracoes_internas)) para manter o tempo de execucao viavel, ja
/// que cada amostra custa N cifragens+decifragens completas.
static void test_eixo3_amortizado_throughput(int iteracoes_internas) {
    logger.section("EIXO 3 — CUSTO AMORTIZADO (SESSAO COMPLETA) E THROUGHPUT AEAD");

    logger.subsection("3.1 — Custo amortizado: handshake unico + N mensagens (1,10,100,1000), "
                       "com " + to_string(iteracoes_internas) + " iteracoes internas (N=1)");
    CurveCtx ctx; init_curve(ctx, 128); // Curve25519 real
    static const int MESSAGE_COUNTS[] = {1, 10, 100, 1000};
    const string payload_str = "telemetria{sensor=A1,valor=23.45,unidade=C}"; // ~43B, tipico de IoT
    vector<uint8_t> payload(payload_str.begin(), payload_str.end());

    cout << "\n  " << left << setw(12) << "N msgs" << setw(10) << "Amostras"
         << setw(18) << "Media(us)" << setw(18) << "P95(us)" << setw(18) << "P99(us)\n";
    cout << "  " << string(86, '-') << "\n";

    ofstream breakdown_f("amortized_breakdown.csv");
    breakdown_f << "n_msgs,trials,handshake_us_mean,msgs_us_mean,total_us_mean,amortized_us_mean\n";

    for (int N : MESSAGE_COUNTS) {
        // N=1 e a metrica-chave (feita da tendencia entre rodadas): usa TODAS
        // as iteracoes internas solicitadas. N>1 usa amostra reduzida (custo
        // cresce linearmente com N mensagens por amostra).
        const int trials = (N == 1) ? max(1, iteracoes_internas) : max(5, min(30, iteracoes_internas));

        // Warm-up: uma sessao completa descartada antes de medir (uma unica vez por N).
        { auto warm_key = simulate_handshake(ctx); (void)warm_key; }

        vector<double> amortized_samples; amortized_samples.reserve(trials);
        double sum_handshake = 0, sum_msgs = 0, sum_total = 0;
        for (int t = 0; t < trials; ++t) {
            double handshake_us = 0, msgs_us = 0;
            double total_us = run_one_protocol_session(ctx, payload, N, &handshake_us, &msgs_us);
            sum_handshake += handshake_us; sum_msgs += msgs_us; sum_total += total_us;
            amortized_samples.push_back(total_us / N);
        }

        BenchStats s = compute_stats("Custo amortizado (N=" + to_string(N) + " msgs)", amortized_samples);
        all_results.push_back(s);

        cout << "  " << left << setw(12) << N << setw(10) << trials << fixed << setprecision(2)
             << setw(18) << s.mean_us << setw(18) << s.p95_us << setw(18) << s.p99_us << "\n";
        breakdown_f << N << "," << trials << "," << fixed << setprecision(3)
                    << (sum_handshake / trials) << "," << (sum_msgs / trials) << ","
                    << (sum_total / trials) << "," << (sum_total / trials / N) << "\n";
    }
    free_curve(ctx);

    logger.subsection("3.2 — Throughput AEAD (ChaCha20-Poly1305), payload 16B..16384B");
    static const size_t PAYLOAD_SIZES[] = {16, 64, 256, 1024, 4096, 16384};
    const int THROUGHPUT_ITER = 200;
    logger.print_stats_table_header();
    ChaChaKey key{}; gera_bytes_aleatorios(key.data(), key.size());
    ChaChaNonce nonce{}; gera_bytes_aleatorios(nonce.data(), nonce.size());

    for (size_t sz : PAYLOAD_SIZES) {
        vector<uint8_t> buf(sz, 0x42);
        auto samples_enc = warmup_and_time([&] {
            volatile auto enc = chacha20poly1305_encrypt(key, nonce, buf.data(), buf.size(), nullptr, 0);
            (void)enc;
        }, THROUGHPUT_ITER);
        string label_enc = "CC20-Poly1305 Enc " + to_string(sz) + " B (throughput)";
        auto stats_enc = compute_stats(label_enc, samples_enc);
        all_results.push_back(stats_enc);
        logger.print_stats_row(stats_enc);

        auto enc_once = chacha20poly1305_encrypt(key, nonce, buf.data(), buf.size(), nullptr, 0);
        auto samples_dec = warmup_and_time([&] {
            volatile auto dec = chacha20poly1305_decrypt(key, nonce, enc_once.ciphertext.data(),
                                                          enc_once.ciphertext.size(), enc_once.tag, nullptr, 0);
            (void)dec;
        }, THROUGHPUT_ITER);
        string label_dec = "CC20-Poly1305 Dec " + to_string(sz) + " B (throughput)";
        auto stats_dec = compute_stats(label_dec, samples_dec);
        all_results.push_back(stats_dec);
        logger.print_stats_row(stats_dec);

        if (stats_enc.mean_us > 0.0) {
            double mb_per_s = (sz / 1e6) / (stats_enc.mean_us / 1e6);
            logger.info("  -> Throughput Enc estimado: " + to_string(mb_per_s) + " MB/s para payload de " + to_string(sz) + " bytes");
        }
        if (stats_dec.mean_us > 0.0) {
            double mb_per_s = (sz / 1e6) / (stats_dec.mean_us / 1e6);
            logger.info("  -> Throughput Dec estimado: " + to_string(mb_per_s) + " MB/s para payload de " + to_string(sz) + " bytes");
        }
    }
}

/* ══════════════════════════════════════════════════════════════════════════
   SEÇÃO 7 — EIXO 4: CONSUMO DE MEMORIA DE SO (getrusage + /proc/self/status)
   ═══════════════════════════════════════════════════════════════════════════ */

static pair<MemUsage, MemUsage> test_eixo4_memoria() {
    logger.section("EIXO 4 — CONSUMO DE MEMORIA DE SO (medicao real, nao teorica)");
    logger.info("Medindo pico de RSS antes/depois de operacoes de grande porte (GMP 256-bit + protocolo completo).");

    auto before = read_mem_usage();
    {
        CurveCtx ctx; init_curve(ctx, 256);
        mpz_t k, u, result; mpz_inits(k, u, result, nullptr);
        mpz_set_ui(u, 9);
        for (int i = 0; i < 20; ++i) { rand_scalar(k, ctx); scalar_mul(result, k, u, ctx); }
        mpz_clears(k, u, result, nullptr);
        free_curve(ctx);
    }
    {
        CurveCtx ctx; init_curve(ctx, 128);
        for (int i = 0; i < 5; ++i) { auto key = simulate_handshake(ctx); (void)key; }
        free_curve(ctx);
    }
    auto after = read_mem_usage();

    cout << "\n  " << left << setw(30) << "Metrica" << setw(15) << "Antes(KB)" << setw(15) << "Depois(KB)\n";
    cout << "  " << string(60, '-') << "\n";
    cout << "  " << left << setw(30) << "getrusage ru_maxrss" << setw(15) << before.maxrss_kb << setw(15) << after.maxrss_kb << "\n";
    cout << "  " << left << setw(30) << "/proc/self/status VmPeak" << setw(15) << before.vmpeak_kb << setw(15) << after.vmpeak_kb << "\n";
    cout << "  " << left << setw(30) << "/proc/self/status VmHWM"  << setw(15) << before.vmhwm_kb  << setw(15) << after.vmhwm_kb  << "\n";

    logger.ok("Consumo de memoria medido via chamadas de sistema POSIX (getrusage/procfs) — sem estimativas teoricas.");
    return {before, after};
}

/// Executa `work` em um processo-filho isolado (fork) e retorna o VmHWM
/// (pico de RSS) exclusivo DESSE filho, lido do seu proprio /proc/self/status.
/// Necessario porque VmHWM/getrusage sao contadores de PICO HISTORICO do
/// processo — medir todos os niveis no mesmo processo faria com que o pico
/// de um nivel maior "escondesse" o de um nivel menor testado depois.
static long measure_isolated_vmhwm_kb(const function<void()> &work) {
    int pipefd[2];
    if (pipe(pipefd) != 0) return -1;
    pid_t pid = fork();
    if (pid == 0) {
        close(pipefd[0]);
        work();
        long v = read_mem_usage().vmhwm_kb;
        ssize_t written = write(pipefd[1], &v, sizeof(v));
        (void)written;
        close(pipefd[1]);
        _exit(0);
    }
    close(pipefd[1]);
    long v = -1;
    ssize_t got = read(pipefd[0], &v, sizeof(v));
    (void)got;
    close(pipefd[0]);
    int status; waitpid(pid, &status, 0);
    return v;
}

/// EIXO 4b — Pico de memoria (VmHWM) por nivel de seguranca simulado (80..256
/// bits), medido em processos-filho isolados (um por nivel). Exporta
/// memory_by_level.csv (uso: grafico memoria vs. tamanho de campo).
static void test_eixo4b_memoria_por_nivel() {
    logger.section("EIXO 4b — CONSUMO DE MEMORIA POR NIVEL DE SEGURANCA (80..256 bits)");
    static const int LEVELS[] = {80, 112, 128, 192, 224, 256};
    const int BATCH = 20;

    ofstream f("memory_by_level.csv");
    f << "sec_bits,vmhwm_kb\n";

    cout << "\n  " << left << setw(20) << "Nivel (bits)" << setw(18) << "VmHWM isolado(KB)\n";
    cout << "  " << string(40, '-') << "\n";

    for (int level : LEVELS) {
        long vmhwm = measure_isolated_vmhwm_kb([level] {
            CurveCtx ctx; init_curve(ctx, level);
            mpz_t k, u, result; mpz_inits(k, u, result, nullptr);
            mpz_set_ui(u, 9);
            for (int i = 0; i < BATCH; ++i) { rand_scalar(k, ctx); scalar_mul(result, k, u, ctx); }
            mpz_clears(k, u, result, nullptr);
            free_curve(ctx);
        });
        f << level << "," << vmhwm << "\n";
        cout << "  " << left << setw(20) << level << setw(18) << vmhwm << "\n";
    }
    logger.ok("Memoria por nivel (isolada via fork) exportada para 'memory_by_level.csv'");
}

/* ══════════════════════════════════════════════════════════════════════════
   SEÇÃO 8 — EXPORTAÇÃO CSV E MAIN
   ═══════════════════════════════════════════════════════════════════════════ */

static void export_csv(const vector<BenchStats> &results, const string &fname = "resultados_benchmark.csv") {
    ofstream f(fname);
    if (!f) { logger.error("Nao foi possivel criar " + fname); return; }
    f << "label,n,mean_us,std_us,min_us,max_us,median_us,p5_us,p95_us,p99_us,vmhwm_kb\n";
    for (const auto &r : results) {
        f << "\"" << r.label << "\"," << r.n << "," << fixed << setprecision(3)
          << r.mean_us << "," << r.std_us << "," << r.min_us << "," << r.max_us << ","
          << r.median_us << "," << r.p5_us << "," << r.p95_us << "," << r.p99_us << "," << r.vmhwm_kb << "\n";
    }
    logger.ok("Resultados exportados para '" + fname + "'");
}

/// Extrai o valor de um argumento de linha de comando no formato `--flag N`
/// ou `--flag=N`. Retorna string vazia se `--flag` nao estiver presente.
static string take_cli_arg(int argc, char **argv, int &i, const string &flag) {
    string a = argv[i];
    if (a.rfind(flag + "=", 0) == 0) return a.substr(flag.size() + 1);
    if (a == flag && i + 1 < argc) return argv[++i];
    return "";
}

/// Le um inteiro positivo interativamente (stdin), com valor padrao caso a
/// linha esteja vazia (usuario apenas pressiona Enter).
static int ask_int_with_default(const string &prompt, int default_value) {
    cout << prompt << " [padrao " << default_value << "]: ";
    string line;
    if (!getline(cin, line) || line.empty()) return default_value;
    try { return max(1, stoi(line)); } catch (...) { return default_value; }
}

/// Largura minima (2) para zero-padding dos nomes run_XX, crescendo conforme
/// a quantidade de macro-rodadas exigir (ex.: 100 rodadas -> "run_001").
static int run_folder_width(int macro_rodadas) {
    int width = 2, v = macro_rodadas;
    while (v >= 10) { v /= 10; ++width; }
    return width;
}

int main(int argc, char **argv) {
    int macro_rodadas = 30;          // MACRO_RODADAS: execucoes externas (uma pasta runs/run_XX cada)
    int iteracoes_internas = 1000;   // ITERACOES_INTERNAS: repeticoes em RAM p/ percentis reais (N=1)
    bool macro_from_cli = false, iters_from_cli = false;

    for (int i = 1; i < argc; ++i) {
        string a = argv[i];
        if (a.rfind("--macro", 0) == 0) {
            string v = take_cli_arg(argc, argv, i, "--macro");
            if (!v.empty()) { macro_rodadas = max(1, stoi(v)); macro_from_cli = true; }
        } else if (a.rfind("--iters", 0) == 0) {
            string v = take_cli_arg(argc, argv, i, "--iters");
            if (!v.empty()) { iteracoes_internas = max(1, stoi(v)); iters_from_cli = true; }
        }
    }
    if (!macro_from_cli) macro_rodadas = ask_int_with_default("MACRO_RODADAS (execucoes externas)", macro_rodadas);
    if (!iters_from_cli) iteracoes_internas = ask_int_with_default("ITERACOES_INTERNAS (repeticoes em RAM p/ percentis reais)", iteracoes_internas);

    cout << "\n==============================================================================\n";
    cout << " SUITE DE BENCHMARK FULL-SYSTEM (qemu-system-arm + Debian ARMHF)\n";
    cout << " EIXO 1: Validacao RFC (8032/7748/5869/8439) + Robustez (testes negativos)\n";
    cout << " EIXO 2: Primitivas e Escalabilidade (GMP 80..256 bits, warm-up)\n";
    cout << " EIXO 3: Custo Amortizado (1/10/100/1000 msgs) e Throughput AEAD\n";
    cout << " EIXO 4: Consumo de Memoria de SO (getrusage + /proc/self/status)\n";
    cout << " MACRO_RODADAS=" << macro_rodadas << "  ITERACOES_INTERNAS=" << iteracoes_internas << "\n";
    cout << "==============================================================================\n";

    init_ed25519();

    const fs::path base_dir = fs::current_path();
    const fs::path runs_dir = base_dir / "runs";
    fs::create_directories(runs_dir);
    const int width = run_folder_width(macro_rodadas);

    int falhas = 0;
    for (int r = 1; r <= macro_rodadas; ++r) {
        ostringstream run_name;
        run_name << "run_" << setfill('0') << setw(width) << r;
        const fs::path run_dir = runs_dir / run_name.str();
        fs::create_directories(run_dir);

        logger.section("MACRO-RODADA " + to_string(r) + "/" + to_string(macro_rodadas) + " -> " + run_dir.string());

        // Reinicia acumuladores globais (evita misturar rodadas anteriores).
        all_results.clear();
        rfc_test_results.clear();
        robustness_test_results.clear();

        fs::current_path(run_dir); // std::filesystem: cada rodada grava isolada em runs/run_XX/
        try {
            test_rfc_vectors();
            test_negative_robustness();
            test_eixo1c_timing_hamming();
            test_eixo2_escalabilidade();
            test_eixo3_amortizado_throughput(iteracoes_internas);
            auto mem_snapshot = test_eixo4_memoria();
            test_eixo4b_memoria_por_nivel();

            logger.section("EXPORTACAO DA RODADA " + to_string(r));
            export_csv(all_results);
            export_test_results_csv(rfc_test_results, "rfc_tests.csv");
            export_test_results_csv(robustness_test_results, "robustness_tests.csv");
            export_memory_csv(mem_snapshot.first, mem_snapshot.second);
            logger.ok("CSVs da rodada exportados em '" + run_dir.string() + "'.");
        } catch (const exception &e) {
            logger.error(string("Excecao na macro-rodada " + to_string(r) + ": ") + e.what());
            ++falhas;
        }
        fs::current_path(base_dir); // volta ao diretorio original antes da proxima rodada
    }

    clear_ed25519();

    logger.section("RESUMO FINAL");
    if (falhas == 0) {
        logger.ok("Suite full-system concluida com sucesso em todas as " + to_string(macro_rodadas) + " macro-rodadas.");
    } else {
        logger.warning(to_string(falhas) + "/" + to_string(macro_rodadas) + " macro-rodada(s) falharam — ver mensagens acima.");
    }
    cout << "\n";
    return falhas == 0 ? 0 : 1;
}

