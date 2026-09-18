#pragma once

#include "../defesa_common.hpp"

inline void executar_simulacao_trafego(DefenseLogger &log) {
    defesa_banner(log, "OPCAO 1 - SIMULACAO DE TRAFEGO CONTINUO");
    int messages = 0;
    log.print("Quantidade de mensagens N: ");
    if (!(std::cin >> messages) || messages <= 0)
        throw std::runtime_error("N deve ser um inteiro positivo");
    std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
    std::string custom_payload;
    log.print("Mensagem a transmitir (ENTER para usar padrao): ");
    std::getline(std::cin, custom_payload);
    const std::string payload = custom_payload.empty()
        ? "telemetria{device=sensor_A1,temp=23.4,humidity=61}" : custom_payload;

    inic_parametros_c25519();
    init_ed25519();
    Ed25519KeyPair identity{};
    mpz_t alice_private, alice_public, bob_private, bob_public, shared_secret;
    mpz_inits(alice_private, alice_public, bob_private, bob_public, shared_secret, nullptr);
    try {
        log.println("\n[HANDSHAKE] Autenticando Alice e negociando chave X25519...");
        defesa_handshake(identity, alice_private, alice_public, bob_private,
                         bob_public, shared_secret, log);
        uint8_t salt[32];
        gera_bytes_aleatorios(salt, sizeof(salt));
        ChaChaKey key = defesa_derivar_chave(shared_secret, salt, sizeof(salt));
        const std::string aad_text = "defesa:device_A:session_v1";

        log.println("\n[TRAFEGO] Mensagem-base: \"", payload, "\"");
        int accepted = 0;
        long long total_send_us = 0;
        long long total_validation_us = 0;
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

            const auto validation_start = Clock::now();
            auto recovered = chacha20poly1305_decrypt(
                key, nonce, encrypted.ciphertext.data(), encrypted.ciphertext.size(),
                encrypted.tag, reinterpret_cast<const uint8_t *>(aad_text.data()),
                aad_text.size());
            const auto validation_us = chrono::duration_cast<Us>(Clock::now() - validation_start).count();
            const Poly1305Tag tag_receptor = defesa_calcular_tag(
                key, nonce, reinterpret_cast<const uint8_t *>(aad_text.data()), aad_text.size(),
                encrypted.ciphertext.data(), encrypted.ciphertext.size());
            total_send_us += send_us;
            total_validation_us += validation_us;
            log.println("  Tag calculada pelo emissor   : ", bytes_to_hex(encrypted.tag));
            log.println("  Tag recalculada pelo receptor: ", bytes_to_hex(tag_receptor));
            log.println("  Verificacao Poly1305         : ",
                        encrypted.tag == tag_receptor ? "IGUAIS" : "DIFERENTES");
            const std::string recovered_text(recovered.begin(), recovered.end());
            const bool equal = recovered_text == payload;
            log.println("  Tempo de Envio (us)         : ", send_us);
            log.println("  Tempo de Validacao (us)    : ", validation_us);
            log.println("  Payload original : ", payload);
            log.println("  Payload recebido : ", recovered_text);
            log.println("  Payload           : ", equal ? "IGUAIS" : "DIFERENTES");
            if (!equal)
                throw std::runtime_error("payload recuperado diferente do original");
            ++accepted;
            log.println(ANSI_GREEN, "  [", index, "/", messages,
                        "] ACEITA: Poly1305 OK; receptor validou.", ANSI_RESET);
        }
        log.println(ANSI_GREEN, "\n[OK] ", accepted, "/", messages,
                    " mensagens aceitas; custo amortizado do handshake demonstrado.", ANSI_RESET);
        const long long total_execution_us = total_send_us + total_validation_us;
        log.println("  Tempo Total de Execucao (us): ", total_execution_us);
        log.println("  Tempo Medio por Mensagem (us): ",
                static_cast<double>(total_execution_us) / messages);
        defesa_limpar(alice_private, alice_public, bob_private, bob_public, shared_secret);
    } catch (...) {
        defesa_limpar(alice_private, alice_public, bob_private, bob_public, shared_secret);
        throw;
    }
}
