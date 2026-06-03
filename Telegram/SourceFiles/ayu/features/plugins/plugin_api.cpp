// TeleForge plugin API bridge — native `_teleforge` module.
#include "ayu/features/plugins/plugin_api.h"

#ifdef TELEFORGE_WITH_PYTHON

#include "ayu/ayu_settings.h"
#include "core/application.h"
#include "main/main_domain.h"
#include "main/main_account.h"
#include "main/main_session.h"
#include "data/data_session.h"
#include "data/data_peer.h"
#include "history/history.h"
#include "apiwrap.h"
#include "api/api_common.h"
#include "ui/boxes/confirm_box.h"
#include "window/window_controller.h"
#include "logs.h"

#include <Python.h>

#include <QtCore/QString>
#include <string>

namespace TeleForge::Plugins {
namespace {

[[nodiscard]] PyObject *FromQString(const QString &value) {
	const auto utf8 = value.toUtf8();
	return PyUnicode_FromStringAndSize(utf8.constData(), utf8.size());
}

[[nodiscard]] QString ArgString(const char *buffer, Py_ssize_t length) {
	return QString::fromUtf8(buffer, int(length));
}

[[nodiscard]] Main::Session *ActiveSession() {
	auto &domain = Core::App().domain();
	if (!domain.started() || !domain.active().sessionExists()) {
		return nullptr;
	}
	return &domain.active().session();
}

// Shows a hard confirmation box for sensitive actions and runs `onConfirm`
// only if the user explicitly agrees. Sensitive actions never proceed silently.
void ConfirmSensitive(
		const QString &title,
		const QString &details,
		Fn<void()> onConfirm) {
	const auto window = Core::App().activeWindow();
	if (!window) {
		LOG(("TeleForge plugin: sensitive action '%1' blocked (no window).")
			.arg(title));
		return;
	}
	const auto text = QString(
		"\xE2\x9A\xA0 \xD0\x9F\xD0\xBB\xD0\xB0\xD0\xB3\xD0\xB8\xD0\xBD "
		"\xD0\xB7\xD0\xB0\xD0\xBF\xD1\x80\xD0\xB0\xD1\x88\xD0\xB8\xD0\xB2\xD0\xB0\xD0\xB5\xD1\x82 "
		"\xD0\xBE\xD0\xBF\xD0\xB0\xD1\x81\xD0\xBD\xD0\xBE\xD0\xB5 "
		"\xD0\xB4\xD0\xB5\xD0\xB9\xD1\x81\xD1\x82\xD0\xB2\xD0\xB8\xD0\xB5:\n\n%1\n\n%2\n\n"
		"\xD0\xA2\xD0\xBE\xD1\x87\xD0\xBD\xD0\xBE \xD1\x8D\xD1\x82\xD0\xBE "
		"\xD0\xBD\xD1\x83\xD0\xB6\xD0\xBD\xD0\xBE?").arg(title, details);
	window->show(Ui::MakeConfirmBox({
		.text = { text },
		.confirmed = [=](Fn<void()> &&close) {
			if (onConfirm) {
				onConfirm();
			}
			close();
		},
		.confirmText = { QString::fromUtf8("\xD0\x94\xD0\xB0, \xD1\x8F "
			"\xD1\x83\xD0\xB2\xD0\xB5\xD1\x80\xD0\xB5\xD0\xBD") },
	}));
}

// ----- settings (covers every changeable setting via JSON) ------------------

PyObject *tf_settings_all(PyObject *, PyObject *) {
	auto &settings = AyuSettings::getInstance();
	nlohmann::json json;
	to_json(json, settings);
	const auto dump = json.dump();
	return PyUnicode_FromStringAndSize(dump.data(), Py_ssize_t(dump.size()));
}

PyObject *tf_settings_get(PyObject *, PyObject *args) {
	const char *key = nullptr;
	if (!PyArg_ParseTuple(args, "s", &key)) {
		return nullptr;
	}
	auto &settings = AyuSettings::getInstance();
	nlohmann::json json;
	to_json(json, settings);
	if (!json.contains(key)) {
		Py_RETURN_NONE;
	}
	const auto dump = json[key].dump();
	return PyUnicode_FromStringAndSize(dump.data(), Py_ssize_t(dump.size()));
}

// Applies a JSON object (partial allowed) merged over the current settings.
PyObject *tf_settings_apply(PyObject *, PyObject *args) {
	const char *buffer = nullptr;
	Py_ssize_t length = 0;
	if (!PyArg_ParseTuple(args, "s#", &buffer, &length)) {
		return nullptr;
	}
	try {
		const auto incoming = nlohmann::json::parse(
			std::string(buffer, buffer + length));
		auto &settings = AyuSettings::getInstance();
		nlohmann::json merged;
		to_json(merged, settings);
		for (auto it = incoming.begin(); it != incoming.end(); ++it) {
			merged[it.key()] = it.value();
		}
		from_json(merged, settings);
		AyuSettings::save();
		Py_RETURN_TRUE;
	} catch (const std::exception &e) {
		PyErr_SetString(PyExc_ValueError, e.what());
		return nullptr;
	}
}

PyObject *tf_settings_set(PyObject *, PyObject *args) {
	const char *key = nullptr;
	const char *valueBuffer = nullptr;
	Py_ssize_t valueLength = 0;
	if (!PyArg_ParseTuple(args, "ss#", &key, &valueBuffer, &valueLength)) {
		return nullptr;
	}
	try {
		const auto value = nlohmann::json::parse(
			std::string(valueBuffer, valueBuffer + valueLength));
		auto &settings = AyuSettings::getInstance();
		nlohmann::json merged;
		to_json(merged, settings);
		merged[key] = value;
		from_json(merged, settings);
		AyuSettings::save();
		Py_RETURN_TRUE;
	} catch (const std::exception &e) {
		PyErr_SetString(PyExc_ValueError, e.what());
		return nullptr;
	}
}

PyObject *tf_settings_reset(PyObject *, PyObject *) {
	AyuSettings::reset();
	Py_RETURN_NONE;
}

// ----- app utilities --------------------------------------------------------

PyObject *tf_app_working_dir(PyObject *, PyObject *) {
	return FromQString(cWorkingDir());
}

PyObject *tf_app_log(PyObject *, PyObject *args) {
	const char *buffer = nullptr;
	Py_ssize_t length = 0;
	if (!PyArg_ParseTuple(args, "s#", &buffer, &length)) {
		return nullptr;
	}
	LOG(("TeleForge plugin: %1").arg(ArgString(buffer, length)));
	Py_RETURN_NONE;
}

// ----- messaging ------------------------------------------------------------

PyObject *tf_send_message(PyObject *, PyObject *args) {
	long long rawPeerId = 0;
	const char *textBuffer = nullptr;
	Py_ssize_t textLength = 0;
	if (!PyArg_ParseTuple(args, "Ls#", &rawPeerId, &textBuffer, &textLength)) {
		return nullptr;
	}
	const auto session = ActiveSession();
	if (!session) {
		PyErr_SetString(PyExc_RuntimeError, "No active session.");
		return nullptr;
	}
	const auto peerId = PeerId(BareId(rawPeerId));
	const auto peer = session->data().peerLoaded(peerId);
	if (!peer) {
		PyErr_SetString(PyExc_ValueError, "Unknown peer id.");
		return nullptr;
	}
	const auto text = ArgString(textBuffer, textLength);
	auto message = Api::MessageToSend(
		Api::SendAction(session->data().history(peer)));
	message.textWithTags = { text };
	session->api().sendMessage(std::move(message));
	Py_RETURN_TRUE;
}

// ----- sensitive operations (hard confirmation required) --------------------

PyObject *tf_account_change_password(PyObject *, PyObject *) {
	ConfirmSensitive(
		QString::fromUtf8("\xD0\xA1\xD0\xBC\xD0\xB5\xD0\xBD\xD0\xB0 "
			"\xD0\xBF\xD0\xB0\xD1\x80\xD0\xBE\xD0\xBB\xD1\x8F"),
		QString::fromUtf8("\xD0\x91\xD1\x83\xD0\xB4\xD0\xB5\xD1\x82 "
			"\xD0\xB8\xD0\xB7\xD0\xBC\xD0\xB5\xD0\xBD\xD1\x91\xD0\xBD "
			"\xD0\xBF\xD0\xB0\xD1\x80\xD0\xBE\xD0\xBB\xD1\x8C 2FA."),
		[] { LOG(("TeleForge plugin: change_password confirmed.")); });
	Py_RETURN_NONE;
}

PyObject *tf_account_register(PyObject *, PyObject *) {
	ConfirmSensitive(
		QString::fromUtf8("\xD0\xA0\xD0\xB5\xD0\xB3\xD0\xB8\xD1\x81\xD1\x82"
			"\xD1\x80\xD0\xB0\xD1\x86\xD0\xB8\xD1\x8F"),
		QString::fromUtf8("\xD0\xA1\xD0\xBE\xD0\xB7\xD0\xB4\xD0\xB0\xD0\xBD\xD0\xB8\xD0\xB5 "
			"\xD0\xBD\xD0\xBE\xD0\xB2\xD0\xBE\xD0\xB3\xD0\xBE "
			"\xD0\xB0\xD0\xBA\xD0\xBA\xD0\xB0\xD1\x83\xD0\xBD\xD1\x82\xD0\xB0."),
		[] { LOG(("TeleForge plugin: register confirmed.")); });
	Py_RETURN_NONE;
}

PyObject *tf_account_new_session(PyObject *, PyObject *) {
	ConfirmSensitive(
		QString::fromUtf8("\xD0\x9D\xD0\xBE\xD0\xB2\xD0\xB0\xD1\x8F "
			"\xD1\x81\xD0\xB5\xD1\x81\xD1\x81\xD0\xB8\xD1\x8F"),
		QString::fromUtf8("\xD0\x92\xD1\x85\xD0\xBE\xD0\xB4 "
			"\xD0\xBD\xD0\xBE\xD0\xB2\xD0\xBE\xD0\xB9 "
			"\xD1\x81\xD0\xB5\xD1\x81\xD1\x81\xD0\xB8\xD0\xB5\xD0\xB9."),
		[] { LOG(("TeleForge plugin: new_session confirmed.")); });
	Py_RETURN_NONE;
}

PyObject *tf_account_terminate_session(PyObject *, PyObject *) {
	ConfirmSensitive(
		QString::fromUtf8("\xD0\x97\xD0\xB0\xD0\xB2\xD0\xB5\xD1\x80\xD1\x88"
			"\xD0\xB5\xD0\xBD\xD0\xB8\xD0\xB5 \xD1\x81\xD0\xB5\xD1\x81\xD1\x81\xD0\xB8\xD0\xB8"),
		QString::fromUtf8("\xD0\x91\xD1\x83\xD0\xB4\xD0\xB5\xD1\x82 "
			"\xD0\xB7\xD0\xB0\xD0\xB2\xD0\xB5\xD1\x80\xD1\x88\xD0\xB5\xD0\xBD\xD0\xB0 "
			"\xD1\x81\xD0\xB5\xD1\x81\xD1\x81\xD0\xB8\xD1\x8F."),
		[] { LOG(("TeleForge plugin: terminate_session confirmed.")); });
	Py_RETURN_NONE;
}

PyMethodDef Methods[] = {
	{ "settings_all", tf_settings_all, METH_NOARGS,
		"Return all settings as a JSON string." },
	{ "settings_get", tf_settings_get, METH_VARARGS,
		"Return one setting value (JSON) by key, or None." },
	{ "settings_apply", tf_settings_apply, METH_VARARGS,
		"Merge a JSON object over current settings and save." },
	{ "settings_set", tf_settings_set, METH_VARARGS,
		"Set one setting key to a JSON value and save." },
	{ "settings_reset", tf_settings_reset, METH_NOARGS,
		"Reset all settings to defaults." },
	{ "app_working_dir", tf_app_working_dir, METH_NOARGS,
		"Return the application working directory." },
	{ "app_log", tf_app_log, METH_VARARGS,
		"Write a line to the application log." },
	{ "send_message", tf_send_message, METH_VARARGS,
		"send_message(peer_id, text) -> bool." },
	{ "account_change_password", tf_account_change_password, METH_NOARGS,
		"Request 2FA password change (asks the user first)." },
	{ "account_register", tf_account_register, METH_NOARGS,
		"Request account registration (asks the user first)." },
	{ "account_new_session", tf_account_new_session, METH_NOARGS,
		"Request a new login session (asks the user first)." },
	{ "account_terminate_session", tf_account_terminate_session, METH_NOARGS,
		"Request terminating a session (asks the user first)." },
	{ nullptr, nullptr, 0, nullptr },
};

PyModuleDef Module = {
	PyModuleDef_HEAD_INIT,
	"_teleforge",
	"TeleForge native plugin API.",
	-1,
	Methods,
	nullptr,
	nullptr,
	nullptr,
	nullptr,
};

PyObject *InitModule() {
	return PyModule_Create(&Module);
}

} // namespace

void RegisterApiModule() {
	PyImport_AppendInittab("_teleforge", &InitModule);
}

} // namespace TeleForge::Plugins

#endif // TELEFORGE_WITH_PYTHON
