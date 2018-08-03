// Copyright (c) 2009-2016 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "crypter.h"

#include "crypto/aes.h"
#include "crypto/sha512.h"
#include "script/script.h"
#include "script/standard.h"
#include "util.h"

#include <string>
#include <vector>

int CCrypter::BytesToKeySHA512AES(const std::vector<unsigned char>& chSalt, const SecureString& strKeyData, int count, unsigned char *key,unsigned char *iv) const
{
    // This mimics the behavior of openssl's EVP_BytesToKey with an aes256cbc
    // cipher and sha512 message digest. Because sha512's output size (64b) is
    // greater than the aes256 block size (16b) + aes256 key size (32b),
    // there's no need to process more than once (D_0).

    if(!count || !key || !iv)
        return 0;

    unsigned char buf[CSHA512::OUTPUT_SIZE];
    CSHA512 di;

    di.Write((const unsigned char*)strKeyData.c_str(), strKeyData.size());
    if(chSalt.size())
        di.Write(&chSalt[0], chSalt.size());
    di.Finalize(buf);

    for(int i = 0; i != count - 1; i++)
        di.Reset().Write(buf, sizeof(buf)).Finalize(buf);

    memcpy(key, buf, WALLET_CRYPTO_KEY_SIZE);
    memcpy(iv, buf + WALLET_CRYPTO_KEY_SIZE, WALLET_CRYPTO_IV_SIZE);
    memory_cleanse(buf, sizeof(buf));
    return WALLET_CRYPTO_KEY_SIZE;
}

bool CCrypter::SetKeyFromPassphrase(const SecureString& strKeyData, const std::vector<unsigned char>& chSalt, const unsigned int nRounds, const unsigned int nDerivationMethod)
{
    if (nRounds < 1 || chSalt.size() != WALLET_CRYPTO_SALT_SIZE)
        return false;

    int i = 0;
    if (nDerivationMethod == 0)
        i = BytesToKeySHA512AES(chSalt, strKeyData, nRounds, vchKey.data(), vchIV.data());

    if (i != (int)WALLET_CRYPTO_KEY_SIZE)
    {
        memory_cleanse(vchKey.data(), vchKey.size());
        memory_cleanse(vchIV.data(), vchIV.size());
        return false;
    }

    fKeySet = true;
    return true;
}

bool CCrypter::SetKey(const CKeyingMaterial& chNewKey, const std::vector<unsigned char>& chNewIV)
{
    if (chNewKey.size() != WALLET_CRYPTO_KEY_SIZE || chNewIV.size() != WALLET_CRYPTO_IV_SIZE)
        return false;

    memcpy(vchKey.data(), chNewKey.data(), chNewKey.size());
    memcpy(vchIV.data(), chNewIV.data(), chNewIV.size());

    fKeySet = true;
    return true;
}

bool CCrypter::Encrypt(const CKeyingMaterial& vchPlaintext, std::vector<unsigned char> &vchCiphertext) const
{
    if (!fKeySet)
        return false;

    // max ciphertext len for a n bytes of plaintext is
    // n + AES_BLOCKSIZE bytes
    vchCiphertext.resize(vchPlaintext.size() + AES_BLOCKSIZE);

    AES256CBCEncrypt enc(vchKey.data(), vchIV.data(), true);
    size_t nLen = enc.Encrypt(&vchPlaintext[0], vchPlaintext.size(), &vchCiphertext[0]);
    if(nLen < vchPlaintext.size())
        return false;
    vchCiphertext.resize(nLen);

    return true;
}

bool CCrypter::Decrypt(const std::vector<unsigned char>& vchCiphertext, CKeyingMaterial& vchPlaintext) const
{
    if (!fKeySet)
        return false;

    // plaintext will always be equal to or lesser than length of ciphertext
    int nLen = vchCiphertext.size();

    vchPlaintext.resize(nLen);

    AES256CBCDecrypt dec(vchKey.data(), vchIV.data(), true);
    nLen = dec.Decrypt(&vchCiphertext[0], vchCiphertext.size(), &vchPlaintext[0]);
    if(nLen == 0)
        return false;
    vchPlaintext.resize(nLen);
    return true;
}

static bool EncryptSecret(const CKeyingMaterial& vMasterKey, const CKeyingMaterial &vchPlaintext, const uint256& nIV, std::vector<unsigned char> &vchCiphertext)
{
    CCrypter cKeyCrypter;
    std::vector<unsigned char> chIV(WALLET_CRYPTO_IV_SIZE);
    memcpy(&chIV[0], &nIV, WALLET_CRYPTO_IV_SIZE);
    if(!cKeyCrypter.SetKey(vMasterKey, chIV))
        return false;
    return cKeyCrypter.Encrypt(*((const CKeyingMaterial*)&vchPlaintext), vchCiphertext);
}

static bool DecryptSecret(const CKeyingMaterial& vMasterKey, const std::vector<unsigned char>& vchCiphertext, const uint256& nIV, CKeyingMaterial& vchPlaintext)
{
    CCrypter cKeyCrypter;
    std::vector<unsigned char> chIV(WALLET_CRYPTO_IV_SIZE);
    memcpy(&chIV[0], &nIV, WALLET_CRYPTO_IV_SIZE);
    if(!cKeyCrypter.SetKey(vMasterKey, chIV))
        return false;
    return cKeyCrypter.Decrypt(vchCiphertext, *((CKeyingMaterial*)&vchPlaintext));
}

static bool DecryptKey(const CKeyingMaterial& vMasterKey, const std::vector<unsigned char>& vchCryptedSecret, const CPubKey& vchPubKey, CKey& key)
{
    CKeyingMaterial vchSecret;
    if(!DecryptSecret(vMasterKey, vchCryptedSecret, vchPubKey.GetHash(), vchSecret))
        return false;

    if (vchSecret.size() != 32)
        return false;

    key.Set(vchSecret.begin(), vchSecret.end(), vchPubKey.IsCompressed());
    return key.VerifyPubKey(vchPubKey);
}

bool CCryptoKeyStore::SetCrypted()
{
    LOCK(cs_KeyStore);
    if (fUseCrypto)
        return true;
    if (!mapKeys.empty())
        return false;
    fUseCrypto = true;
    return true;
}

bool CCryptoKeyStore::Lock()
{
    if (!SetCrypted())
        return false;

    {
        LOCK(cs_KeyStore);
        vMasterKey.clear();
    }

    NotifyStatusChanged(this);
    return true;
}

bool CCryptoKeyStore::Unlock(const CKeyingMaterial& vMasterKeyIn)
{
    {
        LOCK(cs_KeyStore);
        if (!SetCrypted())
            return false;

        bool keyPass = false;
        bool keyFail = false;
        CryptedKeyMap::const_iterator mi = mapCryptedKeys.begin();
        for (; mi != mapCryptedKeys.end(); ++mi)
        {
            const CPubKey &vchPubKey = (*mi).second.first;
            const std::vector<unsigned char> &vchCryptedSecret = (*mi).second.second;
            CKey key;
            if (!DecryptKey(vMasterKeyIn, vchCryptedSecret, vchPubKey, key))
            {
                keyFail = true;
                break;
            }
            keyPass = true;
            if (fDecryptionThoroughlyChecked)
                break;
        }
        if (keyPass && keyFail)
        {
            LogPrintf("The wallet is probably corrupted: Some keys decrypt but not all.\n");
            assert(false);
        }
        if (keyFail || !keyPass)
            return false;
        vMasterKey = vMasterKeyIn;
        fDecryptionThoroughlyChecked = true;
    }
    NotifyStatusChanged(this);
    return true;
}

bool CCryptoKeyStore::AddCryptedPaperKey(const CKeyingMaterial& vchCryptedPaperKey)
{
    {
        LOCK(cs_KeyStore);
        if (!SetCrypted())
            return false;

        this->vchCryptedPaperKey = vchCryptedPaperKey;
    }
    return true;
}

bool CCryptoKeyStore::AddPaperKey(const SecureString& paperKey)
{
    {
        LOCK(cs_KeyStore);
        if (!IsCrypted())
            return CBasicKeyStore::AddPaperKey(paperKey);

        if (IsLocked())
            return false;

        std::vector<unsigned char> vchOut;
        CKeyingMaterial vchPaperKey(paperKey.begin(), paperKey.end());
        bool result = EncryptSecret(vMasterKey, vchPaperKey, DoubleHashOfString("paperkey"), vchOut);

        if (result)
            result = AddCryptedPaperKey(CKeyingMaterial(vchOut.begin(), vchOut.end()));

        memory_cleanse(&vchOut[0], vchOut.size());
        vchOut.clear();

        return result;
    }
    return true;
}

bool CCryptoKeyStore::GetPaperKey(SecureString& paperKey) const
{
    {
        LOCK(cs_KeyStore);

        if (!this->paperKey.empty())
            return CBasicKeyStore::GetPaperKey(paperKey);

        if (!IsCrypted())
            return CBasicKeyStore::GetPaperKey(paperKey);

        std::vector<unsigned char> vchIn(vchCryptedPaperKey.begin(), vchCryptedPaperKey.end());
        CKeyingMaterial vchPaperKey;
        bool result = DecryptSecret(vMasterKey, vchIn, DoubleHashOfString("paperkey"), vchPaperKey);

        memory_cleanse(&vchIn[0], vchIn.size());
        vchIn.clear();

        if (result)
            paperKey = SecureString(vchPaperKey.begin(), vchPaperKey.end());

        return result;
    }
    return false;
}

void CCryptoKeyStore::DecryptPaperKey()
{
    GetPaperKey(this->paperKey);
}

bool CCryptoKeyStore::AddCryptedPinCode(const CKeyingMaterial& vchCryptedPinCode)
{
    {
        LOCK(cs_KeyStore);
        if (!SetCrypted())
            return false;

        this->vchCryptedPinCode = vchCryptedPinCode;
    }
    return true;
}

bool CCryptoKeyStore::AddPinCode(const SecureString& pinCode)
{
    {
        LOCK(cs_KeyStore);
        if (!IsCrypted())
            return CBasicKeyStore::AddPinCode(pinCode);

        if (IsLocked())
            return false;

        std::vector<unsigned char> vchOut;
        CKeyingMaterial vchPinCode(pinCode.begin(), pinCode.end());
        bool result = EncryptSecret(vMasterKey, vchPinCode, DoubleHashOfString("pincode"), vchOut);

        if (result)
            result = AddCryptedPinCode(CKeyingMaterial(vchOut.begin(), vchOut.end()));

        memory_cleanse(&vchOut[0], vchOut.size());
        vchOut.clear();

        return result;
    }
    return true;
}

bool CCryptoKeyStore::GetPinCode(SecureString& pinCode) const
{
    {
        LOCK(cs_KeyStore);

        if (!this->pinCode.empty())
            return CBasicKeyStore::GetPinCode(pinCode);

        if (!IsCrypted())
            return CBasicKeyStore::GetPinCode(pinCode);

        std::vector<unsigned char> vchIn(vchCryptedPinCode.begin(), vchCryptedPinCode.end());
        CKeyingMaterial vchPinCode;
        bool result = DecryptSecret(vMasterKey, vchIn, DoubleHashOfString("pincode"), vchPinCode);

        memory_cleanse(&vchIn[0], vchIn.size());
        vchIn.clear();

        if (result)
            pinCode = SecureString(vchPinCode.begin(), vchPinCode.end());

        return result;
    }
    return false;
}

void CCryptoKeyStore::DecryptPinCode()
{
    GetPinCode(this->pinCode);
}

bool CCryptoKeyStore::AddKeyPubKey(const CKey& key, const CPubKey &pubkey)
{
    {
        LOCK(cs_KeyStore);
        if (!IsCrypted())
            return CBasicKeyStore::AddKeyPubKey(key, pubkey);

        if (IsLocked())
            return false;

        std::vector<unsigned char> vchCryptedSecret;
        CKeyingMaterial vchSecret(key.begin(), key.end());
        if (!EncryptSecret(vMasterKey, vchSecret, pubkey.GetHash(), vchCryptedSecret))
            return false;

        if (!AddCryptedKey(pubkey, vchCryptedSecret))
            return false;
    }
    return true;
}


bool CCryptoKeyStore::AddCryptedKey(const CPubKey &vchPubKey, const std::vector<unsigned char> &vchCryptedSecret)
{
    {
        LOCK(cs_KeyStore);
        if (!SetCrypted())
            return false;

        mapCryptedKeys[vchPubKey.GetID()] = make_pair(vchPubKey, vchCryptedSecret);
    }
    return true;
}

bool CCryptoKeyStore::GetKey(const CKeyID &address, CKey& keyOut) const
{
    {
        LOCK(cs_KeyStore);
        if (!IsCrypted())
            return CBasicKeyStore::GetKey(address, keyOut);

        CryptedKeyMap::const_iterator mi = mapCryptedKeys.find(address);
        if (mi != mapCryptedKeys.end())
        {
            const CPubKey &vchPubKey = (*mi).second.first;
            const std::vector<unsigned char> &vchCryptedSecret = (*mi).second.second;
            return DecryptKey(vMasterKey, vchCryptedSecret, vchPubKey, keyOut);
        }
    }
    return false;
}

bool CCryptoKeyStore::GetPubKey(const CKeyID &address, CPubKey& vchPubKeyOut) const
{
    {
        LOCK(cs_KeyStore);
        if (!IsCrypted())
            return CBasicKeyStore::GetPubKey(address, vchPubKeyOut);

        CryptedKeyMap::const_iterator mi = mapCryptedKeys.find(address);
        if (mi != mapCryptedKeys.end())
        {
            vchPubKeyOut = (*mi).second.first;
            return true;
        }
        // Check for watch-only pubkeys
        return CBasicKeyStore::GetPubKey(address, vchPubKeyOut);
    }
}

bool CCryptoKeyStore::EncryptKeys(CKeyingMaterial& vMasterKeyIn)
{
    {
        LOCK(cs_KeyStore);
        if (!mapCryptedKeys.empty() || IsCrypted())
            return false;

        fUseCrypto = true;
        for (KeyMap::value_type& mKey : mapKeys)
        {
            const CKey &key = mKey.second;
            CPubKey vchPubKey = key.GetPubKey();
            CKeyingMaterial vchSecret(key.begin(), key.end());
            std::vector<unsigned char> vchCryptedSecret;
            if (!EncryptSecret(vMasterKeyIn, vchSecret, vchPubKey.GetHash(), vchCryptedSecret))
                return false;
            if (!AddCryptedKey(vchPubKey, vchCryptedSecret))
                return false;
        }
        mapKeys.clear();
    }
    return true;
}

bool CCryptoKeyStore::GetCryptedPaperKey(CKeyingMaterial& vchCryptedPaperKey)
{
    {
        LOCK(cs_KeyStore);

        if (IsLocked())
            return false;

        if (this->vchCryptedPaperKey.size() == 0)
            return false;

        vchCryptedPaperKey = this->vchCryptedPaperKey;
    }
    return true;
}

bool CCryptoKeyStore::EncryptPaperKey(CKeyingMaterial& vMasterKeyIn)
{
    {
        LOCK(cs_KeyStore);

        if (IsLocked())
            return false;

        SecureString paperKey;
        if (!GetPaperKey(paperKey)) {
            return false;
        }

        std::vector<unsigned char> vchOut;
        CKeyingMaterial vchPaperKey(paperKey.begin(), paperKey.end());
        bool result = EncryptSecret(vMasterKeyIn, vchPaperKey, DoubleHashOfString("paperkey"), vchOut);

        if (result)
        {
            this->vchCryptedPaperKey = CKeyingMaterial(vchOut.begin(), vchOut.end());

            memory_cleanse(&(this->paperKey[0]), this->paperKey.size());
            this->paperKey = "";
        }

        memory_cleanse(&vchOut[0], vchOut.size());
        vchOut.clear();

        return result;
    }
    return true;
}

bool CCryptoKeyStore::GetCryptedPinCode(CKeyingMaterial& vchCryptedPinCode)
{
    {
        LOCK(cs_KeyStore);

        if (IsLocked())
            return false;

        if (this->vchCryptedPinCode.size() == 0)
            return false;

        vchCryptedPinCode = this->vchCryptedPinCode;
    }
    return true;
}

bool CCryptoKeyStore::EncryptPinCode(CKeyingMaterial& vMasterKeyIn)
{
    {
        LOCK(cs_KeyStore);

        if (IsLocked())
            return false;

        SecureString pinCode;
        if (!GetPinCode(pinCode)) {
            return false;
        }

        std::vector<unsigned char> vchOut;
        CKeyingMaterial vchPinCode(pinCode.begin(), pinCode.end());
        bool result = EncryptSecret(vMasterKeyIn, vchPinCode, DoubleHashOfString("pincode"), vchOut);

        if (result)
        {
            this->vchCryptedPinCode = CKeyingMaterial(vchOut.begin(), vchOut.end());

            memory_cleanse(&(this->pinCode[0]), this->pinCode.size());
            this->pinCode = "";
        }

        memory_cleanse(&vchOut[0], vchOut.size());
        vchOut.clear();

        return result;
    }
    return true;
}

uint256 CCryptoKeyStore::DoubleHashOfString(const std::string& str) const
{
    if (str.length() == 0)
        return uint256();

    CSHA256 h1;
    unsigned char h1hash[256 / 8];
    memset(h1hash, 0, sizeof(h1hash));
    h1.Write((const unsigned char *)str.c_str(), str.length());
    h1.Finalize(h1hash);

    CSHA256 h2;
    unsigned char h2hash[256 / 8];
    memset(h2hash, 0, sizeof(h2hash));
    h2.Write(h1hash, sizeof(h1hash));
    h2.Finalize(h2hash);

    uint256 result(std::vector<unsigned char>(h2hash, h2hash + 32));

    return result;
}
