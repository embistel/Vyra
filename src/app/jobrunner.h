#pragma once

#include "core/archive.h"

#include <QObject>
#include <QString>

#include <atomic>
#include <functional>
#include <mutex>

class QThread;

// Runs one long core operation (compress / extract / test) on a worker thread.
// The core reports progress through its callback on that thread; the latest
// snapshot is kept here for the UI to poll, which keeps the core Qt-free.
class JobRunner final : public QObject {
    Q_OBJECT
public:
    using Work = std::function<vyra::OperationResult(const vyra::ProgressCallback &)>;

    explicit JobRunner(Work work, QObject *parent = nullptr);
    ~JobRunner() override;

    void start();
    void cancel() { m_cancel = true; }
    bool isCancelRequested() const { return m_cancel; }

    vyra::Progress latest() const;
    bool succeeded() const { return m_ok; }
    bool cancelled() const { return m_cancelled; }
    QString error() const { return m_error; }
    const vyra::OperationResult &result() const { return m_result; }

signals:
    void finished();

private:
    Work m_work;
    QThread *m_thread = nullptr;
    std::atomic<bool> m_cancel{false};
    mutable std::mutex m_mutex;
    vyra::Progress m_progress;
    vyra::OperationResult m_result;
    bool m_ok = false;
    bool m_cancelled = false;
    QString m_error;
};
