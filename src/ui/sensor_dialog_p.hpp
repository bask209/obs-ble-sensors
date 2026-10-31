// Private Qt dialog class (separate header so AUTOMOC picks up Q_OBJECT).
#pragma once

#include "core/ble_manager.hpp"

#include <QDialog>
#include <QTimer>

#include <map>
#include <memory>
#include <string>

class QCheckBox;
class QLabel;
class QPushButton;
class QTreeWidget;
class QTreeWidgetItem;

namespace blehr {

class SensorDialog : public QDialog {
	Q_OBJECT

public:
	SensorDialog(const std::string &currentKey, QWidget *parent);
	~SensorDialog() override;

	std::string chosenKey() const { return chosen_; }

private:
	struct Nearby {
		Advertisement adv;
		int64_t lastSeenNs = 0;
		bool paired = false; // Listed by Windows as a paired device.
	};

	void startScan();
	void onAdvertisement(const Advertisement &adv);
	void rebuild();
	void refreshStatus();
	void updateButtons();
	std::string selectedKey() const;
	bool selectedIsSaved() const;
	std::string suggestedName(const std::string &key) const;
	bool saveWithAlias(const std::string &key);

	void onSave();
	void onRename();
	void onForget();
	void onUse();

	std::string currentKey_;
	std::string chosen_;
	std::map<std::string, Nearby> nearby_;
	std::unique_ptr<BleManager::ScanToken> scanToken_;
	int64_t scanStartedNs_ = 0;

	QTreeWidget *list_ = nullptr;
	QCheckBox *showAll_ = nullptr;
	QLabel *status_ = nullptr;
	QPushButton *rescan_ = nullptr;
	QPushButton *save_ = nullptr;
	QPushButton *rename_ = nullptr;
	QPushButton *forget_ = nullptr;
	QPushButton *use_ = nullptr;
	QTimer timer_;
};

} // namespace blehr
