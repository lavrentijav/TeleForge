#include "ayu/features/plugins/plugin_runner.h"

#include "ayu/features/plugins/plugin_api.h"
#include "logs.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>

#ifdef TELEFORGE_WITH_PYTHON
#include <Python.h>
#ifdef Q_OS_WIN
#include <windows.h>
#endif
#endif

namespace TeleForge::Plugins {
namespace {

auto g_tools = QJsonArray();

#ifdef TELEFORGE_WITH_PYTHON
auto g_initialized = false;

[[nodiscard]] std::wstring ToWide(const QString &value) {
	return value.toStdWString();
}

// Initializes the embedded interpreter, preferring the self-contained Python
// bundled next to the executable (exe_dir/modules) so plugins do not depend on
// a system Python installation.
void EnsurePython() {
	if (g_initialized) {
		return;
	}
	g_initialized = true;

#ifdef Q_OS_WIN
	// The executable is hardened with DependentLoadFlag=0x800 (System32-only
	// search), so python314.dll sitting next to the exe is invisible to the
	// delay-load resolver. Preload it by full path first; the later delay-load
	// thunk then finds the already-mapped module by name.
	{
		const auto dll = QDir::toNativeSeparators(
			cExeDir() + u"python314.dll"_q);
		const auto wide = dll.toStdWString();
		if (!LoadLibraryExW(
				wide.c_str(),
				nullptr,
				LOAD_WITH_ALTERED_SEARCH_PATH)) {
			LOG(("TeleForge: failed to preload %1 (error %2).")
				.arg(dll)
				.arg(GetLastError()));
		}
	}
#endif

	RegisterApiModule();

	const auto modulesDir = QDir::cleanPath(cExeDir() + u"modules"_q);
	const auto libDir = modulesDir + u"/Lib"_q;
	const auto bundled = QFile::exists(libDir + u"/os.py"_q);

	if (bundled) {
		PyConfig config;
		PyConfig_InitPythonConfig(&config);

		const auto home = ToWide(modulesDir);
		PyConfig_SetString(&config, &config.home, home.c_str());

		config.module_search_paths_set = 1;
		for (const auto &path : {
				libDir,
				modulesDir + u"/DLLs"_q,
				modulesDir }) {
			const auto wide = ToWide(QDir::cleanPath(path));
			PyWideStringList_Append(&config.module_search_paths, wide.c_str());
		}

		const auto status = Py_InitializeFromConfig(&config);
		PyConfig_Clear(&config);
		if (PyStatus_Exception(status)) {
			LOG(("TeleForge: bundled Python init failed, falling back."));
			Py_Initialize();
		}
	} else {
		Py_Initialize();
	}

	// Make `import teleforge` work and add the user plugins directory.
	// Raw triple-quoted strings keep Windows backslashes intact.
	const auto pluginsDir = QDir::cleanPath(cWorkingDir() + u"plugins"_q);
	const auto bootstrap = QString(
		"import sys\n"
		"sys.path.insert(0, r\"\"\"%1\"\"\")\n"
		"sys.path.insert(0, r\"\"\"%2\"\"\")\n")
		.arg(modulesDir, pluginsDir);
	PyRun_SimpleString(bootstrap.toUtf8().constData());
}
#endif

} // namespace

bool PluginRunner::loadPlugin(const QString &path) {
#ifdef TELEFORGE_WITH_PYTHON
	EnsurePython();
	PyObject *mainModule = PyImport_AddModule("__main__");
	PyObject *globals = PyModule_GetDict(mainModule);
	QFile f(path);
	if (!f.open(QIODevice::ReadOnly)) {
		return false;
	}
	const auto bytes = f.readAll();
	if (PyRun_String(
			bytes.constData(),
			Py_file_input,
			globals,
			globals) == nullptr) {
		PyErr_Print();
		return false;
	}
	return true;
#else
	LOG(("TeleForge plugin skipped (Python not linked): %1").arg(path));
	return false;
#endif
}

void PluginRunner::unloadAll() {
#ifdef TELEFORGE_WITH_PYTHON
	if (g_initialized) {
		Py_Finalize();
		g_initialized = false;
	}
#endif
	g_tools = QJsonArray();
}

QJsonArray PluginRunner::toolsJson() {
	return g_tools;
}

QString PluginRunner::callTool(
		const QString &name,
		const QString &argumentsJson) {
	Q_UNUSED(name);
	Q_UNUSED(argumentsJson);
	return QString();
}

} // namespace TeleForge::Plugins
