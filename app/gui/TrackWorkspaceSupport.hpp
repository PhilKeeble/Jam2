#pragma once

#include "LooperProject.hpp"

#include <QJsonObject>
#include <QString>

struct WavMetadata {
    int audioFormat = 0;
    int sampleRate = 0;
    int channels = 0;
    int bitsPerSample = 0;
    qint64 dataBytes = 0;
    int durationMs = 0;
    QString sha256;
};

struct StagedPcm16Asset {
    QString sourcePath;
    QString stagedPath;
    QString displayName;
    QString sha256;
    WavMetadata metadata;
    QString error;
};

WavMetadata readWavMetadata(const QString& path);
StagedPcm16Asset stagePcm16Asset(
    const QString& sourcePath,
    const QString& stagingFolder,
    int expectedSampleRate = 0);
int mergeQuarantinedLocalLanes(
    QJsonObject& song,
    const LooperProject& localProject,
    int expectedSampleRate);
