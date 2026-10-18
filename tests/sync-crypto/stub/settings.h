#pragma once

// Stub of the application settings for the standalone crypto test: the only
// thing the crypto layer needs from it is the working directory, which the test
// drives through an environment variable so one process can act as two devices.
#include <QtCore/QString>
#include <QtCore/QByteArray>

#include <cstdlib>

inline QString cWorkingDir() {
	const auto value = std::getenv("TF_TEST_DIR");
	return value ? QString::fromUtf8(value) : QStringLiteral(".");
}

inline QString cExeDir() {
	return cWorkingDir();
}
