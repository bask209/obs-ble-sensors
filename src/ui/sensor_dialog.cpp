#include "sensor_dialog.hpp"
#include "sensor_dialog_p.hpp"

#include "obs/plugin_context.hpp"

#include <obs-frontend-api.h>

#include <QApplication>
#include <QCheckBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <vector>

namespace blehr {

namespace {
constexpr int64_t kSecondNs = 1000000000LL;
constexpr int64_t kNearbyForgetNs = 30 * kSecondNs;
constexpr int64_t kStatusTimeoutNs = 5 * kSecondNs;
constexpr int kKeyRole = Qt::UserRole;
constexpr size_t kMaxNearby = 300;

// Message boxes that never interpret text as HTML: device names come from the radio.
int askPlain(QWidget *parent, const QString &title, const QString &text,
	     QMessageBox::StandardButtons buttons = QMessageBox::Yes | QMessageBox::No,
	     QMessageBox::StandardButton defaultButton = QMessageBox::No, QMessageBox::Icon icon = QMessageBox::Question)
{
	QMessageBox box(icon, title, text, buttons, parent);
	box.setTextFormat(Qt::PlainText);
	box.setDefaultButton(defaultButton);
	return box.exec();
}

QString q(const std::string &s)
{
	return QString::fromStdString(s);
}

QString signalText(int16_t rssi)
{
	if (rssi >= -60)
		return QStringLiteral("strong signal");
	if (rssi >= -75)
		return QStringLiteral("good signal");
	return QStringLiteral("weak signal");
}

struct Row {
	std::string key;
	QString name;
	QString type;
	QString status;
	bool saved = false;
};
} // namespace

SensorDialog::SensorDialog(const std::string &currentKey, QWidget *parent) : QDialog(parent), currentKey_(currentKey)
{
	setWindowTitle(QStringLiteral("BLE Heart Rate Sensors"));
	setMinimumSize(560, 360);

	auto *layout = new QVBoxLayout(this);
	auto *intro = new QLabel(QStringLiteral(
		"Select your heart-rate sensor and click <b>Use selected sensor</b>. Saved sensors are remembered "
		"and reconnect automatically whenever they are switched on."));
	intro->setWordWrap(true);
	layout->addWidget(intro);

	list_ = new QTreeWidget(this);
	list_->setColumnCount(3);
	list_->setHeaderLabels({QStringLiteral("Device"), QStringLiteral("Type"), QStringLiteral("Status")});
	list_->setRootIsDecorated(false);
	list_->setUniformRowHeights(true);
	list_->setSelectionMode(QAbstractItemView::SingleSelection);
	list_->header()->setStretchLastSection(true);
	list_->setColumnWidth(0, 200);
	list_->setColumnWidth(1, 150);
	layout->addWidget(list_, 1);

	showAll_ = new QCheckBox(QStringLiteral("Show all Bluetooth devices (not only heart-rate sensors)"), this);
	layout->addWidget(showAll_);

	status_ = new QLabel(this);
	status_->setWordWrap(true);
	layout->addWidget(status_);

	auto *buttons = new QHBoxLayout();
	rescan_ = new QPushButton(QStringLiteral("Rescan"), this);
	save_ = new QPushButton(QStringLiteral("Save..."), this);
	rename_ = new QPushButton(QStringLiteral("Rename..."), this);
	forget_ = new QPushButton(QStringLiteral("Forget"), this);
	use_ = new QPushButton(QStringLiteral("Use selected sensor"), this);
	auto *close = new QPushButton(QStringLiteral("Close"), this);
	use_->setDefault(true);
	buttons->addWidget(rescan_);
	buttons->addWidget(save_);
	buttons->addWidget(rename_);
	buttons->addWidget(forget_);
	buttons->addStretch(1);
	buttons->addWidget(use_);
	buttons->addWidget(close);
	layout->addLayout(buttons);

	connect(rescan_, &QPushButton::clicked, this, [this] {
		nearby_.clear();
		startScan();
		rebuild();
	});
	connect(save_, &QPushButton::clicked, this, &SensorDialog::onSave);
	connect(rename_, &QPushButton::clicked, this, &SensorDialog::onRename);
	connect(forget_, &QPushButton::clicked, this, &SensorDialog::onForget);
	connect(use_, &QPushButton::clicked, this, &SensorDialog::onUse);
	connect(close, &QPushButton::clicked, this, &QDialog::reject);
	connect(showAll_, &QCheckBox::toggled, this, [this] { rebuild(); });
	connect(list_, &QTreeWidget::itemSelectionChanged, this, &SensorDialog::updateButtons);
	connect(list_, &QTreeWidget::itemDoubleClicked, this, [this] { onUse(); });
	connect(&timer_, &QTimer::timeout, this, &SensorDialog::refreshStatus);

	rebuild(); // Saved devices show immediately, before any scan result.
	startScan();
	timer_.start(1000);
}

SensorDialog::~SensorDialog()
{
	// Unregister first: once the token is gone no BLE thread can post to this object.
	scanToken_.reset();
	timer_.stop();
}

void SensorDialog::startScan()
{
	scanToken_.reset();
	scanStartedNs_ = monotonicNs();
	auto mgr = plugin::manager();
	if (!mgr)
		return;
	// Called on a Bluetooth thread: hop to the UI thread. If the dialog is destroyed
	// before the queued call runs, Qt drops it (the context object is gone).
	scanToken_ = mgr->addScanListener([this](const Advertisement &adv) {
		QMetaObject::invokeMethod(this, [this, adv] { onAdvertisement(adv); }, Qt::QueuedConnection);
	});
	mgr->requestPairedDevices(); // Delivered through the same listener, off the UI thread.
	refreshStatus();
}

void SensorDialog::onAdvertisement(const Advertisement &adv)
{
	const std::string key = adv.identity.key();
	// Bound memory even if someone floods the air with fake advertisements.
	if (nearby_.size() >= kMaxNearby && !nearby_.count(key) && !adv.paired && !adv.advertisesHeartRate)
		return;
	auto &entry = nearby_[key];
	if (adv.paired) {
		const bool changed = !entry.paired;
		entry.paired = true;
		entry.adv.identity = adv.identity;
		if (entry.adv.name.empty())
			entry.adv.name = adv.name;
		if (changed)
			rebuild();
		return;
	}
	const std::string previousName = entry.adv.name;
	const bool previousHr = entry.adv.advertisesHeartRate;
	const bool isNew = entry.lastSeenNs == 0;
	entry.adv.identity = adv.identity;
	entry.adv.rssi = adv.rssi;
	// Names and service lists may come in separate packets (advert vs scan response).
	if (!adv.name.empty())
		entry.adv.name = adv.name;
	entry.adv.advertisesHeartRate = previousHr || adv.advertisesHeartRate;
	entry.lastSeenNs = monotonicNs();
	if (isNew || previousName != entry.adv.name || previousHr != entry.adv.advertisesHeartRate)
		rebuild();
}

std::string SensorDialog::suggestedName(const std::string &key) const
{
	auto it = nearby_.find(key);
	if (it != nearby_.end() && !it->second.adv.name.empty())
		return it->second.adv.name;
	if (auto saved = plugin::registry().find(key))
		return saved->displayName();
	return "Heart-rate sensor";
}

void SensorDialog::rebuild()
{
	const int64_t now = monotonicNs();
	auto mgr = plugin::manager();
	std::vector<Row> rows;

	for (const auto &d : plugin::registry().list()) {
		Row r;
		r.key = d.key();
		r.saved = true;
		r.name = q(d.displayName());
		auto near = nearby_.find(r.key);
		const bool isNear = near != nearby_.end() && now - near->second.lastSeenNs < kNearbyForgetNs;
		r.type = r.key == currentKey_ ? QStringLiteral("Saved - in use") : QStringLiteral("Saved");
		std::optional<SensorSnapshot> snap = mgr ? mgr->peek(r.key) : std::nullopt;
		if (snap)
			r.status = q(friendlyStatus(*snap, now, kStatusTimeoutNs));
		else if (isNear)
			r.status = QStringLiteral("Nearby, ") + signalText(near->second.adv.rssi);
		else
			r.status = QStringLiteral("Not seen nearby (switch it on / wear it)");
		rows.push_back(std::move(r));
	}

	for (const auto &kv : nearby_) {
		const Advertisement &a = kv.second.adv;
		const bool seen = kv.second.lastSeenNs != 0 && now - kv.second.lastSeenNs < kNearbyForgetNs;
		if (!seen && !kv.second.paired)
			continue;
		if (plugin::registry().find(kv.first))
			continue;
		if (!a.advertisesHeartRate && !kv.second.paired && !showAll_->isChecked())
			continue;
		Row r;
		r.key = kv.first;
		r.name = a.name.empty() ? QStringLiteral("Unnamed device (") + q(formatAddress(a.identity.address)) + ")"
					: q(a.name);
		if (kv.second.paired)
			r.type = a.advertisesHeartRate ? QStringLiteral("Paired - heart rate") : QStringLiteral("Paired with Windows");
		else
			r.type = a.advertisesHeartRate ? QStringLiteral("Heart-rate device")
						       : QStringLiteral("Other device - may be unsupported");
		r.status = seen ? QStringLiteral("Nearby, ") + signalText(a.rssi)
				: QStringLiteral("Paired - not advertising right now");
		rows.push_back(std::move(r));
	}

	// Update in place when the set of rows is unchanged so selection and scrolling stay put.
	bool sameRows = list_->topLevelItemCount() == int(rows.size());
	for (int i = 0; sameRows && i < int(rows.size()); ++i)
		sameRows = list_->topLevelItem(i)->data(0, kKeyRole).toString() == q(rows[size_t(i)].key);

	const std::string selected = selectedKey();
	if (!sameRows) {
		list_->clear();
		for (const auto &r : rows) {
			auto *item = new QTreeWidgetItem(list_);
			item->setData(0, kKeyRole, q(r.key));
		}
	}
	for (int i = 0; i < int(rows.size()); ++i) {
		const Row &r = rows[size_t(i)];
		QTreeWidgetItem *item = list_->topLevelItem(i);
		item->setText(0, r.name);
		item->setText(1, r.type);
		item->setText(2, r.status);
		item->setToolTip(0, q(r.key));
		QFont font = item->font(0);
		font.setBold(r.saved);
		item->setFont(0, font);
		if (!sameRows && (r.key == selected || (selected.empty() && r.key == currentKey_)))
			item->setSelected(true);
	}
	updateButtons();
}

void SensorDialog::refreshStatus()
{
	const int64_t nowNs = monotonicNs();
	for (auto it = nearby_.begin(); it != nearby_.end();) {
		if (!it->second.paired && nowNs - it->second.lastSeenNs >= kNearbyForgetNs)
			it = nearby_.erase(it);
		else
			++it;
	}
	rebuild();
	const int64_t elapsed = monotonicNs() - scanStartedNs_;
	int hrNearby = 0;
	for (const auto &kv : nearby_)
		if (kv.second.adv.advertisesHeartRate && monotonicNs() - kv.second.lastSeenNs < kNearbyForgetNs)
			hrNearby++;

	if (!plugin::manager()) {
		status_->setText(QStringLiteral("Bluetooth support is not available right now."));
	} else if (hrNearby > 0) {
		status_->setText(QStringLiteral("Scanning... %1 heart-rate sensor(s) nearby.").arg(hrNearby));
	} else if (elapsed < 10 * kSecondNs) {
		status_->setText(QStringLiteral("Scanning for nearby Bluetooth heart-rate sensors..."));
	} else {
		status_->setText(QStringLiteral(
			"No heart-rate sensors found yet. Check that Bluetooth is turned on in Windows and that the "
			"sensor is worn - most chest straps only switch on when they touch skin. Watches usually need "
			"their own \"broadcast heart rate\" mode enabled."));
	}
	if (plugin::devModeActive())
		status_->setText(status_->text() + QStringLiteral("  [DEVELOPMENT MODE: simulated Bluetooth]"));
}

std::string SensorDialog::selectedKey() const
{
	const auto items = list_->selectedItems();
	if (items.isEmpty())
		return {};
	return items.first()->data(0, kKeyRole).toString().toStdString();
}

bool SensorDialog::selectedIsSaved() const
{
	const std::string key = selectedKey();
	return !key.empty() && plugin::registry().find(key).has_value();
}

void SensorDialog::updateButtons()
{
	const bool hasSelection = !selectedKey().empty();
	const bool saved = selectedIsSaved();
	save_->setEnabled(hasSelection && !saved);
	rename_->setEnabled(saved);
	forget_->setEnabled(saved);
	use_->setEnabled(hasSelection);
}

bool SensorDialog::saveWithAlias(const std::string &key)
{
	auto identity = identityFromKey(key);
	if (!identity)
		return false;
	bool ok = false;
	const QString alias = QInputDialog::getText(this, QStringLiteral("Save sensor"),
						    QStringLiteral("Name for this sensor:"), QLineEdit::Normal,
						    q(suggestedName(key)), &ok)
				      .trimmed();
	if (!ok)
		return false;
	SavedDevice d;
	d.identity = *identity;
	d.alias = alias.toStdString();
	auto it = nearby_.find(key);
	if (it != nearby_.end())
		d.lastKnownName = it->second.adv.name;
	if (it != nearby_.end() && !it->second.adv.identity.windowsDeviceId.empty())
		d.identity.windowsDeviceId = it->second.adv.identity.windowsDeviceId;
	if (!plugin::registry().save(d))
		askPlain(this, QStringLiteral("Save sensor"),
			 QStringLiteral("The sensor could not be saved to disk. It will work until OBS closes."),
			 QMessageBox::Ok, QMessageBox::Ok, QMessageBox::Warning);
	const bool paired = it != nearby_.end() && it->second.paired;
	if (identity->isResolvablePrivate() && !paired)
		askPlain(this, QStringLiteral("Tip: pair this device with Windows"),
			 QStringLiteral("This device uses a changing (private) Bluetooth address, as many watches and "
					"phones do. It works now, but once the address changes OBS cannot recognise "
					"it again, and it never guesses by name.\n\nFor reliable automatic "
					"reconnection, pair it once in Windows Settings > Bluetooth & devices > Add "
					"device, then select the \"Paired with Windows\" entry here."),
			 QMessageBox::Ok, QMessageBox::Ok, QMessageBox::Information);
	rebuild();
	return true;
}

void SensorDialog::onSave()
{
	const std::string key = selectedKey();
	if (!key.empty())
		saveWithAlias(key);
}

void SensorDialog::onRename()
{
	const std::string key = selectedKey();
	auto saved = plugin::registry().find(key);
	if (!saved)
		return;
	bool ok = false;
	const QString alias = QInputDialog::getText(this, QStringLiteral("Rename sensor"),
						    QStringLiteral("Name for this sensor:"), QLineEdit::Normal,
						    q(saved->displayName()), &ok)
				      .trimmed();
	if (ok) {
		plugin::registry().rename(key, alias.toStdString());
		rebuild();
	}
}

void SensorDialog::onForget()
{
	const std::string key = selectedKey();
	auto saved = plugin::registry().find(key);
	if (!saved)
		return;
	QString text = QStringLiteral("Forget \"%1\"?").arg(q(saved->displayName()));
	if (key == currentKey_)
		text += QStringLiteral("\n\nThis source is using it. It will keep using it until you select another sensor.");
	if (askPlain(this, QStringLiteral("Forget sensor"), text) != QMessageBox::Yes)
		return;
	plugin::registry().forget(key);
	rebuild();
}

void SensorDialog::onUse()
{
	const std::string key = selectedKey();
	if (key.empty())
		return;
	auto near = nearby_.find(key);
	if (!selectedIsSaved() && near != nearby_.end() && !near->second.adv.advertisesHeartRate && !near->second.paired) {
		const auto answer = askPlain(
			this, QStringLiteral("Not a heart-rate sensor?"),
			QStringLiteral("This device does not announce a heart-rate service, so it is probably not a "
				       "heart-rate sensor.\n\nIf you continue, OBS connects to it once only to check. "
				       "Nothing on the device is changed. If it has no heart-rate service it is left "
				       "alone and the source stays invisible.\n\nContinue?"),
			QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
		if (answer != QMessageBox::Yes)
			return;
	}
	if (!selectedIsSaved() && !saveWithAlias(key))
		return;
	chosen_ = key;
	accept();
}

bool runSensorDialog(const std::string &currentKey, std::string &chosenKey)
{
	QWidget *parent = QApplication::activeWindow();
	if (!parent)
		parent = static_cast<QWidget *>(obs_frontend_get_main_window());
	SensorDialog dialog(currentKey, parent);
	if (dialog.exec() != QDialog::Accepted)
		return false;
	chosenKey = dialog.chosenKey();
	return true;
}

} // namespace blehr
