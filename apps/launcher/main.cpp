// bbtime-launcher: import wizard, settings, controls, save backups, logs, signed updates, launch.
// Writes only settings.ini, $XDG_DATA_HOME/bbtime/{lit,tb,backups,app}, $XDG_STATE_HOME/bbtime/logs.
// Original installs are only read (docs/LAUNCHER.md).
#include "bb/level.h"
#include "bb/mods.h"
#include "bb/tb_actor.h"
#include "bb/tb_level.h"
#include "files.h"
#include "settings.h"
#include "updater.h"

#include <QtConcurrent/QtConcurrent>
#include <QtOpenGL>
#include <QtOpenGLWidgets>
#include <QtWidgets>
#include <SDL3/SDL.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <set>
#include <unistd.h>

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

// ---------------------------------------------------------------- original menu art
// The launcher dresses itself in the imported games' own front-end art (only read, never copied): TB menu.men's
// spiral page pc03, its button bitmaps (X.bmp / hover XD.bmp) and Brady Bunch font, the title backdrops and the
// LiT title slot-menu panel colours (navy, two gold rules; see apps/bbtime drawMain/drawOptions). Without an
// install it paints the spiral as rings and uses the system font.

fs::path appRoot() { return bbtime::dataDir() / "app"; }
fs::path gRestart; // launcher to exec once the event loop ends (after an update install)

// Case-insensitive <root>/<parts...> (installs mix Datas/data, DATAS/Data ...); empty if missing.
QString findCi(const fs::path& root, std::initializer_list<const char*> parts) {
    if (root.empty()) return {};
    QString cur = q(root);
    for (const char* p : parts) {
        const QStringList hit = QDir(cur).entryList({p}, QDir::AllEntries | QDir::NoDotAndDotDot); // filters ignore case
        if (hit.isEmpty()) return {};
        cur += "/" + hit.first();
    }
    return cur;
}

QPixmap bmp(const fs::path& root, std::initializer_list<const char*> parts) {
    const QString f = findCi(root, parts);
    return f.isEmpty() ? QPixmap() : QPixmap(f);
}

// TB text colours (TIME_BUSTERS.md §8.1) and the LiT panel gold, as in the collection menu.
constexpr const char* kStyle = R"(
* { font-family: %1; font-size: 16px; }
QToolButton#nav { color: #f21f10; background: transparent; border: none; font-size: 17px; padding: 0px; }
QToolButton#nav:checked, QToolButton#nav:hover { color: #f8e600; }
QToolButton#card { color: #f21f10; background: transparent; border: 3px solid transparent; font-size: 22px; }
QToolButton#card:hover { border-color: #b89036; }
QToolButton#card:checked { border-color: #f8e600; }
QLabel#heading { color: #f21f10; font-size: 38px; }
QLabel#key { color: #f7b048; font-size: 19px; }
QLabel#status, QPlainTextEdit, QListWidget, QTableWidget { font-family: monospace; font-size: 13px; }
QTextBrowser { font-family: sans-serif; font-size: 14px; }
QPushButton { color: #f21f10; background: rgba(0, 0, 14, 220); border: 2px solid #b89036; border-radius: 8px;
              padding: 3px 14px; font-size: 20px; }
QPushButton:hover, QPushButton:focus { color: #f8e600; border-color: #f8e600; }
QPushButton:disabled { color: #808096; border-color: #4a4e66; }
QPushButton#play { font-size: 30px; padding: 2px 20px; }
QGroupBox { color: #f7b048; font-size: 20px; border: 1px solid #9e712b; border-radius: 4px; margin-top: 14px; padding-top: 8px; }
QGroupBox::title { subcontrol-origin: margin; left: 10px; }
QProgressBar { border: 1px solid #b89036; background: black; color: white; text-align: center; }
QProgressBar::chunk { background: #f21f10; }
QStatusBar { color: #f7b048; }
)";

void applyPalette() {
    QApplication::setStyle("Fusion");
    QPalette p;
    const QColor navy(0, 8, 42), base(0, 10, 48), grey(128, 128, 150);
    p.setColor(QPalette::Window, navy);
    p.setColor(QPalette::Base, base);
    p.setColor(QPalette::AlternateBase, QColor(0, 16, 72));
    p.setColor(QPalette::Button, QColor(10, 26, 80));
    p.setColor(QPalette::ToolTipBase, navy);
    for (auto r : {QPalette::WindowText, QPalette::Text, QPalette::ButtonText, QPalette::ToolTipText, QPalette::HighlightedText})
        p.setColor(r, Qt::white);
    p.setColor(QPalette::Highlight, QColor(9, 128, 237));
    p.setColor(QPalette::Link, QColor(248, 230, 0));
    p.setColor(QPalette::PlaceholderText, grey);
    for (auto r : {QPalette::WindowText, QPalette::Text, QPalette::ButtonText}) p.setColor(QPalette::Disabled, r, grey);
    QApplication::setPalette(p);
}

// Window background: an original page scaled to cover (darkened by `dim`), else the TB spiral as reflected rings.
class Backdrop : public QWidget {
public:
    QPixmap art;
    int dim = 0;

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        if (art.isNull()) {
            QRadialGradient g(QRectF(rect()).center(), 70);
            g.setSpread(QGradient::ReflectSpread);
            g.setColorAt(0, QColor(3, 12, 70));
            g.setColorAt(1, QColor(20, 74, 175));
            p.fillRect(rect(), g);
            return;
        }
        const QPixmap s = art.scaled(size(), Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
        p.drawPixmap((width() - s.width()) / 2, (height() - s.height()) / 2, s);
        p.fillRect(rect(), QColor(0, 0, 0, dim));
    }
};

// LiT title slot-menu panel: near-black navy with two gold rules.
class Panel : public QFrame {
protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.fillRect(rect(), QColor(0, 0, 14, 205));
        for (const auto& [d, c] : {std::pair{1, QColor(184, 144, 54)}, std::pair{6, QColor(158, 113, 43)}}) {
            p.setPen(QPen(c, 2));
            p.drawRect(QRectF(rect()).adjusted(d, d, -d, -d));
        }
    }
};

// Mods page 3-D viewer: one glTF (bb::mods::loadPreview, static rest / bind pose), base colour texture × vertex
// colour × 2 (0.5 = the PS1's neutral 128, as in the game). Drag = orbit, wheel = zoom, double-click = reset.
class ModelView : public QOpenGLWidget, protected QOpenGLFunctions {
public:
    QString load(const fs::path& file) {
        QString err;
        try {
            model_ = bb::mods::loadPreview(file);
        } catch (const std::exception& e) {
            model_ = {};
            err = QString::fromUtf8(e.what());
        }
        lo_ = QVector3D(1e30f, 1e30f, 1e30f), hi_ = -lo_, tris_ = 0;
        for (const auto& g : model_.groups) {
            tris_ += g.vertices.size() / 27;
            for (size_t i = 0; i < g.vertices.size(); i += 9) {
                const QVector3D p(g.vertices[i], g.vertices[i + 1], g.vertices[i + 2]);
                lo_ = QVector3D(std::min(lo_.x(), p.x()), std::min(lo_.y(), p.y()), std::min(lo_.z(), p.z()));
                hi_ = QVector3D(std::max(hi_.x(), p.x()), std::max(hi_.y(), p.y()), std::max(hi_.z(), p.z()));
            }
        }
        dirty_ = true;
        reset();
        return err;
    }
    size_t triangles() const { return tris_; }
    ~ModelView() override {  // GL objects die with a current context
        makeCurrent();
        gpu_.clear();
        white_.reset();
        prog_.removeAllShaders();
        doneCurrent();
    }
    void reset() {
        yaw_ = 30, pitch_ = 15, zoom_ = 1;
        update();
    }

protected:
    void initializeGL() override {
        initializeOpenGLFunctions();
        prog_.addShaderFromSourceCode(QOpenGLShader::Vertex,
            "attribute vec3 pos; attribute vec2 uv; attribute vec4 col; uniform mat4 mvp; varying vec2 v_uv; varying vec4 v_col;\n"
            "void main() { v_uv = uv; v_col = col; gl_Position = mvp * vec4(pos, 1.0); }\n");
        prog_.addShaderFromSourceCode(QOpenGLShader::Fragment,
            "#ifdef GL_ES\nprecision mediump float;\n#endif\n"
            "uniform sampler2D tex; uniform float cut; varying vec2 v_uv; varying vec4 v_col;\n"
            "void main() { vec4 t = texture2D(tex, v_uv); if (t.a < cut) discard;\n"
            "  gl_FragColor = vec4(min(t.rgb * v_col.rgb * 2.0, 1.0), t.a * v_col.a); }\n");
        prog_.bindAttributeLocation("pos", 0), prog_.bindAttributeLocation("uv", 1), prog_.bindAttributeLocation("col", 2);
        prog_.link();
        QImage white(1, 1, QImage::Format_RGBA8888);
        white.fill(Qt::white);
        white_ = std::make_unique<QOpenGLTexture>(white);
    }
    void paintGL() override {
        if (dirty_) upload();
        glClearColor(0.05f, 0.05f, 0.12f, 1);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        if (gpu_.empty()) return;
        glEnable(GL_DEPTH_TEST);
        glDisable(GL_CULL_FACE);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        const QVector3D c = (lo_ + hi_) / 2;
        const float r = std::max((hi_ - lo_).length() / 2, 1e-6f), dist = r * 3.2f * zoom_;  // sphere fits the 40° view
        QMatrix4x4 mvp;
        mvp.perspective(40, float(width()) / float(std::max(height(), 1)), dist / 100, dist + r * 4);
        mvp.translate(0, 0, -dist);
        mvp.rotate(pitch_, 1, 0, 0);
        mvp.rotate(yaw_, 0, 1, 0);
        mvp.translate(-c);
        prog_.bind();
        prog_.setUniformValue("mvp", mvp);
        prog_.setUniformValue("tex", 0);
        for (Gpu& g : gpu_) {
            g.buf->bind();
            for (int a = 0; a < 3; ++a) prog_.enableAttributeArray(a);
            prog_.setAttributeBuffer(0, GL_FLOAT, 0, 3, 36);
            prog_.setAttributeBuffer(1, GL_FLOAT, 12, 2, 36);
            prog_.setAttributeBuffer(2, GL_FLOAT, 20, 4, 36);
            prog_.setUniformValue("cut", g.alphaTest ? 0.5f : 0.0f);
            (g.tex ? g.tex.get() : white_.get())->bind(0);
            glDrawArrays(GL_TRIANGLES, 0, g.count);
        }
        prog_.release();
    }
    void mousePressEvent(QMouseEvent* e) override { last_ = e->position(); }
    void mouseMoveEvent(QMouseEvent* e) override {
        const QPointF d = e->position() - last_;
        last_ = e->position();
        yaw_ += float(d.x()) * 0.5f, pitch_ = std::clamp(pitch_ + float(d.y()) * 0.5f, -89.0f, 89.0f);
        update();
    }
    void wheelEvent(QWheelEvent* e) override {
        zoom_ = std::clamp(zoom_ * std::pow(0.999f, float(e->angleDelta().y())), 0.05f, 20.0f);
        update();
    }
    void mouseDoubleClickEvent(QMouseEvent*) override { reset(); }

private:
    struct Gpu {
        std::unique_ptr<QOpenGLBuffer> buf;
        std::unique_ptr<QOpenGLTexture> tex;
        int count = 0;
        bool alphaTest = false;
    };
    bb::mods::Preview model_;
    std::vector<Gpu> gpu_;
    QOpenGLShaderProgram prog_;
    std::unique_ptr<QOpenGLTexture> white_;
    QVector3D lo_, hi_;
    float yaw_ = 30, pitch_ = 15, zoom_ = 1;
    QPointF last_;
    bool dirty_ = false;
    size_t tris_ = 0;

    void upload() {
        dirty_ = false;
        gpu_.clear();
        for (const auto& g : model_.groups) {
            Gpu& o = gpu_.emplace_back();
            o.buf = std::make_unique<QOpenGLBuffer>();
            o.buf->create();
            o.buf->bind();
            o.buf->allocate(g.vertices.data(), int(g.vertices.size() * sizeof(float)));
            o.count = int(g.vertices.size() / 9);
            o.alphaTest = g.alphaTest;
            QImage img = g.image.empty() ? QImage::fromData(g.imageBytes.data(), int(g.imageBytes.size())) : QImage(q(g.image));
            if (img.isNull()) continue;
            o.tex = std::make_unique<QOpenGLTexture>(img.convertToFormat(QImage::Format_RGBA8888), QOpenGLTexture::DontGenerateMipMaps);
            o.tex->setMinMagFilters(QOpenGLTexture::Nearest, QOpenGLTexture::Nearest);  // the exporter's sampler: PS1 look
            o.tex->setWrapMode(QOpenGLTexture::Repeat);
        }
        model_ = {};  // on the GPU now
    }
};

// ---------------------------------------------------------------- main window

class Launcher : public QMainWindow {
public:
    Launcher() {
        std::vector<std::string> warn;
        cfg_ = bbtime::loadSettings(bbtime::settingsPath(), &warn);
        if (bbtime::update::manifestUrl(cfg_.updateUrl) == bbtime::update::kDefaultManifestUrl)
            cfg_.updateUrl.clear();  // an old pinned URL into our releases: not written back on Save
        setWindowTitle(QString("bbtime launcher %1").arg(BBTIME_VERSION));
        game_ = new QComboBox;
        game_->addItems({"Collection menu", kTitles[0], kTitles[1]});

        // TB front-end layout: a column of menu.men picture buttons, the page in a LiT panel on the spiral.
        pages_ = new QStackedWidget;
        heading_ = new QLabel;
        heading_->setObjectName("heading");
        auto* nav = new QVBoxLayout;
        nav->setSpacing(2);
        auto* group = new QButtonGroup(this);
        const std::tuple<const char*, const char*, QWidget*> items[] = {
            {"Games", "Play2", gamesTab()},       {"Graphics & audio", "Video", graphicsTab()},
            {"Controls", "Controls", controlsTab()}, {"Saves", "LoadGame", savesTab()},
            {"Mods", "Play1", modsTab()},          {"Logs", "Load", logsTab()},
            {"Updates", "Config", updatesTab()},
            {"About", "Language", aboutTab()}};
        for (const auto& [name, art, page] : items) {
            auto* b = navButton(QString(name).replace('&', "&&"), art);
            b->setCheckable(true);
            group->addButton(b, pages_->addWidget(page));
            nav->addWidget(b);
            connect(b, &QToolButton::clicked, this, [this, b, page, n = QString(name)] {
                pages_->setCurrentWidget(page);
                heading_->setText(n);
            });
        }
        nav->addStretch();
        auto* quit = navButton("Quit", "Quit");
        nav->addWidget(quit);
        connect(quit, &QToolButton::clicked, this, &QWidget::close);

        auto* save = new QPushButton("Save settings");
        play_ = new QPushButton("Play");
        play_->setObjectName("play");
        play_->setDefault(true);
        auto* path = new QLabel(q(bbtime::settingsPath()));
        path->setStyleSheet("color: #808096; font-family: monospace;");
        auto* bottom = new QHBoxLayout;
        bottom->addWidget(path);
        bottom->addStretch();
        bottom->addWidget(game_);
        bottom->addWidget(save);
        bottom->addWidget(play_);

        auto* panel = new Panel;
        auto* pl = new QVBoxLayout(panel);
        pl->setContentsMargins(18, 10, 18, 16);
        pl->addWidget(heading_);
        pl->addWidget(pages_, 1);
        auto* body = new QHBoxLayout;
        body->addLayout(nav);
        body->addWidget(panel, 1);
        back_ = new Backdrop;
        auto* l = new QVBoxLayout(back_);
        l->addLayout(body, 1);
        l->addLayout(bottom);
        setCentralWidget(back_);
        group->button(0)->click();

        connect(save, &QPushButton::clicked, this, [this] { saveSettings(); });
        connect(play_, &QPushButton::clicked, this, [this] { launch(); });
        connect(game_, &QComboBox::currentIndexChanged, this, [this](int i) {
            for (int g = 0; g < 2; ++g) card_[g]->setChecked(i == g + 1);
        });
        for (const auto& w : warn) statusBar()->showMessage(q("settings.ini " + w), 10000);
        applyArt();
        refreshGames();
        resize(1000, 700);
    }

private:
    bbtime::Settings cfg_;
    QComboBox* game_;
    QPushButton* play_;
    QStackedWidget* pages_;
    QLabel* heading_;
    Backdrop* back_;
    std::vector<std::pair<QToolButton*, const char*>> art_; // buttons dressed with TB menu.men bitmaps
    QToolButton* card_[2]{};
    QProcess* proc_ = nullptr;
    QLabel* gameStatus_[2]{};
    QComboBox *renderer_{}, *fidelity_{}, *msaa_{}, *shadows_{}, *upscaling_{}, *water_{};
    QSpinBox *width_{}, *height_{};
    QDoubleSpinBox* fov_{};
    QCheckBox *fullscreen_{}, *vsync_{}, *reflections_{}, *frameGen_{};
    QSlider *music_{}, *sfx_{};
    QTableWidget* binds_{};
    QListWidget *pads_{}, *backups_{}, *logs_{};
    QPlainTextEdit* logView_{};
    QLabel* updateStatus_{};
    QTextBrowser* notes_{};
    QPushButton *installUpdate_{}, *rollback_{};
    QCheckBox* force_{};
    QProgressBar* updateBar_{};
    std::optional<bbtime::update::Manifest> pending_;

    static fs::path backupRoot() { return bbtime::dataDir() / "backups"; }
    static fs::path logRoot() { return bbtime::stateDir() / "logs"; }

    QToolButton* navButton(const QString& text, const char* art) {
        auto* b = new QToolButton;
        b->setObjectName("nav");
        b->setText(text);
        b->setIconSize(QSize(64, 46));
        b->setAutoRaise(true);
        b->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
        b->setMinimumWidth(130);
        art_.emplace_back(b, art);
        return b;
    }

    // (Re)dresses the window from the imported installs; called again after the import wizard.
    void applyArt() {
        const fs::path& tb = cfg_.tbRoot;
        QString family = "sans-serif";
        if (const QString ttf = findCi(tb, {"Datas", "data", "bradybun.ttf"}); !ttf.isEmpty()) {
            const int id = QFontDatabase::addApplicationFont(ttf); // same file again: Qt returns the same family
            if (const QStringList f = QFontDatabase::applicationFontFamilies(id); !f.isEmpty()) family = '"' + f.first() + '"';
        }
        qApp->setStyleSheet(QString(kStyle).arg(family));
        if (back_->art = bmp(tb, {"Datas", "data", "pc03.bmp"}); back_->art.isNull()) {
            back_->art = bmp(cfg_.litRoot, {"Datas", "ScrnBgEn.bmp"}); // LiT only: its title, darkened
            back_->dim = 150;
        } else {
            back_->dim = 0;
        }
        back_->update();
        for (auto [b, name] : art_) {
            const QString stem = QString("%1.bmp").arg(name), hot = QString("%1D.bmp").arg(name);
            const QPixmap n = bmp(tb, {"Datas", "data", qPrintable(stem)}), h = bmp(tb, {"Datas", "data", qPrintable(hot)});
            QIcon icon;
            if (!n.isNull()) {
                icon.addPixmap(n, QIcon::Normal, QIcon::Off);
                for (auto [mode, state] : {std::pair{QIcon::Active, QIcon::Off}, std::pair{QIcon::Normal, QIcon::On},
                                           std::pair{QIcon::Active, QIcon::On}})
                    icon.addPixmap(h.isNull() ? n : h, mode, state);
            }
            b->setIcon(icon);
            b->setToolButtonStyle(icon.isNull() ? Qt::ToolButtonTextOnly : Qt::ToolButtonTextUnderIcon);
        }
        const QPixmap titles[2] = {bmp(cfg_.litRoot, {"Datas", "ScrnBgEn.bmp"}), bmp(tb, {"Datas", "data", "english.bmp"})};
        for (int g = 0; g < 2; ++g) {
            card_[g]->setIcon(titles[g]);
            card_[g]->setToolButtonStyle(titles[g].isNull() ? Qt::ToolButtonTextOnly : Qt::ToolButtonIconOnly);
        }
        play_->setIcon(QIcon(bmp(tb, {"Datas", "data", "Play1D.bmp"})));
        play_->setIconSize(QSize(46, 42));
    }

    QWidget* gamesTab() {
        auto* w = new QWidget;
        auto* l = new QVBoxLayout(w);
        for (int g = 0; g < 2; ++g) {
            auto* box = new QGroupBox(QString(kTitles[g]).replace('&', "&&"));
            auto* bl = new QHBoxLayout(box);
            card_[g] = new QToolButton; // the game's title screen; picks what Play starts
            card_[g]->setObjectName("card");
            card_[g]->setText(g ? "Time Busters" : "Lost in Time"); // shown until the title art is imported
            card_[g]->setToolTip(QString("Play starts %1").arg(kTitles[g]));
            card_[g]->setCheckable(true);
            card_[g]->setIconSize(QSize(176, 132));
            card_[g]->setFixedSize(188, 144);
            connect(card_[g], &QToolButton::clicked, this, [this, g] {
                game_->setCurrentIndex(game_->currentIndex() == g + 1 ? 0 : g + 1); // again: back to the collection menu
                card_[g]->setChecked(game_->currentIndex() == g + 1);
            });
            gameStatus_[g] = new QLabel;
            gameStatus_[g]->setObjectName("status");
            gameStatus_[g]->setTextInteractionFlags(Qt::TextSelectableByMouse);
            gameStatus_[g]->setWordWrap(true);
            bl->addWidget(card_[g]);
            bl->addWidget(gameStatus_[g], 1);
            l->addWidget(box);
        }
        auto* wizard = new QPushButton("Import original games…");
        connect(wizard, &QPushButton::clicked, this, [this] {
            if (runImportWizard(this, cfg_)) {
                saveSettings();
                applyArt();
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
        shadows_ = new QComboBox;
        for (auto [label, key] : {std::pair{"Off", "off"}, std::pair{"Shadow map", "map"}, std::pair{"Ray-traced", "rt"},
                                  std::pair{"Ray-traced + AO", "rt_ao"}})
            shadows_->addItem(label, key);
        shadows_->setCurrentIndex(std::max(0, shadows_->findData(q(cfg_.shadows))));
        reflections_ = new QCheckBox("Ray-traced reflections (water, ice, polished floors)");
        reflections_->setChecked(cfg_.reflections);
        water_ = new QComboBox;
        water_->addItem("Enhanced (refraction, shore foam, reflections, waves)", "enhanced");
        water_->addItem("Cartoon (toon-shaded, same effects)", "cartoon");
        water_->addItem("Original", "original");
        water_->setCurrentIndex(std::max(0, water_->findData(q(cfg_.water))));
        upscaling_ = new QComboBox;
        for (auto [label, key] : {std::pair{"Off", "off"}, std::pair{"FSR Quality", "fsr_quality"}, std::pair{"FSR Balanced", "fsr_balanced"},
                                  std::pair{"FSR Performance", "fsr_performance"}, std::pair{"FSR Native AA", "native_aa"}})
            upscaling_->addItem(label, key);
        upscaling_->setCurrentIndex(std::max(0, upscaling_->findData(q(cfg_.upscaling))));
        frameGen_ = new QCheckBox("FSR frame generation");
        frameGen_->setChecked(cfg_.frameGeneration);
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
        f->addRow("Shadows (Vulkan)", shadows_);
        f->addRow("", reflections_);
        f->addRow("Water (Vulkan)", water_);
        f->addRow("Upscaling (Vulkan)", upscaling_);
        f->addRow("", frameGen_);
        f->addRow("Music volume", music_);
        f->addRow("Effects volume", sfx_);
        keyLabels(f);
        return w;
    }

    // Form labels in the TB key-label orange.
    static void keyLabels(QFormLayout* f) {
        for (int i = 0; i < f->rowCount(); ++i)
            if (QLayoutItem* it = f->itemAt(i, QFormLayout::LabelRole); it && it->widget()) it->widget()->setObjectName("key");
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

    // ------------------------------------------------------------ mods (docs/MODDING.md)
    QTableWidget* mods_{};
    QTextBrowser* modInfo_{};

    static fs::path modsRoot() { return bb::mods::defaultRoot(); }

    // Rows in load order: enabled mods as in settings.ini, then the rest by id.
    void refreshMods() {
        std::vector<std::string> enabled;
        for (int r = 0; mods_->rowCount() && r < mods_->rowCount(); ++r)
            if (mods_->item(r, 0)->checkState() == Qt::Checked) enabled.push_back(mods_->item(r, 0)->data(Qt::UserRole).toString().toStdString());
        if (!mods_->rowCount()) enabled = cfg_.mods;
        std::vector<bb::mods::Info> all = bb::mods::scan(modsRoot()), rows;
        for (const std::string& id : enabled)
            if (auto it = std::ranges::find(all, id, &bb::mods::Info::id); it != all.end()) rows.push_back(*it), all.erase(it);
        rows.insert(rows.end(), all.begin(), all.end());
        const size_t on = rows.size() - all.size();
        const QSignalBlocker block(mods_);
        mods_->setRowCount(int(rows.size()));
        for (size_t r = 0; r < rows.size(); ++r) {
            const bb::mods::Info& m = rows[r];
            auto* name = new QTableWidgetItem(q(m.error.empty() ? m.name : m.id + " (broken)"));
            name->setData(Qt::UserRole, q(m.id));
            name->setFlags(Qt::ItemIsSelectable | Qt::ItemIsEnabled | Qt::ItemIsUserCheckable);
            name->setCheckState(r < on ? Qt::Checked : Qt::Unchecked);
            name->setToolTip(q(m.dir));
            mods_->setItem(int(r), 0, name);
            const QString game = m.game == "lit" ? "Lost in Time" : m.game == "tb" ? "Time Busters" : "both";
            int c = 1;
            for (const QString& v : {q(m.version), q(m.author), game}) {
                auto* it = new QTableWidgetItem(v);
                it->setFlags(Qt::ItemIsSelectable | Qt::ItemIsEnabled);
                mods_->setItem(int(r), c++, it);
            }
        }
        collectMods();
        showModInfo();
    }

    void collectMods() {
        cfg_.mods.clear();
        for (int r = 0; r < mods_->rowCount(); ++r)
            if (mods_->item(r, 0)->checkState() == Qt::Checked) cfg_.mods.push_back(mods_->item(r, 0)->data(Qt::UserRole).toString().toStdString());
    }

    std::string selectedMod() const {
        const int r = mods_->currentRow();
        return r < 0 ? std::string() : mods_->item(r, 0)->data(Qt::UserRole).toString().toStdString();
    }

    // Details of the selected mod, its validation (levels decoded in the background) and the enabled set's conflicts.
    void showModInfo() {
        const bb::mods::ModSet set = bb::mods::ModSet::load(modsRoot(), cfg_.mods);
        QString conflicts;
        for (const auto& [key, ids] : set.conflicts()) {
            QStringList l;
            for (const std::string& id : ids) l << q(id);
            conflicts += QString("<li><code>%1</code>: %2 (<b>%3</b> wins)</li>").arg(q(key), l.join(" &lt; "), l.last());
        }
        const QString tail = QString("<h3>Conflicts (enabled mods, load order)</h3>%1<p style='color:#808096'>Mods folder: %2</p>")
                                 .arg(conflicts.isEmpty() ? "<p>none</p>" : "<ul>" + conflicts + "</ul>", q(modsRoot()).toHtmlEscaped());
        const std::string id = selectedMod();
        if (id.empty()) {
            modInfo_->setHtml("<p>Select a mod. Tick a mod to enable it; enabled mods load top to bottom (later wins).</p>" + tail);
            return;
        }
        const bb::mods::Info info = bb::mods::readInfo(modsRoot() / id);
        const QString head = QString("<h3>%1 %2</h3><p>%3</p><p>by %4 · game %5 · id <code>%6</code></p>")
                                 .arg(q(info.name).toHtmlEscaped(), q(info.version).toHtmlEscaped(), q(info.description).toHtmlEscaped(),
                                      q(info.author).toHtmlEscaped(), q(info.game), q(info.id));
        modInfo_->setHtml(head + "<p>Validating…</p>" + tail);
        struct Ctx {
            bbtime::Settings cfg;
            std::map<std::string, std::unique_ptr<bb::Scene>> cache;
        };
        background(this, [info, cfg = cfg_] {
            Ctx ctx{cfg, {}};
            auto src = [](void* c, GameId g, const std::string& level) -> const bb::Scene* {
                Ctx& x = *static_cast<Ctx*>(c);
                auto [it, fresh] = x.cache.try_emplace(std::format("{}/{}", int(g), level));
                if (fresh) try {
                    const QString f = findCi(g == GameId::LostInTime ? x.cfg.litRoot : x.cfg.tbRoot, {"Datas", "bze", (level + ".bze").c_str()});
                    if (!f.isEmpty()) {
                        const bb::BzeFile bze = bb::loadBze(path(f));
                        auto sc = std::make_unique<bb::Scene>(g == GameId::LostInTime ? bb::buildLitScene(bze, level) : bb::buildTbScene(bze, level));
                        if (g == GameId::TimeBusters) bb::addTbActors(*sc, bze);
                        it->second = std::move(sc);
                    }
                } catch (const std::exception&) {}
                return it->second.get();
            };
            return bb::mods::validate(info, src, &ctx);
        }, [this, id, head, tail](const std::vector<bb::mods::Issue>& issues) {
            if (selectedMod() != id) return;
            QString v;
            for (const auto& i : issues)
                v += QString("<li><span style='color:%1'>%2</span> <code>%3</code>: %4</li>")
                         .arg(i.error ? "#ff6060" : "#f8e600", i.error ? "error" : "warning", q(i.file).toHtmlEscaped(), q(i.message).toHtmlEscaped());
            modInfo_->setHtml(head + "<h3>Validation</h3>" + (v.isEmpty() ? "<p>no problems found</p>" : "<ul>" + v + "</ul>") + tail);
        });
    }

    void moveMod(int d) {
        const int r = mods_->currentRow(), t = r + d;
        if (r < 0 || t < 0 || t >= mods_->rowCount()) return;
        for (int c = 0; c < mods_->columnCount(); ++c) {
            QTableWidgetItem* a = mods_->takeItem(r, c);
            mods_->setItem(r, c, mods_->takeItem(t, c));
            mods_->setItem(t, c, a);
        }
        mods_->setCurrentCell(t, 0);
        collectMods();
        showModInfo();
    }

    // Returns the new mod's id (selected in the table), empty if cancelled / failed.
    std::string newMod() {
        QDialog d(this);
        d.setWindowTitle("New mod");
        auto* f = new QFormLayout(&d);
        auto* id = new QLineEdit;
        id->setValidator(new QRegularExpressionValidator(QRegularExpression("[A-Za-z0-9._-]+"), id));
        auto *name = new QLineEdit, *version = new QLineEdit("1.0"), *author = new QLineEdit;
        auto* desc = new QPlainTextEdit;
        desc->setMaximumHeight(80);
        auto* game = new QComboBox;
        game->addItem("Both games", "both"), game->addItem(kTitles[0], "lit"), game->addItem(kTitles[1], "tb");
        f->addRow("Folder id", id), f->addRow("Name", name), f->addRow("Version", version), f->addRow("Author", author);
        f->addRow("Game", game), f->addRow("Description", desc);
        auto* bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
        f->addRow(bb);
        connect(bb, &QDialogButtonBox::accepted, &d, &QDialog::accept);
        connect(bb, &QDialogButtonBox::rejected, &d, &QDialog::reject);
        if (d.exec() != QDialog::Accepted || id->text().isEmpty()) return {};
        bb::mods::Info i;
        i.id = id->text().toStdString(), i.name = name->text().isEmpty() ? i.id : name->text().toStdString();
        i.version = version->text().toStdString(), i.author = author->text().toStdString();
        i.description = desc->toPlainText().toStdString(), i.game = game->currentData().toString().toStdString();
        i.dir = modsRoot() / i.id;
        if (fs::exists(i.dir)) { QMessageBox::warning(this, "New mod", "A mod folder with that id exists."); return {}; }
        const std::string e = bb::mods::writeInfo(i);
        for (const char* sub : {"textures", "models", "animations", "sounds", "music"}) fs::create_directories(i.dir / sub);
        statusBar()->showMessage(e.empty() ? "Created " + q(i.dir) : q(e), 8000);
        refreshMods();
        for (int r = 0; r < mods_->rowCount(); ++r)
            if (mods_->item(r, 0)->data(Qt::UserRole).toString() == q(i.id)) mods_->setCurrentCell(r, 0);
        return e.empty() ? i.id : std::string();
    }

    // .zip with mod.json at its root or in one top folder; extracted with unzip (or bsdtar), which refuse ../ paths.
    void installZip() {
        const QString zip = QFileDialog::getOpenFileName(this, "Install mod", QDir::homePath(), "Mod archives (*.zip)");
        if (zip.isEmpty()) return;
        fs::create_directories(modsRoot());
        const fs::path tmp = modsRoot() / ".install";
        fs::remove_all(tmp);
        fs::create_directories(tmp);
        QProcess p;
        if (!QStandardPaths::findExecutable("unzip").isEmpty()) p.start("unzip", {"-q", zip, "-d", q(tmp)});
        else p.start("bsdtar", {"-xf", zip, "-C", q(tmp)});
        p.waitForFinished(120000);
        fs::path dir;
        std::error_code ec;
        for (fs::recursive_directory_iterator it(tmp, ec), end; !ec && it != end; it.increment(ec))
            if (it.depth() <= 1 && it->path().filename() == "mod.json") { dir = it->path().parent_path(); break; }
        QString msg;
        if (p.exitCode() != 0 || dir.empty()) msg = "Not a mod archive (no mod.json): " + zip;
        else {
            // Folder name = id: the mod.json "id" (else the archive's top folder, else the archive name).
            QFile mj(q(dir / "mod.json"));
            QString id = mj.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(mj.readAll()).object().value("id").toString() : QString();
            if (id.isEmpty()) id = dir == tmp ? QFileInfo(zip).completeBaseName() : q(dir.filename());
            const fs::path dest = modsRoot() / path(id);
            if (id.contains('/') || id.startsWith('.')) msg = "Bad mod id in mod.json: " + id;
            else if (fs::exists(dest)) msg = "A mod with the id " + id + " is installed; remove it first.";
            else {
                fs::rename(dir, dest, ec);
                msg = ec ? q("Install failed: " + ec.message()) : "Installed " + q(dest);
            }
        }
        fs::remove_all(tmp, ec);
        statusBar()->showMessage(msg, 10000);
        refreshMods();
    }

    void removeMod() {
        const std::string id = selectedMod();
        if (id.empty() || QMessageBox::question(this, "Remove mod", "Delete the mod folder " + q(modsRoot() / id) + "?") != QMessageBox::Yes)
            return;
        std::error_code ec;
        fs::remove_all(modsRoot() / id, ec);
        statusBar()->showMessage(ec ? q(ec.message()) : "Removed " + q(id), 8000);
        mods_->removeRow(mods_->currentRow());
        refreshMods();
    }

    static QString modkitBinary() { return toolBinary("bbtime-modkit", "/../../tools/bbtime-modkit"); }

    // An updater-installed copy when it is at least as new as this launcher, then the one bundled next to the launcher
    // (AppImage / install / build tree), then PATH. A newer AppImage never runs an older installed game.
    static QString toolBinary(const char* name, const char* buildTree) {
        const QString here = QCoreApplication::applicationDirPath();
        const QString installed = q(bbtime::update::installedArtifact(appRoot(), BBTIME_VERSION, name));
        for (const QString& c : {installed, here + "/" + name, here + buildTree})
            if (!c.isEmpty() && QFileInfo(c).isExecutable()) return QFileInfo(c).canonicalFilePath();
        return QStandardPaths::findExecutable(name);
    }

    // Game / level / kind picker -> bbtime-modkit export (originals stay in the user's own folder).
    void exportAssets() {
        QDialog d(this);
        d.setWindowTitle("Export assets");
        auto* f = new QFormLayout(&d);
        auto* game = new QComboBox;
        game->addItem(kTitles[0], 0), game->addItem(kTitles[1], 1);
        auto* level = new QComboBox;
        level->setEditable(true);
        auto* kind = new QComboBox;
        for (const char* k : {"all", "textures", "models", "sounds", "music"}) kind->addItem(k);
        auto* out = new QLineEdit(q(bbtime::dataDir() / "modkit-export"));
        auto fill = [this, game, level] {
            level->clear();
            level->addItems(levels(game->currentData().toInt()));
            level->setCurrentText(game->currentData().toInt() ? "ARCA01" : "L01A");
        };
        connect(game, &QComboBox::currentIndexChanged, &d, fill);
        fill();
        f->addRow("Game", game), f->addRow("Level", level), f->addRow("What", kind), f->addRow("Output folder", out);
        f->addRow(new QLabel("Textures → PNG, models + animations → glTF, sounds/music → WAV, in the mod layout.\n"
                             "Edit them, then copy the files you changed into a mod folder."));
        auto* bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
        f->addRow(bb);
        connect(bb, &QDialogButtonBox::accepted, &d, &QDialog::accept);
        connect(bb, &QDialogButtonBox::rejected, &d, &QDialog::reject);
        if (d.exec() != QDialog::Accepted) return;
        const QString exe = modkitBinary();
        if (exe.isEmpty()) { QMessageBox::warning(this, "Export", "Cannot find bbtime-modkit."); return; }
        const int g = game->currentData().toInt();
        const QStringList args{"export", "--game", kKeys[g], "--level", level->currentText(), "--what", kind->currentText(), "--out",
                               out->text(), "--root", q(root(cfg_, g))};
        auto* p = new QProcess(this);
        p->setProcessChannelMode(QProcess::MergedChannels);
        modInfo_->setHtml("<p>Exporting…</p>");
        connect(p, &QProcess::finished, this, [this, p, dir = out->text()](int code) {
            modInfo_->setHtml(QString("<h3>Export %1</h3><pre>%2</pre><p><a href='%3'>%4</a></p>")
                                  .arg(code ? "failed" : "done", QString::fromUtf8(p->readAll()).toHtmlEscaped(),
                                       QUrl::fromLocalFile(dir).toString(), dir.toHtmlEscaped()));
            p->deleteLater();
        });
        p->start(exe, args);
    }

    QStringList levels(int g) {
        const QString dir = findCi(root(cfg_, g), {"Datas", "bze"});
        QStringList l;
        for (const QFileInfo& fi : QDir(dir).entryInfoList({"*.bze", "*.BZE"}, QDir::Files)) l << fi.completeBaseName().toUpper();
        l.removeDuplicates();
        l.sort();
        return l;
    }

    // ------------------------------------------------------------ model viewer (Mods page)
    ModelView* view_{};       // created on first preview (see viewerPage)
    QVBoxLayout* viewHost_{};
    QListWidget* models_{};
    QLabel* viewNote_{};
    QComboBox *viewGame_{}, *viewLevel_{};

    // Originals of one level, exported once by bbtime-modkit (--what models) into the user's cache.
    fs::path originalsDir() const {
        return path(QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation)) / "bbtime" / "viewer" /
               kKeys[viewGame_->currentData().toInt()] / viewLevel_->currentText().toUpper().toStdString();
    }

    static std::vector<fs::path> gltfFiles(const fs::path& dir) {
        std::vector<fs::path> files;
        std::error_code ec;
        for (fs::recursive_directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
            if (const QString e = q(it->path().extension()).toLower(); e == ".gltf" || e == ".glb") files.push_back(it->path());
        std::ranges::sort(files);
        return files;
    }

    // Lists the glTF files under base/<subs>; keys = the mod's files (originals they replace get a note).
    // Item data: UserRole file, +1 path relative to the mod / export root, +2 true for an original.
    void addModels(const QString& head, const fs::path& base, std::initializer_list<const char*> subs, std::set<std::string>& keys, bool original) {
        std::vector<fs::path> files;
        for (const char* sub : subs) std::ranges::copy(gltfFiles(base / sub), std::back_inserter(files));
        auto* h = new QListWidgetItem(QString("%1 (%2)").arg(head).arg(files.size()));
        h->setFlags(Qt::NoItemFlags);
        models_->addItem(h);
        for (const fs::path& f : files) {
            const fs::path rel = f.lexically_relative(base);
            const std::string key = bb::mods::keyOf(rel);
            auto* it = new QListWidgetItem(q(rel.generic_string()) + (original && keys.contains(key) ? "   ← replaced by the mod" : ""));
            it->setData(Qt::UserRole, q(f));
            it->setData(Qt::UserRole + 1, q(rel.generic_string()));
            it->setData(Qt::UserRole + 2, original);
            models_->addItem(it);
            if (!original) keys.insert(key);
        }
    }

    void refreshModels(const QString& select = {}) {
        models_->clear();
        std::set<std::string> keys;
        if (const std::string id = selectedMod(); !id.empty()) addModels(q("Mod " + id), modsRoot() / id, {"models", "animations"}, keys, false);
        const fs::path orig = originalsDir();
        if (fs::exists(orig / "models"))
            addModels("Originals " + viewLevel_->currentText().toUpper(), orig, {"models"}, keys, true);
        for (int r = 0; !select.isEmpty() && r < models_->count(); ++r)
            if (models_->item(r)->data(Qt::UserRole).toString() == select) models_->setCurrentRow(r);
    }

    // External files of a .gltf (buffers, images) as written in its URIs, percent-decoded; data: URIs and .glb
    // (embedded) have none. ponytail: external URIs inside a .glb are not followed.
    static std::vector<std::string> sideUris(const fs::path& file) {
        std::vector<std::string> out;
        QFile f(q(file));
        if (q(file.extension()).toLower() != ".gltf" || !f.open(QIODevice::ReadOnly)) return out;
        const QJsonObject o = QJsonDocument::fromJson(f.readAll()).object();
        for (const char* arr : {"buffers", "images"})
            for (const QJsonValue v : o.value(arr).toArray())
                if (const QString u = v.toObject().value("uri").toString(); !u.isEmpty() && !u.startsWith("data:"))
                    out.push_back(QUrl::fromPercentEncoding(u.toUtf8()).toStdString());
        return out;
    }
    // Where a URI of a glTF at `at` lands; empty if absolute or outside `dir`.
    static fs::path inside(const fs::path& dir, const fs::path& at, const std::string& uri) {
        if (fs::path(uri).is_absolute()) return {};
        const fs::path p = (at.parent_path() / uri).lexically_normal(), rel = p.lexically_relative(dir);
        return rel.empty() || *rel.begin() == ".." ? fs::path() : p;
    }

    // Copies a picked .gltf/.glb (+ the files it refers to, same relative URIs) into the selected mod at the path
    // the resolver looks up for the selected model (docs/MODDING.md "Ids").
    void replaceModel() {
        QListWidgetItem* it = models_->currentItem();
        if (!it || it->data(Qt::UserRole).isNull()) { viewNote_->setText("Select the model to replace in the list."); return; }
        // Read the item now: New mod… refreshes (and so deletes) the list items.
        const fs::path rel(it->data(Qt::UserRole + 1).toString().toStdString()), current = path(it->data(Qt::UserRole).toString());
        const bool isOriginal = it->data(Qt::UserRole + 2).toBool();
        std::string id = selectedMod();
        if (id.empty()) {
            if (QMessageBox::question(this, "Replace model", "Replacements are stored in a mod and no mod is selected.\nCreate a new mod?") != QMessageBox::Yes)
                return;
            if ((id = newMod()).empty()) return;
        }
        const QString pick = QFileDialog::getOpenFileName(this, "Replace " + q(rel.generic_string()), QDir::homePath(), "glTF models (*.gltf *.glb)");
        if (pick.isEmpty()) return;
        const fs::path src = path(pick), modDir = modsRoot() / id;
        const fs::path dest = modDir / fs::path(rel).replace_extension(q(src.extension()).toLower().toStdString());
        std::error_code ec;
        if (fs::equivalent(src, dest, ec)) { viewNote_->setText("That file is already the mod's replacement."); return; }
        bb::mods::Preview next;
        try {
            next = bb::mods::loadPreview(src);
        } catch (const std::exception& e) {
            QMessageBox::warning(this, "Replace model", "Cannot read " + pick + ":\n" + QString::fromUtf8(e.what()));
            return;
        }
        if (next.groups.empty()) { QMessageBox::warning(this, "Replace model", pick + " has no triangles."); return; }
        QStringList warn, copies{q(dest.lexically_relative(modDir).generic_string())}, overwrite;
        // The part<N> / "<index>#" world nodes the game addresses (MODDING.md "Models"): the original's, else the current file's.
        fs::path ref = originalsDir() / fs::path(rel).replace_extension(".gltf");
        if (isOriginal || !fs::exists(ref)) ref = current;
        try {
            static const QRegularExpression named(R"(^(part\d+|\d+#.*)$)");
            QStringList missing;
            for (const std::string& n : bb::mods::loadPreview(ref).nodes)
                if (named.match(q(n)).hasMatch() && std::ranges::find(next.nodes, n) == next.nodes.end()) missing << q(n);
            if (!missing.isEmpty())
                warn << QString("%1 node(s) of the original are missing (the game moves parts by these names): %2%3")
                            .arg(missing.size()).arg(missing.mid(0, 12).join(", "), QString(missing.size() > 12 ? ", …" : ""));
        } catch (const std::exception&) {}
        const QString game = q(rel.generic_string()).section('/', 1, 1);  // models/<game>/...
        if (const bb::mods::Info info = bb::mods::readInfo(modDir); info.game != "both" && q(info.game) != game)
            warn << QString("the mod is for '%1', this model is a '%2' model").arg(q(info.game), game);
        std::vector<std::pair<fs::path, fs::path>> side;
        for (const std::string& u : sideUris(src)) {
            const fs::path from = (src.parent_path() / u).lexically_normal(), to = inside(modDir, dest, u);
            if (to.empty()) warn << q("not copied (absolute or outside the mod folder): " + u);
            else if (!fs::is_regular_file(from)) warn << q("missing next to the model: " + u);
            else if (!fs::equivalent(from, to, ec)) side.emplace_back(from, to), copies << q(to.lexically_relative(modDir).generic_string());
        }
        for (const QString& c : copies)
            if (fs::exists(modDir / path(c))) overwrite << c;
        for (const char* e : {".gltf", ".glb"})  // one file per key: the other format goes
            if (const fs::path o = fs::path(dest).replace_extension(e); o != dest && fs::exists(o)) overwrite << q(o.lexically_relative(modDir).generic_string()) + " (removed)";
        if (!warn.isEmpty() || !overwrite.isEmpty()) {
            QMessageBox box(QMessageBox::Question, "Replace model", QString("Copy into mod '%1':\n  %2").arg(q(id), copies.join("\n  ")),
                            QMessageBox::Yes | QMessageBox::No, this);
            QString info;
            if (!overwrite.isEmpty()) info += "Overwrites:\n  " + overwrite.join("\n  ") + "\n";
            if (!warn.isEmpty()) info += "Warnings:\n  " + warn.join("\n  ");
            box.setInformativeText(info);
            if (box.exec() != QMessageBox::Yes) return;
        }
        side.emplace_back(src, dest);
        for (const auto& [from, to] : side) {
            fs::create_directories(to.parent_path(), ec);
            if (!fs::copy_file(from, to, fs::copy_options::overwrite_existing, ec)) {
                QMessageBox::warning(this, "Replace model", q("Copy failed: " + to.string() + ": " + ec.message()));
                return;
            }
        }
        for (const char* e : {".gltf", ".glb"})
            if (const fs::path o = fs::path(dest).replace_extension(e); o != dest) fs::remove(o, ec);
        statusBar()->showMessage(QString("Replaced %1 in mod %2 (%3 file(s))").arg(q(rel.generic_string()), q(id)).arg(side.size()), 10000);
        showModInfo();
        refreshModels(q(dest));
    }

    // Deletes the selected mod's file for the selected model plus the side files no other glTF of the mod uses.
    void removeReplacement() {
        QListWidgetItem* it = models_->currentItem();
        const std::string id = selectedMod();
        if (!it || it->data(Qt::UserRole).isNull() || id.empty()) { viewNote_->setText("Select a mod and one of its models (or the original it replaces)."); return; }
        const fs::path modDir = modsRoot() / id;
        const std::string key = bb::mods::keyOf(fs::path(it->data(Qt::UserRole + 1).toString().toStdString()));
        std::vector<fs::path> files, keep;
        for (const fs::path& f : gltfFiles(modDir)) (bb::mods::keyOf(f.lexically_relative(modDir)) == key ? files : keep).push_back(f);
        if (files.empty()) { viewNote_->setText(q("Mod " + id + " does not replace this model.")); return; }
        std::set<fs::path> used, gone(files.begin(), files.end());
        for (const fs::path& f : keep)
            for (const std::string& u : sideUris(f)) used.insert(inside(modDir, f, u));
        for (const fs::path& f : files)
            for (const std::string& u : sideUris(f))
                if (const fs::path p = inside(modDir, f, u); !p.empty() && !used.contains(p) && fs::is_regular_file(p)) gone.insert(p);
        QStringList l;
        for (const fs::path& p : gone) l << q(p.lexically_relative(modDir).generic_string());
        if (QMessageBox::question(this, "Remove replacement", QString("Delete from mod '%1':\n  %2").arg(q(id), l.join("\n  "))) != QMessageBox::Yes)
            return;
        std::error_code ec;
        for (const fs::path& p : gone) fs::remove(p, ec);
        statusBar()->showMessage(QString("Removed %1 file(s) from mod %2").arg(gone.size()).arg(q(id)), 10000);
        showModInfo();
        refreshModels();
    }

    void loadOriginals() {
        if (viewLevel_->currentText().trimmed().isEmpty()) return;
        if (fs::exists(originalsDir() / "models")) return refreshModels();
        const QString exe = modkitBinary();
        if (exe.isEmpty()) { viewNote_->setText("Cannot find bbtime-modkit."); return; }
        const int g = viewGame_->currentData().toInt();
        const fs::path out = originalsDir();
        auto* p = new QProcess(this);
        p->setProcessChannelMode(QProcess::MergedChannels);
        viewNote_->setText("Exporting the original models…");
        connect(p, &QProcess::finished, this, [this, p, out](int code) {
            if (code) {
                viewNote_->setText("Export failed: " + QString::fromUtf8(p->readAll()).trimmed().right(300));
                std::error_code ec;
                fs::remove_all(out, ec);  // no half export in the cache
            } else {
                viewNote_->setText("Originals exported to " + q(out));
            }
            refreshModels();
            p->deleteLater();
        });
        p->start(exe, {"export", "--game", kKeys[g], "--level", viewLevel_->currentText(), "--what", "models", "--out", q(out),
                       "--root", q(root(cfg_, g))});
    }

    QWidget* viewerPage() {
        auto* w = new QWidget;
        auto* l = new QHBoxLayout(w);
        auto* side = new QVBoxLayout;
        models_ = new QListWidget;
        viewGame_ = new QComboBox;
        viewGame_->addItem(kTitles[0], 0), viewGame_->addItem(kTitles[1], 1);
        viewLevel_ = new QComboBox;
        viewLevel_->setEditable(true);
        auto fill = [this] {
            const int g = viewGame_->currentData().toInt();
            viewLevel_->clear();
            viewLevel_->addItems(levels(g));
            viewLevel_->setCurrentText(g ? "ARCA01" : "L01A");
        };
        connect(viewGame_, &QComboBox::currentIndexChanged, this, fill);
        fill();
        auto* load = new QPushButton("Show originals");
        load->setToolTip("Exports this level's original models once (bbtime-modkit) into the cache and lists them");
        auto* pick = new QHBoxLayout;
        pick->addWidget(viewLevel_, 1);
        pick->addWidget(load);
        auto* edit = new QHBoxLayout;
        models_->setContextMenuPolicy(Qt::ActionsContextMenu);
        for (const auto& [text, tip, fn] : {std::tuple<const char*, const char*, void (Launcher::*)()>{
                 "Replace model…", "Copy a .gltf / .glb (and its files) into the selected mod at this model's path", &Launcher::replaceModel},
                 {"Remove replacement", "Delete the selected mod's file for this model", &Launcher::removeReplacement}}) {
            auto* a = new QAction(text, models_);
            a->setToolTip(tip);
            connect(a, &QAction::triggered, this, fn);
            models_->addAction(a);
            auto* b = new QPushButton(text);
            b->setToolTip(tip);
            connect(b, &QPushButton::clicked, a, &QAction::trigger);
            edit->addWidget(b);
        }
        side->addWidget(models_, 1);
        side->addLayout(edit);
        side->addWidget(viewGame_);
        side->addLayout(pick);
        // The GL view is created on first use: a QOpenGLWidget switches the whole window to GL rendering, and where
        // the platform has no GL (e.g. Wayland without a wayland-egl client integration) Qt then crashes at startup.
        viewHost_ = new QVBoxLayout;
        viewNote_ = new QLabel("Static rest / bind pose (animations are not played). Drag = orbit, wheel = zoom, "
                               "double-click = reset.");
        viewNote_->setWordWrap(true);
        auto* right = new QVBoxLayout;
        right->addLayout(viewHost_, 1);
        right->addWidget(viewNote_);
        l->addLayout(side, 2);
        l->addLayout(right, 3);
        connect(load, &QPushButton::clicked, this, [this] { loadOriginals(); });
        connect(viewLevel_, &QComboBox::currentTextChanged, this, [this] { refreshModels(); });
        connect(models_, &QListWidget::currentItemChanged, this, [this](QListWidgetItem* it) {
            if (!it || it->data(Qt::UserRole).isNull()) return;
            const QString file = it->data(Qt::UserRole).toString();
            if (!view_) {
                QOpenGLContext probe;
                if (!probe.create()) {
                    viewNote_->setText("3-D preview unavailable: no OpenGL on this display platform. Start the launcher "
                                       "with QT_QPA_PLATFORM=xcb, or report it with the Logs page contents.");
                    return;
                }
                view_ = new ModelView;
                view_->setMinimumSize(320, 240);
                viewHost_->addWidget(view_);
            }
            const QString err = view_->load(path(file));
            viewNote_->setText(err.isEmpty() ? QString("%1 triangles · %2").arg(view_->triangles()).arg(file) : "Cannot read " + file + ": " + err);
        });
        return w;
    }

    QWidget* modsTab() {
        auto* w = new QWidget;
        auto* l = new QVBoxLayout(w);
        mods_ = new QTableWidget(0, 4);
        mods_->setHorizontalHeaderLabels({"Mod (tick = enabled)", "Version", "Author", "Game"});
        mods_->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
        mods_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
        mods_->verticalHeader()->setVisible(false);
        mods_->setSelectionBehavior(QAbstractItemView::SelectRows);
        mods_->setSelectionMode(QAbstractItemView::SingleSelection);
        modInfo_ = new QTextBrowser;
        modInfo_->setOpenExternalLinks(true);
        auto* tabs = new QTabWidget;
        tabs->addTab(modInfo_, "Details");
        tabs->addTab(viewerPage(), "3-D model viewer");
        auto* split = new QSplitter(Qt::Vertical);
        split->addWidget(mods_);
        split->addWidget(tabs);
        auto* row = new QHBoxLayout;
        const std::pair<const char*, std::function<void()>> buttons[] = {
            {"▲", [this] { moveMod(-1); }},
            {"▼", [this] { moveMod(1); }},
            {"New mod…", [this] { newMod(); }},
            {"Open folder", [this] {
                 const std::string id = selectedMod();
                 const fs::path dir = id.empty() ? modsRoot() : modsRoot() / id;
                 fs::create_directories(dir);
                 QDesktopServices::openUrl(QUrl::fromLocalFile(q(dir)));
             }},
            {"Install from .zip…", [this] { installZip(); }},
            {"Remove", [this] { removeMod(); }},
            {"Export assets…", [this] { exportAssets(); }},
            {"Refresh", [this] { refreshMods(), refreshModels(); }}};
        for (const auto& [text, fn] : buttons) {
            auto* b = new QPushButton(text);
            connect(b, &QPushButton::clicked, this, fn);
            row->addWidget(b);
        }
        row->addStretch();
        l->addWidget(split, 1);
        l->addLayout(row);
        connect(mods_, &QTableWidget::itemChanged, this, [this](QTableWidgetItem* it) {
            if (it->column() == 0) collectMods(), showModInfo();
        });
        connect(mods_, &QTableWidget::itemSelectionChanged, this, [this] { showModInfo(), refreshModels(); });
        refreshMods();
        refreshModels();
        return w;
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
        auto* channel = new QLabel(q(bbtime::update::manifestUrl(cfg_.updateUrl)));
        channel->setObjectName("status");
        channel->setTextInteractionFlags(Qt::TextSelectableByMouse);
        f->addRow("Release channel", channel);
        keyLabels(f);
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
            updateStatus_->setText("Checking…");
            background(this, [url = bbtime::update::manifestUrl(cfg_.updateUrl), o = updateOptions()] { return bbtime::update::check(url, o); },
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
                if (!r) {
                    updateStatus_->setText(q("Install refused/failed (nothing changed): " + r.error()));
                    installUpdate_->setEnabled(true);
                    return;
                }
                rollback_->setEnabled(bbtime::update::previousVersion(appRoot()).has_value());
                gRestart = bbtime::update::handoffTarget(appRoot(), BBTIME_VERSION);
                if (gRestart.empty()) { // release without a launcher artifact: the next Play uses the new game
                    updateStatus_->setText("Installed to " + q(*r) + ".");
                    return;
                }
                updateStatus_->setText("Installed to " + q(*r) + ". Restarting into the new launcher…");
                QTimer::singleShot(800, this, &QWidget::close); // main() execs gRestart after the event loop
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
        cfg_.shadows = shadows_->currentData().toString().toStdString();
        cfg_.reflections = reflections_->isChecked();
        cfg_.water = water_->currentData().toString().toStdString();
        cfg_.upscaling = upscaling_->currentData().toString().toStdString();
        cfg_.frameGeneration = frameGen_->isChecked();
        cfg_.musicVolume = music_->value();
        cfg_.sfxVolume = sfx_->value();
        collectMods();
        for (int r = 0; r < binds_->rowCount(); ++r) {
            const std::string a = binds_->verticalHeaderItem(r)->text().toStdString();
            const std::string k = binds_->item(r, 0)->text().trimmed().toStdString();
            if (k.empty()) cfg_.bindings.erase(a);
            else cfg_.bindings[a] = k;
        }
    }

    bool saveSettings() {
        collect();
        const std::string e = bbtime::saveSettings(cfg_);
        statusBar()->showMessage(e.empty() ? "Saved " + q(bbtime::settingsPath()) : q(e), 8000);
        return e.empty();
    }

    static QString gameBinary() { return toolBinary("bbtime", "/../bbtime/bbtime"); }

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

// Replaces this process with the launcher `exe` (the release AppImage under app/current), same arguments.
// Returns only if exec fails; the env guard stops the new launcher from handing off again.
void execLauncher(const fs::path& exe, std::vector<char*> args) {
    setenv(bbtime::update::kHandoffEnv, "1", 1);
    if (const fs::path o = bbtime::update::originAppImage(appRoot()); !o.empty())
        setenv(bbtime::update::kOriginEnv, o.c_str(), 0);  // the user's AppImage, refreshed by the new launcher
    args[0] = const_cast<char*>(exe.c_str());
    execv(exe.c_str(), args.data());
    std::perror(("bbtime-launcher: cannot start " + exe.string()).c_str());
    unsetenv(bbtime::update::kHandoffEnv);
}

} // namespace

int main(int argc, char** argv) {
    const std::vector<char*> args(argv, argv + argc + 1); // before QApplication strips its own options
    if (std::getenv(bbtime::update::kHandoffEnv)) {
        // Handed off from the user's older AppImage (menu entry of Gear Lever / Shelly / AppImageLauncher ...):
        // replace that file by this launcher's AppImage so the entry starts the new version directly next time.
        const fs::path origin = bbtime::update::originAppImage(appRoot());
        const char* self = std::getenv("APPIMAGE");
        if (!origin.empty() && self && fs::path(self).string().starts_with(appRoot().string())) {
            const auto r = bbtime::update::refreshAppImage(origin, self);
            if (!r) std::fprintf(stderr, "bbtime-launcher: %s\n", r.error().c_str());
            else if (*r) std::fprintf(stderr, "bbtime-launcher: updated %s to %s\n", origin.c_str(), BBTIME_VERSION);
        }
        unsetenv(bbtime::update::kHandoffEnv);
        unsetenv(bbtime::update::kOriginEnv);
    }
    else if (const fs::path next = bbtime::update::handoffTarget(appRoot(), BBTIME_VERSION); !next.empty())
        execLauncher(next, args); // an update installed a newer launcher: run that one
    QApplication app(argc, argv);
    applyPalette();
    QApplication::setApplicationName("bbtime-launcher");
    QApplication::setApplicationVersion(BBTIME_VERSION);
    QApplication::setDesktopFileName("bbtime-launcher");
    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1"); // keep SIGINT/SIGTERM fatal; Qt owns the event loop
    SDL_Init(SDL_INIT_GAMEPAD); // failure only disables the gamepad list
    int rc;
    {
        Launcher w;
        w.show();
        rc = app.exec();
    }
    SDL_Quit();
    if (!gRestart.empty()) execLauncher(gRestart, args);
    return rc;
}
