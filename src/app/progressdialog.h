#pragma once

#include "app/jobrunner.h"

#include <QDialog>
#include <QElapsedTimer>

#include <deque>

class QLabel;
class QProgressBar;
class QPushButton;
class QTimer;

struct JobOutcome {
    bool ok = false;
    bool cancelled = false;
    QString error;
    vyra::OperationResult result;
};

// Modal progress window with throughput, ratio and ETA, shown only when a job
// takes longer than a moment (quick jobs never flash a window).
class ProgressDialog final : public QDialog {
    Q_OBJECT
public:
    static JobOutcome run(QWidget *parent, const QString &title, const QString &subtitle, JobRunner::Work work);

protected:
    void reject() override;

private:
    ProgressDialog(const QString &title, const QString &subtitle, QWidget *parent);
    void refresh();

    JobRunner *m_runner = nullptr;
    QLabel *m_title = nullptr;
    QLabel *m_subtitle = nullptr;
    QLabel *m_item = nullptr;
    QLabel *m_processed = nullptr;
    QLabel *m_speed = nullptr;
    QLabel *m_time = nullptr;
    QLabel *m_ratio = nullptr;
    QLabel *m_backend = nullptr;
    QProgressBar *m_bar = nullptr;
    QPushButton *m_cancel = nullptr;
    QTimer *m_timer = nullptr;
    struct Sample { double seconds; std::uint64_t bytes; };
    std::deque<Sample> m_samples;
};
