// SPDX-License-Identifier: GPL-2.0-or-later
#include "psd_ui.h"

#include "obs/scene_export.h"
#include "obs/scene_build.h"
#include "psd/psd.h"

#include <QApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QStandardPaths>
#include <QStringList>
#include <QTextEdit>
#include <QVBoxLayout>
#include <cstdlib>

extern "C" {
#include <obs-frontend-api.h>
#include <obs.h>
#include <util/platform.h>
}

namespace obspsd {

namespace {

void show_error(const QString &title, const QString &detail)
{
    QMessageBox box(QMessageBox::Critical, title, detail, QMessageBox::Ok);
    box.exec();
}

// Runs a modal dialog with the OBS main window as parent so it always appears
// in front of the studio UI.
QWidget *main_parent()
{
    return static_cast<QWidget *>(obs_frontend_get_main_window());
}

} // namespace

void run_import_dialog()
{
    const QString path = QFileDialog::getOpenFileName(
        main_parent(), QObject::tr("Import PSD as Scene", "obs-psd"),
        QStandardPaths::writableLocation(QStandardPaths::PicturesLocation),
        QObject::tr("Photoshop Document (*.psd *.PSD)", "obs-psd"));
    if (path.isEmpty())
        return;

    psd::Document doc;
    std::string err;
    if (!psd::read_file(path.toUtf8().constData(), doc, err)) {
        show_error(QObject::tr("PSD import failed", "obs-psd"), QString::fromStdString(err));
        return;
    }

    QDialog dlg(main_parent());
    dlg.setWindowTitle(QObject::tr("Import PSD as Scene", "obs-psd"));
    auto *layout = new QVBoxLayout(&dlg);

    auto *info = new QLabel(QObject::tr("%1 x %2 px, %3 layer(s)")
                                .arg(doc.width)
                                .arg(doc.height)
                                .arg(int(doc.layers.size())),
                            &dlg);
    layout->addWidget(info);

    auto *form = new QFormLayout();
    auto *name_edit = new QLineEdit(QFileInfo(path).completeBaseName(), &dlg);
    form->addRow(QObject::tr("Scene name"), name_edit);

    layout->addLayout(form);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    layout->addWidget(buttons);

    if (dlg.exec() != QDialog::Accepted)
        return;

    SceneBuildOptions opts;
    opts.placement = SceneBuildOptions::Place_FitCanvas;
    std::string build_err;
    obs_source_t *scene_src = nullptr;
    if (!build_scene_from_psd(doc, name_edit->text().toStdString().c_str(), build_err, &scene_src, opts)) {
        show_error(QObject::tr("PSD import failed", "obs-psd"), QString::fromStdString(build_err));
        return;
    }
    if (scene_src) {
        obs_frontend_set_current_scene(scene_src);
        obs_source_release(scene_src);
    }

    QString msg = QObject::tr("Scene \"%1\" created with %2 layer(s).")
                      .arg(name_edit->text())
                      .arg(int(doc.layers.size()));
    QMessageBox::information(main_parent(), QObject::tr("PSD imported", "obs-psd"), msg);
}

void run_export_dialog()
{
    obs_source_t *current = obs_frontend_get_current_scene();
    if (!current) {
        show_error(QObject::tr("No scene", "obs-psd"),
                   QObject::tr("Select a scene first, then export it to PSD."));
        return;
    }
    obs_scene_t *scene = obs_scene_from_source(current);
    if (!scene) {
        obs_source_release(current);
        show_error(QObject::tr("No scene", "obs-psd"), QObject::tr("The selected source is not a scene."));
        return;
    }
    const char *scene_name_c = obs_source_get_name(current);
    const QString scene_name = scene_name_c ? QString::fromUtf8(scene_name_c) : QString("scene");

    QDialog dlg(main_parent());
    dlg.setWindowTitle(QObject::tr("Export Scene to PSD", "obs-psd"));
    auto *layout = new QVBoxLayout(&dlg);

    auto *form = new QFormLayout();
    auto *path_edit = new QLineEdit(&dlg);
    QString default_dir = QString::fromUtf8(getenv("HOME") ? getenv("HOME") : "~");
    path_edit->setText(default_dir + "/" + scene_name + ".psd");
    auto *browse = new QPushButton(QObject::tr("Browse..."), &dlg);
    QObject::connect(browse, &QPushButton::clicked, &dlg, [&]() {
        const QString p = QFileDialog::getSaveFileName(main_parent(), QObject::tr("Save PSD"),
                                                       path_edit->text(),
                                                       QObject::tr("Photoshop Document (*.psd)"));
        if (!p.isEmpty())
            path_edit->setText(p);
    });
    auto *path_row = new QHBoxLayout();
    path_row->addWidget(path_edit);
    path_row->addWidget(browse);
    form->addRow(QObject::tr("Output file"), path_row);

    auto *extra_edit = new QLineEdit(&dlg);
    extra_edit->setPlaceholderText(QObject::tr("extra source ids to skip, comma separated"));
    form->addRow(QObject::tr("Extra ignored ids"), extra_edit);
    layout->addLayout(form);

    auto *note = new QLabel(
        QObject::tr("Live capture layers (browser, video, screen/window/device capture) are skipped.\n"
                    "Add \"[keep]\" to a source name to export it anyway, \"[skip]\" to drop it.\n"
                    "Image and text layers are always exported."),
        &dlg);
    note->setWordWrap(true);
    layout->addWidget(note);

    auto *report = new QTextEdit(&dlg);
    report->setReadOnly(true);
    report->setMinimumHeight(160);
    layout->addWidget(report);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    buttons->button(QDialogButtonBox::Ok)->setText(QObject::tr("Export"));
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    QObject::connect(buttons, &QDialogButtonBox::clicked, &dlg, [&](QAbstractButton *b) {
        if (b == buttons->button(QDialogButtonBox::Ok)) {
            ExportOptions opts;
            opts.extra_ignore_ids = extra_edit->text().toStdString();
            ExportResult res;
            std::string path = path_edit->text().toStdString();
            if (path.size() < 4 || path.compare(path.size() - 4, 4, ".psd") != 0 ||
                path.compare(path.size() - 4, 4, ".PSD") != 0)
                path += ".psd";

            const bool ok = export_scene_to_psd(scene, path, opts, res);

            QStringList lines;
            for (const auto &it : res.items) {
                QString line = QString::fromStdString(it.name) + "  [" + QString::fromStdString(it.type) + "]";
                if (it.skipped)
                    line += "  -- skipped: " + QString::fromStdString(it.reason);
                lines << line;
            }
            lines << "";
            if (ok)
                lines << QObject::tr("Exported %1 layer(s), skipped %2.")
                             .arg(int(res.exported))
                             .arg(int(res.skipped));
            else
                lines << QObject::tr("Export failed: %1").arg(QString::fromStdString(res.error));
            report->setPlainText(lines.join("\n"));

            if (!ok)
                show_error(QObject::tr("Export failed", "obs-psd"), QString::fromStdString(res.error));
        }
    });
    layout->addWidget(buttons);

    dlg.exec();
    obs_source_release(current);
}

} // namespace obspsd
