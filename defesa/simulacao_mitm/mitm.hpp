#pragma once

#include "../defesa_common.hpp"

inline void executar_simulacao_mitm(DefenseLogger &log) {
    defesa_banner(log, "OPCAO 2 - SIMULACAO DE ATAQUE MAN-IN-THE-MIDDLE");
    int messages = 0;
    std::string corrupted_text;
    std::string attack_kind;
    log.print("Quantidade de mensagens N: ");
    if (!(std::cin >> messages) || messages <= 0)
        throw std::runtime_error("N deve ser um inteiro positivo");
    log.print("Indices a corromper (exemplo 13,19; 0 = nenhum): ");
    std::cin >> corrupted_text;
    std::set<int> corrupted;
    if (corrupted_text != "0" && corrupted_text != "-")
        corrupted = defesa_ler_indices(corrupted_text);
    log.print("Corromper Ciphertext ou Tag nesses indices? (C/T) [C]: ");
    std::cin >> attack_kind;
    const bool attack_tag = !attack_kind.empty() && (attack_kind[0] == 'T' || attack_kind[0] == 't');
    std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
    std::string custom_payload;
    log.print("Mensagem a transmitir (ENTER para usar padrao): ");
    std::getline(std::cin, custom_payload);
    const std::string payload = custom_payload.empty()
        ? "telemetria{device=sensor_A1,temp=23.4}" : custom_payload;

    inic_parametros_c25519();
    init_ed25519();
    Ed25519KeyPair identity{};
    mpz_t alice_private, alice_public, bob_private, bob_public, shared_secret;
    mpz_inits(alice_private, alice_public, bob_private, bob_public, shared_secret, nullptr);
    try {
        log.println("\n[HANDSHAKE] Canal autenticado estabelecido.");
        defesa_handshake(identity, alice_private, alice_public, bob_private,
                         bob_public, shared_secret, log);
        uint8_t salt[32];
        gera_bytes_aleatorios(salt, sizeof(salt));
        ChaChaKey key = defesa_derivar_chave(shared_secret, salt, sizeof(salt));
        const std::string aad_text = "defesa:device_A:session_v1";
        int accepted = 0;
        int rejected = 0;
        long long total_send_us = 0;
        long long total_validation_us = 0;

        log.println("\n[TRAFEGO] Mensagem-base: \"", payload, "\" | Ataque nos indices: ",
                    corrupted.empty() ? "nenhum" : (attack_tag ? "TAG" : "CIPHERTEXT"));
        for (int index = 1; index <= messages; ++index) {
            log.println("\n  -- Mensagem ", index, "/", messages, " --");
            ChaChaNonce nonce{};
            gera_bytes_aleatorios(nonce.data(), nonce.size());
            const auto send_start = Clock::now();
            auto encrypted = chacha20poly1305_encrypt(
                key, nonce, reinterpret_cast<const uint8_t *>(payload.data()),
                payload.size(), reinterpret_cast<const uint8_t *>(aad_text.data()),
                aad_text.size());
            const auto send_us = chrono::duration_cast<Us>(Clock::now() - send_start).count();

            std::vector<uint8_t> received_ciphertext = encrypted.ciphertext;
            Poly1305Tag received_tag = encrypted.tag;
            const bool attacked = corrupted.count(index) != 0;
            if (attacked && attack_tag) {
                received_tag[0] ^= 0x01;
                log.println(ANSI_YELLOW, "  [MITM] 1 bit da TAG alterado em transito.", ANSI_RESET);
            } else if (attacked) {
                received_ciphertext[0] ^= 0x01;
                log.println(ANSI_YELLOW, "  [MITM] 1 bit do CIPHERTEXT alterado em transito.", ANSI_RESET);
            }

            log.println("  Ciphertext enviado  : ", bytes_to_hex(encrypted.ciphertext));
            log.println("  Ciphertext recebido : ", bytes_to_hex(received_ciphertext));
            log.println("  Ciphertext          : ",
                        (received_ciphertext == encrypted.ciphertext) ? "IGUAIS" : "DIFERENTES");

            bool tags_match = false;
            std::vector<uint8_t> recovered;
            const auto validation_start = Clock::now();
            try {
                recovered = chacha20poly1305_decrypt(
                    key, nonce, received_ciphertext.data(), received_ciphertext.size(),
                    received_tag, reinterpret_cast<const uint8_t *>(aad_text.data()),
                    aad_text.size());
                tags_match = true;
            } catch (const std::exception &) {
                tags_match = false;
            }
            const auto validation_us = chrono::duration_cast<Us>(Clock::now() - validation_start).count();
            const Poly1305Tag tag_receptor = defesa_calcular_tag(
                key, nonce, reinterpret_cast<const uint8_t *>(aad_text.data()), aad_text.size(),
                received_ciphertext.data(), received_ciphertext.size());
            total_send_us += send_us;
            total_validation_us += validation_us;
            log.println("  Tempo de Envio (us)         : ", send_us);
            log.println("  Tempo de Validacao (us)    : ", validation_us);
            log.println("  Tag enviada pelo emissor     : ", bytes_to_hex(received_tag));
            log.println("  Tag recalculada pelo receptor: ", bytes_to_hex(tag_receptor));
            log.println("  Verificacao Poly1305         : ", tags_match ? "VALIDA" : "INVALIDA");

            if (tags_match) {
                const std::string recovered_text(recovered.begin(), recovered.end());
                const bool equal = recovered_text == payload;
                log.println("  Payload original : ", payload);
                log.println("  Payload recebido : ", recovered_text);
                log.println("  Payload           : ", equal ? "IGUAIS" : "DIFERENTES");
                ++accepted;
                log.println(ANSI_GREEN, "  [", index, "/", messages,
                            "] ACEITA: tag Poly1305 valida.", ANSI_RESET);
            } else {
                ++rejected;
                log.println("  Payload           : NAO DECIFRADO (tag invalida; decifragem abortada)");
                log.println(ANSI_RED, "  [ALERTA] Mensagem ", index,
                            " REJEITADA: Falha de Integridade Poly1305!", ANSI_RESET);
            }
        }
        const bool all_attacks_rejected = !corrupted.empty() &&
                                          rejected == static_cast<int>(corrupted.size());
        log.println((corrupted.empty() || all_attacks_rejected) ? ANSI_GREEN : ANSI_RED,
                    "\n[RESULTADO] Aceitas: ", accepted, " | Rejeitadas: ", rejected,
                    " | Ataques inseridos: ", corrupted.size(), " | Todos detectados: ",
                    (corrupted.empty() ? "N/A" : (all_attacks_rejected ? "SIM" : "NAO")),
                    ANSI_RESET);
                log.println("  Tempo Total de Execucao (us): ", total_send_us + total_validation_us);
                log.println("  Tempo Medio por Mensagem (us): ",
                        static_cast<double>(total_send_us + total_validation_us) / messages);
        defesa_limpar(alice_private, alice_public, bob_private, bob_public, shared_secret);
    } catch (...) {
        defesa_limpar(alice_private, alice_public, bob_private, bob_public, shared_secret);
        throw;
    }
}
