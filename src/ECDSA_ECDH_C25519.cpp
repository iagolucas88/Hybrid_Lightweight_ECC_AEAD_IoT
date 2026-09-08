/*============================================================================
  Arquivo  : src/ECDSA_ECDH_C25519.cpp
  Autor    : Iago Lucas (iagolbg@gmail.com | GitHub: iagolucas88)
  Curso    : Mestrado em Engenharia Mecatrônica — UFRN (Brasil)
  Propósito: Implementação de protocolo de criptografia híbrida para IoT.

  PORTABILIDADE: implementação portável em C++17 puro + GMP.
  Compatível com qualquer processador de 32 bits com suporte a C++17:
  ARM Cortex-A9, Cortex-A5, Cortex-A7, MIPS32, RISC-V RV32, x86, etc.
  Sem uso de intrínsecos, extensões de compilador ou ASM específico de ISA.

  PROTOCOLO IMPLEMENTADO (arquivo único, sem dependências externas além de GMP):
    Fase 1 – Handshake/Autenticação : Ed25519   (EdDSA / RFC 8032)
    Fase 2 – Troca de Chaves        : X25519    (ECDH via Escada de Montgomery)
             Derivação de Chave     : HKDF-SHA512 (RFC 5869)
    Fase 3 – Cifra Simétrica AEAD   : ChaCha20-Poly1305 (RFC 8439)

  CURVAS UTILIZADAS (todas sobre o campo GF(2^255 - 19)):
    • Curve25519  — forma de Montgomery,  usado em X25519 (troca de chaves)
    • Edwards25519 — forma de Edwards torcida, base do Ed25519 (assinatura)
    • X25519      — protocolo ECDH sobre Curve25519

  NOTA: Curve25519 e Edwards25519 são formas birracionalmente equivalentes
  da mesma curva elíptica. X25519 é o protocolo DH construído sobre Curve25519.

  BENCHMARKING: todas as fases são cronometradas com <chrono> e exportadas
  em resultados_benchmark.csv para análise de desempenho.
============================================================================*/

/* ─────────────────────────────────────────────────────────────────────────
   INCLUDES
   ───────────────────────────────────────────────────────────────────────── */
#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>
#include <gmp.h>

using namespace std;
using Clock = chrono::steady_clock;
using Us    = chrono::microseconds;

/* ═══════════════════════════════════════════════════════════════════════════
   SEÇÃO 0 – PARÂMETROS GLOBAIS DA CURVE25519 / EDWARDS25519
   ──────────────────────────────────────────────────────────────────────────
   Curve25519 (forma de Montgomery): By² = x³ + Ax² + x  sobre GF(p)
     p   = 2^255 – 19        campo primo de 255 bits
     A   = 486662             coeficiente da curva de Montgomery
     a24 = (A+2)/4 = 121666  constante usada na fórmula de duplo-e-adição
     Gu  = 9                  coordenada u do ponto base
     n   = ordem do grupo de pontos (cofator 8)

   Edwards25519 (forma de Edwards torcida): −x²+y² = 1+d·x²·y²  sobre GF(p)
     d   = −121665/121666 mod p
     Mesmo campo p = 2^255 − 19; birracionalmente equivalente a Curve25519.

   X25519: protocolo ECDH construído sobre Curve25519 via escada de Montgomery.
   ═══════════════════════════════════════════════════════════════════════════ */

mpz_t CURVA_p, CURVA_n, CURVA_a, CURVA_a24;
mpz_t CURVA_Gu;       // coordenada u do ponto base (Gu = 9)
mpz_t CURVA_zero, CURVA_one;

/// Ponto em coordenadas projetivas (X:Z) para a Escada de Montgomery.
/// A coordenada afim é obtida por u = X · Z^{-1} mod p.
struct P_projetivo {
    mpz_t x, z;
};

void initPontoP(P_projetivo &Pp, const mpz_t xp, const mpz_t zp) {
    mpz_init_set(Pp.x, xp); mpz_init_set(Pp.z, zp);
}
void initPontoP(P_projetivo &Pp) {
    mpz_init(Pp.x); mpz_init(Pp.z);
}
void clearPontoP(P_projetivo &Pp) {
    mpz_clear(Pp.x); mpz_clear(Pp.z);
}

/// Inicializa todos os parâmetros fixos da Curve25519 na memória GMP.
void inic_parametros_c25519() {
    mpz_init_set_str(CURVA_p,
        "57896044618658097711785492504343953926634992332820282019728792003956564819949", 10);
    mpz_init_set_str(CURVA_n,
        "7237005577332262213973186563042994240857116359379907606001950938285454250989", 10);
    mpz_init_set_ui(CURVA_a,   486662);
    mpz_init_set_ui(CURVA_a24, 121666);
    mpz_init_set_ui(CURVA_Gu,  9);
    mpz_init_set_ui(CURVA_zero, 0);
    mpz_init_set_ui(CURVA_one,  1);
}

void limpa_parametros_c25519() {
    mpz_clears(CURVA_p, CURVA_n, CURVA_a, CURVA_a24,
               CURVA_Gu, CURVA_zero, CURVA_one, nullptr);
}

/* ═══════════════════════════════════════════════════════════════════════════
   SEÇÃO 1 – UTILITÁRIOS
   ═══════════════════════════════════════════════════════════════════════════ */

/// Converte n bytes para string hexadecimal (diagnóstico verbose).
string bytes_to_hex(const uint8_t *data, size_t len) {
    ostringstream oss;
    oss << hex << setfill('0');
    for (size_t i = 0; i < len; ++i)
        oss << setw(2) << static_cast<int>(data[i]);
    return oss.str();
}
template <size_t N>
string bytes_to_hex(const array<uint8_t, N> &arr) {
    return bytes_to_hex(arr.data(), N);
}
string bytes_to_hex(const vector<uint8_t> &v) {
    return bytes_to_hex(v.data(), v.size());
}

/// Exporta mpz_t como hex de 32 bytes (zero-padded, big-endian).
string mpz_to_hex32(const mpz_t v) {
    array<uint8_t, 32> buf{};
    size_t written = 0;
    mpz_export(buf.data(), &written, 1, 1, 0, 0, v);
    if (written < 32) {
        move(buf.begin(), buf.begin() + written, buf.begin() + (32 - written));
        fill(buf.begin(), buf.begin() + (32 - written), uint8_t(0));
    }
    return bytes_to_hex(buf);
}

/// Geração de bytes aleatórios via /dev/urandom (seguro para QEMU/Debian).
/// Em bare-metal ARM, substitua pela leitura dos registradores do TRNG.
void gera_bytes_aleatorios(uint8_t *out, size_t len) {
    ifstream urandom("/dev/urandom", ios::binary);
    if (!urandom) throw runtime_error("Falha ao abrir /dev/urandom");
    urandom.read(reinterpret_cast<char *>(out), static_cast<streamsize>(len));
    if (!urandom) throw runtime_error("Leitura incompleta de /dev/urandom");
}

/// Gera escalar aleatório k ∈ [2, n-1] ímpar (chave privada X25519).
void gera_escalar_rand(mpz_t &k) {
    uint8_t seed[64];
    gera_bytes_aleatorios(seed, sizeof(seed));
    mpz_t conv_seed; mpz_init(conv_seed);
    mpz_import(conv_seed, sizeof(seed), 1, 1, 0, 0, seed);
    gmp_randstate_t state;
    gmp_randinit_default(state);
    gmp_randseed(state, conv_seed);
    do { mpz_urandomm(k, state, CURVA_n);
    } while (mpz_cmp_ui(k, 1) <= 0 || mpz_even_p(k));
    mpz_clear(conv_seed);
    gmp_randclear(state);
}

/* ═══════════════════════════════════════════════════════════════════════════
   SEÇÃO 2 – ESCADA DE MONTGOMERY (X25519 — Constant-Time)
   Referência: Bernstein, D. J. "Curve25519: New Diffie-Hellman Speed Records"
   PKC 2006, Appendix B.

   Fórmulas (coordenadas projetivas X:Z — sem divisões no loop):
     Duplo  : x₂ = (X+Z)²(X-Z)²         mod p
               z₂ = (F–E)(F + a24·(F–E))  mod p   onde E=(X-Z)², F=(X+Z)²
     Adição : x₃ = z₁·(BC + AD)²         mod p
               z₃ = x₁·(BC - AD)²         mod p   onde A=X+Z, B=X-Z, C=X'+Z', D=X'-Z'

   MITIGAÇÃO DE TIMING ATTACK: swap condicional idêntico antes e depois de cada
   duplo-e-adição garante fluxo uniforme independente do bit do escalar.
   ═══════════════════════════════════════════════════════════════════════════ */

/// Swap condicional constant-time (estrutural com GMP).
/// NOTA: mpz_swap é O(1) (troca ponteiros), mas não oculta acesso de cache.
/// Para constant-time real de produção use limbs estáticos de 64-bit.
void swap_condicional(P_projetivo &R0, P_projetivo &R1, int bit_cond) {
    if (bit_cond) {
        mpz_swap(R0.x, R1.x);
        mpz_swap(R0.z, R1.z);
    }
}

/// Duplo-e-Adição diferencial (Montgomery ladder step).
/// Calcula simultaneamente: R0 ← 2R0  e  R1 ← R0_old + R1_old.
/// Invariante mantida: R1 – R0 = ponto de entrada (ponto diferencial fixo).
/// x1_diff: coordenada u afim do ponto de entrada (diferencial constante).
void double_add_ponto(P_projetivo &R0, P_projetivo &R1, const mpz_t x1_diff) {
    mpz_t A, B, C, D, E, F, tmp;
    mpz_inits(A, B, C, D, E, F, tmp, nullptr);

    // Salva C e D com os valores ORIGINAIS de R1 antes de qualquer modificação
    mpz_add(C, R1.x, R1.z);        // C = X' + Z'
    mpz_sub(D, R1.x, R1.z);        // D = X' - Z'

    mpz_add(A, R0.x, R0.z);        // A = X + Z
    mpz_sub(B, R0.x, R0.z);        // B = X - Z
    mpz_mul(E, B, B);               // E = (X-Z)²
    mpz_mul(F, A, A);               // F = (X+Z)²

    // DUPLO de R0:
    // x₂ = E·F = (X-Z)²·(X+Z)²  mod p
    mpz_mul(R0.x, E, F);
    mpz_mod(R0.x, R0.x, CURVA_p);
    // z₂ = (F–E)·(F + a24·(F–E))  mod p
    mpz_sub(tmp, F, E);             // tmp = F-E
    mpz_mul(R0.z, CURVA_a24, tmp);  // R0.z = a24·(F-E)
    mpz_add(R0.z, F, R0.z);         // R0.z = F + a24·(F-E)
    mpz_mul(R0.z, tmp, R0.z);       // R0.z = (F-E)·(F + a24·(F-E))
    mpz_mod(R0.z, R0.z, CURVA_p);

    // ADIÇÃO R1 = R0_old + R1_old (usa C e D salvos acima):
    mpz_mul(E, B, C);               // E = B·C = (X-Z)(X'+Z')
    mpz_mul(F, A, D);               // F = A·D = (X+Z)(X'-Z')
    // x₃ = (E+F)²
    mpz_add(R1.x, E, F);
    mpz_mul(R1.x, R1.x, R1.x);
    mpz_mod(R1.x, R1.x, CURVA_p);
    // z₃ = x1_diff·(E–F)²  (x₁ = coordenada u do ponto de entrada)
    mpz_sub(R1.z, E, F);
    mpz_mul(R1.z, R1.z, R1.z);
    mpz_mul(R1.z, R1.z, x1_diff);
    mpz_mod(R1.z, R1.z, CURVA_p);

    mpz_clears(A, B, C, D, E, F, tmp, nullptr);
}

/// Converte coordenada projetiva (X:Z) para afim: u = X·Z^{-1} mod p.
void conv_proj_to_afim(mpz_t &u_afim, const P_projetivo &Pp) {
    mpz_t inv; mpz_init(inv);
    if (mpz_invert(inv, Pp.z, CURVA_p) == 0)
        throw runtime_error("conv_proj_to_afim: Z sem inverso (ponto no infinito)");
    mpz_mul(u_afim, Pp.x, inv);
    mpz_mod(u_afim, u_afim, CURVA_p);
    mpz_clear(inv);
}

/// Escada de Montgomery constant-time: coord_x_afim = k · u_entrada  (mod p).
void multiplicacao_escalar(mpz_t &coord_x_afim,
                            const mpz_t &k_rand,
                            const mpz_t &u_entrada) {
    mpz_t k_var; mpz_init_set(k_var, k_rand);

    P_projetivo R0, R1;
    initPontoP(R0, CURVA_one,  CURVA_zero); // R0 = (1:0) = ponto neutro
    initPontoP(R1, u_entrada,  CURVA_one);  // R1 = (u:1) = ponto de entrada

    size_t n_bits = mpz_sizeinbase(k_var, 2);
    for (int i = static_cast<int>(n_bits) - 1; i >= 0; --i) {
        int bit = mpz_tstbit(k_var, static_cast<mp_bitcnt_t>(i));
        swap_condicional(R0, R1, bit);        // PRÉ: alinha R0 ≤ R1 conforme bit
        double_add_ponto(R0, R1, u_entrada);  // 2R0 e R0+R1 simultâneos
        swap_condicional(R0, R1, bit);        // PÓS: restaura ordem original
    }
    conv_proj_to_afim(coord_x_afim, R0);
    clearPontoP(R0); clearPontoP(R1);
    mpz_clear(k_var);
}

/* ═══════════════════════════════════════════════════════════════════════════
   SEÇÃO 3 – SHA-512 PORTÁVEL (FIPS 180-4)
   Sem dependência de OpenSSL. Funciona em ARM Cortex-A9 (AArch32) usando
   pares de registradores de 32-bit para simular operações de 64-bit.
   ═══════════════════════════════════════════════════════════════════════════ */

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

static inline uint64_t rotr64(uint64_t x, int n) noexcept { return (x>>n)|(x<<(64-n)); }

static void sha512_compress(uint64_t st[8], const uint8_t blk[128]) noexcept {
    uint64_t W[80];
    for (int i=0;i<16;++i) { W[i]=0; for(int j=0;j<8;++j) W[i]=(W[i]<<8)|blk[i*8+j]; }
    for (int i=16;i<80;++i) {
        uint64_t s0=rotr64(W[i-15],1)^rotr64(W[i-15],8)^(W[i-15]>>7);
        uint64_t s1=rotr64(W[i-2],19)^rotr64(W[i-2],61)^(W[i-2]>>6);
        W[i]=W[i-16]+s0+W[i-7]+s1;
    }
    uint64_t a=st[0],b=st[1],c=st[2],d=st[3],e=st[4],f=st[5],g=st[6],h=st[7];
    for (int i=0;i<80;++i){
        uint64_t S1=rotr64(e,14)^rotr64(e,18)^rotr64(e,41);
        uint64_t ch=(e&f)^(~e&g);
        uint64_t t1=h+S1+ch+SHA512_K[i]+W[i];
        uint64_t S0=rotr64(a,28)^rotr64(a,34)^rotr64(a,39);
        uint64_t maj=(a&b)^(a&c)^(b&c);
        uint64_t t2=S0+maj;
        h=g;g=f;f=e;e=d+t1;d=c;c=b;b=a;a=t1+t2;
    }
    st[0]+=a;st[1]+=b;st[2]+=c;st[3]+=d;
    st[4]+=e;st[5]+=f;st[6]+=g;st[7]+=h;
}

/// Calcula SHA-512 sobre `data[0..len-1]`. Retorna digest de 64 bytes.
array<uint8_t,64> sha512(const uint8_t *data, size_t len) {
    uint64_t st[8]={0x6a09e667f3bcc908ULL,0xbb67ae8584caa73bULL,
                    0x3c6ef372fe94f82bULL,0xa54ff53a5f1d36f1ULL,
                    0x510e527fade682d1ULL,0x9b05688c2b3e6c1fULL,
                    0x1f83d9abfb41bd6bULL,0x5be0cd19137e2179ULL};
    uint8_t blk[128];
    size_t i=0;
    while (i+128<=len){ sha512_compress(st,data+i); i+=128; }
    size_t rem=len-i;
    memset(blk,0,128); memcpy(blk,data+i,rem); blk[rem]=0x80;
    if (rem>=112){ sha512_compress(st,blk); memset(blk,0,128); }
    uint64_t bits=static_cast<uint64_t>(len)*8;
    for(int j=7;j>=0;--j){ blk[120+j]=bits&0xFF; bits>>=8; }
    sha512_compress(st,blk);
    array<uint8_t,64> out{};
    for(int j=0;j<8;++j)
        for(int k=7;k>=0;--k)
            out[j*8+(7-k)]=static_cast<uint8_t>(st[j]>>(k*8));
    return out;
}

/* ═══════════════════════════════════════════════════════════════════════════
   SEÇÃO 4 – HMAC-SHA512 e HKDF-SHA512 (RFC 5869)
   HKDF-Extract : PRK = HMAC-SHA512(salt, IKM)
   HKDF-Expand  : T(i) = HMAC-SHA512(PRK, T(i-1) || info || counter)
                  OKM = T(1) || T(2) || ... [primeiros okm_len bytes]
   ═══════════════════════════════════════════════════════════════════════════ */

/// HMAC-SHA512 genérico (key de qualquer tamanho, bloco interno de 128 bytes).
array<uint8_t,64> hmac_sha512(const uint8_t *key, size_t key_len,
                               const uint8_t *msg, size_t msg_len) {
    constexpr size_t B=128;
    uint8_t k[B]{};
    if (key_len>B){ auto h=sha512(key,key_len); memcpy(k,h.data(),64); }
    else memcpy(k,key,key_len);
    uint8_t ipad[B], opad[B];
    for(size_t j=0;j<B;++j){ ipad[j]=k[j]^0x36; opad[j]=k[j]^0x5C; }
    vector<uint8_t> inner(B+msg_len);
    memcpy(inner.data(),ipad,B); memcpy(inner.data()+B,msg,msg_len);
    auto h_inner=sha512(inner.data(),inner.size());
    vector<uint8_t> outer(B+64);
    memcpy(outer.data(),opad,B); memcpy(outer.data()+B,h_inner.data(),64);
    return sha512(outer.data(),outer.size());
}

array<uint8_t,64> hkdf_extract(const uint8_t *salt, size_t salt_len,
                                const uint8_t *ikm,  size_t ikm_len) {
    static const uint8_t def_salt[64]{};
    if (!salt||salt_len==0){ salt=def_salt; salt_len=64; }
    return hmac_sha512(salt,salt_len,ikm,ikm_len);
}

vector<uint8_t> hkdf_expand(const array<uint8_t,64> &prk,
                             const uint8_t *info, size_t info_len,
                             size_t okm_len) {
    vector<uint8_t> okm; okm.reserve(okm_len);
    array<uint8_t,64> T{}; size_t T_len=0; uint8_t ctr=1;
    while(okm.size()<okm_len){
        vector<uint8_t> buf(T_len+info_len+1);
        memcpy(buf.data(),T.data(),T_len);
        if(info&&info_len) memcpy(buf.data()+T_len,info,info_len);
        buf[T_len+info_len]=ctr++;
        T=hmac_sha512(prk.data(),64,buf.data(),buf.size()); T_len=64;
        size_t take=min<size_t>(64,okm_len-okm.size());
        okm.insert(okm.end(),T.begin(),T.begin()+take);
    }
    return okm;
}

/// HKDF completo: IKM + salt + info → OKM de `okm_len` bytes.
vector<uint8_t> hkdf(const uint8_t *ikm,  size_t ikm_len,
                     const uint8_t *salt, size_t salt_len,
                     const uint8_t *info, size_t info_len,
                     size_t okm_len) {
    auto prk=hkdf_extract(salt,salt_len,ikm,ikm_len);
    return hkdf_expand(prk,info,info_len,okm_len);
}

/* ═══════════════════════════════════════════════════════════════════════════
   SEÇÃO 5 – Ed25519 (EdDSA sobre Edwards25519 — RFC 8032 §5.1)
   Curva: −x²+y² = 1+d·x²·y²  sobre GF(p)
     d   = −121665/121666 mod p
     B   = (Bx, By) ponto base com ordem l = CURVA_n
   Assinatura determinística (sem nonce aleatório — elimina reuse-nonce attack):
     s   = SHA512(seed)[0..31]  (escalar clampado)
     nonce_prefix = SHA512(seed)[32..63]
     r   = SHA512(nonce_prefix || M) mod l
     R   = r·B  →  R_comprimido (32 bytes)
     k   = SHA512(R || A || M) mod l
     S   = (r + k·s) mod l
     Assinatura = R_comprimido (32 bytes) || S (32 bytes LE)
   ═══════════════════════════════════════════════════════════════════════════ */

static mpz_t ED_p, ED_d, ED_l, ED_Bx, ED_By, ED_I;

/// Ponto Edwards25519 em coordenadas extendidas (X:Y:Z:T).
/// Coordenadas afins: x=X/Z, y=Y/Z, x·y=T/Z.
struct EdPoint {
    mpz_t X,Y,Z,T;
    EdPoint()  { mpz_inits(X,Y,Z,T,nullptr); }
    ~EdPoint() { mpz_clears(X,Y,Z,T,nullptr); }
};

static void ed_set_neutral(EdPoint &P){
    mpz_set_ui(P.X,0);mpz_set_ui(P.Y,1);mpz_set_ui(P.Z,1);mpz_set_ui(P.T,0);
}
static void ed_copy(EdPoint &dst, const EdPoint &src){
    mpz_set(dst.X,src.X);mpz_set(dst.Y,src.Y);
    mpz_set(dst.Z,src.Z);mpz_set(dst.T,src.T);
}

/// Adição de pontos Edwards25519 — fórmula unificada (Hisil et al. 2008 §3).
/// Funciona para P=Q (duplo) e P≠Q (adição), sem casos especiais.
static void ed_add(EdPoint &R, const EdPoint &P, const EdPoint &Q) {
    mpz_t A,B,C,D,E,F,G,H,t1,t2;
    mpz_inits(A,B,C,D,E,F,G,H,t1,t2,nullptr);
    mpz_mul(A,P.X,Q.X); mpz_mod(A,A,ED_p);
    mpz_mul(B,P.Y,Q.Y); mpz_mod(B,B,ED_p);
    mpz_mul(C,P.T,Q.T); mpz_mod(C,C,ED_p);
    mpz_mul(C,C,ED_d);  mpz_mod(C,C,ED_p);   // C = d·T1·T2
    mpz_mul(D,P.Z,Q.Z); mpz_mod(D,D,ED_p);
    mpz_add(t1,P.X,P.Y); mpz_add(t2,Q.X,Q.Y);
    mpz_mul(E,t1,t2); mpz_mod(E,E,ED_p);
    mpz_sub(E,E,A); mpz_sub(E,E,B); mpz_mod(E,E,ED_p);
    mpz_sub(F,D,C); mpz_mod(F,F,ED_p);
    mpz_add(G,D,C); mpz_mod(G,G,ED_p);
    mpz_add(H,B,A); mpz_mod(H,H,ED_p);       // H = B+A  (a=−1 → −a·A = +A)
    mpz_mul(R.X,E,F); mpz_mod(R.X,R.X,ED_p);
    mpz_mul(R.Y,G,H); mpz_mod(R.Y,R.Y,ED_p);
    mpz_mul(R.Z,F,G); mpz_mod(R.Z,R.Z,ED_p);
    mpz_mul(R.T,E,H); mpz_mod(R.T,R.T,ED_p);
    mpz_clears(A,B,C,D,E,F,G,H,t1,t2,nullptr);
}

/// Multiplicação escalar R = k·P (double-and-add).
static void ed_scalar_mul(EdPoint &R, const mpz_t k, const EdPoint &P) {
    ed_set_neutral(R);
    EdPoint addend; ed_copy(addend,P);
    size_t nbits=mpz_sizeinbase(k,2);
    for(size_t i=0;i<nbits;++i){
        if(mpz_tstbit(k,static_cast<mp_bitcnt_t>(i))){
            EdPoint tmp; ed_add(tmp,R,addend); ed_copy(R,tmp);
        }
        EdPoint tmp2; ed_add(tmp2,addend,addend); ed_copy(addend,tmp2);
    }
}

/// Comprime ponto Edwards → 32 bytes (y-coord LE + sinal de x no bit 255).
static array<uint8_t,32> ed_compress(const EdPoint &P) {
    mpz_t Zinv,xa,ya; mpz_inits(Zinv,xa,ya,nullptr);
    mpz_invert(Zinv,P.Z,ED_p);
    mpz_mul(xa,P.X,Zinv); mpz_mod(xa,xa,ED_p);
    mpz_mul(ya,P.Y,Zinv); mpz_mod(ya,ya,ED_p);
    array<uint8_t,32> out{}; size_t w=0;
    mpz_export(out.data(),&w,-1,1,0,0,ya); // little-endian
    if(mpz_odd_p(xa)) out[31]|=0x80;
    mpz_clears(Zinv,xa,ya,nullptr);
    return out;
}

/// Descomprime 32 bytes → EdPoint (extendidas).
static void ed_decompress(EdPoint &P, const array<uint8_t,32> &enc) {
    array<uint8_t,32> bytes=enc;
    int sx=(bytes[31]>>7)&1; bytes[31]&=0x7F;
    mpz_t y,y2,x2,x,exp,num,den,chk;
    mpz_inits(y,y2,x2,x,exp,num,den,chk,nullptr);
    mpz_import(y,32,-1,1,0,0,bytes.data()); // little-endian
    mpz_mul(y2,y,y); mpz_mod(y2,y2,ED_p);
    mpz_sub_ui(num,y2,1); mpz_mod(num,num,ED_p);
    mpz_mul(den,ED_d,y2); mpz_mod(den,den,ED_p);
    mpz_add_ui(den,den,1); mpz_mod(den,den,ED_p);
    mpz_t di; mpz_init(di); mpz_invert(di,den,ED_p);
    mpz_mul(x2,num,di); mpz_mod(x2,x2,ED_p);
    mpz_add_ui(exp,ED_p,3); mpz_cdiv_q_ui(exp,exp,8);
    mpz_powm(x,x2,exp,ED_p);
    mpz_mul(chk,x,x); mpz_mod(chk,chk,ED_p);
    if(mpz_cmp(chk,x2)!=0){ mpz_mul(x,x,ED_I); mpz_mod(x,x,ED_p); }
    int xodd=mpz_odd_p(x)?1:0;
    if(xodd!=sx){ mpz_sub(x,ED_p,x); mpz_mod(x,x,ED_p); }
    mpz_set(P.X,x); mpz_set(P.Y,y);
    mpz_set_ui(P.Z,1);
    mpz_mul(P.T,x,y); mpz_mod(P.T,P.T,ED_p);
    mpz_clears(y,y2,x2,x,exp,num,den,chk,di,nullptr);
}

/// Reduz 64 bytes little-endian para mpz_t módulo l.
static void reduce512_mod_l(mpz_t &r, const array<uint8_t,64> &bytes_le){
    mpz_t v; mpz_init(v);
    mpz_import(v,64,-1,1,0,0,bytes_le.data());
    mpz_mod(r,v,ED_l);
    mpz_clear(v);
}

/// Inicializa parâmetros Edwards25519.
static void init_ed25519() {
    mpz_init_set_str(ED_p,
        "57896044618658097711785492504343953926634992332820282019728792003956564819949",10);
    mpz_init_set_str(ED_l,
        "7237005577332262213973186563042994240857116359379907606001950938285454250989",10);
    // d = −121665/121666 mod p
    mpz_t num,den,di; mpz_init_set_ui(num,121665); mpz_init_set_ui(den,121666); mpz_init(di);
    mpz_invert(di,den,ED_p); mpz_init(ED_d);
    mpz_mul(ED_d,num,di); mpz_mod(ED_d,ED_d,ED_p);
    mpz_sub(ED_d,ED_p,ED_d); mpz_mod(ED_d,ED_d,ED_p); // negação
    mpz_clears(num,den,di,nullptr);
    // I = 2^{(p-1)/4} mod p
    mpz_t e2,base2; mpz_init(e2); mpz_init_set_ui(base2,2);
    mpz_sub_ui(e2,ED_p,1); mpz_cdiv_q_ui(e2,e2,4);
    mpz_init(ED_I); mpz_powm(ED_I,base2,e2,ED_p);
    mpz_clears(e2,base2,nullptr);
    // By = 4·5^{-1} mod p
    mpz_t four,five,finv; mpz_init_set_ui(four,4); mpz_init_set_ui(five,5); mpz_init(finv);
    mpz_invert(finv,five,ED_p); mpz_init(ED_By);
    mpz_mul(ED_By,four,finv); mpz_mod(ED_By,ED_By,ED_p);
    mpz_clears(four,five,finv,nullptr);
    // Bx a partir de By
    mpz_t by2,xn,xd,xdi,x2,exb,chk;
    mpz_inits(by2,xn,xd,xdi,x2,exb,chk,nullptr);
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

static void clear_ed25519(){
    mpz_clears(ED_p,ED_d,ED_l,ED_Bx,ED_By,ED_I,nullptr);
}

struct Ed25519KeyPair {
    array<uint8_t,64> priv_expanded; // SHA512(seed): [0..31]=escalar, [32..63]=nonce_prefix
    array<uint8_t,32> pub;           // ponto comprimido A = s·B
};

/// Gera par de chaves Ed25519 (seed de 32 bytes via /dev/urandom).
Ed25519KeyPair ed25519_keygen() {
    uint8_t seed[32]; gera_bytes_aleatorios(seed,32);
    Ed25519KeyPair kp{};
    auto h=sha512(seed,32);
    copy(h.begin(),h.end(),kp.priv_expanded.begin());
    kp.priv_expanded[0] &=0xF8;
    kp.priv_expanded[31]&=0x3F;
    kp.priv_expanded[31]|=0x40;
    mpz_t s; mpz_init(s);
    array<uint8_t,32> sb; copy(kp.priv_expanded.begin(),kp.priv_expanded.begin()+32,sb.begin());
    mpz_import(s,32,-1,1,0,0,sb.data()); // little-endian
    EdPoint B,A;
    mpz_set(B.X,ED_Bx); mpz_set(B.Y,ED_By); mpz_set_ui(B.Z,1);
    mpz_mul(B.T,ED_Bx,ED_By); mpz_mod(B.T,B.T,ED_p);
    ed_scalar_mul(A,s,B);
    kp.pub=ed_compress(A);
    mpz_clear(s);
    return kp;
}

/// Assina `msg` de `msg_len` bytes. Retorna sig[64] = R(32) || S(32).
array<uint8_t,64> ed25519_sign(const Ed25519KeyPair &kp,
                                const uint8_t *msg, size_t msg_len) {
    // r = SHA512(nonce_prefix || M) mod l
    vector<uint8_t> ni(32+msg_len);
    copy(kp.priv_expanded.begin()+32,kp.priv_expanded.end(),ni.begin());
    copy(msg,msg+msg_len,ni.begin()+32);
    auto rh=sha512(ni.data(),ni.size());
    mpz_t r; mpz_init(r); reduce512_mod_l(r,rh);
    // R = r·B
    EdPoint B,Rpt;
    mpz_set(B.X,ED_Bx); mpz_set(B.Y,ED_By); mpz_set_ui(B.Z,1);
    mpz_mul(B.T,ED_Bx,ED_By); mpz_mod(B.T,B.T,ED_p);
    ed_scalar_mul(Rpt,r,B);
    auto R_bytes=ed_compress(Rpt);
    // k = SHA512(R || A || M) mod l
    vector<uint8_t> ki;
    ki.insert(ki.end(),R_bytes.begin(),R_bytes.end());
    ki.insert(ki.end(),kp.pub.begin(),kp.pub.end());
    ki.insert(ki.end(),msg,msg+msg_len);
    auto kh=sha512(ki.data(),ki.size());
    mpz_t k; mpz_init(k); reduce512_mod_l(k,kh);
    // S = (r + k·s) mod l
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

/// Verifica assinatura Ed25519. Retorna true se válida.
bool ed25519_verify(const array<uint8_t,32> &pub,
                    const uint8_t *msg, size_t msg_len,
                    const array<uint8_t,64> &sig) {
    try {
        array<uint8_t,32> R_bytes, S_bytes;
        copy(sig.begin(),sig.begin()+32,R_bytes.begin());
        copy(sig.begin()+32,sig.end(),S_bytes.begin());
        mpz_t S; mpz_init(S);
        mpz_import(S,32,-1,1,0,0,S_bytes.data());
        if(mpz_cmp(S,ED_l)>=0){ mpz_clear(S); return false; }
        vector<uint8_t> ki;
        ki.insert(ki.end(),R_bytes.begin(),R_bytes.end());
        ki.insert(ki.end(),pub.begin(),pub.end());
        ki.insert(ki.end(),msg,msg+msg_len);
        auto kh=sha512(ki.data(),ki.size());
        mpz_t k; mpz_init(k); reduce512_mod_l(k,kh);
        EdPoint Rpt,Apt,B;
        ed_decompress(Rpt,R_bytes);
        ed_decompress(Apt,pub);
        mpz_set(B.X,ED_Bx); mpz_set(B.Y,ED_By); mpz_set_ui(B.Z,1);
        mpz_mul(B.T,ED_Bx,ED_By); mpz_mod(B.T,B.T,ED_p);
        EdPoint SB,kA,RkA;
        ed_scalar_mul(SB,S,B);
        ed_scalar_mul(kA,k,Apt);
        ed_add(RkA,Rpt,kA);
        auto lhs=ed_compress(SB), rhs=ed_compress(RkA);
        uint8_t diff=0;
        for(size_t i=0;i<32;++i) diff|=(lhs[i]^rhs[i]);
        mpz_clears(S,k,nullptr);
        return diff==0;
    } catch(...){ return false; }
}

/* ═══════════════════════════════════════════════════════════════════════════
   SEÇÃO 6 – ChaCha20-Poly1305 AEAD (RFC 8439)
   ChaCha20: stream cipher ARX, 20 rounds, estado 16×uint32 (512 bits).
   Poly1305: MAC 128-bit sobre GF(2^130-5). Chave derivada do bloco 0.
   Encrypt-then-MAC: verificação da tag ANTES da decifragem (evita oracle).
   ═══════════════════════════════════════════════════════════════════════════ */

using ChaChaKey   = array<uint8_t,32>;
using ChaChaNonce = array<uint8_t,12>;
using Poly1305Tag = array<uint8_t,16>;

struct AeadCiphertext {
    vector<uint8_t> ciphertext;
    ChaChaNonce     nonce;
    Poly1305Tag     tag;
};

static inline uint32_t load32_le(const uint8_t *p) noexcept {
    return uint32_t(p[0])|(uint32_t(p[1])<<8)|(uint32_t(p[2])<<16)|(uint32_t(p[3])<<24);
}
static inline void store32_le(uint8_t *p, uint32_t v) noexcept { p[0]=v;p[1]=v>>8;p[2]=v>>16;p[3]=v>>24; }
static inline uint64_t load64_le(const uint8_t *p) noexcept {
    return uint64_t(load32_le(p))|(uint64_t(load32_le(p+4))<<32);
}
static inline void store64_le(uint8_t *p, uint64_t v) noexcept {
    store32_le(p,uint32_t(v)); store32_le(p+4,uint32_t(v>>32));
}
static inline uint32_t rotl32(uint32_t x,int n) noexcept { return (x<<n)|(x>>(32-n)); }

/// Quarter-round ChaCha20: núcleo ARX de 4 operações de 32 bits.
/// Estrutura Add-Rotate-XOR resiste a cache-timing (sem lookup tables).
static inline void quarter_round(uint32_t &a,uint32_t &b,uint32_t &c,uint32_t &d) noexcept {
    a+=b;d^=a;d=rotl32(d,16);c+=d;b^=c;b=rotl32(b,12);
    a+=b;d^=a;d=rotl32(d, 8);c+=d;b^=c;b=rotl32(b, 7);
}

/// Gera `len` bytes de keystream ChaCha20 e faz XOR com `out`.
/// counter=0 → subchave Poly1305;  counter≥1 → cifragem do payload.
static void chacha20_xor(const ChaChaKey &key, const ChaChaNonce &nonce,
                          uint32_t counter, uint8_t *out, size_t len) noexcept {
    static const uint32_t SIG[4]={0x61707865u,0x3320646eu,0x79622d32u,0x6b206574u};
    uint8_t blk[64]; size_t off=0;
    while(off<len){
        uint32_t st[16];
        st[0]=SIG[0];st[1]=SIG[1];st[2]=SIG[2];st[3]=SIG[3];
        for(int i=0;i<8;++i) st[4+i]=load32_le(key.data()+i*4);
        st[12]=counter;
        st[13]=load32_le(nonce.data());
        st[14]=load32_le(nonce.data()+4);
        st[15]=load32_le(nonce.data()+8);
        uint32_t ws[16]; memcpy(ws,st,64);
        for(int r=0;r<10;++r){
            quarter_round(ws[0],ws[4],ws[ 8],ws[12]);
            quarter_round(ws[1],ws[5],ws[ 9],ws[13]);
            quarter_round(ws[2],ws[6],ws[10],ws[14]);
            quarter_round(ws[3],ws[7],ws[11],ws[15]);
            quarter_round(ws[0],ws[5],ws[10],ws[15]);
            quarter_round(ws[1],ws[6],ws[11],ws[12]);
            quarter_round(ws[2],ws[7],ws[ 8],ws[13]);
            quarter_round(ws[3],ws[4],ws[ 9],ws[14]);
        }
        for(int i=0;i<16;++i) ws[i]+=st[i];
        for(int i=0;i<16;++i) store32_le(blk+i*4,ws[i]);
        size_t ch=min<size_t>(64,len-off);
        for(size_t i=0;i<ch;++i) out[off+i]^=blk[i];
        ++counter; off+=ch;
    }
}

/// Poly1305 MAC sobre `msg[0..len-1]` com chave de 32 bytes (RFC 8439 §2.5).
/// Aritmética modular sobre GF(2^130-5) com limbs de 26 bits.
static Poly1305Tag poly1305_mac(const array<uint8_t,32> &key,
                                  const uint8_t *msg, size_t len) noexcept {
    uint8_t rb[16]; memcpy(rb,key.data(),16);
    rb[3]&=0x0F;rb[7]&=0x0F;rb[11]&=0x0F;rb[15]&=0x0F;
    rb[4]&=0xFC;rb[8]&=0xFC;rb[12]&=0xFC;
    uint64_t s0=load64_le(key.data()+16), s1=load64_le(key.data()+24);
    uint64_t r0=load32_le(rb)    &0x3FFFFFFULL;
    uint64_t r1=(load32_le(rb+3)>>2)&0x3FFFF03ULL;
    uint64_t r2=(load32_le(rb+6)>>4)&0x3FFC0FFULL;
    uint64_t r3=(load32_le(rb+9)>>6)&0x3F03FFFULL;
    uint64_t r4=(load32_le(rb+12)>>8)&0x00FFFFFULL;
    uint64_t h0=0,h1=0,h2=0,h3=0,h4=0;
    auto blk=[&](const uint8_t *b,size_t bl,bool fin){
        uint8_t buf[17]{}; memcpy(buf,b,bl); buf[bl]=fin?0:1;
        uint64_t n0=load64_le(buf),n1=load64_le(buf+8),n2=buf[16];
        h0+=n0&0x3FFFFFFULL; h1+=(n0>>26)&0x3FFFFFFULL;
        h2+=((n0>>52)|(n1<<12))&0x3FFFFFFULL; h3+=(n1>>14)&0x3FFFFFFULL;
        h4+=(n1>>40)|(n2<<24); if(!fin) h4|=(1ULL<<24);
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
    while(off+16<=len){blk(msg+off,16,false);off+=16;}
    if(off<len) blk(msg+off,len-off,true);
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

/// Constrói buffer de autenticação Poly1305: AAD‖pad16‖CT‖pad16‖len(AAD)‖len(CT).
static vector<uint8_t> poly1305_msg(const uint8_t *aad,size_t al,
                                     const uint8_t *ct, size_t cl){
    auto p16=[](size_t n){return (16-(n%16))%16;};
    vector<uint8_t> buf;
    if(al) buf.insert(buf.end(),aad,aad+al);
    buf.resize(buf.size()+p16(al),0);
    if(cl) buf.insert(buf.end(),ct, ct+cl);
    buf.resize(buf.size()+p16(cl), 0);
    uint8_t tmp[8];
    store64_le(tmp,al); buf.insert(buf.end(),tmp,tmp+8);
    store64_le(tmp,cl); buf.insert(buf.end(),tmp,tmp+8);
    return buf;
}

AeadCiphertext chacha20poly1305_encrypt(
    const ChaChaKey &key, const ChaChaNonce &nonce,
    const uint8_t *pt, size_t pt_len,
    const uint8_t *aad, size_t aad_len)
{
    AeadCiphertext res; res.nonce=nonce; res.ciphertext.resize(pt_len);
    // Subchave Poly1305: bloco ChaCha20 com counter=0
    array<uint8_t,64> pb{}; chacha20_xor(key,nonce,0,pb.data(),64);
    array<uint8_t,32> pk; copy(pb.begin(),pb.begin()+32,pk.begin());
    // Cifrar com counter=1
    if(pt_len){ copy(pt,pt+pt_len,res.ciphertext.begin()); chacha20_xor(key,nonce,1,res.ciphertext.data(),pt_len); }
    auto pm=poly1305_msg(aad,aad_len,res.ciphertext.data(),pt_len);
    res.tag=poly1305_mac(pk,pm.data(),pm.size());
    return res;
}

/// Decrypt com verificação de tag Encrypt-then-MAC (lança exceção se inválida).
vector<uint8_t> chacha20poly1305_decrypt(
    const ChaChaKey &key, const ChaChaNonce &nonce,
    const uint8_t *ct, size_t ct_len,
    const Poly1305Tag &tag,
    const uint8_t *aad, size_t aad_len)
{
    array<uint8_t,64> pb{}; chacha20_xor(key,nonce,0,pb.data(),64);
    array<uint8_t,32> pk; copy(pb.begin(),pb.begin()+32,pk.begin());
    auto pm=poly1305_msg(aad,aad_len,ct,ct_len);
    auto expected=poly1305_mac(pk,pm.data(),pm.size());
    uint8_t diff=0;
    for(size_t i=0;i<16;++i) diff|=(tag[i]^expected[i]);
    if(diff!=0) throw runtime_error("ChaCha20-Poly1305: tag de autenticacao INVALIDA!");
    vector<uint8_t> pt(ct_len);
    if(ct_len){ copy(ct,ct+ct_len,pt.begin()); chacha20_xor(key,nonce,1,pt.data(),ct_len); }
    return pt;
}

/* ═══════════════════════════════════════════════════════════════════════════
   SEÇÃO 7 – EXPORTAÇÃO DE MÉTRICAS CSV
   ═══════════════════════════════════════════════════════════════════════════ */

struct Metricas {
    long long keygen_ed25519_us=0, keygen_x25519_us=0;
    long long sign_us=0, verify_us=0, hkdf_us=0;
    long long encrypt_us=0, decrypt_us=0;
    size_t payload_bytes=0;
};

void exportar_metricas_csv(const Metricas &m,
                            const string &fname="resultados_benchmark.csv") {
    ofstream f(fname);
    if(!f){ cerr<<"[AVISO] Nao foi possivel criar "<<fname<<"\n"; return; }
    f<<"etapa,tempo_us,payload_bytes\n"
     <<"Ed25519_KeyGen,"     <<m.keygen_ed25519_us<<","<<m.payload_bytes<<"\n"
     <<"X25519_KeyGen,"      <<m.keygen_x25519_us <<","<<m.payload_bytes<<"\n"
     <<"Ed25519_Sign,"       <<m.sign_us           <<","<<m.payload_bytes<<"\n"
     <<"Ed25519_Verify,"     <<m.verify_us         <<","<<m.payload_bytes<<"\n"
     <<"HKDF_SHA512,"        <<m.hkdf_us           <<","<<m.payload_bytes<<"\n"
     <<"ChaCha20Poly1305_Encrypt,"<<m.encrypt_us   <<","<<m.payload_bytes<<"\n"
     <<"ChaCha20Poly1305_Decrypt,"<<m.decrypt_us   <<","<<m.payload_bytes<<"\n";
    cout<<"\n[CSV] Metricas exportadas -> '"<<fname<<"'\n";
}

/* ═══════════════════════════════════════════════════════════════════════════
   SEÇÃO 8 – MAIN: ROTEIRO DO PROTOCOLO HÍBRIDO
   ═══════════════════════════════════════════════════════════════════════════ */

int main() {
    inic_parametros_c25519();
    init_ed25519();
    Metricas metricas;

    cout<<"\n";
    cout<<"╔══════════════════════════════════════════════════════════════╗\n";
    cout<<"║  Criptografia Hibrida IoT — ARM Cortex-A9 (QEMU/Debian)     ║\n";
    cout<<"║  Ed25519 | X25519 | HKDF-SHA512 | ChaCha20-Poly1305         ║\n";
    cout<<"╚══════════════════════════════════════════════════════════════╝\n\n";

    /* ──────────────────────────────────────────────────────────────────
       FASE 1 — HANDSHAKE & AUTENTICAÇÃO (Ed25519)
       ────────────────────────────────────────────────────────────────── */
    cout<<"══════════════════════════════════════════════════════════════\n";
    cout<<" FASE 1 — HANDSHAKE & AUTENTICACAO (Ed25519 / RFC 8032)\n";
    cout<<"══════════════════════════════════════════════════════════════\n";

    // 1.1 Geração de identidade de Alice (Device A)
    cout<<"\n[1.1] Gerando par de chaves Ed25519 (Alice / Device A)...\n";
    auto t0=Clock::now();
    auto alice_kp=ed25519_keygen();
    auto t1=Clock::now();
    metricas.keygen_ed25519_us=chrono::duration_cast<Us>(t1-t0).count();
    cout<<"  Chave publica Alice : "<<bytes_to_hex(alice_kp.pub)<<"\n";
    cout<<"  Tempo keygen Ed25519: "<<metricas.keygen_ed25519_us<<" us\n";

    // 1.2 Nonce de sessão (Bob gera e envia para Alice assinar)
    uint8_t session_nonce[32];
    gera_bytes_aleatorios(session_nonce,32);
    cout<<"\n[1.2] Nonce de sessao (gerado por Bob): "
        <<bytes_to_hex(session_nonce,32)<<"\n";

    // 1.3 Alice assina o nonce com Ed25519 (prova de identidade)
    cout<<"\n[1.3] Alice assina o nonce (determinístico, sem nonce aleatorio)...\n";
    t0=Clock::now();
    auto sig=ed25519_sign(alice_kp,session_nonce,32);
    t1=Clock::now();
    metricas.sign_us=chrono::duration_cast<Us>(t1-t0).count();
    cout<<"  Assinatura Ed25519 (64 bytes):\n";
    cout<<"    R: "<<bytes_to_hex(sig.data(),32)<<"\n";
    cout<<"    S: "<<bytes_to_hex(sig.data()+32,32)<<"\n";
    cout<<"  Tempo sign Ed25519  : "<<metricas.sign_us<<" us\n";

    // 1.4 Bob verifica a assinatura usando a chave pública de Alice
    cout<<"\n[1.4] Bob verifica a assinatura de Alice...\n";
    t0=Clock::now();
    bool sig_ok=ed25519_verify(alice_kp.pub,session_nonce,32,sig);
    t1=Clock::now();
    metricas.verify_us=chrono::duration_cast<Us>(t1-t0).count();
    if(!sig_ok){
        cerr<<"  [ERRO] Verificacao FALHOU. Encerrando protocolo.\n";
        limpa_parametros_c25519(); clear_ed25519(); return 1;
    }
    cout<<"  Resultado: VALIDA ✓\n";
    cout<<"  Tempo verify Ed25519: "<<metricas.verify_us<<" us\n";

    /* ──────────────────────────────────────────────────────────────────
       FASE 2 — TROCA DE CHAVES (X25519) + HKDF-SHA512
       ────────────────────────────────────────────────────────────────── */
    cout<<"\n══════════════════════════════════════════════════════════════\n";
    cout<<" FASE 2 — TROCA DE CHAVES (X25519) + HKDF-SHA512 (RFC 5869)\n";
    cout<<"══════════════════════════════════════════════════════════════\n";

    // 2.1 Pares efêmeros X25519 (Alice e Bob — Perfect Forward Secrecy)
    cout<<"\n[2.1] Gerando pares X25519 efemeros (Alice e Bob)...\n";
    t0=Clock::now();
    mpz_t alice_priv,alice_pub,bob_priv,bob_pub;
    mpz_inits(alice_priv,alice_pub,bob_priv,bob_pub,nullptr);
    gera_escalar_rand(alice_priv);
    multiplicacao_escalar(alice_pub,alice_priv,CURVA_Gu);
    gera_escalar_rand(bob_priv);
    multiplicacao_escalar(bob_pub,bob_priv,CURVA_Gu);
    t1=Clock::now();
    metricas.keygen_x25519_us=chrono::duration_cast<Us>(t1-t0).count();
    cout<<"  Chave publica Alice (u): "<<mpz_to_hex32(alice_pub)<<"\n";
    cout<<"  Chave publica Bob   (u): "<<mpz_to_hex32(bob_pub)<<"\n";
    cout<<"  Tempo keygen X25519    : "<<metricas.keygen_x25519_us<<" us\n";

    // 2.2 Shared Secret via Escada de Montgomery
    cout<<"\n[2.2] Calculando Shared Secret (Escada de Montgomery constant-time)...\n";
    mpz_t ss_alice,ss_bob;
    mpz_inits(ss_alice,ss_bob,nullptr);
    multiplicacao_escalar(ss_alice,alice_priv,bob_pub);
    multiplicacao_escalar(ss_bob,  bob_priv,  alice_pub);
    if(mpz_cmp(ss_alice,ss_bob)!=0){
        cerr<<"  [ERRO] Shared secrets divergem!\n";
        limpa_parametros_c25519(); clear_ed25519(); return 1;
    }
    cout<<"  Shared Secret (Alice==Bob): "<<mpz_to_hex32(ss_alice)<<"\n";

    // 2.3 HKDF-SHA512: Shared Secret → chave simétrica ChaCha20 (32 bytes)
    cout<<"\n[2.3] HKDF-SHA512: Shared Secret -> chave simetrica de 256 bits...\n";
    array<uint8_t,32> ikm{};
    { size_t w=0; mpz_export(ikm.data(),&w,1,1,0,0,ss_alice);
      if(w<32){ move(ikm.begin(),ikm.begin()+w,ikm.begin()+(32-w));
                fill(ikm.begin(),ikm.begin()+(32-w),uint8_t(0)); } }
    const string info_str="iot_crypto:chacha20:enc_key:v1";
    t0=Clock::now();
    auto okm=hkdf(ikm.data(),32,
                  session_nonce,32,
                  reinterpret_cast<const uint8_t*>(info_str.data()),info_str.size(),
                  32);
    t1=Clock::now();
    metricas.hkdf_us=chrono::duration_cast<Us>(t1-t0).count();
    ChaChaKey enc_key{}; copy(okm.begin(),okm.end(),enc_key.begin());
    cout<<"  Chave derivada (256 bits): "<<bytes_to_hex(enc_key)<<"\n";
    cout<<"  Tempo HKDF-SHA512        : "<<metricas.hkdf_us<<" us\n";

    /* ──────────────────────────────────────────────────────────────────
       FASE 3 — CIFRA SIMÉTRICA AEAD (ChaCha20-Poly1305)
       ────────────────────────────────────────────────────────────────── */
    cout<<"\n══════════════════════════════════════════════════════════════\n";
    cout<<" FASE 3 — CIFRAGEM AEAD (ChaCha20-Poly1305 / RFC 8439)\n";
    cout<<"══════════════════════════════════════════════════════════════\n";

    // Payload de telemetria IoT
    const string payload=
        R"({"device":"sensor_A1","temp":23.4,"humidity":61,"ts":1718000000,"lat":-5.79,"lon":-35.21})";
    metricas.payload_bytes=payload.size();

    ChaChaNonce msg_nonce{};
    gera_bytes_aleatorios(msg_nonce.data(),12);

    const string aad_str="device_id=sensor_A1;proto=hybrid_v1";
    const auto *aad=reinterpret_cast<const uint8_t*>(aad_str.data());

    // 3.1 Cifragem (Alice envia para Bob)
    cout<<"\n[3.1] Cifrando payload ("<<payload.size()<<" bytes)...\n";
    cout<<"  Plaintext : "<<payload<<"\n";
    cout<<"  Nonce (12B): "<<bytes_to_hex(msg_nonce)<<"\n";
    cout<<"  AAD        : "<<aad_str<<"\n";
    t0=Clock::now();
    auto enc=chacha20poly1305_encrypt(
        enc_key,msg_nonce,
        reinterpret_cast<const uint8_t*>(payload.data()),payload.size(),
        aad,aad_str.size());
    t1=Clock::now();
    metricas.encrypt_us=chrono::duration_cast<Us>(t1-t0).count();
    cout<<"  Ciphertext : "<<bytes_to_hex(enc.ciphertext)<<"\n";
    cout<<"  Tag Poly1305 (16B): "<<bytes_to_hex(enc.tag)<<"\n";
    cout<<"  Tempo encrypt: "<<metricas.encrypt_us<<" us\n";

    // 3.2 Decifragem + verificação de tag (Bob recebe)
    cout<<"\n[3.2] Decifrando e verificando tag (Bob)...\n";
    vector<uint8_t> recovered;
    t0=Clock::now();
    try {
        recovered=chacha20poly1305_decrypt(
            enc_key,msg_nonce,
            enc.ciphertext.data(),enc.ciphertext.size(),
            enc.tag,aad,aad_str.size());
    } catch(const exception &ex){
        cerr<<"  [ERRO] "<<ex.what()<<"\n";
        limpa_parametros_c25519(); clear_ed25519(); return 1;
    }
    t1=Clock::now();
    metricas.decrypt_us=chrono::duration_cast<Us>(t1-t0).count();
    string rec(recovered.begin(),recovered.end());
    cout<<"  Plaintext recuperado: "<<rec<<"\n";
    cout<<"  Integridade         : "<<(rec==payload?"OK ✓":"FALHA ✗")<<"\n";
    cout<<"  Tempo decrypt: "<<metricas.decrypt_us<<" us\n";

    /* ──────────────────────────────────────────────────────────────────
       RESUMO DE DESEMPENHO
       ────────────────────────────────────────────────────────────────── */
    cout<<"\n══════════════════════════════════════════════════════════════\n";
    cout<<" RESUMO DE DESEMPENHO — ARM Cortex-A9 (QEMU/Debian)\n";
    cout<<"══════════════════════════════════════════════════════════════\n";
    cout<<fixed<<setprecision(3);
    auto ms=[](long long u){return u/1000.0;};
    cout<<"  Ed25519  KeyGen   : "<<setw(8)<<metricas.keygen_ed25519_us<<" us  ("<<ms(metricas.keygen_ed25519_us)<<" ms)\n";
    cout<<"  X25519   KeyGen   : "<<setw(8)<<metricas.keygen_x25519_us <<" us  ("<<ms(metricas.keygen_x25519_us )<<" ms)\n";
    cout<<"  Ed25519  Sign     : "<<setw(8)<<metricas.sign_us           <<" us  ("<<ms(metricas.sign_us          )<<" ms)\n";
    cout<<"  Ed25519  Verify   : "<<setw(8)<<metricas.verify_us         <<" us  ("<<ms(metricas.verify_us        )<<" ms)\n";
    cout<<"  HKDF-SHA512       : "<<setw(8)<<metricas.hkdf_us           <<" us  ("<<ms(metricas.hkdf_us          )<<" ms)\n";
    cout<<"  ChaCha20 Encrypt  : "<<setw(8)<<metricas.encrypt_us        <<" us  ("<<ms(metricas.encrypt_us       )<<" ms)\n";
    cout<<"  ChaCha20 Decrypt  : "<<setw(8)<<metricas.decrypt_us        <<" us  ("<<ms(metricas.decrypt_us       )<<" ms)\n";
    long long total=metricas.keygen_ed25519_us+metricas.keygen_x25519_us
                   +metricas.sign_us+metricas.verify_us+metricas.hkdf_us
                   +metricas.encrypt_us+metricas.decrypt_us;
    cout<<"  ──────────────────────────────────────────────────────────\n";
    cout<<"  TOTAL             : "<<setw(8)<<total<<" us  ("<<ms(total)<<" ms)\n";
    cout<<"  Payload           : "<<metricas.payload_bytes<<" bytes\n";

    exportar_metricas_csv(metricas);

    /* ──────────────────────────────────────────────────────────────────
       LIMPEZA GMP
       ────────────────────────────────────────────────────────────────── */
    mpz_clears(alice_priv,alice_pub,bob_priv,bob_pub,ss_alice,ss_bob,nullptr);
    limpa_parametros_c25519();
    clear_ed25519();

    cout<<"\n[OK] Protocolo hibrido concluido com sucesso.\n\n";
    return 0;
}
