#include "CryptoUtils.h"

namespace {
    // ChaCha20 constants
    constexpr uint32_t chachaConstants[4] = {
        0x61707865, 0x3320646e, 0x79622d32, 0x6b206574
    };

  
    const uint32_t k[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,
    0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,
    0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,
    0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,
    0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,
    0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,
    0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,
    0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,
    0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
    };

// Auxiliary functions

    //Move X to y and y To x by  Left
    uint32_t RotateLeft(uint32_t x, int n) {
        return (x << n) | (x >> (32 - n));
    }

    //Move X to y and y To x by Right
    inline uint32_t RotateRight(uint32_t x, uint32_t n) {
        return (x >> n) | (x << (32 - n));
    }

    //Choice function Choose between y && z by x
    inline uint32_t ch(uint32_t x, uint32_t y, uint32_t z) {
        return (x & y) ^ (~x & z);
    }

    void QuarterRound(uint32_t& a, uint32_t& b, uint32_t& c, uint32_t& d) {
        a += b; d ^= a; d = RotateLeft(d, 16);
        c += d; b ^= c; b = RotateLeft(b, 12);
        a += b; d ^= a; d = RotateLeft(d, 8);
        c += d; b ^= c; b = RotateLeft(b, 7);
    }
    
    //Get the most Value between x,y,z and return this value
    inline uint32_t maj(uint32_t x, uint32_t y, uint32_t z) {
        return (x & y) ^ (x & z) ^ (y & z);
    }

    inline uint32_t sig0(uint32_t x) {
        return RotateRight(x, 7) ^ RotateRight(x, 18) ^ (x >> 3);
    }

    inline uint32_t sig1(uint32_t x) {
        return RotateRight(x, 17) ^ RotateRight(x, 19) ^ (x >> 10);
    }

    inline uint32_t ep0(uint32_t x) {
        return RotateRight(x, 2) ^ RotateRight(x, 13) ^ RotateRight(x, 22);
    }

    inline uint32_t ep1(uint32_t x) {
        return RotateRight(x, 6) ^ RotateRight(x, 11) ^ RotateRight(x, 25);
    }
}

CryptoUtils::CryptoUtils() {}

CryptoUtils::~CryptoUtils() {}

//////////////////////////////////////First Position

//Generate secure random numbers (To Generate Salt && Nonce )
void CryptoUtils::GenerateRandomBytes(uint8_t* output, size_t size) {
#if defined(_WIN32)
    if (BCryptGenRandom(nullptr, output, static_cast<ULONG>(size),
        BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) {
        throw std::runtime_error("BCryptGenRandom failed");
    }

#elif defined(__linux__)
    ssize_t result = getrandom(output, size, 0);
    if (result != static_cast<ssize_t>(size)) {
        throw std::runtime_error("getrandom failed");
    }

#else
    // fallback to C++ random_device
    static std::random_device rd;
    for (size_t i = 0; i < size; ++i) {
        output[i] = static_cast<uint8_t>(rd() & 0xFF); //To get last Random 8 bits
    }
#endif
}

// Generate a random key (Nonce)
std::vector<uint8_t> CryptoUtils::GenerateNonce() {
    std::vector<uint8_t> nonce(nonceSize);
    GenerateRandomBytes(nonce.data(), nonce.size());
    return nonce;
}

// Generate a random salt (Salt)
std::vector<uint8_t> CryptoUtils::GenerateSalt() {
    std::vector<uint8_t> salt(saltSize);
    GenerateRandomBytes(salt.data(), salt.size());
    return salt;
}

//////////////////////////////////////Second Position

//To Encryption First TAG
std::vector<uint8_t> CryptoUtils::sha256(const std::vector<uint8_t>& data) {
    static const uint32_t k[64] = {
        0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
        0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
        0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
        0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
        0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
        0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
        0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
        0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
    };

    auto rotr = [](uint32_t x, uint32_t n) { return (x >> n) | (x << (32 - n)); };
    auto ch = [](uint32_t x, uint32_t y, uint32_t z) { return (x & y) ^ (~x & z); };
    auto maj = [](uint32_t x, uint32_t y, uint32_t z) { return (x & y) ^ (x & z) ^ (y & z); };
    auto sig0 = [&](uint32_t x) { return rotr(x, 7) ^ rotr(x, 18) ^ (x >> 3); };
    auto sig1 = [&](uint32_t x) { return rotr(x, 17) ^ rotr(x, 19) ^ (x >> 10); };
    auto ep0 = [&](uint32_t x) { return rotr(x, 2) ^ rotr(x, 13) ^ rotr(x, 22); };
    auto ep1 = [&](uint32_t x) { return rotr(x, 6) ^ rotr(x, 11) ^ rotr(x, 25); };

    uint32_t h[8] = {
        0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,
        0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19
    };

    size_t originalLen = data.size();
    size_t paddedLen = ((originalLen + 9 + 63) / 64) * 64;
    std::vector<uint8_t> padded(paddedLen, 0);

    std::copy(data.begin(), data.end(), padded.begin());
    padded[originalLen] = 0x80;
    uint64_t bitLen = static_cast<uint64_t>(originalLen) * 8;
    for (int i = 0; i < 8; ++i) {
        padded[paddedLen - 1 - i] = (bitLen >> (8 * i)) & 0xff;
    }

    for (size_t i = 0; i < padded.size(); i += 64) {
        uint32_t w[64];
        for (int j = 0; j < 16; j++) {
            w[j] = (padded[i + j * 4] << 24) | (padded[i + j * 4 + 1] << 16) |
                (padded[i + j * 4 + 2] << 8) | padded[i + j * 4 + 3];
        }
        for (int j = 16; j < 64; j++) {
            w[j] = sig1(w[j - 2]) + w[j - 7] + sig0(w[j - 15]) + w[j - 16];
        }

        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];

        for (int j = 0; j < 64; j++) {
            uint32_t t1 = hh + ep1(e) + ch(e, f, g) + k[j] + w[j];
            uint32_t t2 = ep0(a) + maj(a, b, c);
            hh = g; g = f; f = e;
            e = d + t1;
            d = c; c = b; b = a;
            a = t1 + t2;
        }

        h[0] += a; h[1] += b; h[2] += c; h[3] += d;
        h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }

    std::vector<uint8_t> hash(32);
    for (int i = 0; i < 8; i++) {
        hash[i * 4] = (h[i] >> 24) & 0xff;
        hash[i * 4 + 1] = (h[i] >> 16) & 0xff;
        hash[i * 4 + 2] = (h[i] >> 8) & 0xff;
        hash[i * 4 + 3] = h[i] & 0xff;
    }
    return hash;
}

//To Merge Password && Salt
std::vector<uint8_t> CryptoUtils::DeriveKey(
    const std::string& password,
    const std::vector<uint8_t>& salt,
    int iterations)
{
    if (salt.empty()) {
        throw std::invalid_argument("Salt must not be empty");
    }

    std::vector<uint8_t> key(keySize, 0);

    // initial block = password + salt
    std::vector<uint8_t> block(password.begin(), password.end());
    block.insert(block.end(), salt.begin(), salt.end());

    auto u = sha256(block);
    std::copy(u.begin(), u.begin() + key.size(), key.begin());

    for (int i = 1; i < iterations; i++) {
        u = sha256(u);
        for (size_t j = 0; j < key.size(); j++) {
            key[j] ^= u[j % u.size()];
        }
    }
    return key;
}

//////////////////////////////////////Third Position

//Convert 4bytes to 32 Bits from arrays
uint32_t CryptoUtils::Load32(const uint8_t* src) {
    return ((uint32_t)src[0]) |
        ((uint32_t)src[1] << 8) |
        ((uint32_t)src[2] << 16) |
        ((uint32_t)src[3] << 24);
}

//Initialize Chacha20 Settings
void CryptoUtils::SetupChaChaState(uint32_t state[16], const std::vector<uint8_t>& key, const std::vector<uint8_t>& nonce, uint32_t counter) {
    state[0] = chachaConstants[0];
    state[1] = chachaConstants[1];
    state[2] = chachaConstants[2];
    state[3] = chachaConstants[3];

    for (int i = 0; i < 8; ++i)
        state[4 + i] = Load32(key.data() + (i * 4));

    state[12] = counter;
    for (int i = 0; i < 3; ++i)
        state[13 + i] = Load32(nonce.data() + (i * 4));
}

// Apply the ChaCha20 algorithm to a single block
void CryptoUtils::ChaCha20Block(uint32_t out[16], const uint32_t in[16]) {
    uint32_t x[16];
    std::copy(in, in + 16, x);

    // Lambda function for quarter round
    auto quarterRound = [](uint32_t& a, uint32_t& b, uint32_t& c, uint32_t& d) {
        a += b; d ^= a; d = RotateLeft(d, 16);
        c += d; b ^= c; b = RotateLeft(b, 12);
        a += b; d ^= a; d = RotateLeft(d, 8);
        c += d; b ^= c; b = RotateLeft(b, 7);
        };

    for (int i = 0; i < 10; ++i) {
        // Column rounds
        quarterRound(x[0], x[4], x[8], x[12]);
        quarterRound(x[1], x[5], x[9], x[13]);
        quarterRound(x[2], x[6], x[10], x[14]);
        quarterRound(x[3], x[7], x[11], x[15]);

        // Diagonal rounds
        quarterRound(x[0], x[5], x[10], x[15]);
        quarterRound(x[1], x[6], x[11], x[12]);
        quarterRound(x[2], x[7], x[8], x[13]);
        quarterRound(x[3], x[4], x[9], x[14]);
    }

    for (int i = 0; i < 16; ++i) {
        out[i] = x[i] + in[i];
    }
}

// XOR two memory blocks into out
void CryptoUtils::XORBlocks(const uint8_t* a, const uint8_t* b, uint8_t* out, size_t len) {
    size_t i = 0;

#if defined(__AVX2__)
    constexpr size_t simd_size = 32;
    for (; i + simd_size <= len; i += simd_size) {
        __m256i va = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(a + i));
        __m256i vb = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(b + i));
        __m256i vres = _mm256_xor_si256(va, vb);
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(out + i), vres);
    }
#elif defined(__SSE2__)
    constexpr size_t simd_size = 16;
    for (; i + simd_size <= len; i += simd_size) {
        __m128i va = _mm_loadu_si128(reinterpret_cast<const __m128i*>(a + i));
        __m128i vb = _mm_loadu_si128(reinterpret_cast<const __m128i*>(b + i));
        __m128i vres = _mm_xor_si128(va, vb);
        _mm_storeu_si128(reinterpret_cast<__m128i*>(out + i), vres);
    }
#endif

    // 64-bit unrolling
    for (; i + 8 <= len; i += 8) {
        uint64_t va, vb;
        std::memcpy(&va, a + i, 8);
        std::memcpy(&vb, b + i, 8);
        uint64_t vr = va ^ vb;
        std::memcpy(out + i, &vr, 8);
    }

    // Remaining bytes
    for (; i < len; ++i) {
        out[i] = a[i] ^ b[i];
    }
}

// Generate a Poly1305 key
void CryptoUtils::HChaCha20(const uint8_t key[32], const uint8_t nonce[16], uint8_t out[32]) {
    uint32_t state[16] = {
        chachaConstants[0], chachaConstants[1],
        chachaConstants[2], chachaConstants[3],
        Load32(key + 0),
        Load32(key + 4),
        Load32(key + 8),
        Load32(key + 12),
        Load32(key + 16),
        Load32(key + 20),
        Load32(key + 24),
        Load32(key + 28),
        Load32(nonce + 0),
        Load32(nonce + 4),
        Load32(nonce + 8),
        Load32(nonce + 12)
    };

    uint32_t result[16];
    ChaCha20Block(result, state);

    std::copy(reinterpret_cast<uint8_t*>(result),
        reinterpret_cast<uint8_t*>(result) + 32,
        out);
}

// Poly1305 authentication account
void CryptoUtils::Poly1305Mac(const uint8_t* message, size_t length, const uint8_t key[32], uint8_t tag[16]) {
    uint32_t r[5] = {}, h[5] = {}, s[4];
    uint64_t d;

    // Prepare the key
    r[0] = Load32(key + 0) & 0x0fffffff;
    r[1] = Load32(key + 4) & 0x0ffffffc;
    r[2] = Load32(key + 8) & 0x0ffffffc;
    r[3] = Load32(key + 12) & 0x0ffffffc;
    s[0] = Load32(key + 16);
    s[1] = Load32(key + 20);
    s[2] = Load32(key + 24);
    s[3] = Load32(key + 28);

    // Processing the message
    size_t i = 0;
    while (length > 0) {
        uint32_t c[5] = {};
        size_t blockSize = (length < 16) ? length : 16;

        for (size_t j = 0; j < blockSize; ++j) {
            c[j / 4] |= message[i + j] << (8 * (j % 4));
        }
        c[blockSize / 4] |= 1 << (8 * (blockSize % 4));
        i += blockSize;
        length -= blockSize;

        // Apply the algorithm
        for (int j = 0; j < 5; ++j) h[j] += c[j];
        for (int j = 0; j < 5; ++j) {
            d = 0;
            for (int k = 0; k < 5; ++k) {
                d += (uint64_t)h[k] * (j <= k ? r[k - j] : 5 * r[k + 5 - j]);
            }
            h[j] = d & 0xffffffff;
        }
        d = h[0] >> 26; h[0] &= 0x3ffffff;
        for (int j = 1; j < 5; ++j) {
            h[j] += static_cast<uint32_t>(d);
            d = h[j] >> 26;
            h[j] &= 0x3ffffff;
        }
        h[0] += static_cast<uint32_t>(d * 5);
    }

    // Final normalization
    d = h[0] + 5;
    uint32_t g[5];
    g[0] = d & 0xffffffff;
    for (int j = 1; j < 5; ++j) {
        g[j] = h[j] + (d >> 32);
        d = g[j];
    }
    d = (g[4] >> 22) | ((uint64_t)(g[4] & 0x3fffff) << 10);
    d = (d - 1) >> 63;
    for (int j = 0; j < 5; ++j) g[j] &= ~d;
    for (int j = 0; j < 5; ++j) h[j] = (h[j] & d) | (g[j] & ~d);

    // Add the key
    uint32_t f0 = h[0] + s[0];
    uint32_t f1 = h[1] + s[1] + (f0 >> 26); f0 &= 0x3ffffff;
    uint32_t f2 = h[2] + s[2] + (f1 >> 26); f1 &= 0x3ffffff;
    uint32_t f3 = h[3] + s[3] + (f2 >> 26); f2 &= 0x3ffffff;
    uint32_t f4 = h[4] + (f3 >> 26); f3 &= 0x3ffffff;

    // Final output
    *(uint32_t*)(tag + 0) = f0 | (f1 << 26);
    *(uint32_t*)(tag + 4) = (f1 >> 6) | (f2 << 20);
    *(uint32_t*)(tag + 8) = (f2 >> 12) | (f3 << 14);
    *(uint32_t*)(tag + 12) = (f3 >> 18) | (f4 << 8);
}

//Encryption Data
std::vector<uint8_t> CryptoUtils::Encrypt(const std::vector<uint8_t>& plaintext, const std::vector<uint8_t>& key, const std::vector<uint8_t>& nonce)
{
    if (key.size() != keySize) {
        throw std::runtime_error("Invalid key size");
    }
    if (nonce.size() != nonceSize) {
        throw std::runtime_error("Invalid nonce size");
    }

    std::vector<uint8_t> ciphertext(plaintext.size() + tagSize);

    // 1. ChaCha20 encryption
    uint32_t counter = 1;
    uint32_t state[16];
    SetupChaChaState(state, key, nonce, 1);


    size_t bytesProcessed = 0;
    while (bytesProcessed < plaintext.size()) {
        uint32_t block[16];
        ChaCha20Block(block, state);

        size_t blockSize = std::min<size_t>(64, plaintext.size() - bytesProcessed);
        XORBlocks(plaintext.data() + bytesProcessed,
            reinterpret_cast<uint8_t*>(block),
            ciphertext.data() + bytesProcessed,
            blockSize);

        bytesProcessed += blockSize;
        state[12]++; // Increment the counter
    }

    // 2. Generate a Poly1305 key
    uint8_t polyKey[32];
    HChaCha20(key.data(), nonce.data(), polyKey);

    // 3. Data Authentication Account
    Poly1305Mac(ciphertext.data(), plaintext.size(), polyKey, ciphertext.data() + plaintext.size());

    return ciphertext;
}

//Decreption Data
std::vector<uint8_t> CryptoUtils::Decrypt(const std::vector<uint8_t>& ciphertext, const std::vector<uint8_t>& key, const std::vector<uint8_t>& nonce)
{
    if (ciphertext.size() < tagSize) {
        throw std::runtime_error("Invalid ciphertext size");
    }

    const uint8_t* encryptedData = ciphertext.data();
    size_t dataSize = ciphertext.size() - tagSize;

    // Data validation
    uint8_t polyKey[32];
    HChaCha20(key.data(), nonce.data(), polyKey);

    uint8_t calculatedTag[tagSize];
    Poly1305Mac(encryptedData, dataSize, polyKey, calculatedTag);

    if (!std::equal(calculatedTag, calculatedTag + tagSize,
        encryptedData + dataSize)) {
        throw std::runtime_error("Authentication failed");
    }

    // Decryption
    std::vector<uint8_t> plaintext(dataSize);

    uint32_t counter = 1;
    uint32_t state[16];
    SetupChaChaState(state, key, nonce, 1);

    size_t bytesProcessed = 0;
    while (bytesProcessed < dataSize) {
        uint32_t block[16];
        ChaCha20Block(block, state);

        size_t blockSize = std::min<size_t>(64, dataSize - bytesProcessed);
        XORBlocks(encryptedData + bytesProcessed,
            reinterpret_cast<uint8_t*>(block),
            plaintext.data() + bytesProcessed,
            blockSize);

        bytesProcessed += blockSize;
        state[12]++; // Increment the counter
    }

    return plaintext;
}

//////////////////////////////////Run Code

std::vector<uint8_t> CryptoUtils::EncryptWithSalt(const std::vector<uint8_t>& plaintext, const std::string& password)
{

    // 1. Generate random salt
    std::vector<uint8_t> salt = GenerateSalt();

    // 2. Generate a random nonce
    std::vector<uint8_t> nonce = GenerateNonce();

    // 3. Deriving the key
    auto key = DeriveKey(password, salt);

    // 4. Data encryption
    auto ciphertext = Encrypt(plaintext, key, nonce);

    // 5. Merge salt and nonce with the output
    std::vector<uint8_t> result;
    result.reserve(salt.size() + nonce.size() + ciphertext.size());
    result.insert(result.end(), salt.begin(), salt.end());
    result.insert(result.end(), nonce.begin(), nonce.end());
    result.insert(result.end(), ciphertext.begin(), ciphertext.end());

    return result;
}

std::vector<uint8_t> CryptoUtils::DecryptWithSalt(const std::vector<uint8_t>& fullData, const std::string& password)
{

    if (fullData.size() < saltSize + nonceSize + tagSize) {
        throw std::runtime_error("Invalid data size for decryption");
    }

    std::vector<uint8_t> salt(fullData.begin(), fullData.begin() + saltSize);
    auto key = DeriveKey(password, salt);

    std::vector<uint8_t> nonce(
        fullData.begin() + saltSize,
        fullData.begin() + saltSize + nonceSize);

    std::vector<uint8_t> encryptedDataWithTag(
        fullData.begin() + saltSize + nonceSize,
        fullData.end());

    return Decrypt(encryptedDataWithTag, key, nonce);
}

std::vector<uint8_t> CryptoUtils::CreatePassword(std::string password, size_t iterations)
{
    // 1. Generate random salt
    std::vector<uint8_t> salt = GenerateSalt();

    // 2. Deriving the key
    auto key = DeriveKey(password, salt, 1000);

    std::vector<uint8_t> combined;
    combined.reserve(salt.size() + key.size());
    combined.insert(combined.end(), salt.begin(), salt.end());
    combined.insert(combined.end(), key.begin(), key.end());

    return combined;
}

bool CryptoUtils::ValidatePassword(const std::string& password, const std::vector<uint8_t>& storedKey, size_t iterations) {
    size_t saltSize = 16; // نفس الحجم اللي بتولده GenerateSalt()
    if (storedKey.size() <= saltSize) return false;

    // استخراج الـ salt
    std::vector<uint8_t> salt(storedKey.begin(), storedKey.begin() + saltSize);

    // استخراج الـ hash
    std::vector<uint8_t> storedHash(storedKey.begin() + saltSize, storedKey.end());

    // إعادة الاشتقاق من الباسورد المدخل
    std::vector<uint8_t> derived = DeriveKey(password, salt, iterations);

    if (derived.size() != storedHash.size()) return false;

    // مقارنة ثابتة
    volatile uint8_t diff = 0;
    for (size_t i = 0; i < derived.size(); i++) {
        diff |= (derived[i] ^ storedHash[i]);
    }

    return diff == 0;
}
