#include "ayu/features/plugins/plugin_runner.h"

#include "ayu/features/plugins/plugin_api.h"
#include "settings.h"
#include "logs.h"

#include <QtCore/QDir>
#include <QtCore/QFile>

#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>

#ifdef TELEFORGE_WITH_PYTHON
#include <Python.h>
#ifdef Q_OS_WIN
#include <windows.h>
#endif
#endif

namespace TeleForge::Plugins {
namespace {

auto g_tools = QJsonArray();

auto g_menuMutex = std::mutex();
auto g_menuItems = QString(u"[]"_q);

#ifdef TELEFORGE_WITH_PYTHON
auto g_queueMutex = std::mutex();
auto g_queueCond = std::condition_variable();
auto g_tasks = std::deque<std::function<void()>>();
auto g_threadStarted = false;
auto g_threadStop = false;
std::thread g_thread;

void Post(std::function<void()> task) {
	auto lock = std::unique_lock(g_queueMutex);
	g_tasks.push_back(std::move(task));
	g_queueCond.notify_one();
}

auto g_initialized = false;

[[nodiscard]] std::wstring ToWide(const QString &value) {
	return value.toStdWString();
}

// Initializes the embedded interpreter, preferring the self-contained Python
// bundled next to the executable (exe_dir/modules) so plugins do not depend on
// a system Python installation. Runs on the dedicated plugin thread.
void EnsurePython() {
	if (g_initialized) {
		return;
	}
	g_initialized = true;

#ifdef Q_OS_WIN
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

	const auto pluginsDir = QDir::cleanPath(cWorkingDir() + u"plugins"_q);
	const auto bootstrap = QString(
		"import sys\n"
		"sys.path.insert(0, r\"\"\"%1\"\"\")\n"
		"sys.path.insert(0, r\"\"\"%2\"\"\")\n")
		.arg(modulesDir, pluginsDir);
	PyRun_SimpleString(bootstrap.toUtf8().constData());
}

void ExecFile(const QString &path) {
	auto file = QFile(path);
	if (!file.open(QIODevice::ReadOnly)) {
		LOG(("TeleForge plugin: cannot open %1").arg(path));
		return;
	}
	const auto bytes = file.readAll();
	const auto mainModule = PyImport_AddModule("__main__");
	const auto globals = PyModule_GetDict(mainModule);
	if (!PyRun_String(bytes.constData(), Py_file_input, globals, globals)) {
		PyErr_Print();
	}
}

[[nodiscard]] PyObject *TeleforgeModule() {
	const auto module = PyImport_ImportModule("teleforge");
	if (!module) {
		PyErr_Print();
	}
	return module;
}

void CallReset() {
	if (const auto module = TeleforgeModule()) {
		const auto result = PyObject_CallMethod(module, "_reset", nullptr);
		if (!result) {
			PyErr_Print();
		} else {
			Py_DECREF(result);
		}
		Py_DECREF(module);
	}
}

void RefreshMenuCache() {
	auto items = QString(u"[]"_q);
	if (const auto module = TeleforgeModule()) {
		const auto result = PyObject_CallMethod(module, "_menu_items", nullptr);
		if (!result) {
			PyErr_Print();
		} else {
			if (PyUnicode_Check(result)) {
				items = QString::fromUtf8(PyUnicode_AsUTF8(result));
			}
			Py_DECREF(result);
		}
		Py_DECREF(module);
	}
	auto lock = std::unique_lock(g_menuMutex);
	g_menuItems = items;
}

void ThreadLoop() {
	EnsurePython();
	for (;;) {
		auto task = std::function<void()>();
		{
			auto lock = std::unique_lock(g_queueMutex);
			g_queueCond.wait(lock, [] {
				return g_threadStop || !g_tasks.empty();
			});
			if (g_threadStop && g_tasks.empty()) {
				break;
			}
			task = std::move(g_tasks.front());
			g_tasks.pop_front();
		}
		if (task) {
			task();
		}
	}
	if (g_initialized) {
		Py_Finalize();
		g_initialized = false;
	}
}
#endif // TELEFORGE_WITH_PYTHON

} // namespace

void PluginRunner::ensureStarted() {
#ifdef TELEFORGE_WITH_PYTHON
	auto lock = std::unique_lock(g_queueMutex);
	if (g_threadStarted) {
		return;
	}
	g_threadStarted = true;
	g_threadStop = false;
	lock.unlock();
	g_thread = std::thread(ThreadLoop);
#endif
}

void PluginRunner::reloadEnabled(const QStringList &paths) {
#ifdef TELEFORGE_WITH_PYTHON
	ensureStarted();
	Post([paths] {
		CallReset();
		for (const auto &path : paths) {
			ExecFile(path);
		}
		RefreshMenuCache();
	});
#else
	Q_UNUSED(paths);
#endif
}

void PluginRunner::loadPlugin(const QString &path) {
#ifdef TELEFORGE_WITH_PYTHON
	ensureStarted();
	Post([path] {
		ExecFile(path);
		RefreshMenuCache();
	});
#else
	LOG(("TeleForge plugin skipped (Python not linked): %1").arg(path));
#endif
}

void PluginRunner::fireEvent(
		const QString &name,
		const QString &payloadJson) {
#ifdef TELEFORGE_WITH_PYTHON
	if (!g_threadStarted) {
		return;
	}
	Post([name, payloadJson] {
		if (const auto module = TeleforgeModule()) {
			const auto result = PyObject_CallMethod(
				module,
				"_dispatch_event",
				"ss",
				name.toUtf8().constData(),
				payloadJson.toUtf8().constData());
			if (!result) {
				PyErr_Print();
			} else {
				Py_DECREF(result);
			}
			Py_DECREF(module);
		}
	});
#else
	Q_UNUSED(name);
	Q_UNUSED(payloadJson);
#endif
}

void PluginRunner::invokeMenuItem(const QString &itemId) {
#ifdef TELEFORGE_WITH_PYTHON
	if (!g_threadStarted) {
		return;
	}
	Post([itemId] {
		if (const auto module = TeleforgeModule()) {
			const auto result = PyObject_CallMethod(
				module,
				"_invoke_menu",
				"s",
				itemId.toUtf8().constData());
			if (!result) {
				PyErr_Print();
			} else {
				Py_DECREF(result);
			}
			Py_DECREF(module);
		}
	});
#else
	Q_UNUSED(itemId);
#endif
}

QString PluginRunner::menuItemsJson() {
	auto lock = std::unique_lock(g_menuMutex);
	return g_menuItems;
}

void PluginRunner::unloadAll() {
#ifdef TELEFORGE_WITH_PYTHON
	if (g_threadStarted) {
		Post([] {
			CallReset();
			RefreshMenuCache();
		});
	}
#endif
	g_tools = QJsonArray();
}

void PluginRunner::shutdown() {
#ifdef TELEFORGE_WITH_PYTHON
	{
		auto lock = std::unique_lock(g_queueMutex);
		if (!g_threadStarted) {
			return;
		}
		g_threadStop = true;
		g_queueCond.notify_one();
	}
	// Detach rather than join: a plugin hook may be blocked in RunOnMainSync
	// waiting for the main thread, so joining here (on the main thread) would
	// deadlock. The interpreter thread exits cleanly once idle; anything still
	// running is reclaimed at process exit.
	if (g_thread.joinable()) {
		g_thread.detach();
	}
	g_threadStarted = false;
#endif
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
