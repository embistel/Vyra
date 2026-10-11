#include "app/compressdialog.h"
#include "app/qtbridge.h"

#include "core/backend.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include <QToolButton>
#include <QVBoxLayout>

namespace {
const vyra::GpuStatus &gpuStatus()
{
    static const vyra::GpuStatus status = vyra::queryGpu();
    return status;
}

void selectData(QComboBox *box, const QVariant &value)
{
    const int i = box->findData(value);
    if (i >= 0) box->setCurrentIndex(i);
}
}

QString CompressDialog::defaultArchivePath(const QStringList &inputs, const QString &extension)
{
    if (inputs.isEmpty()) return QDir::home().filePath(QStringLiteral("Archive.") + extension);
    const QFileInfo first(inputs.first());
    QDir dir = first.absoluteDir();
    QString base = inputs.size() == 1 ? (first.isDir() ? first.fileName() : first.completeBaseName()) : dir.dirName();
    if (base.isEmpty()) base = QStringLiteral("Archive");
    QString candidate = dir.filePath(base + u'.' + extension);
    for (int i = 2; QFileInfo::exists(candidate); ++i) candidate = dir.filePath(QStringLiteral("%1 (%2).%3").arg(base).arg(i).arg(extension));
    return candidate;
}

vyra::CompressOptions CompressDialog::savedOptions(vyra::Format format)
{
    QSettings s;
    vyra::CompressOptions o;
    o.format = format;
    if (format == vyra::Format::Zip) {
        o.method = vyra::Method(s.value(QStringLiteral("compress/methodZip"), int(vyra::Method::Deflate)).toInt());
        if (!vyra::methodSupportedFor(format, o.method)) o.method = vyra::Method::Deflate;
    } else {
        o.method = vyra::Method(s.value(QStringLiteral("compress/method7z"), int(vyra::Method::LZ4)).toInt());
        if (!vyra::methodSupportedFor(format, o.method)) o.method = vyra::Method::LZ4;
    }
    o.level = s.value(QStringLiteral("compress/level"), 1).toInt();
    o.backend = vyra::BackendKind(s.value(QStringLiteral("compress/backend"), int(vyra::BackendKind::Auto)).toInt());
    o.solidBlockSize = s.value(QStringLiteral("compress/solid"), QVariant::fromValue<qulonglong>(4ull << 30)).toULongLong();
    o.chunkSize = s.value(QStringLiteral("compress/chunk"), 65536).toULongLong();
    return o;
}

CompressDialog::CompressDialog(const QStringList &inputs, QWidget *parent) : QDialog(parent), m_inputs(inputs)
{
    setWindowTitle(tr("Add to Archive"));
    setWindowIcon(QIcon(QStringLiteral(":/icons/app.svg")));
    setWindowFlag(Qt::WindowContextHelpButtonHint, false);
    setMinimumWidth(600);
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(18, 16, 18, 14);
    layout->setSpacing(10);

    auto *title = new QLabel(tr("Create a GPU accelerated archive"), this);
    title->setObjectName(QStringLiteral("DialogTitle"));
    layout->addWidget(title);
    const QString what = inputs.size() == 1 ? QFileInfo(inputs.first()).fileName() : tr("%1 items").arg(inputs.size());
    auto *subtitle = new QLabel(tr("Source: %1").arg(what), this);
    subtitle->setObjectName(QStringLiteral("DialogSubtitle"));
    layout->addWidget(subtitle);

    auto *form = new QFormLayout;
    form->setHorizontalSpacing(14);
    form->setVerticalSpacing(8);
    auto *pathRow = new QHBoxLayout;
    m_path = new QLineEdit(this);
    auto *browse = new QToolButton(this);
    browse->setText(QStringLiteral("…"));
    pathRow->addWidget(m_path, 1);
    pathRow->addWidget(browse);
    form->addRow(tr("Archive:"), pathRow);

    m_format = new QComboBox(this);
    m_format->addItem(QStringLiteral("7z"), int(vyra::Format::SevenZip));
    m_format->addItem(QStringLiteral("zip"), int(vyra::Format::Zip));
    form->addRow(tr("Format:"), m_format);

    m_method = new QComboBox(this);
    form->addRow(tr("Method:"), m_method);

    m_level = new QComboBox(this);
    m_level->addItem(tr("0 · Fastest"), 0);
    m_level->addItem(tr("1 · Fast (recommended)"), 1);
    m_level->addItem(tr("2 · Normal"), 2);
    m_level->addItem(tr("3 · Normal+"), 3);
    m_level->addItem(tr("4 · Good"), 4);
    m_level->addItem(tr("5 · Best"), 5);
    form->addRow(tr("Level:"), m_level);

    m_backend = new QComboBox(this);
    m_backend->addItem(tr("Automatic (GPU when available)"), int(vyra::BackendKind::Auto));
    m_backend->addItem(tr("GPU (NVIDIA nvCOMP)"), int(vyra::BackendKind::Gpu));
    m_backend->addItem(tr("CPU (all cores)"), int(vyra::BackendKind::Cpu));
    form->addRow(tr("Engine:"), m_backend);

    m_solid = new QComboBox(this);
    m_solid->addItem(tr("Off (one block per file)"), QVariant::fromValue<qulonglong>(0));
    m_solid->addItem(QStringLiteral("256 MiB"), QVariant::fromValue<qulonglong>(256ull << 20));
    m_solid->addItem(QStringLiteral("1 GiB"), QVariant::fromValue<qulonglong>(1ull << 30));
    m_solid->addItem(QStringLiteral("4 GiB"), QVariant::fromValue<qulonglong>(4ull << 30));
    m_solid->addItem(QStringLiteral("16 GiB"), QVariant::fromValue<qulonglong>(16ull << 30));
    form->addRow(tr("Solid block:"), m_solid);

    m_chunk = new QComboBox(this);
    m_chunk->addItem(QStringLiteral("64 KiB"), QVariant::fromValue<qulonglong>(64u << 10));
    m_chunk->addItem(QStringLiteral("256 KiB"), QVariant::fromValue<qulonglong>(256u << 10));
    m_chunk->addItem(QStringLiteral("1 MiB"), QVariant::fromValue<qulonglong>(1u << 20));
    m_chunk->addItem(QStringLiteral("4 MiB"), QVariant::fromValue<qulonglong>(4u << 20));
    m_chunk->setToolTip(tr("Independent compression unit. Larger chunks compress Zstd/LZ4 slightly better; Deflate always uses 64 KiB."));
    form->addRow(tr("Chunk size:"), m_chunk);
    layout->addLayout(form);

    m_engine = new QLabel(this);
    m_engine->setObjectName(QStringLiteral("EngineChip"));
    const auto &gpu = gpuStatus();
    m_engine->setText(gpu.available ? tr("GPU ready: %1").arg(fromUtf8(gpu.description))
                                    : tr("GPU unavailable (%1) — the CPU engine will be used.").arg(fromUtf8(gpu.description)));
    m_engine->setWordWrap(true);
    layout->addWidget(m_engine);

    m_compat = new QLabel(this);
    m_compat->setObjectName(QStringLiteral("CompatNote"));
    m_compat->setWordWrap(true);
    layout->addWidget(m_compat);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    buttons->button(QDialogButtonBox::Ok)->setText(tr("Compress"));
    layout->addWidget(buttons);

    connect(buttons, &QDialogButtonBox::accepted, this, &CompressDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(browse, &QToolButton::clicked, this, &CompressDialog::browse);
    connect(m_format, &QComboBox::currentIndexChanged, this, &CompressDialog::formatChanged);
    connect(m_method, &QComboBox::currentIndexChanged, this, &CompressDialog::methodChanged);

    const QString lastFormat = QSettings().value(QStringLiteral("compress/format"), QStringLiteral("7z")).toString();
    const vyra::Format format = lastFormat == QStringLiteral("zip") ? vyra::Format::Zip : vyra::Format::SevenZip;
    const vyra::CompressOptions saved = savedOptions(format);
    m_path->setText(QDir::toNativeSeparators(defaultArchivePath(inputs, QString::fromLatin1(vyra::formatName(format)))));
    selectData(m_format, int(format));
    formatChanged();
    selectData(m_method, int(saved.method));
    selectData(m_level, saved.level);
    selectData(m_backend, int(saved.backend));
    selectData(m_solid, QVariant::fromValue<qulonglong>(saved.solidBlockSize));
    selectData(m_chunk, QVariant::fromValue<qulonglong>(saved.chunkSize));
    methodChanged();
}

void CompressDialog::formatChanged()
{
    const auto format = vyra::Format(m_format->currentData().toInt());
    const QSignalBlocker block(m_method);
    m_method->clear();
    if (format == vyra::Format::SevenZip) {
        m_method->addItem(tr("LZ4 — fastest"), int(vyra::Method::LZ4));
        m_method->addItem(tr("Zstd — fast, smaller"), int(vyra::Method::Zstd));
        m_method->addItem(tr("Deflate — opens in official 7-Zip"), int(vyra::Method::Deflate));
        m_method->addItem(tr("Store — no compression"), int(vyra::Method::Store));
    } else {
        m_method->addItem(tr("Deflate — opens everywhere"), int(vyra::Method::Deflate));
        m_method->addItem(tr("Store — no compression"), int(vyra::Method::Store));
    }
    selectData(m_method, int(savedOptions(format).method));
    // Keep the file name, switch the extension.
    QString path = m_path->text();
    const QString ext = QString::fromLatin1(vyra::formatName(format));
    const QFileInfo info(path);
    if (!path.isEmpty()) {
        const QString suffix = info.suffix().toLower();
        if (suffix == QStringLiteral("7z") || suffix == QStringLiteral("zip")) path.chop(suffix.size() + 1);
        m_path->setText(path + u'.' + ext);
    }
    m_solid->setEnabled(format == vyra::Format::SevenZip);
    methodChanged();
}

void CompressDialog::methodChanged()
{
    const auto format = vyra::Format(m_format->currentData().toInt());
    const auto method = vyra::Method(m_method->currentData().toInt());
    m_level->setEnabled(method != vyra::Method::Store);
    m_backend->setEnabled(method != vyra::Method::Store);
    m_chunk->setEnabled(method == vyra::Method::LZ4 || method == vyra::Method::Zstd);
    QString note;
    if (format == vyra::Format::Zip)
        note = tr("Standard ZIP: opens in Windows Explorer, 7-Zip and every other unzip tool.");
    else if (method == vyra::Method::Deflate || method == vyra::Method::Store)
        note = tr("Standard 7z: opens in the official 7-Zip, NanaZip and other 7z tools.");
    else
        note = tr("7z with %1: opens in NanaZip and 7-Zip-zstd. The official 7-Zip needs the "
                  "lz4/zstd codec plugin. Pick Deflate for the widest 7z compatibility.")
                   .arg(QString::fromLatin1(vyra::methodName(method)));
    m_compat->setText(note);
}

void CompressDialog::browse()
{
    const auto format = vyra::Format(m_format->currentData().toInt());
    const QString filter = format == vyra::Format::Zip ? tr("ZIP archive (*.zip)") : tr("7z archive (*.7z)");
    const QString path = QFileDialog::getSaveFileName(this, tr("Archive to create"), m_path->text(), filter);
    if (!path.isEmpty()) m_path->setText(QDir::toNativeSeparators(path));
}

QString CompressDialog::archivePath() const { return QDir::fromNativeSeparators(m_path->text().trimmed()); }

vyra::CompressOptions CompressDialog::options() const
{
    vyra::CompressOptions o;
    o.format = vyra::Format(m_format->currentData().toInt());
    o.method = vyra::Method(m_method->currentData().toInt());
    o.level = m_level->currentData().toInt();
    o.backend = vyra::BackendKind(m_backend->currentData().toInt());
    o.solidBlockSize = m_solid->currentData().toULongLong();
    o.chunkSize = std::size_t(m_chunk->currentData().toULongLong());
    return o;
}

void CompressDialog::accept()
{
    const QString path = archivePath();
    if (path.isEmpty()) return;
    if (QFileInfo::exists(path) &&
        QMessageBox::question(this, windowTitle(), tr("%1 already exists. Replace it?").arg(QFileInfo(path).fileName())) != QMessageBox::Yes)
        return;
    const vyra::CompressOptions o = options();
    QSettings s;
    s.setValue(QStringLiteral("compress/format"), QString::fromLatin1(vyra::formatName(o.format)));
    s.setValue(o.format == vyra::Format::Zip ? QStringLiteral("compress/methodZip") : QStringLiteral("compress/method7z"), int(o.method));
    s.setValue(QStringLiteral("compress/level"), o.level);
    s.setValue(QStringLiteral("compress/backend"), int(o.backend));
    s.setValue(QStringLiteral("compress/solid"), QVariant::fromValue<qulonglong>(o.solidBlockSize));
    s.setValue(QStringLiteral("compress/chunk"), QVariant::fromValue<qulonglong>(o.chunkSize));
    QDialog::accept();
}
