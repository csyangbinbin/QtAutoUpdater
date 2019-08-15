#include "updater.h"
#include "updater_p.h"
#include "updaterplugin.h"
#include <QtCore/QCoreApplication>
#include <QtCore/QTimer>
#include <QtCore/QSettings>
#include <QtCore/QStandardPaths>
#include <QtCore/QDebug>
#include <QtCore/private/qfactoryloader_p.h>

namespace QtAutoUpdater {

Q_LOGGING_CATEGORY(logQtAutoUpdater, "QtAutoUpdater")

}

using namespace QtAutoUpdater;

Q_GLOBAL_STATIC_WITH_ARGS(QFactoryLoader, loader,
						  (QtAutoUpdater_UpdaterPlugin_iid,
						   QLatin1String("/updaters")))

namespace {

class VariantConfigReader : public UpdaterBackend::IConfigReader
{
public:
	inline VariantConfigReader(QString &&backend, QVariantMap &&map) :
		_backend{std::move(backend)},
		_map{std::move(map)}
	{}

	QString backend() const override {
		return _backend;
	}

	std::optional<QVariant> value(const QString &key) const override {
		return _map.contains(key) ? _map.value(key) : std::optional<QVariant>{std::nullopt};
	}

	QVariant value(const QString &key, const QVariant &defaultValue) const override {
		return _map.value(key, defaultValue);
	}

private:
	QString _backend;
	QVariantMap _map;
};

class SettingsConfigReader : public UpdaterBackend::IConfigReader
{
public:
	inline SettingsConfigReader(QSettings *settings) :
		_settings{settings}
	{}

	QString backend() const override {
		return _settings->value(QStringLiteral("backend")).toString();
	}

	std::optional<QVariant> value(const QString &key) const override {
		return _settings->contains(key) ? _settings->value(key) : std::optional<QVariant>{std::nullopt};
	}

	QVariant value(const QString &key, const QVariant &defaultValue) const override {
		return _settings->value(key, defaultValue);
	}

private:
	QScopedPointer<QSettings, QScopedPointerDeleteLater> _settings;
};

}

Updater::Updater(QObject *parent) :
	Updater(*new UpdaterPrivate{}, parent)
{}

Updater::Updater(UpdaterPrivate &dd, QObject *parent) :
	QObject{dd, parent}
{
	Q_D(Updater);
	d->scheduler = new SimpleScheduler{this};

	QObjectPrivate::connect(qApp, &QCoreApplication::aboutToQuit,
							d, &UpdaterPrivate::_q_appAboutToExit,
							Qt::DirectConnection);
	connect(d->scheduler, &SimpleScheduler::scheduleTriggered,
			this, qOverload<>(&Updater::checkForUpdates));
}

Updater *Updater::createUpdater(QObject *parent, AdminAuthoriser *authoriser)
{
	auto config = UpdaterPrivate::findDefaultConfig();
	if (config)
		return createUpdater(config, parent, authoriser);
	else {
		qCCritical(logQtAutoUpdater) << "Unable to find the default updater configuration file";
		return nullptr;
	}
}

Updater *Updater::createUpdater(const QString &configPath, QObject *parent, AdminAuthoriser *authoriser)
{
	return createUpdater(new QSettings {
							 configPath,
							 QSettings::IniFormat
						 }, parent, authoriser);
}

Updater *Updater::createUpdater(QSettings *config, QObject *parent, AdminAuthoriser *authoriser)
{
	return UpdaterPrivate::createUpdater(new SettingsConfigReader {config}, parent, authoriser);
}

Updater *Updater::createUpdater(QString key, QVariantMap arguments, QObject *parent, AdminAuthoriser *authoriser)
{
	return UpdaterPrivate::createUpdater(new VariantConfigReader {
											 std::move(key),
											 std::move(arguments)
										 }, parent, authoriser);
}

Updater::~Updater()
{
	Q_D(Updater);
	if(d->runOnExit)
		qCWarning(logQtAutoUpdater) << "Updater destroyed with run on exit active before the application quit";
	if (isRunning()) {
		Q_UNIMPLEMENTED();
	}
}

UpdaterBackend *Updater::backend() const
{
	const Q_D(Updater);
	return d->backend;
}

bool Updater::willRunOnExit() const
{
	const Q_D(Updater);
	return d->runOnExit;
}

Updater::State Updater::state() const
{
	const Q_D(Updater);
	return d->state;
}

bool Updater::isRunning() const
{
	const Q_D(Updater);
	return d->state == State::Checking ||
			d->state == State::Installing;
}

QList<UpdateInfo> Updater::updateInfo() const
{
	const Q_D(Updater);
	return d->updateInfos;
}

int Updater::scheduleUpdate(int delaySeconds, bool repeated)
{
	if((static_cast<qint64>(delaySeconds) * 1000ll) > static_cast<qint64>(std::numeric_limits<int>::max())) {
		qCWarning(logQtAutoUpdater) << "delaySeconds to big to be converted to msecs";
		return 0;
	}

	Q_D(Updater);
	return d->scheduler->startSchedule(delaySeconds * 1000, repeated);
}

int Updater::scheduleUpdate(const QDateTime &when)
{
	Q_D(Updater);
	return d->scheduler->startSchedule(when);
}

bool Updater::runUpdater(bool forceOnExit)
{
	Q_D(Updater);
	if (!d->backend->features().testFlag(UpdaterBackend::Feature::ParallelInstall))
		forceOnExit = true;

	if (d->backend->features().testFlag(UpdaterBackend::Feature::TriggerInstall)) {
		if (forceOnExit) {
			if (!d->runOnExit) {
				d->runOnExit = true;
				emit runOnExitChanged(d->runOnExit, {});
			}
			return true;
		} else {
			const auto ok = d->backend->triggerUpdates(d->updateInfos, true);
			if (ok) {
				d->state = State::Installing;
				emit stateChanged(d->state, {});
			}
			return ok;
		}
	} else if (d->backend->features().testFlag(UpdaterBackend::Feature::PerformInstall)) {
		if (forceOnExit) {
			qCCritical(d->backend->logCat()) << "Backend does not support installation after exiting";
			return false;
		} else {
			auto installer = d->backend->installUpdates(d->updateInfos);
			if (installer)  {
				// TODO connect installer to _q_triggerInstallDone
				emit showInstaller(installer, {});
				return true;
			} else
				return false;
		}
	} else {
		qCCritical(d->backend->logCat()) << "Backend does not support installation";
		return false;
	}
}

void Updater::checkForUpdates()
{
	Q_D(Updater);
	if (!isRunning()) {
		d->state = State::Checking;
		d->updateInfos.clear();
		emit updateInfoChanged(d->updateInfos, {});
		if (d->backend->features().testFlag(UpdaterBackend::Feature::CheckProgress))
			emit progressChanged(0.0, QStringLiteral(""), {}); // empty, but not null string
		else
			emit progressChanged(-1.0, tr("Checking for updates…"), {});
		emit stateChanged(d->state, {});
		d->backend->checkForUpdates();
	}
}

void Updater::abortUpdateCheck(int killDelay)
{
	Q_D(Updater);
	if(d->state == State::Checking) {
		if(killDelay != 0) {
			d->backend->abort(false);
			if(killDelay > 0) {
				QTimer::singleShot(killDelay, this, [this](){
					abortUpdateCheck(0);
				});
			}
		} else
			d->backend->abort(true);
	}
}

void Updater::cancelScheduledUpdate(int taskId)
{
	Q_D(Updater);
	d->scheduler->cancelSchedule(taskId);
}

void Updater::cancelExitRun()
{
	Q_D(Updater);
	if (d->runOnExit) {
		d->runOnExit = false;
		d->adminAuth.reset();
		emit runOnExitChanged(d->runOnExit, {});
	}
}

// ------------- private implementation -------------

QSettings *UpdaterPrivate::findDefaultConfig()
{
#ifdef Q_OS_WIN
	// Windows only: try the registry as first and thus preferred location
	{
		auto conf = new QSettings {
			QSettings::NativeFormat,
			QSettings::UserScope,
			QCoreApplication::organizationName(),
			QCoreApplication::applicationName()
		};
		conf->beginGroup(QStringLiteral("updater"));
		if (conf->contains(QStringLiteral("backend")))
			return conf;
		else
			delete conf;
	}
#endif

	// try config directories first
	auto paths = QStandardPaths::locateAll(QStandardPaths::AppConfigLocation, QStringLiteral("updater.conf"));
	// then try data dirs (includes bundle/exe root etc., depending on the platform)
	paths += QStandardPaths::locateAll(QStandardPaths::AppDataLocation, QStringLiteral("updater.conf"));
	for (const auto &path : QStandardPaths::locateAll(QStandardPaths::AppConfigLocation, QStringLiteral("updater.conf"))) {
		const auto conf = new QSettings{path, QSettings::IniFormat};
		if (conf->contains(QStringLiteral("backend")))
			return conf;
		else
			delete conf;
	}

	return nullptr;
}

Updater *UpdaterPrivate::createUpdater(UpdaterBackend::IConfigReader *config, QObject *parent, AdminAuthoriser *authoriser)
{
	auto updater = new Updater{parent};
	auto backend = qLoadPlugin<UpdaterBackend, UpdaterPlugin>(loader, config->backend(), updater);
	if (!backend || !backend->initialize(QScopedPointer<UpdaterBackend::IConfigReader>{config}, QScopedPointer<AdminAuthoriser>{authoriser})) {
		delete updater;
		return nullptr;
	}

	updater->d_func()->setBackend(backend);
	return updater;
}

void UpdaterPrivate::setBackend(UpdaterBackend *newBackend)
{
	Q_Q(Updater);
	backend = newBackend;
	connect(backend, &UpdaterBackend::checkDone,
			this, &UpdaterPrivate::_q_checkDone);
	QObject::connect(backend, &UpdaterBackend::checkProgress,
					 q, std::bind(&Updater::progressChanged, q,
								  std::placeholders::_1,
								  std::placeholders::_2,
								  Updater::QPrivateSignal{}));
	connect(backend, &UpdaterBackend::triggerInstallDone,
			this, &UpdaterPrivate::_q_triggerInstallDone);
}

void UpdaterPrivate::_q_appAboutToExit()
{
	if (runOnExit) {
		runOnExit = false;
		backend->triggerUpdates(updateInfos, false);
	}
}

void UpdaterPrivate::_q_checkDone(bool success, QList<UpdateInfo> updates)
{
	Q_Q(Updater);
	if (success) {
		updateInfos = std::move(updates);
		if (updateInfos.isEmpty())
			state = State::NoUpdates;
		else {
			state = State::NewUpdates;
			emit q->updateInfoChanged(updateInfos, {});
		}
	} else {
		updateInfos.clear();
		state = State::Error;
	}
	emit q->stateChanged(state, {});
	emit q->checkUpdatesDone(state, {});
}

void UpdaterPrivate::_q_triggerInstallDone(bool success)
{
	Q_Q(Updater);
	updateInfos.clear();
	state = success ? State::NoUpdates : State::Error;
	emit q->stateChanged(state, {});
	emit q->installDone(success, {});
}

#include "moc_updater.cpp"
