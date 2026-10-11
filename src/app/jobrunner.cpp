#include "app/jobrunner.h"

#include <QThread>

JobRunner::JobRunner(Work work, QObject *parent) : QObject(parent), m_work(std::move(work)) {}

JobRunner::~JobRunner()
{
    if (m_thread) {
        m_cancel = true;
        m_thread->wait();
        delete m_thread;
    }
}

void JobRunner::start()
{
    m_thread = QThread::create([this] {
        auto callback = [this](const vyra::Progress &p) {
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_progress = p;
            }
            return !m_cancel.load();
        };
        try {
            vyra::OperationResult r = m_work(callback);
            std::lock_guard<std::mutex> lock(m_mutex);
            m_result = std::move(r);
            m_ok = true;
        } catch (const vyra::Cancelled &) {
            m_cancelled = true;
        } catch (const std::exception &e) {
            m_error = QString::fromUtf8(e.what());
        }
    });
    connect(m_thread, &QThread::finished, this, &JobRunner::finished, Qt::QueuedConnection);
    m_thread->start();
}

vyra::Progress JobRunner::latest() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_progress;
}
