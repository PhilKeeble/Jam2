#pragma once

#include "ConcurrentLooperMerge.hpp"
#include "LooperProject.hpp"

#include <QJsonObject>
#include <QString>

struct WavMetadata {
    int audioFormat = 0;
    int sampleRate = 0;
    int channels = 0;
    int bitsPerSample = 0;
    qint64 dataBytes = 0;
    qint64 frames = 0;
    int durationMs = 0;
    QString sha256;
};

struct StagedPcm16Asset {
    QString sourcePath;
    QString stagedPath;
    QString displayName;
    QString sha256;
    WavMetadata metadata;
    int sourceSampleRate = 0;
    qint64 sourceFrames = 0;
    bool resampled = false;
    // True only when this call committed new bytes at stagedPath. Reusing an
    // already-valid content-addressed file or its owned recording source is
    // false, allowing abandoned async work to clean up without deleting reuse.
    bool stagedFileCreated = false;
    QString error;
};

WavMetadata readWavMetadata(const QString& path);
QJsonObject readTrackSidecarJson(const QString& wavPath);
StagedPcm16Asset stagePcm16Asset(
    const QString& sourcePath,
    const QString& stagingFolder,
    int expectedSampleRate = 0,
    const QString& assetFolder = QStringLiteral("imported"));
int mergeSynchronizedLooperLanes(
    QJsonObject& song,
    const LooperProject& localProject);
int mergeLocalOnlyLooperLanes(
    QJsonObject& song,
    const LooperProject& localProject);
