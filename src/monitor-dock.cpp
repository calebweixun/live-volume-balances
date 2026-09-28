/*
 * Live Volume Balancer monitor dock
 * Copyright (C) 2026 live-volume-balances contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <obs-frontend-api.h>
#include <obs-module.h>

#include <QColor>
#include <QComboBox>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPen>
#include <QPainter>
#include <QPaintEvent>
#include <QPalette>
#include <QPointF>
#include <QRectF>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QTimer>
#include <QToolButton>
#include <QVariant>
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

QString db_text(float value, const char *suffix)
{
	if (!std::isfinite(value) || value <= -119.0f)
		return QStringLiteral("—");
	return QStringLiteral("%1 %2").arg(value, 0, 'f', 1).arg(QString::fromUtf8(suffix));
}

class SegmentedMeter final : public QWidget {
public:
	explicit SegmentedMeter(QWidget *parent = nullptr) : QWidget(parent)
	{
		setMinimumHeight(12);
		setMaximumHeight(14);
		setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
		setToolTip(QStringLiteral("-60 dB to 0 dB"));
	}

	void setValue(float value, float marker = NAN)
	{
		m_value = std::isfinite(value) ? value : -120.0f;
		m_marker = std::isfinite(marker) ? marker : NAN;
		update();
	}

protected:
	void paintEvent(QPaintEvent *event) override
	{
		Q_UNUSED(event);
		QPainter painter(this);
		painter.setRenderHint(QPainter::Antialiasing);
		const QRectF bounds = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
		const QColor base = palette().color(QPalette::Base);
		const QColor border = palette().color(QPalette::Mid);
		painter.setPen(border);
		painter.setBrush(base);
		painter.drawRoundedRect(bounds, 4.0, 4.0);

		const QRectF inner = bounds.adjusted(2.0, 2.0, -2.0, -2.0);
		if (inner.width() <= 0.0)
			return;
		const double fraction = std::clamp((static_cast<double>(m_value) + 60.0) / 60.0, 0.0, 1.0);
		const double starts[] = {0.0, 0.66, 0.88};
		const double ends[] = {0.66, 0.88, 1.0};
		const QColor colors[] = {QColor(65, 184, 112), QColor(224, 177, 63), QColor(218, 83, 77)};
		for (int i = 0; i < 3; i++) {
			const double left = inner.left() + starts[i] * inner.width();
			const double right = inner.left() + ends[i] * inner.width();
			const QRectF segment(left, inner.top(), std::max(0.0, right - left - 1.0), inner.height());
			QColor inactive = colors[i];
			inactive.setAlpha(48);
			painter.fillRect(segment, inactive);
			if (fraction > starts[i]) {
				const double fill_right = inner.left() + std::min(fraction, ends[i]) * inner.width();
				const QRectF active(left, inner.top(),
						    std::max(0.0, std::min(fill_right, right) - left - 1.0),
						    inner.height());
				painter.fillRect(active, colors[i]);
			}
		}

		if (std::isfinite(m_marker)) {
			const double marker_fraction =
				std::clamp((static_cast<double>(m_marker) + 60.0) / 60.0, 0.0, 1.0);
			const double x = inner.left() + marker_fraction * inner.width();
			painter.setPen(QPen(palette().color(QPalette::WindowText), 1.5));
			painter.drawLine(QPointF(x, inner.top() - 1.0), QPointF(x, inner.bottom() + 1.0));
		}
	}

private:
	float m_value = -120.0f;
	float m_marker = NAN;
};

class MeterRow final : public QWidget {
public:
	explicit MeterRow(const QString &title, QWidget *parent = nullptr) : QWidget(parent)
	{
		auto *layout = new QHBoxLayout(this);
		layout->setContentsMargins(0, 0, 0, 0);
		layout->setSpacing(7);
		m_title = new QLabel(title, this);
		m_title->setMinimumWidth(78);
		m_title->setMaximumWidth(82);
		m_title->setStyleSheet(QStringLiteral("font-size: 11px; font-weight: 600;"));
		m_meter = new SegmentedMeter(this);
		m_value = new QLabel(QStringLiteral("—"), this);
		m_value->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
		m_value->setMinimumWidth(57);
		m_value->setStyleSheet(QStringLiteral("font-size: 11px; font-variant-numeric: tabular-nums;"));
		layout->addWidget(m_title);
		layout->addWidget(m_meter, 1);
		layout->addWidget(m_value);
	}

	void setReading(float value, float marker, const QString &suffix)
	{
		m_meter->setValue(value, marker);
		m_value->setText(db_text(value, suffix.toUtf8().constData()));
	}

private:
	QLabel *m_title = nullptr;
	QLabel *m_value = nullptr;
	SegmentedMeter *m_meter = nullptr;
};

class ValueCard final : public QFrame {
public:
	explicit ValueCard(const QString &title, QWidget *parent = nullptr) : QFrame(parent)
	{
		setFrameShape(QFrame::StyledPanel);
		setFrameShadow(QFrame::Plain);
		auto *layout = new QVBoxLayout(this);
		layout->setContentsMargins(9, 7, 9, 7);
		layout->setSpacing(1);
		m_title = new QLabel(title, this);
		m_title->setStyleSheet(QStringLiteral("font-size: 11px; font-weight: 600;"));
		m_value = new QLabel(QStringLiteral("—"), this);
		m_value->setStyleSheet(
			QStringLiteral("font-size: 19px; font-weight: 700; font-variant-numeric: tabular-nums;"));
		layout->addWidget(m_title);
		layout->addWidget(m_value);
	}

	void setValue(const QString &value) { m_value->setText(value); }

private:
	QLabel *m_title = nullptr;
	QLabel *m_value = nullptr;
};

class MonitorWidget final : public QWidget {
public:
	MonitorWidget()
	{
		setMinimumWidth(320);
		setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Minimum);
		auto *layout = new QVBoxLayout(this);
		layout->setContentsMargins(10, 8, 10, 10);
		layout->setSpacing(8);

		auto *header = new QHBoxLayout();
		header->setContentsMargins(0, 0, 0, 0);
		m_title = new QLabel(localized("LiveVolumeBalancer"), this);
		m_title->setStyleSheet(QStringLiteral("font-size: 13px; font-weight: 700;"));
		m_status_dot = new QLabel(QStringLiteral("●"), this);
		m_status_dot->setStyleSheet(QStringLiteral("font-size: 11px;"));
		m_status = new QLabel(localized("MonitorWaiting"), this);
		m_status->setStyleSheet(QStringLiteral("font-size: 11px; font-weight: 600;"));
		m_info = new QToolButton(this);
		m_info->setText(QStringLiteral("ⓘ"));
		m_info->setToolTip(localized("InfoTooltip"));
		m_info->setAccessibleName(localized("InfoTooltip"));
		m_info->setAutoRaise(true);
		header->addWidget(m_title);
		header->addStretch(1);
		header->addWidget(m_status_dot);
		header->addWidget(m_status);
		header->addWidget(m_info);
		layout->addLayout(header);

		m_source = new QComboBox(this);
		m_source->setVisible(false);
		m_source->setToolTip(localized("MonitorSource"));
		layout->addWidget(m_source);
		m_input = new MeterRow(localized("MonitorInput"), this);
		m_input_short = new QLabel(QStringLiteral("—"), this);
		m_output = new MeterRow(localized("MonitorOutput"), this);
		m_output_short = new QLabel(QStringLiteral("—"), this);
		for (QLabel *label : {m_input_short, m_output_short}) {
			label->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
			label->setStyleSheet(QStringLiteral("font-size: 11px;"));
		}
		layout->addWidget(m_input);
		layout->addWidget(m_input_short);
		layout->addWidget(m_output);
		layout->addWidget(m_output_short);

		auto *cards = new QHBoxLayout();
		cards->setSpacing(7);
		m_gain_card = new ValueCard(localized("MonitorGain"), this);
		m_peak_card = new ValueCard(localized("MonitorPeakHold"), this);
		m_peak_details = new QLabel(QStringLiteral("—"), this);
		m_peak_details->setStyleSheet(QStringLiteral("font-size: 11px;"));
		m_peak_meter = new SegmentedMeter(this);
		auto *peak_layout = qobject_cast<QVBoxLayout *>(m_peak_card->layout());
		peak_layout->addWidget(m_peak_details);
		peak_layout->addWidget(m_peak_meter);
		cards->addWidget(m_gain_card, 1);
		cards->addWidget(m_peak_card, 1);
		layout->addLayout(cards);

		connect(m_info, &QToolButton::clicked, this, [this] { showInfo(); });
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
	void showInfo()
	{
		QMessageBox box(QMessageBox::Information, localized("InfoTitle"), localized("InfoText"),
				QMessageBox::Ok, this);
		box.setTextFormat(Qt::PlainText);
		box.exec();
	}

	void refresh()
	{
		lvb_monitor_snapshot snapshot{};
		lvb_monitor_read(&snapshot);
		refreshSources(snapshot);
		const auto &stats = snapshot.stats;
		const bool available = snapshot.stats_available;
		m_status->setText(!available ? localized("MonitorWaiting")
					     : (stats.activity_open ? localized("MonitorConnected")
								    : localized("ActivityPaused")));
		m_status_dot->setStyleSheet(
			QStringLiteral("font-size: 11px; color: %1;")
				.arg(palette().color(stats.activity_open ? QPalette::Highlight : QPalette::Mid).name()));
		if (available) {
			m_input->setReading(stats.input_momentary_lufs, stats.target_lufs, QStringLiteral("LUFS"));
			m_output->setReading(stats.output_momentary_lufs, stats.target_lufs, QStringLiteral("LUFS"));
			m_input_short->setText(QStringLiteral("%1  %2").arg(
				localized("MonitorInputShort"), db_text(stats.input_short_term_lufs, "LUFS")));
			m_output_short->setText(QStringLiteral("%1  %2").arg(
				localized("MonitorOutputShort"), db_text(stats.output_short_term_lufs, "LUFS")));
			m_gain_card->setValue(QStringLiteral("%1%2 dB")
						      .arg(stats.gain_db > 0.05f ? QStringLiteral("+") : QString())
						      .arg(stats.gain_db, 0, 'f', 1));
			m_peak_card->setValue(db_text(stats.peak_hold_dbtp, "dBTP"));
			m_peak_details->setText(QStringLiteral("%1 %2").arg(localized("MonitorCeiling"),
									    db_text(stats.peak_ceiling_dbtp, "dBTP")));
			m_peak_meter->setValue(stats.peak_hold_dbtp, stats.peak_ceiling_dbtp);
		} else {
			m_input->setReading(-120.0f, NAN, QStringLiteral("LUFS"));
			m_output->setReading(-120.0f, NAN, QStringLiteral("LUFS"));
			m_input_short->setText(QStringLiteral("%1  —").arg(localized("MonitorInputShort")));
			m_output_short->setText(QStringLiteral("%1  —").arg(localized("MonitorOutputShort")));
			m_gain_card->setValue(QStringLiteral("—"));
			m_peak_card->setValue(QStringLiteral("—"));
			m_peak_details->setText(QStringLiteral("%1 —").arg(localized("MonitorCeiling")));
			m_peak_meter->setValue(-120.0f);
		}
	}

	void refreshSources(const lvb_monitor_snapshot &snapshot)
	{
		lvb_monitor_source sources[LVB_MONITOR_MAX_SOURCES]{};
		const size_t count = lvb_monitor_list_sources(sources, LVB_MONITOR_MAX_SOURCES);
		m_source->setVisible(count > 1);
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

	QLabel *m_title = nullptr;
	QLabel *m_status_dot = nullptr;
	QLabel *m_status = nullptr;
	QLabel *m_input_short = nullptr;
	QLabel *m_output_short = nullptr;
	QLabel *m_peak_details = nullptr;
	QComboBox *m_source = nullptr;
	QToolButton *m_info = nullptr;
	MeterRow *m_input = nullptr;
	MeterRow *m_output = nullptr;
	SegmentedMeter *m_peak_meter = nullptr;
	ValueCard *m_gain_card = nullptr;
	ValueCard *m_peak_card = nullptr;
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
