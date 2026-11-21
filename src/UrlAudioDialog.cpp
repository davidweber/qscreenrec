#include "UrlAudioDialog.h"

#include <QClipboard>
#include <QComboBox>
#include <QDir>
#include <QFileInfo>
#include <QFormLayout>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QProcessEnvironment>
#include <QProgressBar>
#include <QPushButton>
#include <QRegularExpression>
#include <QSettings>
#include <QStandardPaths>
#include <QUrl>
#include <QVBoxLayout>

namespace {
// Marks the line on which yt-dlp reports the final file, so it cannot be confused with
// anything else on stdout.
const QByteArray kFileMarker = "QSCREENREC_FILE:";

QStringList extraToolDirs()
{
    const QString home = QDir::homePath();
    return {
        home + QStringLiteral("/.local/bin"), home + QStringLiteral("/bin"),
        QStringLiteral("/opt/homebrew/bin"), QStringLiteral("/usr/local/bin"),   // macOS GUI apps get a bare PATH
        QStringLiteral("/opt/local/bin"),
    };
}
}

QString UrlAudioDialog::findTool(const QString &name)
{
    QString path = QStandardPaths::findExecutable(name);
    if (path.isEmpty())
        path = QStandardPaths::findExecutable(name, extraToolDirs());
    return path;
}

UrlAudioDialog::UrlAudioDialog(const QString &outputFolder, QWidget *parent)
    : QDialog(parent)
    , m_folder(outputFolder)
    , m_ytDlp(findTool(QStringLiteral("yt-dlp")))
    , m_ffmpeg(findTool(QStringLiteral("ffmpeg")))
{
    setObjectName(QStringLiteral("urlAudioDialog"));
    setWindowTitle(tr("Audio from URL"));
    setMinimumWidth(520);

    m_urlEdit = new QLineEdit;
    m_urlEdit->setPlaceholderText(tr("https://www.youtube.com/watch?v=…"));
    m_urlEdit->setClearButtonEnabled(true);
    // Convenience: start with a link that is already on the clipboard.
    const QString clip = QGuiApplication::clipboard()->text().trimmed();
    if (clip.startsWith(QLatin1String("http://")) || clip.startsWith(QLatin1String("https://"))) {
        if (!clip.contains(QLatin1Char('\n')) && clip.size() < 2000)
            m_urlEdit->setText(clip);
    }

    m_formatCombo = new QComboBox;
    m_formatCombo->addItem(tr("Original audio (no conversion)"), QString());
    if (!m_ffmpeg.isEmpty()) {
        m_formatCombo->addItem(tr("M4A (AAC)"), QStringLiteral("m4a"));
        m_formatCombo->addItem(tr("MP3"), QStringLiteral("mp3"));
        m_formatCombo->addItem(tr("FLAC"), QStringLiteral("flac"));
        m_formatCombo->addItem(tr("WAV"), QStringLiteral("wav"));
        m_formatCombo->addItem(tr("Opus"), QStringLiteral("opus"));
        const int i = m_formatCombo->findData(QSettings().value("url/format", QStringLiteral("m4a")));
        m_formatCombo->setCurrentIndex(qMax(0, i));
    }

    auto *form = new QFormLayout;
    form->addRow(tr("URL:"), m_urlEdit);
    form->addRow(tr("Save as:"), m_formatCombo);
    form->addRow(tr("Folder:"), new QLabel(QDir::toNativeSeparators(m_folder)));

    m_progress = new QProgressBar;
    m_progress->setRange(0, 100);
    m_progress->setValue(0);
    m_progress->setTextVisible(true);

    m_status = new QLabel;
    m_status->setWordWrap(true);
    m_status->setTextInteractionFlags(Qt::TextBrowserInteraction);
    m_status->setOpenExternalLinks(true);

    m_downloadButton = new QPushButton(tr("Extract audio"));
    m_downloadButton->setObjectName(QStringLiteral("playButton"));
    m_downloadButton->setDefault(true);
    m_closeButton = new QPushButton(tr("Close"));
    connect(m_downloadButton, &QPushButton::clicked, this, &UrlAudioDialog::start);
    connect(m_closeButton, &QPushButton::clicked, this, &UrlAudioDialog::reject);
    auto *buttons = new QHBoxLayout;
    buttons->addStretch();
    buttons->addWidget(m_downloadButton);
    buttons->addWidget(m_closeButton);

    auto *note = new QLabel(tr("Only save audio you have the right to download — your own uploads, or "
                               "content whose licence or site terms allow it."));
    note->setWordWrap(true);
    note->setEnabled(false);

    auto *layout = new QVBoxLayout(this);
    layout->addLayout(form);
    layout->addWidget(m_progress);
    layout->addWidget(m_status);
    layout->addWidget(note);
    layout->addLayout(buttons);

    if (m_ytDlp.isEmpty()) {
        m_downloadButton->setEnabled(false);
        m_status->setText(tr("This needs the free tool <b>yt-dlp</b>, which was not found on this computer. "
                             "Install it, then reopen this dialog:<br>"
                             "Linux/macOS: <code>pipx install yt-dlp</code> or <code>brew install yt-dlp</code> · "
                             "Windows: <code>winget install yt-dlp</code><br>"
                             "<a href=\"https://github.com/yt-dlp/yt-dlp#installation\">Installation instructions</a>"));
    } else if (m_ffmpeg.isEmpty()) {
        m_status->setText(tr("ffmpeg was not found, so the audio is saved in the site's original format "
                             "(usually M4A or WebM/Opus). Install ffmpeg to convert to MP3, FLAC or WAV."));
    }
}

UrlAudioDialog::~UrlAudioDialog()
{
    if (m_process && m_process->state() != QProcess::NotRunning) {
        m_process->disconnect(this);
        m_process->kill();
        m_process->waitForFinished(2000);
    }
}

void UrlAudioDialog::setBusy(bool busy)
{
    m_urlEdit->setEnabled(!busy);
    m_formatCombo->setEnabled(!busy);
    m_downloadButton->setEnabled(!busy && !m_ytDlp.isEmpty());
    m_closeButton->setText(busy ? tr("Cancel") : tr("Close"));
}

void UrlAudioDialog::start()
{
    const QString text = m_urlEdit->text().trimmed();
    const QUrl url = QUrl::fromUserInput(text);
    if (text.isEmpty() || !url.isValid() || (url.scheme() != QLatin1String("http") && url.scheme() != QLatin1String("https"))) {
        m_status->setText(tr("Enter the web address (http:// or https://) of the video."));
        return;
    }
    if (!QDir().mkpath(m_folder)) {
        m_status->setText(tr("Cannot create the folder %1").arg(QDir::toNativeSeparators(m_folder)));
        return;
    }

    const QString format = m_formatCombo->currentData().toString();
    QSettings().setValue("url/format", format);

    QStringList args {
        QStringLiteral("--no-playlist"),            // a watch?v=…&list=… link means that one video
        QStringLiteral("--newline"),                // one progress line per update
        QStringLiteral("--no-colors"),
        QStringLiteral("--windows-filenames"),      // titles become names that are valid everywhere
        QStringLiteral("--no-overwrites"),
        QStringLiteral("--paths"), m_folder,
        QStringLiteral("--output"), QStringLiteral("%(title).150B [%(id)s].%(ext)s"),
        QStringLiteral("--print"), QStringLiteral("after_move:") + QString::fromLatin1(kFileMarker) + QStringLiteral("%(filepath)s"),
        QStringLiteral("--no-simulate"),
        QStringLiteral("--no-quiet"),               // --print would otherwise silence the progress lines
    };
    if (!m_ffmpeg.isEmpty()) {
        // With ffmpeg: take the best audio (or, where a site only has combined files, the
        // best file) and extract/convert the audio track.
        args << QStringLiteral("--format") << QStringLiteral("bestaudio/best")
             << QStringLiteral("--extract-audio")
             << QStringLiteral("--ffmpeg-location") << QFileInfo(m_ffmpeg).absolutePath();
        if (!format.isEmpty())
            args << QStringLiteral("--audio-format") << format;
    } else {
        // Without ffmpeg nothing can be extracted or converted: only audio-only streams work.
        args << QStringLiteral("--format") << QStringLiteral("bestaudio");
    }
    // "--" ends the options, so the address can never be read as one.
    args << QStringLiteral("--") << url.toString(QUrl::FullyEncoded);

    m_savedFile.clear();
    m_lastError.clear();
    m_buffer.clear();
    m_progress->setValue(0);
    m_status->setText(tr("Starting…"));
    setBusy(true);

    delete m_process;
    m_process = new QProcess(this);
    m_process->setProcessChannelMode(QProcess::MergedChannels);
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("PYTHONIOENCODING"), QStringLiteral("utf-8"));
    env.insert(QStringLiteral("PYTHONUTF8"), QStringLiteral("1"));
    m_process->setProcessEnvironment(env);
    connect(m_process, &QProcess::readyReadStandardOutput, this, &UrlAudioDialog::onOutput);
    connect(m_process, &QProcess::finished, this, &UrlAudioDialog::onFinished);
    connect(m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            setBusy(false);
            m_status->setText(tr("Could not start yt-dlp (%1).").arg(QDir::toNativeSeparators(m_ytDlp)));
        }
    });
    m_process->start(m_ytDlp, args);
}

void UrlAudioDialog::onOutput()
{
    static const QRegularExpression percent(QStringLiteral(R"(\[download\]\s+(\d+(?:\.\d+)?)%)"));
    m_buffer += m_process->readAllStandardOutput();
    for (;;) {
        const qsizetype end = m_buffer.indexOf('\n');
        if (end < 0)
            break;
        const QByteArray raw = m_buffer.left(end).trimmed();
        m_buffer.remove(0, end + 1);
        if (raw.isEmpty())
            continue;
        if (raw.startsWith(kFileMarker)) {
            m_savedFile = QString::fromUtf8(raw.mid(kFileMarker.size()));
            continue;
        }
        const QString line = QString::fromUtf8(raw);
        const auto match = percent.match(line);
        if (match.hasMatch()) {
            m_progress->setValue(int(match.captured(1).toDouble()));
            m_status->setText(tr("Downloading…"));
        } else if (line.startsWith(QLatin1String("ERROR:"))) {
            m_lastError = line.mid(6).trimmed();
        } else if (line.startsWith(QLatin1String("[ExtractAudio]"))) {
            m_progress->setValue(100);
            m_status->setText(tr("Extracting audio…"));
        } else if (line.startsWith(QLatin1Char('['))) {
            m_status->setText(line.left(160).toHtmlEscaped());
        }
    }
}

void UrlAudioDialog::onFinished(int exitCode, QProcess::ExitStatus status)
{
    onOutput();
    setBusy(false);
    if (status == QProcess::NormalExit && exitCode == 0 && !m_savedFile.isEmpty() && QFileInfo::exists(m_savedFile)) {
        m_progress->setValue(100);
        accept();       // the caller shows the result and can play it
        return;
    }
    m_progress->setValue(0);
    if (status != QProcess::NormalExit)
        m_status->setText(tr("Cancelled."));
    else
        m_status->setText(tr("Failed: %1").arg((m_lastError.isEmpty() ? tr("yt-dlp reported an error.") : m_lastError)
                                                      .left(400).toHtmlEscaped()));
}

void UrlAudioDialog::reject()
{
    if (m_process && m_process->state() != QProcess::NotRunning) {
        m_process->kill();          // "Cancel" while downloading; the dialog stays open
        return;
    }
    QDialog::reject();
}
