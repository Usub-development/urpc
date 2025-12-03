#ifndef URPC_APPCRYPTO_H
#define URPC_APPCRYPTO_H

#include <array>
#include <cstdint>
#include <span>
#include <vector>

#include <openssl/evp.h>
#include <openssl/rand.h>
#include <ulog/ulog.h>

namespace urpc
{
    struct AppCipherContext
    {
        std::array<uint8_t, 32> key{};
        bool valid{false};
    };

    inline bool app_encrypt_gcm(
        const AppCipherContext& ctx,
        std::span<const uint8_t> plaintext,
        std::vector<uint8_t>& out)
    {
        if (!ctx.valid)
            return false;

        EVP_CIPHER_CTX* c = EVP_CIPHER_CTX_new();
        if (!c)
            return false;

        const EVP_CIPHER* cipher = EVP_aes_256_gcm();
        if (!cipher)
        {
            EVP_CIPHER_CTX_free(c);
            return false;
        }

        uint8_t iv[12];
        if (RAND_bytes(iv, sizeof(iv)) != 1)
        {
            EVP_CIPHER_CTX_free(c);
            return false;
        }

        if (EVP_EncryptInit_ex(c, cipher, nullptr, nullptr, nullptr) != 1)
        {
            EVP_CIPHER_CTX_free(c);
            return false;
        }

        if (EVP_CIPHER_CTX_ctrl(
            c, EVP_CTRL_GCM_SET_IVLEN,
            static_cast<int>(sizeof(iv)), nullptr) != 1)
        {
            EVP_CIPHER_CTX_free(c);
            return false;
        }

        if (EVP_EncryptInit_ex(
            c, nullptr, nullptr,
            ctx.key.data(), iv) != 1)
        {
            EVP_CIPHER_CTX_free(c);
            return false;
        }

        out.clear();
        out.reserve(12 + plaintext.size() + 16);

        out.insert(out.end(), iv, iv + sizeof(iv));

        std::vector<uint8_t> ct;
        ct.resize(plaintext.size() + 16);

        int len = 0;
        int total = 0;

        if (!plaintext.empty())
        {
            if (EVP_EncryptUpdate(
                c,
                ct.data(),
                &len,
                plaintext.data(),
                static_cast<int>(plaintext.size())) != 1)
            {
                EVP_CIPHER_CTX_free(c);
                return false;
            }
            total = len;
        }

        if (EVP_EncryptFinal_ex(c, ct.data() + total, &len) != 1)
        {
            EVP_CIPHER_CTX_free(c);
            return false;
        }
        total += len;
        ct.resize(total);

        out.insert(out.end(), ct.begin(), ct.end());

        uint8_t tag[16];
        if (EVP_CIPHER_CTX_ctrl(
            c, EVP_CTRL_GCM_GET_TAG,
            static_cast<int>(sizeof(tag)),
            tag) != 1)
        {
            EVP_CIPHER_CTX_free(c);
            return false;
        }
        out.insert(out.end(), tag, tag + sizeof(tag));

        EVP_CIPHER_CTX_free(c);
        return true;
    }

    inline bool app_decrypt_gcm(
        const AppCipherContext& ctx,
        std::span<const uint8_t> enc,
        std::vector<uint8_t>& out)
    {
        if (!ctx.valid)
            return false;

        if (enc.size() < 12 + 16)
            return false;

        const uint8_t* iv = enc.data();
        const uint8_t* tag = enc.data() + enc.size() - 16;
        const uint8_t* ct = enc.data() + 12;
        const std::size_t ct_len = enc.size() - 12 - 16;

        EVP_CIPHER_CTX* c = EVP_CIPHER_CTX_new();
        if (!c)
            return false;

        const EVP_CIPHER* cipher = EVP_aes_256_gcm();
        if (!cipher)
        {
            EVP_CIPHER_CTX_free(c);
            return false;
        }

        if (EVP_DecryptInit_ex(c, cipher, nullptr, nullptr, nullptr) != 1)
        {
            EVP_CIPHER_CTX_free(c);
            return false;
        }

        if (EVP_CIPHER_CTX_ctrl(
            c, EVP_CTRL_GCM_SET_IVLEN,
            12, nullptr) != 1)
        {
            EVP_CIPHER_CTX_free(c);
            return false;
        }

        if (EVP_DecryptInit_ex(
            c, nullptr, nullptr,
            ctx.key.data(), iv) != 1)
        {
            EVP_CIPHER_CTX_free(c);
            return false;
        }

        out.clear();
        out.resize(ct_len);

        int len = 0;
        int total = 0;

        if (ct_len > 0)
        {
            if (EVP_DecryptUpdate(
                c,
                out.data(),
                &len,
                ct,
                static_cast<int>(ct_len)) != 1)
            {
                EVP_CIPHER_CTX_free(c);
                return false;
            }
            total = len;
        }

        if (EVP_CIPHER_CTX_ctrl(
            c, EVP_CTRL_GCM_SET_TAG,
            16, const_cast<uint8_t*>(tag)) != 1)
        {
            EVP_CIPHER_CTX_free(c);
            return false;
        }

        if (EVP_DecryptFinal_ex(c, out.data() + total, &len) != 1)
        {
            EVP_CIPHER_CTX_free(c);
            return false;
        }
        total += len;
        out.resize(total);

        EVP_CIPHER_CTX_free(c);
        return true;
    }
}

#endif // URPC_APPCRYPTO_H
