[![License: MIT](https://img.shields.io/badge/License-MIT-green.svg)](LICENSE)

# Hybrid Lightweight ECC and AEAD Encryption Protocol for IoT

Implementação acadêmica de protocolo de criptografia híbrida com **suite de benchmarking robusto de 4 EIXOS** em C++17 + GMP puro (sem OpenSSL, sem assembly). Desenvolvido para dissertação de mestrado em Engenharia Mecatrônica (UFRN, Brasil).

## Protocolo Implementado

```
Fase 1 — Autenticação  : Ed25519   (EdDSA, RFC 8032)
Fase 2 — Troca de Chave: X25519   (ECDH, RFC 7748)
          Derivação    : HKDF-SHA512 (RFC 5869)
Fase 3 — Cifra AEAD    : ChaCha20-Poly1305 (RFC 8439)
```

**Alvo**: ARM Cortex-A9 emulado via QEMU/Debian Linux

---

## 🎯 Suite de Benchmarking (4 EIXOS)

A suite `benchmarks/benchmark_suite.cpp` implementa avaliação rigorosa em 4 eixos:

### **EIXO 1: Validação Funcional e Segurança (Robustez)**
- ✓ Testes de corretude básica (Montgomery ladder)
- ✓ Testes negativos (adulteração de ciphertext, tag, nonce, AAD)
- ✓ Análise de timing (detect timing leaks via Hamming weight)
- ✓ Aborta programa se validação falhar (requisito pré-teste)

### **EIXO 2: Desempenho Isolado e Memória (Primitivas)**
- ✓ Escalabilidade aritmética GMP (80, 112, 128, 192, 224, 256 bits)
- ✓ Latência isolada: Ed25519 KeyGen/Sign/Verify, HKDF, ChaCha20-Poly1305
- ✓ Pico de memória heap via hooks GMP (`mp_set_memory_functions()`)
- ✓ Warm-up cycles (3 iterações antes de medição)

### **EIXO 3: Protocolo Completo e Custo Amortizado**
- ✓ Latência de sessão criptográfica completa (handshake + 1 msg)
- ✓ Custo amortizado (handshake vs. 1/10/100/1000 mensagens)
- ✓ Throughput AEAD (16 B – 16 KB) em KB/s
- ✓ Impacto de AAD em ChaCha20-Poly1305

### **EIXO 4: Metodologia e Saída**
- ✓ Logger estruturado com tabelas ASCII
- ✓ Exportação CSV robusto (label, n, média, σ, min, máx, P95, P99, heap)
- ✓ Estatísticas: média, desvio padrão, mediana, percentis (P95, P99)
- ✓ `resumo_testes.txt`: documentação completa de cada teste

---

## Estrutura do Repositório

```
├── src/ECDSA_ECDH_C25519.cpp        ← Protocolo híbrido + benchmark embutido
├── benchmarks/
│   ├── benchmark_suite.cpp          ← Suite multi-eixo (EIXO 1–4)
│   └── plot_results.py              ← Gráficos (pandas, matplotlib)
├── resumo_testes.txt                ← Documentação COMPLETA (3500+ linhas)
├── toolchains/armhf-gcc.cmake       ← Toolchain cross-compile ARM
├── CMakeLists.txt
├── run_all.sh                       ← Automação (N rodadas)
└── README.md                        ← Este arquivo
```

---

## Compilar e Executar

### Pré-requisitos

```bash
# Debian / Ubuntu
sudo apt-get install -y build-essential cmake libgmp-dev

# ARM cross-compile (opcional)
sudo apt-get install -y arm-linux-gnueabihf-gcc arm-linux-gnueabihf-g++ \
                       libgmp-dev:armhf qemu-arm-static
```

### Build Nativo

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build

# Protocolo único
./build/protocolo_hibrido

# Suite de benchmarks (4 EIXOS)
./build/benchmark_suite
```

### Build ARM Cross-Compile

```bash
cmake -S . -B build-arm \
  -DCMAKE_TOOLCHAIN_FILE=toolchains/armhf-gcc.cmake \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build-arm

# Executar em QEMU
qemu-arm ./build-arm/benchmark_suite
```

### Automação (N Rodadas)

```bash
./run_all.sh 3  # Executa 3 rodadas, cria benchmark_runs/<timestamp>/
```

Saída: `comparison_summary.csv`, `stability_cv.csv`, gráficos

---

## Resultados Esperados (EIXO 2 — Exemplo)

```
Operação                                n   Média (µs)   σ (µs)   P95 (µs)
────────────────────────────────────────────────────────────────────────
Curve25519-like (80-bit / 160-bit)     50   1366.18     141.23   1480.00
Curve25519-like (112-bit / 224-bit)    50   2185.92     195.47   2310.00
Curve25519 (128-bit / 255-bit) [REAL]  50   3120.22     233.19   3594.00
Curve25519-like (192-bit / 384-bit)    50   7148.67     402.81   7920.00
Curve25519-like (224-bit / 448-bit)    50   10230.15    605.22   11250.00
Curve25519-like (256-bit / 521-bit)    50   14858.90    899.15   16240.00
```

**Crescimento**: ~O(n²) com tamanho de campo (esperado em GMP Karatsuba)

---

## Documentação Completa

Consulte **`resumo_testes.txt`** para:
- Descrição detalhada de cada teste (EIXO 1–4)
- Metodologia precisa de coleta
- Interpretação de resultados
- Limitações e considerações
- Referências às RFCs (7748, 8032, 5869, 8439)
- Análise acadêmica de escalabilidade

---

## Análise de Dados

### Exportação CSV

```bash
./build/benchmark_suite  # Gera 'benchmark_results.csv'
```

Colunas:
- `label`: Nome do teste
- `n`: Número de amostras
- `mean_us`, `std_us`: Média e desvio padrão (µs)
- `min_us`, `max_us`: Mínimo e máximo
- `median_us`, `p95_us`, `p99_us`: Percentis
- `peak_heap_bytes`: Pico de memória heap (GMP)

### Gráficos e Plots

```bash
python3 benchmarks/plot_results.py
# Gera PNG em plots/
```

---

## Níveis de Segurança Testados

| Segurança | Campo | Primo de Referência | Observação |
|-----------|-------|---------------------|------------|
| 80-bit    | 160b  | ≈ 2^160 | Extrapolação |
| 112-bit   | 224b  | NIST P-224 | Extrapolação |
| **128-bit** | **255b** | **2^255 − 19** | ✓ Curve25519 real |
| 192-bit   | 384b  | NIST P-384 | Extrapolação |
| 224-bit   | 448b  | Goldilocks | Extrapolação |
| 256-bit   | 521b  | NIST P-521 | Extrapolação |

> **Nota**: Níveis ≠ 128-bit medem custo acadêmico da aritmética GMP,
> não curvas cryptographically-validated. Serve para análise de escalabilidade.

---

## Arquivos Principais

| Arquivo | Propósito |
|---------|-----------|
| `src/ECDSA_ECDH_C25519.cpp` | Protocolo completo + benchmark simples |
| `benchmarks/benchmark_suite.cpp` | Suite robusto (4 EIXOS, 50–500 iter) |
| `benchmarks/plot_results.py` | Gráficos automatizados (Matplotlib) |
| `resumo_testes.txt` | Documentação completa (3500+ linhas) |
| `run_all.sh` | Automação de múltiplas rodadas |
| `toolchains/armhf-gcc.cmake` | Cross-compile para ARM |

---

## Limitações Conhecidas

⚠️  **QEMU vs. Hardware Real**
- Tempos absolutos em µs NÃO são válidos para hardware
- Comparações relativas (A vs. B) são válidas
- Usar para validar funcionamento, não para estimar latência de produção

⚠️  **GMP vs. Aritmética Especializada**
- GMP não oferece constant-time real (timing leak detectável)
- Speedup esperado: 3–10× com limbs fixos + redução especializada
- Adequado para pesquisa, não para produção crítica

⚠️  **Falta de Vetores RFC**
- Testes negativos inclusos, mas não vetores oficiais das RFCs
- Para validação criptográfica: adicionar RFC 8032/7748/8439 vectors

---

## Interpretação de Resultados

**EIXO 1 — Robustez**:
Se algum teste falhar, programa aborta antes de benchmarks

**EIXO 2 — Escalabilidade**:
Crescimento O(n²) esperado (Karatsuba em GMP)

**EIXO 3 — Amortização**:
Handshake é 1500× mais custoso que 1 mensagem

**EIXO 4 — Saída**:
Todos os dados em CSV, pronto para análise em Python/R/Excel

---

## Referências

- [RFC 7748](https://tools.ietf.org/html/rfc7748) — Elliptic Curves for Security
- [RFC 8032](https://tools.ietf.org/html/rfc8032) — EdDSA
- [RFC 5869](https://tools.ietf.org/html/rfc5869) — HKDF
- [RFC 8439](https://tools.ietf.org/html/rfc8439) — ChaCha20-Poly1305
- [Bernstein 2006](https://cr.yp.to/papers/cv.pdf) — Curve25519
- [GMP Manual](https://gmplib.org/manual/) — GNU MP

---

## Licença

MIT — ver [LICENSE](LICENSE)
├── toolchains/
└── ECDSA_ECDH_C25519.cpp
```

The legacy prototype remains in the repository root as a reference while the modular layout is introduced.

## Build

Native build:

```bash
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

Cross-compile for ARM hard-float:

```bash
cmake -S . -B build-arm \
  -DCMAKE_TOOLCHAIN_FILE=toolchains/armhf-gcc.cmake \
  -DIOT_CRYPTO_BUILD_BENCHMARKS=OFF
cmake --build build-arm
ctest --test-dir build-arm --output-on-failure
```

## Continuous Integration

The GitHub Actions workflow installs the ARM cross-toolchain, ARM GMP package, and QEMU user emulation. Tests are built as ARM binaries and executed on the GitHub-hosted x86 runner through `qemu-user-static`.

## Next Engineering Steps

1. Move reusable arithmetic from the legacy prototype into the modular source files.
2. Replace placeholder APIs with complete domain parameters, point representations, and protocol logic.
3. Expand tests to cover key generation, scalar multiplication edge cases, and serialization.
4. Add benchmark cases for scalar multiplication and signature verification paths.

## License

This project is released under the MIT License. If you want a copyleft distribution model later, the repository can be switched to GPLv3 with corresponding notice updates.
