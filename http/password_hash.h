#ifndef PASSWORD_HASH_H
#define PASSWORD_HASH_H

#include <openssl/evp.h>
#include <openssl/rand.h>

#include <cstdlib>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

namespace password_hash
{
    const int SALT_BYTES = 16;
    const int HASH_BYTES = 32;
    const int PBKDF2_ITERATIONS = 100000;

    inline std::string bytes_to_hex(const unsigned char *data, size_t len)
    {
        std::ostringstream oss;
        for (size_t i = 0; i < len; ++i)
        {
            oss << std::hex << std::setw(2) << std::setfill('0')
                << static_cast<int>(data[i]);
        }
        return oss.str();
    }

    inline bool hex_to_bytes(const std::string &hex, std::vector<unsigned char> &out)
    {
        if (hex.size() % 2 != 0)
        {
            return false;
        }

        out.clear();
        out.reserve(hex.size() / 2);

        for (size_t i = 0; i < hex.size(); i += 2)
        {
            std::string byte_string = hex.substr(i, 2);
            char *end = nullptr;
            long value = std::strtol(byte_string.c_str(), &end, 16);

            if (end == nullptr || *end != '\0' || value < 0 || value > 255)
            {
                return false;
            }

            out.push_back(static_cast<unsigned char>(value));
        }

        return true;
    }

    inline bool constant_time_equal(const std::vector<unsigned char> &a,
                                    const std::vector<unsigned char> &b)
    {
        if (a.size() != b.size())
        {
            return false;
        }

        unsigned char diff = 0;
        for (size_t i = 0; i < a.size(); ++i)
        {
            diff |= a[i] ^ b[i];
        }

        return diff == 0;
    }

    inline std::string make_password_hash(const std::string &password)
    {
        unsigned char salt[SALT_BYTES];
        unsigned char hash[HASH_BYTES];

        if (RAND_bytes(salt, SALT_BYTES) != 1)
        {
            return "";
        }

        if (PKCS5_PBKDF2_HMAC(password.c_str(),
                              password.size(),
                              salt,
                              SALT_BYTES,
                              PBKDF2_ITERATIONS,
                              EVP_sha256(),
                              HASH_BYTES,
                              hash) != 1)
        {
            return "";
        }

        std::ostringstream oss;
        oss << "pbkdf2_sha256"
            << "$" << PBKDF2_ITERATIONS
            << "$" << bytes_to_hex(salt, SALT_BYTES)
            << "$" << bytes_to_hex(hash, HASH_BYTES);

        return oss.str();
    }

    inline bool verify_password(const std::string &password,
                                const std::string &stored_hash)
    {
        size_t first = stored_hash.find('$');
        size_t second = stored_hash.find('$', first + 1);
        size_t third = stored_hash.find('$', second + 1);

        if (first == std::string::npos ||
            second == std::string::npos ||
            third == std::string::npos)
        {
            return false;
        }

        std::string algorithm = stored_hash.substr(0, first);
        std::string iterations_text = stored_hash.substr(first + 1, second - first - 1);
        std::string salt_hex = stored_hash.substr(second + 1, third - second - 1);
        std::string hash_hex = stored_hash.substr(third + 1);

        if (algorithm != "pbkdf2_sha256")
        {
            return false;
        }

        int iterations = std::atoi(iterations_text.c_str());
        if (iterations <= 0)
        {
            return false;
        }

        std::vector<unsigned char> salt;
        std::vector<unsigned char> saved_hash;

        if (!hex_to_bytes(salt_hex, salt) || !hex_to_bytes(hash_hex, saved_hash))
        {
            return false;
        }

        std::vector<unsigned char> input_hash(saved_hash.size());

        if (PKCS5_PBKDF2_HMAC(password.c_str(),
                              password.size(),
                              salt.data(),
                              salt.size(),
                              iterations,
                              EVP_sha256(),
                              input_hash.size(),
                              input_hash.data()) != 1)
        {
            return false;
        }

        return constant_time_equal(input_hash, saved_hash);
    }
}

#endif

