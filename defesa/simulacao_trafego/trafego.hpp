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
        for (int index = 1; index <= messages; ++index) {
            log.println("\n  -- Mensagem ", index, "/", messages, " --");
            ChaChaNonce nonce{};
            gera_bytes_aleatorios(nonce.data(), nonce.size());
            auto encrypted = chacha20poly1305_encrypt(
                key, nonce, reinterpret_cast<const uint8_t *>(payload.data()),
                payload.size(), reinterpret_cast<const uint8_t *>(aad_text.data()),
                aad_text.size());

            const Poly1305Tag tag_receptor = defesa_calcular_tag(
                key, nonce, reinterpret_cast<const uint8_t *>(aad_text.data()), aad_text.size(),
                encrypted.ciphertext.data(), encrypted.ciphertext.size());
            log.println("  Tag calculada pelo emissor   : ", bytes_to_hex(encrypted.tag));
            log.println("  Tag recalculada pelo receptor: ", bytes_to_hex(tag_receptor));
            const bool tags_match = (encrypted.tag == tag_receptor);
            log.println("  Verificacao Poly1305         : ", tags_match ? "IGUAIS" : "DIFERENTES");
            if (!tags_match)
                throw std::runtime_error("tag divergente em trafego sem ataque (falha inesperada)");

            auto recovered = defesa_decrypt_payload(key, nonce, encrypted.ciphertext);
            const std::string recovered_text(recovered.begin(), recovered.end());
            const bool equal = recovered_text == payload;
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
        defesa_limpar(alice_private, alice_public, bob_private, bob_public, shared_secret);
    } catch (...) {
        defesa_limpar(alice_private, alice_public, bob_private, bob_public, shared_secret);
        throw;
    }
}
