#include "ayu/features/sync/teleforge_sync_crypto.h"

#include "base/openssl_help.h"
#include "logs.h"
#include "settings.h"

#include <openssl/evp.h>
#include <openssl/rand.h>

#include <QtCore/QCryptographicHash>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QHash>
#include <QtCore/QMessageAuthenticationCode>

#include <algorithm>
#include <cstdint>
#include <mutex>
#include <utility>

#ifdef Q_OS_WIN
#include <windows.h>
#endif // Q_OS_WIN

namespace TeleForge::Sync {
namespace {

const auto kMagic = QByteArray("TFSYNC1\0", 8);
constexpr auto kNonceSize = 12;
constexpr auto kTagSize = 16;
constexpr auto kKeySize = 32;
constexpr auto kSaltSize = 16;
constexpr auto kSecretSize = 32;
constexpr auto kPbkdf2Iterations = 300000;
const auto kFieldPrefix = QStringLiteral("tfs1:");
const auto kSecretMagic = QByteArray("TFDK1", 5);
const auto kStateFileName = QStringLiteral("teleforge_sync_keystate");
const auto kSecretFileName = QStringLiteral("teleforge_device_secret");

auto g_mutex = std::mutex();
auto g_legacyKey = QByteArray();
auto g_migrationComplete = false;
auto g_secret = QByteArray();
auto g_secretLoaded = false;
auto g_stateLoaded = false;
auto g_stateSalt = QByteArray();
auto g_keyCache = QHash<QByteArray, QByteArray>();

[[nodiscard]] QByteArray HkdfSha256(
		const QByteArray &ikm,
		const QByteArray &salt,
		const QByteArray &info) {
	auto data = salt + ikm + info;
	return QCryptographicHash::hash(data, QCryptographicHash::Sha256);
}

[[nodiscard]] QByteArray Pbkdf2Sha256(
		const QByteArray &password,
		const QByteArray &salt,
		int iterations,
		int length) {
	auto result = QByteArray(length, Qt::Uninitialized);
	const auto ok = PKCS5_PBKDF2_HMAC(
		password.constData(),
		password.size(),
		reinterpret_cast<const unsigned char*>(salt.constData()),
		salt.size(),
		iterations,
		EVP_sha256(),
		length,
		reinterpret_cast<unsigned char*>(result.data()));
	return (ok == 1) ? result : QByteArray();
}

[[nodiscard]] QString TeleforgeDir() {
	const auto base = cWorkingDir().isEmpty()
		? QStringLiteral(".")
		: cWorkingDir();
	const auto path = QDir::cleanPath(base + QStringLiteral("/tdata"));
	QDir().mkpath(path);
	return path + u'/';
}

[[nodiscard]] QByteArray ReadWholeFile(const QString &path) {
	auto file = QFile(path);
	if (!file.open(QIODevice::ReadOnly)) {
		return {};
	}
	return file.readAll();
}

void WriteWholeFile(const QString &path, const QByteArray &data) {
	auto file = QFile(path);
	if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
		LOG(("TeleForge crypto: cannot write %1").arg(path));
		return;
	}
	file.write(data);
	file.close();
	file.setPermissions(
		QFileDevice::ReadOwner | QFileDevice::WriteOwner);
}

#ifdef Q_OS_WIN
struct DpapiBlob {
	unsigned long cbData = 0;
	unsigned char *pbData = nullptr;
};

using DpapiProtectFn = int (WINAPI*)(
	DpapiBlob*,
	const wchar_t*,
	DpapiBlob*,
	void*,
	void*,
	unsigned long,
	DpapiBlob*);
using DpapiUnprotectFn = int (WINAPI*)(
	DpapiBlob*,
	wchar_t**,
	DpapiBlob*,
	void*,
	void*,
	unsigned long,
	DpapiBlob*);

constexpr auto kDpapiUiForbidden = 0x1ul;

[[nodiscard]] void *DpapiProc(const char *name) {
	static auto module = LoadLibraryW(L"crypt32.dll");
	return module ? reinterpret_cast<void*>(GetProcAddress(module, name)) : nullptr;
}

[[nodiscard]] QByteArray DpapiProtect(const QByteArray &plain) {
	const auto fn = reinterpret_cast<DpapiProtectFn>(DpapiProc("CryptProtectData"));
	if (!fn || plain.isEmpty()) {
		return {};
	}
	auto input = plain;
	auto in = DpapiBlob{
		.cbData = static_cast<unsigned long>(input.size()),
		.pbData = reinterpret_cast<unsigned char*>(input.data()),
	};
	auto out = DpapiBlob();
	if (!fn(
			&in,
			L"TeleForge device secret",
			nullptr,
			nullptr,
			nullptr,
			kDpapiUiForbidden,
			&out)) {
		return {};
	}
	auto result = QByteArray(
		reinterpret_cast<const char*>(out.pbData),
		static_cast<int>(out.cbData));
	LocalFree(out.pbData);
	return result;
}

[[nodiscard]] QByteArray DpapiUnprotect(const QByteArray &wrapped) {
	const auto fn = reinterpret_cast<DpapiUnprotectFn>(
		DpapiProc("CryptUnprotectData"));
	if (!fn || wrapped.isEmpty()) {
		return {};
	}
	auto input = wrapped;
	auto in = DpapiBlob{
		.cbData = static_cast<unsigned long>(input.size()),
		.pbData = reinterpret_cast<unsigned char*>(input.data()),
	};
	auto out = DpapiBlob();
	if (!fn(
			&in,
			nullptr,
			nullptr,
			nullptr,
			nullptr,
			kDpapiUiForbidden,
			&out)) {
		return {};
	}
	auto result = QByteArray(
		reinterpret_cast<const char*>(out.pbData),
		static_cast<int>(out.cbData));
	LocalFree(out.pbData);
	return result;
}
#endif // Q_OS_WIN

[[nodiscard]] QByteArray WrapSecret(const QByteArray &raw) {
#ifdef Q_OS_WIN
	if (const auto wrapped = DpapiProtect(raw); !wrapped.isEmpty()) {
		return kSecretMagic + char(1) + wrapped;
	}
#endif // Q_OS_WIN
	return kSecretMagic + char(0) + raw;
}

[[nodiscard]] QByteArray UnwrapSecret(const QByteArray &stored) {
	if (!stored.startsWith(kSecretMagic) || stored.size() <= kSecretMagic.size()) {
		return {};
	}
	const auto kind = stored.at(kSecretMagic.size());
	const auto payload = stored.mid(kSecretMagic.size() + 1);
#ifdef Q_OS_WIN
	if (kind == 1) {
		return DpapiUnprotect(payload);
	}
#endif // Q_OS_WIN
	return (kind == 0) ? payload : QByteArray();
}

void LoadKeyStateLocked() {
	if (g_stateLoaded) {
		return;
	}
	g_stateLoaded = true;
	auto salt = QByteArray();
	auto complete = false;
	const auto raw = ReadWholeFile(TeleforgeDir() + kStateFileName);
	if (!raw.isEmpty()) {
		for (const auto &line : QString::fromLatin1(raw).split(u'\n')) {
			const auto index = line.indexOf(u'=');
			if (index <= 0) {
				continue;
			}
			const auto key = line.left(index);
			const auto value = line.mid(index + 1).trimmed();
			if (key == u"salt"_q) {
				salt = QByteArray::fromHex(value.toLatin1());
			} else if (key == u"migrated"_q) {
				complete = (value == u"1"_q);
			}
		}
	}
	auto fresh = false;
	if (salt.size() != kSaltSize) {
		salt = QByteArray(kSaltSize, Qt::Uninitialized);
		RAND_bytes(
			reinterpret_cast<unsigned char*>(salt.data()),
			kSaltSize);
		fresh = true;
	}
	g_stateSalt = salt;
	g_migrationComplete = complete;
	if (fresh) {
		WriteWholeFile(
			TeleforgeDir() + kStateFileName,
			"salt=" + salt.toHex() + "\nmigrated="
				+ (complete ? "1" : "0") + "\n");
	}
}

void SaveMigrationStateLocked() {
	g_stateLoaded = true;
	WriteWholeFile(
		TeleforgeDir() + kStateFileName,
		"salt=" + g_stateSalt.toHex() + "\nmigrated="
			+ (g_migrationComplete ? "1" : "0") + "\n");
}

[[nodiscard]] QByteArray DeviceSecretLocked() {
	if (!g_secretLoaded) {
		g_secretLoaded = true;
		const auto path = TeleforgeDir() + kSecretFileName;
		auto secret = UnwrapSecret(ReadWholeFile(path));
		if (secret.size() != kSecretSize) {
			if (!secret.isEmpty()) {
				LOG(("TeleForge crypto: device secret unusable, regenerating "
					"(existing sync data will not decrypt)"));
			}
			secret = QByteArray(kSecretSize, Qt::Uninitialized);
			RAND_bytes(
				reinterpret_cast<unsigned char*>(secret.data()),
				kSecretSize);
			WriteWholeFile(path, WrapSecret(secret));
		}
		g_secret = secret;
	}
	return g_secret;
}

} // namespace

QByteArray DeriveLegacySyncKey(const QByteArray &masterMaterial) {
	return HkdfSha256(
		masterMaterial,
		QByteArray("TeleForge-Sync-v1"),
		QByteArray("account"));
}

QByteArray DeriveSyncKey(
		const QByteArray &secret,
		const QByteArray &optionalExportKey,
		const QByteArray &accountBinding) {
	if (secret.size() != kKeySize) {
		return {};
	}
	auto password = secret;
	if (!optionalExportKey.isEmpty()) {
		auto mac = QMessageAuthenticationCode(
			QCryptographicHash::Sha256,
			secret);
		mac.addData(optionalExportKey);
		password = mac.result();
	}
	const auto lock = std::unique_lock(g_mutex);
	LoadKeyStateLocked();
	auto cacheKey = QCryptographicHash::hash(
		password + g_stateSalt + accountBinding,
		QCryptographicHash::Sha256);
	if (const auto cached = g_keyCache.constFind(cacheKey);
		cached != g_keyCache.cend() && cached->size() == kKeySize) {
		return *cached;
	}
	const auto key = Pbkdf2Sha256(
		password,
		g_stateSalt + accountBinding,
		kPbkdf2Iterations,
		kKeySize);
	if (key.isEmpty()) {
		return {};
	}
	if (g_keyCache.size() > 4) {
		g_keyCache.clear();
	}
	g_keyCache.insert(cacheKey, key);
	return key;
}

QByteArray DeviceSecret() {
	const auto lock = std::unique_lock(g_mutex);
	return DeviceSecretLocked();
}

void SetLegacySyncKey(const QByteArray &key) {
	const auto lock = std::unique_lock(g_mutex);
	g_legacyKey = key;
}

void MarkSyncMigrationComplete() {
	{
		const auto lock = std::unique_lock(g_mutex);
		if (g_migrationComplete) {
			return;
		}
		LoadKeyStateLocked();
		g_migrationComplete = true;
		SaveMigrationStateLocked();
	}
}

bool SyncMigrationComplete() {
	const auto lock = std::unique_lock(g_mutex);
	LoadKeyStateLocked();
	return g_migrationComplete;
}

QByteArray FieldCryptKey() {
	const auto secret = DeviceSecret();
	auto mac = QMessageAuthenticationCode(QCryptographicHash::Sha256, secret);
	mac.addData(QByteArrayLiteral("TeleForge-Field-v1"));
	return mac.result();
}

QString ProtectSecret(const QString &plain) {
	if (plain.isEmpty()) {
		return QString();
	}
	const auto encrypted = EncryptBundle(plain.toUtf8(), FieldCryptKey());
	if (encrypted.isEmpty()) {
		LOG(("TeleForge crypto: field encryption unavailable"));
		return plain;
	}
	return kFieldPrefix + QString::fromLatin1(encrypted.toBase64());
}

bool IsProtectedSecret(const QString &stored) {
	return stored.startsWith(kFieldPrefix);
}

QString UnprotectSecret(const QString &stored) {
	if (!IsProtectedSecret(stored)) {
		return stored;
	}
	const auto raw = QByteArray::fromBase64(
		stored.mid(kFieldPrefix.size()).toLatin1());
	const auto plain = DecryptBundle(raw, FieldCryptKey());
	if (!plain) {
		LOG(("TeleForge crypto: stored secret cannot be decrypted"));
		return QString();
	}
	return QString::fromUtf8(*plain);
}

QByteArray EncryptBundle(const QByteArray &plain, const QByteArray &key) {
	if (key.size() != kKeySize) {
		return {};
	}

	auto nonce = QByteArray(kNonceSize, Qt::Uninitialized);
	RAND_bytes(reinterpret_cast<unsigned char*>(nonce.data()), kNonceSize);

	auto ctx = EVP_CIPHER_CTX_new();
	if (!ctx) {
		return {};
	}
	auto out = QByteArray();
	out.reserve(kMagic.size() + kNonceSize + plain.size() + kTagSize + 16);
	out.append(kMagic);
	out.append(nonce);

	auto ciphertext = QByteArray(plain.size() + 16, Qt::Uninitialized);
	auto tag = QByteArray(kTagSize, Qt::Uninitialized);
	int len = 0;
	int total = 0;

	if (EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1
		|| EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, kNonceSize, nullptr) != 1
		|| EVP_EncryptInit_ex(
			ctx,
			nullptr,
			nullptr,
			reinterpret_cast<const unsigned char*>(key.constData()),
			reinterpret_cast<const unsigned char*>(nonce.constData())) != 1
		|| EVP_EncryptUpdate(
			ctx,
			reinterpret_cast<unsigned char*>(ciphertext.data()),
			&len,
			reinterpret_cast<const unsigned char*>(plain.constData()),
			plain.size()) != 1) {
		EVP_CIPHER_CTX_free(ctx);
		return {};
	}
	total = len;
	if (EVP_EncryptFinal_ex(
			ctx,
			reinterpret_cast<unsigned char*>(ciphertext.data()) + len,
			&len) != 1) {
		EVP_CIPHER_CTX_free(ctx);
		return {};
	}
	total += len;
	if (EVP_CIPHER_CTX_ctrl(
			ctx,
			EVP_CTRL_GCM_GET_TAG,
			kTagSize,
			tag.data()) != 1) {
		EVP_CIPHER_CTX_free(ctx);
		return {};
	}
	EVP_CIPHER_CTX_free(ctx);

	ciphertext.resize(total);
	out.append(ciphertext);
	out.append(tag);
	return out;
}

namespace {

[[nodiscard]] std::optional<QByteArray> DecryptBundleWithKey(
		const QByteArray &encrypted,
		const QByteArray &key) {
	if (encrypted.size() < kMagic.size() + kNonceSize + kTagSize + 1) {
		return std::nullopt;
	}
	if (!encrypted.startsWith(kMagic)) {
		return std::nullopt;
	}
	if (key.size() != kKeySize) {
		return std::nullopt;
	}
	const auto nonce = encrypted.mid(kMagic.size(), kNonceSize);
	const auto tag = encrypted.right(kTagSize);
	const auto ciphertext = encrypted.mid(
		kMagic.size() + kNonceSize,
		encrypted.size() - kMagic.size() - kNonceSize - kTagSize);

	auto ctx = EVP_CIPHER_CTX_new();
	if (!ctx) {
		return std::nullopt;
	}
	auto plain = QByteArray(ciphertext.size(), Qt::Uninitialized);
	int len = 0;
	int total = 0;

	if (EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1
		|| EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, kNonceSize, nullptr) != 1
		|| EVP_DecryptInit_ex(
			ctx,
			nullptr,
			nullptr,
			reinterpret_cast<const unsigned char*>(key.constData()),
			reinterpret_cast<const unsigned char*>(nonce.constData())) != 1
		|| EVP_DecryptUpdate(
			ctx,
			reinterpret_cast<unsigned char*>(plain.data()),
			&len,
			reinterpret_cast<const unsigned char*>(ciphertext.constData()),
			ciphertext.size()) != 1) {
		EVP_CIPHER_CTX_free(ctx);
		return std::nullopt;
	}
	total = len;
	if (EVP_CIPHER_CTX_ctrl(
			ctx,
			EVP_CTRL_GCM_SET_TAG,
			kTagSize,
			const_cast<char*>(tag.data())) != 1
		|| EVP_DecryptFinal_ex(
			ctx,
			reinterpret_cast<unsigned char*>(plain.data()) + len,
			&len) != 1) {
		EVP_CIPHER_CTX_free(ctx);
		return std::nullopt;
	}
	total += len;
	EVP_CIPHER_CTX_free(ctx);
	plain.resize(total);
	return plain;
}

} // namespace

std::optional<QByteArray> DecryptBundle(
		const QByteArray &encrypted,
		const QByteArray &key) {
	if (const auto plain = DecryptBundleWithKey(encrypted, key)) {
		return plain;
	}
	auto legacy = QByteArray();
	{
		const auto lock = std::unique_lock(g_mutex);
		LoadKeyStateLocked();
		if (g_migrationComplete || g_legacyKey.isEmpty()) {
			return std::nullopt;
		}
		legacy = g_legacyKey;
	}
	if (legacy == key) {
		return std::nullopt;
	}
	const auto plain = DecryptBundleWithKey(encrypted, legacy);
	if (plain) {
		LOG(("TeleForge sync: read a pre-v2 bundle, re-encrypting on next upload"));
		MarkSyncMigrationComplete();
	}
	return plain;
}

QString ExportKeyBase64(const QByteArray &key) {
	return QString::fromLatin1(key.toBase64());
}

std::optional<QByteArray> ImportKeyBase64(const QString &encoded) {
	const auto bytes = QByteArray::fromBase64(encoded.trimmed().toLatin1());
	if (bytes.size() != kKeySize) {
		return std::nullopt;
	}
	return bytes;
}

QString HmacTag(const QByteArray &data, const QByteArray &key) {
	auto mac = QMessageAuthenticationCode(QCryptographicHash::Sha256, key);
	mac.addData(data);
	return QString::fromLatin1(mac.result().toHex().left(32));
}

QString HmacId(long long id, const QByteArray &key) {
	return HmacTag(QByteArray::number(qlonglong(id)), key);
}

std::vector<float> ProjectEmbedding(
		const std::vector<float> &vector,
		const QByteArray &key) {
	const auto n = int(vector.size());
	if (n <= 0) {
		return {};
	}

	// Deterministic keyed byte stream (HMAC-SHA256 in counter mode).
	auto stream = QByteArray();
	const auto need = n * 8 + 64;
	auto counter = 0;
	while (stream.size() < need) {
		auto mac = QMessageAuthenticationCode(QCryptographicHash::Sha256, key);
		mac.addData(QByteArrayLiteral("tf-emb-proj-v1"));
		mac.addData(QByteArray::number(counter++));
		stream.append(mac.result());
	}
	const auto bytes = reinterpret_cast<const std::uint8_t*>(stream.constData());
	auto pos = 0;
	const auto nextU32 = [&]() -> std::uint32_t {
		const auto b0 = std::uint32_t(bytes[pos++]);
		const auto b1 = std::uint32_t(bytes[pos++]);
		const auto b2 = std::uint32_t(bytes[pos++]);
		const auto b3 = std::uint32_t(bytes[pos++]);
		return (b0 << 24) | (b1 << 16) | (b2 << 8) | b3;
	};

	// Fisher-Yates permutation of component indices.
	auto perm = std::vector<int>(n);
	for (auto i = 0; i != n; ++i) {
		perm[i] = i;
	}
	for (auto i = n - 1; i > 0; --i) {
		const auto j = int(nextU32() % std::uint32_t(i + 1));
		std::swap(perm[i], perm[j]);
	}

	// Apply the permutation plus a keyed sign flip per output position. The
	// result is an orthogonal transform of the input, so cosine/L2 are kept.
	auto result = std::vector<float>(n, 0.f);
	for (auto i = 0; i != n; ++i) {
		const auto sign = (bytes[pos++] & 1u) ? -1.f : 1.f;
		result[i] = sign * vector[perm[i]];
	}
	return result;
}

} // namespace TeleForge::Sync
