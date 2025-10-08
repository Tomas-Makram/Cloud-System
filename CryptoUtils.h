#ifndef CRYPTO_UTILS_H
#define CRYPTO_UTILS_H

#include <vector>
#include <string>
#include <cstdint>
#include <fstream>
#include <random>
#include <algorithm>
#include <array>
#include <iostream>
#include <iomanip>
#include <cctype>
#include <cstring>
#include <stdexcept>
#include <cstdlib>

#ifdef _WIN32
#include <windows.h>
#include <bcrypt.h>
#pragma comment(lib, "bcrypt.lib")
#elif __linux__
#include <sys/syscall.h>
#include <unistd.h>

static ssize_t getrandom(void* buf, size_t buflen, unsigned int flags) {
    return syscall(SYS_getrandom, buf, buflen, flags);
}

#else
#include <random>  // fallback
#endif

class CryptoUtils {
public:
    // [16 - byte Salt] [24 - byte Nonce] [Encrypted Data] [16 - byte Tag]
    const size_t nonceSize = 24;  // 192-bit
    const size_t saltSize = 16;  // 128-bit salt
    static constexpr size_t tagSize = 16; // 128-bit
    const size_t keySize = saltSize + tagSize;  // 256-bit (Salt + Tag)
    static constexpr int defaultLoopIteration = 100000;

    // Constructor
    CryptoUtils();

    // Destructor
    ~CryptoUtils();

    // Basic encryption and decryption
    std::vector<uint8_t> EncryptWithSalt(const std::vector<uint8_t>& plaintext,const std::string& password);
    std::vector<uint8_t> DecryptWithSalt(const std::vector<uint8_t>& fullData,const std::string& password);

    std::vector<uint8_t> CreatePassword(std::string password, size_t iterations = defaultLoopIteration);

    bool ValidatePassword(const std::string& password, const std::vector<uint8_t>& storedKey, size_t iterations = defaultLoopIteration);

    const size_t ExtraSize()
    {
        return keySize + nonceSize; //16 + 24 + 16 = 56
    }

private:
    //////////////////////////////////First Position
    // Generate Nonce and salt
    std::vector<uint8_t> GenerateNonce();
    std::vector<uint8_t> GenerateSalt();

    //////////////////////////////////Second Position
    //To Encryption First TAG
    std::vector<uint8_t> sha256(const std::vector<uint8_t>& data);

    // Derivation of keys
    std::vector<uint8_t> DeriveKey(const std::string& password,
        const std::vector<uint8_t>& salt,
        int iterations = defaultLoopIteration);

    //Convert Bits to Bytes
    uint32_t Load32(const uint8_t* src);

    //Initialize Chacha20 Settings
    void SetupChaChaState(
        uint32_t state[16],
        const std::vector<uint8_t>& key,
        const std::vector<uint8_t>& nonce,
        uint32_t counter = 1
    );

    //Basic encryption and decryption
    std::vector<uint8_t> Encrypt(const std::vector<uint8_t>& plaintext,
        const std::vector<uint8_t>& key,
        const std::vector<uint8_t>& nonce);

    std::vector<uint8_t> Decrypt(
        const std::vector<uint8_t>& ciphertext,
        const std::vector<uint8_t>& key,
        const std::vector<uint8_t>& nonce);

    // Internal algorithms
    void ChaCha20Block(uint32_t out[16], const uint32_t in[16]);
    void Poly1305Mac(const uint8_t* message, size_t length,
        const uint8_t key[32], uint8_t tag[16]);

    // Helper functions
    void XORBlocks(const uint8_t* a, const uint8_t* b, uint8_t* out, size_t len);
    void GenerateRandomBytes(uint8_t* output, size_t size);
    void HChaCha20(const uint8_t key[32], const uint8_t nonce[16], uint8_t out[32]);
};
#endif // CRYPTO_UTILS_H