/*
 * Live Volume Balancer monitor dock
 * Copyright (C) 2026 live-volume-balances contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <obs-frontend-api.h>
#include <obs-module.h>

#include <QFormLayout>
#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QSignalBlocker>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
#include <cmath>

#include "monitor-bridge.h"

namespace {

constexpr auto DockId = "live_volume_balancer_monitor";

QString localized(const char *key)
{
	return QString::fromUtf8(obs_module_text(key));
}

int meter_value(float db, int minimum_db, int maximum_db)
{
	if (!std::isfinite(db))
		return 0;
	return std::clamp(static_cast<int>(std::lround(db)) - minimum_db, 0, maximum_db - minimum_db);
}

QString db_text(float value, const char *suffix)
{
	if (!std::isfinite(value) || value <= -119.0f)
		return QStringLiteral("—");
	return QStringLiteral("%1 %2").arg(value, 0, 'f', 1).arg(QString::fromUtf8(suffix));
}

QString mode_text(enum lvb_mode mode)
{
	switch (mode) {
	case LVB_MODE_WORSHIP:
		return localized("ModeWorship");
	case LVB_MODE_WORSHIP_ACOUSTIC:
		return localized("ModeWorshipAcoustic");
	case LVB_MODE_SERMON:
		return localized("ModeSermon");
	case LVB_MODE_AUTO_ASSIST:
		return localized("ModeAutoAssist");
	case LVB_MODE_CUSTOM:
	default:
		return localized("ModeCustom");
	}
}

class MonitorWidget final : public QWidget {
public:
	MonitorWidget()
	{
		setMinimumWidth(290);
		auto *layout = new QVBoxLayout(this);
		auto *form = new QFormLayout();
		m_status = new QLabel(localized("MonitorWaiting"), this);
		m_source = new QComboBox(this);
		m_mode = new QLabel(QStringLiteral("—"), this);
		m_momentary = new QProgressBar(this);
		m_shortTerm = new QProgressBar(this);
		m_peak = new QProgressBar(this);
		m_gain = new QProgressBar(this);
		m_activity = new QLabel(QStringLiteral("—"), this);
		m_calibration = new QProgressBar(this);
		m_calibration->setRange(0, 100);
		m_calibration->setFormat(localized("CalibrationIdle"));
		configureDbMeter(m_momentary, -60, 0);
		configureDbMeter(m_shortTerm, -60, 0);
		configureDbMeter(m_peak, -60, 6);
		m_gain->setRange(0, 42);
		m_gain->setFormat(QStringLiteral("%v dB"));

		form->addRow(localized("MonitorFeed"), m_status);
		form->addRow(localized("MonitorSource"), m_source);
		form->addRow(localized("MonitorMode"), m_mode);
		form->addRow(localized("MonitorMomentary"), m_momentary);
		form->addRow(localized("MonitorShortTerm"), m_shortTerm);
		form->addRow(localized("MonitorPeak"), m_peak);
		form->addRow(localized("MonitorGain"), m_gain);
		form->addRow(localized("MonitorVoice"), m_activity);
		form->addRow(localized("CalibrationProgress"), m_calibration);
		layout->addLayout(form);

		m_note = new QLabel(localized("MonitorNote"), this);
		m_note->setWordWrap(true);
		layout->addWidget(m_note);
		auto *buttons = new QHBoxLayout();
		m_start = new QPushButton(localized("CalibrationStart"), this);
		m_apply = new QPushButton(localized("CalibrationApply"), this);
		m_reset = new QPushButton(localized("CalibrationReset"), this);
		m_apply->setEnabled(false);
		m_reset->setEnabled(false);
		buttons->addWidget(m_start);
		buttons->addWidget(m_apply);
		buttons->addWidget(m_reset);
		layout->addLayout(buttons);

		connect(m_start, &QPushButton::clicked, this, [] { lvb_monitor_start_calibration(); });
		connect(m_apply, &QPushButton::clicked, this, [] { lvb_monitor_apply_calibration(); });
		connect(m_reset, &QPushButton::clicked, this, [] { lvb_monitor_reset_calibration(); });
		connect(m_source, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int index) {
			if (index >= 0)
				lvb_monitor_select_source(m_source->itemData(index).toUInt());
		});

		auto *timer = new QTimer(this);
		connect(timer, &QTimer::timeout, this, [this] { refresh(); });
		timer->start(100);
		refresh();
	}

private:
	static void configureDbMeter(QProgressBar *meter, int minimum_db, int maximum_db)
	{
		meter->setRange(0, maximum_db - minimum_db);
		meter->setFormat(QStringLiteral("%v"));
		meter->setTextVisible(true);
	}

	void refresh()
	{
		lvb_monitor_snapshot snapshot{};
		lvb_monitor_read(&snapshot);
		refreshSources(snapshot);
		const auto &stats = snapshot.stats;
		m_status->setText(snapshot.stats_available ? localized("MonitorConnected")
							   : localized("MonitorWaiting"));
		m_source->setEnabled(snapshot.source_available);
		m_mode->setText(snapshot.stats_available ? mode_text(stats.mode) : QStringLiteral("—"));
		if (snapshot.stats_available) {
			m_momentary->setValue(meter_value(stats.momentary_lufs, -60, 0));
			m_momentary->setFormat(db_text(stats.momentary_lufs, "LUFS"));
			m_shortTerm->setValue(meter_value(stats.short_term_lufs, -60, 0));
			m_shortTerm->setFormat(db_text(stats.short_term_lufs, "LUFS"));
			m_peak->setValue(meter_value(stats.true_peak_dbtp, -60, 6));
			m_peak->setFormat(db_text(stats.true_peak_dbtp, "dBTP"));
			m_gain->setValue(std::clamp(static_cast<int>(std::lround(stats.gain_db)) + 24, 0, 42));
			m_gain->setFormat(QStringLiteral("%1 dB").arg(stats.gain_db, 0, 'f', 1));
			m_activity->setText(stats.voice_active ? localized("VoiceLikeActive")
							       : localized("VoiceLikeInactive"));
		} else {
			m_momentary->setValue(0);
			m_momentary->setFormat(QStringLiteral("—"));
			m_shortTerm->setValue(0);
			m_shortTerm->setFormat(QStringLiteral("—"));
			m_peak->setValue(0);
			m_peak->setFormat(QStringLiteral("—"));
			m_gain->setValue(24);
			m_gain->setFormat(QStringLiteral("—"));
			m_activity->setText(QStringLiteral("—"));
		}

		m_start->setEnabled(snapshot.stats_available && !snapshot.calibration_waiting &&
				    !snapshot.calibration_action_pending && !stats.calibration_active);
		m_apply->setEnabled(snapshot.stats_available && stats.calibration_ready &&
				    !snapshot.calibration_action_pending);
		m_reset->setEnabled(snapshot.source_available);
		if (!snapshot.stats_available) {
			m_calibration->setValue(0);
			m_calibration->setFormat(localized("CalibrationIdle"));
		} else if (snapshot.calibration_waiting) {
			m_calibration->setValue(0);
			m_calibration->setFormat(localized("CalibrationWaiting"));
		} else if (stats.calibration_active) {
			m_calibration->setValue(
				std::clamp(static_cast<int>(std::lround(stats.calibration_progress * 100.0f)), 0, 100));
			m_calibration->setFormat(localized("CalibrationRunning"));
		} else if (stats.calibration_ready) {
			m_calibration->setValue(100);
			m_calibration->setFormat(localized("CalibrationSuggestion")
							 .arg(stats.calibration_measured_lufs, 0, 'f', 1)
							 .arg(stats.calibration_suggestion_db, 0, 'f', 1)
							 .arg(mode_text(stats.calibration_mode)));
		} else {
			m_calibration->setValue(0);
			m_calibration->setFormat(localized("CalibrationIdle"));
		}
	}

	void refreshSources(const lvb_monitor_snapshot &snapshot)
	{
		lvb_monitor_source sources[LVB_MONITOR_MAX_SOURCES]{};
		const size_t count = lvb_monitor_list_sources(sources, LVB_MONITOR_MAX_SOURCES);
		bool changed = m_source->count() != static_cast<int>(count);
		for (size_t i = 0; !changed && i < count; i++) {
			changed = m_source->itemData(static_cast<int>(i)).toUInt() != sources[i].instance_id ||
				  m_source->itemText(static_cast<int>(i)) != QString::fromUtf8(sources[i].name);
		}
		if (changed) {
			const QSignalBlocker blocker(m_source);
			m_source->clear();
			for (size_t i = 0; i < count; i++)
				m_source->addItem(QString::fromUtf8(sources[i].name),
						  QVariant::fromValue(sources[i].instance_id));
		}
		for (int i = 0; i < m_source->count(); i++) {
			if (m_source->itemData(i).toUInt() == snapshot.source_instance_id) {
				if (m_source->currentIndex() != i) {
					const QSignalBlocker blocker(m_source);
					m_source->setCurrentIndex(i);
				}
				break;
			}
		}
	}

	QLabel *m_status = nullptr;
	QComboBox *m_source = nullptr;
	QLabel *m_mode = nullptr;
	QLabel *m_activity = nullptr;
	QLabel *m_note = nullptr;
	QProgressBar *m_momentary = nullptr;
	QProgressBar *m_shortTerm = nullptr;
	QProgressBar *m_peak = nullptr;
	QProgressBar *m_gain = nullptr;
	QProgressBar *m_calibration = nullptr;
	QPushButton *m_start = nullptr;
	QPushButton *m_apply = nullptr;
	QPushButton *m_reset = nullptr;
};

} // namespace

extern "C" void lvb_monitor_dock_load(void)
{
	auto *widget = new MonitorWidget();
	if (!obs_frontend_add_dock_by_id(DockId, obs_module_text("MonitorDockTitle"), widget))
		delete widget;
}

extern "C" void lvb_monitor_dock_unload(void)
{
	obs_frontend_remove_dock(DockId);
}
