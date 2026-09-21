#pragma once

#include "../defesa_common.hpp"
#include <bitset>

inline std::string payload_demonstracao(const std::string &msg, std::size_t size) {
    if (!msg.empty() && msg != "mensagem para a banca") {
        if (size <= msg.size()) return msg;
        return msg + std::string(size - msg.size(), ' ');
    }
    const std::string base_msg = "telemetria{device=sensor_A1,temp=23.4,humidity=61}";
    if (size <= base_msg.size()) {
        return base_msg.substr(0, size);
    }
    const std::string prefix = "telemetria{device=sensor_A1,temp=23.4,padding=";
    const std::string suffix = "}";
    if (size < prefix.size() + suffix.size() + 1) {
        std::string s = base_msg;
        s.resize(size, ' ');
        return s;
    }
    const std::size_t pad_len = size - prefix.size() - suffix.size();
    return prefix + std::string(pad_len, '0') + suffix;
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
    (void)packet_counter;
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
        const std::string payload = payload_demonstracao(custom_message, payload_size);
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
        log.println("  Nonce Base da sessao (N0): ", bytes_to_hex(nonce));
        log.println("  Payload solicitado       : ", payload_size, " bytes");
        log.println("  Blocos payload (64 B)   : ", payload_blocks);
        log.println("  Total ChaCha20           : 1 bloco Poly1305 + ", payload_blocks,
                    " blocos payload = ", total_blocks, " blocos");

        log.println("\n[AEAD] Pacote ", last_counter, " — EVOLUCAO DO NONCE (Regra RFC 8439 Anti-Reuso):");
        log.println("  Nonce Base da Sessao (N0) : ", bytes_to_hex(nonce));
        log.println("  Incremento do Contador    : +", last_counter, " (0x", std::hex, last_counter, std::dec, ")");
        log.println("  Nonce do Pacote ", last_counter, "        : ", bytes_to_hex(packet_nonce));
        log.println("  Nonce em little-endian    : ", defesa_nonce_little_endian(packet_nonce));

        std::string display_orig = payload;
        while (!display_orig.empty() && display_orig.back() == ' ')
            display_orig.pop_back();
        log.println("\n  Payload original: \"", display_orig, "\" (", payload.size(), " bytes)");

        log.println("\n[1/", total_blocks, "] Gerando subchave Poly1305 com bloco ChaCha20=0");
        imprimir_bloco_chacha(key, packet_nonce, last_counter, 0, log);
        for (std::size_t block = 1; block <= payload_blocks; ++block) {
            const std::size_t offset = (block - 1) * 64;
            const std::size_t block_bytes = std::min<std::size_t>(64, payload.size() - offset);
            log.println("\n[", block + 1, "/", total_blocks,
                        "] Cifrando bloco de payload ChaCha20=", block,
                        " (", block_bytes, " bytes)");
            auto ks = imprimir_bloco_chacha(key, packet_nonce, last_counter,
                                            static_cast<uint32_t>(block), log);

            std::vector<uint8_t> pt_chunk(payload.begin() + offset, payload.begin() + offset + block_bytes);
            std::vector<uint8_t> ct_chunk(block_bytes);
            for (std::size_t i = 0; i < block_bytes; ++i) {
                ct_chunk[i] = pt_chunk[i] ^ ks[i];
            }
            log.println("    Operacao XOR de Cifragem (Payload (+) Keystream = Ciphertext):");
            if (block_bytes <= 32) {
                log.println("      Payload bloco (hex)    : ", bytes_to_hex(pt_chunk));
                log.println("      Keystream bloco (hex)  : ", bytes_to_hex(ks.data(), block_bytes));
                log.println("      Ciphertext bloco (hex) : ", bytes_to_hex(ct_chunk));
            } else {
                log.println("      Payload bloco (hex)    : ", bytes_to_hex(pt_chunk.data(), 32), "... (", block_bytes, " bytes)");
                log.println("      Keystream bloco (hex)  : ", bytes_to_hex(ks.data(), 32), "...");
                log.println("      Ciphertext bloco (hex) : ", bytes_to_hex(ct_chunk.data(), 32), "...");
            }
        }

        auto encrypted = chacha20poly1305_encrypt(
            key, packet_nonce, reinterpret_cast<const uint8_t *>(payload.data()),
            payload.size(), reinterpret_cast<const uint8_t *>(aad.data()), aad.size());
        const Poly1305Tag receiver_tag = defesa_calcular_tag(
            key, packet_nonce, reinterpret_cast<const uint8_t *>(aad.data()), aad.size(),
            encrypted.ciphertext.data(), encrypted.ciphertext.size());

        log.println("\n=============================================================================");
        log.println(" [MEIO FISICO / ENLACE DE RADIO] QUADRO TRANSMITIDO NO CANAL (ANTENA / CABO)");
        log.println("=============================================================================");
        log.println(" Estrutura do Pacote AEAD em Transito:");
        log.println("   [ Nonce (12 Bytes) | Ciphertext (", encrypted.ciphertext.size(), " Bytes) | Tag Poly1305 (16 Bytes) ]");
        log.println("\n Detalhamento dos Campos no Meio Fisico:");
        log.println("   1. Nonce de Enlace (12 Bytes)   : ", bytes_to_hex(packet_nonce), " (Publico / anti-replay)");
        log.println("   2. Ciphertext Cifrado (", encrypted.ciphertext.size(), " B) : ",
                    (encrypted.ciphertext.size() <= 48 ? bytes_to_hex(encrypted.ciphertext) : bytes_to_hex(encrypted.ciphertext.data(), 48) + "..."));
        log.println("   3. Tag de Autenticacao (16 B)   : ", bytes_to_hex(encrypted.tag), " (Garante integridade e autenticidade)");
        log.println("\n Sobrecarga do Protocolo no Canal IoT:");
        log.println("   Payload Util : ", payload.size(), " Bytes");
        log.println("   Overhead AEAD: 28 Bytes (12 B Nonce + 16 B Tag Poly1305)");
        log.println("   Total Transmitido: ", payload.size() + 28, " Bytes (Goodput: ",
                    std::fixed, std::setprecision(1), (static_cast<double>(payload.size()) / (payload.size() + 28)) * 100.0, " %)");
        log.println("=============================================================================");

        log.println("\n[AUTENTICACAO]");
        log.println("  Tag enviada pelo emissor     : ", bytes_to_hex(encrypted.tag));
        log.println("  Tag recalculada pelo receptor: ", bytes_to_hex(receiver_tag));
        log.println("  Verificacao Poly1305         : ", encrypted.tag == receiver_tag ? "IGUAIS (VALIDA)" : "DIFERENTES (FALHA)");
        if (encrypted.tag != receiver_tag)
            throw std::runtime_error("tag divergente durante a demonstracao");

        auto recovered = defesa_decrypt_payload(key, packet_nonce, encrypted.ciphertext);
        const std::string recovered_payload(recovered.begin(), recovered.end());
        std::string display_received = recovered_payload;
        while (!display_received.empty() && display_received.back() == ' ')
            display_received.pop_back();

        log.println("\n[DECIFRAGEM]");
        log.println("  Tag validada com sucesso pelo receptor.");
        log.println("  Payload original : \"", display_orig, "\"");
        log.println("  Payload recebido : \"", display_received, "\"");
        log.println("  Payloads         : ", recovered_payload == payload ? "IGUAIS - ACEITO" : "DIFERENTES - REJEITADO");

        mpz_clears(x_affine, y_affine, inverse, nullptr);
        defesa_limpar(alice_private, alice_public, bob_private, bob_public,
                  shared_secret);
    } catch (...) {
        limpa_parametros_c25519();
        clear_ed25519();
        throw;
    }
}
