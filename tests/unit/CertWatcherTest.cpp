#include "server/CertWatcher.h"

#include <catch2/catch_test_macros.hpp>

#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>

#include <cstdio>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;
using wikicore::server::CertWatcher;

namespace {

class TempDir {
 public:
  TempDir()
      : path_(fs::temp_directory_path() /
              fs::path("wiki-certwatcher-test-" +
                        std::to_string(reinterpret_cast<std::uintptr_t>(this)))) {
    fs::create_directories(path_);
  }
  ~TempDir() { fs::remove_all(path_); }
  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;
  const fs::path& path() const { return path_; }

 private:
  fs::path path_;
};

// Generates a throwaway self-signed cert/key pair at the given paths, via
// direct OpenSSL calls (no `openssl` CLI dependency) — fresh per test run
// rather than a committed fixture, so there's nothing to expire years from
// now and silently break CI.
void writeSelfSignedCert(const fs::path& certPath, const fs::path& keyPath) {
  EVP_PKEY* pkey = EVP_RSA_gen(2048);
  REQUIRE(pkey != nullptr);

  X509* cert = X509_new();
  REQUIRE(cert != nullptr);
  X509_set_version(cert, 2);
  ASN1_INTEGER_set(X509_get_serialNumber(cert), 1);
  X509_gmtime_adj(X509_getm_notBefore(cert), 0);
  X509_gmtime_adj(X509_getm_notAfter(cert), 3600);  // 1 hour -- test-only
  X509_set_pubkey(cert, pkey);

  X509_NAME* name = X509_get_subject_name(cert);
  X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                              reinterpret_cast<const unsigned char*>("wiki-test"), -1, -1, 0);
  X509_set_issuer_name(cert, name);
  REQUIRE(X509_sign(cert, pkey, EVP_sha256()) > 0);

  FILE* certFile = fopen(certPath.c_str(), "w");
  REQUIRE(certFile != nullptr);
  PEM_write_X509(certFile, cert);
  fclose(certFile);

  FILE* keyFile = fopen(keyPath.c_str(), "w");
  REQUIRE(keyFile != nullptr);
  PEM_write_PrivateKey(keyFile, pkey, nullptr, nullptr, 0, nullptr, nullptr);
  fclose(keyFile);

  X509_free(cert);
  EVP_PKEY_free(pkey);
}

}  // namespace

TEST_CASE("CertWatcher::filesLookValid: a real self-signed cert/key pair "
          "validates",
          "[CertWatcher]") {
  TempDir dir;
  const fs::path cert = dir.path() / "fullchain.pem";
  const fs::path key = dir.path() / "privkey.pem";
  writeSelfSignedCert(cert, key);
  REQUIRE(CertWatcher::filesLookValid(cert, key));
}

TEST_CASE("CertWatcher::filesLookValid: a missing cert file is rejected",
          "[CertWatcher]") {
  TempDir dir;
  const fs::path cert = dir.path() / "fullchain.pem";
  const fs::path key = dir.path() / "privkey.pem";
  writeSelfSignedCert(cert, key);
  fs::remove(cert);
  REQUIRE_FALSE(CertWatcher::filesLookValid(cert, key));
}

TEST_CASE("CertWatcher::filesLookValid: an empty cert file is rejected",
          "[CertWatcher]") {
  TempDir dir;
  const fs::path cert = dir.path() / "fullchain.pem";
  const fs::path key = dir.path() / "privkey.pem";
  writeSelfSignedCert(cert, key);
  std::ofstream(cert, std::ios::trunc).close();
  REQUIRE_FALSE(CertWatcher::filesLookValid(cert, key));
}

TEST_CASE("CertWatcher::filesLookValid: garbage bytes instead of PEM are "
          "rejected",
          "[CertWatcher]") {
  TempDir dir;
  const fs::path cert = dir.path() / "fullchain.pem";
  const fs::path key = dir.path() / "privkey.pem";
  writeSelfSignedCert(cert, key);
  std::ofstream(cert, std::ios::trunc) << "not a certificate, just noise";
  REQUIRE_FALSE(CertWatcher::filesLookValid(cert, key));
}

TEST_CASE("CertWatcher::filesLookValid: a valid cert paired with a garbage "
          "key is rejected",
          "[CertWatcher]") {
  TempDir dir;
  const fs::path cert = dir.path() / "fullchain.pem";
  const fs::path key = dir.path() / "privkey.pem";
  writeSelfSignedCert(cert, key);
  std::ofstream(key, std::ios::trunc) << "not a private key";
  REQUIRE_FALSE(CertWatcher::filesLookValid(cert, key));
}

TEST_CASE("CertWatcher::filesLookValid: a valid cert paired with a "
          "DIFFERENT valid key (mismatched pair) is rejected",
          "[CertWatcher]") {
  // Each file individually parses as perfectly good PEM -- only the
  // cert/key correspondence check catches this. Regression test for
  // the exact crash trantor's SSL_CTX_check_private_key() would hit on
  // reloadSSLFiles() if this watcher let a mismatched pair through (see
  // CertWatcher.h's own comment on why this check exists at all).
  TempDir dir;
  const fs::path cert = dir.path() / "fullchain.pem";
  const fs::path key = dir.path() / "privkey.pem";
  const fs::path otherCert = dir.path() / "other-fullchain.pem";
  const fs::path otherKey = dir.path() / "other-privkey.pem";
  writeSelfSignedCert(cert, key);
  writeSelfSignedCert(otherCert, otherKey);

  REQUIRE_FALSE(CertWatcher::filesLookValid(cert, otherKey));
}
