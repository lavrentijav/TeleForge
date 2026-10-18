// Standalone proof of the archive key model.
//
// The passphrase checks run in one process: the passphrase key depends only on
// the passphrase and the account binding, so a second "device" is simply a
// second derivation.
//
// The device-bound checks need two processes, because the device secret is
// cached for the lifetime of a process: `device-write` runs with one working
// directory and saves a bundle, `device-read` runs with another and must fail
// to open it.

#include "tfc/features/sync/teleforge_sync_crypto.h"

#include <QtCore/QByteArray>
#include <QtCore/QFile>

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>

namespace {

int g_failures = 0;

void Check(bool ok, const std::string &what) {
	std::cout << (ok ? "  ok   " : "  FAIL ") << what << "\n";
	if (!ok) {
		++g_failures;
	}
}

void UseDevice(const char *dir) {
	setenv("TF_TEST_DIR", dir, 1);
}

const auto kBundlePath = "/tmp/tf-device-bundle.bin";
const auto kBinding = QByteArray("286834+79990000000");
const auto kPlain = QByteArray("telegram message text that must stay private");

int DeviceWrite() {
	using namespace TeleForge::Sync;
	UseDevice("/tmp/tf-device-a");
	SetDefaultKeyMode(KeyMode::DeviceV2, {});
	const auto key = DeriveSyncKey(DeviceSecret(), {}, kBinding);
	const auto bundle = EncryptBundleForWrite(kPlain, key);
	Check(!bundle.isEmpty(), "device A wrote a bundle");
	Check(bundle.startsWith(QByteArray("TFSYNC1\0", 8)), "device bundle keeps the v1 header");
	auto file = QFile(kBundlePath);
	if (!file.open(QIODevice::WriteOnly)) {
		Check(false, "bundle saved to disk");
		return 1;
	}
	file.write(bundle);
	file.close();
	Check(true, "bundle saved to disk");
	return g_failures ? 1 : 0;
}

int DeviceRead() {
	using namespace TeleForge::Sync;
	UseDevice("/tmp/tf-device-b");
	auto file = QFile(kBundlePath);
	if (!file.open(QIODevice::ReadOnly)) {
		Check(false, "bundle loaded from disk");
		return 1;
	}
	const auto bundle = file.readAll();
	file.close();
	Check(!bundle.isEmpty(), "bundle loaded from disk");

	const auto key = DeriveSyncKey(DeviceSecret(), {}, kBinding);
	const auto opened = DecryptBundle(bundle, key);
	Check(!opened.has_value(), "device-bound bundle does not open on another machine");
	if (opened) {
		std::cout << "       (decrypted bytes: " << opened->size() << ")\n";
	}
	return g_failures ? 1 : 0;
}

int PassphraseChecks() {
	using namespace TeleForge::Sync;

	const auto passphrase = QByteArray("correct horse battery staple");

	UseDevice("/tmp/tf-device-a");
	const auto keyA = DeriveAccountKey(passphrase, kBinding);
	Check(keyA.size() == 32, "passphrase key is 32 bytes");

	const auto salt = AccountSalt(kBinding);
	SetDefaultKeyMode(KeyMode::AccountArgon2id, salt);
	const auto bundle = EncryptBundleForWrite(kPlain, keyA);
	Check(!bundle.isEmpty(), "bundle written");
	Check(bundle.startsWith(QByteArray("TFSYNC2\0", 8)), "bundle carries the v2 header");

	KeyMode mode = KeyMode::DeviceV2;
	QByteArray headerSalt;
	Check(ReadBundleParams(bundle, &mode, &headerSalt), "header is readable");
	Check(mode == KeyMode::AccountArgon2id, "header records the account mode");
	Check(headerSalt == salt, "header salt matches the account salt");

	UseDevice("/tmp/tf-device-b");
	const auto keyB = DeriveAccountKey(passphrase, kBinding);
	Check(keyA == keyB, "both devices derive the same passphrase key");

	if (const auto opened = DecryptBundle(bundle, keyB)) {
		Check(*opened == kPlain, "device B decrypts what device A wrote");
	} else {
		Check(false, "device B decrypts what device A wrote");
	}

	const auto wrong = DeriveAccountKey(QByteArray("wrong passphrase"), kBinding);
	Check(wrong != keyA, "a different passphrase gives a different key");
	Check(!DecryptBundle(bundle, wrong).has_value(), "wrong passphrase cannot decrypt");
	return g_failures ? 1 : 0;
}

} // namespace

int main(int argc, char **argv) {
	const auto mode = (argc > 1) ? std::string(argv[1]) : std::string();
	if (mode == "device-write") {
		return DeviceWrite();
	} else if (mode == "device-read") {
		return DeviceRead();
	}
	return PassphraseChecks();
}
