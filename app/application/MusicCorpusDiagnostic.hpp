#pragma once

#include <QJsonObject>
#include <QString>

struct Jam2MusicCorpusOptions {
    bool includeAudio = true;
    bool matchedComplexitySeeds = false;
    int samplesPerCell = 4;
    QString styleId;
    QString profileId;
};

QJsonObject jam2WriteFullFormMusicCorpus(
    const QString& artifactRoot,
    const QString& seedNamespace,
    const Jam2MusicCorpusOptions& options = {});
