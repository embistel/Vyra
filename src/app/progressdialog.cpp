#include "app/progressdialog.h"
#include "app/qtbridge.h"

#include <QEventLoop>
#include <QFontMetrics>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>

namespace {
QString phaseText(vyra::Phase phase)
{
    switch (phase) {
    case vyra::Phase::Scanning: return ProgressDialog::tr("Scanning files…");
    case vyra::Phase::Compressing: return ProgressDialog::tr("Compressing…");
    case vyra::Phase::Extracting: return ProgressDialog::tr("Extracting…");
    case vyra::Phase::Testing: return ProgressDialog::tr("Testing…");
    case vyra::Phase::Finalizing: return ProgressDialog::tr("Finishing…");
    }
    return {};
}
}

ProgressDialog::ProgressDialog(const QString &title, const QString &subtitle, QWidget *parent) : QDialog(parent)
{
    setWindowTitle(title);
    setWindowIcon(QIcon(QStringLiteral(":/icons/app.svg")));
    setWindowFlag(Qt::WindowContextHelpButtonHint, false);
    setMinimumWidth(520);
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(18, 16, 18, 14);
    layout->setSpacing(8);

    m_title = new QLabel(title, this);
    m_title->setObjectName(QStringLiteral("DialogTitle"));
    m_subtitle = new QLabel(subtitle, this);
    m_subtitle->setObjectName(QStringLiteral("DialogSubtitle"));
    m_subtitle->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(m_title);
    layout->addWidget(m_subtitle);

    m_bar = new QProgressBar(this);
    m_bar->setRange(0, 0);
    m_bar->setTextVisible(true);
    layout->addWidget(m_bar);
    m_item = new QLabel(this);
    m_item->setObjectName(QStringLiteral("DialogSubtitle"));
    layout->addWidget(m_item);

    auto *form = new QFormLayout;
    form->setHorizontalSpacing(18);
    form->setVerticalSpacing(4);
    m_processed = new QLabel(QStringLiteral("—"), this);
    m_speed = new QLabel(QStringLiteral("—"), this);
    m_time = new QLabel(QStringLiteral("—"), this);
    m_ratio = new QLabel(QStringLiteral("—"), this);
    m_backend = new QLabel(QStringLiteral("—"), this);
    form->addRow(tr("Processed:"), m_processed);
    form->addRow(tr("Speed:"), m_speed);
    form->addRow(tr("Elapsed / remaining:"), m_time);
    form->addRow(tr("Ratio:"), m_ratio);
    form->addRow(tr("Engine:"), m_backend);
    layout->addLayout(form);

    auto *buttons = new QHBoxLayout;
    buttons->addStretch(1);
    m_cancel = new QPushButton(tr("Cancel"), this);
    buttons->addWidget(m_cancel);
    layout->addLayout(buttons);
    connect(m_cancel, &QPushButton::clicked, this, &ProgressDialog::reject);

    m_timer = new QTimer(this);
    m_timer->setInterval(120);
    connect(m_timer, &QTimer::timeout, this, &ProgressDialog::refresh);
}

void ProgressDialog::reject()
{
    if (!m_runner || m_runner->isCancelRequested()) return;
    m_runner->cancel();
    m_cancel->setEnabled(false);
    m_cancel->setText(tr("Cancelling…"));
}

void ProgressDialog::refresh()
{
    const vyra::Progress p = m_runner->latest();
    if (p.bytesTotal > 0 && p.phase != vyra::Phase::Scanning) {
        m_bar->setRange(0, 1000);
        m_bar->setValue(int(1000.0 * double(p.bytesDone) / double(p.bytesTotal)));
        m_bar->setFormat(QStringLiteral("%1%").arg(100.0 * double(p.bytesDone) / double(p.bytesTotal), 0, 'f', 1));
    } else {
        m_bar->setRange(0, 0);
    }
    const QString item = fromUtf8(p.currentItem);
    m_item->setText(phaseText(p.phase) + QStringLiteral("  ") +
                    m_item->fontMetrics().elidedText(item, Qt::ElideMiddle, qMax(200, m_item->width() - 160)));
    if (p.phase == vyra::Phase::Scanning) {
        m_processed->setText(tr("%1 files · %2").arg(p.filesTotal).arg(formatSize(p.bytesTotal)));
    } else {
        m_processed->setText(tr("%1 / %2 · %3 / %4 files")
                                 .arg(formatSize(p.bytesDone), formatSize(p.bytesTotal))
                                 .arg(p.filesDone)
                                 .arg(p.filesTotal));
    }
    // Speed over the last two seconds; overall average while warming up.
    m_samples.push_back({p.elapsedSeconds, p.bytesDone});
    while (m_samples.size() > 2 && m_samples.back().seconds - m_samples.front().seconds > 2.0) m_samples.pop_front();
    double speed = 0;
    if (m_samples.size() >= 2 && m_samples.back().seconds > m_samples.front().seconds)
        speed = double(m_samples.back().bytes - m_samples.front().bytes) / (m_samples.back().seconds - m_samples.front().seconds);
    else if (p.elapsedSeconds > 0)
        speed = double(p.bytesDone) / p.elapsedSeconds;
    m_speed->setText(speed > 0 ? formatSize(std::uint64_t(speed)) + QStringLiteral("/s") : QStringLiteral("—"));
    const double remaining = speed > 0 && p.bytesTotal > p.bytesDone ? double(p.bytesTotal - p.bytesDone) / speed : -1;
    m_time->setText(formatDuration(p.elapsedSeconds) + QStringLiteral(" / ") + formatDuration(remaining));
    if (p.outputBytes && p.bytesDone && p.phase == vyra::Phase::Compressing)
        m_ratio->setText(QStringLiteral("%1%").arg(100.0 * double(p.outputBytes) / double(p.bytesDone), 0, 'f', 1));
    else if (p.phase != vyra::Phase::Compressing)
        m_ratio->setText(QStringLiteral("—"));
    if (!p.backend.empty()) m_backend->setText(fromUtf8(p.backend));
}

JobOutcome ProgressDialog::run(QWidget *parent, const QString &title, const QString &subtitle, JobRunner::Work work)
{
    ProgressDialog dialog(title, subtitle, parent);
    dialog.setWindowModality(parent ? Qt::WindowModal : Qt::ApplicationModal);
    JobRunner runner(std::move(work));
    dialog.m_runner = &runner;
    QEventLoop loop;
    QObject::connect(&runner, &JobRunner::finished, &loop, &QEventLoop::quit);
    QTimer showTimer;
    showTimer.setSingleShot(true);
    QObject::connect(&showTimer, &QTimer::timeout, &dialog, [&dialog] {
        dialog.refresh();
        dialog.show();
        dialog.m_timer->start();
    });
    runner.start();
    showTimer.start(350);
    loop.exec();
    dialog.m_timer->stop();
    dialog.hide();
    JobOutcome outcome;
    outcome.ok = runner.succeeded();
    outcome.cancelled = runner.cancelled();
    outcome.error = runner.error();
    outcome.result = runner.result();
    return outcome;
}
