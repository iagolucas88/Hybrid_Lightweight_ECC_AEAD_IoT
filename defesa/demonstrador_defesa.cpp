#include "defesa_common.hpp"
#include "simulacao_inspecao/inspecao.hpp"
#include "simulacao_mitm/mitm.hpp"
#include "simulacao_timing/timing.hpp"
#include "simulacao_trafego/trafego.hpp"

int main() {
    DefenseLogger logger;
    logger.println("\n============================================================");
    logger.println(" DEMONSTRADOR INTERATIVO - DEFESA DE MESTRADO");
    logger.println(" Ed25519 | X25519 | HKDF-SHA512 | ChaCha20-Poly1305");
    logger.println(" Auditoria desta execucao: ", logger.path());
    logger.println("============================================================");

    bool running = true;
    while (running) {
        logger.println("\nMenu:");
        logger.println("  1 - Simulacao de trafego continuo (custo amortizado)");
        logger.println("  2 - Simulacao de ataque Man-in-the-Middle no AEAD");
        logger.println("  3 - Demonstracao de timing e Conditional Swap");
        logger.println("  4 - Inspecao matematica (deep-dive)");
        logger.println("  0 - Sair");
        logger.print("Escolha: ");

        int option = -1;
        if (!(std::cin >> option)) {
            logger.println("Entrada invalida. Encerrando.");
            return 1;
        }

        try {
            switch (option) {
            case 1:
                executar_simulacao_trafego(logger);
                break;
            case 2:
                executar_simulacao_mitm(logger);
                break;
            case 3:
                executar_simulacao_timing(logger);
                break;
            case 4:
                executar_inspecao_matematica(logger);
                break;
            case 0:
                running = false;
                logger.println("Sessao encerrada. Auditoria preservada em ", logger.path(), ".");
                break;
            default:
                logger.println("Opcao inexistente.");
                break;
            }
        } catch (const std::exception &error) {
            logger.println(ANSI_RED, "[ERRO] ", error.what(), ANSI_RESET);
            logger.println("A demonstracao pode continuar pelo menu.");
        }
    }
    return 0;
}