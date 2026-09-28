/*
 * Live Volume Balancer monitor dock
 * Copyright (C) 2026 live-volume-balances contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <obs-frontend-api.h>
#include <obs-module.h>

#include <QColor>
#include <QElapsedTimer>
#include <QFrame>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPen>
#include <QPainter>
#include <QPaintEvent>
#include <QPalette>
#include <QPointF>
#include <QRectF>
#include <QResizeEvent>
#include <QSizePolicy>
#include <QScrollArea>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

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

void set_label_text(QLabel *label, const QString &text)
{
	if (label && label->text() != text)
		label->setText(text);
}

class SegmentedMeter final : public QWidget {
public:
	explicit SegmentedMeter(QWidget *parent = nullptr) : QWidget(parent)
	{
		setMinimumHeight(10);
		setMaximumHeight(12);
		setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
		setToolTip(localized("MeterRangeHelp"));
	}

	void setValue(float value, float marker = NAN)
	{
		const float next_value = std::isfinite(value) ? value : -120.0f;
		const float next_marker = std::isfinite(marker) ? marker : NAN;
		const bool marker_changed = std::isfinite(next_marker) != std::isfinite(m_marker) ||
					    (std::isfinite(next_marker) && std::fabs(next_marker - m_marker) >= 0.1f);
		if (std::fabs(next_value - m_value) < 0.1f && !marker_changed)
			return;
		m_value = next_value;
		m_marker = next_marker;
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
		layout->setSpacing(5);
		m_title = new QLabel(title, this);
		m_title->setMinimumWidth(62);
		m_title->setMaximumWidth(72);
		m_title->setStyleSheet(QStringLiteral("font-size: 11px; font-weight: 600;"));
		m_meter = new SegmentedMeter(this);
		m_meter->setToolTip(localized("FastMeterHelp"));
		m_value = new QLabel(QStringLiteral("—"), this);
		m_value->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
		m_value->setMinimumWidth(76);
		m_value->setStyleSheet(QStringLiteral("font-size: 11px; font-variant-numeric: tabular-nums;"));
		layout->addWidget(m_title);
		layout->addWidget(m_meter, 1);
		layout->addWidget(m_value);
	}

	void setReading(float value, const QString &suffix)
	{
		m_meter->setValue(value);
		set_label_text(m_value, db_text(value, suffix.toUtf8().constData()));
	}

private:
	QLabel *m_title = nullptr;
	QLabel *m_value = nullptr;
	SegmentedMeter *m_meter = nullptr;
};

class SourceMonitorWidget final : public QFrame {
public:
	explicit SourceMonitorWidget(const lvb_monitor_source &source, QWidget *parent = nullptr)
		: QFrame(parent),
		  m_source(source)
	{
		setFrameShape(QFrame::StyledPanel);
		setFrameShadow(QFrame::Plain);
		setMinimumWidth(340);
		auto *layout = new QVBoxLayout(this);
		layout->setContentsMargins(6, 4, 6, 4);
		layout->setSpacing(2);

		auto *header = new QHBoxLayout();
		header->setContentsMargins(0, 0, 0, 0);
		header->setSpacing(4);
		m_name = new QLabel(QString::fromUtf8(source.name), this);
		m_name->setStyleSheet(QStringLiteral("font-size: 12px; font-weight: 700;"));
		m_name->setMinimumWidth(0);
		m_name->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
		m_name->setToolTip(QString::fromUtf8(source.name));
		m_status_dot = new QLabel(QStringLiteral("●"), this);
		m_status_dot->setStyleSheet(QStringLiteral("font-size: 11px;"));
		m_status = new QLabel(localized("MonitorWaiting"), this);
		m_status->setStyleSheet(QStringLiteral("font-size: 11px; font-weight: 600;"));
		m_settings = new QToolButton(this);
		m_settings->setText(QStringLiteral("⚙"));
		m_settings->setToolTip(localized("OpenFilterSettings"));
		m_settings->setAccessibleName(localized("OpenFilterSettings"));
		m_settings->setAutoRaise(true);
		m_info = new QToolButton(this);
		m_info->setText(QStringLiteral("ⓘ"));
		m_info->setToolTip(localized("InfoTooltip"));
		m_info->setAccessibleName(localized("InfoTooltip"));
		m_info->setAutoRaise(true);
		header->addWidget(m_name, 1);
		header->addWidget(m_status_dot);
		header->addWidget(m_status);
		header->addWidget(m_settings);
		header->addWidget(m_info);
		layout->addLayout(header);

		m_input = new MeterRow(localized("MonitorInputFast"), this);
		m_output = new MeterRow(localized("MonitorOutputFast"), this);
		layout->addWidget(m_input);
		layout->addWidget(m_output);

		m_momentary = new QLabel(localized("MonitorMomentaryUnavailable"), this);
		m_short_term = new QLabel(localized("MonitorShortTermUnavailable"), this);
		for (QLabel *label : {m_momentary, m_short_term}) {
			label->setStyleSheet(QStringLiteral("font-size: 11px; font-variant-numeric: tabular-nums;"));
			label->setToolTip(localized("RollingReadingsHelp"));
		}
		layout->addWidget(m_momentary);
		layout->addWidget(m_short_term);

		m_summary = new QLabel(localized("MonitorSummaryUnavailable"), this);
		m_summary->setStyleSheet(QStringLiteral("font-size: 11px; font-variant-numeric: tabular-nums;"));
		layout->addWidget(m_summary);
		m_peak_meter = new SegmentedMeter(this);
		m_peak_meter->setToolTip(localized("PeakMeterHelp"));
		layout->addWidget(m_peak_meter);

		m_latency = new QLabel(localized("MonitorLatencyUnavailable"), this);
		m_latency->setStyleSheet(QStringLiteral("font-size: 11px; font-variant-numeric: tabular-nums;"));
		m_latency->setToolTip(localized("MonitorLatencyHelp"));
		layout->addWidget(m_latency);

		connect(m_settings, &QToolButton::clicked, this,
			[this] { lvb_monitor_open_source_properties(m_source.instance_id); });
		connect(m_info, &QToolButton::clicked, this, [this] { showInfo(); });
	}

	bool matches(const lvb_monitor_source &source) const
	{
		return m_source.instance_id == source.instance_id && m_source.telemetry_slot == source.telemetry_slot &&
		       std::strncmp(m_source.name, source.name, sizeof(m_source.name)) == 0;
	}

	void refresh()
	{
		lvb_monitor_snapshot snapshot{};
		lvb_monitor_read_source(m_source.telemetry_slot, m_source.instance_id, &snapshot);
		if (snapshot.stats_available && snapshot.sequence != m_last_sequence) {
			m_last_sequence = snapshot.sequence;
			m_last_audio_update.start();
		}
		const bool available = snapshot.source_available && snapshot.stats_available &&
				       m_last_audio_update.isValid() && m_last_audio_update.elapsed() < 300;
		const auto &stats = snapshot.stats;
		const QString status_text = !available ? localized("MonitorWaiting")
						       : (stats.activity_open ? localized("MonitorConnected")
									      : localized("ActivityPaused"));
		set_label_text(m_status, status_text);
		const QColor status_color =
			palette().color(available && stats.activity_open ? QPalette::Highlight : QPalette::Mid);
		const QString status_style = QStringLiteral("font-size: 11px; color: %1;").arg(status_color.name());
		if (m_status_dot->styleSheet() != status_style)
			m_status_dot->setStyleSheet(status_style);
		if (m_settings->isEnabled() != snapshot.source_available)
			m_settings->setEnabled(snapshot.source_available);

		if (!available) {
			m_input->setReading(-120.0f, QStringLiteral("dBFS"));
			m_output->setReading(-120.0f, QStringLiteral("dBFS"));
			set_label_text(m_momentary, localized("MonitorMomentaryUnavailable"));
			set_label_text(m_short_term, localized("MonitorShortTermUnavailable"));
			set_label_text(m_summary, localized("MonitorSummaryUnavailable"));
			m_peak_meter->setValue(-120.0f);
			set_label_text(m_latency, localized("MonitorLatencyUnavailable"));
			return;
		}

		m_input->setReading(stats.input_fast_rms_dbfs, QStringLiteral("dBFS"));
		m_output->setReading(stats.output_fast_rms_dbfs, QStringLiteral("dBFS"));
		set_label_text(m_momentary, localized("MonitorMomentaryFormat")
						    .arg(db_text(stats.input_momentary_lufs, "LUFS"),
							 db_text(stats.output_momentary_lufs, "LUFS")));
		set_label_text(m_short_term, localized("MonitorShortTermFormat")
						     .arg(db_text(stats.input_short_term_lufs, "LUFS"),
							  db_text(stats.output_short_term_lufs, "LUFS")));
		const QString gain = QStringLiteral("%1%2 dB")
					     .arg(stats.gain_db > 0.05f ? QStringLiteral("+") : QString())
					     .arg(stats.gain_db, 0, 'f', 1);
		set_label_text(m_summary, localized("MonitorSummaryFormat")
						  .arg(gain, db_text(stats.peak_hold_dbtp, "dBTP"),
						       db_text(stats.peak_ceiling_dbtp, "dBTP")));
		m_peak_meter->setValue(stats.peak_hold_dbtp, stats.peak_ceiling_dbtp);
		if (std::isfinite(stats.sample_rate_hz) && stats.sample_rate_hz > 0.0f) {
			const float latency_ms = lvb_output_latency_ms(stats.sample_rate_hz);
			set_label_text(m_latency, localized("MonitorLatencyFormat")
							  .arg(QString::number(latency_ms, 'f', 3),
							       QString::number(LVB_TRUE_PEAK_LATENCY)));
		} else {
			set_label_text(m_latency, localized("MonitorLatencyUnavailable"));
		}
	}

protected:
	void resizeEvent(QResizeEvent *event) override
	{
		QFrame::resizeEvent(event);
		if (!m_name)
			return;
		const QString full_name = QString::fromUtf8(m_source.name);
		const QString elided =
			QFontMetrics(m_name->font()).elidedText(full_name, Qt::ElideMiddle, m_name->width());
		set_label_text(m_name, elided);
	}

private:
	void showInfo()
	{
		QMessageBox box(QMessageBox::Information, localized("InfoTitle"), localized("InfoText"),
				QMessageBox::Ok, this);
		box.setTextFormat(Qt::PlainText);
		box.exec();
	}

	lvb_monitor_source m_source{};
	QLabel *m_name = nullptr;
	QLabel *m_status_dot = nullptr;
	QLabel *m_status = nullptr;
	QLabel *m_momentary = nullptr;
	QLabel *m_short_term = nullptr;
	QLabel *m_summary = nullptr;
	QLabel *m_latency = nullptr;
	QToolButton *m_settings = nullptr;
	QToolButton *m_info = nullptr;
	MeterRow *m_input = nullptr;
	MeterRow *m_output = nullptr;
	SegmentedMeter *m_peak_meter = nullptr;
	QElapsedTimer m_last_audio_update;
	uint32_t m_last_sequence = 0;
};

class MonitorWidget final : public QWidget {
public:
	MonitorWidget()
	{
		setMinimumWidth(360);
		setSizePolicy(QSizePolicy::Preferred, QSizePolicy::MinimumExpanding);
		auto *layout = new QVBoxLayout(this);
		layout->setContentsMargins(4, 4, 4, 4);
		layout->setSpacing(0);

		m_scroll = new QScrollArea(this);
		m_scroll->setWidgetResizable(true);
		m_scroll->setFrameShape(QFrame::NoFrame);
		m_content = new QWidget(m_scroll);
		m_content_layout = new QVBoxLayout(m_content);
		m_content_layout->setContentsMargins(2, 2, 2, 2);
		m_content_layout->setSpacing(3);
		m_scroll->setWidget(m_content);
		layout->addWidget(m_scroll);

		auto *refresh_timer = new QTimer(this);
		refresh_timer->setTimerType(Qt::PreciseTimer);
		connect(refresh_timer, &QTimer::timeout, this, [this] { refresh(); });
		refresh_timer->start(16);

		auto *source_timer = new QTimer(this);
		source_timer->setTimerType(Qt::CoarseTimer);
		connect(source_timer, &QTimer::timeout, this, [this] { refreshSources(); });
		source_timer->start(400);
		refreshSources();
	}

private:
	void refresh()
	{
		for (SourceMonitorWidget *source : m_source_widgets)
			source->refresh();
	}

	void refreshSources()
	{
		lvb_monitor_source sources[LVB_MONITOR_MAX_SOURCES]{};
		const size_t count = lvb_monitor_list_sources(sources, LVB_MONITOR_MAX_SOURCES);
		bool changed = !m_sources_initialized || m_source_widgets.size() != count;
		for (size_t i = 0; !changed && i < count; i++)
			changed = !m_source_widgets[i]->matches(sources[i]);
		if (!changed)
			return;

		while (QLayoutItem *item = m_content_layout->takeAt(0)) {
			if (QWidget *widget = item->widget())
				delete widget;
			delete item;
		}
		m_source_widgets.clear();
		if (count == 0) {
			auto *empty = new QLabel(localized("MonitorWaiting"), m_content);
			empty->setAlignment(Qt::AlignCenter);
			empty->setStyleSheet(QStringLiteral("font-size: 11px;"));
			m_content_layout->addWidget(empty);
		} else {
			for (size_t i = 0; i < count; i++) {
				auto *widget = new SourceMonitorWidget(sources[i], m_content);
				m_content_layout->addWidget(widget);
				m_source_widgets.push_back(widget);
			}
			m_content_layout->addStretch(1);
		}
		m_sources_initialized = true;
	}

	QScrollArea *m_scroll = nullptr;
	QWidget *m_content = nullptr;
	QVBoxLayout *m_content_layout = nullptr;
	std::vector<SourceMonitorWidget *> m_source_widgets;
	bool m_sources_initialized = false;
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
