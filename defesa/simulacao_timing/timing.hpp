#pragma once

#include "../defesa_common.hpp"

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
        log.println("\n  Caso     Hamming-weight   Passos ladder   Operacoes (D+A)   Tempo (us)");
        log.println("  ---------------------------------------------------------------------");
        for (std::size_t index = 0; index < scalars.size(); ++index) {
            const auto start = Clock::now();
            multiplicacao_escalar(result, scalars[index], CURVA_Gu);
            const auto elapsed = chrono::duration_cast<Us>(Clock::now() - start).count();
            const int bits = static_cast<int>(mpz_sizeinbase(scalars[index], 2));
            log.println("  ", std::setw(4), index + 1, "     ", std::setw(5),
                        mpz_popcount(scalars[index]), "          ", std::setw(5), bits,
                        "             ", std::setw(5), bits, "          ", elapsed);
        }
        log.println(ANSI_YELLOW,
                    "\n  Nota: cada passo executa um duplo e uma adicao diferencial.", ANSI_RESET);
        log.println(ANSI_YELLOW,
                    "  GMP possui alocacoes/tamanhos variaveis; isto demonstra a estrutura CSWAP,");
        log.println(ANSI_YELLOW,
                    "  mas nao constitui prova de constant-time de producao.", ANSI_RESET);
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
