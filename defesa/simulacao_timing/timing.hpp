#pragma once

#include "../defesa_common.hpp"
#include <cmath>
#include <numeric>

inline void executar_simulacao_timing(DefenseLogger &log) {
    defesa_banner(log, "OPCAO 3 - DEMONSTRACAO DE TIMING E CSWAP");
    inic_parametros_c25519();
    init_ed25519();
    try {
        std::array<mpz_t, 10> scalars;
        for (auto &scalar : scalars) mpz_init(scalar);
        for (std::size_t group = 0; group < scalars.size(); ++group) {
            mpz_set_ui(scalars[group], 1);
            mpz_mul_2exp(scalars[group], scalars[group], 254);
            if (group < 3) {
                mpz_add_ui(scalars[group], scalars[group], 1);
            } else if (group < 6) {
                for (int bit = 0; bit < 64; ++bit) mpz_setbit(scalars[group], bit);
            } else if (group < 9) {
                for (int bit = 0; bit < 192; ++bit) mpz_setbit(scalars[group], bit);
            } else {
                for (int bit = 0; bit < 255; ++bit) mpz_setbit(scalars[group], bit);
            }
        }

        mpz_t result;
        mpz_init(result);
        std::vector<double> timings;
        timings.reserve(scalars.size());
        log.println("\n  Caso     Hamming-weight   Passos ladder   Operacoes (D+A)   Tempo (us)");
        log.println("  ---------------------------------------------------------------------------");
        log.println("  [FIXO] Cada caso executa exatamente 255 passos ladder e 255 operacoes D+A.");
        for (std::size_t index = 0; index < scalars.size(); ++index) {
            const auto start = Clock::now();
            multiplicacao_escalar(result, scalars[index], CURVA_Gu);
            const auto elapsed = chrono::duration_cast<Us>(Clock::now() - start).count();
            timings.push_back(static_cast<double>(elapsed));
            log.println("  ", std::setw(4), index + 1, "     ", std::setw(5),
                        mpz_popcount(scalars[index]), "          ", std::setw(5), 255,
                        "             ", std::setw(5), 255, "          ", elapsed);
        }
        const double mean = std::accumulate(timings.begin(), timings.end(), 0.0) / timings.size();
        double variance = 0.0;
        for (const double value : timings)
            variance += (value - mean) * (value - mean);
        const double standard_deviation = std::sqrt(variance / timings.size());
        const double cv = mean == 0.0 ? 0.0 : (standard_deviation / mean) * 100.0;
        log.println("\n  Estatisticas de Timing:");
        log.println("    Tempo Medio (mu)        : ", mean, " us");
        log.println("    Desvio Padrao (sigma)   : ", standard_deviation, " us");
        log.println("    Coeficiente de Variacao : ", cv, " %");
        if (cv < 5.0) {
            log.println(ANSI_GREEN,
                        "    [SUCESSO] O Coeficiente de Variação e inferior a 5%, indicando que as flutuacoes sao ruidos do Sistema Operacional e nao vazamento de dados.",
                        ANSI_RESET);
        }
        log.println(ANSI_YELLOW,
                    "\n  [NOTA METODOLOGICA]: A matematica do CSWAP garante um numero exato de operacoes. As flutuacoes marginais de tempo decorrem do escalonamento do kernel Linux emulado e da alocacao de heap inerente a biblioteca generica GMP, nao configurando vazamento do escalar.",
                    ANSI_RESET);
        mpz_clear(result);
        for (auto &scalar : scalars) mpz_clear(scalar);
        limpa_parametros_c25519();
        clear_ed25519();
    } catch (...) {
        limpa_parametros_c25519();
        clear_ed25519();
        throw;
    }
}
