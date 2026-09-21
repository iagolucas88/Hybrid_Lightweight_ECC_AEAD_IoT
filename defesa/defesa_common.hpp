#pragma once

#define main protocolo_hibrido_original_main
#include "../src/ECDSA_ECDH_C25519.cpp"
#undef main

#include "defesa_logger.hpp"
#include <cstdint>
#include <iomanip>
#include <limits>
#include <set>
#include <sstream>

inline std::string defesa_mpz_hex(const mpz_t value) {
    return mpz_to_hex32(value);
}

inline std::string defesa_chave_little_endian(const ChaChaKey &key) {
    std::ostringstream output;
    output << std::hex << std::setfill('0');
    for (std::size_t index = 0; index < key.size(); index += 4) {
        output << std::setw(8) << load32_le(key.data() + index);
        if (index + 4 < key.size()) output << ' ';
    }
    return output.str();
}

inline std::string defesa_nonce_little_endian(const ChaChaNonce &nonce) {
    std::ostringstream output;
    output << std::hex << std::setfill('0');
    for (std::size_t index = 0; index < nonce.size(); index += 4) {
        output << std::setw(8) << load32_le(nonce.data() + index);
        if (index + 4 < nonce.size()) output << ' ';
    }
    return output.str();
}

inline std::string defesa_mpz_hex_le(const mpz_t value, std::size_t bytes) {
    std::vector<uint8_t> buffer(bytes, 0);
    std::size_t written = 0;
    mpz_export(buffer.data(), &written, -1, 1, 0, 0, value);
    return bytes_to_hex(buffer);
}

inline void defesa_handshake(Ed25519KeyPair &identity,
                             mpz_t &alice_private,
                             mpz_t &alice_public,
                             mpz_t &bob_private,
                             mpz_t &bob_public,
                             mpz_t &shared_secret,
                             DefenseLogger &log) {
    identity = ed25519_keygen();
    gera_escalar_rand(alice_private);
    gera_escalar_rand(bob_private);
    multiplicacao_escalar(alice_public, alice_private, CURVA_Gu);
    multiplicacao_escalar(bob_public, bob_private, CURVA_Gu);
    multiplicacao_escalar(shared_secret, alice_private, bob_public);

    mpz_t check;
    mpz_init(check);
    multiplicacao_escalar(check, bob_private, alice_public);
    if (mpz_cmp(shared_secret, check) != 0) {
        mpz_clear(check);
        throw std::runtime_error("ECDH X25519 produziu segredos diferentes");
    }
    mpz_clear(check);

    log.println("  Ed25519 publico: ", bytes_to_hex(identity.pub));
    log.println("  X25519 Alice u : ", defesa_mpz_hex(alice_public));
    log.println("  X25519 Bob u   : ", defesa_mpz_hex(bob_public));
    log.println("  Shared secret  : ", defesa_mpz_hex(shared_secret));
}

inline ChaChaKey defesa_derivar_chave(const mpz_t &shared_secret,
                                      const uint8_t *salt,
                                      std::size_t salt_len) {
    std::array<uint8_t, 32> ikm{};
    std::size_t written = 0;
    mpz_export(ikm.data(), &written, 1, 1, 0, 0, shared_secret);
    const std::string info = "iot_crypto:defesa:chacha20:v1";
    auto okm = hkdf(ikm.data(), ikm.size(), salt, salt_len,
                    reinterpret_cast<const uint8_t *>(info.data()), info.size(), 32);
    ChaChaKey key{};
    std::copy(okm.begin(), okm.end(), key.begin());
    return key;
}

/// Recalcula a tag Poly1305 exatamente como o receptor faria, a partir do
/// ciphertext efetivamente recebido (permite comparar com a tag transmitida).
inline Poly1305Tag defesa_calcular_tag(const ChaChaKey &key, const ChaChaNonce &nonce,
                                       const uint8_t *aad, std::size_t aad_len,
                                       const uint8_t *ciphertext, std::size_t ct_len) {
    std::array<uint8_t, 64> subkey_block{};
    chacha20_xor(key, nonce, 0, subkey_block.data(), subkey_block.size());
    std::array<uint8_t, 32> poly_key{};
    std::copy(subkey_block.begin(), subkey_block.begin() + 32, poly_key.begin());
    auto auth_msg = poly1305_msg(aad, aad_len, ciphertext, ct_len);
    return poly1305_mac(poly_key, auth_msg.data(), auth_msg.size());
}

/// Decifra sem reverificar a tag (usar somente apos confirmar a tag manualmente).
inline std::vector<uint8_t> defesa_decrypt_payload(const ChaChaKey &key, const ChaChaNonce &nonce,
                                                   std::vector<uint8_t> ciphertext) {
    chacha20_xor(key, nonce, 1, ciphertext.data(), ciphertext.size());
    return ciphertext;
}

/// Gera o bloco de keystream puro (ChaCha20 aplicado sobre 64 bytes zerados).
inline std::array<uint8_t, 64> defesa_keystream_bloco(const ChaChaKey &key, const ChaChaNonce &nonce,
                                                      uint32_t counter) {
    std::array<uint8_t, 64> block{};
    chacha20_xor(key, nonce, counter, block.data(), block.size());
    return block;
}

inline void defesa_limpar(mpz_t &alice_private, mpz_t &alice_public,
                          mpz_t &bob_private, mpz_t &bob_public,
                          mpz_t &shared_secret) {
    mpz_clears(alice_private, alice_public, bob_private, bob_public,
               shared_secret, nullptr);
    limpa_parametros_c25519();
    clear_ed25519();
}

inline std::set<int> defesa_ler_indices(const std::string &value, int max_index = -1) {
    std::set<int> indices;
    std::stringstream input(value);
    std::string token;
    while (std::getline(input, token, ',')) {
        std::stringstream item(token);
        int index = 0;
        char extra = 0;
        if (!(item >> index) || (item >> extra) || index <= 0)
            throw std::runtime_error("indices invalidos; use o formato 13,19");
        if (max_index > 0 && index > max_index) {
            throw std::runtime_error("Indice de ataque " + std::to_string(index) +
                                     " excede a quantidade total de mensagens N (" +
                                     std::to_string(max_index) + ")");
        }
        indices.insert(index);
    }
    return indices;
}

inline void defesa_banner(DefenseLogger &log, const std::string &title) {
    log.println("\n============================================================");
    log.println(" ", title);
    log.println("============================================================");
}
