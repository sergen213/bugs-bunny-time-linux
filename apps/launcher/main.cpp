// bbtime-launcher: import wizard, settings, controls, save backups, logs, signed updates, launch.
// Writes only settings.ini, $XDG_DATA_HOME/bbtime/{lit,tb,backups,app}, $XDG_STATE_HOME/bbtime/logs.
// Original installs are only read (docs/LAUNCHER.md).
#include "files.h"
#include "settings.h"
#include "updater.h"

#include <QtConcurrent/QtConcurrent>
#include <QtWidgets>
#include <SDL3/SDL.h>

#include <atomic>
#include <format>

namespace fs = std::filesystem;
using bb::GameId;

namespace {

QString q(const fs::path& p) { return QString::fromStdString(p.string()); }
QString q(std::string_view s) { return QString::fromUtf8(s.data(), qsizetype(s.size())); }
QString q(const std::string& s) { return QString::fromStdString(s); }
fs::path path(const QString& s) { return fs::path(s.trimmed().toStdString()); }

constexpr const char* kTitles[2] = {"Bugs Bunny: Lost in Time", "Bugs Bunny & Taz: Time Busters"};
constexpr const char* kKeys[2] = {"lit", "tb"};
constexpr GameId kIds[2] = {GameId::LostInTime, GameId::TimeBusters};

fs::path& root(bbtime::Settings& s, int g) { return g == 0 ? s.litRoot : s.tbRoot; }

// Runs `work` on the thread pool and `done(result)` on the GUI thread.
template <class Work, class Done> void background(QObject* owner, Work work, Done done) {
    using R = decltype(work());
    auto* w = new QFutureWatcher<R>(owner);
    QObject::connect(w, &QFutureWatcher<R>::finished, owner, [w, done] { done(w->result()); w->deleteLater(); });
    w->setFuture(QtConcurrent::run(std::move(work)));
}

// ---------------------------------------------------------------- import wizard

struct ImportState {
    QLineEdit* folder[2]{};
    bbtime::InstallReport report[2];
    fs::path result[2]; // chosen roots after the wizard (install in place, or the copy)
};

class FoldersPage : public QWizardPage {
public:
    FoldersPage(ImportState& st, const bbtime::Settings& cfg) {
        setTitle("Original game folders");
        setSubTitle("Pick the disc mount, install folder or extracted ISO directory of each game (the folder that "
                    "contains Datas/). It is only read, never modified. Leave a game empty to skip it.");
        auto* form = new QFormLayout(this);
        for (int g = 0; g < 2; ++g) {
            st.folder[g] = new QLineEdit(q(g == 0 ? cfg.litRoot : cfg.tbRoot));
            auto* browse = new QPushButton("Browse…");
            auto* row = new QHBoxLayout;
            row->addWidget(st.folder[g]);
            row->addWidget(browse);
            form->addRow(kTitles[g], row);
            connect(st.folder[g], &QLineEdit::textChanged, this, &QWizardPage::completeChanged);
            connect(browse, &QPushButton::clicked, this, [this, e = st.folder[g]] {
                const QString d = QFileDialog::getExistingDirectory(this, "Original game folder", e->text());
                if (!d.isEmpty()) e->setText(d);
            });
        }
        st_ = &st;
    }
    bool isComplete() const override {
        return !st_->folder[0]->text().trimmed().isEmpty() || !st_->folder[1]->text().trimmed().isEmpty();
    }

private:
    ImportState* st_;
};

class ValidatePage : public QWizardPage {
public:
    explicit ValidatePage(ImportState& st) : st_(st) {
        setTitle("Validation");
        setSubTitle("Executable version and hash, file inventory and BZE level decoding.");
        auto* l = new QVBoxLayout(this);
        text_ = new QPlainTextEdit;
        text_->setReadOnly(true);
        text_->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
        l->addWidget(text_);
    }
    void initializePage() override {
        done_ = false;
        text_->setPlainText("Scanning…");
        const fs::path dirs[2] = {path(st_.folder[0]->text()), path(st_.folder[1]->text())};
        background(this, [a = dirs[0], b = dirs[1]] {
            return std::pair{a.empty() ? bbtime::InstallReport{} : bbtime::checkInstall(a),
                             b.empty() ? bbtime::InstallReport{} : bbtime::checkInstall(b)};
        }, [this](const std::pair<bbtime::InstallReport, bbtime::InstallReport>& r) {
            st_.report[0] = r.first;
            st_.report[1] = r.second;
            QString out;
            for (int g = 0; g < 2; ++g) {
                const auto& rep = st_.report[g];
                if (rep.root.empty()) { out += QString("%1: skipped\n\n").arg(kTitles[g]); continue; }
                out += QString("%1: %2\n").arg(kTitles[g], rep.ok(kIds[g]) ? "OK" : "NOT USABLE (ignored)");
                out += q(rep.summary()) + "\n";
            }
            text_->setPlainText(out);
            done_ = true;
            emit completeChanged();
        });
    }
    bool isComplete() const override { return done_ && (st_.report[0].ok(kIds[0]) || st_.report[1].ok(kIds[1])); }

private:
    ImportState& st_;
    QPlainTextEdit* text_;
    bool done_ = false;
};

class InstallPage : public QWizardPage {
public:
    explicit InstallPage(ImportState& st) : st_(st) {
        setTitle("Use or copy");
        auto* l = new QVBoxLayout(this);
        inPlace_ = new QRadioButton("Use the original folders in place (read-only)");
        copy_ = new QRadioButton(QString("Copy Datas/ into %1/<game>/ (≈500 MB per game; disc can be removed)")
                                     .arg(q(bbtime::dataDir())));
        inPlace_->setChecked(true);
        auto* go = new QPushButton("Copy now");
        bar_ = new QProgressBar;
        status_ = new QLabel;
        l->addWidget(inPlace_);
        l->addWidget(copy_);
        l->addWidget(go);
        l->addWidget(bar_);
        l->addWidget(status_);
        l->addStretch();
        go->setEnabled(false);
        connect(copy_, &QRadioButton::toggled, this, [this, go](bool on) { go->setEnabled(on && !busy_); emit completeChanged(); });
        connect(go, &QPushButton::clicked, this, [this, go] {
            go->setEnabled(false);
            busy_ = true;
            copied_ = false;
            emit completeChanged();
            cancel_ = false;
            background(this, [this] {
                std::string err;
                uint64_t base = 0, total = 0;
                for (int g = 0; g < 2; ++g) total += st_.report[g].ok(kIds[g]) ? st_.report[g].bytes : 0;
                for (int g = 0; g < 2 && err.empty(); ++g) {
                    if (!st_.report[g].ok(kIds[g])) continue;
                    const fs::path dst = bbtime::dataDir() / kKeys[g];
                    err = bbtime::copyDatas(st_.report[g], dst, [&](uint64_t d, uint64_t) {
                        const int pct = total ? int((base + d) * 100 / total) : 100;
                        QMetaObject::invokeMethod(bar_, [b = bar_, pct] { b->setValue(pct); }, Qt::QueuedConnection);
                    }, cancel_);
                    if (err.empty()) st_.result[g] = dst;
                    base += st_.report[g].bytes;
                }
                return err;
            }, [this, go](const std::string& err) {
                busy_ = false;
                copied_ = err.empty();
                bar_->setValue(copied_ ? 100 : 0);
                status_->setText(copied_ ? "Copied." : q("Copy failed: " + err));
                go->setEnabled(copy_->isChecked());
                emit completeChanged();
            });
        });
    }
    void initializePage() override {
        for (int g = 0; g < 2; ++g) st_.result[g] = st_.report[g].ok(kIds[g]) ? st_.report[g].root : fs::path{};
    }
    // Copied roots only if "Copy" is still selected at Finish; otherwise the validated originals.
    void finish() {
        if (inPlace_->isChecked()) initializePage();
    }
    bool isComplete() const override { return !busy_ && (inPlace_->isChecked() || copied_); }
    void stop() { cancel_ = true; QThreadPool::globalInstance()->waitForDone(); }

private:
    ImportState& st_;
    QRadioButton *inPlace_, *copy_;
    QProgressBar* bar_;
    QLabel* status_;
    bool busy_ = false, copied_ = false;
    std::atomic<bool> cancel_ = false;
};

// Returns true and updates cfg roots when the wizard is accepted.
bool runImportWizard(QWidget* parent, bbtime::Settings& cfg) {
    ImportState st;
    QWizard w(parent);
    w.setWindowTitle("Import original games");
    w.addPage(new FoldersPage(st, cfg));
    w.addPage(new ValidatePage(st));
    auto* inst = new InstallPage(st);
    w.addPage(inst);
    w.resize(760, 520);
    const bool ok = w.exec() == QDialog::Accepted;
    inst->stop();
    inst->finish();
    if (!ok) return false;
    for (int g = 0; g < 2; ++g)
        if (!st.result[g].empty()) root(cfg, g) = st.result[g];
    return true;
}

// ---------------------------------------------------------------- main window

class Launcher : public QMainWindow {
public:
    Launcher() {
        std::vector<std::string> warn;
        cfg_ = bbtime::loadSettings(bbtime::settingsPath(), &warn);
        setWindowTitle(QString("bbtime launcher %1").arg(BBTIME_VERSION));
        auto* tabs = new QTabWidget;
        tabs->addTab(gamesTab(), "Games");
        tabs->addTab(graphicsTab(), "Graphics && audio");
        tabs->addTab(controlsTab(), "Controls");
        tabs->addTab(savesTab(), "Saves");
        tabs->addTab(logsTab(), "Logs");
        tabs->addTab(updatesTab(), "Updates");
        tabs->addTab(aboutTab(), "About");

        game_ = new QComboBox;
        game_->addItems({"Collection menu", kTitles[0], kTitles[1]});
        auto* save = new QPushButton("Save settings");
        play_ = new QPushButton("Play");
        play_->setDefault(true);
        auto* bottom = new QHBoxLayout;
        bottom->addWidget(new QLabel(q(bbtime::settingsPath())));
        bottom->addStretch();
        bottom->addWidget(game_);
        bottom->addWidget(save);
        bottom->addWidget(play_);
        auto* central = new QWidget;
        auto* l = new QVBoxLayout(central);
        l->addWidget(tabs);
        l->addLayout(bottom);
        setCentralWidget(central);
        connect(save, &QPushButton::clicked, this, [this] { saveSettings(); });
        connect(play_, &QPushButton::clicked, this, [this] { launch(); });
        for (const auto& w : warn) statusBar()->showMessage(q("settings.ini " + w), 10000);
        refreshGames();
        resize(900, 640);
    }

private:
    bbtime::Settings cfg_;
    QComboBox* game_;
    QPushButton* play_;
    QProcess* proc_ = nullptr;
    QLabel* gameStatus_[2]{};
    QComboBox *renderer_{}, *fidelity_{}, *msaa_{};
    QSpinBox *width_{}, *height_{};
    QDoubleSpinBox* fov_{};
    QCheckBox *fullscreen_{}, *vsync_{};
    QSlider *music_{}, *sfx_{};
    QTableWidget* binds_{};
    QListWidget *pads_{}, *backups_{}, *logs_{};
    QPlainTextEdit* logView_{};
    QLineEdit* updateUrl_{};
    QLabel* updateStatus_{};
    QTextBrowser* notes_{};
    QPushButton *installUpdate_{}, *rollback_{};
    QCheckBox* force_{};
    QProgressBar* updateBar_{};
    std::optional<bbtime::update::Manifest> pending_;

    static fs::path appRoot() { return bbtime::dataDir() / "app"; }
    static fs::path backupRoot() { return bbtime::dataDir() / "backups"; }
    static fs::path logRoot() { return bbtime::stateDir() / "logs"; }

    QWidget* gamesTab() {
        auto* w = new QWidget;
        auto* l = new QVBoxLayout(w);
        for (int g = 0; g < 2; ++g) {
            auto* box = new QGroupBox(QString(kTitles[g]).replace('&', "&&"));
            auto* bl = new QVBoxLayout(box);
            gameStatus_[g] = new QLabel;
            gameStatus_[g]->setTextInteractionFlags(Qt::TextSelectableByMouse);
            gameStatus_[g]->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
            bl->addWidget(gameStatus_[g]);
            l->addWidget(box);
        }
        auto* wizard = new QPushButton("Import original games…");
        connect(wizard, &QPushButton::clicked, this, [this] {
            if (runImportWizard(this, cfg_)) {
                saveSettings();
                refreshGames();
            }
        });
        l->addWidget(wizard, 0, Qt::AlignLeft);
        l->addStretch();
        return w;
    }

    void refreshGames() {
        for (int g = 0; g < 2; ++g) {
            const fs::path r = root(cfg_, g);
            if (r.empty()) { gameStatus_[g]->setText("Not imported. Run the import wizard."); continue; }
            gameStatus_[g]->setText("Checking " + q(r) + " …");
            background(this, [r] { return bbtime::checkInstall(r); }, [this, g](const bbtime::InstallReport& rep) {
                gameStatus_[g]->setText(QString(rep.ok(kIds[g]) ? "Ready.\n" : "PROBLEM: re-run the import wizard.\n") +
                                        q(rep.summary()).trimmed());
            });
        }
    }

    QWidget* graphicsTab() {
        auto* w = new QWidget;
        auto* f = new QFormLayout(w);
        renderer_ = new QComboBox;
        renderer_->addItem("OpenGL 4.6", "gl");
        renderer_->addItem("Vulkan", "vk");
        renderer_->setCurrentIndex(renderer_->findData(q(cfg_.renderer)));
        fidelity_ = new QComboBox;
        fidelity_->addItem("Enhanced (filtering, MSAA, Hor+)", "enhanced");
        fidelity_->addItem("Original (nearest, 4:3)", "original");
        fidelity_->setCurrentIndex(fidelity_->findData(q(cfg_.fidelity)));
        width_ = new QSpinBox;
        width_->setRange(320, 16384);
        width_->setValue(cfg_.width);
        height_ = new QSpinBox;
        height_->setRange(200, 16384);
        height_->setValue(cfg_.height);
        auto* preset = new QComboBox;
        preset->addItem("Presets…");
        for (QSize z : {QSize(640, 480), QSize(800, 600), QSize(1280, 720), QSize(1600, 900), QSize(1920, 1080),
                        QSize(2560, 1440), QSize(3840, 2160)})
            preset->addItem(QString("%1x%2").arg(z.width()).arg(z.height()), z);
        connect(preset, &QComboBox::activated, this, [this, preset](int i) {
            if (i == 0) return;
            const QSize z = preset->itemData(i).toSize();
            width_->setValue(z.width());
            height_->setValue(z.height());
            preset->setCurrentIndex(0);
        });
        auto* res = new QHBoxLayout;
        res->addWidget(width_);
        res->addWidget(new QLabel("×"));
        res->addWidget(height_);
        res->addWidget(preset);
        fullscreen_ = new QCheckBox("Fullscreen");
        fullscreen_->setChecked(cfg_.fullscreen);
        vsync_ = new QCheckBox("Vertical sync");
        vsync_->setChecked(cfg_.vsync);
        fov_ = new QDoubleSpinBox;
        fov_->setRange(30, 120);
        fov_->setSuffix("° vertical at 4:3");
        fov_->setValue(cfg_.fov);
        msaa_ = new QComboBox;
        for (int s : {0, 2, 4, 8}) msaa_->addItem(s ? QString("%1×").arg(s) : QString("Off"), s);
        msaa_->setCurrentIndex(msaa_->findData(cfg_.msaa));
        auto slider = [](int v) {
            auto* s = new QSlider(Qt::Horizontal);
            s->setRange(0, 100);
            s->setValue(v);
            return s;
        };
        music_ = slider(cfg_.musicVolume);
        sfx_ = slider(cfg_.sfxVolume);
        f->addRow("Renderer", renderer_);
        f->addRow("Fidelity", fidelity_);
        f->addRow("Resolution", res);
        f->addRow("", fullscreen_);
        f->addRow("", vsync_);
        f->addRow("Field of view", fov_);
        f->addRow("Anti-aliasing (MSAA)", msaa_);
        f->addRow("Music volume", music_);
        f->addRow("Effects volume", sfx_);
        return w;
    }

    QWidget* controlsTab() {
        auto* w = new QWidget;
        auto* l = new QVBoxLayout(w);
        l->addWidget(new QLabel("Keyboard bindings: SDL key names, comma-separated for several keys "
                                "(e.g. \"Space,D\"). Defaults are the original game's."));
        binds_ = new QTableWidget(int(std::size(bbtime::kActions)), 1);
        binds_->setHorizontalHeaderLabels({"Keys"});
        binds_->horizontalHeader()->setStretchLastSection(true);
        int row = 0;
        for (std::string_view a : bbtime::kActions) {
            binds_->setVerticalHeaderItem(row, new QTableWidgetItem(q(a)));
            const auto it = cfg_.bindings.find(std::string(a));
            binds_->setItem(row++, 0, new QTableWidgetItem(it == cfg_.bindings.end() ? QString() : q(it->second)));
        }
        auto* defaults = new QPushButton("Restore defaults");
        connect(defaults, &QPushButton::clicked, this, [this] {
            const bbtime::Settings d;
            for (int r = 0; r < binds_->rowCount(); ++r)
                binds_->item(r, 0)->setText(q(d.bindings.at(binds_->verticalHeaderItem(r)->text().toStdString())));
        });
        pads_ = new QListWidget;
        pads_->setMaximumHeight(110);
        auto* refresh = new QPushButton("Refresh gamepads");
        connect(refresh, &QPushButton::clicked, this, [this] { refreshPads(); });
        l->addWidget(binds_);
        l->addWidget(defaults, 0, Qt::AlignLeft);
        l->addWidget(new QLabel("Connected gamepads (SDL standard layout; the game maps them like a PlayStation pad):"));
        l->addWidget(pads_);
        l->addWidget(refresh, 0, Qt::AlignLeft);
        refreshPads();
        return w;
    }

    void refreshPads() {
        pads_->clear();
        SDL_UpdateGamepads();
        int n = 0;
        SDL_JoystickID* ids = SDL_GetGamepads(&n);
        for (int i = 0; i < n; ++i) {
            const char* name = SDL_GetGamepadNameForID(ids[i]);
            pads_->addItem(QString("%1  (id %2)").arg(name ? name : "unknown gamepad").arg(ids[i]));
        }
        SDL_free(ids);
        if (n == 0) pads_->addItem(SDL_WasInit(SDL_INIT_GAMEPAD) ? "No gamepad detected." : "SDL gamepad support unavailable.");
    }

    QWidget* savesTab() {
        auto* w = new QWidget;
        auto* l = new QVBoxLayout(w);
        l->addWidget(new QLabel(QString("Saves: %1\nBackups: %2").arg(q(bbtime::saveDir()), q(backupRoot()))));
        backups_ = new QListWidget;
        auto* backup = new QPushButton("Back up now");
        auto* restore = new QPushButton("Restore selected…");
        auto* open = new QPushButton("Open backups folder");
        auto* row = new QHBoxLayout;
        row->addWidget(backup);
        row->addWidget(restore);
        row->addWidget(open);
        row->addStretch();
        l->addWidget(backups_);
        l->addLayout(row);
        connect(backup, &QPushButton::clicked, this, [this] {
            fs::path out;
            const std::string e = bbtime::backupSaves(bbtime::saveDir(), backupRoot(), &out);
            statusBar()->showMessage(e.empty() ? "Backed up to " + q(out) : q(e), 10000);
            refreshBackups();
        });
        connect(restore, &QPushButton::clicked, this, [this] {
            const QListWidgetItem* it = backups_->currentItem();
            if (!it || QMessageBox::question(this, "Restore saves", "Replace the current saves with " + it->text() +
                                                     "?\nThe current saves are backed up first.") != QMessageBox::Yes)
                return;
            const std::string e = bbtime::restoreSaves(backupRoot() / it->text().toStdString(), bbtime::saveDir(), backupRoot());
            statusBar()->showMessage(e.empty() ? "Restored " + it->text() : q(e), 10000);
            refreshBackups();
        });
        connect(open, &QPushButton::clicked, this, [] {
            fs::create_directories(backupRoot());
            QDesktopServices::openUrl(QUrl::fromLocalFile(q(backupRoot())));
        });
        refreshBackups();
        return w;
    }

    void refreshBackups() {
        backups_->clear();
        for (const fs::path& p : bbtime::listBackups(backupRoot())) backups_->addItem(q(p.filename()));
    }

    QWidget* logsTab() {
        auto* w = new QWidget;
        auto* l = new QHBoxLayout(w);
        logs_ = new QListWidget;
        logs_->setMaximumWidth(260);
        logView_ = new QPlainTextEdit;
        logView_->setReadOnly(true);
        logView_->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
        l->addWidget(logs_);
        l->addWidget(logView_);
        connect(logs_, &QListWidget::currentTextChanged, this, [this](const QString& name) {
            QFile f(q(logRoot()) + "/" + name);
            logView_->setPlainText(f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.readAll()) : QString());
        });
        refreshLogs();
        return w;
    }

    void refreshLogs() {
        logs_->clear();
        QDir d(q(logRoot()));
        logs_->addItems(d.entryList({"*.log"}, QDir::Files, QDir::Name | QDir::Reversed));
        if (logs_->count() == 0) logView_->setPlainText("No game logs yet. Logs of games started here go to " + q(logRoot()));
    }

    QWidget* updatesTab() {
        auto* w = new QWidget;
        auto* l = new QVBoxLayout(w);
        auto* f = new QFormLayout;
        const auto cur = bbtime::update::currentVersion(appRoot());
        f->addRow("Running version", new QLabel(BBTIME_VERSION));
        f->addRow("Installed (updater)", new QLabel(cur ? q(*cur) + "  in " + q(appRoot()) : QString("none")));
        updateUrl_ = new QLineEdit(q(cfg_.updateUrl));
        updateUrl_->setPlaceholderText("https://github.com/OWNER/bbtime/releases/latest/download/manifest.json");
        f->addRow("Manifest URL", updateUrl_);
        l->addLayout(f);
        auto* checkB = new QPushButton("Check for updates");
        installUpdate_ = new QPushButton("Install");
        installUpdate_->setEnabled(false);
        force_ = new QCheckBox("Allow reinstall/downgrade");
        rollback_ = new QPushButton("Roll back");
        rollback_->setEnabled(bbtime::update::previousVersion(appRoot()).has_value());
        auto* row = new QHBoxLayout;
        for (QWidget* b : std::initializer_list<QWidget*>{checkB, installUpdate_, force_, rollback_}) row->addWidget(b);
        row->addStretch();
        l->addLayout(row);
        updateBar_ = new QProgressBar;
        updateStatus_ = new QLabel("Updates are verified with the bbtime release key (Ed25519) and SHA-256.");
        updateStatus_->setWordWrap(true);
        notes_ = new QTextBrowser;
        l->addWidget(updateBar_);
        l->addWidget(updateStatus_);
        l->addWidget(notes_);
        connect(checkB, &QPushButton::clicked, this, [this, checkB] {
            pending_.reset();
            installUpdate_->setEnabled(false);
            checkB->setEnabled(false);
            cfg_.updateUrl = updateUrl_->text().trimmed().toStdString();
            updateStatus_->setText("Checking…");
            background(this, [url = cfg_.updateUrl, o = updateOptions()] { return bbtime::update::check(url, o); },
                       [this, checkB](const std::expected<bbtime::update::Manifest, std::string>& m) {
                checkB->setEnabled(true);
                if (!m) { updateStatus_->setText(q("Check failed: " + m.error())); return; }
                const auto cur = bbtime::update::currentVersion(appRoot());
                using bbtime::update::Version, bbtime::update::parseVersion;
                const Version have = std::max(parseVersion(cur.value_or("0.0.0")).value_or(Version{}),
                                              parseVersion(BBTIME_VERSION).value_or(Version{}));
                const bool newer = bbtime::update::parseVersion(m->version) > have;
                updateStatus_->setText(q(std::format("Signed release {} found ({} files) — {}.", m->version,
                                                     m->artifacts.size(), newer ? "newer" : "not newer than installed")));
                notes_->setMarkdown(q(m->notes));
                pending_ = *m;
                installUpdate_->setEnabled(true);
            });
        });
        connect(installUpdate_, &QPushButton::clicked, this, [this] {
            if (!pending_) return;
            installUpdate_->setEnabled(false);
            updateStatus_->setText("Downloading and verifying…");
            auto o = updateOptions();
            o.force = force_->isChecked();
            o.progress = [bar = updateBar_](uint64_t d, uint64_t t) {
                QMetaObject::invokeMethod(bar, [bar, pct = t ? int(d * 100 / t) : 100] { bar->setValue(pct); }, Qt::QueuedConnection);
            };
            background(this, [m = *pending_, o] { return bbtime::update::install(m, o, BBTIME_VERSION); },
                       [this](const std::expected<fs::path, std::string>& r) {
                updateStatus_->setText(r ? "Installed to " + q(*r) + ". Restart the launcher from there."
                                         : q("Install refused/failed (nothing changed): " + r.error()));
                rollback_->setEnabled(bbtime::update::previousVersion(appRoot()).has_value());
                installUpdate_->setEnabled(!r);
            });
        });
        connect(rollback_, &QPushButton::clicked, this, [this] {
            const auto r = bbtime::update::rollback(appRoot());
            updateStatus_->setText(r ? "Rolled back to " + q(*r) : q("Rollback failed: " + r.error()));
        });
        return w;
    }

    bbtime::update::Options updateOptions() const {
        bbtime::update::Options o;
        o.installRoot = appRoot();
        return o;
    }

    QWidget* aboutTab() {
        auto* t = new QTextBrowser;
        QFile f(":/CHANGELOG.md");
        const QString log = f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.readAll()) : "CHANGELOG.md missing";
        t->setMarkdown(QString("**bbtime %1** — native Linux engine for *Bugs Bunny: Lost in Time* and "
                               "*Bugs Bunny & Taz: Time Busters*. Requires your own original game data.\n\n")
                           .arg(BBTIME_VERSION) + log);
        return t;
    }

    void collect() {
        cfg_.renderer = renderer_->currentData().toString().toStdString();
        cfg_.fidelity = fidelity_->currentData().toString().toStdString();
        cfg_.width = width_->value();
        cfg_.height = height_->value();
        cfg_.fullscreen = fullscreen_->isChecked();
        cfg_.vsync = vsync_->isChecked();
        cfg_.fov = float(fov_->value());
        cfg_.msaa = msaa_->currentData().toInt();
        cfg_.musicVolume = music_->value();
        cfg_.sfxVolume = sfx_->value();
        for (int r = 0; r < binds_->rowCount(); ++r) {
            const std::string a = binds_->verticalHeaderItem(r)->text().toStdString();
            const std::string k = binds_->item(r, 0)->text().trimmed().toStdString();
            if (k.empty()) cfg_.bindings.erase(a);
            else cfg_.bindings[a] = k;
        }
        cfg_.updateUrl = updateUrl_->text().trimmed().toStdString();
    }

    bool saveSettings() {
        collect();
        const std::string e = bbtime::saveSettings(cfg_);
        statusBar()->showMessage(e.empty() ? "Saved " + q(bbtime::settingsPath()) : q(e), 8000);
        return e.empty();
    }

    // Prefers an updater-installed game, then the one next to the launcher (install or build tree), then PATH.
    static QString gameBinary() {
        const QString here = QCoreApplication::applicationDirPath();
        for (const QString& c : {q(appRoot() / "current/bbtime"), here + "/bbtime", here + "/../bbtime/bbtime"})
            if (QFileInfo(c).isExecutable()) return QFileInfo(c).canonicalFilePath();
        return QStandardPaths::findExecutable("bbtime");
    }

    void launch() {
        if (proc_ || !saveSettings()) return;
        const QString exe = gameBinary();
        if (exe.isEmpty()) { QMessageBox::warning(this, "bbtime", "Cannot find the bbtime executable."); return; }
        QStringList args;
        if (game_->currentIndex() > 0) args << "--game" << kKeys[game_->currentIndex() - 1];
        fs::create_directories(logRoot());
        const QString name = QDateTime::currentDateTime().toString("'bbtime-'yyyyMMdd-HHmmss'.log'");
        auto* log = new QFile(q(logRoot()) + "/" + name, this);
        log->open(QIODevice::WriteOnly | QIODevice::Text);
        log->write(QString("$ %1 %2\n").arg(exe, args.join(' ')).toUtf8());
        proc_ = new QProcess(this);
        proc_->setProcessChannelMode(QProcess::MergedChannels);
        connect(proc_, &QProcess::readyRead, this, [this, log, name] {
            const QByteArray b = proc_->readAll();
            log->write(b);
            log->flush();
            if (logs_->currentItem() && logs_->currentItem()->text() == name) logView_->appendPlainText(QString::fromUtf8(b).trimmed());
        });
        connect(proc_, &QProcess::finished, this, [this, log](int code, QProcess::ExitStatus st) {
            log->write(QString("\n[exit %1%2]\n").arg(code).arg(st == QProcess::CrashExit ? ", crashed" : "").toUtf8());
            log->close();
            log->deleteLater();
            proc_->deleteLater();
            proc_ = nullptr;
            play_->setEnabled(true);
            statusBar()->showMessage(QString("bbtime exited with code %1").arg(code), 10000);
        });
        connect(proc_, &QProcess::errorOccurred, this, [this, exe](QProcess::ProcessError e) {
            if (e == QProcess::FailedToStart) statusBar()->showMessage("Failed to start " + exe, 10000);
        });
        play_->setEnabled(false);
        proc_->start(exe, args);
        refreshLogs();
        logs_->setCurrentRow(0);
    }

    void closeEvent(QCloseEvent* e) override {
        collect();
        bbtime::saveSettings(cfg_);
        QMainWindow::closeEvent(e);
    }
};

} // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QApplication::setApplicationName("bbtime-launcher");
    QApplication::setApplicationVersion(BBTIME_VERSION);
    QApplication::setDesktopFileName("bbtime-launcher");
    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1"); // keep SIGINT/SIGTERM fatal; Qt owns the event loop
    SDL_Init(SDL_INIT_GAMEPAD); // failure only disables the gamepad list
    Launcher w;
    w.show();
    const int rc = app.exec();
    SDL_Quit();
    return rc;
}
