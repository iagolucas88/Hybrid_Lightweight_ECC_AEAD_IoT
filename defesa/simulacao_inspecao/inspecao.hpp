#pragma once

#include "../defesa_common.hpp"
#include <bitset>

inline std::string payload_demonstracao(std::size_t size) {
    const std::string prefix = "telemetria{device=sensor_A1,data=";
    std::string payload;
    payload.reserve(size);
    for (std::size_t index = 0; index < size; ++index)
        payload.push_back(prefix[index % prefix.size()]);
    return payload;
}

inline ChaChaNonce nonce_pacote_sequencial(const ChaChaNonce &base_nonce,
                                           uint32_t packet_counter) {
    ChaChaNonce packet_nonce = base_nonce;
    for (int index = 11; index >= 0 && packet_counter != 0; --index) {
        const uint32_t sum = static_cast<uint32_t>(packet_nonce[index]) +
                             (packet_counter & 0xffu);
        packet_nonce[index] = static_cast<uint8_t>(sum & 0xffu);
        packet_counter = (packet_counter >> 8u) + (sum >> 8u);
    }
    return packet_nonce;
}

inline std::array<uint8_t, 64> imprimir_bloco_chacha(const ChaChaKey &key,
                                                      const ChaChaNonce &nonce,
                                                      uint32_t packet_counter,
                                                      uint32_t block_counter,
                                                      DefenseLogger &log) {
    uint32_t input_state[16];
    const uint32_t constants[4] = {0x61707865u, 0x3320646eu, 0x79622d32u, 0x6b206574u};
    for (int index = 0; index < 4; ++index) input_state[index] = constants[index];
    for (int index = 0; index < 8; ++index)
        input_state[4 + index] = load32_le(key.data() + index * 4);
    input_state[12] = block_counter;
    input_state[13] = load32_le(nonce.data());
    input_state[14] = load32_le(nonce.data() + 4);
    input_state[15] = load32_le(nonce.data() + 8);

    const auto keystream = defesa_keystream_bloco(key, nonce, block_counter);
    uint32_t output_state[16];
    for (int index = 0; index < 16; ++index)
        output_state[index] = load32_le(keystream.data() + index * 4);

    auto print_matrix = [&](const char *title, const uint32_t *matrix) {
        log.println("    ", title);
        for (int row = 0; row < 4; ++row) {
            log.print("      ");
            for (int column = 0; column < 4; ++column)
                log.print(std::hex, std::setw(8), std::setfill('0'),
                          matrix[row * 4 + column], " ");
            log.println(std::dec, std::setfill(' '));
        }
    };

    const char *purpose = block_counter == 0
        ? "subchave de autenticacao Poly1305 (uso unico)"
        : "keystream do payload";
    log.println("    Bloco ChaCha20 = ", block_counter, " | ", purpose);
    print_matrix("Estado de entrada (antes das 20 rounds):", input_state);
    print_matrix("Estado de saida / keystream:", output_state);
    return keystream;
}

inline void executar_inspecao_matematica(DefenseLogger &log) {
    defesa_banner(log, "OPCAO 4 - INSPECAO MATEMATICA");
    unsigned int last_counter = 0;
    log.print("Mostrar pacotes N de 0 ate (0..32): ");
    if (!(std::cin >> last_counter) || last_counter > 32)
        throw std::runtime_error("contador final deve estar entre 0 e 32");
    std::size_t payload_size = 0;
    log.print("Tamanho do payload da mensagem em bytes: ");
    if (!(std::cin >> payload_size) || payload_size == 0 || payload_size > 65536)
        throw std::runtime_error("payload deve estar entre 1 e 65536 bytes");
    const std::size_t payload_blocks = (payload_size + 63) / 64;
    const std::size_t total_blocks = 1 + payload_blocks;
    std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
    std::string custom_message;
    log.print("Mensagem a assinar com Ed25519 (ENTER para usar padrao): ");
    std::getline(std::cin, custom_message);
    const std::string message = custom_message.empty() ? "mensagem para a banca" : custom_message;

    inic_parametros_c25519();
    init_ed25519();
    try {
        Ed25519KeyPair identity = ed25519_keygen();
        auto signature = ed25519_sign(identity,
                                      reinterpret_cast<const uint8_t *>(message.data()),
                                      message.size());
        const bool signature_valid = ed25519_verify(
            identity.pub, reinterpret_cast<const uint8_t *>(message.data()),
            message.size(), signature);

        EdPoint signature_point;
        ed_decompress(signature_point, [&]() {
            std::array<uint8_t, 32> encoded{};
            std::copy(signature.begin(), signature.begin() + 32, encoded.begin());
            return encoded;
        }());
        mpz_t x_affine, y_affine;
        mpz_inits(x_affine, y_affine, nullptr);
        mpz_t inverse;
        mpz_init(inverse);
        mpz_invert(inverse, signature_point.Z, ED_p);
        mpz_mul(x_affine, signature_point.X, inverse);
        mpz_mod(x_affine, x_affine, ED_p);
        mpz_mul(y_affine, signature_point.Y, inverse);
        mpz_mod(y_affine, y_affine, ED_p);

        uint8_t salt[32];
        gera_bytes_aleatorios(salt, sizeof(salt));
        Ed25519KeyPair handshake_identity{};
        mpz_t alice_private, alice_public, bob_private, bob_public, shared_secret;
        mpz_inits(alice_private, alice_public, bob_private, bob_public,
              shared_secret, nullptr);
        defesa_handshake(handshake_identity, alice_private, alice_public,
                 bob_private, bob_public, shared_secret, log);
        ChaChaKey key = defesa_derivar_chave(shared_secret, salt, sizeof(salt));
        ChaChaNonce nonce{};
        gera_bytes_aleatorios(nonce.data(), nonce.size());
        const std::string aad = "defesa:device_A:session_v1";
        const std::string payload = payload_demonstracao(payload_size);
        const ChaChaNonce packet_nonce = nonce_pacote_sequencial(nonce, last_counter);

        log.println("\n  Mensagem assinada        : \"", message, "\"");
        log.println("  Ponto R da assinatura Ed25519 em coordenadas afins:");
        log.println("    x = ", defesa_mpz_hex(x_affine));
        log.println("    y = ", defesa_mpz_hex(y_affine));
        log.println("  Assinatura R comprimida  : ", bytes_to_hex(signature.data(), 32));
        log.println("  Assinatura S             : ", bytes_to_hex(signature.data() + 32, 32));
        log.println("  Verificacao Ed25519      : ", signature_valid ? "VALIDA" : "INVALIDA");
        log.println("  Chave final HKDF-SHA512  : ", bytes_to_hex(key));
        log.println("  Chave HKDF little-endian : ", defesa_chave_little_endian(key));
        log.println("  Nonce de demonstracao    : ", bytes_to_hex(nonce));
        log.println("  Payload solicitado       : ", payload_size, " bytes");
        log.println("  Blocos payload (64 B)   : ", payload_blocks);
        log.println("  Total ChaCha20           : 1 bloco Poly1305 + ", payload_blocks,
                    " blocos payload = ", total_blocks, " blocos");

        log.println("\n[AEAD] Pacote ", last_counter, " | nonce do pacote ", last_counter,
                    " = ", bytes_to_hex(packet_nonce));
        log.println("  Payload original: ", payload);
        log.println("\n[1/", total_blocks, "] Gerando subchave Poly1305 com bloco ChaCha20=0");
        imprimir_bloco_chacha(key, packet_nonce, last_counter, 0, log);
        for (std::size_t block = 1; block <= payload_blocks; ++block) {
            log.println("\n[", block + 1, "/", total_blocks,
                        "] Cifrando bloco de payload ChaCha20=", block,
                        " (", std::min<std::size_t>(64, payload_size - (block - 1) * 64), " bytes)");
            imprimir_bloco_chacha(key, packet_nonce, last_counter,
                                  static_cast<uint32_t>(block), log);
        }

        auto encrypted = chacha20poly1305_encrypt(
            key, packet_nonce, reinterpret_cast<const uint8_t *>(payload.data()),
            payload.size(), reinterpret_cast<const uint8_t *>(aad.data()), aad.size());
        const Poly1305Tag receiver_tag = defesa_calcular_tag(
            key, packet_nonce, reinterpret_cast<const uint8_t *>(aad.data()), aad.size(),
            encrypted.ciphertext.data(), encrypted.ciphertext.size());
        log.println("\n[TRANSMISSAO] Ciphertext enviado: ", bytes_to_hex(encrypted.ciphertext));
        log.println("[AUTENTICACAO] Tag emissor   : ", bytes_to_hex(encrypted.tag));
        log.println("[AUTENTICACAO] Tag receptor : ", bytes_to_hex(receiver_tag));
        log.println("[AUTENTICACAO] Tags: ", encrypted.tag == receiver_tag ? "IGUAIS" : "DIFERENTES");
        if (encrypted.tag != receiver_tag)
            throw std::runtime_error("tag divergente durante a demonstracao");
        auto recovered = defesa_decrypt_payload(key, packet_nonce, encrypted.ciphertext);
        const std::string recovered_payload(recovered.begin(), recovered.end());
        log.println("\n[DECIFRAGEM] Tag validada; payload recebido: ", recovered_payload);
        log.println("[DECIFRAGEM] Payloads: ", recovered_payload == payload ? "IGUAIS - ACEITO" : "DIFERENTES - REJEITADO");

        mpz_clears(x_affine, y_affine, inverse, nullptr);
        defesa_limpar(alice_private, alice_public, bob_private, bob_public,
                  shared_secret);
    } catch (...) {
        limpa_parametros_c25519();
        clear_ed25519();
        throw;
    }
}
