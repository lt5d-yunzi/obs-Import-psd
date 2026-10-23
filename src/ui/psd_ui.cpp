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
#include <QColorDialog>
#include <QStandardPaths>
#include <QStringList>
#include <QTextEdit>
#include <QCheckBox>
#include <QSpinBox>
#include <QPushButton>
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

    // Text layers: colour and stroke. PSD keeps these inside nested binary
    // formats that are not fully decoded yet, so expose them here.
    int text_layers = 0;
    for (const auto &l : doc.layers)
        if (!l.text.empty())
            ++text_layers;

    QColor text_color(204, 153, 204);
    QColor stroke_color(255, 255, 255);
    double stroke_width = 16.0;
    bool stroke_on = true;

    auto *text_group = new QGroupBox(QObject::tr("Text layers (%1 found)").arg(text_layers), &dlg);
    auto *text_form = new QFormLayout(text_group);

    auto *color_button = new QPushButton(text_group);
    auto set_button_color = [color_button](const QColor &c) {
        color_button->setText(c.name(QColor::HexRgb).toUpper());
        color_button->setStyleSheet(QString("background-color: %1; color: %2;")
                                        .arg(c.name())
                                        .arg(c.lightness() > 128 ? "black" : "white"));
    };
    set_button_color(text_color);
    QObject::connect(color_button, &QPushButton::clicked, &dlg, [&, color_button]() {
        const QColor picked = QColorDialog::getColor(text_color, &dlg, QObject::tr("Text colour"));
        if (picked.isValid()) {
            text_color = picked;
            set_button_color(text_color);
        }
    });
    text_form->addRow(QObject::tr("Fill colour"), color_button);

    auto *stroke_check = new QCheckBox(QObject::tr("White stroke"), text_group);
    stroke_check->setChecked(stroke_on);
    text_form->addRow(QString(), stroke_check);

    auto *stroke_spin = new QSpinBox(text_group);
    stroke_spin->setRange(0, 64);
    stroke_spin->setValue(int(stroke_width));
    stroke_spin->setSuffix(" px");
    text_form->addRow(QObject::tr("Stroke width"), stroke_spin);

    auto *stroke_color_button = new QPushButton(text_group);
    auto set_stroke_button_color = [stroke_color_button](const QColor &c) {
        stroke_color_button->setText(c.name(QColor::HexRgb).toUpper());
        stroke_color_button->setStyleSheet(QString("background-color: %1; color: %2;")
                                               .arg(c.name())
                                               .arg(c.lightness() > 128 ? "black" : "white"));
    };
    set_stroke_button_color(stroke_color);
    QObject::connect(stroke_color_button, &QPushButton::clicked, &dlg, [&, stroke_color_button]() {
        const QColor picked = QColorDialog::getColor(stroke_color, &dlg, QObject::tr("Stroke colour"));
        if (picked.isValid()) {
            stroke_color = picked;
            set_stroke_button_color(stroke_color);
        }
    });
    text_form->addRow(QObject::tr("Stroke colour"), stroke_color_button);

    if (text_layers > 0)
        layout->addWidget(text_group);

    layout->addLayout(form);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    layout->addWidget(buttons);

    if (dlg.exec() != QDialog::Accepted)
        return;

    SceneBuildOptions opts;
    opts.placement = SceneBuildOptions::Place_FitCanvas;
    if (text_layers > 0) {
        opts.text_style_valid = true;
        opts.text_color[0] = text_color.redF();
        opts.text_color[1] = text_color.greenF();
        opts.text_color[2] = text_color.blueF();
        opts.text_color[3] = 1.0;
        opts.text_stroke = stroke_check->isChecked();
        opts.text_stroke_width = double(stroke_spin->value());
        opts.text_stroke_color[0] = stroke_color.redF();
        opts.text_stroke_color[1] = stroke_color.greenF();
        opts.text_stroke_color[2] = stroke_color.blueF();
        opts.text_stroke_color[3] = 1.0;
    }
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

    // Missing PSD fonts: the text was rendered with a substitute; tell the user
    // which font to install to get the original look back.
    const std::vector<MissingFont> fonts = missing_fonts();
    if (!fonts.empty()) {
        QStringList lines;
        std::string seen;
        for (const MissingFont &f : fonts) {
            if (seen.find("|" + f.wanted + "|") != std::string::npos)
                continue;
            seen += "|" + f.wanted + "|";
            lines << QObject::tr("  %1  (used %2 instead)")
                         .arg(QString::fromStdString(f.wanted))
                         .arg(QString::fromStdString(f.used));
        }
        QMessageBox box(QMessageBox::Warning, QObject::tr("Missing fonts", "obs-psd"),
                        QObject::tr("This PSD uses fonts that are not installed, so those text "
                                    "layers were rendered with a similar system font:\n\n%1\n\n"
                                    "Install the font(s) and re-import to match the original "
                                    "exactly. Search the foundry or a font site for the PostScript "
                                    "name shown above.")
                            .arg(lines.join("\n")),
                        QMessageBox::Ok, main_parent());
        box.exec();
    }
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
