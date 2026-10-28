#include "TrackLaneRemoval.hpp"
#include "LooperProject.hpp"
#include "ProjectPersistenceCoordinator.hpp"
#include <QApplication>
#include <QAbstractButton>
#include <QDir>
#include <QFile>
#include <QMessageBox>
#include <QPushButton>
#include <QTemporaryDir>
#include <QTimer>
#include <iostream>

int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    int failures = 0;
    const auto expect = [&](bool ok, const char* message) {
        if (!ok) { ++failures; std::cerr << "FAIL: " << message << '\n'; }
    };
    QTemporaryDir root;
    ProjectPersistenceCoordinator persistence;
    persistence.initializeWorkspace(root.path());
    persistence.setProjectFolder(root.path());
    LooperProject project;
    const auto addLane = [&](const QString& id, const QString& path) {
        LooperLane lane;
        lane.id = id; lane.name = id; lane.assetPath = path;
        lane.assetHash = path.isEmpty() ? QString{} : QString(64, QLatin1Char('a'));
        lane.sampleRate = path.isEmpty() ? 0 : 48000;
        lane.sourceFrames = path.isEmpty() ? 0 : 100;
        expect(project.appendLane(0, lane), "fixture lane appends");
    };
    const QString wav = QDir(root.path()).filePath(QStringLiteral("recorded.wav"));
    { QFile file(wav); expect(file.open(QIODevice::WriteOnly) && file.write("RIFF fixture") > 0, "WAV fixture exists"); }
    persistence.registerTransientWav(wav);
    addLane(QStringLiteral("recorded"), QStringLiteral("recorded.wav"));
    addLane(QStringLiteral("other"), {});
    auto request = jam2::gui::describeTrackLaneRemoval(project, persistence, 0, 0);
    expect(request && request->wavExists && request->wavPath == wav, "removal resolves relative paths and stable identities");
    LooperProject shared = project;
    LooperLane duplicate = shared.banks().at(0).lanes.at(0);
    duplicate.id = QStringLiteral("shared-wav");
    expect(shared.appendLane(0, duplicate), "shared-file fixture appends");
    const auto sharedRequest = jam2::gui::describeTrackLaneRemoval(shared, persistence, 0, 0);
    expect(sharedRequest && sharedRequest->fileReferences == 2, "confirmation counts other lanes that reference the same file");
    bool inspected = false;
    const auto decide = [&](bool accept) {
        QTimer::singleShot(0, &app, [&, accept] {
            auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
            expect(box != nullptr, "real confirmation is modal");
            if (!box) return;
            expect(box->informativeText().contains(QStringLiteral("permanently delete")) &&
                box->informativeText().contains(wav), "confirmation explains disk deletion and exact file");
            expect(box->buttons().size() == 2, "confirmation has one removal operation plus Cancel");
            expect(box->defaultButton() == box->button(QMessageBox::Cancel), "Cancel is the safe default");
            for (auto* button : box->buttons()) {
                if ((accept && box->buttonRole(button) == QMessageBox::DestructiveRole) ||
                    (!accept && button == box->button(QMessageBox::Cancel))) {
                    if (accept) expect(project.moveLane(0, 0, 1), "remote-style reordering is processed inside the modal loop");
                    inspected = true; button->click(); return;
                }
            }
            box->reject();
        });
    };
    decide(false);
    expect(!jam2::gui::confirmTrackLaneRemoval(*request, nullptr) && inspected && QFile::exists(wav) &&
        project.banks().at(0).lanes.size() == 2, "Cancel leaves the WAV and lane untouched");
    decide(true);
    expect(jam2::gui::confirmTrackLaneRemoval(*request, nullptr), "real destructive action confirms removal");
    expect(project.banks().at(0).lanes.at(1).id == QStringLiteral("recorded"), "modal update relocated the recorded lane");
    const auto removed = jam2::gui::removeTrackLaneAndWav(project, persistence, *request);
    expect(removed.removed && removed.laneIndex == 1 && !QFile::exists(wav) &&
        !persistence.ownsTransientWav(wav) && project.banks().at(0).lanes.size() == 1 &&
        project.banks().at(0).lanes.at(0).id == QStringLiteral("other"), "confirmed removal deletes the exact lane and its transient WAV after reordering");
    const auto empty = jam2::gui::describeTrackLaneRemoval(project, persistence, 0, 0);
    expect(empty && !empty->wavExists && jam2::gui::removeTrackLaneAndWav(project, persistence, *empty).removed,
        "empty lanes remove without attempting file deletion");
    const QString bad = QDir(root.path()).filePath(QStringLiteral("directory.wav"));
    expect(QDir().mkpath(bad), "non-file fixture creates");
    addLane(QStringLiteral("invalid"), bad);
    const auto invalid = jam2::gui::describeTrackLaneRemoval(project, persistence, 0, 0);
    const auto failed = jam2::gui::removeTrackLaneAndWav(project, persistence, *invalid);
    expect(!failed.removed && !failed.error.isEmpty() && project.banks().at(0).lanes.size() == 1 && QDir(bad).exists(),
        "invalid file operation reports an error and preserves the lane");
    auto stale = *invalid;
    stale.assetHash = QString(64, QLatin1Char('b'));
    expect(!jam2::gui::removeTrackLaneAndWav(project, persistence, stale).removed,
        "a changed attachment cannot be deleted through an old confirmation");
    const QString externalPath = QDir(root.path()).filePath(QStringLiteral("external.wav"));
    { QFile file(externalPath); expect(file.open(QIODevice::WriteOnly) && file.write("wav") == 3, "untracked WAV fixture creates"); }
    addLane(QStringLiteral("external"), externalPath);
    const auto external = jam2::gui::describeTrackLaneRemoval(project, persistence, 0, 1);
    expect(external && !persistence.ownsTransientWav(externalPath) &&
        jam2::gui::removeTrackLaneAndWav(project, persistence, *external).removed && !QFile::exists(externalPath),
        "confirmed removal deletes an absolute WAV even when it is not a transient file");
    addLane(QStringLiteral("missing"), QDir(root.path()).filePath(QStringLiteral("missing.wav")));
    const auto missing = jam2::gui::describeTrackLaneRemoval(project, persistence, 0, 1);
    expect(missing && !missing->wavExists && jam2::gui::removeTrackLaneAndWav(project, persistence, *missing).removed,
        "a lane with an already missing WAV can still be removed");
#ifdef Q_OS_UNIX
    const QString lockedDirectory = QDir(root.path()).filePath(QStringLiteral("locked"));
    expect(QDir().mkpath(lockedDirectory), "deletion-failure directory creates");
    const QString lockedPath = QDir(lockedDirectory).filePath(QStringLiteral("locked.wav"));
    { QFile file(lockedPath); expect(file.open(QIODevice::WriteOnly) && file.write("wav") == 3, "locked WAV fixture creates"); }
    LooperProject locked;
    LooperLane lockedLane;
    lockedLane.id = QStringLiteral("locked"); lockedLane.name = lockedLane.id;
    lockedLane.assetPath = lockedPath; lockedLane.assetHash = QString(64, QLatin1Char('c'));
    lockedLane.sampleRate = 48000; lockedLane.sourceFrames = 100;
    expect(locked.appendLane(0, lockedLane), "locked lane appends");
    auto lockedRequest = jam2::gui::describeTrackLaneRemoval(locked, persistence, 0, 0);
    expect(QFile::setPermissions(lockedDirectory, QFile::ReadOwner | QFile::ExeOwner), "directory deletion permission is revoked");
    const auto denied = jam2::gui::removeTrackLaneAndWav(locked, persistence, *lockedRequest);
    expect(QFile::setPermissions(lockedDirectory, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner), "directory permissions restore");
    expect(!denied.removed && denied.error.contains(QStringLiteral("Could not delete")) && QFile::exists(lockedPath) &&
        locked.banks().at(0).lanes.size() == 1, "failed disk deletion keeps both the lane and WAV and reports the error");
#endif
    expect(!jam2::gui::describeTrackLaneRemoval(project, persistence, -1, 0), "invalid lane selection is rejected");
    return failures == 0 ? 0 : 1;
}
